// Run the real screenshot/capture-control/computer chain on an owned display.
// No visual model, account, browser profile, external site, or user content is used.
#include <QApplication>
#include <QCryptographicHash>
#include <QAbstractNativeEventFilter>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <algorithm>
#include <limits>
#include <optional>
#include <thread>
#include <vector>
#include <json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiScreenshotTool.h"
#include "MaiTool.h"
#include "agent/DesktopScreenCaptureControl.h"

namespace {
using Json = nlohmann::json;

class MouseWitness final : public QAbstractNativeEventFilter {
public:
    HWND target = nullptr;
    QPoint lastPress;
    QString characters;
    int presses = 0;
    int controlClicks = 0;
    int shiftClicks = 0;
    int verticalWheel = 0;
    int horizontalWheel = 0;
    std::vector<UINT> buttonDowns;
    std::vector<UINT> buttonUps;
    bool nativeEventFilter(const QByteArray&, void* value, qintptr*) override {
        const auto* message = static_cast<MSG*>(value);
        if (GetAncestor(message->hwnd, GA_ROOT) == target) {
            if (message->message == WM_MOUSEWHEEL) verticalWheel += short(HIWORD(message->wParam));
            if (message->message == WM_MOUSEHWHEEL) horizontalWheel += short(HIWORD(message->wParam));
            if (message->message == WM_LBUTTONDOWN || message->message == WM_RBUTTONDOWN ||
                message->message == WM_MBUTTONDOWN || message->message == WM_XBUTTONDOWN)
                buttonDowns.push_back(message->message);
            if (message->message == WM_LBUTTONUP || message->message == WM_RBUTTONUP ||
                message->message == WM_MBUTTONUP || message->message == WM_XBUTTONUP)
                buttonUps.push_back(message->message);
            if (message->message == WM_LBUTTONDOWN) {
                if (LOWORD(message->wParam) & MK_CONTROL) ++controlClicks;
                if (LOWORD(message->wParam) & MK_SHIFT) ++shiftClicks;
            }
        }
        if (message->message == WM_CHAR && GetAncestor(message->hwnd, GA_ROOT) == target)
            characters += QChar(ushort(message->wParam));
        if (message->message == WM_LBUTTONDOWN && GetAncestor(message->hwnd, GA_ROOT) == target) {
            POINT point{short(LOWORD(message->lParam)), short(HIWORD(message->lParam))};
            if (ClientToScreen(message->hwnd, &point)) {
                lastPress = QPoint(point.x, point.y);
                ++presses;
            }
        }
        return false;
    }
};

bool ownsDisplay(const DesktopNativeDisplay& display, HWND backdrop) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == backdrop) return true;
        if (!IsWindowVisible(window) || IsIconic(window)) continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return false;
        if (bounds.right <= display.x || bounds.bottom <= display.y || bounds.left >= display.x + display.width ||
            bounds.top >= display.y + display.height)
            continue;
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId()) return false;
    }
    return false;
}

MaiToolResult invoke(MaiTool* tool, const Json& arguments, const MaiToolContext& context) {
    std::optional<MaiToolResult> result;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        result.emplace(tool->execute(arguments.dump(), context));
        finished = true;
    });
    while (!finished.load()) QTest::qWait(5);
    worker.join();
    return std::move(*result);
}

QPointF markerCenter(const QImage& image, QRgb color = 0xcc33ff) {
    QRect marker;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if ((image.pixel(x, y) & 0xffffff) == (color & 0xffffff)) marker |= QRect(x, y, 1, 1);
    return marker.isEmpty() ? QPointF(-1, -1) : QRectF(marker).center();
}

// A disposable slider makes the release point observable without any website.
// The deliberately short drag below must remain unaligned after mouse-up.
class HeldSlider final : public QWidget {
public:
    int handleX = 160;
    int presses = 0;
    int releases = 0;
    int heldMoves = 0;
    bool held = false;
    static constexpr int targetX = 620;
    static constexpr int trackY = 220;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#263147"));
        painter.fillRect(120, trackY - 4, 560, 8, QColor("#8b9cb8"));
        painter.fillRect(targetX - 18, trackY - 20, 36, 40, QColor("#df8547"));
        painter.fillRect(handleX - 18, trackY - 20, 36, 40, QColor("#28af77"));
        painter.setPen(Qt::white);
        painter.drawText(
            120, 150,
            held ? QStringLiteral("Owned slider: mouse held") : QStringLiteral("Owned slider: mouse released"));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        ++presses;
        held = true;
        update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton)) return;
        ++heldMoves;
        handleX = qRound(event->position().x());
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        ++releases;
        held = false;
        update();
    }
};
}  // namespace

class DesktopComputerScreenshotChainNativeTest : public QObject {
    Q_OBJECT
private slots:
    void screenshotPixelsFocusAndFreshSnapshotsCompose_data();
    void screenshotPixelsFocusAndFreshSnapshotsCompose();
    void screenshotPreservesHeldButtonAndAtomicDragReleases();
};

void DesktopComputerScreenshotChainNativeTest::screenshotPixelsFocusAndFreshSnapshotsCompose_data() {
    QTest::addColumn<QString>("sampleText");
    QTest::newRow("ascii") << QStringLiteral("MaiChat owned screenshot chain verified");
    QTest::newRow("bmp") << QStringLiteral("\u539f\u751f\u4e2d\u6587\u3001\u6807\u70b9 A");
    QTest::newRow("middle-pair") << QStringLiteral("A\U0001f600B");
    QTest::newRow("leading-pair") << QStringLiteral("\U0001f600A");
    QTest::newRow("trailing-pair") << QStringLiteral("A\U0001f600");
    QTest::newRow("cjk-pair") << QStringLiteral("\u4e2d\U0001f600\u6587");
    QTest::newRow("zwj-pairs") << QStringLiteral("A\U0001f469\u200d\U0001f4bb\U0001f600B");
    QTest::newRow("repeated-pairs") << QStringLiteral("\U0001f600\U0001f600");
}

