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
#include <iterator>
#include <optional>
#include <sstream>
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

template <class Work> auto nativeWork(Work work) {
    using Result = decltype(work());
    std::optional<Result> result;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        result.emplace(work());
        finished = true;
    });
    while (!finished.load()) QTest::qWait(5);
    worker.join();
    return std::move(*result);
}

MaiToolResult invoke(MaiTool* tool, const Json& arguments, const MaiToolContext& context) {
    return nativeWork([&] { return tool->execute(arguments.dump(), context); });
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
    const auto controller = std::make_shared<DesktopComputerController>();
    registerDesktopComputerTools(registry, controller);
    registry.add(makeMaiScreenshotTool());
    MaiTool* computer = registry.find("computer");
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

    const auto selected = invoke(computer, {{"method", "get_window"},
        {"parameters", {{"id", std::uint64_t(form.winId())}}}}, context);
    QVERIFY2(!selected.hasError(), selected.error().message().c_str());
    const Json selectedWindow = Json::parse(selected.output());
    const auto nativeWindows = controller->windows();
    QVERIFY(nativeWindows);
    const auto native = std::find_if(nativeWindows.value().begin(), nativeWindows.value().end(),
        [&](const DesktopNativeWindow& window) { return window.id == std::uint64_t(form.winId()); });
    QVERIFY(native != nativeWindows.value().end());
    const DesktopNativeWindow target = *native;
    const double scale = target.coordinateScale;
    QVERIFY(scale > 0);
    const auto observeWindow = [&] {
        return invoke(computer, {{"method", "get_window_state"},
            {"parameters", {{"window", selectedWindow}, {"include_text", true}}}}, context);
    };
    auto observed = observeWindow();
    QVERIFY2(!observed.hasError(), observed.error().message().c_str());
    Json window = Json::parse(observed.output());
    QCOMPARE(target.height, display.height - 72);
    QVERIFY(observed.images().size() == 1);
    QCOMPARE(window.at("screenshots").at(0).at("image_attachment_index").get<int>(), 0);
    QVERIFY(!window.at("screenshots").at(0).contains("url"));
    const auto click = [&](const Json& snapshot, bool screenRatios) {
        const double x = screenRatios ? point.x() / (image.width() - 1) * (target.width - 1) / scale
                                      : (display.x + point.x() - target.x) / scale;
        const double y = screenRatios ? point.y() / (image.height() - 1) * (target.height - 1) / scale
                                      : (display.y + point.y() - target.y) / scale;
        return invoke(computer, {{"method", "click"}, {"parameters", {
            {"window", snapshot.at("window")}, {"screenshotId", snapshot.at("screenshots").at(0).at("id")},
            {"x", x}, {"y", y}}}}, context);
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
                     {"windowHeight", target.height}});

    const Json staleInput{{"method", "type_text"}, {"parameters", {
        {"window", selectedWindow}, {"text", "must not type"}}}};
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
    const auto typed = invoke(computer, {{"method", "type_text"},
        {"parameters", {{"window", selectedWindow}, {"text", expected}}}}, context);
    QVERIFY2(!typed.hasError(), typed.error().message().c_str());
    QTest::qWait(50);
    qInfo() << "[unicode-order] nativeCharactersMatch" << (mouse.characters.toUtf8().toStdString() == expected)
            << "renderedTextMatches" << (input.text().toStdString() == expected);
    QTRY_COMPARE(input.text().toStdString(), expected);
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    const auto verified = nativeWork([&] { return maiDesktopFocusedText(target); });
    QVERIFY2(verified, verified.error().message().c_str());
    QCOMPARE(verified.value(), expected);
    trace.push_back({{"stage", "fresh_window_mapping_click_type_uia_readback"}, {"verified", true}});
    observed = observeWindow();
    QVERIFY(!observed.hasError());
    window = Json::parse(observed.output());
    std::optional<std::size_t> inputIndex;
    std::istringstream tree(window.at("accessibility").at("tree").get<std::string>());
    for (std::string line; std::getline(tree, line);) {
        if (line.find("Owned screenshot coordinate input") == std::string::npos) continue;
        std::size_t index = 0;
        if (std::istringstream(line) >> index) inputIndex = index;
    }
    QVERIFY(inputIndex.has_value());
    const auto replaced = invoke(computer, {{"method", "set_value"}, {"parameters", {
        {"window", selectedWindow}, {"element_index", *inputIndex}, {"value", expected}}}}, context);
    QVERIFY2(!replaced.hasError(), replaced.error().message().c_str());
    QTRY_COMPARE(input.text().toStdString(), expected);
    trace.push_back({{"stage", "native_value_replacement_keeps_complete_unicode_order"}, {"verified", true}});

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
    const auto focused = invoke(computer, {{"method", "activate_window"},
        {"parameters", {{"window", selectedWindow}}}}, context);
    qInfo() << "[focus-native-immediate] returnedError" << focused.hasError() << "foregroundMatches"
            << (GetForegroundWindow() == handle);
    if (focused.hasError()) {
        QTest::qWait(50);
        qInfo() << "[focus-native-after-owner-dispatch] foregroundMatches" << (GetForegroundWindow() == handle);
    }
    QVERIFY2(!focused.hasError(), focused.error().message().c_str());
    QTRY_COMPARE(GetForegroundWindow(), handle);
    // Explicit activation has no token parameter. Exercise the existing
    // ordinary focus operation separately to retain its single-use contract.
    const auto focusBinding = nativeWork([&] { return controller->observe(context.sessionId, {}, context.root,
        false, target.id, false, [&] { return canceled.load(); }); });
    QVERIFY2(focusBinding, focusBinding.error().message().c_str());
    DesktopComputerAction focusAction;
    focusAction.action = "focus";
    focusAction.windowId = target.id;
    focusAction.snapshotId = Json::parse(focusBinding.value().json).at("snapshot_id").get<std::string>();
    focusAction.isCanceled = [&] { return canceled.load(); };
    const auto consumedFocus = nativeWork([&] { return controller->perform(context.sessionId, focusAction); });
    QVERIFY2(consumedFocus, consumedFocus.error().message().c_str());
    QVERIFY(!nativeWork([&] { return controller->perform(context.sessionId, focusAction); }));
    QVERIFY(invoke(computer, {{"method", "type_text"}, {"parameters", {
        {"window", selectedWindow}, {"text", "must not type after focus without observing"}}}}, context).hasError());
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
    const auto staleGeometry = invoke(computer, {{"method", "type_text"}, {"parameters", {
        {"window", selectedWindow}, {"text", "must not type using old geometry"}}}}, context);
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
    const auto observeDisplay = [&] {
        return nativeWork([&] { return controller->observeDisplay(context.sessionId, context.root, display.id,
            [&] { return canceled.load(); }); });
    };
    const auto performDisplay = [&](DesktopComputerAction action) {
        action.desktopScope = true;
        action.workspace = context.root;
        action.displayId = display.id;
        action.isCanceled = [&] { return canceled.load(); };
        return nativeWork([&] { return controller->perform(context.sessionId, action); });
    };
    const auto displayObserved = observeDisplay();
    QVERIFY2(displayObserved, displayObserved.error().message().c_str());
    Json displaySnapshot = Json::parse(displayObserved.value().json);
    // Select from the very image bound to this token, not the earlier standalone
    // screenshot. The following click -> returned observation -> type is the
    // ordinary API sequence; the standalone screenshot does not grant input.
    const QImage boundImage(QString::fromStdString(displayObserved.value().imagePath));
    QVERIFY(!boundImage.isNull());
    const QPointF boundPoint = markerCenter(boundImage);
    QVERIFY(boundPoint.x() >= 0);
    DesktopComputerAction displayClickAction;
    displayClickAction.action = "click";
    displayClickAction.snapshotId = displaySnapshot.at("snapshot_id").get<std::string>();
    displayClickAction.xRatio = boundPoint.x() / (boundImage.width() - 1);
    displayClickAction.yRatio = boundPoint.y() / (boundImage.height() - 1);
    const auto displayClick = performDisplay(displayClickAction);
    QVERIFY2(displayClick, displayClick.error().message().c_str());
    const Json displayResult = Json::parse(displayClick.value().observation.json);
    QVERIFY(displayClick.value().dispatched);
    QVERIFY(displayResult.at("frame_sequence").get<std::uint64_t>() >
            displaySnapshot.at("frame_sequence").get<std::uint64_t>());
    QTRY_VERIFY(input.hasFocus());
    QVERIFY(!displayClick.value().observation.imagePath.empty());
    QVERIFY(displayResult.at("snapshot_id") != displaySnapshot.at("snapshot_id"));
    const auto displayType = [&](const Json& token, const std::string& text) {
        DesktopComputerAction action;
        action.action = "type";
        action.snapshotId = token.at("snapshot_id").get<std::string>();
        action.text = text;
        return performDisplay(action);
    };
    QVERIFY(!displayType(displaySnapshot, "must not type with the consumed click token"));
    const std::string suffix = " [fresh display token]";
    const auto displayTyped = displayType(displayResult, expected + suffix);
    QVERIFY2(displayTyped, displayTyped.error().message().c_str());
    QTRY_COMPARE(input.text().toUtf8().toStdString(), expected + suffix);
    QVERIFY(!displayTyped.value().observation.imagePath.empty());
    const Json typedDisplay = Json::parse(displayTyped.value().observation.json);
    QVERIFY(typedDisplay.at("snapshot_id") != displayResult.at("snapshot_id"));
    if (QString::fromLatin1(QTest::currentDataTag()) == QStringLiteral("ascii")) {
        for (const char* action : {"type", "replace_text"}) {
            const auto current = observeDisplay();
            QVERIFY2(current, current.error().message().c_str());
            DesktopComputerAction down;
            down.action = "key_down";
            down.snapshotId = Json::parse(current.value().json).at("snapshot_id").get<std::string>();
            down.key = "Ctrl";
            const auto held = performDisplay(down);
            QVERIFY2(held, held.error().message().c_str());
            QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
            const QString textBefore = input.text();
            DesktopComputerAction literalAction;
            literalAction.action = action;
            literalAction.snapshotId = Json::parse(held.value().observation.json).at("snapshot_id").get<std::string>();
            literalAction.text = "literal text must not become a shortcut";
            const auto literal = performDisplay(literalAction);
            qInfo() << "[literal-held-modifier]" << action << "error" << !literal << "textUnchanged"
                    << (input.text() == textBefore);
            QVERIFY2(!literal, "Literal text insertion must reject a held shortcut modifier");
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
    const auto controller = std::make_shared<DesktopComputerController>();
    registry.add(makeMaiScreenshotTool());
    auto* screenshot = registry.find("screenshot");
    const auto observeOwned = [&] {
        return nativeWork([&] { return controller->observeDisplay(context.sessionId, context.root, display.id,
            [&] { return canceled.load(); }); });
    };
    const auto performOwned = [&](DesktopComputerAction action) {
        action.desktopScope = true;
        action.workspace = context.root;
        action.displayId = display.id;
        action.isCanceled = [&] { return canceled.load(); };
        return nativeWork([&] { return controller->perform(context.sessionId, action); });
    };
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
        std::string imagePath;
        if (bound) {
            const auto captured = observeOwned();
            QVERIFY2(captured, captured.error().message().c_str());
            imagePath = captured.value().imagePath;
        } else {
            const auto captured = invoke(screenshot, {{"action", "capture"}}, context);
            QVERIFY2(!captured.hasError(), captured.error().message().c_str());
            QVERIFY(captured.images().size() == 1);
            imagePath = captured.images().front().path;
        }
        QVERIFY(!imagePath.empty());
        QVERIFY(slider.held && slider.releases == 0);
        QVERIFY(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
        trace.push_back({{"stage", bound ? "bound_observe_while_held" : "screenshot_while_held"},
                         {"buttonStillDown", true},
                         {"receiverReleases", slider.releases}});
        const QString evidence = qEnvironmentVariable("MAICHAT_CHAIN_EVIDENCE");
        if (!evidence.isEmpty())
            QVERIFY(QImage(QString::fromStdString(imagePath))
                        .save(evidence +
                              (bound ? QStringLiteral("-held-observe.png") : QStringLiteral("-held-screenshot.png"))));
    }
    releaseFixtureButton();
    QTRY_VERIFY(!slider.held && slider.releases == 1);

    // Generic independent button input must compose with observation and move;
    // this is an input capability check, not an application-specific recipe.
    const auto independentObservation = observeOwned();
    QVERIFY2(independentObservation, independentObservation.error().message().c_str());
    const Json independentSnapshot = Json::parse(independentObservation.value().json);
    const QImage independentImage(QString::fromStdString(independentObservation.value().imagePath));
    const QPointF independentStart = markerCenter(independentImage, QColor("#28af77").rgb());
    QVERIFY(independentStart.x() >= 0);
    const int beforeIndependentRelease = slider.releases;
    const auto independentCleanup = qScopeGuard([&] { controller->releaseInput(context.sessionId); });
    DesktopComputerAction independentDownAction;
    independentDownAction.action = "mouse_down";
    independentDownAction.snapshotId = independentSnapshot.at("snapshot_id").get<std::string>();
    independentDownAction.xRatio = independentStart.x() / (independentImage.width() - 1);
    independentDownAction.yRatio = independentStart.y() / (independentImage.height() - 1);
    const auto independentDown = performOwned(independentDownAction);
    QVERIFY2(independentDown, independentDown.error().message().c_str());
    QTRY_VERIFY(slider.held);
    const auto whileHeld = observeOwned();
    QVERIFY2(whileHeld, whileHeld.error().message().c_str());
    QCOMPARE(slider.releases, beforeIndependentRelease);
    DesktopComputerAction independentMoveAction;
    independentMoveAction.action = "move";
    independentMoveAction.snapshotId = Json::parse(whileHeld.value().json).at("snapshot_id").get<std::string>();
    independentMoveAction.xRatio = (independentStart.x() + 30 * slider.devicePixelRatioF()) /
                                   (independentImage.width() - 1);
    independentMoveAction.yRatio = independentStart.y() / (independentImage.height() - 1);
    const auto independentMove = performOwned(independentMoveAction);
    QVERIFY2(independentMove, independentMove.error().message().c_str());
    QVERIFY(slider.held && slider.releases == beforeIndependentRelease);
    DesktopComputerAction independentUpAction;
    independentUpAction.action = "mouse_up";
    const auto independentUp = performOwned(independentUpAction);
    QVERIFY2(independentUp, independentUp.error().message().c_str());
    QTRY_VERIFY(!slider.held && slider.releases == beforeIndependentRelease + 1);
    trace.push_back({{"stage", "independent_down_observe_move_up"}, {"heldAcrossObservation", true}});

    // First stop short; the tool must not be mistaken for an alignment checker.
    // Then use a fresh image to reach the marker with one complete drag.
    for (const int offset : {-40, 0}) {
        const auto observed = observeOwned();
        QVERIFY2(observed, observed.error().message().c_str());
        const QImage image(QString::fromStdString(observed.value().imagePath));
        QVERIFY(!image.isNull());
        const QPointF point = markerCenter(image, QColor("#28af77").rgb());
        const QPointF target = markerCenter(image, QColor("#df8547").rgb());
        QVERIFY(point.x() >= 0 && target.x() >= 0);
        const int releasesBefore = slider.releases;
        DesktopComputerAction drag;
        drag.action = "drag";
        drag.snapshotId = Json::parse(observed.value().json).at("snapshot_id").get<std::string>();
        drag.xRatio = point.x() / (image.width() - 1);
        drag.yRatio = point.y() / (image.height() - 1);
        drag.toXRatio = (target.x() + offset * slider.devicePixelRatioF()) / (image.width() - 1);
        drag.toYRatio = target.y() / (image.height() - 1);
        drag.durationMs = 200;
        const auto dragged = performOwned(drag);
        QVERIFY2(dragged, dragged.error().message().c_str());
        QTRY_COMPARE(slider.releases, releasesBefore + 1);
        QVERIFY(!slider.held && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
        QVERIFY(slider.heldMoves > 0);
        QVERIFY(std::abs(slider.handleX - HeldSlider::targetX - offset) <= 1);
        QVERIFY(dragged.value().dispatched && !dragged.value().verified);
        QVERIFY(!dragged.value().observation.imagePath.empty());
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
            QVERIFY(QImage(QString::fromStdString(dragged.value().observation.imagePath)).save(base + QStringLiteral(".png")));
        }
    }
    const auto mutate = [&](DesktopComputerAction action) -> MaiResult<DesktopComputerActionResult> {
        const auto image = observeOwned();
        if (!image) return image.error();
        action.snapshotId = Json::parse(image.value().json).at("snapshot_id").get<std::string>();
        return performOwned(action);
    };
    const QPoint safePoint = nativePoint(QPoint(70, 70));
    const double safeX = (safePoint.x() - display.x) / (display.width - 1);
    const double safeY = (safePoint.y() - display.y) / (display.height - 1);
    const auto pointRequest = [](const char* kind, double x, double y,
                                 DesktopMouseButton button = DesktopMouseButton::Left) {
        DesktopComputerAction action;
        action.action = kind;
        action.xRatio = x;
        action.yRatio = y;
        action.button = button;
        return action;
    };
    const auto keyRequest = [](const char* kind, const char* key) {
        DesktopComputerAction action;
        action.action = kind;
        action.key = key;
        return action;
    };
    const std::pair<const char*, int> buttons[] = {{"left", VK_LBUTTON},
                                                   {"right", VK_RBUTTON},
                                                   {"middle", VK_MBUTTON},
                                                   {"back", VK_XBUTTON1},
                                                   {"forward", VK_XBUTTON2}};
    const DesktopMouseButton nativeButtons[] = {DesktopMouseButton::Left, DesktopMouseButton::Right,
        DesktopMouseButton::Middle, DesktopMouseButton::Back, DesktopMouseButton::Forward};
    for (std::size_t index = 0; index < std::size(buttons); ++index) {
        const auto& button = buttons[index];
        const auto down = mutate(pointRequest("mouse_down", safeX, safeY, nativeButtons[index]));
        QVERIFY2(down, down.error().message().c_str());
        QTRY_VERIFY(GetAsyncKeyState(button.second) & 0x8000);
        const auto state = controller->inputState(context.sessionId);
        QVERIFY(state.buttonsDown == std::vector<std::string>{button.first});
        POINT beforeDuplicate{};
        QVERIFY(GetCursorPos(&beforeDuplicate));
        const auto duplicate = mutate(pointRequest("mouse_down", safeX + 0.1, safeY, nativeButtons[index]));
        QVERIFY2(duplicate, duplicate.error().message().c_str());
        POINT afterDuplicate{};
        QVERIFY(GetCursorPos(&afterDuplicate));
        QCOMPARE(QPoint(beforeDuplicate.x, beforeDuplicate.y), QPoint(afterDuplicate.x, afterDuplicate.y));
        const auto up = performOwned(pointRequest("mouse_up", 0, 0, nativeButtons[index]));
        QVERIFY2(up, up.error().message().c_str());
        QTRY_VERIFY(!(GetAsyncKeyState(button.second) & 0x8000));
        const auto tapped = mutate(pointRequest("click", safeX, safeY, nativeButtons[index]));
        QVERIFY2(tapped, tapped.error().message().c_str());
        QTRY_VERIFY(!(GetAsyncKeyState(button.second) & 0x8000));
        const auto releasesBeforeDouble = events.buttonUps.size();
        const auto doubled = mutate(pointRequest("double_click", safeX, safeY, nativeButtons[index]));
        QVERIFY2(doubled, doubled.error().message().c_str());
        QTRY_VERIFY(events.buttonUps.size() >= releasesBeforeDouble + 2);
    }
    QVERIFY(std::find(events.buttonDowns.begin(), events.buttonDowns.end(), WM_MBUTTONDOWN) !=
            events.buttonDowns.end());
    QVERIFY(std::find(events.buttonDowns.begin(), events.buttonDowns.end(), WM_XBUTTONDOWN) !=
            events.buttonDowns.end());
    trace.push_back({{"stage", "five_buttons_independent_and_click"}, {"duplicateDownDoesNotMove", true}});

    const auto controlDown = mutate(keyRequest("key_down", "Ctrl"));
    QVERIFY2(controlDown, controlDown.error().message().c_str());
    QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const int controlClicksBefore = events.controlClicks;
    const auto controlClick = mutate(pointRequest("click", safeX, safeY));
    QVERIFY2(controlClick, controlClick.error().message().c_str());
    QTRY_VERIFY(events.controlClicks > controlClicksBefore);
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const auto tappedKey = mutate(keyRequest("key", "A"));
    QVERIFY2(tappedKey, tappedKey.error().message().c_str());
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    auto scroll = pointRequest("scroll", safeX, safeY);
    scroll.scrollAxis = DesktopScrollAxis::Horizontal;
    scroll.scrollLines = 1;
    const auto horizontal = mutate(scroll);
    QVERIFY2(horizontal, horizontal.error().message().c_str());
    QTRY_VERIFY(events.horizontalWheel >= WHEEL_DELTA);
    scroll.scrollAxis = DesktopScrollAxis::Vertical;
    const auto vertical = mutate(scroll);
    QVERIFY2(vertical, vertical.error().message().c_str());
    QTRY_VERIFY(events.verticalWheel >= WHEEL_DELTA);
    QVERIFY(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    const auto controlUp = performOwned(keyRequest("key_up", "Control"));
    QVERIFY2(controlUp, controlUp.error().message().c_str());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL) & 0x8000));
    trace.push_back({{"stage", "held_control_click_key_and_both_scroll_axes"}, {"modifierPreserved", true}});

    const auto shiftDown = mutate(keyRequest("key_down", "Shift"));
    QVERIFY2(shiftDown, shiftDown.error().message().c_str());
    const int shiftClicksBefore = events.shiftClicks;
    auto drag = pointRequest("drag", safeX, safeY);
    drag.toXRatio = safeX + 0.05;
    drag.toYRatio = safeY;
    drag.durationMs = 100;
    const auto shiftDrag = mutate(drag);
    QVERIFY2(shiftDrag, shiftDrag.error().message().c_str());
    QTRY_VERIFY(events.shiftClicks > shiftClicksBefore);
    QVERIFY(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    QVERIFY(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
    canceled = true;
    const auto canceledRelease = nativeWork([&] { return controller->releaseInput(context.sessionId); });
    QVERIFY2(!canceledRelease, canceledRelease.message().c_str());
    QTRY_VERIFY(!(GetAsyncKeyState(VK_SHIFT) & 0x8000));
    canceled = false;
    trace.push_back({{"stage", "shift_drag_and_cancelled_release_all"}, {"onlyTransientMouseReleased", true}});
    for (const char* key : {"ShiftLeft", "ShiftRight"}) {
        const auto down = mutate(keyRequest("key_down", key));
        QVERIFY2(down, down.error().message().c_str());
    }
    QVERIFY(performOwned(keyRequest("key_up", "ShiftLeft")));
    QTRY_VERIFY(!(GetAsyncKeyState(VK_LSHIFT) & 0x8000));
    QVERIFY(GetAsyncKeyState(VK_RSHIFT) & 0x8000);
    QVERIFY(performOwned(keyRequest("key_up", "ShiftRight")));
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
