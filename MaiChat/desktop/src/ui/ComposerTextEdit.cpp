// 文件作用：桌面端共用界面；ComposerTextEdit 负责富文本输入、选区与编辑操作，本文件给出其实现。
// 主要入口：setMimeHandler、setCornerAction、setLeadingAction、setLeadingHint。
// 直接依赖：ui/UiZoom.h；实现与这些接口协作。
#include "ui/ComposerTextEdit.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QResizeEvent>
#include <QTextDocument>
#include <QUrl>
#include <utility>

#include "ui/UiZoom.h"
#include "ui/DesktopRasterImage.h"

namespace {

QImage dataImage(const QUrl& url) {
    constexpr qsizetype maximumBytes = 128 * 1024 * 1024;
    const QByteArray encoded = url.toEncoded();
    const qsizetype comma = encoded.indexOf(',');
    if (comma < 0) return {};
    const QByteArray metadata = encoded.left(comma).toLower();
    const QByteArray payload = encoded.mid(comma + 1);
    if (metadata.endsWith(";base64")) {
        if (payload.size() > (maximumBytes + 2) / 3 * 4) return {};
        const auto decoded = QByteArray::fromBase64Encoding(
            payload, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) return {};
        return desktopReadRasterBytes(decoded.decoded).image;
    }
    // Percent decoding may shrink by at most three. Keep the encoded allocation
    // bounded as well; the native image API checks the resulting byte budget.
    if (payload.size() > maximumBytes) return {};
    return desktopReadRasterBytes(QByteArray::fromPercentEncoding(payload)).image;
}

class ComposerRasterDocument final : public QTextDocument {
public:
    explicit ComposerRasterDocument(QObject* parent) : QTextDocument(parent) {}

protected:
    QVariant loadResource(int type, const QUrl& name) override {
        if (type != QTextDocument::ImageResource)
            return QTextDocument::loadResource(type, name);
        QImage image;
        if (name.scheme() == QStringLiteral("data")) {
            image = dataImage(name);
        } else {
            QUrl resolved = name;
            if (resolved.isRelative() && !baseUrl().isEmpty()) resolved = baseUrl().resolved(name);
            QString path;
            if (resolved.isLocalFile() && resolved.host().isEmpty()) path = resolved.toLocalFile();
            else if (resolved.scheme().isEmpty()) {
                path = resolved.path();
                // Existing attachment names are native paths, including draft
                // names containing URL punctuation. Preserve their identity.
                if (resolved.hasQuery()) path += QLatin1Char('?') + resolved.query();
                if (resolved.hasFragment()) path += QLatin1Char('#') + resolved.fragment();
            }
#ifdef Q_OS_WIN
            else if (resolved.host().isEmpty() && resolved.scheme().size() == 1 &&
                     resolved.scheme().at(0) >= QLatin1Char('a') &&
                     resolved.scheme().at(0) <= QLatin1Char('z') &&
                     (resolved.path().startsWith(QLatin1Char('/')) ||
                      resolved.path().startsWith(QLatin1Char('\\')))) {
                // Native attachment names such as C:/... parse as scheme "c".
                // Restore the local drive explicitly; a null image remains a
                // terminal failure, without a Qt decoder or resource fallback.
                path = resolved.scheme().toUpper() + QLatin1Char(':') + resolved.path();
                if (resolved.hasQuery()) path += QLatin1Char('?') + resolved.query();
                if (resolved.hasFragment()) path += QLatin1Char('#') + resolved.fragment();
            }
#endif
            if (path.startsWith(QStringLiteral("//")) || path.startsWith(QStringLiteral("\\\\")))
                path.clear(); // Do not turn UNC/network resources into native file requests.
            if (!path.isEmpty()) {
                if (path.startsWith(QStringLiteral(":/"))) {
                    QFile resource(path);
                    constexpr qint64 maximumBytes = 128ll * 1024 * 1024;
                    if (resource.open(QIODevice::ReadOnly) && resource.size() <= maximumBytes)
                        image = desktopReadRasterBytes(resource.read(maximumBytes + 1)).image;
                } else image = desktopReadRasterFile(path).image;
            }
        }
        // An invalid QVariant would invoke document/global ResourceProvider and
        // QTextImageHandler's byte/path decoder. A typed null QImage is terminal.
        return QVariant::fromValue(image);
    }
};

} // namespace

ComposerTextEdit::ComposerTextEdit(QWidget* parent) : QTextEdit(parent) {
    setDocument(new ComposerRasterDocument(this));
    setAcceptDrops(true);
}

void ComposerTextEdit::setMimeHandler(std::function<bool(const QMimeData*)> handler) {
    mimeHandler_ = std::move(handler);
}

void ComposerTextEdit::setCornerAction(QWidget* action) {
    cornerAction_ = action;
    if (!cornerAction_) return;
    cornerAction_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::setLeadingAction(QWidget* action) {
    leadingAction_ = action;
    if (!leadingAction_) return;
    leadingAction_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::setLeadingHint(QWidget* hint) {
    leadingHint_ = hint;
    if (!leadingHint_) return;
    leadingHint_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::setSecondaryLeadingAction(QWidget* action) {
    secondaryLeadingAction_ = action;
    if (!action) return;
    action->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::positionCornerAction() {
    if (cornerAction_) {
        const int inset = qMax(UiZoom::s(6), cornerAction_->width() / 5);
        cornerAction_->move(width() - cornerAction_->width() - inset,
                            height() - cornerAction_->height() - inset);
        cornerAction_->raise();
    }
    if (leadingAction_) {
        const int horizontalInset = UiZoom::s(12);
        const int verticalInset = UiZoom::s(10);
        setViewportMargins(0, 0, 0, leadingAction_->height() + UiZoom::s(10));
        leadingAction_->move(horizontalInset,
                             height() - leadingAction_->height() - verticalInset);
        leadingAction_->raise();
        if (secondaryLeadingAction_) {
            secondaryLeadingAction_->move(leadingAction_->x() + leadingAction_->width() + UiZoom::s(8),
                                         leadingAction_->y());
            secondaryLeadingAction_->raise();
        }
        if (leadingHint_) {
            leadingHint_->move(leadingAction_->x(),
                               leadingAction_->y() - leadingHint_->height() - UiZoom::s(5));
            leadingHint_->raise();
        }
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
}

void ComposerTextEdit::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    positionCornerAction();
}

bool ComposerTextEdit::canInsertFromMimeData(const QMimeData* source) const {
    if (hasLocalFile(source)) return true;
    return QTextEdit::canInsertFromMimeData(source);
}

void ComposerTextEdit::insertFromMimeData(const QMimeData* source) {
    if (mimeHandler_ && mimeHandler_(source)) return;
    QTextEdit::insertFromMimeData(source);
}

void ComposerTextEdit::dragEnterEvent(QDragEnterEvent* event) {
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dragEnterEvent(event);
}

void ComposerTextEdit::dragMoveEvent(QDragMoveEvent* event) {
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dragMoveEvent(event);
}

bool ComposerTextEdit::hasLocalFile(const QMimeData* source) {
    if (!source || !source->hasUrls()) return false;
    for (const QUrl& url : source->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) return true;
    }
    return false;
}
