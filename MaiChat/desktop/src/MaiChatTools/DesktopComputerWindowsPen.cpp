#include "MaiChatTools/DesktopComputerWindowsPenInternal.h"
#include "MaiChatTools/DesktopComputerWindowsPointerScope.h"

#if defined(_WIN32) && !defined(MAI_DESKTOP_PEN_NO_NATIVE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#include <Windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include "MaiThread.h"

namespace {
using Phase = DesktopInputPhase;
using Clock = std::chrono::steady_clock;
struct Model {
    DesktopWindowsPenPoint point;
    bool inRange = false, contact = false;
    std::uint8_t tool = 1, buttons = 0;
};
std::uint32_t toolFlags(const Model& model) {
    if (!model.inRange) return 0;
    // Windows erase intent is INVERTED in hover; ERASER is the contact
    // switch. UP clears that switch, while LEAVE clears every tool switch.
    const auto eraser = model.tool == 2 ? kDesktopPenInverted | (model.contact ? kDesktopPenEraser : 0) : 0;
    return eraser | (model.buttons ? kDesktopPenBarrel : 0);
}
struct Published {
    DesktopNativeInputState state;
    MaiError fault;
    std::optional<std::pair<std::int32_t, std::int32_t>> position;
};
MaiError invalid(const char* message) {
    return {MaiErrorCode::InvalidInput, message};
}
MaiError canceled() {
    return {MaiErrorCode::Canceled, "pen input canceled"};
}
MaiError busy() {
    return {MaiErrorCode::Busy, "another pen operation or cleanup is in flight"};
}
bool nativeNumber(double value) {
    return std::isfinite(value) && value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}
bool sameDesktop(const DesktopWindowsPenDesktop& a, const DesktopWindowsPenDesktop& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
MaiResult<Model> projected(const DesktopInputPen& input, const DesktopInputPort& port, Model model) {
    for (const auto value : {port.x, port.y, port.width, port.height, port.environmentX, port.environmentY,
                             port.environmentWidth, port.environmentHeight})
        if (!nativeNumber(value)) return invalid("pen viewport and desktop need finite native coordinates");
    for (const auto value : {port.environmentX, port.environmentY, port.environmentWidth, port.environmentHeight})
        if (std::floor(value) != value) return invalid("pen virtual desktop needs integral native bounds");
    if (port.width < 1 || port.height < 1 || port.environmentWidth < 1 || port.environmentHeight < 1 ||
        !nativeNumber(port.x + port.width - 1) || !nativeNumber(port.y + port.height - 1) ||
        !nativeNumber(port.environmentX + port.environmentWidth - 1) ||
        !nativeNumber(port.environmentY + port.environmentHeight - 1))
        return invalid("pen viewport or desktop endpoints are invalid");
    const auto x = std::llround(port.x + input.x * (port.width - 1));
    const auto y = std::llround(port.y + input.y * (port.height - 1));
    if (x < port.environmentX || y < port.environmentY || x > port.environmentX + port.environmentWidth - 1 ||
        y > port.environmentY + port.environmentHeight - 1)
        return invalid("pen point is outside the observed virtual desktop");
    model.point.x = static_cast<std::int32_t>(x);
    model.point.y = static_cast<std::int32_t>(y);
    model.point.desktop = {static_cast<std::int32_t>(port.environmentX), static_cast<std::int32_t>(port.environmentY),
                           static_cast<std::int32_t>(port.environmentWidth),
                           static_cast<std::int32_t>(port.environmentHeight)};
    model.inRange = true;
    model.contact = input.phase != Phase::Hover;
    model.buttons = input.buttons;
    if (input.toolType) model.tool = input.toolType;
    model.point.penFlags = toolFlags(model);
    model.point.hasPressure = model.contact && input.pressureOrDistance > 0;
    model.point.pressure = model.point.hasPressure ? std::uint32_t(std::lround(input.pressureOrDistance * 1024.0)) : 0;
    model.point.tiltX = model.point.tiltY = 0;
    if (input.tilt != 255 && input.rotation != 65535) {
        constexpr double pi = 3.14159265358979323846;
        const auto rotation = input.rotation % 360;
        const auto azimuth = -double(rotation) * pi / 180;
        const auto altitude = double(input.tilt) * pi / 180;
        const auto radial = std::sin(altitude);
        const auto vertical = input.tilt == 90 ? 0.0 : std::cos(altitude);
        // Snap cardinal components before atan2: tiny cos(pi/2) at a90-degree
        // tilt must not turn an orthogonal zero component into another90.
        const auto sx = rotation == 0 || rotation == 180 ? 0.0 : std::sin(azimuth);
        const auto sy = rotation == 90 || rotation == 270 ? 0.0 : std::cos(azimuth);
        model.point.tiltX = std::int32_t(std::lround(std::atan2(sx * radial, vertical) * 180 / pi));
        model.point.tiltY = std::int32_t(std::lround(std::atan2(sy * radial, vertical) * 180 / pi));
    }
    return model;
}
DesktopWindowsPenPoint packet(const Model& model, std::uint32_t action) {
    auto point = model.point;
    point.flags = action | (model.inRange ? kDesktopPenInRange : 0) | (model.contact ? kDesktopPenInContact : 0);
    return point;
}
class Admission {
public:
    explicit Admission(std::atomic<bool>& active) : active_(active) {
        bool expected = false;
        acquired_ = active_.compare_exchange_strong(expected, true);
    }
    ~Admission() {
        if (acquired_) active_ = false;
    }
    bool acquired() const {
        return acquired_;
    }

private:
    std::atomic<bool>& active_;
    bool acquired_ = false;
};
class Pen final : public DesktopComputerWindowsPen {
public:
    Pen(std::shared_ptr<DesktopWindowsPenTransport> transport, std::chrono::milliseconds refresh,
        DesktopWindowsPenReceiverValidator validator)
        : transport_(std::move(transport)), refresh_(refresh), validator_(std::move(validator)) {}
    ~Pen() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ++epoch_;
        }
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    MaiError dispatch(const DesktopInputPen& input, const DesktopInputPort& port,
                      std::function<bool()> isCanceled) override {
        return dispatchCommand(input, port, {}, std::move(isCanceled));
    }
    MaiError dispatchGuarded(const DesktopInputPen& input, const DesktopInputPort& port,
                            const DesktopNativeInput& receiverScope, std::function<bool()> isCanceled) override {
        auto owned = receiverScope;
        owned.isCanceled = {};
        owned.key.clear();
        owned.text.clear();
        return dispatchCommand(input, port, std::move(owned), std::move(isCanceled));
    }
    MaiError dispatchCommand(const DesktopInputPen& input, const DesktopInputPort& port,
                             std::optional<DesktopNativeInput> receiverScope, std::function<bool()> isCanceled) {
        const auto generation = epoch_.load();  // Stop cannot become this request's new generation.
        Admission admission(active_);
        if (!admission.acquired()) return busy();
        if (cleanupRequested_) return busy();
        DesktopInputEvent event;
        event.kind = DesktopInputEventKind::Pen;
        event.pen = input;
        if (const auto error = maiDesktopValidateInputEvents({event})) return error;
        if (receiverScope && !validator_ && !maiDesktopInputEventsOnlyRelease({event}))
            return {MaiErrorCode::NotSupported, "pen receiver validation is unavailable"};
        auto command = std::make_shared<Command>();
        command->input = input;
        command->port = port;
        command->cancel = std::move(isCanceled);
        command->receiverScope = std::move(receiverScope);
        command->epoch = generation;
        command->onlyRelease = maiDesktopInputEventsOnlyRelease({event});
        return execute(std::move(command));
    }
    DesktopNativeInputState state() const override {
        return std::atomic_load(&snapshot_)->state;
    }
    MaiError validate() const override {
        return std::atomic_load(&snapshot_)->fault;
    }
    std::optional<std::pair<std::int32_t, std::int32_t>> position() const override {
        return std::atomic_load(&snapshot_)->position;
    }
    MaiError releaseAll() override {
        ++epoch_;
        cleanupRequested_ = true;
        changed_.notify_all();
        Admission admission(active_);
        if (!admission.acquired()) return busy();
        auto command = std::make_shared<Command>();
        command->cleanup = true;
        command->epoch = epoch_.load();
        return execute(std::move(command));
    }

private:
    struct Command {
        DesktopInputPen input;
        DesktopInputPort port;
        std::function<bool()> cancel;
        std::optional<DesktopNativeInput> receiverScope;
        std::promise<MaiError> result;
        std::uint64_t epoch = 0;
        bool cleanup = false, receiverRejected = false, onlyRelease = false;
    };
    MaiError execute(std::shared_ptr<Command> command) {
        auto result = command->result.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
            pending_ = std::move(command);
        }
        changed_.notify_all();
        return result.get();
    }
    bool revoked(const Command& command) const {
        if (epoch_.load() != command.epoch) return true;
        if (command.onlyRelease) return false;  // A surviving report still needs a receiver, never caller permission.
        const bool requested = command.cancel && command.cancel();
        return requested || epoch_.load() != command.epoch;
    }
    MaiError validateReceiver(const DesktopWindowsPenPoint& point) const {
        if (!receiverScope_) return {};
        if (!validator_) return {MaiErrorCode::NotSupported, "pen receiver validation is unavailable"};
        return validator_(*receiverScope_, point.x, point.y);
    }
    void publish() {
        auto published = std::make_shared<Published>();
        published->fault = fault_;
        if (created_) {
            published->state.devices = {"windows_native_pen"};
            published->state.penActive = model_.inRange || model_.contact || model_.buttons || uncertain_;
            if (published->state.penActive) published->position = std::make_pair(model_.point.x, model_.point.y);
        }
        std::atomic_store(&snapshot_, std::shared_ptr<const Published>(std::move(published)));
    }
    MaiError send(const DesktopWindowsPenPoint& point, Command* command,
                  std::optional<std::uint64_t> generation = {}) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (generation && epoch_.load() != *generation) return canceled();
            if (command && revoked(*command)) return canceled();
            // Cleanup has neither a command nor a generation. Every new
            // command effect and autonomous report rechecks its exact point,
            // including retries after a native ERROR_NOT_READY result.
            if (command || generation) {
                if (const auto error = validateReceiver(point)) {
                    if (command) command->receiverRejected = true;
                    return error;
                }
                if (generation && epoch_.load() != *generation) return canceled();
                if (command && revoked(*command)) return canceled();
            }
            DesktopWindowsPenSubmission result;
            try {
                result = transport_->inject(point);
            } catch (...) {
                // The call may have reached User32 before throwing. Only this
                // attempted source/point is eligible for cleanup, never(0,0).
                const bool mayContact = model_.contact || (point.flags & (kDesktopPenInContact | kDesktopPenDown));
                model_.point = point;
                model_.contact = (point.flags & (kDesktopPenInContact | kDesktopPenDown)) != 0;
                model_.inRange = (point.flags & kDesktopPenInRange) != 0;
                if (model_.inRange) model_.tool = point.penFlags & (kDesktopPenInverted | kDesktopPenEraser) ? 2 : 1;
                uncertain_ = mayContact;
                throw;
            }
            if (result.kind == DesktopWindowsPenSubmitKind::Accepted) return {};
            if (result.kind == DesktopWindowsPenSubmitKind::Failed)
                return result.error ? result.error : MaiError{MaiErrorCode::Internal, "pen injection failed"};
            if (attempt == 2)
                return result.error ? result.error : MaiError{MaiErrorCode::Busy, "pen injection remained not ready"};
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {MaiErrorCode::Internal, "pen injection exhausted its retry budget"};
    }
    MaiError leave(bool destroy, bool cancel) {
        MaiError first;
        if (created_) {
            if (model_.contact || uncertain_) {
                auto released = model_;
                released.contact = false;
                released.inRange = true;
                released.buttons = 0;
                released.point.penFlags = toolFlags(released);
                released.point.hasPressure = false;
                released.point.pressure = 0;
                first = send(packet(released, kDesktopPenUp | (cancel ? kDesktopPenCanceled : 0)), nullptr);
                if (!first) model_ = released;
            }
            if (model_.inRange) {
                auto outside = model_;
                outside.contact = outside.inRange = false;
                outside.buttons = 0;
                outside.point.penFlags = toolFlags(outside);
                outside.point.hasPressure = false;
                outside.point.pressure = 0;
                const auto error = send(packet(outside, kDesktopPenUpdate), nullptr);
                if (!error) model_ = outside;
                if (!first) first = error;
            }
            if (destroy) {
                const auto error = transport_->destroy();  // Attempt even after UP/leave failure.
                if (!error) {
                    created_ = uncertain_ = false;
                    model_ = {};
                    receiverScope_.reset();
                }
                if (!first) first = error;
            }
        }
        if (destroy && !created_) uncertain_ = false;
        publish();
        return first;
    }
    MaiError cleanup(MaiError asynchronousFault = {}) {
        cleanupRequested_ = false;
        const auto cleanupEpoch = epoch_.load();
        // The writer can finish a notified cleanup before its public command
        // arrives. The same request must retain its real failure even if the
        // source is now empty; a new release epoch may retry and clear it.
        if (lastCleanupEpoch_ == cleanupEpoch && lastCleanupError_) return lastCleanupError_;
        // Preserve an asynchronous failure while leave publishes the now-empty
        // native state. Empty resources must never hide a known refresh fault.
        fault_ = asynchronousFault;
        const auto error = leave(true, true);
        fault_ = error ? error : asynchronousFault;
        lastCleanupEpoch_ = cleanupEpoch;
        lastCleanupError_ = error;
        publish();
        return error;
    }
    MaiError process(Command& command) {
        if (command.cleanup) return cleanup();
        const auto phase = command.input.phase;
        const bool release = command.input.buttons == 0 &&
                             (phase == Phase::Up || phase == Phase::ButtonOnly || phase == Phase::HoverLeave ||
                              phase == Phase::Cancel || phase == Phase::CancelAll);
        if (phase == Phase::Cancel || phase == Phase::CancelAll) return cleanup();
        if (phase == Phase::HoverLeave) {
            const auto error = leave(false, false);
            if (!error) return {};
            const auto released = cleanup();
            return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                            : error;
        }
        if (fault_) return fault_;
        if (phase == Phase::Up && (!created_ || !model_.inRange)) return {};
        if (!release && created_ && !maiDesktopWindowsPointerScopeMatches(receiverScope_, command.receiverScope))
            return {MaiErrorCode::Busy, "release the owned pen source before changing its receiver scope"};
        Model candidate = model_;
        std::uint32_t action = kDesktopPenUpdate;
        if (phase == Phase::Up) {
            candidate.contact = false;
            candidate.buttons = command.input.buttons;
            candidate.point.penFlags = toolFlags(candidate);
            candidate.point.hasPressure = false;
            candidate.point.pressure = 0;
            if (model_.contact)
                action = kDesktopPenUp;
            else if (model_.buttons == candidate.buttons)
                return {};
        } else if (phase == Phase::ButtonOnly) {
            if (!created_ || !model_.inRange)
                return command.input.buttons ? invalid("pen buttons need an owned in-range position") : MaiError{};
            candidate.buttons = command.input.buttons;
            if (!release && command.input.toolType) candidate.tool = command.input.toolType;
            candidate.point.penFlags = toolFlags(candidate);
        } else {
            const auto projected = ::projected(command.input, command.port, model_);
            if (!projected) return projected.error();
            candidate = projected.value();
            if (created_ && !sameDesktop(model_.point.desktop, candidate.point.desktop)) {
                const MaiError changed{MaiErrorCode::Busy,
                                       "pen virtual desktop changed; retire the owned source first"};
                const auto released = cleanup();
                return released
                           ? MaiError{changed.code(), changed.message() + "; cleanup failed: " + released.message()}
                           : changed;
            }
            action = candidate.contact ? (model_.contact ? kDesktopPenUpdate : kDesktopPenDown)
                                       : (model_.contact ? kDesktopPenUp : kDesktopPenUpdate);
        }
        if (!release && revoked(command)) {
            const auto released = cleanup();
            return released ? MaiError{MaiErrorCode::Canceled, "pen canceled; cleanup failed: " + released.message()}
                            : canceled();
        }
        if (!release) {
            // Only a new effect may bind a new receiver. A paired UP keeps
            // the old receiver for the hover reports that can follow it.
            receiverScope_ = std::move(command.receiverScope);
            if (const auto error = validateReceiver(candidate.point)) {
                const auto released = cleanup();
                return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                                : error;
            }
        } else if (candidate.inRange && action == kDesktopPenUpdate) {
            // A barrel-only release may keep contact/hover alive in UPDATE.
            // Retire the complete source if that surviving report loses its
            // receiver, while cached UP/leave remain independent cleanup.
            if (const auto error = validateReceiver(candidate.point)) {
                const auto released = cleanup();
                return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                                : error;
            }
        }
        const bool changedTool = candidate.tool != model_.tool;
        if (!release && created_ && model_.inRange && changedTool) {
            // A physical tool-role change crosses out-of-range. End an owned
            // stroke explicitly rather than changing its eraser bit in place.
            const auto error = leave(false, false);
            if (error) {
                const auto released = cleanup();
                return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                                : error;
            }
        }
        if (!created_) {
            creating_ = true;
            creatingModel_ = candidate;
            const auto error = transport_->create(1);
            creating_ = false;
            if (error) return error;
            created_ = true;
            publish();
        }
        if (!release && candidate.contact && !model_.inRange && (candidate.tool == 2 || changedTool)) {
            auto intent = candidate;
            intent.contact = false;
            intent.buttons = 0;
            intent.point.penFlags = toolFlags(intent);
            intent.point.hasPressure = false;
            intent.point.pressure = 0;
            // User32 must see the legal in-range tool selection before DOWN.
            // These are consecutive calls: no target ACK, timer or fixed wait.
            const auto error = send(packet(intent, kDesktopPenUpdate), &command);
            if (error) {
                if (error.code() != MaiErrorCode::Canceled && !command.receiverRejected) {
                    model_ = intent;
                    uncertain_ = false;  // A legal hover has no contact switch.
                }
                const auto released = cleanup();
                return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                                : error;
            }
            model_ = intent;
            publish();  // A canceled/failed DOWN must clean this accepted hover.
        }
        action = candidate.contact ? (model_.contact ? kDesktopPenUpdate : kDesktopPenDown)
                                   : (model_.contact ? kDesktopPenUp : kDesktopPenUpdate);
        const bool survivingReport = release && candidate.inRange && action == kDesktopPenUpdate;
        const auto error = send(packet(candidate, action), !release || survivingReport ? &command : nullptr);
        if (error) {
            // A failed system call is not an accepted new state. Retain its
            // attempted geometry conservatively only for owned cleanup/retry.
            if (error.code() != MaiErrorCode::Canceled && !command.receiverRejected) {
                model_ = candidate;
                uncertain_ = true;
            }
            const auto released = cleanup();
            return released ? MaiError{error.code(), error.message() + "; cleanup failed: " + released.message()}
                            : error;
        }
        model_ = candidate;
        publish();
        if (!release && revoked(command)) {
            const auto released = cleanup();
            return released ? MaiError{MaiErrorCode::Canceled, "pen canceled; cleanup failed: " + released.message()}
                            : canceled();
        }
        nextRefresh_ = Clock::now() + refresh_;
        return {};
    }
    void work() {
        MaiThread::setCurrentName("mai-input-pen");
        for (;;) {
            std::shared_ptr<Command> command;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const auto ready = [this] { return stopping_ || pending_ || cleanupRequested_.load(); };
                if (!ready()) {
                    if (created_ && model_.inRange && refresh_.count() > 0 && !fault_)
                        changed_.wait_until(lock, nextRefresh_, ready);
                    else
                        changed_.wait(lock, ready);
                }
                if (stopping_) break;
                command = std::move(pending_);
            }
            if (command) {
                MaiError error;
                try {
                    error = process(*command);
                    if (epoch_.load() != command->epoch || cleanupRequested_) {
                        const auto released = cleanup();
                        if (!error) error = released ? released : canceled();
                    }
                } catch (...) {
                    if (creating_) {
                        creating_ = false;
                        created_ = uncertain_ = true;
                        model_ = creatingModel_;
                    }
                    try {
                        cleanup();
                    } catch (...) {
                        publish();
                    }
                    error = {MaiErrorCode::Internal, "pen transport operation threw"};
                }
                // The synchronous caller's closure/token must die before its
                // response, including cancellation and transport exceptions.
                command->cancel = {};
                command->result.set_value(error);
            } else {
                const auto generation = epoch_.load();
                Admission admission(active_);
                if (!admission.acquired()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                try {
                    if (cleanupRequested_)
                        cleanup();
                    else if (created_ && model_.inRange && !fault_ && refresh_.count() > 0 &&
                             Clock::now() >= nextRefresh_) {
                        const auto error = send(packet(model_, kDesktopPenUpdate), nullptr, generation);
                        if (error || epoch_.load() != generation || cleanupRequested_) {
                            cleanup(error);
                        }
                        nextRefresh_ = Clock::now() + refresh_;
                    }
                } catch (...) {
                    const MaiError failure{MaiErrorCode::Internal, "pen refresh transport threw"};
                    fault_ = failure;
                    try {
                        cleanup(failure);
                    } catch (...) {
                        publish();
                    }
                }
            }
        }
        try {
            cleanup();
        } catch (...) {
        }
    }
    std::shared_ptr<DesktopWindowsPenTransport> transport_;
    const std::chrono::milliseconds refresh_;
    const DesktopWindowsPenReceiverValidator validator_;
    std::optional<DesktopNativeInput> receiverScope_;
    std::atomic<bool> active_{false}, cleanupRequested_{false};
    std::atomic<std::uint64_t> epoch_{0};
    mutable std::shared_ptr<const Published> snapshot_ = std::make_shared<const Published>();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    std::shared_ptr<Command> pending_;
    bool stopping_ = false, created_ = false, uncertain_ = false, creating_ = false;
    Model model_, creatingModel_;
    MaiError fault_;
    std::uint64_t lastCleanupEpoch_ = 0;
    MaiError lastCleanupError_;
    Clock::time_point nextRefresh_;
};

