// Compressed user bitmaps enter the ordinary codec; Qt only owns/presents its pixels.
#pragma once

#include "MaiImageDecode.h"

#include <QByteArray>
#include <QImage>
#include <QString>

struct DesktopRasterImageResult {
    QImage image;
    MaiImageInfo info{};
    int errorCode = 0;
};

// Synchronous; the QImage owns its pixels after return. A valid ICC is attached
// for presentation; no extra primaries mapping or color transform is performed.
// Options/cancellation have
// the ordinary codec contract; no QObject, callback or byte storage is retained.
DesktopRasterImageResult desktopReadRasterFile(const QString& path, const MaiImageReadOptions& options = {});
DesktopRasterImageResult desktopReadRasterBytes(const QByteArray& bytes, const MaiImageReadOptions& options = {});
MaiImageInfo desktopInspectRasterFile(const QString& path, const MaiImageReadOptions& options = {});
MaiImageInfo desktopInspectRasterBytes(const QByteArray& bytes, const MaiImageReadOptions& options = {});
