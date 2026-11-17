// Opt-in product-chain acceptance: ordinary Controller and registry tool share
// one real NativeSession. Only this process's receiver/workspace is inspected.
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QResizeEvent>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <json.hpp>
#include <optional>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

#include "MaiChatTools/DesktopComputerTools.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
using Phase = DesktopInputPhase;
using Kind = DesktopInputEventKind;
std::uint64_t identifier(const void* value) {
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(value));
}
Json point(POINT value) {
    return Json::array({value.x, value.y});
}
bool pointerMessage(UINT message) {
    switch (message) {
        case WM_POINTERUPDATE:
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_POINTERENTER:
        case WM_POINTERLEAVE:
        case WM_POINTERACTIVATE:
        case WM_POINTERCAPTURECHANGED:
        case WM_POINTERWHEEL:
        case WM_POINTERHWHEEL:
        case WM_NCPOINTERUPDATE:
        case WM_NCPOINTERDOWN:
        case WM_NCPOINTERUP:
        case WM_POINTERROUTEDTO:
        case WM_POINTERROUTEDAWAY:
        case WM_POINTERROUTEDRELEASED: return true;
        default: return false;
    }
}
void dispatchFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
bool waitFor(const std::function<bool()>& ready, int milliseconds = 2000) {
    if (ready()) return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready()) loop.quit();
    });
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return ready();
}
struct Raw {
    HWND hwnd = nullptr;
    UINT message = 0, id = 0;
    UINT_PTR wparam = 0;
    LONG_PTR lparam = 0;
    std::string phase;
    ULONGLONG observed = 0;
    DWORD messageTime = 0, typeError = 0, infoError = 0, frameError = 0;
    POINTER_INPUT_TYPE type = PT_POINTER;
    POINTER_PEN_INFO pen{};
    UINT32 frameCount = 1;
    bool typeOk = false, infoOk = false, frameOk = false, control = false, roundtrip = false;
    POINT envelope{}, client{}, origin{}, roundtripScreen{};
};
class Receiver final : public QWidget {
public:
    Receiver() {
        setWindowTitle(QStringLiteral("MaiChat owned product Pen receiver"));
        setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_InputMethodEnabled, false);
        resize(800, 560);
    }
    ~Receiver() override {
        if (surface) DestroyWindow(surface);
    }
    bool createSurface() {
        root_ = reinterpret_cast<HWND>(winId());
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"MaiOwnedProductPenReceiver20261117";
        type.lpfnWndProc = procedure;
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&type);
        surface = CreateWindowExW(0, type.lpszClassName, L"Owned pen surface", WS_CHILD | WS_VISIBLE, 0, 0, width(),
                                  height(), root_, nullptr, type.hInstance, this);
        resizeSurface();
        return surface != nullptr;
    }
    HWND root() const {
        return root_;
    }
    bool owns(HWND value) const {
        DWORD process = 0;
        if (value) GetWindowThreadProcessId(value, &process);
        return value && process == GetCurrentProcessId() && GetAncestor(value, GA_ROOT) == root_;
    }
    void observe(HWND value, UINT message, WPARAM wparam, LPARAM lparam) {
        if (!owns(value)) return;
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) canceled = true;
        if (message == WM_KEYUP && (wparam == VK_CONTROL || wparam == VK_LCONTROL)) ++controlUps;
        if (!pointerMessage(message)) return;
        Raw event;
        event.hwnd = value;
        event.message = message;
        event.id = GET_POINTERID_WPARAM(wparam);
        event.wparam = UINT_PTR(wparam);
        event.lparam = LONG_PTR(lparam);
        event.phase = phase;
        event.observed = GetTickCount64();
        event.messageTime = DWORD(GetMessageTime());
        event.control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        event.envelope = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetLastError(0);
        event.typeOk = GetPointerType(event.id, &event.type) != FALSE;
        event.typeError = event.typeOk ? 0 : GetLastError();
        SetLastError(0);
        event.infoOk = GetPointerPenInfo(event.id, &event.pen) != FALSE;
        event.infoError = event.infoOk ? 0 : GetLastError();
        POINTER_PEN_INFO frame{};
        SetLastError(0);
        event.frameOk = GetPointerFramePenInfo(event.id, &event.frameCount, &frame) != FALSE;
        event.frameError = event.frameOk ? 0 : GetLastError();
        if (event.infoOk) {
            event.client = event.pen.pointerInfo.ptPixelLocation;
            event.origin = {};
            if (ScreenToClient(value, &event.client) && ClientToScreen(value, &event.origin)) {
                event.roundtripScreen = event.client;
                event.roundtrip = ClientToScreen(value, &event.roundtripScreen) &&
                                  event.roundtripScreen.x == event.pen.pointerInfo.ptPixelLocation.x &&
                                  event.roundtripScreen.y == event.pen.pointerInfo.ptPixelLocation.y;
            }
        }
        if (raw.size() < 4096)
            raw.push_back(event);
        else
            truncated = true;
    }
    std::atomic<bool> canceled{false};
    std::string phase = "prepare";
    std::vector<Raw> raw;
    HWND surface = nullptr;
    unsigned controlUps = 0;
    bool truncated = false;

