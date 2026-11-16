// Windows touchscreen source reports, independent of a HID SDK. A resident
// writer introduces interactive contacts through its normal report cycle;
// cleanup refreshes/cancels only this source's native handle and slots.
#if defined(_WIN32)
#if !defined(WINVER) || WINVER < 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#if !defined(NTDDI_VERSION) || NTDDI_VERSION < 0x0A000006
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "MaiChatTools/DesktopComputerWindowsTouchInternal.h"
#include "MaiThread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <future>
#include <optional>
#include <mutex>
#include <set>
#include <thread>

namespace {
using Phase = DesktopInputPhase;
constexpr std::size_t maxContacts = 10;

struct Environment {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};

struct HeldContact {
    std::uint32_t slot = 0;
    std::int32_t screenX = 0, screenY = 0;
    std::int64_t left = 0, top = 0, right = 0, bottom = 0;
    std::uint32_t pressure = 0, orientation = 0;
    bool touching = false, hasPressure = false, hasOrientation = false, hasArea = false;
    // Possible native positions belong to this contact, so an accepted UP
    // retires its uncertainty rather than leaving an unrelated parent cache.
    std::vector<std::pair<std::int32_t, std::int32_t>> uncertainPositions;
};
using Contacts = std::map<std::uint32_t, HeldContact>;

MaiError invalid(const char* message) { return {MaiErrorCode::InvalidInput, message}; }

bool nativeCoordinate(double value) {
    return std::isfinite(value) && value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

MaiResult<Environment> environment(const DesktopInputPort& port) {
    if (!nativeCoordinate(port.environmentX) || !nativeCoordinate(port.environmentY) ||
        !nativeCoordinate(port.environmentWidth) || !nativeCoordinate(port.environmentHeight) ||
        port.environmentWidth < 1 || port.environmentHeight < 1 ||
        std::floor(port.environmentX) != port.environmentX || std::floor(port.environmentY) != port.environmentY ||
        std::floor(port.environmentWidth) != port.environmentWidth || std::floor(port.environmentHeight) != port.environmentHeight)
        return invalid("touch virtual desktop needs finite integral native bounds");
    const auto right = port.environmentX + port.environmentWidth - 1;
    const auto bottom = port.environmentY + port.environmentHeight - 1;
    if (!nativeCoordinate(right) || !nativeCoordinate(bottom)) return invalid("touch virtual desktop endpoints overflow native coordinates");
    return Environment{static_cast<std::int32_t>(port.environmentX), static_cast<std::int32_t>(port.environmentY),
        static_cast<std::int32_t>(port.environmentWidth), static_cast<std::int32_t>(port.environmentHeight)};
}

MaiResult<HeldContact> project(const DesktopInputTouch& input, const DesktopInputPort& port,
                               const Environment& desktop, std::uint32_t slot) {
    if (!nativeCoordinate(port.x) || !nativeCoordinate(port.y) || !nativeCoordinate(port.width) ||
        !nativeCoordinate(port.height) || port.width < 1 || port.height < 1 ||
        !nativeCoordinate(port.x + port.width - 1) || !nativeCoordinate(port.y + port.height - 1))
        return invalid("touch viewport needs finite native bounds of at least one pixel");
    const auto x = std::llround(port.x + input.x * (port.width - 1));
    const auto y = std::llround(port.y + input.y * (port.height - 1));
    if (x < desktop.x || y < desktop.y || x >= std::int64_t(desktop.x) + desktop.width ||
        y >= std::int64_t(desktop.y) + desktop.height) return invalid("touch point is outside the observed virtual desktop");
    HeldContact result;
    result.slot = slot;
    result.screenX = static_cast<std::int32_t>(x);
    result.screenY = static_cast<std::int32_t>(y);
    result.touching = input.phase != Phase::Hover;
    result.hasPressure = !result.touching || input.pressureOrDistance > 0;
    result.pressure = !result.touching ? 0 : input.pressureOrDistance > 0 ?
        static_cast<std::uint32_t>(std::lround(input.pressureOrDistance * 1024.0)) : 512;
    result.hasOrientation = input.rotation != 0xffff;
    // The protocol measures clockwise from vertical; Win32 measures from +X.
    // Ellipse axes are symmetric, but retaining the full native angle avoids
    // turning an unknown orientation into a reported zero-degree measurement.
    result.orientation = result.hasOrientation ? (input.rotation % 360 + 270) % 360 : 0;
    result.left = result.right = x;
    result.top = result.bottom = y;
    const double major = input.contactAreaMajor > 0 ? input.contactAreaMajor : input.contactAreaMinor;
    const double minor = input.contactAreaMinor > 0 ? input.contactAreaMinor : major;
    result.hasArea = major > 0 && minor > 0;
    if (result.hasArea) {
        constexpr double pi = 3.14159265358979323846;
        const auto angle = result.hasOrientation ? (input.rotation % 360) * pi / 180 : pi / 4;
        const auto width = std::hypot(major * port.width * std::sin(angle), minor * port.width * std::cos(angle));
        const auto height = std::hypot(major * port.height * std::cos(angle), minor * port.height * std::sin(angle));
        result.left = (std::max)(std::int64_t(desktop.x), static_cast<std::int64_t>(std::floor(x - width / 2)));
        result.top = (std::max)(std::int64_t(desktop.y), static_cast<std::int64_t>(std::floor(y - height / 2)));
        result.right = (std::min)(std::int64_t(desktop.x) + desktop.width,
            (std::max)(result.left + 1, static_cast<std::int64_t>(std::ceil(x + width / 2))));
        result.bottom = (std::min)(std::int64_t(desktop.y) + desktop.height,
            (std::max)(result.top + 1, static_cast<std::int64_t>(std::ceil(y + height / 2))));
    }
    return result;
}

DesktopWindowsTouchPoint nativePoint(const HeldContact& contact, const Environment& desktop, std::uint32_t flags) {
    DesktopWindowsTouchPoint result;
    result.pointerId = contact.slot;
    result.flags = flags;
    result.screenX = contact.screenX;
    result.screenY = contact.screenY;
    result.x = static_cast<std::int32_t>(std::int64_t(contact.screenX) - desktop.x);
    result.y = static_cast<std::int32_t>(std::int64_t(contact.screenY) - desktop.y);
    // A valid center can survive a virtual-desktop rebase while the cached
    // contact box crosses its new edge. Clip the box before native narrowing.
    result.left = static_cast<std::int32_t>((std::max)(std::int64_t(desktop.x), contact.left) - desktop.x);
    result.top = static_cast<std::int32_t>((std::max)(std::int64_t(desktop.y), contact.top) - desktop.y);
    result.right = static_cast<std::int32_t>((std::min)(std::int64_t(desktop.x) + desktop.width, contact.right) - desktop.x);
    result.bottom = static_cast<std::int32_t>((std::min)(std::int64_t(desktop.y) + desktop.height, contact.bottom) - desktop.y);
    result.pressure = contact.pressure;
    result.orientation = contact.orientation;
    result.hasPressure = contact.hasPressure;
    result.hasOrientation = contact.hasOrientation;
    result.hasArea = contact.hasArea;
    return result;
}

DesktopWindowsTouchFrame frame(const Environment& desktop) {
    DesktopWindowsTouchFrame result;
    result.virtualX = desktop.x;
    result.virtualY = desktop.y;
    result.virtualWidth = desktop.width;
    result.virtualHeight = desktop.height;
    return result;
}

std::uint32_t restingFlags(const HeldContact& contact) {
    return kDesktopTouchUpdate | kDesktopTouchInRange | (contact.touching ? kDesktopTouchInContact : 0);
}

struct TouchPublished {
    DesktopNativeInputState state;
    std::vector<std::pair<std::int32_t, std::int32_t>> positions;
    MaiError fault;
};
struct TouchStep {
    Contacts next;
    Environment desktop;
    DesktopWindowsTouchFrame frame;
    bool expands = false;
};
struct TouchPlan {
    std::vector<TouchStep> steps;
    bool retireLayout = false;
};
bool sameEnvironment(const Environment& a, const Environment& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
bool touching(const Contacts& contacts) {
    return std::any_of(contacts.begin(), contacts.end(), [](const auto& entry) { return entry.second.touching; });
}

class TouchFrames final : public DesktopComputerWindowsTouch {
public:
    TouchFrames(std::shared_ptr<DesktopWindowsTouchTransport> transport, std::chrono::milliseconds interval)
        : transport_(std::move(transport)), interval_((std::max)(std::chrono::milliseconds(0), interval)) {
        static std::atomic<std::uint64_t> sequence{0};
        sourceName_ = "windows-touch-" + std::to_string(++sequence);
    }
    ~TouchFrames() override {
        stopping_ = true;
        ++epoch_;
        cleanupRequested_ = true;
        changed_.notify_all();
        if (writer_.joinable()) writer_.join();
    }
    MaiError dispatch(const std::vector<DesktopInputTouch>& inputs, const DesktopInputPort& port,
                      std::function<bool()> isCanceled) override {
        const auto generation = epoch_.load();
        PublicCall call(admitted_);
        if (!call.accepted) return busy();
        if (cleanupRequested_) return busy();
        DesktopInputEvent request;
        request.kind = DesktopInputEventKind::Touch;
        request.contacts = inputs;
        if (const auto error = maiDesktopValidateInputEvents({request})) return error;
        auto command = std::make_shared<Command>();
        command->inputs = inputs;
        command->port = port;
        command->cancel = std::move(isCanceled);
        command->epoch = generation;
        command->onlyRelease = maiDesktopInputEventsOnlyRelease({request});
        return execute(std::move(command));
    }
    DesktopNativeInputState state() const override {
        return std::atomic_load(&published_)->state;
    }
    std::vector<std::pair<std::int32_t, std::int32_t>> positions() const override {
        return std::atomic_load(&published_)->positions;
    }
    MaiError validate() const override {
        return std::atomic_load(&published_)->fault;
    }
    MaiError releaseAll() override {
        // Queue scheduling and background admission share this short lock. No
        // native call or caller callback holds it, so Stop cannot wait on I/O.
        std::unique_lock<std::mutex> lock(queueMutex_);
        const auto generation = ++epoch_;  // Revoke before checking admission.
        cleanupRequested_ = true;
        PublicCall call(admitted_);
        lock.unlock();
        changed_.notify_all();
        if (!call.accepted) return busy();
        auto command = std::make_shared<Command>();
        command->cleanup = true;
        command->epoch = generation;
        return execute(std::move(command));
    }

private:
    using Clock = std::chrono::steady_clock;
    struct PublicCall {
        explicit PublicCall(std::atomic<bool>& value) : value(value) {
            bool expected = false;
            accepted = value.compare_exchange_strong(expected, true);
        }
        ~PublicCall() {
            if (accepted) value = false;
        }
        std::atomic<bool>& value;
        bool accepted = false;
    };
    struct Command {
        std::vector<DesktopInputTouch> inputs;
        DesktopInputPort port;
        std::function<bool()> cancel;
        std::promise<MaiError> response;
        std::uint64_t epoch = 0;
        bool cleanup = false, onlyRelease = false;
    };
    static MaiError busy() {
        return {MaiErrorCode::Busy, "touch source work or cleanup is in flight"};
    }
    static MaiError canceled() {
        return {MaiErrorCode::Canceled, "touch input canceled"};
    }

    MaiError execute(std::shared_ptr<Command> command) {
        auto result = command->response.get_future();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!writer_.joinable()) writer_ = std::thread([this] { work(); });
            pending_ = std::move(command);
        }
        changed_.notify_all();
        return result.get();
    }
    bool revoked(const Command* command, std::uint64_t generation) {
        if (epoch_.load() != generation || stopping_) return true;
        if (!command || command->onlyRelease) return false;
        const bool result = command->cancel && command->cancel();
        return result || epoch_.load() != generation || stopping_;
    }
    void publish() {
        auto value = std::make_shared<TouchPublished>();
        value->fault = fault_;
        if (created_) {
            value->state.devices = {sourceName_};
            for (const auto& entry : contacts_) {
                value->state.touchPointers.push_back(entry.first);
                value->positions.emplace_back(entry.second.screenX, entry.second.screenY);
                for (const auto& point : entry.second.uncertainPositions)
                    if (std::find(value->positions.begin(), value->positions.end(), point) == value->positions.end())
                        value->positions.push_back(point);
            }
        }
        std::atomic_store(&published_, std::shared_ptr<const TouchPublished>(std::move(value)));
    }

    MaiResult<TouchPlan> preflight(const Command& command) const {
        TouchPlan plan;
        auto simulated = contacts_;
        auto desktop = desktop_;
        for (const auto& input : command.inputs) {
            const auto before = simulated;
            std::map<std::uint32_t, bool> ending;
            bool expands = false;
            if (input.phase == Phase::CancelAll) {
                for (const auto& entry : simulated) ending[entry.first] = true;
                simulated.clear();
            } else if (input.phase == Phase::Up || input.phase == Phase::Cancel || input.phase == Phase::HoverLeave) {
                const auto held = simulated.find(input.pointerId);
                if (held == simulated.end()) continue;
                ending[input.pointerId] = input.phase == Phase::Cancel;
                simulated.erase(held);
            } else {
                const auto bounds = environment(command.port);
                if (!bounds) return bounds.error();
                if (created_ && !contacts_.empty() && !sameEnvironment(bounds.value(), desktop_))
                    plan.retireLayout = true;
                desktop = bounds.value();
                const auto old = simulated.find(input.pointerId);
                std::uint32_t slot = old == simulated.end() ? 0 : old->second.slot;
                if (old == simulated.end()) {
                    std::array<bool, maxContacts> used{};
                    for (const auto& entry : simulated) used[entry.second.slot] = true;
                    while (slot < maxContacts && used[slot]) ++slot;
                    if (slot == maxContacts) return busy();
                }
                const auto projected = project(input, command.port, desktop, slot);
                if (!projected) return projected.error();
                expands = old == simulated.end() || (!old->second.touching && projected.value().touching);
                simulated[input.pointerId] = projected.value();
            }
            if (before.empty() && simulated.empty()) continue;
            TouchStep step;
            step.next = simulated;
            step.desktop = desktop;
            step.expands = expands;
            step.frame = frame(desktop);
            if (!plan.retireLayout) {
                for (const auto& old : before) {
                    const auto end = ending.find(old.first);
                    if (end != ending.end())
                        step.frame.points.push_back(
                            nativePoint(old.second, desktop,
                                        (old.second.touching ? kDesktopTouchUp : kDesktopTouchUpdate) |
                                            (end->second ? kDesktopTouchCanceled : 0)));
                }
                for (const auto& entry : simulated) {
                    const auto old = before.find(entry.first);
                    auto flags = restingFlags(entry.second);
                    if (old == before.end() || (!old->second.touching && entry.second.touching))
                        flags =
                            kDesktopTouchInRange |
                            (entry.second.touching ? kDesktopTouchDown | kDesktopTouchInContact : kDesktopTouchUpdate);
                    else if (old->second.touching && !entry.second.touching)
                        flags = kDesktopTouchUp | kDesktopTouchInRange;
                    step.frame.points.push_back(nativePoint(entry.second, desktop, flags));
                }
                for (const auto& point : step.frame.points)
                    if (point.x < 0 || point.y < 0 || point.x >= desktop.width || point.y >= desktop.height)
                        return invalid("owned touch lies outside the observed virtual desktop");
            }
            plan.steps.push_back(std::move(step));
        }
        return plan;
    }

    MaiError submit(DesktopWindowsTouchFrame value, const Command* command,
                    std::optional<std::uint64_t> generation = {}) {
        std::sort(value.points.begin(), value.points.end(), [](const auto& a, const auto& b) { return a.pointerId < b.pointerId; });
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (generation && revoked(command, *generation)) return canceled();
            const auto result = transport_->inject(value);
            switch (result.kind) {
            case DesktopWindowsTouchSubmitKind::Accepted: return {};
            case DesktopWindowsTouchSubmitKind::Failed:
                return result.error ? result.error : MaiError{MaiErrorCode::Internal, "native touch frame rejected"};
            case DesktopWindowsTouchSubmitKind::Retry:
                if (attempt != 7) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                break;
            default: return {MaiErrorCode::Internal, "unknown native touch submission status"};
            }
        }
        return {MaiErrorCode::Busy, "native touch frame remained not ready after eight identical attempts"};
    }
    MaiError open(const Command& command) {
        if (created_) return {};
        if (revoked(&command, command.epoch)) return canceled();
        created_ = true;  // Includes a possible native create that throws after acquiring its handle.
        publish();
        const auto error = transport_->create(maxContacts);
        if (error) return error;
        if (revoked(&command, command.epoch)) return canceled();
        return {};
    }
    MaiError report(const Command* command, std::uint64_t generation) {
        if (!created_ || contacts_.empty()) return {};
        auto update = frame(desktop_);
        for (const auto& entry : contacts_)
            update.points.push_back(nativePoint(entry.second, desktop_, restingFlags(entry.second)));
        if (const auto error = submit(std::move(update), command, generation)) return error;
        if (epoch_.load() != generation || stopping_) return canceled();
        seedPending_ = false;
        nextReport_ = Clock::now() + interval_;
        return {};
    }
    MaiError finishSeedReport(const Command& command) {
        // This is the ordinary source reporting clock. It is not a target-window
        // acknowledgment or a documented Windows delivery fence.
        while (interval_.count() > 0 && Clock::now() < nextReport_) {
            if (revoked(&command, command.epoch)) return canceled();
            std::unique_lock<std::mutex> lock(queueMutex_);
            changed_.wait_until(lock, (std::min)(nextReport_, Clock::now() + std::chrono::milliseconds(5)), [&] {
                return stopping_ || cleanupRequested_.load() || epoch_.load() != command.epoch;
            });
        }
        if (revoked(&command, command.epoch)) return canceled();
        return report(&command, command.epoch);
    }
    void rememberUncertain(const TouchStep& step) {
        for (const auto& entry : step.next) {
            const auto found = contacts_.find(entry.first);
            if (found == contacts_.end())
                contacts_[entry.first] = entry.second;
            else if (found->second.screenX != entry.second.screenX || found->second.screenY != entry.second.screenY)
                found->second.uncertainPositions.emplace_back(entry.second.screenX, entry.second.screenY);
        }
        publish();
    }
    MaiError retire(bool clearFault) {
        const auto generation = epoch_.load();
        MaiError first;
        if (created_) {
            if (!contacts_.empty()) {
                auto cancel = frame(desktop_);
                for (const auto& entry : contacts_)
                    cancel.points.push_back(nativePoint(
                        entry.second, desktop_,
                        kDesktopTouchCanceled | (entry.second.touching ? kDesktopTouchUp : kDesktopTouchUpdate)));
                try {
                    first = submit(std::move(cancel), nullptr);
                } catch (...) {
                    first = {MaiErrorCode::Internal, "touch cleanup injection threw"};
                }
                if (!first) {
                    contacts_.clear();
                } else {
                    fault_ = first;
                    publish();
                }
            }
            MaiError destroyed;
            try {
                destroyed = transport_->destroy();
            } catch (...) {
                destroyed = {MaiErrorCode::Internal, "touch source destruction threw"};
            }
            if (!destroyed) {
                created_ = false;
                contacts_.clear();

                desktop_ = {};
                seedPending_ = false;
                nextReport_ = {};
            } else if (!first)
                first = destroyed;
        }
        if (first) {
            fault_ = first;
            // Cleanup may run after revocation but before its synchronous
            // command reaches the writer. Preserve that request's real failure
            // across an idempotent empty-source cleanup.
            cleanupFailure_ = first;
            cleanupFailureEpoch_ = epoch_.load();
        } else if (clearFault) {
            fault_ = {};
            cleanupFailure_ = {};
        }
        if (epoch_.load() == generation) cleanupRequested_ = false;
        publish();
        return first;
    }
    MaiError run(Command& command) {
        if (command.cleanup) {
            if (cleanupFailure_ && cleanupFailureEpoch_ >= command.epoch) return cleanupFailure_;
            return retire(true);
        }
        if (revoked(&command, command.epoch)) {
            const auto error = canceled();
            if (created_) {
                fault_ = error;
                publish();
                retire(false);
            }
            return error;
        }
        if (fault_ && !command.onlyRelease) return fault_;
        const auto prepared = preflight(command);
        if (!prepared) return prepared.error();  // Entire batch pure preflight, before create or inject.
        if (prepared.value().retireLayout) {
            fault_ = {MaiErrorCode::Busy, "touch coordinate plane changed; the source was retired"};
            // Old physical points must never be reinterpreted in a new plane.
            MaiError destroyed;
            try {
                destroyed = created_ ? transport_->destroy() : MaiError{};
            } catch (...) {
                destroyed = {MaiErrorCode::Internal, "touch layout retirement threw"};
            }
            if (!destroyed) {
                created_ = false;
                contacts_.clear();
                desktop_ = {};
                seedPending_ = false;
                nextReport_ = {};
            }
            if (destroyed) fault_ = destroyed;
            publish();
            return fault_;
        }
        bool introduced = false;
        for (const auto& step : prepared.value().steps) {
            if (revoked(&command, command.epoch)) {
                fault_ = canceled();
                publish();
                retire(false);
                return canceled();
            }
            if (step.expands && seedPending_) {
                if (const auto error = finishSeedReport(command)) {
                    fault_ = error;
                    publish();
                    retire(false);
                    return error;
                }
            }
            if (!step.frame.points.empty()) {
                if (const auto error = open(command)) {
                    fault_ = error;
                    publish();
                    retire(false);
                    return error;
                }
                const bool hadTouch = touching(contacts_);
                try {
                    if (const auto error = submit(step.frame, &command, command.epoch)) {
                        fault_ = error;
                        publish();
                        if (introduced || step.expands || error.code() == MaiErrorCode::Canceled)
                            retire(false);
                        else
                            publish();  // A failed UP retains its owned binding for retry.
                        return error;
                    }
                } catch (...) {
                    desktop_ = step.desktop;
                    fault_ = {MaiErrorCode::Internal, "touch injection threw after a possible native submission"};
                    rememberUncertain(step);
                    retire(false);
                    return fault_;
                }
                contacts_ = step.next;  // Each accepted partial stage is authoritative immediately.
                desktop_ = step.desktop;
                introduced = introduced || step.expands;
                if (!hadTouch && touching(contacts_)) {
                    seedPending_ = true;
                    nextReport_ = Clock::now() + interval_;
                }
                if (!touching(contacts_)) seedPending_ = false;
                if (nextReport_ == Clock::time_point{}) nextReport_ = Clock::now() + interval_;
                publish();
                if (contacts_.empty()) {
                    const auto error = retire(false);
                    if (error) return error;
                }
            }
        }
        if (revoked(&command, command.epoch)) {
            fault_ = canceled();
            publish();
            retire(false);
            return canceled();
        }
        return {};
    }
    void complete(const std::shared_ptr<Command>& command, MaiError result) {
        command->cancel = {};  // No caller closure survives the synchronous response.
        command->response.set_value(std::move(result));
    }
    void work() {
        MaiThread::setCurrentName("mai-input-touch");
        for (;;) {
            // Capture before cleanup tests/admission. A later Stop must not
            // become this report's newly adopted generation.
            const auto generation = epoch_.load();
            std::shared_ptr<Command> command;
            std::optional<PublicCall> background;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                if (!pending_ && !cleanupRequested_ && !stopping_) {
                    if (created_ && !contacts_.empty() && !fault_ && interval_.count() > 0)
                        changed_.wait_until(lock, nextReport_,
                                            [&] { return pending_ || cleanupRequested_.load() || stopping_; });
                    else
                        changed_.wait(lock, [&] { return pending_ || cleanupRequested_.load() || stopping_; });
                }
                command = std::move(pending_);
                if (!command) {
                    // Reports and deferred cleanup also own public admission.
                    // A transport callback can read snapshots, but reentrant
                    // dispatch/release must never wait on this same writer.
                    background.emplace(admitted_);
                    if (!background->accepted) {
                        // A caller can own admission while preparing its job.
                        // Yield the queue lock rather than spin past a due tick.
                        changed_.wait_for(lock, std::chrono::milliseconds(1));
                        continue;
                    }
                }
            }
            if (stopping_) {
                retire(false);
                if (command) complete(command, canceled());
                return;
            }
            if (command) {
                MaiError result;
                try {
                    result = run(*command);
                } catch (...) {
                    fault_ = {MaiErrorCode::Internal, "touch source work threw"};
                    publish();
                    retire(false);
                    result = fault_;
                }
                if (cleanupRequested_) {
                    const auto cleaned = retire(false);
                    if (!result && cleaned) result = cleaned;
                }
                complete(command, std::move(result));
                continue;
            }
            if (cleanupRequested_) {
                retire(false);
                continue;
            }
            if (created_ && !contacts_.empty() && !fault_ && interval_.count() > 0 && Clock::now() >= nextReport_ &&
                epoch_.load() == generation && !cleanupRequested_) {
                try {
                    if (const auto error = report(nullptr, generation)) {
                        fault_ = error;
                        publish();
                        retire(false);
                    }
                } catch (...) {
                    fault_ = {MaiErrorCode::Internal, "touch periodic report threw"};
                    publish();
                    retire(false);
                }
            }
        }
    }

    std::shared_ptr<DesktopWindowsTouchTransport> transport_;
    std::chrono::milliseconds interval_;
    std::string sourceName_;
    std::atomic<bool> admitted_{false}, cleanupRequested_{false}, stopping_{false};
    std::atomic<std::uint64_t> epoch_{0};
    std::shared_ptr<const TouchPublished> published_ = std::make_shared<TouchPublished>();
    std::mutex queueMutex_;
    std::condition_variable changed_;
    std::thread writer_;
    std::shared_ptr<Command> pending_;
    // The resident writer alone owns this native source and contact ledger.
    Contacts contacts_;
    Environment desktop_;
    bool created_ = false, seedPending_ = false;
    Clock::time_point nextReport_{};
    MaiError fault_, cleanupFailure_;
    std::uint64_t cleanupFailureEpoch_ = 0;
};

