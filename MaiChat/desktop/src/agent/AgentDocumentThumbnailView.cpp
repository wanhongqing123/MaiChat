// 文件作用：复用普通首屏渲染的卡片内容视图；布局仅显示当前绑定，所有鼠标事件落回原文件按钮。
#include "agent/AgentDocumentThumbnailView.h"
#include <QEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

AgentDocumentThumbnailView::AgentDocumentThumbnailView(QWidget* parent,
                                                       AgentDocumentThumbnailBinding::AccessGuard accessGuard,
                                                       ThemeProvider theme, LayoutChanged layoutChanged)
    : QWidget(parent), theme_(std::move(theme)), layoutChanged_(std::move(layoutChanged)) {
    setObjectName(QStringLiteral("agentDocumentThumbnail"));
    setAccessibleName(QStringLiteral("文档首屏缩略图"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(0);
    binding_ = new AgentDocumentThumbnailBinding(
        this,
        [this](const auto& result) {
            image_ = result.image;
            setProperty("agentThumbnailState", image_.isNull() ? "unavailable" : "ready");
            setVisible(!image_.isNull());
            updateGeometry();
            update();
            notifyLayoutChanged();
        },
        std::move(accessGuard));
    connect(timer_, &QTimer::timeout, this, [this] {
        if (request_.path.isEmpty()) return;
        auto request = request_;
        request.logicalSize = QSize(qBound(64, width(), 1024), qBound(32, height(), 768));
        request.devicePixelRatio = devicePixelRatioF();
        if (theme_) {
            const auto theme = theme_();
            request.markdownScale = theme.first;
            request.bodyFamily = theme.second;
        }
        binding_->request(request);
    });
    hide();
}
void AgentDocumentThumbnailView::setDocument(const AgentDocumentThumbnailRequest& request) {
    if (request_.path != request.path || request_.mimeType != request.mimeType) {
        binding_->clear();
        image_ = {};
    }
    request_ = request;
    setProperty("agentThumbnailState", "loading");
    show();
    updateGeometry();
    schedule();
    notifyLayoutChanged();
}
void AgentDocumentThumbnailView::clear() {
    timer_->stop();
    binding_->clear();
    request_ = {};
    image_ = {};
    setProperty("agentThumbnailState", "unavailable");
    hide();
    updateGeometry();
    notifyLayoutChanged();
}
const QImage& AgentDocumentThumbnailView::thumbnail() const {
    return image_;
}
QString AgentDocumentThumbnailView::sourcePath() const {
    return request_.path;
}
QSize AgentDocumentThumbnailView::sizeHint() const {
    return {312, 192};
}
QSize AgentDocumentThumbnailView::minimumSizeHint() const {
    return {64, 32};
}
bool AgentDocumentThumbnailView::hasHeightForWidth() const {
    return true;
}
int AgentDocumentThumbnailView::heightForWidth(int width) const {
    return qMax(32, qRound(width * 192.0 / 312));
}
void AgentDocumentThumbnailView::schedule() {
    if (timer_ && !request_.path.isEmpty()) timer_->start();
}
void AgentDocumentThumbnailView::notifyLayoutChanged() {
    // Keep the callback alive independently if remeasurement removes this card.
    const auto changed = layoutChanged_;
    if (changed) changed();
}
void AgentDocumentThumbnailView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    if (image_.isNull()) {
        painter.setPen(QColor(QStringLiteral("#667085")));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("正在生成缩略图…"));
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(rect()), image_);
    }
}
void AgentDocumentThumbnailView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    schedule();
}
bool AgentDocumentThumbnailView::event(QEvent* event) {
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::ScreenChangeInternal)
        schedule();
    return QWidget::event(event);
}
