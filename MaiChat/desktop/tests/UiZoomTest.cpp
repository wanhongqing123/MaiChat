// 文件作用：验证桌面 UI 缩放、语义字号、偏好重载与逻辑像素边界。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QtTest/QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>
#include "markdown/MarkdownLabel.h"

#include "ui/UiZoom.h"

namespace {

struct PreferenceProbe {
    QJsonObject values;
    QString error;
};

void preferenceChildMarker(const char* stage) {
    std::fprintf(stderr, "[text-preset-child] %s\n", stage);
    std::fflush(stderr);
}

// 每次重新启动真实 UiZoom 消费者，避免静态缓存掩盖偏好加载问题；只使用临时 Ini。
PreferenceProbe probePreference(const QString& directory, const QString& mode,
                                const QString& preset, qreal screenScale = 1.0) {
    QProcess child;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_SCALE_FACTOR"), QString::number(screenScale));
    environment.remove(QStringLiteral("QT_SCREEN_SCALE_FACTORS"));
    environment.insert(QStringLiteral("QT_DEBUG_PLUGINS"), QStringLiteral("1"));
    child.setProcessEnvironment(environment);
    child.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--text-preset-child"), directory, mode, preset,
         QStringLiteral("-platform"), QStringLiteral("offscreen")});
    if (!child.waitForStarted(5000)) return {{}, child.errorString()};
    if (!child.waitForFinished(10000)) {
        child.kill();
        child.waitForFinished(5000);
        return {{}, QStringLiteral("Text preference child timed out; exit=%1; stdout=%2; stderr=%3")
            .arg(child.exitCode()).arg(QString::fromUtf8(child.readAllStandardOutput()),
                                      QString::fromUtf8(child.readAllStandardError()))};
    }
    const QByteArray output = child.readAllStandardOutput();
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)
        return {{}, QStringLiteral("Preference child exited with code %1: ").arg(child.exitCode())
            + QString::fromUtf8(child.readAllStandardError()) + QString::fromUtf8(output)};
    QJsonParseError parseError;
    const QJsonDocument result = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !result.isObject())
        return {{}, QStringLiteral("Invalid preference probe output: ") + QString::fromUtf8(output)};
    return {result.object(), {}};
}

int runPreferenceChild(const QStringList& arguments) {
    preferenceChildMarker("configure private settings");
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatUiZoomTests"));
    QCoreApplication::setApplicationName(QStringLiteral("preferences"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, arguments.at(2));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, arguments.at(2));
    const QString mode = arguments.at(3);
    preferenceChildMarker("apply preference operation");
    if (mode == QStringLiteral("write")) UiZoom::setTextPreset(arguments.at(4));
    if (mode == QStringLiteral("seed")) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/textSize"), arguments.at(4));
        settings.sync();
        if (settings.status() != QSettings::NoError) return 1;
    }
    preferenceChildMarker("read UiZoom snapshot");
    const QJsonObject result{
        {QStringLiteral("preset"), UiZoom::textPreset()},
        {QStringLiteral("textFactor"), UiZoom::textFactor()},
        {QStringLiteral("zoomFactor"), UiZoom::factor()},
        {QStringLiteral("bodyPixels"), UiZoom::textPixels(UiZoom::TextRole::Body)},
        {QStringLiteral("headingPixels"), UiZoom::textPixels(UiZoom::TextRole::Heading1)},
        {QStringLiteral("devicePixelRatio"), QGuiApplication::primaryScreen()->devicePixelRatio()},
        {QStringLiteral("style"), UiZoom::scaleQss(QStringLiteral("font-size: @body; padding: 8px;"))}};
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    preferenceChildMarker("emit snapshot");
    return std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout)
            == static_cast<std::size_t>(output.size()) ? 0 : 2;
}

}  // namespace

class UiZoomTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void clampsFactorRange();
    void scalesPixelValues();
    void scalesPixelValuesInQss();
    void sharesTextRolesWithoutScalingGeometryTwice();
    void smallPresetScalesAllTextRoles();
    void textPresetPersistsAcrossProcesses_data();
    void textPresetPersistsAcrossProcesses();
    void smallPresetUsesLogicalPixelsAtEachScreenScale_data();
    void smallPresetUsesLogicalPixelsAtEachScreenScale();
    void invalidStoredPresetFallsBackToStandard_data();
    void invalidStoredPresetFallsBackToStandard();
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

void UiZoomTest::smallPresetScalesAllTextRoles() {
    UiZoom::setTextPreset(QStringLiteral("small"));
    QCOMPARE(UiZoom::textPreset(), QStringLiteral("small"));
    QCOMPARE(UiZoom::textFactor(), 0.875);
    const struct { UiZoom::TextRole role; int pixels; } expectations[] = {
        {UiZoom::TextRole::Body, 14}, {UiZoom::TextRole::Detail, 12},
        {UiZoom::TextRole::Metadata, 11}, {UiZoom::TextRole::Heading1, 19},
        {UiZoom::TextRole::Heading2, 18}, {UiZoom::TextRole::Heading3, 16},
        {UiZoom::TextRole::Title, 18}, {UiZoom::TextRole::Section, 16},
        {UiZoom::TextRole::Display, 25}, {UiZoom::TextRole::Hero, 28}};
    for (const auto& expectation : expectations)
        QCOMPARE(UiZoom::textPixels(expectation.role), expectation.pixels);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("font-size: @body; padding: 8px;")),
             QStringLiteral("font-size: 14px; padding: 8px;"));
    UiZoom::setFactor(1.5);
    QCOMPARE(UiZoom::textPixels(UiZoom::TextRole::Body), 21);
    QCOMPARE(UiZoom::scaleQss(QStringLiteral("font-size: @body; padding: 8px;")),
             QStringLiteral("font-size: 21px; padding: 12px;"));
}