#if defined(_WIN32)
class NativeTransport final : public DesktopWindowsTouchTransport {
public:
    explicit NativeTransport(std::shared_ptr<DesktopWindowsTouchNativeTrace> trace = {}, bool feedback = true, bool explicitQpc = false)
        : trace_(std::move(trace)), feedback_(feedback), explicitQpc_(explicitQpc) {
        module_ = LoadLibraryW(L"user32.dll");
        if (module_) {
            create_ = reinterpret_cast<Create>(GetProcAddress(module_, "CreateSyntheticPointerDevice"));
            inject_ = reinterpret_cast<Inject>(GetProcAddress(module_, "InjectSyntheticPointerInput"));
            destroy_ = reinterpret_cast<Destroy>(GetProcAddress(module_, "DestroySyntheticPointerDevice"));
            dpi_ = reinterpret_cast<Dpi>(GetProcAddress(module_, "SetThreadDpiAwarenessContext"));
            getDpi_ = reinterpret_cast<GetDpi>(GetProcAddress(module_, "GetThreadDpiAwarenessContext"));
            awareness_ = reinterpret_cast<Awareness>(GetProcAddress(module_, "GetAwarenessFromDpiAwarenessContext"));
            equalDpi_ = reinterpret_cast<EqualDpi>(GetProcAddress(module_, "AreDpiAwarenessContextsEqual"));
        }
    }
    ~NativeTransport() override { destroy(); if (module_) FreeLibrary(module_); }
    MaiError create(std::uint32_t capacity) override {
        if (!create_ || !inject_ || !destroy_)
            return {MaiErrorCode::NotSupported, "native touchscreen needs Windows 10 1809 synthetic pointer APIs"};
        if (device_) return {MaiErrorCode::Busy, "this native touchscreen source is already open"};
        lastAcceptedQpc_ = pendingQpc_ = 0;
        pendingBytes_.clear();
        const auto previousDpi = dpi_ ? dpi_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
        if (trace_) { creator_ = currentDpi(); creatorThread_ = GetCurrentThreadId(); capacity_ = capacity; }
        device_ = create_(PT_TOUCH, capacity, feedback_ ? POINTER_FEEDBACK_DEFAULT : POINTER_FEEDBACK_NONE);
        const auto error = device_ ? ERROR_SUCCESS : GetLastError();
        if (dpi_ && previousDpi) dpi_(previousDpi);
        return device_ ? MaiError{}
                       : MaiError{MaiErrorCode::Internal,
                                  "create owned touchscreen failed (Win32 " + std::to_string(error) + ")"};
    }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame& value) override {
        if (!device_ || value.points.empty() || value.points.size() > maxContacts)
            return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::InvalidInput, "invalid owned native touchscreen frame"}};
        const auto before = trace_ ? currentDpi() : DpiSample{};
        const auto previousDpi = dpi_ ? dpi_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
        const auto effective = trace_ ? currentDpi() : DpiSample{};
        const bool sameDesktop = GetSystemMetrics(SM_XVIRTUALSCREEN) == value.virtualX &&
            GetSystemMetrics(SM_YVIRTUALSCREEN) == value.virtualY && GetSystemMetrics(SM_CXVIRTUALSCREEN) == value.virtualWidth &&
            GetSystemMetrics(SM_CYVIRTUALSCREEN) == value.virtualHeight;
        if (!sameDesktop) {
            if (dpi_ && previousDpi) dpi_(previousDpi);
            return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Busy, "virtual desktop changed before touchscreen submission"}};
        }
        std::array<POINTER_TYPE_INFO, maxContacts> frame{};
        for (std::size_t index = 0; index < value.points.size(); ++index) {
            const auto& source = value.points[index];
            frame[index].type = PT_TOUCH;
            auto& native = frame[index].touchInfo;
            native.pointerInfo.pointerType = PT_TOUCH;
            native.pointerInfo.pointerId = source.pointerId;
            native.pointerInfo.pointerFlags = source.flags;
            native.pointerInfo.ptPixelLocation = {source.x, source.y};
            native.pressure = source.pressure;
            native.orientation = source.orientation;
            native.rcContact = {source.left, source.top, source.right, source.bottom};
            if (source.hasPressure) native.touchMask |= TOUCH_MASK_PRESSURE;
            if (source.hasOrientation) native.touchMask |= TOUCH_MASK_ORIENTATION;
            if (source.hasArea) native.touchMask |= TOUCH_MASK_CONTACTAREA;
        }
        if (explicitQpc_) {
            // Diagnostic only: POINTER_INFO documents optional injection
            // timestamps. Synthetic's own page does not restate those rules.
            // Keep the first element's timestamp stable across native retries.
            const auto size = sizeof(POINTER_TYPE_INFO) * value.points.size();
            const auto* begin = reinterpret_cast<const std::uint8_t*>(frame.data());
            const std::vector<std::uint8_t> plain(begin, begin + size);
            if (!pendingQpc_ || pendingBytes_ != plain) {
                LARGE_INTEGER now{}, frequency{};
                if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
                    if (dpi_ && previousDpi) dpi_(previousDpi);
                    return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "test injection timestamp counter unavailable"}};
                }
                const auto sample = std::uint64_t(now.QuadPart);
                const auto minimumStep = std::uint64_t(frequency.QuadPart) / 10000 + 1;
                if (sample <= lastAcceptedQpc_ || sample - lastAcceptedQpc_ < minimumStep) {
                    if (dpi_ && previousDpi) dpi_(previousDpi);
                    return {DesktopWindowsTouchSubmitKind::Retry, {MaiErrorCode::Busy, "test injection timestamp needs a later 0.1-ms window"}};
                }
                pendingQpc_ = sample; // Sampled now, never a fabricated future tick.
                pendingBytes_ = plain;
            }
            frame[0].touchInfo.pointerInfo.PerformanceCount = pendingQpc_;
        }
        DesktopWindowsTouchNativePacket packet;
        if (trace_) {
            packet.frame = value;
            packet.creatorThread = creatorThread_;
            packet.injectorThread = GetCurrentThreadId();
            packet.maxContacts = capacity_;
            packet.visualFeedback = feedback_;
            packet.creatorDpi = creator_.context;
            packet.beforeDpi = before.context;
            packet.effectiveDpi = effective.context;
            packet.creatorAwareness = creator_.awareness;
            packet.beforeAwareness = before.awareness;
            packet.effectiveAwareness = effective.awareness;
            packet.creatorV2 = creator_.v2;
            packet.beforeV2 = before.v2;
            packet.effectiveV2 = effective.v2;
            packet.stride = sizeof(POINTER_TYPE_INFO);
            packet.sourceHandle = std::uint64_t(reinterpret_cast<std::uintptr_t>(device_));
            packet.bytes.resize(sizeof(POINTER_TYPE_INFO) * value.points.size());
            std::memcpy(packet.bytes.data(), frame.data(), packet.bytes.size());
        }
        if (trace_) packet.callStartedMs = GetTickCount64();
        const auto accepted = inject_(device_, frame.data(), static_cast<UINT32>(value.points.size()));
        const auto error = accepted ? ERROR_SUCCESS : GetLastError();
        if (trace_) packet.callFinishedMs = GetTickCount64();
        if (dpi_ && previousDpi) dpi_(previousDpi);
        if (trace_) {
            packet.restoredDpi = currentDpi().context;
            packet.accepted = accepted != FALSE;
            packet.error = error;
            trace_->append(std::move(packet));
        }
        if (accepted) {
            if (explicitQpc_) { lastAcceptedQpc_ = pendingQpc_; pendingQpc_ = 0; pendingBytes_.clear(); }
            return {};
        }
        if (error != ERROR_NOT_READY) { pendingQpc_ = 0; pendingBytes_.clear(); }
        return {error == ERROR_NOT_READY ? DesktopWindowsTouchSubmitKind::Retry : DesktopWindowsTouchSubmitKind::Failed,
            {MaiErrorCode::Internal, "owned touchscreen frame failed (Win32 " + std::to_string(error) + ")"}};
    }
    MaiError destroy() override {
        if (device_) { destroy_(device_); device_ = nullptr; }
        return {};
    }
