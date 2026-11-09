// Qt receives owning RGBA and color tags; compressed input is handled only by Media.
#include "ui/DesktopRasterImage.h"

#include <cerrno>

#include <QColorSpace>

namespace {

DesktopRasterImageResult present(MaiImageDecodeResult decoded, const MaiImageInfo& info) {
    DesktopRasterImageResult result;
    result.info = info;
    result.errorCode = decoded.error_code;
    if (decoded.rgba && decoded.error_code == 0) {
        // Copy before releasing the native allocation. The returned image can be
        // cached or delivered to another thread without retaining codec state.
        result.image =
            QImage(decoded.rgba, decoded.width, decoded.height, decoded.stride, QImage::Format_RGBA8888).copy();
        if (result.image.isNull()) {
            result.errorCode = -ENOMEM;
            result.info = {};
            result.info.error_code = result.errorCode;
        } else if (decoded.icc_profile && decoded.icc_size > 0) {
            const QColorSpace color = QColorSpace::fromIccProfile(
                QByteArray(reinterpret_cast<const char*>(decoded.icc_profile), decoded.icc_size));
            if (color.isValid()) result.image.setColorSpace(color);
        }
    }
    maiImageDecodeFree(decoded.rgba);
    return result;
}

}  // namespace

DesktopRasterImageResult desktopReadRasterFile(const QString& path, const MaiImageReadOptions& options) {
    MaiImageInfo info{};
    if (path.contains(QChar::Null)) {
        DesktopRasterImageResult result;
        result.info.error_code = result.errorCode = -EINVAL;
        return result;
    }
    const QByteArray utf8 = path.toUtf8();
    const MaiImageDecodeResult decoded = maiImageReadFile(utf8.constData(), &options, &info);
    return present(decoded, info);
}

DesktopRasterImageResult desktopReadRasterBytes(const QByteArray& bytes, const MaiImageReadOptions& options) {
    MaiImageInfo info{};
    const MaiImageDecodeResult decoded = maiImageReadMemory(reinterpret_cast<const unsigned char*>(bytes.constData()),
                                                            static_cast<size_t>(bytes.size()), &options, &info);
    return present(decoded, info);
}

MaiImageInfo desktopInspectRasterFile(const QString& path, const MaiImageReadOptions& options) {
    if (path.contains(QChar::Null)) {
        MaiImageInfo info{};
        info.error_code = -EINVAL;
        return info;
    }
    const QByteArray utf8 = path.toUtf8();
    return maiImageInspectFile(utf8.constData(), &options);
}

MaiImageInfo desktopInspectRasterBytes(const QByteArray& bytes, const MaiImageReadOptions& options) {
    return maiImageInspectMemory(reinterpret_cast<const unsigned char*>(bytes.constData()),
                                 static_cast<size_t>(bytes.size()), &options);
}
