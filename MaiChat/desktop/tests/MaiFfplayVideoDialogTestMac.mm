// 测试宿主走真实 macOS 前台激活，避免后台 QtTest 的逻辑窗口状态代替屏幕状态。
#import <AppKit/AppKit.h>
#include <QString>
#include <QDebug>
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

static NSMenu* sTrackedShareMenu = nil;

bool clickFfplayTestNativeButton(QWidget* widget) {
    NSView* root = reinterpret_cast<NSView*>(widget->window()->winId());
    NSWindow* window = root.window;
    if (!window) return false;
    const QPoint center = widget->mapTo(widget->window(), widget->rect().center());
    const NSPoint local = NSMakePoint(center.x(), root.flipped ? center.y() : NSHeight(root.bounds) - center.y());
    const NSPoint point = [root convertPoint:local toView:nil];
    for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
        NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil
            eventNumber:0 clickCount:1 pressure:type == NSEventTypeLeftMouseDown ? 1.0 : 0.0];
        [NSApp postEvent:event atStart:NO];
    }
    return true;
}

void* scheduleFfplayTestSaveCancel(bool* observed) {
    NSTimer* timer = [NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer* active) {
        NSWindow* window = NSApp.modalWindow;
        if (window && [window respondsToSelector:@selector(cancel:)]) {
            qInfo() << "[native-save-panel]" << QString::fromUtf8(NSStringFromClass(window.class).UTF8String);
            *observed = true;
            [NSApp sendAction:@selector(cancel:) to:window from:nil];
            [active invalidate];
        }
    }];
    [timer retain];
    for (NSString* mode in @[NSModalPanelRunLoopMode, NSEventTrackingRunLoopMode, NSDefaultRunLoopMode])
        [NSRunLoop.mainRunLoop addTimer:timer forMode:mode];
    return timer;
}

void stopFfplayTestSaveCancel(void* token) {
    NSTimer* timer = (NSTimer*)token;
    [timer invalidate];
    [timer release];
}

void observeFfplayTestNativePanel() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSMenuDidBeginTrackingNotification
            object:nil queue:nil usingBlock:^(NSNotification* note) {
                sTrackedShareMenu = (NSMenu*)note.object;
                qInfo() << "[native-share-menu] begin items" << sTrackedShareMenu.numberOfItems;
            }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSMenuDidEndTrackingNotification
            object:nil queue:nil usingBlock:^(NSNotification*) {
                qInfo() << "[native-share-menu] end";
                sTrackedShareMenu = nil;
            }];
    });
}

// Close only the isolated test application's transient native picker. Never
// choose a share destination or write a file during the control wiring check.
bool cancelFfplayTestNativePanel() {
    if (sTrackedShareMenu) {
        [sTrackedShareMenu cancelTrackingWithoutAnimation];
        return true;
    }
    NSWindow* popup = nil;
    for (NSWindow* window in NSApp.windows) {
        if (!window.visible) continue;
        NSString* name = NSStringFromClass(window.class);
        qInfo() << "[native-share-window]" << QString::fromUtf8(name.UTF8String)
                << "level" << window.level << "key" << window.keyWindow;
        if ([name containsString:@"Popover"] || [name containsString:@"Sharing"] ||
            [name containsString:@"Menu"]) popup = window;
    }
    if (!popup) return false;
    NSEvent* escape = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:popup.windowNumber context:nil
        characters:@"\x1b" charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:53];
    [NSApp postEvent:escape atStart:YES];
    return true;
}
