// Opt-in owned multi-touch acceptance through the real Controller/tool core.
// Sender success is never substituted for native pointer/frame observations.
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QResizeEvent>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <json.hpp>
#include <map>
#include <optional>
#include <set>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiScreenshot.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
using Phase = DesktopInputPhase;
constexpr std::uint32_t firstId = 11, secondId = 0xfffffffe, thirdId = 33;
std::uint64_t id(const void* value) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(value));
}
Json point(POINT value) {
    return Json::array({value.x, value.y});
}
void dispatchFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
bool waitFor(const std::function<bool()>& ready, int milliseconds = 1000) {
    if (ready()) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(2);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (ready()) loop.quit();
    });
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    return ready();
}
bool pointerMessage(UINT message) {
    switch (message) {
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_POINTERENTER:
        case WM_POINTERLEAVE:
        case WM_POINTERACTIVATE:
        case WM_POINTERCAPTURECHANGED:
        case WM_POINTERROUTEDTO:
        case WM_POINTERROUTEDAWAY:
        case WM_POINTERROUTEDRELEASED: return true;
        default: return false;
    }
}
Json touchInfo(const POINTER_TOUCH_INFO& touch) {
    const auto& p = touch.pointerInfo;
    return {{"id", p.pointerId},
            {"frame_id", p.frameId},
            {"source", id(p.sourceDevice)},
            {"target", id(p.hwndTarget)},
            {"type", int(p.pointerType)},
            {"flags", p.pointerFlags},
            {"native_time", p.dwTime},
            {"performance_count", p.PerformanceCount},
            {"point", point(p.ptPixelLocation)},
            {"mask", touch.touchMask},
            {"pressure", touch.pressure},
            {"orientation", touch.orientation},
            {"contact",
             Json::array({touch.rcContact.left, touch.rcContact.top, touch.rcContact.right, touch.rcContact.bottom})}};
}
struct Raw {
    HWND hwnd = nullptr;
    UINT message = 0, pointerId = 0;
    std::string stage;
    ULONGLONG observed = 0;
    DWORD messageTime = 0, infoError = 0, frameError = 0, typeError = 0;
    POINTER_INPUT_TYPE type = PT_POINTER;
    POINTER_TOUCH_INFO touch{};
    std::vector<POINTER_TOUCH_INFO> frame;
    UINT32 requiredCount = 10;
    POINT envelope{}, client{}, origin{}, roundtripScreen{};
    bool typeOk = false, infoOk = false, frameOk = false, roundtrip = false;
};
class Receiver final : public QWidget {
public:
    Receiver() {
        setWindowTitle(QStringLiteral("MaiChat owned Controller multi-touch receiver"));
        setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_InputMethodEnabled, false);
        resize(900, 660);
    }
    ~Receiver() override {
        if (surface) DestroyWindow(surface);
    }
    HWND root() const {
        return root_;
    }
    bool owns(HWND window) const {
        DWORD process = 0;
        if (window) GetWindowThreadProcessId(window, &process);
        return window && process == GetCurrentProcessId() && GetAncestor(window, GA_ROOT) == root_;
    }
    bool createSurface() {
        root_ = reinterpret_cast<HWND>(winId());
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"MaiOwnedProductTouchReceiver20261117";
        type.lpfnWndProc = procedure;
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&type);
        surface = CreateWindowExW(0, type.lpszClassName, L"Owned touch surface", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1,
                                  root_, nullptr, type.hInstance, this);
        resizeSurface();
        return surface != nullptr;
    }
    void record(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (!owns(window)) return;
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) canceled = true;
        if (!pointerMessage(message)) return;
        Raw e;
        e.hwnd = window;
        e.message = message;
        e.pointerId = GET_POINTERID_WPARAM(wparam);
        e.stage = stage;
        e.observed = GetTickCount64();
        e.messageTime = DWORD(GetMessageTime());
        e.envelope = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetLastError(0);
        e.typeOk = GetPointerType(e.pointerId, &e.type) != FALSE;
        e.typeError = e.typeOk ? 0 : GetLastError();
        SetLastError(0);
        e.infoOk = GetPointerTouchInfo(e.pointerId, &e.touch) != FALSE;
        e.infoError = e.infoOk ? 0 : GetLastError();
        std::array<POINTER_TOUCH_INFO, 10> frame{};
        SetLastError(0);
        e.frameOk = GetPointerFrameTouchInfo(e.pointerId, &e.requiredCount, frame.data()) != FALSE;
        e.frameError = e.frameOk ? 0 : GetLastError();
        if (e.frameOk && e.requiredCount <= frame.size())
            e.frame.assign(frame.begin(), frame.begin() + e.requiredCount);
        if (e.infoOk) {
            e.client = e.touch.pointerInfo.ptPixelLocation;
            if (ScreenToClient(window, &e.client) && ClientToScreen(window, &e.origin)) {
                e.roundtripScreen = e.client;
                e.roundtrip = ClientToScreen(window, &e.roundtripScreen) &&
                              e.roundtripScreen.x == e.touch.pointerInfo.ptPixelLocation.x &&
                              e.roundtripScreen.y == e.touch.pointerInfo.ptPixelLocation.y;
            }
        }
        if (events.size() < 4096)
            events.push_back(e);
        else
            truncated = true;
    }
    HWND surface = nullptr;
    std::atomic<bool> canceled{false};
    std::string stage = "prepare";
    std::vector<Raw> events;
    bool truncated = false;