void UiZoomTest::textPresetPersistsAcrossProcesses_data() {
    QTest::addColumn<QString>("preset");
    QTest::addColumn<int>("bodyPixels");
    QTest::newRow("small") << QStringLiteral("small") << 14;
    QTest::newRow("standard") << QStringLiteral("standard") << 16;
    QTest::newRow("large") << QStringLiteral("large") << 18;
    QTest::newRow("extraLarge") << QStringLiteral("extraLarge") << 20;
}

void UiZoomTest::textPresetPersistsAcrossProcesses() {
    QFETCH(QString, preset);
    QFETCH(int, bodyPixels);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto writer = probePreference(directory.path(), QStringLiteral("write"), preset);
    QVERIFY2(writer.error.isEmpty(), qPrintable(writer.error));
    QCOMPARE(writer.values.value(QStringLiteral("preset")).toString(), preset);
    const auto reader = probePreference(directory.path(), QStringLiteral("read"), {});
    QVERIFY2(reader.error.isEmpty(), qPrintable(reader.error));
    QCOMPARE(reader.values.value(QStringLiteral("preset")).toString(), preset);
    QCOMPARE(reader.values.value(QStringLiteral("bodyPixels")).toInt(), bodyPixels);
    QCOMPARE(reader.values.value(QStringLiteral("zoomFactor")).toDouble(), 1.0);
}

void UiZoomTest::smallPresetUsesLogicalPixelsAtEachScreenScale_data() {
    QTest::addColumn<qreal>("screenScale");
    QTest::newRow("100-percent") << qreal(1.0);
    QTest::newRow("150-percent") << qreal(1.5);
}

void UiZoomTest::smallPresetUsesLogicalPixelsAtEachScreenScale() {
    QFETCH(qreal, screenScale);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto probe = probePreference(directory.path(), QStringLiteral("write"), QStringLiteral("small"), screenScale);
    QVERIFY2(probe.error.isEmpty(), qPrintable(probe.error));
    QCOMPARE(probe.values.value(QStringLiteral("devicePixelRatio")).toDouble(), screenScale);
    QCOMPARE(probe.values.value(QStringLiteral("textFactor")).toDouble(), 0.875);
    QCOMPARE(probe.values.value(QStringLiteral("bodyPixels")).toInt(), 14);
    QCOMPARE(probe.values.value(QStringLiteral("headingPixels")).toInt(), 19);
    QCOMPARE(probe.values.value(QStringLiteral("style")).toString(),
             QStringLiteral("font-size: 14px; padding: 8px;"));
}

void UiZoomTest::invalidStoredPresetFallsBackToStandard_data() {
    QTest::addColumn<QString>("storedPreset");
    QTest::newRow("unknown") << QStringLiteral("obsolete-preset");
    QTest::newRow("empty") << QString();
    QTest::newRow("numeric") << QStringLiteral("0");
}

void UiZoomTest::invalidStoredPresetFallsBackToStandard() {
    QFETCH(QString, storedPreset);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto writer = probePreference(directory.path(), QStringLiteral("seed"), storedPreset);
    QVERIFY2(writer.error.isEmpty(), qPrintable(writer.error));
    const auto reader = probePreference(directory.path(), QStringLiteral("read"), {});
    QVERIFY2(reader.error.isEmpty(), qPrintable(reader.error));
    QCOMPARE(reader.values.value(QStringLiteral("preset")).toString(), QStringLiteral("standard"));
    QCOMPARE(reader.values.value(QStringLiteral("bodyPixels")).toInt(), 16);
    UiZoom::setTextPreset(storedPreset);
    QCOMPARE(UiZoom::textPreset(), QStringLiteral("standard"));
}

void UiZoomTest::changesExistingEditorsAndMarkdownWithoutLosingDrafts() {
    QLineEdit editor;
    editor.setText(QStringLiteral("Hello 你好 draft"));
    editor.setSelection(2, 5);
    UiZoom::setStyle(&editor, QStringLiteral("font-size: @body; padding: 8px;"));
    MarkdownLabel markdown;
    markdown.setMarkdown(QStringLiteral("# Heading\n\nHello **你好** `code`"));
    for (const QString& preset : {QStringLiteral("small"), QStringLiteral("large"), QStringLiteral("extraLarge"), QStringLiteral("standard")}) {
        UiZoom::setTextPreset(preset);
        UiZoom::refreshText();
        QCOMPARE(editor.text(), QStringLiteral("Hello 你好 draft"));
        QCOMPARE(editor.selectionStart(), 2);
        QCOMPARE(editor.selectedText().size(), 5);
        QCOMPARE(editor.styleSheet(), QStringLiteral("font-size: %1px; padding: 8px;")
            .arg(UiZoom::textPixels(UiZoom::TextRole::Body)));
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

int main(int argc, char** argv) {
    const bool preferenceChild = argc > 1 && QByteArray(argv[1]) == QByteArray("--text-preset-child");
    if (preferenceChild) preferenceChildMarker("before QApplication");
    QApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (preferenceChild) {
        std::fprintf(stderr, "[text-preset-child] after QApplication; argc=%lld; platform=%s\n",
            static_cast<long long>(arguments.size()), qPrintable(QGuiApplication::platformName()));
        std::fflush(stderr);
        if (arguments.size() != 5) return 3;
        return runPreferenceChild(arguments);
    }
    app.setAttribute(Qt::AA_Use96Dpi, true);
    UiZoomTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "UiZoomTest.moc"
