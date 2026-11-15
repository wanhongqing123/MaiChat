// Run the real screenshot/capture-control/computer chain on an owned display.
// No visual model, account, browser profile, external site, or user content is used.
#include <QApplication>
#include <QCryptographicHash>
#include <QAbstractNativeEventFilter>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <optional>
#include <thread>
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
    bool nativeEventFilter(const QByteArray&, void* value, qintptr*) override {
        const auto* message = static_cast<MSG*>(value);
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

QPointF markerCenter(const QImage& image) {
    QRect marker;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if ((image.pixel(x, y) & 0xffffff) == 0xcc33ff) marker |= QRect(x, y, 1, 1);
    return marker.isEmpty() ? QPointF(-1, -1) : QRectF(marker).center();
}
}  // namespace

class DesktopComputerScreenshotChainNativeTest : public QObject {
    Q_OBJECT
private slots:
    void screenshotPixelsFocusAndFreshSnapshotsCompose_data();
    void screenshotPixelsFocusAndFreshSnapshotsCompose();
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

    const auto displayObserved = invoke(
        computer, {{"action", "observe"}, {"target", "display"}, {"display_id", std::to_string(display.id)}}, context);
    QVERIFY2(!displayObserved.hasError(), displayObserved.error().message().c_str());
    Json displaySnapshot = Json::parse(displayObserved.output());
    const auto displayClick = invoke(computer,
                                     {{"action", "click"},
                                      {"target", "display"},
                                      {"display_id", std::to_string(display.id)},
                                      {"snapshot_id", displaySnapshot.at("snapshot_id")},
                                      {"x_ratio", point.x() / (image.width() - 1)},
                                      {"y_ratio", point.y() / (image.height() - 1)}},
                                     context);
    QVERIFY2(!displayClick.hasError(), displayClick.error().message().c_str());
    const Json displayResult = Json::parse(displayClick.output());
    QVERIFY(displayResult.value("dispatched", false));
    QVERIFY(displayResult.at("observation").at("frame_sequence").get<std::uint64_t>() >
            displaySnapshot.at("frame_sequence").get<std::uint64_t>());
    QTRY_VERIFY(input.hasFocus());
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
    QCOMPARE(input.text().toStdString(), expected);
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

QTEST_MAIN(DesktopComputerScreenshotChainNativeTest)
#include "DesktopComputerScreenshotChainNativeTest.moc"
