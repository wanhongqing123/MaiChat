// Shared input policy around one native ledger. All platform calls, state and
// cleanup belong to native_; this layer owns only scheduling and scope policy.
#include "MaiChatTools/DesktopComputerInputSession.h"
#include "MaiChatTools/DesktopComputerBackend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "MaiThread.h"

MaiError DesktopNativeInputSession::resetControls() {
    return releaseAll();
}
MaiError DesktopNativeInputSession::inputEvents(const std::vector<DesktopInputEvent>&, const DesktopNativeInput&,
                                                const DesktopInputPort&, const DesktopInputOptions&) {
    return {MaiErrorCode::NotSupported, "typed input is unavailable in this native session"};
}
DesktopInputCapabilities DesktopNativeInputSession::capabilities() const {
    return {};
}
DesktopInputCapabilities DesktopComputerBackend::inputCapabilities() const {
    return {};
}
DesktopInputFeedbackBatch DesktopNativeInputSession::feedback(std::uint64_t after, std::uint32_t) const {
    DesktopInputFeedbackBatch result;
    result.nextSequence = after;
    return result;
}

namespace {
using Clock = std::chrono::steady_clock;
// Host/native callbacks may reenter on this same thread. A non-recursive
// mutex's try_lock is not a portable reentry test; claim admission atomically
// instead, including reset and autonomous repeat dispatch.
class OperationAdmission {
public:
    explicit OperationAdmission(std::atomic<bool>& active) : active_(active) {
        bool expected = false;
        acquired_ = active_.compare_exchange_strong(expected, true, std::memory_order_acquire);
    }
    ~OperationAdmission() {
        if (acquired_) active_.store(false, std::memory_order_release);
    }
    bool owns_lock() const {
        return acquired_;
    }

private:
    std::atomic<bool>& active_;
    bool acquired_ = false;
};
class ActiveCall {
public:
    explicit ActiveCall(std::atomic<bool>& active) : active_(active) {
        active_.store(true);
    }
    ~ActiveCall() {
        active_.store(false);
    }

private:
    std::atomic<bool>& active_;
};
class ActiveCleanup {
public:
    explicit ActiveCleanup(std::atomic<int>& depth) : depth_(depth) {
        depth_.fetch_add(1);
    }
    ~ActiveCleanup() {
        depth_.fetch_sub(1);
    }

private:
    std::atomic<int>& depth_;
};

MaiError nativeScope(const DesktopNativeInput& scope) {
    if (scope.isCanceled && scope.isCanceled()) return {MaiErrorCode::Canceled, "computer input canceled"};
#if defined(_WIN32)
    return maiDesktopWindowsValidateInputScope(scope);
#else
    if (scope.desktopScope) {
        const auto displays = maiDesktopDisplays();
        if (!displays) return displays.error();
        for (const auto& display : displays.value()) {
            if (display.id != scope.display.id) continue;
            if (display.x == scope.display.x && display.y == scope.display.y && display.width == scope.display.width &&
                display.height == scope.display.height)
                return {};
            return {MaiErrorCode::Busy, "observed display geometry changed before input"};
        }
        return {MaiErrorCode::NotFound, "observed display is no longer active"};
    }
    if (!scope.expectedWindowId || !scope.expectedProcessId)
        return {MaiErrorCode::InvalidInput, "typed input needs an observed window or display"};
    DesktopNativeWindow target;
    target.id = scope.expectedWindowId;
    target.processId = scope.expectedProcessId;
    const auto allowed = maiDesktopWindowAcceptsInput(target);
    if (!allowed) return allowed.error();
    return allowed.value() ? MaiError{} : MaiError{MaiErrorCode::Busy, "selected window no longer accepts input"};
#endif
}
bool ownsRaw(const DesktopNativeInputState& state, const DesktopInputRawKey& source) {
    const auto identity = maiDesktopInputKeyIdentity(source);
    return std::any_of(state.rawKeys.begin(), state.rawKeys.end(),
                       [&](const auto& held) { return maiDesktopInputKeyIdentity(held) == identity; });
}
MaiError enabled(const DesktopInputEvent& event, const DesktopInputOptions& options) {
    if ((event.kind == DesktopInputEventKind::RawKey || event.kind == DesktopInputEventKind::UnicodeText) &&
        !options.keyboardEnabled)
        return {MaiErrorCode::NotSupported, "keyboard input is disabled for this session"};
    const bool pointer = event.kind <= DesktopInputEventKind::Wheel || event.kind == DesktopInputEventKind::Touch ||
                         event.kind == DesktopInputEventKind::Pen;
    if (pointer && !options.mouseEnabled)
        return {MaiErrorCode::NotSupported, "pointer input is disabled for this session"};
    if ((event.kind == DesktopInputEventKind::Touch || event.kind == DesktopInputEventKind::Pen) &&
        !options.nativePenTouchEnabled)
        return {MaiErrorCode::NotSupported, "native pen and touch input are disabled for this session"};
    return {};
}

class InputSession final : public DesktopNativeInputSession {
public:
    InputSession(std::shared_ptr<DesktopNativeInputSession> native, DesktopInputScopeValidator validator)
        : native_(std::move(native)), validator_(std::move(validator)) {}
    ~InputSession() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        releaseAll();
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    MaiError input(const DesktopNativeInput& input) override {
        OperationAdmission operation(operationActive_);
        if (!operation.owns_lock()) return busy();
        const auto generation = epoch_.load();
        if (cleanupDepth_.load()) return busy();
        ActiveCall active(publicInFlight_);
        publicRevision_.fetch_add(1);
        const bool releasing =
            input.kind == DesktopNativeInputKind::KeyUp || input.kind == DesktopNativeInputKind::MouseUp;
        auto scoped = input;
        scoped.isCanceled = releasing ? std::function<bool()>{} : cancellation(generation, input.isCanceled);
        if (!releasing) {
            DesktopInputOptions options;
            MaiError previous;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                options = options_;
                previous = asyncFailure_;
            }
            if (previous && !native_->state().empty()) return fail(previous, generation);
            DesktopInputEvent category;
            category.kind =
                input.kind == DesktopNativeInputKind::Type || input.kind == DesktopNativeInputKind::ReplaceFocusedText
                    ? DesktopInputEventKind::UnicodeText
                : input.kind == DesktopNativeInputKind::Key || input.kind == DesktopNativeInputKind::KeyDown
                    ? DesktopInputEventKind::RawKey
                    : DesktopInputEventKind::RelativeMove;
            if (const auto disabled = enabled(category, options)) return fail(disabled, generation);
            if (scoped.isCanceled()) return fail({MaiErrorCode::Canceled, "computer input canceled"}, generation);
            const auto ready = validator_(scoped);  // May synchronously ask the GUI; no policy mutex is held.
            if (ready) return fail(ready, generation);
            if (scoped.isCanceled())
                return fail({MaiErrorCode::Canceled, "input revoked during target validation"}, generation);
            rememberScope(input, {}, generation);
        }
        const auto error = native_->input(scoped);
        if (error) return fail(error, generation);
        if (!releasing && scoped.isCanceled())
            return fail({MaiErrorCode::Canceled, "input canceled after native dispatch"}, generation);
        reconcileRepeat(native_->state());
        return {};
    }
    MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& scope,
                         const DesktopInputPort& port, const DesktopInputOptions& requested) override {
        OperationAdmission operation(operationActive_);
        if (!operation.owns_lock()) return busy();
        const auto generation = epoch_.load();
        if (cleanupDepth_.load()) return busy();
        ActiveCall active(publicInFlight_);
        publicRevision_.fetch_add(1);
        if (const auto invalid = maiDesktopValidateInputEvents(events)) return fail(invalid, generation);
        DesktopInputOptions options, retainedOptions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            options = requested.inheritExisting ? options_ : requested;
            retainedOptions = options_;
        }
        options.inheritExisting = false;
        if (const auto invalid = maiDesktopValidateInputOptions(options)) return fail(invalid, generation);
        const bool releasing = maiDesktopInputEventsOnlyRelease(events);
        // Cleanup may not change the inherited configuration or wheel remainder.
        // Its caller options are still validated, but native receives the live policy.
        if (releasing) options = retainedOptions;
        if (!releasing) {
            MaiError previous;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                previous = asyncFailure_;
            }
            if (previous && !native_->state().empty()) return fail(previous, generation);
            const auto ready = validator_(scope);
            if (ready) return fail(ready, generation);
            if (canceled(generation, scope.isCanceled))
                return fail({MaiErrorCode::Canceled, "input revoked during target validation"}, generation);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                options_ = options;
                asyncFailure_ = {};
                if (!options.keyboardRepeat || options.repeatDelayMs <= 0 || !options.keyboardEnabled) repeat_.reset();
            }
            rememberScope(scope, port, generation);
        }
        for (const auto& source : events) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(source.delayMs);
            while (Clock::now() < deadline) {
                // A host callback can read/cancel this session. Invoke it
                // outside the policy mutex, just like validation/native work.
                if (canceled(generation, scope.isCanceled)) break;
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait_until(lock, (std::min)(deadline, Clock::now() + std::chrono::milliseconds(5)));
            }
            if (!releasing && canceled(generation, scope.isCanceled))
                return fail({MaiErrorCode::Canceled, "input batch canceled during delay"}, generation);
            auto guard = scope;
            guard.kind = source.kind == DesktopInputEventKind::RawKey        ? DesktopNativeInputKind::Key
                         : source.kind == DesktopInputEventKind::UnicodeText ? DesktopNativeInputKind::Type
                                                                             : DesktopNativeInputKind::Move;
            guard.isCanceled = releasing ? std::function<bool()>{} : cancellation(generation, scope.isCanceled);
            if (!releasing) {
                if (const auto ready = validator_(guard)) return fail(ready, generation);
                if (guard.isCanceled())
                    return fail({MaiErrorCode::Canceled, "input revoked during target validation"}, generation);
                if (const auto disabled = enabled(source, options)) return fail(disabled, generation);
            }
            const auto before = native_->state();
            auto prepared = source;
            prepared.delayMs = 0;  // Consume delay here; native receives one already scheduled event.
            const auto error = native_->inputEvents({prepared}, guard, port, options);
            if (error) return fail(error, generation);
            if (!releasing && guard.isCanceled())
                return fail({MaiErrorCode::Canceled, "input canceled after native dispatch"}, generation);
            const auto after = native_->state();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (epoch_.load() != generation) continue;
                if (source.kind == DesktopInputEventKind::RawKey && !source.key.release && !source.key.repeat &&
                    !ownsRaw(before, source.key) && ownsRaw(after, source.key)) {
                    repeat_.reset();
                    if (options.keyboardRepeat && options.repeatDelayMs > 0) {
                        // Retain the source key only, never unused text/contact
                        // fields supplied through the ordinary event DTO.
                        repeat_.emplace();
                        repeat_->kind = DesktopInputEventKind::RawKey;
                        repeat_->key = source.key;
                        repeat_->key.repeat = true;  // Original source identity; native reuses its frozen binding.
                        nextRepeat_ = Clock::now() + std::chrono::milliseconds(options.repeatDelayMs);
                    }
                }
                if (repeat_ && !ownsRaw(after, repeat_->key)) repeat_.reset();
                if (repeat_ && !worker_.joinable()) worker_ = std::thread([this] { work(); });
            }
        }
        changed_.notify_all();
        return {};
    }
    DesktopNativeInputState state() const override {
        return native_->state();
    }
    DesktopInputCapabilities capabilities() const override {
        return native_->capabilities();
    }
    DesktopInputFeedbackBatch feedback(std::uint64_t after, std::uint32_t limit) const override {
        return native_->feedback(after, limit);
    }
    MaiError validate() const override {
        const auto revision = publicRevision_.load(), generation = epoch_.load();
        std::optional<DesktopNativeInput> scope;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (asyncFailure_) return asyncFailure_;
            if (bound_) scope = scope_;
        }
        auto error = native_->validate();
        if (!error && scope) error = validator_(*scope);
        // A completed validation of an old context cannot expire its replacement.
        return publicRevision_.load() == revision && epoch_.load() == generation ? error : MaiError{};
    }
    MaiError resetControls() override {
        OperationAdmission operation(operationActive_);
        if (!operation.owns_lock() || cleanupDepth_.load()) return busy();
        ActiveCleanup cleanup(cleanupDepth_);
        const auto generation = epoch_.fetch_add(1) + 1;
        publicRevision_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            repeat_.reset();
            boundEpoch_ = generation;
        }
        changed_.notify_all();
        return native_->resetControls();  // A failed reset keeps the native retry binding and original scope.
    }
    MaiError releaseAll() override {
        ActiveCleanup cleanup(cleanupDepth_);
        epoch_.fetch_add(1);
        publicRevision_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            repeat_.reset();
        }
        changed_.notify_all();
        const auto error = native_->releaseAll();  // Never hold the policy mutex while native cleanup waits.
        if (!error) {
            std::lock_guard<std::mutex> lock(mutex_);
            bound_ = false;
            asyncFailure_ = {};
        }
        return error;  // Busy is not an empty state; state() remains the native's live projection.
    }

