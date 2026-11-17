// 文件作用：在后台把 PDF 字节的第一页渲染到 QImage；只依赖 CoreGraphics，不创建窗口或改变前台应用。
#include <QByteArray>
#include <QImage>
#include <QString>
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <cmath>

QImage renderAgentMacPdfFirstPage(const QByteArray& bytes, QString* error) {
    CGDataProviderRef provider =
        CGDataProviderCreateWithData(nullptr, bytes.constData(), size_t(bytes.size()), nullptr);
    if (!provider) {
        *error = QStringLiteral("PDF 数据无法读取。");
        return {};
    }
    CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
    CGDataProviderRelease(provider);
    if (!document || CGPDFDocumentIsEncrypted(document) || CGPDFDocumentGetNumberOfPages(document) == 0) {
        if (document) CGPDFDocumentRelease(document);
        *error = QStringLiteral("PDF 文档无法预览。");
        return {};
    }
    CGPDFPageRef page = CGPDFDocumentGetPage(document, 1);
    CGRect pageBox = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    const int rotation = CGPDFPageGetRotationAngle(page);
    if (rotation % 180) std::swap(pageBox.size.width, pageBox.size.height);
    if (!std::isfinite(pageBox.size.width) || !std::isfinite(pageBox.size.height) || pageBox.size.width <= 0 ||
        pageBox.size.height <= 0) {
        CGPDFDocumentRelease(document);
        *error = QStringLiteral("PDF 页面尺寸无效。");
        return {};
    }
    const double scale = std::min(1800.0 / pageBox.size.width, 3000.0 / pageBox.size.height);
    const int width = std::max(1, int(std::ceil(pageBox.size.width * scale)));
    const int height = std::max(1, int(std::ceil(pageBox.size.height * scale)));
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
    CGContextRef context =
        image.isNull()
            ? nullptr
            : CGBitmapContextCreate(image.bits(), size_t(width), size_t(height), 8, size_t(image.bytesPerLine()),
                                    colors, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
    CGColorSpaceRelease(colors);
    if (!context) {
        CGPDFDocumentRelease(document);
        *error = QStringLiteral("PDF 页面无法解码。");
        return {};
    }
    // QImage的top-down像素存储与此CoreGraphics bitmap上下文已经对应；
    // 额外翻转会把页首放到页尾。先显式放大像素，再仅求逻辑页的旋转/裁剪变换。
    // CGPDFPageGetDrawingTransform在小页放入大矩形时不会放大，不能靠它做raster倍率。
    CGContextScaleCTM(context, double(width) / pageBox.size.width, double(height) / pageBox.size.height);
    CGContextConcatCTM(context,
                       CGPDFPageGetDrawingTransform(page, kCGPDFCropBox,
                           CGRectMake(0, 0, pageBox.size.width, pageBox.size.height), 0, true));
    CGContextClipToRect(context, CGPDFPageGetBoxRect(page, kCGPDFCropBox));
    CGContextDrawPDFPage(context, page);
    CGContextRelease(context);
    CGPDFDocumentRelease(document);
    return image;
}