#if defined(_WIN32) && !defined(MAI_DESKTOP_PEN_NO_NATIVE)
class NativeTransport final : public DesktopWindowsPenTransport {
public:
    ~NativeTransport() override {
        destroy();
        if (module_) FreeLibrary(module_);
    }
    MaiError create(std::uint32_t capacity) override {
        if (capacity != 1) return invalid("system pen supports exactly one pointer");
        if (device_) return {};
        if (!module_) {
            module_ = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module_) {
                create_ = reinterpret_cast<Create>(GetProcAddress(module_, "CreateSyntheticPointerDevice"));
                inject_ = reinterpret_cast<Inject>(GetProcAddress(module_, "InjectSyntheticPointerInput"));
                destroy_ = reinterpret_cast<Destroy>(GetProcAddress(module_, "DestroySyntheticPointerDevice"));
                dpi_ = reinterpret_cast<Dpi>(GetProcAddress(module_, "SetThreadDpiAwarenessContext"));
            }
        }
        if (!create_ || !inject_ || !destroy_)
            return {MaiErrorCode::NotSupported, "system pen injection requires Windows10 1809 or later"};
        const auto previous = dpi_ ? dpi_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
        device_ = create_(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        const auto error = device_ ? 0 : GetLastError();
        if (previous) dpi_(previous);
        return device_ ? MaiError{}
                       : MaiError{MaiErrorCode::NotConfigured,
                                  "could not create system pen source: " + std::to_string(error)};
    }
    DesktopWindowsPenSubmission inject(const DesktopWindowsPenPoint& point) override {
        if (!device_)
            return {DesktopWindowsPenSubmitKind::Failed, {MaiErrorCode::NotConfigured, "system pen source is closed"}};
        const auto previous = dpi_ ? dpi_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
        const DesktopWindowsPenDesktop current{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                               GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                               GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        const auto position = maiDesktopWindowsPenApiPosition(point, current);
        if (!position) {
            if (previous) dpi_(previous);
            return {DesktopWindowsPenSubmitKind::Failed, position.error()};
        }
        POINTER_TYPE_INFO input{};
        input.type = PT_PEN;
        input.penInfo.pointerInfo.pointerType = PT_PEN;
        input.penInfo.pointerInfo.pointerId = point.pointerId;
        input.penInfo.pointerInfo.pointerFlags = point.flags;
        input.penInfo.pointerInfo.ptPixelLocation = {position.value().x, position.value().y};
        input.penInfo.penFlags = point.penFlags;
        input.penInfo.penMask = PEN_MASK_TILT_X | PEN_MASK_TILT_Y | (point.hasPressure ? PEN_MASK_PRESSURE : 0);
        input.penInfo.pressure = point.pressure;
        input.penInfo.tiltX = point.tiltX;
        input.penInfo.tiltY = point.tiltY;
        const bool accepted = inject_(device_, &input, 1) != FALSE;
        const auto error = accepted ? 0 : GetLastError();
        if (previous) dpi_(previous);
        if (accepted) return {};
        return {error == ERROR_NOT_READY ? DesktopWindowsPenSubmitKind::Retry : DesktopWindowsPenSubmitKind::Failed,
                {error == ERROR_NOT_READY ? MaiErrorCode::Busy : MaiErrorCode::Internal,
                 "system pen injection failed: " + std::to_string(error)}};
    }
    MaiError destroy() override {
        if (device_) {
            destroy_(device_);
            device_ = nullptr;
        }
        return {};
    }

private:
    using Create = HSYNTHETICPOINTERDEVICE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using Inject = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
    using Destroy = void(WINAPI*)(HSYNTHETICPOINTERDEVICE);
    using Dpi = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE module_ = nullptr;
    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    Create create_ = nullptr;
    Inject inject_ = nullptr;
    Destroy destroy_ = nullptr;
    Dpi dpi_ = nullptr;
};
#else
class NativeTransport final : public DesktopWindowsPenTransport {
public:
    MaiError create(std::uint32_t) override {
        return {MaiErrorCode::NotSupported, "Windows system pen is unavailable on this platform"};
    }
    DesktopWindowsPenSubmission inject(const DesktopWindowsPenPoint&) override {
        return {DesktopWindowsPenSubmitKind::Failed, {MaiErrorCode::NotSupported, "Windows system pen is unavailable"}};
    }
    MaiError destroy() override {
        return {};
    }
};
#endif
}  // namespace