private:
    static MaiError busy() {
        return {MaiErrorCode::Busy, "another ordinary input operation or cleanup is in flight"};
    }
    bool canceled(std::uint64_t generation, const std::function<bool()>& caller) const {
        if (epoch_.load() != generation) return true;
        if (caller && caller()) return true;
        // A callback may revoke through releaseAll() while returning false.
        return epoch_.load() != generation;
    }
    std::function<bool()> cancellation(std::uint64_t generation, const std::function<bool()>& caller) const {
        return [this, generation, caller] { return canceled(generation, caller); };
    }
    void rememberScope(const DesktopNativeInput& scope, const DesktopInputPort& port, std::uint64_t generation) {
        auto retained = scope;
        retained.isCanceled = {};
        retained.text.clear();
        retained.key.clear();
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch_.load() != generation) return;
        // An admitted scope after the failed input was cleaned supersedes its
        // old asynchronous error, including when this call is a composite.
        asyncFailure_ = {};
        scope_ = std::move(retained);
        port_ = port;
        boundEpoch_ = generation;
        bound_ = true;
    }
    void reconcileRepeat(const DesktopNativeInputState& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (repeat_ && !ownsRaw(state, repeat_->key)) repeat_.reset();
    }
    MaiError fail(const MaiError& error, std::uint64_t generation) {
        // Asynchronous release already owns revocation/cleanup for an older
        // operation. Do not turn its late return into another cleanup request.
        if (epoch_.load() != generation) return error;
        const auto cleanup = releaseAll();
        return cleanup ? MaiError{error.code(), error.message() + "; cleanup failed: " + cleanup.message()} : error;
    }
    void work() {
        MaiThread::setCurrentName("mai-input-repeat");
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (!repeat_)
                changed_.wait(lock, [this] { return stopping_ || repeat_.has_value(); });
            else
                changed_.wait_for(lock, std::chrono::milliseconds(5));
            if (stopping_) break;
            if (!repeat_ || Clock::now() < nextRepeat_) continue;
            if (epoch_.load() != boundEpoch_) {
                repeat_.reset();
                continue;
            }
            if (publicInFlight_.load() || cleanupDepth_.load()) continue;
            const auto revision = publicRevision_.load(), generation = boundEpoch_;
            auto guard = scope_;
            guard.kind = DesktopNativeInputKind::Key;
            guard.isCanceled = cancellation(generation, {});
            const auto event = *repeat_;
            const auto port = port_;
            const auto options = options_;
            lock.unlock();
            const auto held = ownsRaw(native_->state(), event.key);
            const auto ready = held ? validator_(guard) : MaiError{};
            OperationAdmission operation(operationActive_);
            lock.lock();
            if (!operation.owns_lock() || stopping_ || publicInFlight_.load() || cleanupDepth_.load() ||
                epoch_.load() != generation || publicRevision_.load() != revision || !repeat_)
                continue;
            if (!held) {
                repeat_.reset();
                continue;
            }
            if (ready) {
                failAsync(lock, ready);
                continue;
            }
            lock.unlock();
            const auto sent = native_->inputEvents({event}, guard, port, options);
            lock.lock();
            if (epoch_.load() != generation) continue;
            if (sent) {
                failAsync(lock, sent);
                continue;
            }
            nextRepeat_ = Clock::now() + std::chrono::milliseconds(options.repeatPeriodMs);
        }
    }
    void failAsync(std::unique_lock<std::mutex>& lock, const MaiError& failure) {
        ActiveCleanup cleanup(cleanupDepth_);
        epoch_.fetch_add(1);
        repeat_.reset();
        asyncFailure_ = failure;
        changed_.notify_all();
        lock.unlock();
        const auto error = native_->releaseAll();
        lock.lock();
        if (error) asyncFailure_ = {failure.code(), failure.message() + "; cleanup failed: " + error.message()};
    }
    std::shared_ptr<DesktopNativeInputSession> native_;
    DesktopInputScopeValidator validator_;
    mutable std::mutex mutex_;
    std::atomic<bool> operationActive_{false};
    std::atomic<bool> publicInFlight_{false};
    std::atomic<std::uint64_t> publicRevision_{0}, epoch_{0};
    std::atomic<int> cleanupDepth_{0};
    std::condition_variable changed_;
    std::thread worker_;
    bool stopping_ = false, bound_ = false;
    std::uint64_t boundEpoch_ = 0;
    MaiError asyncFailure_;
    DesktopNativeInput scope_;
    DesktopInputPort port_;
    DesktopInputOptions options_;
    std::optional<DesktopInputEvent> repeat_;
    Clock::time_point nextRepeat_;
};
}  // namespace

std::shared_ptr<DesktopNativeInputSession> makeDesktopComputerInputSession(
    std::shared_ptr<DesktopNativeInputSession> native, DesktopInputScopeValidator validator) {
    if (!native) return {};
    // Native classifies the actual command/frozen binding for secure input.
    // Treating every Key as text here would block Tab/Escape/Meta navigation.
    if (!validator) validator = nativeScope;
    return std::make_shared<InputSession>(std::move(native), std::move(validator));
}
DesktopInputCapabilities maiDesktopInputCapabilities() {
    const auto native = maiDesktopCreateInputSession();
    return native ? native->capabilities() : DesktopInputCapabilities{};
}
