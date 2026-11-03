// 测试宿主走真实 macOS 前台激活，避免后台 QtTest 的逻辑窗口状态代替屏幕状态。
#import <AppKit/AppKit.h>
#include <QString>
#include <QWidget>

void activateFfplayTestApplication() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

bool ffplayTestWindowInteractive(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    return [NSApp isActive] && [view.window isOnActiveSpace];
}

void activateFfplayTestWindow(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    // 测试 App 由远程终端启动，显式移入当前 Space 后才检验原生全屏和鼠标唤醒。
    window.collectionBehavior =
        NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenPrimary;
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

QString ffplayTestNativeState(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    NSWindow* window = view.window;
    return QStringLiteral("active=%1 space=%2 nativeFrame=%3 mouse=%4 frontmost=%5")
        .arg([NSApp isActive])
        .arg([window isOnActiveSpace])
        .arg(QString::fromUtf8(NSStringFromRect(window.frame).UTF8String))
        .arg(QString::fromUtf8(NSStringFromPoint([NSEvent mouseLocation]).UTF8String))
        .arg(QString::fromUtf8(NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier.UTF8String));
}
