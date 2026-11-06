// 文件作用：以 macOS Vision 定位原图人脸；只返回提示，处理范围由共用核心决定。
#include "MaiChatTools/DesktopPortraitFaces.h"

#include <algorithm>
#include <stdexcept>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include "MaiPortraitBridge.h"
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

QJsonArray desktopPortraitFaces(const QImage& rgba, const std::function<bool()>& isCanceled) {
    @autoreleasepool {
        QJsonArray candidates;
        const auto detect = [&](const QRect& native, int maximumSide) {
            if (isCanceled && isCanceled()) throw std::runtime_error("Face detection was canceled");
            const QSize sampleSize = std::max(native.width(), native.height()) <= maximumSide
                                         ? native.size()
                                         : native.size().scaled(maximumSide, maximumSide, Qt::KeepAspectRatio);
            QImage image(sampleSize, QImage::Format_RGBA8888);
            if (image.isNull()) throw std::runtime_error("Cannot allocate face detection pixels");
            {
                QPainter painter(&image);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(image.rect(), rgba, native);
            }
            CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGDataProviderRef provider =
                CGDataProviderCreateWithData(nullptr, image.constBits(), image.sizeInBytes(), nullptr);
            CGImageRef pixels = CGImageCreate(image.width(), image.height(), 8, 32, image.bytesPerLine(), color,
                                              kCGBitmapByteOrder32Big | kCGImageAlphaLast, provider, nullptr, false,
                                              kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            CGColorSpaceRelease(color);
            if (!pixels) throw std::runtime_error("Cannot create the Vision image");
            VNDetectFaceLandmarksRequest* request = [[VNDetectFaceLandmarksRequest alloc] init];
            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:pixels options:@{}];
            NSError* error = nil;
            const BOOL succeeded = [handler performRequests:@[ request ] error:&error];
            CGImageRelease(pixels);
            if (!succeeded) throw std::runtime_error("Vision face detection failed");
            for (VNFaceObservation* face in request.results) {
                const CGRect box = face.boundingBox;
                const double x = native.x() + box.origin.x * native.width();
                const double y = native.y() + (1 - CGRectGetMaxY(box)) * native.height();
                const double width = box.size.width * native.width(), height = box.size.height * native.height();
                const auto eye = [&](VNFaceLandmarkRegion2D* region) -> QJsonObject {
                    if (!region || !region.pointCount) return {};
                    double eyeX = 0, eyeY = 0;
                    for (NSUInteger index = 0; index < region.pointCount; ++index) {
                        eyeX += region.normalizedPoints[index].x;
                        eyeY += region.normalizedPoints[index].y;
                    }
                    return {{QStringLiteral("x"), x + eyeX / region.pointCount * width},
                            {QStringLiteral("y"), y + (1 - eyeY / region.pointCount) * height}};
                };
                candidates.append(
                    QJsonObject{{QStringLiteral("box"), QJsonObject{{QStringLiteral("x"), x},
                                                                    {QStringLiteral("y"), y},
                                                                    {QStringLiteral("width"), width},
                                                                    {QStringLiteral("height"), height}}},
                                {QStringLiteral("confidence"), face.confidence},
                                {QStringLiteral("landmarks"),
                                 QJsonObject{{QStringLiteral("left_eye"), eye(face.landmarks.leftEye)},
                                             {QStringLiteral("right_eye"), eye(face.landmarks.rightEye)}}}});
            }
        };
        detect(rgba.rect(), 2048);
        // 原生重叠窗口覆盖小脸；不把一个放大的全图缩略图当作额外原图证据。
        if (rgba.width() > 2048 || rgba.height() > 2048) {
            for (int top = 0; top < rgba.height(); top += 1050)
                for (int left = 0; left < rgba.width(); left += 1050)
                    detect(QRect(left, top, std::min(1400, rgba.width() - left), std::min(1400, rgba.height() - top)),
                           1400);
        }
        const QByteArray hints =
            QJsonDocument(QJsonObject{{QStringLiteral("_face_hints"), candidates}}).toJson(QJsonDocument::Compact);
        char* merged = maiPortraitMergeFaceHints(hints.constData());
        if (!merged) throw std::runtime_error("Cannot merge face detections");
        const QJsonObject response = QJsonDocument::fromJson(QByteArray(merged)).object();
        maiPortraitFree(merged);
        if (!response.value(QStringLiteral("ok")).toBool()) throw std::runtime_error("Face detections are invalid");
        return response.value(QStringLiteral("output")).toObject().value(QStringLiteral("faces")).toArray();
    }
}
