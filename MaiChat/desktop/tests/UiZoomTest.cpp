// 文件作用：验证桌面 UI 缩放比例边界和像素尺寸转换。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QtTest/QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QFontDatabase>
#include <QFontInfo>
#include "markdown/MarkdownLabel.h"

#include "ui/UiZoom.h"

class UiZoomTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void clampsFactorRange();
    void scalesPixelValues();
    void scalesPixelValuesInQss();
    void sharesTextRolesWithoutScalingGeometryTwice();
    void changesExistingEditorsAndMarkdownWithoutLosingDrafts();
    void fontFamilyUpdatesExistingControlsAndMarkdown();
};

void UiZoomTest::cleanup() {
    UiZoom::setFactor(1.0);
    UiZoom::setTextPreset(QStringLiteral("standard"));
    UiZoom::setFontFamilyId(QStringLiteral("system"));
}

void UiZoomTest::initTestCase() {
    for (const QString& name : {QStringLiteral("NotoSansSC-Regular"), QStringLiteral("NotoSansSC-Medium"),
             QStringLiteral("NotoSansSC-Bold"), QStringLiteral("NotoSerifSC-Regular"),
             QStringLiteral("NotoSerifSC-Medium"), QStringLiteral("NotoSerifSC-SemiBold"), QStringLiteral("NotoSerifSC-Bold")}) {
        QVERIFY(QFontDatabase::addApplicationFont(QString::fromUtf8(MAICHAT_TEST_FONTS_DIR) + QLatin1Char('/') + name + QStringLiteral(".otf")) >= 0);
    }
}

void UiZoomTest::fontFamilyUpdatesExistingControlsAndMarkdown() {
    QLineEdit editor;
    editor.setText(QStringLiteral("Hello 你好 draft"));
    editor.setSelection(1, 4);
    UiZoom::setStyle(&editor, QStringLiteral("font-size: @body;"));
    // 正常界面先完成控件初始化；Qt 6 对隐藏顶层控件排队交付字体变化事件。
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));
    MarkdownLabel markdown;
    markdown.setMarkdown(QStringLiteral("# Heading\n\nHello 你好 body `code`"));
    for (const QString& id : {QStringLiteral("notoSans"), QStringLiteral("notoSerif"), QStringLiteral("system")}) {
        QVERIFY(UiZoom::availableFontFamilyIds().contains(id));
        UiZoom::setFontFamilyId(id);
        UiZoom::refreshText();
        // 系统默认携带平台字体和 CJK 回退列表；让 Qt 独立解析同一完整请求，
        // 不能用仅首个 family 的请求替代它，也不能在字体事件交付前读取旧值。
        QFont expectedFont;
        expectedFont.setFamilies(UiZoom::fontFamilies());
        expectedFont.setPixelSize(UiZoom::textPixels(UiZoom::TextRole::Body));
        QTRY_COMPARE(QFontInfo(editor.font()).family(), QFontInfo(expectedFont).family());
        QCOMPARE(editor.text(), QStringLiteral("Hello 你好 draft"));
        QCOMPARE(editor.selectionStart(), 1);
        QCOMPARE(editor.selectedText().size(), 4);
        QCOMPARE(markdown.theme().bodyFamily, UiZoom::fontFamilies().first());
        QCOMPARE(markdown.theme().bodyPixelSize, 16);
        QVERIFY(markdown.theme().codeFamily != markdown.theme().bodyFamily);
    }
}

void UiZoomTest::sharesTextRolesWithoutScalingGeometryTwice() {
    UiZoom::setFactor(1.0);
    UiZoom::setTextPreset(QStringLiteral("standard"));
    QCOMPARE(UiZoom::textPixels(UiZoom::TextRole::Body), 16);
    QCOMPARE(UiZoom::textPixels(UiZoom::TextRole::Heading1), 22);
    UiZoom::setTextPreset(QStringLiteral("large"));
    QCOMPARE(UiZoom::textPixels(UiZoom::TextRole::Body), 18);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("font-size: @body; padding: 8px;")),
             QStringLiteral("font-size: 18px; padding: 8px;"));
    UiZoom::setTextPreset(QStringLiteral("extraLarge"));
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::textPixels(UiZoom::TextRole::Body), 30);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("font-size: @body; padding: 8px;")),
             QStringLiteral("font-size: 30px; padding: 12px;"));
}

void UiZoomTest::changesExistingEditorsAndMarkdownWithoutLosingDrafts() {
    QLineEdit editor;
    editor.setText(QStringLiteral("Hello 你好 draft"));
    editor.setSelection(2, 5);
    UiZoom::setStyle(&editor, QStringLiteral("font-size: @body; padding: 8px;"));
    MarkdownLabel markdown;
    markdown.setMarkdown(QStringLiteral("# Heading\n\nHello **你好** `code`"));
    for (const QString& preset : {QStringLiteral("large"), QStringLiteral("extraLarge"), QStringLiteral("standard")}) {
        UiZoom::setTextPreset(preset);
        UiZoom::refreshText();
        QCOMPARE(editor.text(), QStringLiteral("Hello 你好 draft"));
        QCOMPARE(editor.selectionStart(), 2);
        QCOMPARE(editor.selectedText().size(), 5);
        QCOMPARE(markdown.theme().bodyPixelSize, UiZoom::textPixels(UiZoom::TextRole::Body));
        QCOMPARE(markdown.theme().codePixelSize, UiZoom::textPixels(UiZoom::TextRole::Detail));
    }
}

void UiZoomTest::clampsFactorRange() {
    QCOMPARE(UiZoom::setFactor(1.0), 1.0);
    QCOMPARE(UiZoom::setFactor(9.0), UiZoom::maxFactor());
    QCOMPARE(UiZoom::setFactor(0.1), UiZoom::minFactor());
    QCOMPARE(UiZoom::setFactor(1.3), 1.3);
}

void UiZoomTest::scalesPixelValues() {
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::s(10), 15);
    QCOMPARE(UiZoom::s(0), 0);
    // 1px 边框等非零最小值不被缩没。
    UiZoom::setFactor(0.8);
    QCOMPARE(UiZoom::s(1), 1);
}

void UiZoomTest::scalesPixelValuesInQss() {
    UiZoom::setFactor(1.5);
    const QString scaled = UiZoom::scaleQss(
        QStringLiteral("font-size: 13px; padding: 0px 8px; border: 1px solid #000;"));
    QCOMPARE(scaled, QStringLiteral("font-size: 20px; padding: 0px 12px; border: 2px solid #000;"));

    // 倍率 1.0 时原样返回。
    UiZoom::setFactor(1.0);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("margin: 7px;")), QStringLiteral("margin: 7px;"));

    // Markdown 渲染器输出的 HTML 内嵌 <style> 也按同一套规则缩放（消息正文随整体缩放）。
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("<style>body{font-size:14px;}h1{font-size:22px;}</style>")),
             QStringLiteral("<style>body{font-size:21px;}h1{font-size:33px;}</style>"));
}

QTEST_MAIN(UiZoomTest)
#include "UiZoomTest.moc"
