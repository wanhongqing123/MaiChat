// 文件作用：共用播放器的窄竖向音量浮层；只管理呈现，音量/静音仍由宿主的同一核心入口执行。
#pragma once

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

class MaiVideoVolumePopup final : public QMenu {
public:
    explicit MaiVideoVolumePopup(QToolButton* anchor) : QMenu(anchor), anchor_(anchor) {
        setObjectName(QStringLiteral("ffplayVolumeMenu"));
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(56, 152);
        setStyleSheet(QStringLiteral("QMenu {background:transparent;border:0;padding:0;}"));
        mute_ = new QAction(QStringLiteral("静音"), this);
        mute_->setObjectName(QStringLiteral("ffplayMuteAction"));
        mute_->setCheckable(true);
        auto* content = new QWidget(this);
        content->setFixedSize(size());
        content->setStyleSheet(QStringLiteral("QWidget {background:transparent;}"));
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(8, 6, 8, 12);
        layout->setSpacing(8);
        readout_ = new QToolButton(content);
        readout_->setObjectName(QStringLiteral("ffplayVolumeReadout"));
        readout_->setFixedSize(40, 22);
        readout_->setCheckable(true);
        readout_->setCursor(Qt::PointingHandCursor);
        readout_->setStyleSheet(QStringLiteral(
            "QToolButton {color:#f4f4f4;background:transparent;border:0;border-radius:4px;font-size:11px;padding:0;}"
            "QToolButton:hover {background:rgba(255,255,255,22);}"
            "QToolButton:checked {color:#bbbbbb;}"));
        slider_ = new QSlider(Qt::Vertical, content);
        slider_->setObjectName(QStringLiteral("ffplayVolumeSlider"));
        slider_->setAccessibleName(QStringLiteral("音量"));
        slider_->setRange(0, 100);
        slider_->setSingleStep(5);
        slider_->setPageStep(10);
        slider_->setValue(100);
        slider_->setFixedSize(22, 104);
        slider_->setStyleSheet(QStringLiteral(
            "QSlider::groove:vertical {width:2px;background:rgba(255,255,255,55);border-radius:1px;margin:4px 0;}"
            "QSlider::sub-page:vertical {background:rgba(255,255,255,55);border-radius:1px;}"
            "QSlider::add-page:vertical {background:#ffffff;border-radius:1px;}"
            "QSlider::handle:vertical {height:8px;margin:0 -3px;background:#ffffff;border:0;border-radius:4px;}"));
        layout->addWidget(readout_, 0, Qt::AlignHCenter);
        layout->addWidget(slider_, 0, Qt::AlignHCenter);
        auto* action = new QWidgetAction(this);
        action->setDefaultWidget(content);
        addAction(action);
        // 百分比本身是可达的静音入口，不用额外文字行撑宽弹层；原 QAction 和 M 键继续可用。
        connect(readout_, &QToolButton::clicked, mute_, &QAction::trigger);
        connect(slider_, &QSlider::valueChanged, this, [this] { refreshReadout(); });
        connect(mute_, &QAction::toggled, this, [this] { refreshReadout(); });
        // owned HUD 是独立原生窗口。沿 QObject/widget owner 链监听，隐藏、最小化或关闭时收起，
        // 移动/拉伸时重新锚定；不让 Popup 脱离播放器继续留在桌面上。
        for (QWidget* widget = anchor; widget; widget = widget->parentWidget()) widget->installEventFilter(this);
        refreshReadout();
    }

    QSlider* slider() const { return slider_; }
    QAction* muteAction() const { return mute_; }

    void refreshReadout() {
        readout_->setText(QString::number(slider_->value()) + QLatin1Char('%'));
        const QSignalBlocker blocker(readout_);
        readout_->setChecked(mute_->isChecked());
        const QString description = QStringLiteral("音量 %1%；%2，点击%3 (M)")
            .arg(slider_->value())
            .arg(mute_->isChecked() ? QStringLiteral("已静音") : QStringLiteral("有声"))
            .arg(mute_->isChecked() ? QStringLiteral("取消静音") : QStringLiteral("静音"));
        readout_->setAccessibleName(description);
        readout_->setToolTip(description);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        // 自绘背景保留逐像素 alpha 和圆角，内容由 QWidgetAction 的真实控件绘制。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(42, 42, 42, 220));
        painter.drawRoundedRect(QRectF(rect()), 9, 9);
    }

    void showEvent(QShowEvent* event) override {
        QMenu::showEvent(event);
        positionAboveAnchor();
        slider_->setFocus(Qt::PopupFocusReason);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_M && event->modifiers() == Qt::NoModifier) {
            mute_->trigger();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
            event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown ||
            event->key() == Qt::Key_Home || event->key() == Qt::Key_End) {
            QKeyEvent forwarded(event->type(), event->key(), event->modifiers(), event->text(),
                                event->isAutoRepeat(), event->count());
            QCoreApplication::sendEvent(slider_, &forwarded);
            event->accept();
            return;
        }
        QMenu::keyPressEvent(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (isVisible()) {
            if (event->type() == QEvent::Hide || event->type() == QEvent::Close)
                hide();
            else if (event->type() == QEvent::Move || event->type() == QEvent::Resize)
                positionAboveAnchor();
            else if (event->type() == QEvent::WindowStateChange) {
                auto* widget = qobject_cast<QWidget*>(watched);
                if (widget && widget->isMinimized()) hide();
                else positionAboveAnchor();
            }
        }
        return QMenu::eventFilter(watched, event);
    }

private:
    void positionAboveAnchor() {
        if (!anchor_) return;
        const QPoint anchor = anchor_->mapToGlobal(QPoint(anchor_->width() / 2, 0));
        QPoint position(anchor.x() - width() / 2, anchor.y() - height() - 8);
        QScreen* screen = QGuiApplication::screenAt(anchor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect bounds = screen->availableGeometry();
            position.setX(qBound(bounds.left(), position.x(), qMax(bounds.left(), bounds.right() - width() + 1)));
            position.setY(qBound(bounds.top(), position.y(), qMax(bounds.top(), bounds.bottom() - height() + 1)));
        }
        move(position);
    }

    QPointer<QToolButton> anchor_;
    QSlider* slider_ = nullptr;
    QToolButton* readout_ = nullptr;
    QAction* mute_ = nullptr;
};
