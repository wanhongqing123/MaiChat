// 测试宿主走真实 macOS 前台激活，避免后台 QtTest 的逻辑窗口状态代替屏幕状态。
#import <AppKit/AppKit.h>
#import <QuartzCore/CALayer.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QDebug>
#include <QWidget>
#include <vector>

namespace {
struct OwnedDrawableProbe {
    CAMetalLayer* layer;
    id<CAMetalDrawable> drawable;
    id<MTLCommandQueue> queue;
};
NSLock* drawableProbeLock = [[NSLock alloc] init];
OwnedDrawableProbe* drawableProbe = nullptr;
using NextDrawable = id<CAMetalDrawable> (*)(id, SEL);
NextDrawable originalNextDrawable = nullptr;

id<CAMetalDrawable> observeOwnedDrawable(id layer, SEL selector) {
    id<CAMetalDrawable> drawable = originalNextDrawable(layer, selector);
    [drawableProbeLock lock];
    if (drawableProbe && drawableProbe->layer == layer && drawable) {
        [drawable retain];
        [drawableProbe->drawable release];
        drawableProbe->drawable = drawable;
    }
    [drawableProbeLock unlock];
    return drawable;
}
}

// 只观察本进程自有播放器真正提交的 drawable，不使用屏幕捕获或 TCC API。
// 原 IMP 始终原样调用，末尾恢复；不更换 layer、不改变正式后端或持有其他 App 资源。
void* beginOwnedFfplayDrawableProbe(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    std::vector<CALayer*> pending{view.layer};
    CAMetalLayer* layer = nil;
    while (!pending.empty()) {
        CALayer* candidate = pending.back(); pending.pop_back();
        if ([candidate isKindOfClass:CAMetalLayer.class]) { layer = (CAMetalLayer*)candidate; break; }
        for (CALayer* child in candidate.sublayers) pending.push_back(child);
    }
    if (!layer || !layer.device) return nullptr;
    auto* probe = new OwnedDrawableProbe{[layer retain], nil, [layer.device newCommandQueue]};
    if (!probe->queue) { [layer release]; delete probe; return nullptr; }
    [drawableProbeLock lock];
    drawableProbe = probe;
    Method method = class_getInstanceMethod(CAMetalLayer.class, @selector(nextDrawable));
    originalNextDrawable = (NextDrawable)method_getImplementation(method);
    method_setImplementation(method, (IMP)observeOwnedDrawable);
    [drawableProbeLock unlock];
    return probe;
}

void endOwnedFfplayDrawableProbe(void* value) {
    auto* probe = static_cast<OwnedDrawableProbe*>(value);
    if (!probe) return;
    [drawableProbeLock lock];
    Method method = class_getInstanceMethod(CAMetalLayer.class, @selector(nextDrawable));
    method_setImplementation(method, (IMP)originalNextDrawable);
    drawableProbe = nullptr;
    [probe->drawable release];
    [probe->queue release];
    [probe->layer release];
    [drawableProbeLock unlock];
    delete probe;
}

