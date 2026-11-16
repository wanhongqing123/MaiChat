// Opt-in SDK-free Windows receiver. All input targets this process's own
// foreground/hit-tested window. API acceptance and received WM evidence are
// separate checks; no later cursor sample substitutes for event coordinates.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QResizeEvent>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <thread>
#include <vector>
#include "MaiChatTools/DesktopComputerInputSession.h"
#include "MaiScreenshot.h"

namespace {
using Kind = DesktopInputEventKind;
bool pollingPumpControl = false;
std::uint64_t identifier(HWND window) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(window));
}
QJsonArray point(POINT value) {
    return {int(value.x), int(value.y)};
}
QJsonArray rect(RECT value) {
    return {int(value.left), int(value.top), int(value.right), int(value.bottom)};
}
void pumpFor(int milliseconds = 25) {
    if (!pollingPumpControl) {
        // Run the same continuous GUI dispatcher as the App. Sleeping between
        // processEvents calls can add one Windows timer tick to every UIA/COM
        // round trip and must not be measured as native input dispatch cost.
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
        return;
    }
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}
bool pumpUntil(const std::function<bool()>& ready, int milliseconds = 2000) {
    QElapsedTimer timer;
    timer.start();
    do {
        if (ready()) return true;
        pumpFor(2);
    } while (timer.elapsed() < milliseconds);
    return ready();
}
struct Event {
    UINT message = 0, key = 0;
    HWND window = nullptr;
    POINT client{}, origin{}, screen{};
    bool positionValid = false, extended = false, previousDown = false, control = false;
    DWORD positionError = 0, mouseFlags = 0;
    unsigned button = 0;
    int wheel = 0;
    std::uint64_t time = 0;
    double highResolutionMs = 0;
    DWORD queuedTime = 0;
    std::uint64_t wparam = 0;
    std::int64_t lparam = 0;
};
bool mouseMessage(UINT message) {
    return message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL ||
           message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MBUTTONDOWN ||
           message == WM_MBUTTONUP || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
           message == WM_XBUTTONDOWN || message == WM_XBUTTONUP;
}
bool down(UINT message) {
    return message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN ||
           message == WM_XBUTTONDOWN;
}
bool up(UINT message) {
    return message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP || message == WM_XBUTTONUP;
}

class Receiver final : public QWidget {
public:
    Receiver() {
        setWindowTitle(QStringLiteral("MaiChat owned SDK-free system input receiver"));
        setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        resize(780, 560);
    }
    ~Receiver() override {
        if (edit) {
            SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalEdit));
            DestroyWindow(edit);
        }
        if (surface) DestroyWindow(surface);
    }
    bool createChildren() {
        nativeRoot_ = reinterpret_cast<HWND>(winId());
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MaiOwnedSystemInputReceiver20261117";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        surface = CreateWindowExW(0, wc.lpszClassName, L"Owned input surface", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1,
                                  root(), nullptr, wc.hInstance, this);
        if (!surface) return false;
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL, 0,
                               0, 1, 1, surface, nullptr, wc.hInstance, nullptr);
        if (!edit) return false;
        SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        originalEdit = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(edit, GWLP_WNDPROC));
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure));
        layoutChildren();
        return true;
    }
    // Cached on the GUI thread; workers never call QWidget::winId().
    HWND root() const {
        return nativeRoot_;
    }
    bool owned(HWND window) const {
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        return window && pid == GetCurrentProcessId() && GetAncestor(window, GA_ROOT) == root();
    }
    void observe(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (!owned(window)) return;
        if (!mouseMessage(message) && message != WM_KEYDOWN && message != WM_KEYUP && message != WM_SYSKEYDOWN &&
            message != WM_SYSKEYUP && message != WM_CHAR)
            return;
        if (events.size() >= 8192) {
            truncated = true;
            return;
        }
        Event value;
        value.window = window;
        value.message = message;
        value.wparam = std::uint64_t(wp);
        value.lparam = std::int64_t(lp);
        value.time = GetTickCount64();
        value.highResolutionMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        value.queuedTime = DWORD(GetMessageTime());
        if (message == WM_CHAR) text.append(QChar(ushort(wp)));
        if (mouseMessage(message)) {
            value.mouseFlags = LOWORD(wp);
            value.control = (value.mouseFlags & MK_CONTROL) != 0;
            value.client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetLastError(0);
            const bool originOk = ClientToScreen(window, &value.origin) != FALSE;
            value.positionError = GetLastError();
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
                value.screen = value.client;  // Wheel LPARAM is already a screen point.
                SetLastError(0);
                const bool converted = ScreenToClient(window, &value.client) != FALSE;
                value.positionError = converted ? value.positionError : GetLastError();
                POINT roundTrip = value.client;
                value.positionValid = originOk && converted && ClientToScreen(window, &roundTrip) &&
                                      roundTrip.x == value.screen.x && roundTrip.y == value.screen.y;
                value.wheel = short(HIWORD(wp));
            } else {
                value.screen = value.client;
                SetLastError(0);
                value.positionValid = ClientToScreen(window, &value.screen) != FALSE && originOk;
                if (!value.positionValid) value.positionError = GetLastError();
            }
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) value.button = 1;
            if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) value.button = 2;
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) value.button = 3;
            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) value.button = HIWORD(wp) == XBUTTON1 ? 4 : 5;
        } else {
            value.key = UINT(wp);
            value.extended = (std::uintptr_t(lp) & (std::uintptr_t(1) << 24)) != 0;
            value.previousDown = (std::uintptr_t(lp) & (std::uintptr_t(1) << 30)) != 0;
            value.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        }
        events.push_back(value);
    }
    std::optional<Event> lastMove() const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->message == WM_MOUSEMOVE && it->positionValid) return *it;
        return {};
    }
    unsigned count(UINT message, UINT key = 0) const {
        return unsigned(std::count_if(events.begin(), events.end(), [&](const auto& value) {
            return value.message == message && (!key || value.key == key);
        }));
    }
    std::atomic<bool> canceled{false};
    HWND surface = nullptr, edit = nullptr;
    bool truncated = false;
    QString text;
    std::vector<Event> events;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        layoutChildren();
    }
    void closeEvent(QCloseEvent* event) override {
        canceled = true;
        QWidget::closeEvent(event);
    }

