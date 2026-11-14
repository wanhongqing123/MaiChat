// 仅测试进程自己的 PDFKit 窗口；不抓屏、不请求 TCC、不操作用户窗口。
#import <AppKit/AppKit.h>
#import <PDFKit/PDFKit.h>
#include <QString>
#include <QByteArray>

bool closeOwnedAgentFilePdfPreview(const QString& title) {
    const QByteArray utf8 = title.toUtf8();
    NSString* expected = [NSString stringWithUTF8String:utf8.constData()];
    for (NSWindow* window in NSApp.windows) {
        if (!window.visible || ![window.title isEqualToString:expected] ||
            ![window.contentView isKindOfClass:PDFView.class]) continue;
        PDFView* view = static_cast<PDFView*>(window.contentView);
        if (view.document.pageCount == 0) continue;
        [window close];
        return true;
    }
    return false;
}
