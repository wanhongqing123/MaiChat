// 文件作用：验证桌面端IM Markdown 消息在真实桌面窗口中的排版。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFile>
#include <QDir>
#include <QPushButton>
#include "ui/UiZoom.h"
#include <QImage>
#include <QTest>

#include "app/RemoteIMApplication.h"
#include "im/FakeRemoteIMClient.h"
#include "ui/MainWindow.h"

// 视觉自检程序：把**真的 MainWindow** 停在聊天页，灌几条 markdown 消息，存成 PNG。
//
// 和 AgentPageVisualProbe 是一对：那个看 AI 助手页，这个看 IM。两边现在走的是
// 同一套渲染（MarkdownDocument → MarkdownLayout → 自己画），所以**两张图并排看**
// 才算验完——只看一边看不出「两边不一致」这种问题。
//
// 不用登录，每次结果一样（FakeRemoteIMClient + 直接构造 MainWindow）。
//
//   im_markdown_visual_probe <输出路径.png>

namespace {

const char* const kIncoming = R"(这一条是**收到**的长消息，用来看正文、行内代码和列表。

## 这周的改动

- 消息气泡换成了自己绘制，不再走 `QTextDocument`
- 行内代码是带内边距的胶囊，比如 `MarkdownLayout::paint`
- 任务列表是真的复选框：
  - [x] 块树
  - [x] 排版
  - [ ] 语法高亮

```cpp
// 一篇内容排一次版，多次绘制，按可见区裁剪
layout.paint(&painter, origin, clip);
```

> [!TIP]
> 主题和 AI 助手页共用一份 `MarkdownTheme`，外观要调只改一个地方。

| 层 | 职责 |
| --- | --- |
| MarkdownDocument | **预期**：<br>320×180<br><br>单元格内保留空行 |
| MarkdownLayout | 块树 → 像素 |
| MarkdownLabel | **实际**：<br/>320×240<br />下一行<BR>最后一行 |

代码示例保持原样：`literal <br>`。
)";

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    // Product main registers this same shared face; probes must do so as well.
    if (QFontDatabase::addApplicationFont(QStringLiteral(MAICHAT_CODE_FONT_FILE)) < 0) return 2;
    QString fixture;
    if (argc > 2) {
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly)) return 2;
        fixture = QString::fromUtf8(file.readAll());
    }
    QDir fontDirectory(QString::fromUtf8(MAICHAT_TEST_FONTS_DIR));
    for (const QString& name : fontDirectory.entryList({QStringLiteral("*.otf")}, QDir::Files))
        QFontDatabase::addApplicationFont(fontDirectory.filePath(name));
    if (qEnvironmentVariableIsSet("MAICHAT_PROBE_FONT_FAMILY"))
        UiZoom::setFontFamilyId(qEnvironmentVariable("MAICHAT_PROBE_FONT_FAMILY"));
    QFont selectedFont = app.font();
    selectedFont.setFamilies(UiZoom::fontFamilies());
    selectedFont.setPixelSize(UiZoom::textPixels(UiZoom::TextRole::Body));
    app.setFont(selectedFont);
#ifdef Q_OS_WIN
    // 和 main.cpp 设的应用字体保持一致，否则中文落到宋体、字距被整像素提示打乱，
    // 截图不代表真机。见 main.cpp 里 setHintingPreference 那段注释。
    QFont appFont;
    appFont.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI"),
                         QStringLiteral("Microsoft YaHei")});
    appFont.setPixelSize(13);
    app.setFont(appFont);
#endif
    const QString output =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("im-markdown.png");

    RemoteIMApplication imApplication(QStringLiteral("desktop-user"),
                                      std::make_unique<FakeRemoteIMClient>());
    imApplication.addContact(QStringLiteral("phone-user"), QStringLiteral("iPhone"));
    imApplication.selectPeer(QStringLiteral("phone-user"));
    imApplication.chatState().receiveText(QStringLiteral("phone-user"),
                                          fixture.isEmpty() ? QString::fromUtf8(kIncoming) : fixture);
    if (fixture.isEmpty()) imApplication.chatState().receiveText(
        QStringLiteral("phone-user"),
        QStringLiteral("顺手看一眼 [链接](https://example.com) 和 ~~删除线~~，还有短消息贴时间戳的样子。"));

    MainWindow window(imApplication);
    window.resize(1295, 857);
    window.show();
    QTest::qWaitForWindowExposed(&window);
    QTest::qWait(400);

    const QImage image = window.grab().toImage();
    if (!image.save(output)) {
        qWarning("failed to save %s", qPrintable(output));
        return 1;
    }
    if (auto* settings = window.findChild<QPushButton*>(QStringLiteral("settingsNavButton"))) {
        settings->click(); QTest::qWait(100);
        QImage settingsImage(window.size(), QImage::Format_ARGB32);
        settingsImage.fill(Qt::white); window.render(&settingsImage);
        QString settingsPath = output; settingsPath.replace(QStringLiteral(".png"), QStringLiteral("-settings.png"));
        if (!settingsImage.save(settingsPath)) return 1;
    }
    qInfo("wrote %s (%dx%d)", qPrintable(output), image.width(), image.height());
    return 0;
}