private:
    void layoutChildren() {
        if (!surface) return;
        RECT client{};
        GetClientRect(root(), &client);
        MoveWindow(surface, 0, 0, client.right, client.bottom, TRUE);
        if (edit)
            MoveWindow(edit, 16, (std::max)(80L, client.bottom - 140), (std::max)(100L, client.right - 32), 120, TRUE);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            receiver = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(receiver));
        }
        if (!receiver) return DefWindowProcW(window, message, wp, lp);
        receiver->observe(window, message, wp, lp);
        if (message == WM_KEYDOWN && wp == VK_ESCAPE) receiver->canceled = true;
        if (down(message)) SetCapture(window);
        if (up(message) && !(LOWORD(wp) & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) &&
            GetCapture() == window)
            ReleaseCapture();
        if (message == WM_CONTEXTMENU) return 0;
        if (window == receiver->edit) return CallWindowProcW(receiver->originalEdit, window, message, wp, lp);
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT area{};
            GetClientRect(window, &area);
            FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"Owned receiver: mouse surface above, Unicode edit below", -1, &area, DT_CENTER | DT_TOP);
            EndPaint(window, &paint);
            return 0;
        }
        if (mouseMessage(message)) return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ? TRUE : 0;
        return DefWindowProcW(window, message, wp, lp);
    }
    WNDPROC originalEdit = nullptr;
    HWND nativeRoot_ = nullptr;
};

