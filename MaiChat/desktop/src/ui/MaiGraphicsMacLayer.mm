// 文件作用：桌面端共用界面；MaiGraphicsMacLayer 负责macOS 图形层与窗口合成，本文件给出其实现。
// 实现边界：与 MaiGraphicsMacLayer 的公开声明配套，调用方通过相应接口进入。
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <QWidget>
#include <QGuiApplication>
#include <QDebug>
#include <QPointer>
#include <memory>
#include "ui/MaiMacVideoWindow.h"

namespace {
struct FullscreenObserver {
  QPointer<QWidget> owner;
  void* context;
  MaiMacFullscreenCallback callback;
  NSMutableArray* tokens;
  std::shared_ptr<bool> active = std::make_shared<bool>(true);
};
}

void* maiObserveMacVideoFullscreen(QWidget* widget, void* context, MaiMacFullscreenCallback callback) {
  if (!widget || !callback || QGuiApplication::platformName() != QStringLiteral("cocoa")) return nullptr;
  NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
  if (!window) return nullptr;
  auto* observer = new FullscreenObserver{widget, context, callback, [[NSMutableArray alloc] init]};
  NSArray* names = @[NSWindowWillEnterFullScreenNotification, NSWindowDidEnterFullScreenNotification,
                     NSWindowWillExitFullScreenNotification, NSWindowDidExitFullScreenNotification];
  for (NSUInteger index = 0; index < names.count; ++index) {
    const auto phase = static_cast<MaiMacFullscreenPhase>(index);
    id token = [NSNotificationCenter.defaultCenter addObserverForName:names[index] object:window queue:nil
        usingBlock:^(NSNotification*) {
          if (phase == MaiMacFullscreenPhase::WillEnter || phase == MaiMacFullscreenPhase::WillExit) {
            if (observer->owner) observer->callback(observer->context, phase);
            return;
          }
          // Did 通知仍在 AppKit 完成栈内。Qt 队列可被原生事件递归冲刷，不能据此
          // 立即反向 toggle；先回到主 dispatch 队列，再让 GUI 消费下一次意图。
          const QPointer<QWidget> owner = observer->owner;
          const auto active = observer->active;
          const auto callback = observer->callback;
          void* context = observer->context;
          dispatch_async(dispatch_get_main_queue(), ^{
            if (*active && owner) callback(context, phase);
          });
        }];
    [observer->tokens addObject:token];
  }
  return observer;
}

void maiReleaseMacVideoFullscreenObserver(void* value) {
  auto* observer = static_cast<FullscreenObserver*>(value);
  if (!observer) return;
  *observer->active = false;
  for (id token in observer->tokens) [NSNotificationCenter.defaultCenter removeObserver:token];
  [observer->tokens release];
  delete observer;
}

extern "C" void *maiGraphicsCreateMacLayer(QWidget *widget) {
  widget->setAttribute(Qt::WA_NativeWindow);
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  [view setWantsLayer:YES];
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.frame = view.bounds;
  layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
  [view setLayer:layer];
  return [layer retain];
}

extern "C" void maiGraphicsResizeMacLayer(void *nativeLayer, QWidget *widget) {
  if (!nativeLayer || !widget)
    return;
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  CAMetalLayer *layer = static_cast<CAMetalLayer *>(nativeLayer);
  // QWidget 的 resizeEvent 先于 Cocoa 子 NSView 的待处理几何提交。这里再读
  // view.bounds 会把旧的 480x270 写回 Metal，首个可见帧只覆盖窗口的一角。
  // 使用同一轮 Qt 已确定的客户区尺寸，并在映射前提交，不等待下一轮隐式事务。
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  // Cocoa Qt 把 Metal 放在 QContainerLayer 中。其首次 layoutSublayers 也必须
  // 在同一轮完成，否则稍后的容器布局会暂时把内容层写回旧边界。
  [view layoutSubtreeIfNeeded];
  [view.layer layoutIfNeeded];
  layer.frame = CGRectMake(0, 0, widget->width(), widget->height());
  layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
  [CATransaction commit];
  [CATransaction flush];
}

extern "C" void maiGraphicsRetainMacLayer(void *nativeLayer) {
  [static_cast<CAMetalLayer *>(nativeLayer) retain];
}

extern "C" void maiGraphicsReleaseMacLayer(void *nativeLayer) {
  [static_cast<CAMetalLayer *>(nativeLayer) release];
}

extern "C" void maiGraphicsStyleMacVideoWindow(QWidget *widget) {
  // WId is platform-plugin opaque. Only cocoa supplies an NSView pointer;
  // offscreen uses small integer handles that must never receive ObjC messages.
  if (!widget || QGuiApplication::platformName() != QStringLiteral("cocoa"))
    return;
  NSView *view = reinterpret_cast<NSView *>(widget->winId());
  NSWindow *window = view.window;
  if (!window)
    return;
  // Qt 的最终几何准备好后直接映射同一窗口；不再叠加 AppKit 默认的弹出缩放，
  // 否则窗口服务器仍可先显示中间大小，Qt/native frame 检查却始终是目标矩形。
  window.animationBehavior = NSWindowAnimationBehaviorNone;
  window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  window.titlebarAppearsTransparent = YES;
  window.backgroundColor = [NSColor colorWithCalibratedRed:0.03 green:0.05 blue:0.08 alpha:1.0];
}

