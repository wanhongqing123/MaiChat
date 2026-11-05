// 文件作用：播放器持久拥有的原生浮层，为 Qt 控件提供真实的透明窗口合成。
#pragma once

#include <QCoreApplication>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QWidget>

// 顶层 Tool 自带原生窗口，Windows layered window 和 Cocoa 内容视图均可按像素合成。
// 窗口自构造起保持不变；背景、几何、显隐和 owner 状态跟随由播放器统一管理，
// 不通过 reparent、改 flags 或截图背景在普通模式与全屏模式之间切换。
class MaiVideoHud final : public QWidget {
public:
    explicit MaiVideoHud(QWidget* owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), owner_(owner) {
        // 必须在原生窗口创建前启用 alpha，不能等进入全屏才改变窗口格式。
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_QuitOnClose, false);
        setAttribute(Qt::WA_DeleteOnClose, false);
        // ShowWithoutActivating 仅限制 show 时抢焦点，仍允许用户点击控件后操作键盘。
        // 不设置 WindowDoesNotAcceptFocus，也不让 Mac Tool 在 App 失焦后继续悬浮。
        setFocusPolicy(Qt::StrongFocus);
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        // Alt+F4/系统关闭命令应关闭播放器，不能只留下正在播放而没有控件的 owner。
        if (owner_) {
            event->ignore();
            owner_->close();
        } else QWidget::closeEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        // 透明顶层带有 NoSystemBackground；显式绘制样式背景，才能保留渐变的逐像素 alpha。
        QStyleOption option;
        option.initFrom(this);
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    }

    void keyPressEvent(QKeyEvent* event) override {
        QWidget::keyPressEvent(event);
        if (!event->isAccepted()) forwardKeyEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        QWidget::keyReleaseEvent(event);
        if (!event->isAccepted()) forwardKeyEvent(event);
    }

private:
    void forwardKeyEvent(QKeyEvent* event) {
        if (!owner_) return;
        // 子控件已先处理按键；只有抵达浮层的剩余事件交还播放器。
        // 保留原生键值、重复与文本信息，并同时转发 release，避免按下/松开分属两个窗口。
        QKeyEvent forwarded(event->type(), event->key(), event->modifiers(), event->nativeScanCode(),
                            event->nativeVirtualKey(), event->nativeModifiers(), event->text(),
                            event->isAutoRepeat(), event->count());
        QCoreApplication::sendEvent(owner_.data(), &forwarded);
        // owner 已有一次正常的事件分发；阻止 QApplication 再沿 QObject 父链重复送达。
        event->accept();
    }

    // 关闭 owner 并不等于销毁它；显隐归播放器控制，只有实际 QObject 析构才销毁浮层。
    QPointer<QWidget> owner_;
};
