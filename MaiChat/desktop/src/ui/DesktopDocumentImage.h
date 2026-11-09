// Shared compressed-image policy for HTML preview and self-contained PDF resources.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QUrl>

#include "MaiImageDecode.h"

// The local SVG reader uses the same pre-allocation encoded-byte limit as data
// resources. Raster source/output budgets remain the ordinary codec contract.
inline constexpr qsizetype kDesktopDocumentImageMaxEncodedBytes = 128 * 1024 * 1024;

// Synchronous presentation helpers. A successful QImage owns its pixels; null
// means rejection/decoding failure. No host QObject, callback or input survives
// the call. URL/file/qrc authorization remains with each document's loader.
// Data URLs accept compressed image/* or explicitly declared self-contained SVG;
// SVG uses the vector renderer and cannot be a fallback for a failed bitmap.
QImage desktopDocumentDataImage(const QUrl& url, const MaiImageReadOptions& options = {});
QImage desktopDocumentVectorImage(const QByteArray& bytes);