protected:
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        const auto* event = static_cast<MSG*>(message);
        if (event->hwnd == root_) record(root_, event->message, event->wParam, event->lParam);
        return QWidget::nativeEvent(type, message, result);
    }
    void closeEvent(QCloseEvent* event) override {
        canceled = true;
        event->ignore();
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        resizeSurface();
    }

private:
    void resizeSurface() {
        if (!surface) return;
        RECT bounds{};
        GetClientRect(root_, &bounds);
        MoveWindow(surface, 0, 0, bounds.right, bounds.bottom, TRUE);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            receiver = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(receiver));
        }
        if (receiver) receiver->record(window, message, wparam, lparam);
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT area{};
            GetClientRect(window, &area);
            FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            DrawTextW(dc, L"Owned multi-touch: two contacts, capture, independent lifetimes", -1, &area,
                      DT_TOP | DT_CENTER);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    HWND root_ = nullptr;
};

class Fixture {
public:
    explicit Fixture(QString output)
        : output_(std::move(output)), controller_(std::make_shared<DesktopComputerController>()) {
        registerDesktopComputerTools(registry_, controller_);
        tool_ = registry_.findForModel("desktop_computer");
        context_.sessionId = "owned-touch-session-" + std::to_string(GetCurrentProcessId());
        context_.turnId = "owned-touch-turn-" + std::to_string(GetCurrentProcessId());
        context_.root = workspace_.path().toUtf8().toStdString();
        context_.cancel = &receiver_.canceled;
    }
    ~Fixture() {
        if (inputStarted_) release("destructor_owned_cleanup");
    }
    bool prepare() {
        receiver_.move(150, 110);
        receiver_.show();
        if (!workspace_.isValid() || !tool_ || !receiver_.createSurface())
            return precondition("owned_fixture_unavailable");
        SetWindowPos(receiver_.root(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(receiver_.root());
        if (!waitFor([&] { return IsWindowVisible(receiver_.root()) && GetForegroundWindow() == receiver_.root(); },
                     500))
            return precondition("owned_foreground_not_acquired");
        if (!GetWindowRect(receiver_.root(), &initialBounds_)) return precondition("owned_bounds_unavailable");
        for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_CONTROL, VK_SHIFT,
                              VK_MENU, VK_LWIN, VK_RWIN})
            if (GetAsyncKeyState(key) & 0x8000) {
                precondition_["external_key"] = key;
                return precondition("external_control_held");
            }
        prepared_ = true;
        if (!observe(false, "initial_owned_observe")) {
            precondition("owned_observation_failed");
            return false;
        }
        anchor_ = projected(.3F, .4F);
        if (!admitted(anchor_)) {
            prepared_ = false;
            return precondition("owned_point_gate_closed");
        }
        return precondition("ready", true);
    }
    void exercise() {
        POINT cursor{};
        if (!GetPhysicalCursorPos(&cursor) || !receiver_.owns(WindowFromPhysicalPoint(cursor))) {
            auto position = action("move");
            position.xRatio = .3;
            position.yRatio = .4;
            inputStarted_ = true;
            const auto error = run("owned_mouse_positioning", [&] {
                const auto result = controller_->perform(context_.sessionId, position);
                return result ? MaiError{} : result.error();
            });
            check("owned_mouse_positioning", !error);
            if (error) return;
            if (!observe(false, "observe_after_positioning")) return;
        }
        primary();
        if (release("release_before_cold_ten") && controller_->inputState(context_.sessionId).empty() &&
            !receiver_.canceled)
            coldTen();
    }
    int save() {
        if (inputStarted_) release("final_owned_release");
        Json events = Json::array();
        for (const auto& e : receiver_.events) {
            Json frame = Json::array();
            for (const auto& peer : e.frame) frame.push_back(touchInfo(peer));
            events.push_back({{"hwnd", id(e.hwnd)},
                              {"message", e.message},
                              {"pointer_id", e.pointerId},
                              {"stage", e.stage},
                              {"observed_ms", e.observed},
                              {"message_time", e.messageTime},
                              {"type_ok", e.typeOk},
                              {"type", int(e.type)},
                              {"type_error", e.typeError},
                              {"info_ok", e.infoOk},
                              {"info_error", e.infoError},
                              {"touch", touchInfo(e.touch)},
                              {"frame_ok", e.frameOk},
                              {"frame_error", e.frameError},
                              {"frame_required_count", e.requiredCount},
                              {"frame", frame},
                              {"envelope_point", point(e.envelope)},
                              {"client", point(e.client)},
                              {"client_origin", point(e.origin)},
                              {"client_to_screen", point(e.roundtripScreen)},
                              {"roundtrip_ok", e.roundtrip}});
        }
        const Json report{{"fixture", "product-controller-and-tool-owned-touch"},
                          {"pid", GetCurrentProcessId()},
                          {"owned_window_id", id(receiver_.root())},
                          {"session_id", context_.sessionId},
                          {"turn_id", context_.turnId},
                          {"precondition_skip", !prepared_},
                          {"precondition", precondition_},
                          {"failures", failures_},
                          {"input_attempted", inputStarted_},
                          {"model_inference_used", false},
                          {"continuous_event_loop", true},
                          {"final_owned_state_empty", controller_->inputState(context_.sessionId).empty()},
                          {"truncated", receiver_.truncated},
                          {"checks", checks_},
                          {"calls", calls_},
                          {"lifetimes", lifetimes_},
                          {"raw", events}};
        const auto bytes = QByteArray::fromStdString(report.dump(2));
        QSaveFile file(QDir(output_).filePath(QStringLiteral("touch-native-receipt.json")));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        std::printf("Product Touch failures=%d skip=%d raw=%zu\n", failures_, !prepared_, receiver_.events.size());
        return !saved ? 2 : !prepared_ ? 77 : failures_ ? 1 : 0;
    }

private:
    bool precondition(const char* reason, bool ready = false) {
        const auto hit = WindowFromPhysicalPoint(anchor_);
        DWORD process = 0;
        if (hit) GetWindowThreadProcessId(hit, &process);
        precondition_.update({{"reason", reason},
                              {"ready", ready},
                              {"hwnd", id(receiver_.root())},
                              {"surface", id(receiver_.surface)},
                              {"foreground", id(GetForegroundWindow())},
                              {"point", point(anchor_)},
                              {"hit", id(hit)},
                              {"hit_pid", process}});
        return ready;
    }
    bool admitted(POINT point) const {
        RECT bounds{};
        DWORD process = 0;
        GetWindowThreadProcessId(receiver_.root(), &process);
        return !receiver_.canceled && IsWindowVisible(receiver_.root()) && GetForegroundWindow() == receiver_.root() &&
               process == GetCurrentProcessId() && GetWindowRect(receiver_.root(), &bounds) &&
               EqualRect(&initialBounds_, &bounds) && receiver_.owns(WindowFromPhysicalPoint(point));
    }
    void check(const char* name, bool passed) {
        checks_.push_back({{"name", name}, {"passed", passed}});
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures_;
    }
    MaiError run(const char* name, const std::function<MaiError()>& function) {
        receiver_.stage = name;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        bool expired = false;
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
            receiver_.canceled = true;
            expired = true;
        });
        MaiError error;
        const auto started = GetTickCount64();
        std::thread worker([&] {
            try {
                error = function();
            } catch (...) {
                error = {MaiErrorCode::Internal, "owned Touch fixture operation threw"};
            }
            QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
        });
        watchdog.start(12000);
        loop.exec();
        watchdog.stop();
        worker.join();
        const auto state = controller_->inputState(context_.sessionId);
        calls_.push_back({{"operation", name},
                          {"started_ms", started},
                          {"finished_ms", GetTickCount64()},
                          {"code", int(error.code())},
                          {"error", error.message()},
                          {"watchdog", expired},
                          {"owned_contacts", state.touchPointers},
                          {"devices", state.devices}});
        if (expired) check("operation_watchdog_not_fired", false);
        dispatchFor(15);
        return error;
    }
    bool observe(bool model, const char* name) {
        const auto error = run(name, [&] {
            if (model) {
                const auto result = tool_->execute(Json{{"action", "observe"},
                                                        {"window_id", std::to_string(id(receiver_.root()))},
                                                        {"include_elements", false}}
                                                       .dump(),
                                                   context_);
                if (result.hasError()) return result.error();
                snapshot_ = Json::parse(result.output(), nullptr, false);
                if (!result.images().empty())
                    return MaiError{MaiErrorCode::Protocol, "metadata observe returned images"};
            } else {
                const auto result = controller_->observe(
                    context_.sessionId, "", context_.root, false, id(receiver_.root()), false,
                    [&] { return receiver_.canceled.load(); }, context_.turnId);
                if (!result) return result.error();
                snapshot_ = Json::parse(result.value().json, nullptr, false);
                if (!result.value().imagePath.empty())
                    return MaiError{MaiErrorCode::Protocol, "metadata observe captured an image"};
            }
            if (!snapshot_.is_object() ||
                snapshot_.value("window_id", std::string()) != std::to_string(id(receiver_.root())) ||
                snapshot_.value("process_id", std::uint64_t(0)) != GetCurrentProcessId() ||
                snapshot_.value("snapshot_id", std::string()).empty() || !snapshot_.contains("bounds") ||
                !snapshot_["bounds"].is_object() || snapshot_["bounds"].value("width", 0.0) <= 0 ||
                snapshot_["bounds"].value("height", 0.0) <= 0)
                return MaiError{MaiErrorCode::Protocol, "observation did not bind the owned receiver"};
            return MaiError{};
        });
        check(name, !error);
        return !error;
    }
    POINT projected(float x, float y) const {
        const auto& bounds = snapshot_.at("bounds");
        return {LONG(std::llround(bounds.value("x", 0.0) + double(x) * (bounds.value("width", 0.0) - 1))),
                LONG(std::llround(bounds.value("y", 0.0) + double(y) * (bounds.value("height", 0.0) - 1)))};
    }
    DesktopComputerAction action(const char* name, bool cleanup = false) {
        DesktopComputerAction result;
        result.action = name;
        result.workspace = context_.root;
        result.turnId = context_.turnId;
        result.isCanceled = [&] { return receiver_.canceled.load(); };
        result.inputOptions.inheritExisting = true;
        if (!cleanup) {
            result.windowId = id(receiver_.root());
            result.snapshotId = snapshot_.value("snapshot_id", std::string());
        }
        return result;
    }
    static DesktopInputTouch contact(std::uint32_t identity, Phase phase, float x = 0, float y = 0,
                                     float pressure = .4F) {
        DesktopInputTouch value;
        value.pointerId = identity;
        value.phase = phase;
        value.x = x;
        value.y = y;
        value.pressureOrDistance = pressure;
        value.contactAreaMajor = .08F;
        value.contactAreaMinor = .04F;
        value.rotation = 90;
        return value;
    }
    static const char* phaseName(Phase phase) {
        switch (phase) {
            case Phase::Down: return "down";
            case Phase::Move: return "move";
            case Phase::Up: return "up";
            case Phase::Cancel: return "cancel";
            case Phase::CancelAll: return "cancel_all";
            default: return "invalid";
        }
    }
    bool batch(const std::vector<DesktopInputTouch>& contacts, bool model, bool cleanup, const char* name) {
        if (!cleanup)
            for (const auto& c : contacts)
                if (!admitted(projected(c.x, c.y))) {
                    receiver_.canceled = true;
                    check("owned_contact_point_gate", false);
                    return false;
                }
        inputStarted_ = true;
        const auto error = run(name, [&] {
            if (model) {
                Json values = Json::array();
                for (const auto& c : contacts) {
                    Json value{{"phase", phaseName(c.phase)}, {"pointer_id", c.pointerId}};
                    if (!cleanup)
                        value.update({{"x_ratio", c.x},
                                      {"y_ratio", c.y},
                                      {"pressure_or_distance", c.pressureOrDistance},
                                      {"contact_area_major", c.contactAreaMajor},
                                      {"contact_area_minor", c.contactAreaMinor},
                                      {"rotation", c.rotation}});
                    values.push_back(value);
                }
                Json request{{"action", "input_events"},
                             {"events", Json::array({{{"type", "touch"}, {"contacts", values}}})}};
                if (!cleanup)
                    request.update({{"window_id", std::to_string(id(receiver_.root()))},
                                    {"snapshot_id", snapshot_.value("snapshot_id", std::string())}});
                const auto result = tool_->execute(request.dump(), context_);
                if (result.hasError()) return result.error();
                const auto output = Json::parse(result.output(), nullptr, false);
                if (!output.is_object() || !output.value("dispatched", false) ||
                    output.value("accepted_events", 0) != 1)
                    return MaiError{MaiErrorCode::Protocol, "tool did not dispatch one full contact event"};
                return MaiError{};
            }
            auto request = action("input_events", cleanup);
            DesktopInputEvent event;
            event.kind = DesktopInputEventKind::Touch;
            event.contacts = contacts;
            request.events = {event};
            const auto result = controller_->perform(context_.sessionId, request);
            if (!result) return result.error();
            if (!result.value().dispatched || result.value().acceptedEvents != 1)
                return MaiError{MaiErrorCode::Protocol, "ordinary call did not dispatch one full contact event"};
            return MaiError{};
        });
        check(name, !error);
        return !error;
    }
    bool release(const char* name) {
        MaiError error;
        const auto start = GetTickCount64();
        do {
            error = run(name, [&] { return controller_->releaseInput(context_.sessionId, context_.turnId); });
            if (!error || error.code() != MaiErrorCode::Busy) break;
            dispatchFor(10);
        } while (GetTickCount64() - start < 1000);
        check(name, !error);
        return !error;
    }
    const Raw* event(std::uint32_t native, UINT message, std::size_t begin) const {
        for (std::size_t i = begin; i < receiver_.events.size(); ++i) {
            const auto& e = receiver_.events[i];
            if (e.message == message && e.pointerId == native && e.infoOk && e.typeOk && e.type == PT_TOUCH) return &e;
        }
        return nullptr;
    }
    std::optional<std::uint32_t> downAt(POINT position, std::size_t begin) const {
        for (std::size_t i = begin; i < receiver_.events.size(); ++i) {
            const auto& e = receiver_.events[i];
            if (e.message == WM_POINTERDOWN && e.infoOk && e.typeOk && e.type == PT_TOUCH &&
                e.touch.pointerInfo.ptPixelLocation.x == position.x &&
                e.touch.pointerInfo.ptPixelLocation.y == position.y)
                return e.pointerId;
        }
        return {};
    }
    bool fullFrame(std::size_t begin, const std::set<std::uint32_t>& ids) const {
        for (std::size_t i = begin; i < receiver_.events.size(); ++i) {
            const auto& e = receiver_.events[i];
            if (!e.frameOk || e.frame.size() != ids.size()) continue;
            std::set<std::uint32_t> peers;
            bool valid = true;
            HANDLE source = e.frame.front().pointerInfo.sourceDevice;
            const auto frameId = e.frame.front().pointerInfo.frameId;
            for (const auto& t : e.frame) {
                peers.insert(t.pointerInfo.pointerId);
                valid = valid && t.pointerInfo.pointerType == PT_TOUCH && t.pointerInfo.sourceDevice == source &&
                        t.pointerInfo.frameId == frameId && (t.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) &&
                        receiver_.owns(t.pointerInfo.hwndTarget);
            }
            if (valid && peers == ids) return true;
        }
        return false;
    }
    void lifetime(std::uint32_t native, std::size_t begin, bool longHold) {
        const auto* down = event(native, WM_POINTERDOWN, begin);
        const auto* up = event(native, WM_POINTERUP, begin);
        const bool pair =
            down && up && down < up && down->touch.pointerInfo.sourceDevice == up->touch.pointerInfo.sourceDevice;
        const DWORD nativeDelta = pair ? DWORD(up->touch.pointerInfo.dwTime - down->touch.pointerInfo.dwTime) : 0;
        const ULONGLONG observedDelta = pair ? up->observed - down->observed : 0;
        lifetimes_.push_back(
            {{"id", native}, {"pair", pair}, {"native_delta_ms", nativeDelta}, {"observed_delta_ms", observedDelta}});
        check("actual_native_down_up_same_source", pair);
        if (longHold) {
            check("native_lifetime_over_1250ms", pair && nativeDelta >= 1250);
            check("observed_lifetime_over_1250ms", pair && observedDelta >= 1250);
        }
    }
    void captureWhileHeld(std::size_t begin) {
        RECT before{}, after{};
        DWORD beforePid = 0, afterPid = 0;
        GetWindowRect(receiver_.root(), &before);
        GetWindowThreadProcessId(receiver_.root(), &beforePid);
        MaiScreenshot image;
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::WindowById;
        request.windowId = id(receiver_.root());
        request.isCanceled = [&] { return receiver_.canceled.load(); };
        const auto error =
            run("capture_exact_owned_window_while_held", [&] { return maiCaptureScreenshot(request, image); });
        GetWindowRect(receiver_.root(), &after);
        GetWindowThreadProcessId(receiver_.root(), &afterPid);
        check("capture_is_only_the_exact_owned_window",
              !error && beforePid == GetCurrentProcessId() && afterPid == beforePid && EqualRect(&before, &after) &&
                  image.width > 0 && image.height > 0 && !image.pngBytes.empty());
        check("capture_keeps_both_contacts", controller_->inputState(context_.sessionId).touchPointers.size() == 2 &&
                                                 std::none_of(receiver_.events.begin() + begin, receiver_.events.end(),
                                                              [](const auto& e) { return e.message == WM_POINTERUP; }));
        if (!image.pngBytes.empty()) {
            QSaveFile file(QDir(output_).filePath(QStringLiteral("owned-held-touch.png")));
            const QByteArray bytes(reinterpret_cast<const char*>(image.pngBytes.data()), int(image.pngBytes.size()));
            check("owned_capture_saved",
                  file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit());
        }
    }
    void primary() {
        if (!observe(false, "observe_before_cold_pair")) return;
        const auto begin = receiver_.events.size();
        const auto a = contact(firstId, Phase::Down, .3F, .4F, .4F);
        const auto b = contact(secondId, Phase::Down, .7F, .4F, .8F);
        const auto aPoint = projected(a.x, a.y), bPoint = projected(b.x, b.y);
        if (!batch({a, b}, false, false, "ordinary_cold_two_contacts_one_batch")) return;
        const auto heldAt = GetTickCount64();
        waitFor([&] { return downAt(aPoint, begin) && downAt(bPoint, begin); }, 500);
        const auto nativeA = downAt(aPoint, begin), nativeB = downAt(bPoint, begin);
        check("cold_two_actual_distinct_downs", nativeA && nativeB && *nativeA != *nativeB);
        if (nativeA && nativeB) check("cold_frame_has_both_peers", fullFrame(begin, {*nativeA, *nativeB}));
        receiver_.stage = "hold_before_observe";
        dispatchFor(450);
        if (!observe(true, "model_observe_while_two_held")) return;
        check("observation_keeps_two_owned_contacts",
              controller_->inputState(context_.sessionId).touchPointers.size() == 2);
        captureWhileHeld(begin);
        receiver_.stage = "hold_after_capture";
        if (GetTickCount64() - heldAt < 1300) dispatchFor(int(1300 - (GetTickCount64() - heldAt)));
        check("no_early_up_during_pair_hold", std::none_of(receiver_.events.begin() + begin, receiver_.events.end(),
                                                           [](const auto& e) { return e.message == WM_POINTERUP; }));
        if (nativeA && nativeB) {
            const auto* da = event(*nativeA, WM_POINTERDOWN, begin);
            const auto* db = event(*nativeB, WM_POINTERDOWN, begin);
            const auto width = double(a.contactAreaMajor) * snapshot_["bounds"].value("width", 0.0);
            const auto height = double(a.contactAreaMinor) * snapshot_["bounds"].value("height", 0.0);
            const auto properties = [width, height](const Raw* e, unsigned pressure) {
                return e && e->roundtrip &&
                       (e->touch.touchMask & (TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE)) ==
                           (TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE) &&
                       // Shared screen direction: source90deg points toward-X,
                       // which is native Win32 orientation180deg.
                       e->touch.pressure == pressure && e->touch.orientation == 180 &&
                       e->touch.rcContact.left <= e->touch.pointerInfo.ptPixelLocation.x &&
                       e->touch.rcContact.right > e->touch.pointerInfo.ptPixelLocation.x &&
                       e->touch.rcContact.top <= e->touch.pointerInfo.ptPixelLocation.y &&
                       e->touch.rcContact.bottom > e->touch.pointerInfo.ptPixelLocation.y &&
                       std::abs(double(e->touch.rcContact.right - e->touch.rcContact.left) - width) <= 2 &&
                       std::abs(double(e->touch.rcContact.bottom - e->touch.rcContact.top) - height) <= 2 &&
                       e->touch.rcContact.bottom > e->touch.rcContact.top &&
                       e->touch.rcContact.right > e->touch.rcContact.left;
            };
            check("contact_pressure_orientation_area_and_event_coordinates",
                  properties(da, 410) && properties(db, 819));
        }
        const auto moveBegin = receiver_.events.size();
        auto moved = contact(secondId, Phase::Move, .65F, .6F, .65F);
        const auto movedPoint = projected(moved.x, moved.y);
        if (!batch({moved}, true, false, "model_one_contact_move_preserves_peer")) return;
        dispatchFor(60);
        if (nativeA && nativeB) {
            check("move_frame_keeps_stable_native_peers", fullFrame(moveBegin, {*nativeA, *nativeB}));
            const bool actual =
                std::any_of(receiver_.events.begin() + moveBegin, receiver_.events.end(), [&](const auto& e) {
                    return e.infoOk && e.pointerId == *nativeB && e.message == WM_POINTERUPDATE &&
                           e.touch.pointerInfo.ptPixelLocation.x == movedPoint.x &&
                           e.touch.pointerInfo.ptPixelLocation.y == movedPoint.y;
                });
            check("requested_contact_really_moved", actual);
        }
        if (!batch({contact(firstId, Phase::Up)}, false, true, "ordinary_independent_up_without_token")) return;
        dispatchFor(80);
        check("one_contact_survives_independent_up",
              controller_->inputState(context_.sessionId).touchPointers == std::vector<std::uint32_t>{secondId});
        if (nativeA && nativeB) {
            check("first_actual_up_does_not_end_peer",
                  event(*nativeA, WM_POINTERUP, begin) && !event(*nativeB, WM_POINTERUP, begin));
            lifetime(*nativeA, begin, true);
        }
        if (!observe(false, "observe_before_adding_third")) return;
        const auto thirdBegin = receiver_.events.size();
        auto third = contact(thirdId, Phase::Down, .5F, .7F, .6F);
        const auto thirdPoint = projected(third.x, third.y);
        if (!batch({third}, true, false, "model_add_third_while_peer_survives")) return;
        dispatchFor(80);
        const auto nativeC = downAt(thirdPoint, thirdBegin);
        if (nativeB)
            check("third_contact_joins_same_full_peer_frame",
                  nativeC && *nativeC != *nativeB && fullFrame(thirdBegin, {*nativeB, *nativeC}));
        if (!batch({contact(thirdId, Phase::Cancel)}, true, true, "model_cancel_only_third_without_token")) return;
        dispatchFor(80);
        check("survivor_remains_after_peer_cancel",
              controller_->inputState(context_.sessionId).touchPointers == std::vector<std::uint32_t>{secondId});
        if (nativeB && nativeC)
            check("cancel_has_actual_up_without_canceling_survivor",
                  event(*nativeC, WM_POINTERUP, thirdBegin) && !event(*nativeB, WM_POINTERUP, begin));
        if (!release("ordinary_release_remaining_contact")) return;
        dispatchFor(80);
        if (nativeB) lifetime(*nativeB, begin, true);
        check("all_touch_controls_and_source_released", controller_->inputState(context_.sessionId).empty());
    }
    void coldTen() {
        if (!observe(true, "model_observe_before_cold_ten")) return;
        std::vector<DesktopInputTouch> contacts;
        std::vector<POINT> expected;
        for (int i = 0; i < 10; ++i) {
            auto c = contact(std::uint32_t(100 + i), Phase::Down, .18F + (i % 5) * .16F, i < 5 ? .35F : .65F, .5F);
            c.contactAreaMajor = c.contactAreaMinor = .02F;
            expected.push_back(projected(c.x, c.y));
            contacts.push_back(c);
        }
        const auto begin = receiver_.events.size();
        if (!batch(contacts, true, false, "model_cold_ten_contacts_one_batch")) return;
        waitFor(
            [&] {
                return std::all_of(expected.begin(), expected.end(),
                                   [&](POINT p) { return downAt(p, begin).has_value(); });
            },
            600);
        std::set<std::uint32_t> natives;
        for (const auto p : expected)
            if (const auto native = downAt(p, begin)) natives.insert(*native);
        check("cold_ten_actual_unique_downs", natives.size() == 10);
        check("cold_ten_full_native_frame", natives.size() == 10 && fullFrame(begin, natives));
        check("cold_ten_owned_state", controller_->inputState(context_.sessionId).touchPointers.size() == 10);
        dispatchFor(100);
        std::optional<std::uint32_t> replacementNative;
        std::size_t replacementDownAt = receiver_.events.size();
        if (natives.size() == 10) {
            // Fill every slot before replacing one contact in a single public
            // batch. The source must release the old slot before reusing it;
            // array-size-only fake contracts cannot prove an actual new DOWN.
            const auto retiredNative = downAt(expected.front(), begin);
            std::set<std::uint32_t> survivors = natives;
            if (retiredNative) survivors.erase(*retiredNative);
            std::map<std::uint32_t, HANDLE> originalSources;
            for (const auto native : survivors)
                if (const auto* down = event(native, WM_POINTERDOWN, begin))
                    originalSources.emplace(native, down->touch.pointerInfo.sourceDevice);
            if (!observe(false, "observe_before_full_slot_replacement")) return;
            const auto replacementBegin = receiver_.events.size();
            auto ending = contacts.front();
            ending.phase = Phase::Up;
            auto replacement = contact(200, Phase::Down, .5F, .82F, .6F);
            replacement.contactAreaMajor = replacement.contactAreaMinor = .02F;
            const auto replacementPoint = projected(replacement.x, replacement.y);
            if (!batch({ending, replacement}, false, false, "ordinary_full_slot_up_and_new_down_same_batch")) return;
            waitFor(
                [&] {
                    return retiredNative && event(*retiredNative, WM_POINTERUP, replacementBegin) &&
                           downAt(replacementPoint, replacementBegin).has_value();
                },
                600);
            replacementNative = downAt(replacementPoint, replacementBegin);
            const auto* retiredUp = retiredNative ? event(*retiredNative, WM_POINTERUP, replacementBegin) : nullptr;
            const auto* replacementDown =
                replacementNative ? event(*replacementNative, WM_POINTERDOWN, replacementBegin) : nullptr;
            check("full_slot_old_contact_has_actual_up", retiredUp != nullptr);
            check("full_slot_replacement_has_new_unique_down",
                  replacementNative && survivors.count(*replacementNative) == 0 && replacementDown);
            if (replacementDown) {
                replacementDownAt = std::size_t(replacementDown - receiver_.events.data());
                const auto downCount = std::count_if(receiver_.events.begin() + replacementBegin,
                                                     receiver_.events.end(), [&](const auto& e) {
                                                         return e.message == WM_POINTERDOWN && e.infoOk && e.typeOk &&
                                                                e.type == PT_TOUCH && e.pointerId == *replacementNative;
                                                     });
                check("full_slot_replacement_down_occurs_once", downCount == 1);
            }
            check("full_slot_up_precedes_slot_reuse",
                  retiredUp && replacementDown && retiredUp < replacementDown &&
                      DWORD(replacementDown->touch.pointerInfo.dwTime - retiredUp->touch.pointerInfo.dwTime) < 60000);
            bool peerStable = survivors.size() == 9 && originalSources.size() == 9;
            for (const auto native : survivors) {
                peerStable = peerStable && !event(native, WM_POINTERUP, begin);
                bool observedSamePeer = false;
                for (std::size_t i = replacementBegin; i < receiver_.events.size(); ++i)
                    for (const auto& peer : receiver_.events[i].frame)
                        if (peer.pointerInfo.pointerId == native &&
                            peer.pointerInfo.sourceDevice == originalSources[native] &&
                            (peer.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT))
                            observedSamePeer = true;
                peerStable = peerStable && observedSamePeer;
            }
            check("full_slot_nine_peers_keep_ids_source_and_contact", peerStable);
            auto activeNativeIds = survivors;
            if (replacementNative) activeNativeIds.insert(*replacementNative);
            check("full_slot_replacement_has_full_ten_peer_frame",
                  activeNativeIds.size() == 10 && fullFrame(replacementBegin, activeNativeIds));
            const auto state = controller_->inputState(context_.sessionId);
            std::set<std::uint32_t> activeClientIds(state.touchPointers.begin(), state.touchPointers.end());
            std::set<std::uint32_t> expectedClientIds{200};
            for (std::uint32_t client = 101; client <= 109; ++client) expectedClientIds.insert(client);
            check("full_slot_native_state_replaces_only_client_100", activeClientIds == expectedClientIds);
        }
        if (!release("ordinary_release_all_ten")) return;
        dispatchFor(100);
        for (const auto native : natives) lifetime(native, begin, false);
        // A legal native slot may be recycled after its real UP. Start the
        // replacement lifetime at its DOWN, so an older UP cannot satisfy it.
        if (replacementNative) lifetime(*replacementNative, replacementDownAt, false);
        check("cold_ten_final_state_empty", controller_->inputState(context_.sessionId).empty());
        check("raw_observation_not_truncated", !receiver_.truncated);
    }
    QString output_;
    QTemporaryDir workspace_;
    Receiver receiver_;
    std::shared_ptr<DesktopComputerController> controller_;
    MaiToolRegistry registry_;
    MaiTool* tool_ = nullptr;
    MaiToolContext context_;
    RECT initialBounds_{};
    POINT anchor_{};
    bool prepared_ = false, inputStarted_ = false;
    int failures_ = 0;
    Json snapshot_, precondition_ = Json::object(), checks_ = Json::array(), calls_ = Json::array(),
                    lifetimes_ = Json::array();
};
}  // namespace

int main(int argc, char** argv) {
    bool optIn = false;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--run-owned-input") == 0) optIn = true;
    if (!optIn) {
        std::puts("SKIP: --run-owned-input --output-dir ABS required");
        return 77;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QApplication app(argc, argv);
    const auto args = app.arguments();
    const int output = args.indexOf(QStringLiteral("--output-dir"));
    if (output < 0 || output + 1 >= args.size()) return 2;
    const auto directory = QDir::cleanPath(args[output + 1]);
    if (!QDir::isAbsolutePath(directory) || QFileInfo(directory).isSymLink() ||
        QFileInfo::exists(QDir(directory).filePath(QStringLiteral("touch-native-receipt.json"))) ||
        !QDir().mkpath(directory))
        return 2;
    Fixture fixture(directory);
    if (fixture.prepare()) fixture.exercise();
    return fixture.save();
}