protected:
    bool nativeEvent(const QByteArray& type, void* value, qintptr* result) override {
        auto* message = static_cast<MSG*>(value);
        if (message->hwnd == root_) observe(root_, message->message, message->wParam, message->lParam);
        return QWidget::nativeEvent(type, value, result);
    }
    void closeEvent(QCloseEvent* event) override {
        canceled = true;
        event->ignore();  // Keep the owned target alive until cleanup finishes.
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
    static LRESULT CALLBACK procedure(HWND value, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(value, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            receiver = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(value, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(receiver));
        }
        if (receiver) receiver->observe(value, message, wparam, lparam);
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(value, &paint);
            RECT area{};
            GetClientRect(value, &area);
            FillRect(dc, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            DrawTextW(dc, L"Owned Controller / model-tool Pen acceptance", -1, &area, DT_CENTER | DT_TOP);
            EndPaint(value, &paint);
            return 0;
        }
        return DefWindowProcW(value, message, wparam, lparam);
    }
    HWND root_ = nullptr;
};

class Fixture {
public:
    explicit Fixture(QString directory)
        : directory_(std::move(directory)), controller_(std::make_shared<DesktopComputerController>()) {
        registerDesktopComputerTools(registry_, controller_);
        tool_ = registry_.findForModel("desktop_computer");
        context_.sessionId = "owned-pen-session-" + std::to_string(GetCurrentProcessId());
        context_.turnId = "owned-pen-turn-" + std::to_string(GetCurrentProcessId());
        context_.root = workspace_.path().toUtf8().toStdString();
        context_.cancel = &receiver_.canceled;
    }
    ~Fixture() {
        if (inputStarted_)
            run("destructor_owned_cleanup",
                [&] { return controller_->releaseInput(context_.sessionId, context_.turnId); });
    }
    bool prepare() {
        receiver_.move(170, 130);
        receiver_.show();
        if (!workspace_.isValid() || !tool_ || !receiver_.createSurface())
            return precondition("owned_fixture_unavailable");
        SetWindowPos(receiver_.root(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(receiver_.root());
        if (!waitFor([&] { return IsWindowVisible(receiver_.root()) && GetForegroundWindow() == receiver_.root(); },
                     500))
            return precondition("owned_foreground_not_acquired");
        if (!GetWindowRect(receiver_.root(), &initialBounds_)) return precondition("owned_bounds_unavailable");
        for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_CONTROL, VK_LCONTROL,
                              VK_RCONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
            if (GetAsyncKeyState(key) & 0x8000) {
                precondition_["external_held_key"] = key;
                return precondition("external_control_already_held");
            }
        SetFocus(receiver_.surface);
        prepared_ = true;
        if (!observe(false, "initial_ordinary_observe")) {
            precondition("owned_metadata_observation_failed");
            return false;
        }
        anchor_ = projected(.4F, .45F);
        moved_ = projected(.6F, .55F);
        if (!admitted(anchor_) || !admitted(moved_)) {
            prepared_ = false;
            return precondition("owned_point_gate_not_ready");
        }
        return precondition("ready", true);
    }
    void exercise() {
        POINT cursor{};
        if (!GetPhysicalCursorPos(&cursor) || !receiver_.owns(WindowFromPhysicalPoint(cursor))) {
            auto position = action("move");
            position.xRatio = .4;
            position.yRatio = .45;
            if (!ordinary(position, "owned_mouse_positioning")) return;
        }
        if (!observe(true, "tool_observe_before_ctrl")) return;
        auto control = action("key_down");
        control.key = "Ctrl";
        if (!ordinary(control, "ordinary_ctrl_down")) return;
        if (!observe(false, "ordinary_observe_before_hover")) return;
        primaryBegin_ = receiver_.raw.size();
        auto hover = pen(Phase::Hover);
        hover.pressureOrDistance = .25F;
        if (!batch(hover, true, false, "tool_hover")) return;
        if (!observe(false, "ordinary_observe_before_down")) return;
        auto down = pen(Phase::Down);
        if (!batch(down, false, false, "ordinary_down")) return;
        const auto heldAt = GetTickCount64();
        receiver_.phase = "hold_before_observe";
        dispatchFor(450);
        const auto beforeObserveUps = count(WM_POINTERUP, primaryBegin_);
        if (!observe(true, "tool_mid_hold_observe")) return;
        const auto held = controller_->inputState(context_.sessionId);
        check("observe_keeps_pen_and_ctrl_owned", held.penActive && !held.keysDown.empty());
        check("observe_does_not_emit_pen_up", count(WM_POINTERUP, primaryBegin_) == beforeObserveUps);
        receiver_.phase = "hold_after_observe";
        if (GetTickCount64() - heldAt < 1250) dispatchFor(int(1250 - (GetTickCount64() - heldAt)));
        check("contact_remains_held_over_one_second",
              count(WM_POINTERUP, primaryBegin_) == 0 && controller_->inputState(context_.sessionId).penActive);
        auto move = pen(Phase::Move);
        move.x = .6F;
        move.y = .55F;
        move.pressureOrDistance = .75F;
        move.tilt = 45;
        move.rotation = 90;
        if (!batch(move, true, false, "tool_move")) return;
        for (const auto bits : {1, 3, 2}) {
            if (!observe(bits != 3, ("observe_before_buttons_" + std::to_string(bits)).c_str())) return;
            auto button = pen(Phase::ButtonOnly);
            button.toolType = 0;
            button.buttons = std::uint8_t(bits);
            const auto begin = receiver_.raw.size();
            if (!batch(button, bits == 3, false, ("buttons_" + std::to_string(bits)).c_str())) return;
            dispatchFor(40);
            check(("barrel_held_after_logical_" + std::to_string(bits)).c_str(), hasBarrel(begin, true));
        }
        auto noButtons = pen(Phase::ButtonOnly);
        noButtons.toolType = 0;
        const auto noButtonsBegin = receiver_.raw.size();
        if (!batch(noButtons, true, true, "tool_buttons_zero_without_token")) return;
        dispatchFor(40);
        check("barrel_zero_releases_with_cached_position", hasBarrel(noButtonsBegin, false));
        plannedUp_ = GetTickCount64();
        const auto upBegin = receiver_.raw.size();
        if (!batch(pen(Phase::Up), false, true, "ordinary_up_without_token")) return;
        dispatchFor(60);
        check("up_retains_owned_hover_state", controller_->inputState(context_.sessionId).penActive);
        check("up_has_native_in_range_without_contact",
              std::any_of(receiver_.raw.begin() + upBegin, receiver_.raw.end(), [](const auto& event) {
                  return event.infoOk && (event.pen.pointerInfo.pointerFlags & POINTER_FLAG_INRANGE) &&
                         !(event.pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT);
              }));
        const auto leaveBegin = receiver_.raw.size();
        if (!batch(pen(Phase::HoverLeave), true, true, "tool_leave_without_token")) return;
        dispatchFor(80);
        check("leave_has_native_exit_and_no_hover_state",
              count(WM_POINTERLEAVE, leaveBegin) > 0 && !controller_->inputState(context_.sessionId).penActive);
        primaryEnd_ = receiver_.raw.size();
        checkPrimary();
        const auto ctrlUps = receiver_.controlUps;
        if (!release(true, "tool_release_all")) return;
        check("release_all_emits_ctrl_up", receiver_.controlUps > ctrlUps && !(GetKeyState(VK_CONTROL) & 0x8000));
        check("release_all_empty_native_state", controller_->inputState(context_.sessionId).empty());
        // Exercise cleanup while the pen is actually down, not only after UP.
        for (bool cancel : {false, true}) {
            if (!observe(cancel, cancel ? "tool_observe_before_cancel" : "ordinary_observe_before_eraser")) return;
            auto second = pen(Phase::Down);
            second.toolType = cancel ? 1 : 2;
            second.tilt = 0;
            second.rotation = 0;
            const auto begin = receiver_.raw.size();
            if (!batch(second, cancel, false, cancel ? "tool_down_for_cancel" : "ordinary_eraser_down")) return;
            dispatchFor(100);
            if (cancel) {
                if (!batch(pen(Phase::CancelAll), true, true, "tool_cancel_all_without_token")) return;
            } else if (!release(false, "ordinary_release_held_eraser"))
                return;
            dispatchFor(100);
            check(cancel ? "cancel_all_actual_up_and_empty" : "ordinary_release_actual_up_and_empty",
                  hasPair(begin, !cancel) && controller_->inputState(context_.sessionId).empty());
        }
    }
    int save() {
        if (inputStarted_) release(false, "final_owned_release");
        Json raw = Json::array();
        for (const auto& e : receiver_.raw) {
            const auto& p = e.pen.pointerInfo;
            raw.push_back({{"hwnd", identifier(e.hwnd)},
                           {"message", e.message},
                           {"wparam", e.wparam},
                           {"lparam", e.lparam},
                           {"phase", e.phase},
                           {"observed_ms", e.observed},
                           {"message_time", e.messageTime},
                           {"pointer_id", e.id},
                           {"type_ok", e.typeOk},
                           {"type", int(e.type)},
                           {"type_error", e.typeError},
                           {"pen_info_ok", e.infoOk},
                           {"pen_info_error", e.infoError},
                           {"frame_ok", e.frameOk},
                           {"frame_error", e.frameError},
                           {"frame_count", e.frameCount},
                           {"native_pointer_id", p.pointerId},
                           {"frame_id", p.frameId},
                           {"source_device", identifier(p.sourceDevice)},
                           {"native_target", identifier(p.hwndTarget)},
                           {"native_flags", p.pointerFlags},
                           {"native_time", p.dwTime},
                           {"performance_count", p.PerformanceCount},
                           {"native_point", point(p.ptPixelLocation)},
                           {"envelope_point", point(e.envelope)},
                           {"client_point", point(e.client)},
                           {"client_origin", point(e.origin)},
                           {"client_to_screen", point(e.roundtripScreen)},
                           {"roundtrip_ok", e.roundtrip},
                           {"pen_flags", e.pen.penFlags},
                           {"pen_mask", e.pen.penMask},
                           {"pressure", e.pen.pressure},
                           {"tilt_x", e.pen.tiltX},
                           {"tilt_y", e.pen.tiltY},
                           {"rotation", e.pen.rotation},
                           {"control", e.control}});
        }
        const auto final = controller_->inputState(context_.sessionId);
        const Json report{{"fixture", "product-controller-and-model-tool-pen"},
                          {"pid", GetCurrentProcessId()},
                          {"session_id", context_.sessionId},
                          {"turn_id", context_.turnId},
                          {"owned_window_id", identifier(receiver_.root())},
                          {"precondition_skip", !prepared_},
                          {"precondition", precondition_},
                          {"failures", failures_},
                          {"input_attempted", inputStarted_},
                          {"model_inference_used", false},
                          {"same_controller_and_owner", true},
                          {"continuous_qt_event_loop", true},
                          {"final_owned_state_empty", final.empty()},
                          {"interrupted", receiver_.canceled.load()},
                          {"native_hold_ms", nativeHold_},
                          {"observed_hold_ms", observedHold_},
                          {"primary_begin", primaryBegin_},
                          {"primary_end", primaryEnd_},
                          {"checks", checks_},
                          {"calls", calls_},
                          {"raw", raw}};
        QSaveFile file(QDir(directory_).filePath(QStringLiteral("pen-native-receipt.json")));
        const auto text = QByteArray::fromStdString(report.dump(2));
        const bool saved = file.open(QIODevice::WriteOnly) && file.write(text) == text.size() && file.commit();
        std::printf("Product Pen failures=%d skip=%d raw=%zu native_hold=%lu observed_hold=%llu\n", failures_,
                    !prepared_, receiver_.raw.size(), nativeHold_, observedHold_);
        return !saved ? 2 : !prepared_ ? 77 : failures_ ? 1 : 0;
    }

private:
    bool precondition(const char* reason, bool ready = false) {
        const auto hit = WindowFromPhysicalPoint(anchor_);
        DWORD pid = 0;
        if (hit) GetWindowThreadProcessId(hit, &pid);
        precondition_.update({{"reason", reason},
                              {"ready", ready},
                              {"hwnd", identifier(receiver_.root())},
                              {"surface", identifier(receiver_.surface)},
                              {"foreground", identifier(GetForegroundWindow())},
                              {"anchor", point(anchor_)},
                              {"anchor_hit", identifier(hit)},
                              {"anchor_pid", pid}});
        return ready;
    }
    bool admitted(POINT target) const {
        RECT bounds{};
        DWORD process = 0;
        GetWindowThreadProcessId(receiver_.root(), &process);
        return !receiver_.canceled && process == GetCurrentProcessId() && IsWindowVisible(receiver_.root()) &&
               GetForegroundWindow() == receiver_.root() && GetWindowRect(receiver_.root(), &bounds) &&
               EqualRect(&bounds, &initialBounds_) && receiver_.owns(WindowFromPhysicalPoint(target));
    }
    MaiError run(const char* label, const std::function<MaiError()>& function) {
        receiver_.phase = label;
        QEventLoop loop;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
            timedOut = true;
            receiver_.canceled = true;
        });
        MaiError result;
        const auto started = GetTickCount64();
        std::thread worker([&] {
            try {
                result = function();
            } catch (...) {
                result = {MaiErrorCode::Internal, "owned fixture operation threw"};
            }
            QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
        });
        watchdog.start(12000);
        loop.exec();
        watchdog.stop();
        worker.join();
        const auto state = controller_->inputState(context_.sessionId);
        calls_.push_back({{"operation", label},
                          {"started_ms", started},
                          {"finished_ms", GetTickCount64()},
                          {"code", int(result.code())},
                          {"error", result.message()},
                          {"watchdog", timedOut},
                          {"pen_active", state.penActive},
                          {"keys_down", state.keysDown},
                          {"devices", state.devices}});
        if (timedOut) check("operation_watchdog_not_fired", false);
        dispatchFor(15);
        return result;
    }
    void check(const char* name, bool ok) {
        checks_.push_back({{"name", name}, {"passed", ok}});
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures_;
    }
    bool observe(bool model, const char* label) {
        const auto error = run(label, [&] {
            if (model) {
                const auto result = tool_->execute(Json{{"action", "observe"},
                                                        {"window_id", std::to_string(identifier(receiver_.root()))},
                                                        {"include_elements", false}}
                                                       .dump(),
                                                   context_);
                if (result.hasError()) return result.error();
                snapshot_ = Json::parse(result.output(), nullptr, false);
                if (!result.images().empty())
                    return MaiError{MaiErrorCode::Protocol, "metadata tool observe returned images"};
            } else {
                const auto result = controller_->observe(
                    context_.sessionId, "", context_.root, false, identifier(receiver_.root()), false,
                    [&] { return receiver_.canceled.load(); }, context_.turnId);
                if (!result) return result.error();
                snapshot_ = Json::parse(result.value().json, nullptr, false);
                if (!result.value().imagePath.empty())
                    return MaiError{MaiErrorCode::Protocol, "metadata ordinary observe returned image"};
            }
            if (!snapshot_.is_object() ||
                snapshot_.value("window_id", std::string()) != std::to_string(identifier(receiver_.root())) ||
                snapshot_.value("process_id", std::uint64_t(0)) != GetCurrentProcessId() ||
                snapshot_.value("snapshot_id", std::string()).empty() || snapshot_.value("has_image", false) ||
                !snapshot_.contains("bounds") || !snapshot_["bounds"].is_object() ||
                snapshot_["bounds"].value("width", 0.0) <= 0 || snapshot_["bounds"].value("height", 0.0) <= 0)
                return MaiError{MaiErrorCode::Protocol, "observation did not bind this owned window"};
            return MaiError{};
        });
        check(label, !error);
        return !error;
    }
    POINT projected(float x, float y) const {
        const auto& bounds = snapshot_.at("bounds");
        return {LONG(std::llround(bounds.value("x", 0.0) + double(x) * (bounds.value("width", 0.0) - 1))),
                LONG(std::llround(bounds.value("y", 0.0) + double(y) * (bounds.value("height", 0.0) - 1)))};
    }
    DesktopComputerAction action(const char* type, bool cleanup = false) {
        DesktopComputerAction value;
        value.action = type;
        value.workspace = context_.root;
        value.turnId = context_.turnId;
        value.isCanceled = [&] { return receiver_.canceled.load(); };
        if (!cleanup) {
            value.windowId = identifier(receiver_.root());
            value.snapshotId = snapshot_.value("snapshot_id", std::string());
        }
        return value;
    }
    bool ordinary(DesktopComputerAction request, const char* label) {
        if (!admitted(anchor_)) {
            check("ordinary_owned_gate", false);
            receiver_.canceled = true;
            return false;
        }
        inputStarted_ = true;
        const auto error = run(label, [&] {
            const auto result = controller_->perform(context_.sessionId, request);
            if (!result) return result.error();
            if (!result.value().dispatched) return MaiError{MaiErrorCode::Protocol, "ordinary input did not dispatch"};
            return MaiError{};
        });
        check(label, !error);
        return !error;
    }
    static DesktopInputPen pen(Phase phase) {
        DesktopInputPen value;
        value.phase = phase;
        value.x = .4F;
        value.y = .45F;
        value.pressureOrDistance = .5F;
        value.tilt = 30;
        value.rotation = 270;
        return value;
    }
    static const char* phaseName(Phase phase) {
        switch (phase) {
            case Phase::Hover: return "hover";
            case Phase::Down: return "down";
            case Phase::Move: return "move";
            case Phase::Up: return "up";
            case Phase::ButtonOnly: return "button_only";
            case Phase::HoverLeave: return "hover_leave";
            case Phase::Cancel: return "cancel";
            case Phase::CancelAll: return "cancel_all";
        }
        return "invalid";
    }
    bool batch(const DesktopInputPen& value, bool model, bool cleanup, const char* label) {
        const bool positioned = value.phase == Phase::Hover || value.phase == Phase::Down || value.phase == Phase::Move;
        const auto target = positioned ? projected(value.x, value.y) : moved_;
        if (!cleanup && !admitted(target)) {
            check("pen_owned_gate", false);
            receiver_.canceled = true;
            return false;
        }
        inputStarted_ = true;
        const auto error = run(label, [&] {
            if (model) {
                Json event{{"type", "pen"},
                           {"phase", phaseName(value.phase)},
                           {"tool_type", value.toolType},
                           {"buttons", value.buttons}};
                if (positioned)
                    event.update({{"x_ratio", value.x},
                                  {"y_ratio", value.y},
                                  {"pressure_or_distance", value.pressureOrDistance},
                                  {"tilt", value.tilt},
                                  {"rotation", value.rotation}});
                Json request{{"action", "input_events"}, {"events", Json::array({event})}};
                if (!cleanup)
                    request.update({{"window_id", std::to_string(identifier(receiver_.root()))},
                                    {"snapshot_id", snapshot_.value("snapshot_id", std::string())}});
                const auto result = tool_->execute(request.dump(), context_);
                if (result.hasError()) return result.error();
                const auto body = Json::parse(result.output(), nullptr, false);
                if (!body.is_object() || !body.value("dispatched", false) || body.value("accepted_events", 0) != 1)
                    return MaiError{MaiErrorCode::Protocol, "tool input did not accept exactly one pen event"};
                return MaiError{};
            }
            auto request = action("input_events", cleanup);
            request.inputOptions.inheritExisting = true;
            DesktopInputEvent event;
            event.kind = Kind::Pen;
            event.pen = value;
            request.events = {event};
            const auto result = controller_->perform(context_.sessionId, request);
            if (!result) return result.error();
            if (!result.value().dispatched || result.value().acceptedEvents != 1)
                return MaiError{MaiErrorCode::Protocol, "ordinary input did not accept exactly one pen event"};
            return MaiError{};
        });
        check(label, !error);
        return !error;
    }
    bool release(bool model, const char* label) {
        MaiError error;
        const auto began = GetTickCount64();
        do {
            error = run(label, [&] {
                if (!model) return controller_->releaseInput(context_.sessionId, context_.turnId);
                const auto result = tool_->execute(R"({"action":"release_all"})", context_);
                return result.hasError() ? result.error() : MaiError{};
            });
            if (!error || error.code() != MaiErrorCode::Busy) break;
            dispatchFor(10);  // Cleanup is idempotent; mutation/token errors are never retried.
        } while (GetTickCount64() - began < 1000);
        check(label, !error);
        return !error;
    }
    unsigned count(UINT message, std::size_t begin) const {
        return unsigned(std::count_if(
            receiver_.raw.begin() + std::min(begin, receiver_.raw.size()), receiver_.raw.end(),
            [&](const auto& event) { return event.message == message && (!event.typeOk || event.type == PT_PEN); }));
    }
    bool hasBarrel(std::size_t begin, bool held) const {
        return std::any_of(receiver_.raw.begin() + std::min(begin, receiver_.raw.size()), receiver_.raw.end(),
                           [&](const auto& e) {
                               return e.infoOk && e.type == PT_PEN && e.message == WM_POINTERUPDATE &&
                                      (e.pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) &&
                                      bool(e.pen.penFlags & PEN_FLAG_BARREL) == held &&
                                      e.pen.pointerInfo.ptPixelLocation.x == moved_.x &&
                                      e.pen.pointerInfo.ptPixelLocation.y == moved_.y;
                           });
    }
    bool hasPair(std::size_t begin, bool eraser) const {
        const Raw* down = nullptr;
        const Raw* up = nullptr;
        for (std::size_t i = begin; i < receiver_.raw.size(); ++i) {
            const auto& e = receiver_.raw[i];
            if (!e.infoOk || e.type != PT_PEN) continue;
            if (e.message == WM_POINTERDOWN && !down) down = &e;
            if (e.message == WM_POINTERUP && !up) up = &e;
        }
        return down && up && down->pen.pointerInfo.pointerId == up->pen.pointerInfo.pointerId &&
               down->pen.pointerInfo.sourceDevice == up->pen.pointerInfo.sourceDevice &&
               (!eraser || (down->pen.penFlags & PEN_FLAG_ERASER));
    }
    void checkPrimary() {
        const Raw* down = nullptr;
        const Raw* up = nullptr;
        unsigned downs = 0, ups = 0;
        bool hover = false, moved = false, info = true, frame = true, coords = true, control = true;
        for (std::size_t i = primaryBegin_; i < primaryEnd_; ++i) {
            const auto& e = receiver_.raw[i];
            if (e.typeOk && e.type != PT_PEN) continue;
            if (e.message == WM_POINTERDOWN) {
                ++downs;
                if (!down) down = &e;
            }
            if (e.message == WM_POINTERUP) {
                ++ups;
                if (!up) up = &e;
            }
            if (e.infoOk && !down && (e.pen.pointerInfo.pointerFlags & POINTER_FLAG_INRANGE) &&
                !(e.pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT))
                hover = true;
            if (e.message != WM_POINTERDOWN && e.message != WM_POINTERUPDATE && e.message != WM_POINTERUP) continue;
            info = info && e.typeOk && e.type == PT_PEN && e.infoOk;
            frame = frame && e.frameOk && e.frameCount == 1;
            coords = coords && e.roundtrip && receiver_.owns(e.pen.pointerInfo.hwndTarget);
            control = control && e.control;
            if (e.infoOk && e.message == WM_POINTERUPDATE &&
                (e.pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) &&
                e.pen.pointerInfo.ptPixelLocation.x == moved_.x && e.pen.pointerInfo.ptPixelLocation.y == moved_.y &&
                e.pen.pressure == 768 && e.pen.tiltX == -45 && e.pen.tiltY == 0)
                moved = true;
        }
        const bool pair = down && up && down->infoOk && up->infoOk;
        if (pair) {
            nativeHold_ = DWORD(up->pen.pointerInfo.dwTime - down->pen.pointerInfo.dwTime);
            observedHold_ = up->observed - down->observed;
        }
        check("native_pen_raw_present", primaryEnd_ > primaryBegin_);
        check("single_native_down_and_up", downs == 1 && ups == 1);
        check("native_pen_type_info_and_single_frame", info && frame && pair);
        check("hover_before_contact", hover);
        check("down_at_expected_pressure_and_tilt", pair && down->pen.pressure == 512 && down->pen.tiltX == 30 &&
                                                        down->pen.tiltY == 0 &&
                                                        down->pen.pointerInfo.ptPixelLocation.x == anchor_.x &&
                                                        down->pen.pointerInfo.ptPixelLocation.y == anchor_.y);
        check("move_changes_pressure_tilt_and_position", moved);
        check("up_at_cached_move_position", pair && up->pen.pointerInfo.ptPixelLocation.x == moved_.x &&
                                                up->pen.pointerInfo.ptPixelLocation.y == moved_.y);
        check("same_native_pointer_and_source",
              pair && down->pen.pointerInfo.pointerId == up->pen.pointerInfo.pointerId &&
                  down->pen.pointerInfo.sourceDevice == up->pen.pointerInfo.sourceDevice);
        check("native_hold_over_one_second", pair && nativeHold_ >= 1000);
        check("observed_hold_over_one_second", pair && observedHold_ >= 1000);
        check("no_early_up_before_requested_up", pair && plannedUp_ && up->observed >= plannedUp_);
        check("event_owned_coordinates_roundtrip", coords && pair);
        check("ordinary_ctrl_survives_model_pen_events", control && pair);
        check("raw_record_not_truncated", !receiver_.truncated);
    }
    QString directory_;
    QTemporaryDir workspace_;
    Receiver receiver_;
    std::shared_ptr<DesktopComputerController> controller_;
    MaiToolRegistry registry_;
    MaiTool* tool_ = nullptr;
    MaiToolContext context_;
    RECT initialBounds_{};
    POINT anchor_{}, moved_{};
    bool prepared_ = false, inputStarted_ = false;
    int failures_ = 0;
    std::size_t primaryBegin_ = 0, primaryEnd_ = 0;
    ULONGLONG plannedUp_ = 0, observedHold_ = 0;
    DWORD nativeHold_ = 0;
    Json snapshot_, precondition_ = Json::object(), calls_ = Json::array(), checks_ = Json::array();
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
        QFileInfo::exists(QDir(directory).filePath(QStringLiteral("pen-native-receipt.json"))) ||
        !QDir().mkpath(directory))
        return 2;
    Fixture fixture(directory);
    if (fixture.prepare()) fixture.exercise();
    return fixture.save();
}