void DesktopComputerScreenshotChainNativeTest::screenshotPixelsFocusAndFreshSnapshotsCompose() {
    QFETCH(QString, sampleText);
    if (!qEnvironmentVariableIsSet("MAICHAT_RUN_SCREENSHOT_COMPUTER_CHAIN"))
        QSKIP("Explicit owned screenshot/computer chain opt-in required");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        if (GetAsyncKeyState(key) & 0x8000) QSKIP("Physical mouse input is held");

    QWidget backdrop(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    backdrop.setStyleSheet(QStringLiteral("background:#263147"));
    backdrop.showFullScreen();
    QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
    const HWND backdropHandle = reinterpret_cast<HWND>(backdrop.winId());
    const auto displays = maiDesktopDisplays();
    QVERIFY2(displays, displays.error().message().c_str());
    DesktopNativeDisplay display;
    const auto monitor =
        std::uint64_t(reinterpret_cast<std::uintptr_t>(MonitorFromWindow(backdropHandle, MONITOR_DEFAULTTONULL)));
    for (const auto& item : displays.value())
        if (item.id == monitor) display = item;
    QVERIFY(display.id);

    QWidget form(&backdrop, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    form.setWindowTitle(QStringLiteral("MaiChat owned screenshot/computer chain"));
    form.setFocusPolicy(Qt::StrongFocus);
    form.setStyleSheet(QStringLiteral("background:#263147;color:white"));
    QLineEdit input(&form);
    input.setAccessibleName(QStringLiteral("Owned screenshot coordinate input"));
    input.setStyleSheet(QStringLiteral("background:#cc33ff;color:black;border:0"));
    input.setTextMargins(10, 2, 10, 2);
    form.show();
    const HWND handle = reinterpret_cast<HWND>(form.winId());
    MouseWitness mouse;
    mouse.target = handle;
    qApp->installNativeEventFilter(&mouse);
    const auto removeMouse = qScopeGuard([&] { qApp->removeNativeEventFilter(&mouse); });
    // Recreate the actual 1600-pixel display / 1528-pixel window relationship.
    QVERIFY(SetWindowPos(handle, HWND_TOPMOST, int(display.x), int(display.y), int(display.width),
                         int(display.height) - 72, SWP_SHOWWINDOW));
    QVERIFY(QTest::qWaitForWindowExposed(&form));
    input.setGeometry(80, form.height() - 90, 350, 40);
    form.setFocus();
    QTest::qWait(100);
    QVERIFY(ownsDisplay(display, backdropHandle));

    DesktopNativeInput activate;
    activate.desktopScope = true;
    activate.display = display;
    activate.kind = DesktopNativeInputKind::Click;
    activate.x = display.x + 30;
    activate.y = display.y + 30;
    const MaiError activated = maiDesktopInput(activate);
    QVERIFY2(!activated, activated.message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), handle);
    QVERIFY(!input.hasFocus());

    std::atomic<bool> canceled{false};
    QTimer guard;
    guard.setInterval(20);
    connect(&guard, &QTimer::timeout, this, [&] {
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        if (!ownsDisplay(display, backdropHandle) || foregroundProcess != GetCurrentProcessId()) canceled = true;
    });
    guard.start();
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    MaiToolContext context;
    context.sessionId = "owned-screenshot-computer-chain";
    context.root = workspace.path().toStdString();
    context.model = "glm-5.3-flash";
    context.cancel = &canceled;
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, std::make_shared<DesktopComputerController>());
    registry.add(makeMaiScreenshotTool());
    MaiTool* computer = registry.find("desktop_computer");
    MaiTool* screenshot = registry.find("screenshot");
    QVERIFY(computer && screenshot);
    DesktopScreenCaptureControl captureControl;
    QVERIFY(!captureControl.captureState().capturing);
    const auto cleanup = qScopeGuard([&] { maiStopScreenshotCapture(); });
    Json trace = Json::array();
    wchar_t pluginPath[32768]{};
    QVERIFY(GetModuleFileNameW(GetModuleHandleW(L"qwindows.dll"), pluginPath, 32768));
    QFile plugin(QString::fromWCharArray(pluginPath));
    QVERIFY(plugin.open(QIODevice::ReadOnly));
    const QByteArray pluginHash = QCryptographicHash::hash(plugin.readAll(), QCryptographicHash::Sha256).toHex();
    const QByteArray expectedPlugin = qgetenv("MAICHAT_EXPECT_QWINDOWS_SHA");
    if (!expectedPlugin.isEmpty()) QCOMPARE(pluginHash, expectedPlugin);
    trace.push_back({{"stage", "actual_loaded_windows_platform"}, {"sha256", pluginHash.toStdString()}});
    wchar_t guiPath[32768]{};
    QVERIFY(GetModuleFileNameW(GetModuleHandleW(L"Qt6Gui.dll"), guiPath, 32768));
    QFile gui(QString::fromWCharArray(guiPath));
    QVERIFY(gui.open(QIODevice::ReadOnly));
    const QByteArray guiHash = QCryptographicHash::hash(gui.readAll(), QCryptographicHash::Sha256).toHex();
    const QByteArray expectedGui = qgetenv("MAICHAT_EXPECT_QTGUI_SHA");
    if (!expectedGui.isEmpty()) QCOMPARE(guiHash, expectedGui);
    trace.push_back({{"stage", "actual_loaded_qt_gui"}, {"sha256", guiHash.toStdString()}});

    const auto capture = invoke(screenshot, {{"action", "capture"}}, context);
    QVERIFY2(!capture.hasError(), capture.error().message().c_str());
    const Json screen = Json::parse(capture.output());
    QCOMPARE(screen.value("mode", std::string()), std::string("display"));
    QVERIFY(!screen.contains("snapshot_id"));
    QCOMPARE(screen.at("width").get<int>(), int(display.width));
    QCOMPARE(screen.at("height").get<int>(), int(display.height));
    QVERIFY(capture.images().size() == 1);
    QImage image(QString::fromStdString(capture.images()[0].path));
    QVERIFY(!image.isNull());
    QPointF point = markerCenter(image);
    QVERIFY(point.x() >= 0);
    captureControl.refresh();
    QVERIFY(captureControl.captureState().capturing);
    const auto initialCapture = captureControl.captureState();

    const auto observeWindow = [&] {
        return invoke(
            computer,
            {{"action", "observe"}, {"target", "window"}, {"window_id", std::to_string(std::uint64_t(form.winId()))}},
            context);
    };
    auto observed = observeWindow();
    QVERIFY2(!observed.hasError(), observed.error().message().c_str());
    Json window = Json::parse(observed.output());
    const Json bounds = window.at("bounds");
    QCOMPARE(bounds.at("height").get<double>(), display.height - 72);
    const auto click = [&](const Json& snapshot, bool screenRatios) {
        return invoke(
            computer,
            {{"action", "click"},
             {"target", "window"},
             {"window_id", snapshot.at("window_id")},
             {"snapshot_id", snapshot.at("snapshot_id")},
             {"x_ratio", screenRatios ? point.x() / (image.width() - 1)
                                      : (display.x + point.x() - snapshot.at("bounds").at("x").get<double>()) /
                                            (snapshot.at("bounds").at("width").get<double>() - 1)},
             {"y_ratio", screenRatios ? point.y() / (image.height() - 1)
                                      : (display.y + point.y() - snapshot.at("bounds").at("y").get<double>()) /
                                            (snapshot.at("bounds").at("height").get<double>() - 1)}},
            context);
    };
    const auto wrong = click(window, true);
    QVERIFY2(!wrong.hasError(), wrong.error().message().c_str());
    QTest::qWait(40);
    QVERIFY2(!input.hasFocus(), "Whole-display ratios must demonstrably miss this shorter window's field");
    QVERIFY(mouse.presses > 0);
    const int expectedScreenY = qRound(display.y + point.y());
    QVERIFY(expectedScreenY - mouse.lastPress.y() > 50);
    trace.push_back({{"stage", "mixed_display_ratios_on_window"},
                     {"missPixelsY", expectedScreenY - mouse.lastPress.y()},
                     {"nativeEventX", mouse.lastPress.x()},
                     {"nativeEventY", mouse.lastPress.y()},
                     {"displayHeight", image.height()},
                     {"windowHeight", bounds.at("height")}});

    const Json staleInput{{"action", "type"},
                          {"target", "window"},
                          {"window_id", window.at("window_id")},
                          {"snapshot_id", window.at("snapshot_id")},
                          {"text", "must not type"}};
    const auto stale = invoke(computer, staleInput, context);
    QVERIFY(stale.hasError());
    const auto anotherScreenshot = invoke(screenshot, {{"action", "capture"}}, context);
    QVERIFY(!anotherScreenshot.hasError());
    QVERIFY(invoke(computer, staleInput, context).hasError());
    QVERIFY(input.text().isEmpty());
    trace.push_back({{"stage", "consumed_snapshot_stays_invalid_after_screenshot"}, {"rejected", true}});

    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const auto correct = click(window, false);
    QVERIFY2(!correct.hasError(), correct.error().message().c_str());
    QTRY_VERIFY(input.hasFocus());
    QCOMPARE(mouse.lastPress, QPoint(qRound(display.x + point.x()), qRound(display.y + point.y())));
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const std::string expected = sampleText.toUtf8().toStdString();
    const auto typed = invoke(computer,
                              {{"action", "type"},
                               {"target", "window"},
                               {"window_id", window.at("window_id")},
                               {"snapshot_id", window.at("snapshot_id")},
                               {"text", expected}},
                              context);
    QVERIFY2(!typed.hasError(), typed.error().message().c_str());
    QTest::qWait(50);
    qInfo() << "[unicode-order] nativeCharactersMatch" << (mouse.characters.toUtf8().toStdString() == expected)
            << "renderedTextMatches" << (input.text().toStdString() == expected);
    QTRY_COMPARE(input.text().toStdString(), expected);
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const auto verified = invoke(computer,
                                 {{"action", "verify"},
                                  {"target", "window"},
                                  {"window_id", window.at("window_id")},
                                  {"snapshot_id", window.at("snapshot_id")},
                                  {"expected_text", expected}},
                                 context);
    QVERIFY2(!verified.hasError(), verified.error().message().c_str());
    QVERIFY(Json::parse(verified.output()).value("verified", false));
    trace.push_back({{"stage", "fresh_window_mapping_click_type_uia_readback"}, {"verified", true}});
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const auto replaced = invoke(computer,
                                 {{"action", "replace_text"},
                                  {"target", "window"},
                                  {"window_id", window.at("window_id")},
                                  {"snapshot_id", window.at("snapshot_id")},
                                  {"text", expected}},
                                 context);
    QVERIFY2(!replaced.hasError(), replaced.error().message().c_str());
    QTRY_COMPARE(input.text().toStdString(), expected);
    trace.push_back({{"stage", "selected_text_replacement_keeps_complete_unicode_order"}, {"verified", true}});

    QWidget other(&backdrop, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    other.setStyleSheet(QStringLiteral("background:#34805f"));
    other.setGeometry(40, 40, 220, 100);
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    RECT otherBounds{};
    QVERIFY(GetWindowRect(reinterpret_cast<HWND>(other.winId()), &otherBounds));
    activate.x = otherBounds.left + 25;
    activate.y = otherBounds.top + 25;
    const MaiError switched = maiDesktopInput(activate);
    QVERIFY2(!switched, switched.message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(other.winId()));
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const auto focused = invoke(computer,
                                {{"action", "focus"},
                                 {"target", "window"},
                                 {"window_id", window.at("window_id")},
                                 {"snapshot_id", window.at("snapshot_id")}},
                                context);
    qInfo() << "[focus-native-immediate] returnedError" << focused.hasError() << "foregroundMatches"
            << (GetForegroundWindow() == handle);
    if (focused.hasError()) {
        QTest::qWait(50);
        qInfo() << "[focus-native-after-owner-dispatch] foregroundMatches" << (GetForegroundWindow() == handle);
    }
    QVERIFY2(!focused.hasError(), focused.error().message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), handle);
    QVERIFY(invoke(computer,
                   {{"action", "type"},
                    {"target", "window"},
                    {"window_id", window.at("window_id")},
                    {"snapshot_id", window.at("snapshot_id")},
                    {"text", "must not type after focus without observing"}},
                   context)
                .hasError());
    QCOMPARE(input.text().toStdString(), expected);
    trace.push_back({{"stage", "other_owned_foreground_then_explicit_focus"},
                     {"nativeForegroundRestored", true},
                     {"focusSnapshotSingleUse", true}});
    other.hide();

    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    form.move(form.pos() + QPoint(35, 17));
    QTest::qWait(100);
    POINT beforeGeometryAttempt{};
    QVERIFY(GetCursorPos(&beforeGeometryAttempt));
    const auto staleGeometry = invoke(computer,
                                      {{"action", "type"},
                                       {"target", "window"},
                                       {"window_id", window.at("window_id")},
                                       {"snapshot_id", window.at("snapshot_id")},
                                       {"text", "must not type using old geometry"}},
                                      context);
    QVERIFY(staleGeometry.hasError());
    QCOMPARE(input.text().toStdString(), expected);
    POINT afterGeometryAttempt{};
    QVERIFY(GetCursorPos(&afterGeometryAttempt));
    QCOMPARE(QPoint(afterGeometryAttempt.x, afterGeometryAttempt.y),
             QPoint(beforeGeometryAttempt.x, beforeGeometryAttempt.y));
    trace.push_back({{"stage", "window_move_invalidates_old_observation"}, {"rejectedBeforeInput", true}});
    const auto movedCapture = invoke(screenshot, {{"action", "capture"}}, context);
    QVERIFY2(!movedCapture.hasError(), movedCapture.error().message().c_str());
    image = QImage(QString::fromStdString(movedCapture.images()[0].path));
    QVERIFY(!image.isNull());
    point = markerCenter(image);
    QVERIFY(point.x() >= 0);

    input.clear();
    form.setFocus();
    QTest::qWait(50);
    QVERIFY(!input.hasFocus());
    const auto displayObserved = invoke(
        computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}}, context);
    QVERIFY2(!displayObserved.hasError(), displayObserved.error().message().c_str());
    Json displaySnapshot = Json::parse(displayObserved.output());
    // Select from the very image bound to this token, not the earlier standalone
    // screenshot. The following click -> returned observation -> type is the
    // exact sequence described to the model.
    const QImage boundImage(QString::fromStdString(displayObserved.images().at(0).path));
    QVERIFY(!boundImage.isNull());
    const QPointF boundPoint = markerCenter(boundImage);
    QVERIFY(boundPoint.x() >= 0);
    const auto displayClick = invoke(computer,
                                     {{"action", "click"},
                                      {"target", "display"},
                                      {"display_id", std::to_string(display.id)},
                                      {"snapshot_id", displaySnapshot.at("snapshot_id")},
                                      {"x_ratio", boundPoint.x() / (boundImage.width() - 1)},
                                      {"y_ratio", boundPoint.y() / (boundImage.height() - 1)}},
                                     context);
    QVERIFY2(!displayClick.hasError(), displayClick.error().message().c_str());
    const Json displayResult = Json::parse(displayClick.output());
    QVERIFY(displayResult.value("dispatched", false));
    QVERIFY(displayResult.at("observation").at("frame_sequence").get<std::uint64_t>() >
            displaySnapshot.at("frame_sequence").get<std::uint64_t>());
    QTRY_VERIFY(input.hasFocus());
    QVERIFY(displayClick.images().size() == 1);
    QVERIFY(displayResult.at("observation").at("snapshot_id") != displaySnapshot.at("snapshot_id"));
    const auto displayType = [&](const Json& token, const std::string& text) {
        return invoke(computer,
                      {{"action", "type"},
                       {"target", "display"},
                       {"display_id", std::to_string(display.id)},
                       {"snapshot_id", token.at("snapshot_id")},
                       {"text", text}},
                      context);
    };
    QVERIFY(displayType(displaySnapshot, "must not type with the consumed click token").hasError());
    const std::string suffix = " [fresh display token]";
    const auto displayTyped = displayType(displayResult.at("observation"), expected + suffix);
    QVERIFY2(!displayTyped.hasError(), displayTyped.error().message().c_str());
    QTRY_COMPARE(input.text().toUtf8().toStdString(), expected + suffix);
    QVERIFY(displayTyped.images().size() == 1);
    const Json typedDisplay = Json::parse(displayTyped.output());
    QVERIFY(typedDisplay.at("observation").at("snapshot_id") != displayResult.at("observation").at("snapshot_id"));
    if (QString::fromLatin1(QTest::currentDataTag()) == QStringLiteral("ascii")) {
        for (const char* action : {"type", "replace_text"}) {
            const auto current = invoke(
                computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}},
                context);
            QVERIFY2(!current.hasError(), current.error().message().c_str());
            const auto held = invoke(computer,
                                     {{"action", "key_down"},
                                      {"target", "display"},
                                      {"display_id", std::to_string(display.id)},
                                      {"snapshot_id", Json::parse(current.output()).at("snapshot_id")},
                                      {"key", "Ctrl"}},
                                     context);
            QVERIFY2(!held.hasError(), held.error().message().c_str());
            QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
            const QString textBefore = input.text();
            const auto literal =
                invoke(computer,
                       {{"action", action},
                        {"target", "display"},
                        {"display_id", std::to_string(display.id)},
                        {"snapshot_id", Json::parse(held.output()).at("observation").at("snapshot_id")},
                        {"text", "literal text must not become a shortcut"}},
                       context);
            qInfo() << "[literal-held-modifier]" << action << "error" << literal.hasError() << "textUnchanged"
                    << (input.text() == textBefore);
            QVERIFY2(literal.hasError(), "Literal text insertion must reject a held shortcut modifier");
            QCOMPARE(literal.error().code(), MaiErrorCode::Busy);
            QCOMPARE(input.text(), textBefore);
            QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
        }
    }
    trace.push_back({{"stage", "bound_display_observe_click_new_snapshot_type"},
                     {"oldTokenRejected", true},
                     {"newTokenAccepted", true},
                     {"exactTextReadBack", true}});
    captureControl.refresh();
    QCOMPARE(captureControl.captureState().workerStarts, initialCapture.workerStarts);
    QCOMPARE(captureControl.captureState().sessionStarts, initialCapture.sessionStarts);
    trace.push_back({{"stage", "bound_display_click_with_new_frame"}, {"focused", true}, {"oneCaptureWorker", true}});
    DesktopNativeWindow unresponsive;
    unresponsive.id = std::uint64_t(form.winId());
    unresponsive.processId = GetCurrentProcessId();
    other.show();
    QTest::qWait(30);
    activate.x = otherBounds.left + 25;
    activate.y = otherBounds.top + 25;
    const MaiError focusBeforePause = maiDesktopInput(activate);
    QVERIFY2(!focusBeforePause, focusBeforePause.message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(other.winId()));
    MaiError activationTimeout;
    std::atomic<bool> activationStarted{false};
    std::chrono::milliseconds activationElapsed{0};
    std::thread activationWorker([&] {
        activationStarted = true;
        const auto start = std::chrono::steady_clock::now();
        activationTimeout = maiDesktopActivateWindow(unresponsive);
        activationElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    });
    while (!activationStarted.load()) std::this_thread::yield();
    // Only this fixture's GUI queue pauses. The real user process is untouched;
    // the worker must fail within its bounded foreground wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    activationWorker.join();
    QVERIFY(activationTimeout.hasError());
    QVERIFY(activationElapsed.count() < 400);
    QCOMPARE(input.text().toUtf8().toStdString(), expected + suffix);
    trace.push_back({{"stage", "unresponsive_owned_target_has_bounded_failure"},
                     {"elapsedMs", activationElapsed.count()},
                     {"noInputDispatched", true}});
    QString evidence = qEnvironmentVariable("MAICHAT_CHAIN_EVIDENCE");
    if (!evidence.isEmpty()) evidence += QStringLiteral("-") + QString::fromLatin1(QTest::currentDataTag());
    if (!evidence.isEmpty()) {
        QVERIFY(image.save(evidence + QStringLiteral(".png")));
        QFile file(evidence + QStringLiteral(".json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const std::string data = trace.dump(2);
        QCOMPARE(file.write(data.data(), qint64(data.size())), qint64(data.size()));
    }
    captureControl.requestStop();
    QTRY_VERIFY_WITH_TIMEOUT(!captureControl.isStopping(), 10000);
    QVERIFY(!captureControl.captureState().capturing && !captureControl.captureState().workerRunning);
    QVERIFY(!canceled.load());
}

void DesktopComputerScreenshotChainNativeTest::screenshotPreservesHeldButtonAndAtomicDragReleases() {
    if (!qEnvironmentVariableIsSet("MAICHAT_RUN_SCREENSHOT_COMPUTER_CHAIN"))
        QSKIP("Explicit owned screenshot/computer chain opt-in required");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        if (GetAsyncKeyState(key) & 0x8000) QSKIP("Physical mouse input is held");
    HeldSlider slider;
    slider.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    slider.showFullScreen();
    QVERIFY(QTest::qWaitForWindowExposed(&slider));
    const HWND handle = reinterpret_cast<HWND>(slider.winId());
    MouseWitness events;
    events.target = handle;
    qApp->installNativeEventFilter(&events);
    const auto removeEvents = qScopeGuard([&] { qApp->removeNativeEventFilter(&events); });
    const auto monitor =
        std::uint64_t(reinterpret_cast<std::uintptr_t>(MonitorFromWindow(handle, MONITOR_DEFAULTTONULL)));
    const auto displays = maiDesktopDisplays();
    QVERIFY(displays);
    DesktopNativeDisplay display;
    for (const auto& item : displays.value())
        if (item.id == monitor) display = item;
    QVERIFY(display.id && ownsDisplay(display, handle));
    POINT clientOrigin{0, 0};
    QVERIFY(ClientToScreen(handle, &clientOrigin));
    const auto nativePoint = [&](QPoint point) {
        return QPoint(clientOrigin.x + qRound(point.x() * slider.devicePixelRatioF()),
                      clientOrigin.y + qRound(point.y() * slider.devicePixelRatioF()));
    };
    DesktopNativeInput activate;
    activate.desktopScope = true;
    activate.display = display;
    activate.kind = DesktopNativeInputKind::Click;
    const QPoint blank = nativePoint(QPoint(60, 60));
    activate.x = blank.x();
    activate.y = blank.y();
    const auto activated = maiDesktopInput(activate);
    QVERIFY2(!activated, activated.message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), handle);
    QTRY_VERIFY(!slider.held && slider.releases > 0);
    slider.presses = slider.releases = slider.heldMoves = 0;

    std::atomic<bool> canceled{false};
    QTimer guard;
    connect(&guard, &QTimer::timeout, this, [&] {
        if (!ownsDisplay(display, handle) || GetForegroundWindow() != handle) canceled = true;
    });
    guard.start(20);
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    MaiToolContext context;
    context.sessionId = "owned-held-slider";
    context.root = workspace.path().toStdString();
    context.model = "glm-5.3-flash";
    context.cancel = &canceled;
    MaiToolRegistry registry;
    registerDesktopComputerTools(registry, std::make_shared<DesktopComputerController>());
    registry.add(makeMaiScreenshotTool());
    auto* computer = registry.find("desktop_computer");
    auto* screenshot = registry.find("screenshot");
    const auto stop = qScopeGuard([&] { maiStopScreenshotCapture(); });
    // This fixture alone supplies the down/up pair. It is not a model action or
    // a proposed way to bypass the production "user already holds a button" guard.
    bool fixtureHeld = false;
    const auto releaseFixtureButton = [&] {
        if (!fixtureHeld) return;
        INPUT up{};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(up));
        fixtureHeld = false;
    };
    const auto release = qScopeGuard(releaseFixtureButton);
    const QPoint start = nativePoint(QPoint(slider.handleX, HeldSlider::trackY));
    QVERIFY(SetCursorPos(start.x(), start.y()));
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    QCOMPARE(SendInput(1, &down, sizeof(down)), UINT(1));
    fixtureHeld = true;
    QTRY_VERIFY(slider.held && slider.presses == 1);
    Json trace = Json::array();
    for (const bool bound : {false, true}) {
        const Json args =
            bound ? Json{{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}}
                  : Json{{"action", "capture"}};
        const auto captured = invoke(bound ? computer : screenshot, args, context);
        QVERIFY2(!captured.hasError(), captured.error().message().c_str());
        QVERIFY(captured.images().size() == 1);
        QVERIFY(slider.held && slider.releases == 0);
        QVERIFY(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
        trace.push_back({{"stage", bound ? "bound_observe_while_held" : "screenshot_while_held"},
                         {"buttonStillDown", true},
                         {"receiverReleases", slider.releases}});
        const QString evidence = qEnvironmentVariable("MAICHAT_CHAIN_EVIDENCE");
        if (!evidence.isEmpty())
            QVERIFY(QImage(QString::fromStdString(captured.images().at(0).path))
                        .save(evidence +
                              (bound ? QStringLiteral("-held-observe.png") : QStringLiteral("-held-screenshot.png"))));
    }
    releaseFixtureButton();
    QTRY_VERIFY(!slider.held && slider.releases == 1);

    // Generic independent button input must compose with observation and move;
    // this is an input capability check, not an application-specific recipe.
    const auto independentObservation = invoke(
        computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}}, context);
    QVERIFY2(!independentObservation.hasError(), independentObservation.error().message().c_str());
    const Json independentSnapshot = Json::parse(independentObservation.output());
    const QImage independentImage(QString::fromStdString(independentObservation.images().at(0).path));
    const QPointF independentStart = markerCenter(independentImage, QColor("#28af77").rgb());
    QVERIFY(independentStart.x() >= 0);
    const int beforeIndependentRelease = slider.releases;
    const auto independentCleanup = qScopeGuard([&] { invoke(computer, {{"action", "release_all"}}, context); });
    const auto independentDown = invoke(computer,
                                        {{"action", "mouse_down"},
                                         {"target", "display"},
                                         {"display_id", std::to_string(display.id)},
                                         {"snapshot_id", independentSnapshot.at("snapshot_id")},
                                         {"button", "left"},
                                         {"x_ratio", independentStart.x() / (independentImage.width() - 1)},
                                         {"y_ratio", independentStart.y() / (independentImage.height() - 1)}},
                                        context);
    QVERIFY2(!independentDown.hasError(), independentDown.error().message().c_str());
    QTRY_VERIFY(slider.held);
    const auto whileHeld = invoke(
        computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}}, context);
    QVERIFY2(!whileHeld.hasError(), whileHeld.error().message().c_str());
    QCOMPARE(slider.releases, beforeIndependentRelease);
    const auto independentMove =
        invoke(computer,
               {{"action", "move"},
                {"target", "display"},
                {"display_id", std::to_string(display.id)},
                {"snapshot_id", Json::parse(whileHeld.output()).at("snapshot_id")},
                {"x_ratio", (independentStart.x() + 30 * slider.devicePixelRatioF()) / (independentImage.width() - 1)},
                {"y_ratio", independentStart.y() / (independentImage.height() - 1)}},
               context);
    QVERIFY2(!independentMove.hasError(), independentMove.error().message().c_str());
    QVERIFY(slider.held && slider.releases == beforeIndependentRelease);
    const auto independentUp = invoke(
        computer,
        {{"action", "mouse_up"}, {"target", "display"}, {"display_id", std::to_string(display.id)}, {"button", "left"}},
        context);
    QVERIFY2(!independentUp.hasError(), independentUp.error().message().c_str());
    QTRY_VERIFY(!slider.held && slider.releases == beforeIndependentRelease + 1);
    trace.push_back({{"stage", "independent_down_observe_move_up"}, {"heldAcrossObservation", true}});

    // First stop short; the tool must not be mistaken for an alignment checker.
    // Then use a fresh image to reach the marker with one complete drag.
    for (const int offset : {-40, 0}) {
        const auto observed =
            invoke(computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}},
                   context);
        QVERIFY2(!observed.hasError(), observed.error().message().c_str());
        const QImage image(QString::fromStdString(observed.images().at(0).path));
        QVERIFY(!image.isNull());
        const QPointF point = markerCenter(image, QColor("#28af77").rgb());
        const QPointF target = markerCenter(image, QColor("#df8547").rgb());
        QVERIFY(point.x() >= 0 && target.x() >= 0);
        const int releasesBefore = slider.releases;
        const auto dragged =
            invoke(computer,
                   {{"action", "drag"},
                    {"target", "display"},
                    {"display_id", std::to_string(display.id)},
                    {"snapshot_id", Json::parse(observed.output()).at("snapshot_id")},
                    {"x_ratio", point.x() / (image.width() - 1)},
                    {"y_ratio", point.y() / (image.height() - 1)},
                    {"to_x_ratio", (target.x() + offset * slider.devicePixelRatioF()) / (image.width() - 1)},
                    {"to_y_ratio", target.y() / (image.height() - 1)},
                    {"duration_ms", 200}},
                   context);
        QVERIFY2(!dragged.hasError(), dragged.error().message().c_str());
        QTRY_COMPARE(slider.releases, releasesBefore + 1);
        QVERIFY(!slider.held && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
        QVERIFY(slider.heldMoves > 0);
        QVERIFY(std::abs(slider.handleX - HeldSlider::targetX - offset) <= 1);
        const Json result = Json::parse(dragged.output());
        QVERIFY(result.value("dispatched", false) && !result.value("verified", true));
        QVERIFY(result.contains("observation") && dragged.images().size() == 1);
        const bool aligned = std::abs(slider.handleX - HeldSlider::targetX) <= 1;
        QCOMPARE(aligned, offset == 0);
        trace.push_back({{"stage", offset ? "short_drag_then_returned_image" : "aligned_drag_then_returned_image"},
                         {"buttonAlreadyUp", true},
                         {"aligned", aligned},
                         {"dispatched", true},
                         {"verified", false},
                         {"receiverReleases", slider.releases}});
        const QString evidence = qEnvironmentVariable("MAICHAT_CHAIN_EVIDENCE");
        if (!evidence.isEmpty()) {
            const QString base = evidence + QStringLiteral("-slider-%1").arg(offset);
            QVERIFY(QImage(QString::fromStdString(dragged.images().at(0).path)).save(base + QStringLiteral(".png")));
        }
    }
    const auto observeOwned = [&] {
        return invoke(computer,
                      {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}},
                      context);
    };
    const auto mutate = [&](Json args) {
        const auto image = observeOwned();
        if (image.hasError()) return image;
        args["target"] = "display";
        args["display_id"] = std::to_string(display.id);
        args["snapshot_id"] = Json::parse(image.output()).at("snapshot_id");
        return invoke(computer, args, context);
    };
    const QPoint safePoint = nativePoint(QPoint(70, 70));
    const double safeX = (safePoint.x() - display.x) / (display.width - 1);
    const double safeY = (safePoint.y() - display.y) / (display.height - 1);
    const std::pair<const char*, int> buttons[] = {{"left", VK_LBUTTON},
                                                   {"right", VK_RBUTTON},
                                                   {"middle", VK_MBUTTON},
                                                   {"back", VK_XBUTTON1},
                                                   {"forward", VK_XBUTTON2}};
    for (const auto& button : buttons) {
        const auto down =
            mutate({{"action", "mouse_down"}, {"button", button.first}, {"x_ratio", safeX}, {"y_ratio", safeY}});
        QVERIFY2(!down.hasError(), down.error().message().c_str());
        QTRY_VERIFY(GetAsyncKeyState(button.second) & 0x8000);
        const auto state = invoke(computer, {{"action", "input_state"}}, context);
        QVERIFY(!state.hasError());
        QVERIFY(Json::parse(state.output()).at("buttons_down") == Json::array({button.first}));
        POINT beforeDuplicate{};
        QVERIFY(GetCursorPos(&beforeDuplicate));
        const auto duplicate =
            mutate({{"action", "mouse_down"}, {"button", button.first}, {"x_ratio", safeX + 0.1}, {"y_ratio", safeY}});
        QVERIFY2(!duplicate.hasError(), duplicate.error().message().c_str());
        POINT afterDuplicate{};
        QVERIFY(GetCursorPos(&afterDuplicate));
        QCOMPARE(QPoint(beforeDuplicate.x, beforeDuplicate.y), QPoint(afterDuplicate.x, afterDuplicate.y));
        const auto up = invoke(computer, {{"action", "mouse_up"}, {"button", button.first}}, context);
        QVERIFY2(!up.hasError(), up.error().message().c_str());
        QTRY_VERIFY(!(GetAsyncKeyState(button.second) & 0x8000));
        const auto tapped =
            mutate({{"action", "click"}, {"button", button.first}, {"x_ratio", safeX}, {"y_ratio", safeY}});
        QVERIFY2(!tapped.hasError(), tapped.error().message().c_str());
        QTRY_VERIFY(!(GetAsyncKeyState(button.second) & 0x8000));
        const auto releasesBeforeDouble = events.buttonUps.size();
        const auto doubled =
            mutate({{"action", "double_click"}, {"button", button.first}, {"x_ratio", safeX}, {"y_ratio", safeY}});
        QVERIFY2(!doubled.hasError(), doubled.error().message().c_str());
        QTRY_VERIFY(events.buttonUps.size() >= releasesBeforeDouble + 2);
    }
    QVERIFY(std::find(events.buttonDowns.begin(), events.buttonDowns.end(), WM_MBUTTONDOWN) !=
            events.buttonDowns.end());
    QVERIFY(std::find(events.buttonDowns.begin(), events.buttonDowns.end(), WM_XBUTTONDOWN) !=
            events.buttonDowns.end());
    trace.push_back({{"stage", "five_buttons_independent_and_click"}, {"duplicateDownDoesNotMove", true}});

    const auto controlDown = mutate({{"action", "key_down"}, {"key", "Ctrl"}});
    QVERIFY2(!controlDown.hasError(), controlDown.error().message().c_str());
    QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const int controlClicksBefore = events.controlClicks;
    const auto controlClick = mutate({{"action", "click"}, {"x_ratio", safeX}, {"y_ratio", safeY}});
    QVERIFY2(!controlClick.hasError(), controlClick.error().message().c_str());
    QTRY_VERIFY(events.controlClicks > controlClicksBefore);
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const auto tappedKey = mutate({{"action", "key"}, {"key", "A"}});
    QVERIFY2(!tappedKey.hasError(), tappedKey.error().message().c_str());
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const auto horizontal =
        mutate({{"action", "scroll"}, {"axis", "horizontal"}, {"lines", 1}, {"x_ratio", safeX}, {"y_ratio", safeY}});
    QVERIFY2(!horizontal.hasError(), horizontal.error().message().c_str());
    QTRY_VERIFY(events.horizontalWheel >= WHEEL_DELTA);
    const auto vertical = mutate({{"action", "scroll"}, {"lines", 1}, {"x_ratio", safeX}, {"y_ratio", safeY}});
    QVERIFY2(!vertical.hasError(), vertical.error().message().c_str());
    QTRY_VERIFY(events.verticalWheel >= WHEEL_DELTA);
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const auto controlUp = invoke(computer, {{"action", "key_up"}, {"key", "Control"}}, context);
    QVERIFY2(!controlUp.hasError(), controlUp.error().message().c_str());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
    trace.push_back({{"stage", "held_control_click_key_and_both_scroll_axes"}, {"modifierPreserved", true}});

    const auto shiftDown = mutate({{"action", "key_down"}, {"key", "Shift"}});
    QVERIFY2(!shiftDown.hasError(), shiftDown.error().message().c_str());
    const int shiftClicksBefore = events.shiftClicks;
    const auto shiftDrag = mutate({{"action", "drag"},
                                   {"x_ratio", safeX},
                                   {"y_ratio", safeY},
                                   {"to_x_ratio", safeX + 0.05},
                                   {"to_y_ratio", safeY},
                                   {"duration_ms", 100}});
    QVERIFY2(!shiftDrag.hasError(), shiftDrag.error().message().c_str());
    QTRY_VERIFY(events.shiftClicks > shiftClicksBefore);
    QVERIFY(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    QVERIFY(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
    canceled = true;
    const auto canceledRelease = invoke(computer, {{"action", "release_all"}}, context);
    QVERIFY2(!canceledRelease.hasError(), canceledRelease.error().message().c_str());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_SHIFT) & 0x8000));
    canceled = false;
    trace.push_back({{"stage", "shift_drag_and_cancelled_release_all"}, {"onlyTransientMouseReleased", true}});
    for (const char* key : {"ShiftLeft", "ShiftRight"}) {
        const auto down = mutate({{"action", "key_down"}, {"key", key}});
        QVERIFY2(!down.hasError(), down.error().message().c_str());
    }
    QVERIFY(!invoke(computer, {{"action", "key_up"}, {"key", "ShiftLeft"}}, context).hasError());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_LSHIFT) & 0x8000));
    QVERIFY(GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    QVERIFY(!invoke(computer, {{"action", "key_up"}, {"key", "ShiftRight"}}, context).hasError());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_SHIFT) & 0x8000));
    trace.push_back({{"stage", "independent_modifier_sides"}, {"otherSidePreserved", true}});

    // The native session is also an ordinary API. A retained per-call callback
    // would keep this token alive after the caller drops its copy.
    auto ordinary = maiDesktopCreateInputSession();
    DesktopNativeInput ordinaryDown;
    ordinaryDown.desktopScope = true;
    ordinaryDown.display = display;
    ordinaryDown.kind = DesktopNativeInputKind::KeyDown;
    ordinaryDown.key = "Ctrl";
    auto callbackToken = std::make_shared<int>(1);
    std::weak_ptr<int> weakToken = callbackToken;
    ordinaryDown.isCanceled = [callbackToken] { return false; };
    const auto ordinaryResult = ordinary->input(ordinaryDown);
    QVERIFY2(!ordinaryResult, ordinaryResult.message().c_str());
    ordinaryDown.isCanceled = {};
    callbackToken.reset();
    QVERIFY(weakToken.expired());
    QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    auto outsider = maiDesktopCreateInputSession();
    QVERIFY(outsider->input(ordinaryDown));
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    DesktopNativeInput unownedUp;
    unownedUp.kind = DesktopNativeInputKind::KeyUp;
    unownedUp.key = "Ctrl";
    QVERIFY(!outsider->input(unownedUp));
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    ordinary.reset();
    QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
    trace.push_back({{"stage", "ordinary_api_callback_lifetime_external_hold_and_destructor"},
                     {"callbackReleased", true},
                     {"externalHoldPreserved", true},
                     {"ownedHoldReleased", true}});
    ordinary = maiDesktopCreateInputSession();
    QVERIFY(!ordinary->input(ordinaryDown));
    DesktopNativeInput invalidMove = ordinaryDown;
    invalidMove.kind = DesktopNativeInputKind::Move;
    invalidMove.x = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(ordinary->input(invalidMove));
    QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
    QVERIFY(ordinary->state().empty());
    QVERIFY(!ordinary->input(ordinaryDown));
    DesktopNativeInput canceledMove = ordinaryDown;
    canceledMove.kind = DesktopNativeInputKind::Move;
    canceledMove.x = safePoint.x();
    canceledMove.y = safePoint.y();
    canceledMove.isCanceled = [] { return true; };
    QVERIFY(ordinary->input(canceledMove));
    QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
    QVERIFY(ordinary->state().empty());
    trace.push_back({{"stage", "native_error_and_cancel_cleanup"}, {"ownedHoldsReleased", true}});
    const QString evidence = qEnvironmentVariable("MAICHAT_CHAIN_EVIDENCE");
    if (!evidence.isEmpty()) {
        QFile file(evidence + QStringLiteral("-held-slider.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const std::string bytes = trace.dump(2);
        QCOMPARE(file.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
    }
    QVERIFY(!canceled.load());
}

QTEST_MAIN(DesktopComputerScreenshotChainNativeTest)
#include "DesktopComputerScreenshotChainNativeTest.moc"
