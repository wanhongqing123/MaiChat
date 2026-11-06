// 文件作用：Windows 复用现有 YuNet 视觉实现；只提供原生坐标提示。
#include "MaiChatTools/DesktopPortraitFaces.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <QPainter>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "MaiPortraitBridge.h"
#include "MaiChatTools/MaiImageVision.h"

QJsonArray desktopPortraitFaces(const QImage& rgba, const std::function<bool()>& isCanceled) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static std::unique_ptr<void, decltype(&maiImageVisionDestroy)> vision(nullptr, maiImageVisionDestroy);
    if (!vision) {
        QFile face(QStringLiteral(":/maichat/models/face_detection_yunet_2023mar.onnx"));
        QFile person(QStringLiteral(":/maichat/models/human_segmentation_pphumanseg_2023mar.onnx"));
        if (!face.open(QIODevice::ReadOnly) || !person.open(QIODevice::ReadOnly))
            throw std::runtime_error("Bundled face detection models are unavailable");
        const QByteArray faceBytes = face.readAll(), personBytes = person.readAll();
        char* error = nullptr;
        vision.reset(maiImageVisionCreate(
            reinterpret_cast<const unsigned char*>(faceBytes.constData()), faceBytes.size(),
            reinterpret_cast<const unsigned char*>(personBytes.constData()), personBytes.size(), &error));
        const std::string detail = error ? error : "Cannot initialize face detector";
        maiImageVisionFree(error);
        if (!vision) throw std::runtime_error(detail);
    }
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
        const auto result = maiImageVisionDetectFacesRgba(vision.get(), image.constBits(), image.width(),
                                                          image.height(), image.bytesPerLine());
        std::unique_ptr<void, decltype(&maiImageVisionFree)> faces(result.faces, maiImageVisionFree),
            error(result.error, maiImageVisionFree);
        if (result.error || (result.count && !result.faces)) throw std::runtime_error("Face detection failed");
        const double sx = double(native.width()) / image.width(), sy = double(native.height()) / image.height();
        for (int index = 0; index < result.count; ++index) {
            const auto& face = result.faces[index];
            const auto eye = [&](int point) {
                return QJsonObject{{QStringLiteral("x"), native.x() + face.landmarks[point * 2] * sx},
                                   {QStringLiteral("y"), native.y() + face.landmarks[point * 2 + 1] * sy}};
            };
            candidates.append(QJsonObject{
                {QStringLiteral("box"), QJsonObject{{QStringLiteral("x"), native.x() + face.x * sx},
                                                    {QStringLiteral("y"), native.y() + face.y * sy},
                                                    {QStringLiteral("width"), face.width * sx},
                                                    {QStringLiteral("height"), face.height * sy}}},
                {QStringLiteral("confidence"), face.confidence},
                {QStringLiteral("landmarks"),
                 QJsonObject{{QStringLiteral("left_eye"), eye(1)}, {QStringLiteral("right_eye"), eye(0)}}}});
        }
    };
    detect(rgba.rect(), 2048);
    if (rgba.width() > 2048 || rgba.height() > 2048)
        for (int top = 0; top < rgba.height(); top += 1050)
            for (int left = 0; left < rgba.width(); left += 1050)
                detect(QRect(left, top, std::min(1400, rgba.width() - left), std::min(1400, rgba.height() - top)),
                       1400);
    const QByteArray hints =
        QJsonDocument(QJsonObject{{QStringLiteral("_face_hints"), candidates}}).toJson(QJsonDocument::Compact);
    char* merged = maiPortraitMergeFaceHints(hints.constData());
    if (!merged) throw std::runtime_error("Cannot merge face detections");
    const auto response = QJsonDocument::fromJson(QByteArray(merged)).object();
    maiPortraitFree(merged);
    if (!response.value(QStringLiteral("ok")).toBool()) throw std::runtime_error("Face detections are invalid");
    return response.value(QStringLiteral("output")).toObject().value(QStringLiteral("faces")).toArray();
}
