// 文件作用：桌面端共用界面；MaiGraphicsMacLayer 负责macOS 图形层与窗口合成，本文件给出其实现。
// 实现边界：与 MaiGraphicsMacLayer 的公开声明配套，调用方通过相应接口进入。
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QWidget>
#include <QGuiApplication>
#include <QDebug>

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