QImage copyOwnedFfplayDrawable(void* value) {
    auto* probe = static_cast<OwnedDrawableProbe*>(value);
    [drawableProbeLock lock];
    id<CAMetalDrawable> drawable = [probe->drawable retain];
    [drawableProbeLock unlock];
    if (!drawable) return {};
    id<MTLTexture> texture = drawable.texture;
    const auto format = texture.pixelFormat;
    if (format != MTLPixelFormatBGRA8Unorm && format != MTLPixelFormatBGRA8Unorm_sRGB) {
        [drawable release]; return {};
    }
    const NSUInteger width = texture.width, height = texture.height;
    const NSUInteger stride = (width * 4 + 255) & ~NSUInteger(255);
    id<MTLBuffer> buffer = [probe->layer.device newBufferWithLength:stride * height
        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [probe->queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
    [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(width, height, 1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:stride destinationBytesPerImage:stride * height];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    QImage image;
    if (buffer && command.status == MTLCommandBufferStatusCompleted)
        image = QImage(static_cast<const uchar*>(buffer.contents), int(width), int(height), int(stride),
            QImage::Format_ARGB32).copy();
    [buffer release];
    [drawable release];
    return image;
}

// 只读采样自有播放器的实际原生几何，避免用 Qt 的目标状态代替转场中的显示层。
QJsonObject ffplayTestVideoLayerGeometry(QWidget* widget) {
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    CALayer* root = view.layer;
    QJsonObject value{{"nativeWidth", NSWidth(view.bounds)}, {"nativeHeight", NSHeight(view.bounds)},
                      {"nativeFullscreen", bool(view.window.styleMask & NSWindowStyleMaskFullScreen)}};
    value.insert("rootLayerClass", QString::fromUtf8(NSStringFromClass(root.class).UTF8String));
    value.insert("rootLayerWidth", CGRectGetWidth(root.frame));
    value.insert("rootLayerHeight", CGRectGetHeight(root.frame));
    value.insert("rootAnimationCount", int(root.animationKeys.count));
    value.insert("windowLevel", double(view.window.level));
    value.insert("nativeResizable", bool(view.window.styleMask & NSWindowStyleMaskResizable));
    value.insert("windowVisible", bool(view.window.visible));
    value.insert("windowFrame", QString::fromUtf8(NSStringFromRect(view.window.frame).UTF8String));
    value.insert("windowNumber", double(view.window.windowNumber));
    value.insert("parentWindowNumber", double(view.window.parentWindow.windowNumber));
    // orderedWindows excludes panels. Query only this test process's visible
    // native windows so Tool HUDs participate in the same real z-order sample.
    const NSUInteger order = [[NSWindow windowNumbersWithOptions:0]
        indexOfObject:@(view.window.windowNumber)];
    value.insert("windowOrder", order == NSNotFound ? -1 : int(order));
    std::vector<CALayer*> pending{root};
    while (!pending.empty()) {
        CALayer* candidate = pending.back();
        pending.pop_back();
        for (CALayer* child in candidate.sublayers) pending.push_back(child);
        if (![candidate isKindOfClass:CAMetalLayer.class]) continue;
        CAMetalLayer* metal = static_cast<CAMetalLayer*>(candidate);
        value.insert("metalLayerWidth", CGRectGetWidth(metal.frame));
        value.insert("metalLayerHeight", CGRectGetHeight(metal.frame));
        value.insert("metalDrawableWidth", metal.drawableSize.width);
        value.insert("metalDrawableHeight", metal.drawableSize.height);
        value.insert("metalParentClass", QString::fromUtf8(NSStringFromClass(metal.superlayer.class).UTF8String));
        CALayer* shown = metal.presentationLayer ?: metal;
        value.insert("shownMetalWidth", CGRectGetWidth(shown.frame));
        value.insert("shownMetalHeight", CGRectGetHeight(shown.frame));
        value.insert("metalAnimationCount", int(metal.animationKeys.count));
        break;
    }
    CALayer* shownRoot = root.presentationLayer;
    value.insert("shownViewportWidth", CGRectGetWidth(shownRoot ? shownRoot.bounds : root.bounds));
    value.insert("shownViewportHeight", CGRectGetHeight(shownRoot ? shownRoot.bounds : root.bounds));
    for (CALayer* layer in root.sublayers) {
        if (![layer isKindOfClass:NSClassFromString(@"AVPlayerLayer")]) continue;
        const CGRect videoRect = static_cast<AVPlayerLayer*>(layer).videoRect;
        value.insert("videoRectWidth", CGRectGetWidth(videoRect));
        value.insert("videoRectHeight", CGRectGetHeight(videoRect));
        value.insert("videoBoundsWidth", CGRectGetWidth(layer.bounds));
        value.insert("videoBoundsHeight", CGRectGetHeight(layer.bounds));
        CALayer* shown = layer.presentationLayer;
        value.insert("videoLayerIdentity", QString::number(reinterpret_cast<quintptr>(layer)));
        value.insert("videoWidth", CGRectGetWidth(layer.frame));
        value.insert("videoHeight", CGRectGetHeight(layer.frame));
        value.insert("shownWidth", CGRectGetWidth(shown ? shown.frame : layer.frame));
        value.insert("shownHeight", CGRectGetHeight(shown ? shown.frame : layer.frame));
        value.insert("animationCount", int(layer.animationKeys.count));
    }
    return value;
}

void resizeFfplayTestNativeWindow(QWidget* widget, QSize size) {
    NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
    NSRect frame = window.frame;
    frame.origin.y += frame.size.height - size.height();
    frame.size = NSMakeSize(size.width(), size.height());
    [window setFrame:frame display:YES];
}

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