class Fixture {
public:
    explicit Fixture(QString directory)
        : directory_(std::move(directory)), session_(makeDesktopComputerInputSession(maiDesktopCreateInputSession())) {
        options_.keyboardRepeat = false;
    }
    bool prepare() {
        receiver_.move(120, 100);
        receiver_.show();
        if (!receiver_.createChildren())
            return preconditionResult("native_children_creation_failed", false, GetLastError());
        if (!pumpUntil([&] { return IsWindowVisible(receiver_.root()); }))
            return preconditionResult("owned_root_not_visible");
        if (!session_) return preconditionResult("native_session_unavailable");
        SetWindowPos(receiver_.root(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(receiver_.root());
        RECT area{};
        POINT origin{};
        if (!GetClientRect(receiver_.surface, &area))
            return preconditionResult("get_client_rect_failed", false, GetLastError());
        if (!ClientToScreen(receiver_.surface, &origin))
            return preconditionResult("client_to_screen_failed", false, GetLastError());
        precondition_.insert(QStringLiteral("client_origin"), point(origin));
        precondition_.insert(QStringLiteral("client_rect"), rect(area));
        port_.x = origin.x;
        port_.y = origin.y;
        port_.width = area.right;
        port_.height = area.bottom;
        port_.environmentX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        port_.environmentY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        port_.environmentWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        port_.environmentHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (!GetWindowRect(receiver_.root(), &initialBounds_))
            return preconditionResult("get_window_rect_failed", false, GetLastError());
        scope_.expectedWindowId = identifier(receiver_.root());
        scope_.expectedProcessId = GetCurrentProcessId();
        scope_.isCanceled = [this] {
            QElapsedTimer cost;
            cost.start();
            cancelChecks_.fetch_add(1);
            const bool result = !admitted();
            cancelCostNs_.fetch_add(std::uint64_t(cost.nsecsElapsed()));
            return result;
        };
        const POINT activation{origin.x + 36, origin.y + 56};
        activation_ = activation;
        activationKnown_ = true;
        if (!receiver_.owned(WindowFromPhysicalPoint(activation)))
            return preconditionResult("activation_point_not_owned");
        for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_CONTROL, VK_SHIFT, VK_MENU,
                        VK_LWIN, VK_RWIN})
            if (GetAsyncKeyState(key) & 0x8000) {
                precondition_.insert(QStringLiteral("external_key"), key);
                return preconditionResult("external_key_already_pressed");
            }
        const bool foregroundReady = pumpUntil([&] { return GetForegroundWindow() == receiver_.root(); }, 150);
        POINT cursor{};
        const bool cursorReady = GetPhysicalCursorPos(&cursor) && receiver_.owned(WindowFromPhysicalPoint(cursor));
        precondition_.insert(QStringLiteral("initial_foreground_owned"), foregroundReady);
        precondition_.insert(QStringLiteral("initial_cursor_owned"), cursorReady);
        if (!foregroundReady || !cursorReady) {
            // A successful SetForegroundWindow does not move the cursor. Use
            // this same session's ordinary display move (or activation click)
            // only after proving its target belongs to this process.
            const auto displays = maiDesktopDisplays();
            if (!displays) {
                precondition_.insert(QStringLiteral("api_error"), QString::fromStdString(displays.error().message()));
                return preconditionResult("display_geometry_unavailable");
            }
            for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_CONTROL, VK_SHIFT, VK_MENU,
                            VK_LWIN, VK_RWIN})
                if (GetAsyncKeyState(key) & 0x8000) {
                    precondition_.insert(QStringLiteral("external_key"), key);
                    return preconditionResult("external_key_before_positioning");
                }
            if (!receiver_.owned(WindowFromPhysicalPoint(activation)))
                return preconditionResult("activation_hit_changed");
            DesktopNativeInput activate;
            activate.kind = foregroundReady ? DesktopNativeInputKind::Move : DesktopNativeInputKind::Click;
            activate.desktopScope = true;
            activate.x = activation.x;
            activate.y = activation.y;
            for (const auto& display : displays.value())
                if (activation.x >= display.x && activation.y >= display.y &&
                    activation.x < display.x + display.width && activation.y < display.y + display.height)
                    activate.display = display;
            activate.isCanceled = [this, activation] {
                return receiver_.canceled || !receiver_.owned(WindowFromPhysicalPoint(activation));
            };
            precondition_.insert(QStringLiteral("positioning"),
                                 foregroundReady ? QStringLiteral("display_move") : QStringLiteral("display_click"));
            if (!activate.display.id) return preconditionResult("activation_point_has_no_display");
            const auto error = call([&] { return session_->input(activate); }, "owned_initial_positioning");
            if (error) {
                precondition_.insert(QStringLiteral("api_error"), QString::fromStdString(error.message()));
                return preconditionResult("owned_initial_positioning_failed");
            }
        }
        if (!pumpUntil([&] { return admitted(); }))
            return preconditionResult("foreground_cursor_or_bounds_gate_not_ready");
        SetFocus(receiver_.edit);
        if (GetFocus() != receiver_.edit) return preconditionResult("owned_edit_not_focused");
        if (!admitted()) return preconditionResult("final_owned_gate_changed");
        return preconditionResult("ready", true);
    }
    void run() {
        mouse();
        keyboard();
        heldScreenshot();
        clearing();
    }
    int finish() {
        const auto cleanup = call([&] { return session_ ? session_->releaseAll() : MaiError{}; });
        if (prepared_) check("final_owned_state_empty", !cleanup && session_->state().empty());
        call([] { return maiStopScreenshotCapture(); });
        if (prepared_) check("receiver_not_truncated", !receiver_.truncated);
        QJsonArray events;
        for (const auto& value : receiver_.events)
            events.append(QJsonObject{{"message", int(value.message)},
                                      {"hwnd", QString::number(identifier(value.window))},
                                      {"observed_ms", QString::number(value.time)},
                                      {"observed_high_resolution_ms", value.highResolutionMs},
                                      {"queued_time", int(value.queuedTime)},
                                      {"wparam", QString::number(value.wparam)},
                                      {"lparam", QString::number(value.lparam)},
                                      {"client", point(value.client)},
                                      {"client_origin", point(value.origin)},
                                      {"event_screen", point(value.screen)},
                                      {"position_from_message", value.positionValid},
                                      {"position_error", int(value.positionError)},
                                      {"key", int(value.key)},
                                      {"extended", value.extended},
                                      {"previous_down", value.previousDown},
                                      {"control", value.control},
                                      {"button", int(value.button)},
                                      {"mouse_flags", int(value.mouseFlags)},
                                      {"wheel_delta", value.wheel}});
        const QJsonObject report{{"fixture", "sdk-free-system-input-native"},
                                 {"pid", int(GetCurrentProcessId())},
                                 {"precondition_skip", !prepared_},
                                 {"precondition", precondition_},
                                 {"failures", failures_},
                                 {"available_basic_input_passed", prepared_ && failures_ == 0},
                                 {"full_sunshine_native_acceptance", false},
                                 {"interrupted", bool(receiver_.canceled)},
                                 {"polling_pump_control", pollingPumpControl},
                                 {"cancel_callback_count", QString::number(cancelChecks_.load())},
                                 {"cancel_callback_ns", QString::number(cancelCostNs_.load())},
                                 {"repeat_timing", repeatTiming_},
                                 {"cases", cases_},
                                 {"calls", calls_},
                                 {"events", events},
                                 {"screenshot", screenshot_}};
        QSaveFile output(QDir(directory_).filePath(QStringLiteral("system-native-receipt.json")));
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        const bool saved = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
        std::printf("SDK-free basic native input failures=%d precondition_skip=%d\n", failures_, !prepared_);
        return !saved ? 2 : !prepared_ ? 77 : failures_ ? 1 : 0;
    }

