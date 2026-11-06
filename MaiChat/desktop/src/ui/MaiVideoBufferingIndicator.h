// 文件作用：视频等待首帧时只显示居中的加载图标，不把正常缓冲当成错误提示。
// 随可见性启停动画；透明命中保证图标不拦截播放器的鼠标操作。
#pragma once

#include <QHideEvent>
#include <QPainter>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

class MaiVideoBufferingIndicator final : public QWidget {
public:
    explicit MaiVideoBufferingIndicator(QWidget* parent) : QWidget(parent), timer_(this) {
        setObjectName(QStringLiteral("ffplayBufferingIndicator"));
        setAccessibleName(QStringLiteral("正在缓冲视频"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NativeWindow);
        setFixedSize(44, 44);
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this] { angle_ = (angle_ + 12) % 360; update(); });
        hide();
    }

protected:
    void showEvent(QShowEvent* event) override { QWidget::showEvent(event); timer_.start(); }
    void hideEvent(QHideEvent* event) override { timer_.stop(); QWidget::hideEvent(event); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        // 原生子窗口不能依赖应用的浅色 Window 调色板；加载期与黑色视频视口一致，
        // 否则白色旋转环在独立重绘/截图时会落到白底上，实际没有可见对比。
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF ring = QRectF(rect()).adjusted(7, 7, -7, -7);
        painter.setPen(QPen(QColor(255, 255, 255, 55), 3));
        painter.drawEllipse(ring);
        painter.setPen(QPen(QColor(255, 255, 255, 235), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(ring, -angle_ * 16, 100 * 16);
    }

private:
    QTimer timer_;
    int angle_ = 0;
};
