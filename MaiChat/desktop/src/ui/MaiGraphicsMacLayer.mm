// 文件作用：桌面端共用界面；MaiGraphicsMacLayer 负责macOS 图形层与窗口合成，本文件给出其实现。
// 实现边界：与 MaiGraphicsMacLayer 的公开声明配套，调用方通过相应接口进入。
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

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
  layer.frame = view.bounds;
  layer.contentsScale = view.window ? view.window.backingScaleFactor : 1.0;
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
  window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  window.titlebarAppearsTransparent = YES;
  window.backgroundColor = [NSColor colorWithCalibratedRed:0.03 green:0.05 blue:0.08 alpha:1.0];
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