private:
    bool preconditionResult(const char* reason, bool ready = false, DWORD error = 0) {
        POINT cursor{};
        SetLastError(0);
        const bool cursorRead = GetPhysicalCursorPos(&cursor) != FALSE;
        const DWORD cursorError = GetLastError();
        const HWND cursorHit = cursorRead ? WindowFromPhysicalPoint(cursor) : nullptr;
        const HWND activationHit = activationKnown_ ? WindowFromPhysicalPoint(activation_) : nullptr;
        DWORD cursorPid = 0, activationPid = 0;
        if (cursorHit) GetWindowThreadProcessId(cursorHit, &cursorPid);
        if (activationHit) GetWindowThreadProcessId(activationHit, &activationPid);
        precondition_.insert(QStringLiteral("ready"), ready);
        precondition_.insert(QStringLiteral("reason"), QString::fromLatin1(reason));
        precondition_.insert(QStringLiteral("win32_error"), int(error));
        precondition_.insert(QStringLiteral("root"), QString::number(identifier(receiver_.root())));
        precondition_.insert(QStringLiteral("surface"), QString::number(identifier(receiver_.surface)));
        precondition_.insert(QStringLiteral("edit"), QString::number(identifier(receiver_.edit)));
        precondition_.insert(QStringLiteral("foreground"), QString::number(identifier(GetForegroundWindow())));
        precondition_.insert(QStringLiteral("cursor_read"), cursorRead);
        precondition_.insert(QStringLiteral("cursor_error"), int(cursorError));
        precondition_.insert(QStringLiteral("cursor_screen"), point(cursor));
        precondition_.insert(QStringLiteral("cursor_hit"), QString::number(identifier(cursorHit)));
        precondition_.insert(QStringLiteral("cursor_hit_pid"), int(cursorPid));
        precondition_.insert(QStringLiteral("cursor_owned"), cursorRead && receiver_.owned(cursorHit));
        precondition_.insert(QStringLiteral("activation_known"), activationKnown_);
        precondition_.insert(QStringLiteral("physical_activation"), point(activation_));
        precondition_.insert(QStringLiteral("activation_hit"), QString::number(identifier(activationHit)));
        precondition_.insert(QStringLiteral("activation_hit_pid"), int(activationPid));
        precondition_.insert(QStringLiteral("pid"), int(GetCurrentProcessId()));
        prepared_ = ready;
        return ready;
    }
    bool admitted() const {
        RECT bounds{};
        POINT cursor{};
        DWORD pid = 0;
        GetWindowThreadProcessId(receiver_.root(), &pid);
        return !receiver_.canceled && IsWindowVisible(receiver_.root()) && GetForegroundWindow() == receiver_.root() &&
               pid == GetCurrentProcessId() && GetWindowRect(receiver_.root(), &bounds) &&
               EqualRect(&bounds, &initialBounds_) && GetPhysicalCursorPos(&cursor) &&
               receiver_.owned(WindowFromPhysicalPoint(cursor));
    }
    bool target(double x, double y) const {
        return receiver_.owned(WindowFromPhysicalPoint({LONG(std::llround(x)), LONG(std::llround(y))}));
    }
    POINT position(double x, double y) const {
        return {LONG(std::llround(port_.x + x * (port_.width - 1))),
                LONG(std::llround(port_.y + y * (port_.height - 1)))};
    }
    void check(const char* name, bool passed, QString detail = {}) {
        cases_.append(QJsonObject{{"name", QString::fromLatin1(name)}, {"passed", passed}, {"detail", detail}});
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures_;
    }
    MaiError call(const std::function<MaiError()>& action, const char* label = "fixture") {
        MaiError result;
        const auto began = GetTickCount64();
        QJsonArray attempts;
        for (;;) {
            std::atomic<bool> done{false};
            const auto attemptBegan = GetTickCount64();
            std::thread worker([&] {
                result = action();
                done = true;
            });
            while (!done.load()) {
                if (GetTickCount64() - began > 10000) {
                    receiver_.canceled = true;
                    if (session_) session_->releaseAll();
                }
                pumpFor(2);
            }
            worker.join();
            attempts.append(QJsonObject{{"started_ms", QString::number(attemptBegan)},
                                        {"finished_ms", QString::number(GetTickCount64())},
                                        {"code", int(result.code())},
                                        {"error", QString::fromStdString(result.message())}});
            // A scheduled repeat may own admission. Retry only this exact
            // no-effect policy result, with a finite budget and live GUI pump.
            // Target/focus/cancel errors and partial native work are not retried.
            if (result.code() != MaiErrorCode::Busy ||
                result.message() != "another ordinary input operation or cleanup is in flight" ||
                receiver_.canceled || GetTickCount64() - began >= 2000) break;
            pumpFor(2);
        }
        calls_.append(QJsonObject{{"operation", QString::fromLatin1(label)},
                                  {"started_ms", QString::number(began)},
                                  {"finished_ms", QString::number(GetTickCount64())},
                                  {"attempts", attempts},
                                  {"admission_retries", attempts.size() - 1},
                                  {"code", int(result.code())},
                                  {"error", QString::fromStdString(result.message())}});
        pumpFor();
        return result;
    }
    bool typed(const char* name, const std::vector<DesktopInputEvent>& events) {
        if (!admitted()) {
            receiver_.canceled = true;
            check(name, false, QStringLiteral("owned foreground/hit-test gate closed"));
            return false;
        }
        const auto error = call([&] { return session_->inputEvents(events, scope_, port_, options_); }, name);
        check(name, !error, QString::fromStdString(error.message()));
        return !error;
    }
    bool legacy(const char* name, DesktopNativeInput input) {
        const bool positioned =
            input.kind == DesktopNativeInputKind::Move || input.kind == DesktopNativeInputKind::MouseDown ||
            input.kind == DesktopNativeInputKind::Scroll || input.kind == DesktopNativeInputKind::Click ||
            input.kind == DesktopNativeInputKind::Drag;
        if (!admitted() || (positioned && !target(input.x, input.y)) ||
            (input.kind == DesktopNativeInputKind::Drag && !target(input.toX, input.toY))) {
            receiver_.canceled = true;
            check(name, false, QStringLiteral("owned target gate closed"));
            return false;
        }
        input.expectedWindowId = scope_.expectedWindowId;
        input.expectedProcessId = scope_.expectedProcessId;
        input.isCanceled = scope_.isCanceled;
        const auto error = call([&] { return session_->input(input); }, name);
        check(name, !error, QString::fromStdString(error.message()));
        return !error;
    }
    bool move(const char* name, double x, double y) {
        const auto expected = position(x, y);
        if (!target(expected.x, expected.y)) {
            check(name, false);
            receiver_.canceled = true;
            return false;
        }
        DesktopInputEvent event;
        event.kind = Kind::AbsoluteMove;
        event.x = x;
        event.y = y;
        return typed(name, {event});
    }
    DesktopInputEvent button(unsigned id, bool release = false) const {
        DesktopInputEvent e;
        e.kind = Kind::MouseButton;
        e.button = std::uint8_t(id);
        e.release = release;
        return e;
    }
    DesktopInputEvent key(std::uint16_t code, bool release = false, std::uint8_t modifiers = 0) const {
        DesktopInputEvent e;
        e.kind = Kind::RawKey;
        e.key.code = code;
        e.key.release = release;
        e.key.modifiers = modifiers;
        return e;
    }
    void mouse() {
        if (!move("typed_absolute_setup", .3, .3)) return;
        const auto before = receiver_.lastMove();
        DesktopInputEvent relative;
        relative.kind = Kind::RelativeMove;
        relative.deltaX = 17;
        relative.deltaY = 11;
        typed("typed_relative", {relative});
        const auto after = receiver_.lastMove();
        check("relative_native_event_direction", before && after && after->time >= before->time &&
                                                     after->screen.x > before->screen.x &&
                                                     after->screen.y > before->screen.y);
        const auto wheelStart = receiver_.events.size();
        DesktopInputEvent wheel;
        wheel.kind = Kind::Wheel;
        wheel.wheelDelta120 = 15;
        typed("fine_vertical_15", {wheel});
        wheel.horizontal = true;
        wheel.wheelDelta120 = -30;
        typed("fine_horizontal_minus30", {wheel});
        int vertical = 0, horizontal = 0;
        for (std::size_t i = wheelStart; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            if (event.message == WM_MOUSEWHEEL) vertical += event.wheel;
            if (event.message == WM_MOUSEHWHEEL) horizontal += event.wheel;
        }
        check("fine_two_axis_native_signed_deltas", vertical == 15 && horizontal == -30);
        for (unsigned id = 1; id <= 5 && !receiver_.canceled; ++id) {
            const auto reference = receiver_.lastMove();
            const auto start = receiver_.events.size();
            typed("stationary_button_down", {button(id)});
            typed("stationary_button_up", {button(id, true)});
            unsigned downs = 0, ups = 0;
            std::size_t downAt = 0, upAt = 0;
            bool samePoint = reference.has_value();
            for (std::size_t i = start; i < receiver_.events.size(); ++i) {
                const auto& event = receiver_.events[i];
                if (event.button != id) continue;
                if (down(event.message)) {
                    ++downs;
                    downAt = i;
                }
                if (up(event.message)) {
                    ++ups;
                    upAt = i;
                }
                samePoint = samePoint && event.positionValid && std::abs(event.screen.x - reference->screen.x) <= 1 &&
                            std::abs(event.screen.y - reference->screen.y) <= 1;
            }
            check("five_stationary_buttons_native_order_and_position",
                  downs == 1 && ups == 1 && downAt < upAt && samePoint, QStringLiteral("button %1").arg(id));
        }
        for (bool twice : {false, true}) {
            const auto targetPoint = position(.45, .3);
            DesktopNativeInput click;
            click.kind = twice ? DesktopNativeInputKind::DoubleClick : DesktopNativeInputKind::Click;
            click.x = targetPoint.x;
            click.y = targetPoint.y;
            const auto start = receiver_.events.size();
            legacy(twice ? "ordinary_double_click" : "ordinary_click", click);
            unsigned downs = 0, ups = 0;
            bool ordered = true, pressed = false;
            for (std::size_t i = start; i < receiver_.events.size(); ++i) {
                const auto& event = receiver_.events[i];
                if (event.button != 1) continue;
                if (down(event.message)) {
                    ordered = ordered && !pressed;
                    pressed = true;
                    ++downs;
                }
                if (up(event.message)) {
                    ordered = ordered && pressed;
                    pressed = false;
                    ++ups;
                }
                ordered = ordered && event.positionValid && std::abs(event.screen.x - targetPoint.x) <= 1 &&
                          std::abs(event.screen.y - targetPoint.y) <= 1;
            }
            check("ordinary_click_native_pair_order",
                  downs == (twice ? 2U : 1U) && ups == downs && ordered && !pressed);
        }
        const auto dragStart = receiver_.events.size();
        const auto begin = position(.2, .45), end = position(.5, .5);
        DesktopNativeInput drag;
        drag.kind = DesktopNativeInputKind::Drag;
        drag.x = begin.x;
        drag.y = begin.y;
        drag.toX = end.x;
        drag.toY = end.y;
        drag.durationMs = 100;
        legacy("ordinary_complete_drag", drag);
        unsigned downs = 0, ups = 0;
        bool heldMove = false, endedAtTarget = false;
        for (std::size_t i = dragStart; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            if (event.message == WM_LBUTTONDOWN) ++downs;
            if (event.message == WM_MOUSEMOVE && (event.mouseFlags & MK_LBUTTON)) heldMove = true;
            if (event.message == WM_LBUTTONUP) {
                ++ups;
                endedAtTarget = event.positionValid && std::abs(event.screen.x - end.x) <= 1 &&
                                std::abs(event.screen.y - end.y) <= 1;
            }
        }
        check("ordinary_drag_native_hold_move_up",
              downs == 1 && ups == 1 && heldMove && endedAtTarget && session_->state().buttonsDown.empty());
    }
    void keyboard() {
        if (receiver_.canceled) return;
        SetFocus(receiver_.edit);
        const auto start = receiver_.events.size();
        typed("extended_enter_down", {key(VK_RETURN, false, 0x10)});
        typed("extended_enter_up_uses_frozen_binding", {key(VK_RETURN, true)});
        bool extendedDown = false, extendedUp = false;
        std::size_t downAt = 0, upAt = 0;
        for (std::size_t i = start; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            if (event.key != VK_RETURN || !event.extended) continue;
            if (event.message == WM_KEYDOWN) {
                extendedDown = true;
                downAt = i;
            }
            if (event.message == WM_KEYUP) {
                extendedUp = true;
                upAt = i;
            }
        }
        check("raw_extended_native_pair", extendedDown && extendedUp && downAt < upAt);
        for (bool ordinary : {false, true}) {
            SetWindowTextW(receiver_.edit, L"");
            receiver_.text.clear();
            const std::string utf8 =
                "A"
                "\xf0\x9f\x98\x80"
                "B";
            if (ordinary) {
                auto input = scope_;
                input.kind = DesktopNativeInputKind::Type;
                input.text = utf8;
                legacy("ordinary_unicode", input);
            } else {
                DesktopInputEvent text;
                text.kind = Kind::UnicodeText;
                text.text = utf8;
                typed("typed_unicode", {text});
            }
            const auto expected = QString::fromUtf8(utf8.data(), int(utf8.size()));
            check("unicode_native_utf16_surrogate_sequence", receiver_.text == expected);
            wchar_t text[64]{};
            GetWindowTextW(receiver_.edit, text, 64);
            check("unicode_owned_edit_contents", QString::fromWCharArray(text) == expected);
        }
        auto control = scope_;
        control.kind = DesktopNativeInputKind::KeyDown;
        control.key = "Ctrl";
        legacy("ordinary_control_down", control);
        auto begin = receiver_.events.size();
        typed("typed_a_under_ordinary_control", {key('A'), key('A', true)});
        checkControl(begin, false);
        control.kind = DesktopNativeInputKind::KeyUp;
        legacy("ordinary_control_up", control);
        typed("typed_control_down", {key(VK_LCONTROL)});
        begin = receiver_.events.size();
        auto letter = scope_;
        letter.kind = DesktopNativeInputKind::Key;
        letter.key = "A";
        legacy("ordinary_a_under_typed_control", letter);
        const auto at = receiver_.lastMove();
        if (at) {
            auto scroll = scope_;
            scroll.kind = DesktopNativeInputKind::Scroll;
            scroll.x = at->screen.x;
            scroll.y = at->screen.y;
            scroll.lines = 1;
            legacy("ordinary_wheel_under_typed_control", scroll);
        }
        checkControl(begin, true);
        check("control_native_lease_survives_composites", !session_->state().rawKeys.empty());
        typed("typed_control_up", {key(VK_LCONTROL, true)});
        options_.keyboardRepeat = true;
        options_.repeatDelayMs = 25;
        options_.repeatPeriodMs = 15;
        const auto count = receiver_.count(WM_KEYDOWN, VK_F6);
        const auto repeatStart = receiver_.events.size();
        typed("repeat_f6_down", {key(VK_F6)});
        const auto checksAfterDown = cancelChecks_.load();
        check("repeat_native_keydowns", pumpUntil([&] { return receiver_.count(WM_KEYDOWN, VK_F6) >= count + 3; }));
        QJsonArray repeatTimes, repeatIntervals, preciseRepeatIntervals;
        std::optional<std::uint64_t> previousRepeat;
        std::optional<double> previousPreciseRepeat;
        for (std::size_t i = repeatStart; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            if (event.message != WM_KEYDOWN || event.key != VK_F6) continue;
            repeatTimes.append(QString::number(event.time));
            if (previousRepeat) repeatIntervals.append(int(event.time - *previousRepeat));
            if (previousPreciseRepeat) preciseRepeatIntervals.append(event.highResolutionMs - *previousPreciseRepeat);
            previousRepeat = event.time;
            previousPreciseRepeat = event.highResolutionMs;
        }
        repeatTiming_ = QJsonObject{
            {"configured_delay_ms", int(options_.repeatDelayMs)},
            {"configured_period_ms", options_.repeatPeriodMs},
            {"received_down_times_ms", repeatTimes},
            {"received_intervals_ms", repeatIntervals},
            {"received_high_resolution_intervals_ms", preciseRepeatIntervals},
            {"caller_checks_during_background_wait", QString::number(cancelChecks_.load() - checksAfterDown)}};
        typed("repeat_f6_up", {key(VK_F6, true)});
        check("repeat_owned_state_cleared", session_->state().empty());
        const auto stopped = receiver_.count(WM_KEYDOWN, VK_F6);
        // Native target validation on this host can outlast the configured
        // repeat period; observe beyond a full measured dispatch cycle.
        pumpFor(600);
        check("repeat_stopped_after_native_up", receiver_.count(WM_KEYDOWN, VK_F6) == stopped);
        options_.keyboardRepeat = false;
    }
    void checkControl(std::size_t start, bool wheel) {
        bool keySeen = false, wheelSeen = !wheel;
        for (std::size_t i = start; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            keySeen = keySeen || (event.message == WM_KEYDOWN && event.key == 'A' && event.control);
            wheelSeen = wheelSeen || (event.message == WM_MOUSEWHEEL && event.wheel == 120 && event.control);
        }
        check("control_cross_entry_native_evidence", keySeen && wheelSeen);
    }
    void heldScreenshot() {
        if (receiver_.canceled || !move("held_absolute_setup", .25, .4)) return;
        const auto start = receiver_.events.size();
        const auto anchor = position(.25, .4);
        auto input = scope_;
        input.kind = DesktopNativeInputKind::MouseDown;
        input.button = DesktopMouseButton::Left;
        input.x = anchor.x;
        input.y = anchor.y;
        if (!legacy("ordinary_left_down", input)) return;
        const auto upCount = receiver_.count(WM_LBUTTONUP);
        RECT before{}, after{};
        DWORD pidBefore = 0, pidAfter = 0;
        GetWindowRect(receiver_.root(), &before);
        GetWindowThreadProcessId(receiver_.root(), &pidBefore);
        MaiScreenshot image;
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::WindowById;
        request.windowId = scope_.expectedWindowId;
        request.isCanceled = scope_.isCanceled;
        const auto captured = call([&] { return maiCaptureScreenshot(request, image); }, "owned_window_capture");
        GetWindowRect(receiver_.root(), &after);
        GetWindowThreadProcessId(receiver_.root(), &pidAfter);
        check("capture_only_owned_window_by_id", !captured && pidBefore == GetCurrentProcessId() &&
                                                     pidAfter == pidBefore && EqualRect(&before, &after) &&
                                                     !image.pngBytes.empty());
        check("capture_does_not_release_left",
              receiver_.count(WM_LBUTTONUP) == upCount && !session_->state().buttonsDown.empty());
        if (!image.pngBytes.empty()) {
            const QByteArray bytes(reinterpret_cast<const char*>(image.pngBytes.data()), int(image.pngBytes.size()));
            QSaveFile file(QDir(directory_).filePath(QStringLiteral("owned-held.png")));
            check("owned_capture_saved",
                  file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit());
            screenshot_ = QJsonObject{
                {"target", "WindowById"},
                {"window_id", QString::number(request.windowId)},
                {"width", image.width},
                {"height", image.height},
                {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        }
        input.kind = DesktopNativeInputKind::Move;
        const auto end = position(.6, .4);
        input.x = end.x;
        input.y = end.y;
        legacy("ordinary_move_preserving_left", input);
        DesktopInputEvent relative;
        relative.kind = Kind::RelativeMove;
        relative.deltaX = 9;
        relative.deltaY = 5;
        typed("relative_move_preserving_left", {relative});
        const auto moved = receiver_.lastMove();
        check("held_native_move_flag", moved && (moved->mouseFlags & MK_LBUTTON));
        typed("typed_up_releases_ordinary_left", {button(1, true)});
        unsigned downs = 0, ups = 0;
        std::size_t downAt = 0, upAt = 0;
        POINT release{};
        for (std::size_t i = start; i < receiver_.events.size(); ++i) {
            const auto& event = receiver_.events[i];
            if (event.message == WM_LBUTTONDOWN) {
                ++downs;
                downAt = i;
            }
            if (event.message == WM_LBUTTONUP) {
                ++ups;
                upAt = i;
                release = event.screen;
            }
        }
        check("single_down_move_up_across_capture", downs == 1 && ups == 1 && downAt < upAt && moved &&
                                                        release.x == moved->screen.x && release.y == moved->screen.y &&
                                                        session_->state().buttonsDown.empty());
    }
    void clearing() {
        if (receiver_.canceled) return;
        for (bool reset : {true, false}) {
            typed("cleanup_key_down", {key(VK_LCONTROL)});
            typed("cleanup_left_down", {button(1)});
            const auto keyUps = receiver_.count(WM_KEYUP, VK_CONTROL) + receiver_.count(WM_KEYUP, VK_LCONTROL);
            const auto mouseUps = receiver_.count(WM_LBUTTONUP);
            const auto error = call([&] { return reset ? session_->resetControls() : session_->releaseAll(); });
            check(reset ? "reset_actual_owned_state_empty" : "release_actual_owned_state_empty",
                  !error && session_->state().empty());
            const auto finalKeyUps = receiver_.count(WM_KEYUP, VK_CONTROL) + receiver_.count(WM_KEYUP, VK_LCONTROL);
            check("cleanup_native_key_mouse_ups",
                  finalKeyUps > keyUps && receiver_.count(WM_LBUTTONUP) == mouseUps + 1);
        }
    }
    QString directory_;
    Receiver receiver_;
    std::shared_ptr<DesktopNativeInputSession> session_;
    DesktopNativeInput scope_;
    DesktopInputPort port_;
    DesktopInputOptions options_;
    RECT initialBounds_{};
    POINT activation_{};
    bool activationKnown_ = false;
    bool prepared_ = false;
    int failures_ = 0;
    std::atomic<std::uint64_t> cancelChecks_{0}, cancelCostNs_{0};
    QJsonArray cases_, calls_;
    QJsonObject precondition_, screenshot_;
    QJsonObject repeatTiming_;
};
}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--run-owned-input") == 0) run = true;
        if (qstrcmp(argv[i], "--polling-pump-control") == 0) pollingPumpControl = true;
    }
    if (!run) {
        std::puts("SKIP: opt-in only; --run-owned-input --output-dir <owned evidence directory>");
        return 77;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QApplication application(argc, argv);
    // Qt uses the native wide command line on Windows; don't narrow evidence
    // paths through the CRT's local codepage before constructing QApplication.
    const auto args = application.arguments();
    const int output = args.indexOf(QStringLiteral("--output-dir"));
    if (output < 0 || output + 1 >= args.size()) return 2;
    const QString directory = args[output + 1];
    if (!QDir().mkpath(directory)) return 2;
    Fixture fixture(directory);
    if (fixture.prepare()) fixture.run();
    return fixture.finish();
}
#else
#include <cstdio>
int main() {
    std::puts("SKIP: Windows-only system input receiver");
    return 77;
}
#endif