extern "C" void maiGraphicsAlignMacVideoHuds(QWidget *owner, QWidget *header, QWidget *footer) {
  if (!owner || !header || !footer || QGuiApplication::platformName() != QStringLiteral("cocoa"))
    return;
  NSWindow *ownerWindow = reinterpret_cast<NSView *>(owner->winId()).window;
  NSWindow *headerWindow = reinterpret_cast<NSView *>(header->winId()).window;
  NSWindow *footerWindow = reinterpret_cast<NSView *>(footer->winId()).window;
  if (!ownerWindow || !headerWindow || !footerWindow)
    return;
  // Qt::Tool 默认浮在 App 所有普通窗之上；QObject/transient owner 并不限制该层级。
  // 保留 Qt 管理的同一对窗口，以 owner 的层级和相邻顺序显示，不重建 Qt 对象或更改 Qt 父链。
  headerWindow.level = footerWindow.level = ownerWindow.level;
  // QObject/transient owner 不会建立 AppKit 窗口组。独立 Tool 会留在原 Space，
  // 即使 Qt/native visible 均为 true，视频进入原生全屏后也看不到控制栏。
  // 持久绑定同一 native owner，随该窗口进入/退出 Space，不追随其它激活的主窗。
  for (NSWindow *hud : @[headerWindow, footerWindow]) {
    const NSWindowCollectionBehavior behavior =
        (hud.collectionBehavior &
         ~(NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenPrimary)) |
        NSWindowCollectionBehaviorFullScreenAuxiliary;
    if (hud.collectionBehavior != behavior)
      hud.collectionBehavior = behavior;
    if (hud.parentWindow != ownerWindow) {
      if (hud.parentWindow)
        [hud.parentWindow removeChildWindow:hud];
      [ownerWindow addChildWindow:hud ordered:NSWindowAbove];
    }
  }
  // Cocoa 的 Qt::Tool 默认还带 Resizable。原生边角可先截获拖动，只拉大 HUD，
  // 视频 owner 完全收不到 resize；HUD 只能由 owner 布局，拖动仍交还共用 Qt 处理。
  for (NSWindow *hud : @[headerWindow, footerWindow]) {
    if (hud.styleMask & NSWindowStyleMaskResizable)
      hud.styleMask &= ~NSWindowStyleMaskResizable;
  }
  NSArray<NSNumber *> *windows = [NSWindow windowNumbersWithOptions:0];
  const NSUInteger ownerOrder = [windows indexOfObject:@(ownerWindow.windowNumber)];
  if (ownerOrder == NSNotFound)
    return;
  bool realign = false;
  for (NSWindow *hud : @[headerWindow, footerWindow]) {
    const NSUInteger hudOrder = [windows indexOfObject:@(hud.windowNumber)];
    if (hudOrder == NSNotFound)
      continue;
    if (hudOrder > ownerOrder) {
      realign = true;
      continue;
    }
    for (NSUInteger index = hudOrder + 1; index < ownerOrder; ++index) {
      if (![windows[index] isEqualToNumber:@(headerWindow.windowNumber)] &&
          ![windows[index] isEqualToNumber:@(footerWindow.windowNumber)])
        realign = true;
    }
  }
  // 只修复 owner/HUD 倒序或被其它主窗隔开的次序。相邻于 owner 上方，不能抢到
  // 盖住 owner 的主窗、菜单或其它应用之前；正常鼠标移动不重复 raise。
  if (realign) {
    [headerWindow orderWindow:NSWindowAbove relativeTo:ownerWindow.windowNumber];
    [footerWindow orderWindow:NSWindowAbove relativeTo:ownerWindow.windowNumber];
  }
}

extern "C" void maiGraphicsHideMacClosingVideoWindow(QWidget *widget) {
  if (!widget || QGuiApplication::platformName() != QStringLiteral("cocoa"))
    return;
  NSWindow *window = reinterpret_cast<NSView *>(widget->winId()).window;
  if (!window)
    return;
  // 关闭已请求退出全屏后立即撤出显示，窗口和 Qt delegate 继续存活到 DidExit。
  // 不销毁原生宿主，也不展示 showNormal 的中间窗口。
  window.animationBehavior = NSWindowAnimationBehaviorNone;
  [window orderOut:nil];
}

extern "C" void maiGraphicsShareMacVideo(QWidget *anchor, const char *utf8Path) {
  if (!anchor || !utf8Path || !*utf8Path)
    return;
  NSView *view = reinterpret_cast<NSView *>(anchor->winId());
  NSString *path = [NSString stringWithUTF8String:utf8Path];
  if (qEnvironmentVariableIntValue("MAICHAT_TRACE_VIDEO_CONTROLS") == 1)
    qInfo() << "[native-share-anchor]" << "window" << bool(view.window)
            << "active" << bool(NSApp.active) << "view" << QString::fromUtf8(NSStringFromClass(view.class).UTF8String)
            << "bounds" << QString::fromUtf8(NSStringFromRect(view.bounds).UTF8String)
            << "visible" << QString::fromUtf8(NSStringFromRect(view.visibleRect).UTF8String)
            << "hidden" << bool(view.hiddenOrHasHiddenAncestor);
  if (!view.window || !path)
    return;
  NSURL *url = [NSURL fileURLWithPath:path];
  NSSharingServicePicker *picker = [[NSSharingServicePicker alloc] initWithItems:@[ url ]];
  if (qEnvironmentVariableIntValue("MAICHAT_TRACE_VIDEO_CONTROLS") == 1)
    qInfo() << "[native-share-services]" << [NSSharingService sharingServicesForItems:@[url]].count;
  [picker showRelativeToRect:view.bounds ofView:view preferredEdge:NSRectEdgeMinY];
  [picker release];
}
