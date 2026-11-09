// One MIME/size/SVG policy for document resources; Qt presents owning pixels.
#include "ui/DesktopDocumentImage.h"

#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QXmlStreamReader>

#include <utility>

#include "ui/DesktopRasterImage.h"

namespace {

// The document accepts bounded compressed image input; neither invalid bytes
// nor rejected URLs may reach QTextDocument's provider/name decoder fallback.
constexpr qint64 kDocumentImagePixels = 64'000'000;

bool documentDataPayload(const QUrl& url, QByteArray& bytes, bool& svg) {
    if (url.scheme() != QStringLiteral("data")) return false;
    const QByteArray encoded = url.toEncoded(QUrl::FullyEncoded);
    const qsizetype comma = encoded.indexOf(',');
    const qsizetype base64Limit = ((kDesktopDocumentImageMaxEncodedBytes + 2) / 3) * 4;
    if (comma < 6 || comma > 512 || encoded.size() - comma - 1 > base64Limit * 3) return false;
    QByteArray metadata = encoded.mid(5, comma - 5).toLower();
    const bool base64 = metadata.endsWith(";base64");
    if (base64) metadata.chop(7);
    const QByteArray mime = metadata.split(';').first();
    if (!mime.startsWith("image/")) return false;
    svg = mime == "image/svg+xml";
    const qsizetype limit = base64 ? base64Limit : kDesktopDocumentImageMaxEncodedBytes;
    const auto hex = [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
    };
    // Count percent-decoded bytes before allocating the percent/base64 result.
    qsizetype decodedLength = 0;
    for (qsizetype index = comma + 1; index < encoded.size(); ++index) {
        if (encoded[index] == '%') {
            if (index + 2 >= encoded.size() || !hex(encoded[index + 1]) || !hex(encoded[index + 2])) return false;
            index += 2;
        }
        if (++decodedLength > limit) return false;
    }
    QByteArray payload = QByteArray::fromPercentEncoding(encoded.mid(comma + 1));
    if (base64) {
        if (payload.size() % 4 != 0) return false;
        const qsizetype padding = payload.endsWith("==") ? 2 : payload.endsWith('=') ? 1 : 0;
        if (payload.size() / 4 * 3 - padding > kDesktopDocumentImageMaxEncodedBytes) return false;
        auto decoded = QByteArray::fromBase64Encoding(payload, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) return false;
        bytes = std::move(decoded.decoded);
    } else {
        bytes = std::move(payload);
    }
    return !bytes.isEmpty() && bytes.size() <= kDesktopDocumentImageMaxEncodedBytes;
}

}  // namespace

QImage desktopDocumentVectorImage(const QByteArray& bytes) {
    if (bytes.isEmpty() || bytes.size() > kDesktopDocumentImageMaxEncodedBytes) return {};
    // Preserve ordinary self-contained vector SVG. QtSvg has no resource
    // interception hook: embedded images, external references, CSS blocks and
    // active content are rejected before handing the bytes to QSvgRenderer.
    // This deliberately does not claim arbitrary SVG document compatibility.
    QXmlStreamReader xml(bytes);
    int depth = 0;
    int elements = 0;
    bool root = false;
    const QRegularExpression urls(QStringLiteral(R"(url\s*\(([^)]*)\))"), QRegularExpression::CaseInsensitiveOption);
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference ||
            token == QXmlStreamReader::ProcessingInstruction)
            return {};
        if (token == QXmlStreamReader::EndElement) --depth;
        if (token != QXmlStreamReader::StartElement) continue;
        if (++depth > 64 || ++elements > 100000) return {};
        const auto name = xml.name().toString().toLower();
        if (!root) {
            if (name != QStringLiteral("svg")) return {};
            root = true;
        }
        if (name == QStringLiteral("image") || name == QStringLiteral("feimage") ||
            name == QStringLiteral("foreignobject") || name == QStringLiteral("script") ||
            name == QStringLiteral("style"))
            return {};
        for (const auto& attribute : xml.attributes()) {
            const auto key = attribute.name().toString().toLower();
            const auto value = attribute.value().toString().trimmed();
            if (key == QStringLiteral("base") || key.startsWith(QStringLiteral("on")) ||
                value.contains(QLatin1Char('\\')))
                return {};
            if (key == QStringLiteral("href") && (!value.startsWith(QLatin1Char('#')) || value.size() < 2)) return {};
            auto matches = urls.globalMatch(value);
            while (matches.hasNext()) {
                auto reference = matches.next().captured(1).trimmed();
                if ((reference.startsWith(QLatin1Char('\'')) && reference.endsWith(QLatin1Char('\''))) ||
                    (reference.startsWith(QLatin1Char('"')) && reference.endsWith(QLatin1Char('"'))))
                    reference = reference.mid(1, reference.size() - 2).trimmed();
                if (!reference.startsWith(QLatin1Char('#')) || reference.size() < 2) return {};
            }
            if (value.contains(QStringLiteral("@import"), Qt::CaseInsensitive)) return {};
        }
    }
    if (xml.hasError() || !root) return {};
    QSvgRenderer renderer(bytes);
    renderer.setAnimationEnabled(false);
    const QSize size = renderer.defaultSize();
    if (!renderer.isValid() || size.width() <= 0 || size.height() <= 0 ||
        qint64(size.width()) * size.height() > kDocumentImagePixels)
        return {};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    image.fill(Qt::transparent);
    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}

QImage desktopDocumentDataImage(const QUrl& url, const MaiImageReadOptions& options) {
    QByteArray bytes;
    bool svg = false;
    if (!documentDataPayload(url, bytes, svg)) return {};
    // The declared SVG branch never becomes a fallback for a failed bitmap.
    return svg ? desktopDocumentVectorImage(bytes) : desktopReadRasterBytes(bytes, options).image;
}