private:
    struct DpiSample { std::int64_t context = 0; int awareness = -1; bool v2 = false; };
    DpiSample currentDpi() const {
        const auto context = getDpi_ ? getDpi_() : nullptr;
        return {static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(context)),
            context && awareness_ ? int(awareness_(context)) : -1,
            context && equalDpi_ && equalDpi_(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    }
    using Create = HSYNTHETICPOINTERDEVICE (WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using Inject = BOOL (WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
    using Destroy = VOID (WINAPI*)(HSYNTHETICPOINTERDEVICE);
    using Dpi = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetDpi = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using Awareness = DPI_AWARENESS (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using EqualDpi = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    HMODULE module_ = nullptr;
    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    Create create_ = nullptr;
    Inject inject_ = nullptr;
    Destroy destroy_ = nullptr;
    Dpi dpi_ = nullptr;
    GetDpi getDpi_ = nullptr;
    Awareness awareness_ = nullptr;
    EqualDpi equalDpi_ = nullptr;
    DpiSample creator_;
    DWORD creatorThread_ = 0;
    std::uint32_t capacity_ = 0;
    std::shared_ptr<DesktopWindowsTouchNativeTrace> trace_;
    bool feedback_ = true;
    bool explicitQpc_ = false;
    std::uint64_t lastAcceptedQpc_ = 0, pendingQpc_ = 0;
    std::vector<std::uint8_t> pendingBytes_;
};
#else
class NativeTransport final : public DesktopWindowsTouchTransport {
public:
    explicit NativeTransport(std::shared_ptr<DesktopWindowsTouchNativeTrace> = {}, bool = true, bool = false) {}
    MaiError create(std::uint32_t) override { return {MaiErrorCode::NotSupported, "Windows native touchscreen is unavailable on this platform"}; }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame&) override {
        return {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::NotSupported, "Windows native touchscreen is unavailable"}};
    }
    MaiError destroy() override { return {}; }
};
#endif
} // namespace

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouchWithTransport(
    std::shared_ptr<DesktopWindowsTouchTransport> transport, std::chrono::milliseconds refreshInterval) {
    return transport ? std::make_shared<TouchFrames>(std::move(transport), refreshInterval) : nullptr;
}

std::shared_ptr<DesktopComputerWindowsTouch> makeDesktopComputerWindowsTouch() {
    return makeDesktopComputerWindowsTouchWithTransport(std::make_shared<NativeTransport>());
}

std::shared_ptr<DesktopWindowsTouchTransport> makeDesktopWindowsTouchNativeTransportForTesting(
    std::shared_ptr<DesktopWindowsTouchNativeTrace> trace, bool visualFeedback, bool explicitQpc) {
    return std::make_shared<NativeTransport>(std::move(trace), visualFeedback, explicitQpc);
}
