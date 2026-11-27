// 视频时间轴的鼠标定位与标准悬停时间提示；不直接操作解码器。
#include "ui/MaiVideoSeekSlider.h"

#include <QMouseEvent>
#include <QStyleOptionSlider>
#include <QToolTip>
#include <algorithm>
#include <cmath>

MaiVideoSeekSlider::MaiVideoSeekSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    setMouseTracking(true);
}
void MaiVideoSeekSlider::setDurationUs(int64_t duration) {
    durationUs_ = std::max<int64_t>(0, duration);
}
void MaiVideoSeekSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(event);
        return;
    }
    // 拖动时左侧时钟已实时显示目标；不要在鼠标抓取期间另建原生 tooltip 窗口。
    QToolTip::hideText();
    setToolTip(QString());
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    dragOffset_ = handle.contains(event->pos()) ? event->pos().x() - handle.center().x() : 0;
    setSliderDown(true);
    moveTo(event->pos().x());
    event->accept();
}
void MaiVideoSeekSlider::mouseMoveEvent(QMouseEvent *event) {
    if (isSliderDown()) {
        moveTo(event->pos().x());
        event->accept();
        return;
    }
    if (durationUs_ > 0 && maximum() > minimum()) {
        const auto us =
            int64_t(std::llround(valueAt(event->pos().x(), isSliderDown() ? dragOffset_ : 0) *
                                 double(durationUs_) / maximum()));
        const auto seconds = us / 1000000;
        const QString text =
            QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
        // 同时更新标准 tooltip，避免鼠标停住后的 Qt ToolTip 事件用空提示覆盖实时提示。
        setToolTip(text);
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    }
    QSlider::mouseMoveEvent(event);
}
void MaiVideoSeekSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && isSliderDown()) {
        moveTo(event->pos().x());
        setSliderDown(false);
        event->accept();
    } else
        QSlider::mouseReleaseEvent(event);
}
void MaiVideoSeekSlider::leaveEvent(QEvent *event) {
    QToolTip::hideText();
    QSlider::leaveEvent(event);
}
int MaiVideoSeekSlider::valueAt(int x, int offset) const {
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const int span = std::max(1, groove.width() - handle.width());
    return QStyle::sliderValueFromPosition(minimum(), maximum(),
                                           x - offset - groove.x() - handle.width() / 2, span,
                                           option.upsideDown);
}
void MaiVideoSeekSlider::moveTo(int x) { setSliderPosition(valueAt(x, dragOffset_)); }