std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPenWithTransport(
    std::shared_ptr<DesktopWindowsPenTransport> transport, std::chrono::milliseconds refreshInterval) {
    return makeDesktopComputerWindowsPenWithTransport(std::move(transport), refreshInterval, {});
}
std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPenWithTransport(
    std::shared_ptr<DesktopWindowsPenTransport> transport, std::chrono::milliseconds refreshInterval,
    DesktopWindowsPenReceiverValidator validator) {
    return transport && refreshInterval.count() >= 0 &&
                   refreshInterval.count() <= std::numeric_limits<std::int32_t>::max()
               ? std::make_shared<Pen>(std::move(transport), refreshInterval, std::move(validator))
               : nullptr;
}
std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPen() {
    return makeDesktopComputerWindowsPenWithTransport(std::make_shared<NativeTransport>());
}
std::shared_ptr<DesktopComputerWindowsPen> makeDesktopComputerWindowsPen(DesktopWindowsPenReceiverValidator validator) {
    return makeDesktopComputerWindowsPenWithTransport(std::make_shared<NativeTransport>(), std::chrono::milliseconds(16),
                                                      std::move(validator));
}
MaiResult<DesktopWindowsPenApiPosition> maiDesktopWindowsPenApiPosition(
    const DesktopWindowsPenPoint& point, const DesktopWindowsPenDesktop& currentDesktop) {
    const auto& cached = point.desktop;
    const auto right = std::int64_t(cached.x) + cached.width - 1;
    const auto bottom = std::int64_t(cached.y) + cached.height - 1;
    if (cached.width < 1 || cached.height < 1 || right > std::numeric_limits<std::int32_t>::max() ||
        bottom > std::numeric_limits<std::int32_t>::max())
        return invalid("cached pen virtual desktop bounds are invalid");
    if (!sameDesktop(cached, currentDesktop))
        return {MaiErrorCode::Busy, "pen virtual desktop geometry changed before injection"};
    const auto x = std::int64_t(point.x) - cached.x;
    const auto y = std::int64_t(point.y) - cached.y;
    if (x < 0 || y < 0 || x >= cached.width || y >= cached.height)
        return invalid("pen point is outside the cached virtual desktop");
    return DesktopWindowsPenApiPosition{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
}
