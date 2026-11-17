// Shared policy contracts against one recording native ledger. No Qt, driver,
// SDK, desktop enumeration or OS input is used by this executable.
#include "MaiChatTools/DesktopComputerInputSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#if defined(MAI_COMMON_MUTEX_AUDIT)
unsigned commonMutexReentryAttempts();
#endif

namespace {
std::atomic<int> failures{0}, checks{0};
#define CHECK(value)                                   \
    do {                                               \
        ++checks;                                      \
        if (!(value)) {                                \
            std::cerr << __LINE__ << ": " #value "\n"; \
            ++failures;                                \
        }                                              \
    } while (false)
using Clock = std::chrono::steady_clock;
using Kind = DesktopInputEventKind;
DesktopInputEvent raw(std::uint16_t code, bool up = false) {
    DesktopInputEvent event;
    event.kind = Kind::RawKey;
    event.key.code = code;
    event.key.release = up;
    return event;
}
DesktopInputEvent relative(std::int32_t x = 1) {
    DesktopInputEvent event;
    event.kind = Kind::RelativeMove;
    event.deltaX = x;
    return event;
}
bool waitUntil(const std::function<bool()>& condition, int milliseconds = 400) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition() && Clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return condition();
}
bool modifier(std::uint16_t code) {
    if (code & 0x8000) code &= 0xff;
    return (code >= 0x10 && code <= 0x12) || (code >= 0xa0 && code <= 0xa5) || code == 0x5b || code == 0x5c;
}

// A native-boundary recorder, not a successful-by-default provider. It accepts
// prepared single events and has one authoritative source/binding ledger.
class Native final : public DesktopNativeInputSession {
public:
    MaiError input(const DesktopNativeInput& scope) override {
        ++ordinaryCalls;
        {
            std::unique_lock<std::mutex> lock(blockMutex);
            ordinaryEntered = true;
            changed.notify_all();
            if (blockOrdinary) changed.wait(lock, [&] { return proceed; });
        }
        if (scope.isCanceled && scope.isCanceled()) return {MaiErrorCode::Canceled, "native ordinary canceled"};
        std::lock_guard<std::mutex> lock(mutex);
        if (ordinaryError) return ordinaryError;
        if ((scope.kind == DesktopNativeInputKind::KeyDown || scope.kind == DesktopNativeInputKind::KeyUp) &&
            scope.key == "Z") {
            DesktopInputRawKey original;
            original.code = 'Z';
            const auto identity = std::uint32_t(0x80000000) | maiDesktopInputKeyIdentity(original);
            if (scope.kind == DesktopNativeInputKind::KeyDown)
                bindings.emplace(identity, Binding{original, original, false});
            else
                bindings.erase(identity);
        }
        if (secureFocus &&
            (scope.kind == DesktopNativeInputKind::Type || scope.kind == DesktopNativeInputKind::ReplaceFocusedText ||
             ((scope.kind == DesktopNativeInputKind::Key || scope.kind == DesktopNativeInputKind::KeyDown) &&
              scope.key != "Tab" && scope.key != "Escape" && scope.key != "Meta")))
            return {MaiErrorCode::NotSupported, "native secure-focus classification"};
        if ((scope.kind == DesktopNativeInputKind::Type || scope.kind == DesktopNativeInputKind::ReplaceFocusedText) &&
            heldModifier())
            return {MaiErrorCode::Busy, "native literal text conflicts with owned modifier"};
        return {};
    }
    MaiError inputEvents(const std::vector<DesktopInputEvent>& events, const DesktopNativeInput& scope,
                         const DesktopInputPort& port, const DesktopInputOptions& options) override {
        if (events.size() != 1 || events.front().delayMs != 0)
            return {MaiErrorCode::InvalidInput, "native recorder accepts prepared singleton events only"};
        if (scope.isCanceled && scope.isCanceled()) return {MaiErrorCode::Canceled, "native typed canceled"};
        const auto event = events.front();
        if (event.kind > Kind::UnicodeText) return {MaiErrorCode::NotSupported, "deferred native family"};
        std::unique_lock<std::mutex> lock(mutex);
        seen.push_back(event);
        lastOptions = options;
        lastPort = port;
        lastScope = scope;
        lastScope.isCanceled = {};
        lastScope.text.clear();
        lastScope.key.clear();
        if (secureFocus && event.kind == Kind::UnicodeText)
            return {MaiErrorCode::NotSupported, "native secure-focus classification"};
        if (secureFocus && event.kind == Kind::RawKey && !event.key.release) {
            const auto found = bindings.find(maiDesktopInputKeyIdentity(event.key));
            const auto binding =
                found == bindings.end() ? maiDesktopRemapInputKey(event.key, options) : found->second.effective;
            const auto code = std::uint16_t((binding.code & 0x8000) ? binding.code & 0xff : binding.code);
            if (code != 9 && code != 0x1b && !modifier(code))
                return {MaiErrorCode::NotSupported, "native secure-focus classification"};
        }
        if (event.kind == Kind::UnicodeText && heldModifier())
            return {MaiErrorCode::Busy, "native literal text conflicts with owned modifier"};
        if (event.kind == Kind::RawKey) applyKey(event.key, options);
        if (event.kind == Kind::MouseButton) {
            if (event.release)
                held.buttonsDown.clear();
            else
                held.buttonsDown = {"left"};
        }
        if (failAt && seen.size() == failAt) return {MaiErrorCode::Internal, "injected native submit failure"};
        const auto callback = afterTyped;
        lock.unlock();
        if (callback) callback();
        return {};
    }
    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(mutex);
        auto result = held;
        result.rawKeys.clear();
        result.keysDown.clear();
        for (const auto& [identity, binding] : bindings) {
            (void)identity;
            if (binding.rawSource) result.rawKeys.push_back(binding.source);
            result.keysDown.push_back("raw:" + std::to_string(binding.effective.code));
        }
        return result;
    }
    MaiError validate() const override {
        return validationError;
    }
    MaiError releaseAll() override {
        ++releases;
        {
            std::unique_lock<std::mutex> lock(blockMutex);
            releaseEntered = true;
            changed.notify_all();
            if (blockRelease) changed.wait(lock, [&] { return releaseProceed; });
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (cleanupError) return cleanupError;
        if (failRelease) return {MaiErrorCode::Internal, "injected native cleanup failure"};
        bindings.clear();
        held = {};
        return {};
    }
    MaiError resetControls() override {
        ++resets;
        std::unique_lock<std::mutex> lock(mutex);
        if (failRelease) return {MaiErrorCode::Internal, "injected native reset failure"};
        bindings.clear();
        held = {};
        const auto callback = afterReset;
        lock.unlock();
        if (callback) callback();
        return {};
    }
    DesktopInputCapabilities capabilities() const override {
        DesktopInputCapabilities value;
        value.baseline = "recording-native";
        return value;
    }
    DesktopInputFeedbackBatch feedback(std::uint64_t after, std::uint32_t limit) const override {
        feedbackAfter = after;
        feedbackLimit = limit;
        DesktopInputFeedbackBatch batch;
        batch.nextSequence = after;
        return batch;
    }
    std::vector<DesktopInputEvent> events() const {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }
    std::vector<DesktopInputRawKey> transitions() const {
        std::lock_guard<std::mutex> lock(mutex);
        return output;
    }
    DesktopInputOptions options() const {
        std::lock_guard<std::mutex> lock(mutex);
        return lastOptions;
    }
    mutable std::mutex mutex;
    mutable std::uint64_t feedbackAfter = 0;
    mutable std::uint32_t feedbackLimit = 0;
    std::atomic<int> ordinaryCalls{0}, releases{0}, resets{0};
    std::size_t failAt = 0;
    bool failRelease = false, secureFocus = false;
    MaiError ordinaryError, validationError, cleanupError;
    std::function<void()> afterTyped, afterReset;
    std::mutex blockMutex;
    std::condition_variable changed;
    bool blockOrdinary = false, ordinaryEntered = false, proceed = false;
    bool blockRelease = false, releaseEntered = false, releaseProceed = false;

private:
    struct Binding {
        DesktopInputRawKey source, effective;
        bool rawSource = true;
    };
    bool heldModifier() const {
        for (const auto& [identity, binding] : bindings) {
            (void)identity;
            if (modifier(binding.effective.code)) return true;
        }
        return false;
    }
    void applyKey(const DesktopInputRawKey& source, const DesktopInputOptions& options) {
        const auto identity = maiDesktopInputKeyIdentity(source);
        const auto found = bindings.find(identity);
        const auto same = [](const auto& first, const auto& second) {
            return first.code == second.code && first.flags == second.flags &&
                   (first.modifiers & 0x10) == (second.modifiers & 0x10);
        };
        if (source.release) {
            if (found == bindings.end()) return;
            auto effective = found->second.effective;
            bindings.erase(found);
            const bool alias = std::any_of(bindings.begin(), bindings.end(),
                                           [&](const auto& item) { return same(item.second.effective, effective); });
            if (!alias) {
                effective.release = true;
                output.push_back(effective);
            }
        } else if (source.repeat) {
            if (found != bindings.end()) {
                auto effective = found->second.effective;
                effective.repeat = true;
                output.push_back(effective);
            }
        } else if (found == bindings.end()) {
            const auto effective = maiDesktopRemapInputKey(source, options);
            const bool alias = std::any_of(bindings.begin(), bindings.end(),
                                           [&](const auto& item) { return same(item.second.effective, effective); });
            bindings.emplace(identity, Binding{source, effective});
            if (!alias) output.push_back(effective);
        }
    }
    DesktopNativeInputState held;
    DesktopInputOptions lastOptions;
    DesktopInputPort lastPort;
    DesktopNativeInput lastScope;
    std::map<std::uint32_t, Binding> bindings;
    std::vector<DesktopInputEvent> seen;
    std::vector<DesktopInputRawKey> output;
};

std::shared_ptr<DesktopNativeInputSession> wrap(
    const std::shared_ptr<Native>& native,
    DesktopInputScopeValidator validator = [](const auto&) { return MaiError{}; }) {
    return makeDesktopComputerInputSession(native, std::move(validator));
}
DesktopInputOptions noRepeat() {
    DesktopInputOptions options;
    options.keyboardRepeat = false;
    return options;
}

void forwardingAndCallbackLifetime() {
    auto native = std::make_shared<Native>();
    int checked = 0;
    auto session = wrap(native, [&](const auto&) {
        ++checked;
        return MaiError{};
    });
    auto alive = std::make_shared<int>(1);
    std::weak_ptr<int> weak = alive;
    DesktopNativeInput scope;
    scope.isCanceled = [alive] { return false; };
    auto options = noRepeat();
    options.keyBindings = {{{0x41, 0x42}}};
    CHECK(!session->inputEvents({raw(0x41)}, scope, {}, options));
    CHECK(native->events().size() == 1 && native->events()[0].key.code == 0x41);
    CHECK(native->transitions().size() == 1 && native->transitions()[0].code == 0x42);
    CHECK(session->state().rawKeys.size() == 1 && session->state().rawKeys[0].code == 0x41);
    scope.isCanceled = {};
    alive.reset();
    CHECK(weak.expired());
    CHECK(checked > 0);
    CHECK(session->capabilities().baseline == "recording-native");
    CHECK(!makeDesktopComputerInputSession({}));
    CHECK(session->feedback(42, 3).nextSequence == 42 && native->feedbackAfter == 42 && native->feedbackLimit == 3);
    CHECK(native->releases == 0);
    CHECK(!session->input({}) && native->ordinaryCalls == 1);
    CHECK(!session->releaseAll() && session->state().empty());
}
void preflightAndCleanupFailures() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    auto invalid = relative();
    invalid.x = 2;
    invalid.kind = Kind::AbsoluteMove;
    CHECK(session->inputEvents({invalid}, {}, {}, noRepeat()));
    CHECK(native->events().empty() && native->releases > 0);
    native->failAt = 1;
    native->failRelease = true;
    const auto failure = session->inputEvents({raw(0x41)}, {}, {}, noRepeat());
    CHECK(failure.code() == MaiErrorCode::Internal && failure.message().find("cleanup failure") != std::string::npos);
    CHECK(!session->state().empty());
    native->failRelease = false;
    auto changed = noRepeat();
    changed.keyBindings = {{{0x41, 0x43}}};
    CHECK(!session->inputEvents({raw(0x41, true)}, {}, {}, changed));
    CHECK(session->state().empty());
    native->ordinaryError = {MaiErrorCode::Busy, "original ordinary failure"};
    native->failRelease = true;
    const auto ordinary = session->input({});
    CHECK(ordinary.code() == MaiErrorCode::Busy && ordinary.message().find("original ordinary") != std::string::npos &&
          ordinary.message().find("cleanup failure") != std::string::npos);
    native->failRelease = false;
}
void nativeOwnsAliasesAndFrozenBindings() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    auto options = noRepeat();
    options.keyBindings = {{{0x41, 0x43}}, {{0x42, 0x43}}};
    CHECK(!session->inputEvents({raw(0x41), raw(0x42)}, {}, {}, options));
    CHECK(native->transitions().size() == 1 && session->state().rawKeys.size() == 2);
    options.keyBindings = {{{0x41, 0x44}}, {{0x42, 0x45}}};
    CHECK(!session->inputEvents({raw(0x41, true)}, {}, {}, options));
    CHECK(native->transitions().size() == 1);
    CHECK(!session->inputEvents({raw(0x42, true)}, {}, {}, options));
    CHECK(native->transitions().size() == 2 && native->transitions().back().code == 0x43 &&
          native->transitions().back().release);
    CHECK(maiDesktopInputKeyIdentity(raw(0x41).key) == maiDesktopInputKeyIdentity(raw(0x8041).key));
    auto first = raw(0x41);
    first.key.modifiers = 0x10;
    CHECK(!session->inputEvents({first}, {}, {}, noRepeat()));
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
    CHECK(session->state().rawKeys.size() == 1 && session->state().rawKeys[0].modifiers == 0x10);
}
void repeatUsesOnlyNativeSourceProjection() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    options.keyBindings = {{{0x41, 0x42}}};
    auto pressed = raw(0x41);
    pressed.text = "unused caller payload";
    CHECK(!session->inputEvents({pressed}, {}, {}, options));
    CHECK(waitUntil([&] { return native->events().size() >= 3; }));
    CHECK(native->events().back().key.code == 0x41 && native->events().back().key.repeat);
    CHECK(native->events().back().text.empty());
    CHECK(native->transitions().back().code == 0x42);
    CHECK(!native->releaseAll());
    const auto count = native->transitions().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    CHECK(native->transitions().size() == count && session->state().empty());
}
void canonicalAliasesKeepOneFrozenRepeatBinding() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    DesktopInputOptions first;
    first.repeatDelayMs = 25;
    first.repeatPeriodMs = 15;
    first.keyBindings = {{{0x41, 0x42}}};
    const auto send = [&](const DesktopInputEvent& event, const DesktopInputOptions& options) {
        const auto end = Clock::now() + std::chrono::milliseconds(500);
        MaiError result;
        do {
            result = session->inputEvents({event}, {}, {}, options);
            if (result.code() != MaiErrorCode::Busy ||
                result.message() != "another ordinary input operation or cleanup is in flight")
                return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < end);
        return result;
    };
    CHECK(!send(raw(0x8041), first));
    auto changed = first;
    changed.keyBindings = {{{0x41, 0x43}}};
    auto alias = raw(0x41);
    alias.key.modifiers = 0x10;  // A duplicate cannot rewrite the first extended/native binding.
    CHECK(!send(alias, changed));
    CHECK(session->state().rawKeys.size() == 1 && session->state().rawKeys[0].code == 0x8041);
    CHECK(waitUntil([&] {
        const auto events = native->events();
        return std::any_of(events.begin(), events.end(), [](const auto& event) { return event.key.repeat; });
    }));
    const auto transitions = native->transitions();
    CHECK(std::count_if(transitions.begin(), transitions.end(),
                        [](const auto& key) { return !key.repeat && !key.release; }) == 1);
    CHECK(std::all_of(transitions.begin(), transitions.end(),
                      [](const auto& key) { return key.code == 0x8042 && key.modifiers == 0; }));
    CHECK(!send(raw(0x41, true), changed));
    CHECK(session->state().empty());
    const auto released = native->transitions();
    CHECK(!released.empty() && released.back().release && released.back().code == 0x8042);
    const auto stopped = native->events().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(native->events().size() == stopped);
}
void firstRepeatDelayStartsBeforeSlowNativeDispatch() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    DesktopInputOptions options;
    options.repeatDelayMs = 400;
    options.repeatPeriodMs = 40;
    std::mutex barrier;
    std::condition_variable changed;
    bool downEntered = false, returnDown = false, repeated = false;
    Clock::time_point enteredAt{}, returnedAt{}, repeatedAt{};
    native->afterTyped = [&] {
        const auto event = native->events().back();
        std::unique_lock<std::mutex> lock(barrier);
        if (event.key.repeat) {
            if (!repeated) repeatedAt = Clock::now();
            repeated = true;
            changed.notify_all();
        } else if (!downEntered) {
            downEntered = true;
            enteredAt = Clock::now();
            changed.notify_all();
            changed.wait(lock, [&] { return returnDown; });
            returnedAt = Clock::now();
        }
    };
    MaiError result;
    std::thread pending([&] { result = session->inputEvents({raw(0x75)}, {}, {}, options); });
    {
        std::unique_lock<std::mutex> lock(barrier);
        CHECK(changed.wait_for(lock, std::chrono::seconds(1), [&] { return downEntered; }));
        // The initial repeat deadline has elapsed before native returns. A
        // barrier preserves that ordering without making native inject input.
        const auto afterDelay = enteredAt + std::chrono::milliseconds(options.repeatDelayMs + 100);
        while (downEntered && Clock::now() < afterDelay) changed.wait_until(lock, afterDelay);
        returnDown = true;
        changed.notify_all();
    }
    pending.join();
    CHECK(!result);
    {
        std::unique_lock<std::mutex> lock(barrier);
        // A due repeat may wait for the worker, but must not wait another full
        // initial delay after the slow dispatch completes. The private virtual
        // clock audit separately proves the deadline without wall-clock bounds.
        CHECK(changed.wait_until(lock, returnedAt + std::chrono::milliseconds(options.repeatDelayMs / 2),
                                 [&] { return repeated; }));
        if (repeated) CHECK(repeatedAt >= returnedAt);
    }
    CHECK(!session->releaseAll());
    CHECK(session->state().empty());
    session.reset();  // Join any repeat callback before its captured barrier is destroyed.
}
void disablingRepeatStopsSchedulingWithoutInventingAnUp() {
    std::mutex barrier;
    std::condition_variable changed;
    bool repeatEntered = false, finishRepeat = false;
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    native->afterTyped = [&] {
        const auto events = native->events();
        if (events.empty() || !events.back().key.repeat) return;
        std::unique_lock<std::mutex> lock(barrier);
        if (repeatEntered) return;
        repeatEntered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return finishRepeat; });
    };
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, options));
    CHECK(waitUntil([&] { return native->events().size() > 1; }));
    {
        std::unique_lock<std::mutex> lock(barrier);
        CHECK(changed.wait_for(lock, std::chrono::seconds(1), [&] { return repeatEntered; }));
    }
    const auto before = native->events().size();
    const auto disabled = session->inputEvents({relative()}, {}, {}, noRepeat());
    std::printf("Repeat-disable first admission: code=%d error=%s\n", int(disabled.code()), disabled.message().c_str());
    CHECK(disabled.code() == MaiErrorCode::Busy &&
          disabled.message() == "another ordinary input operation or cleanup is in flight");
    CHECK(native->events().size() == before && native->options().keyboardRepeat);
    {
        std::lock_guard<std::mutex> lock(barrier);
        finishRepeat = true;
        changed.notify_all();
    }
    // Only this exact policy admission refusal has no effects. A native Busy,
    // target failure or Internal result must fail rather than be retried.
    auto admitted = disabled;
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    while (admitted.code() == MaiErrorCode::Busy &&
           admitted.message() == "another ordinary input operation or cleanup is in flight" &&
           Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        admitted = session->inputEvents({relative()}, {}, {}, noRepeat());
    }
    CHECK(!admitted);
    const auto events = native->events();
    CHECK(std::count_if(events.begin(), events.end(),
                        [](const auto& event) { return event.kind == Kind::RelativeMove; }) == 1);
    const auto count = native->transitions().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    CHECK(native->transitions().size() == count && session->state().rawKeys.size() == 1);
}
void policyLeavesSecureIntentToTheNative() {
    auto native = std::make_shared<Native>();
    native->secureFocus = true;
    // This isolated native fake has no real HWND. Inject only its target gate;
    // secure intent must still be classified by the exercised native recorder.
    auto session = wrap(native, [](const DesktopNativeInput& scope) {
        return scope.expectedWindowId == 101 && scope.expectedProcessId == 1
                   ? MaiError{}
                   : MaiError{MaiErrorCode::Busy, "fixture target identity changed"};
    });
    DesktopNativeInput scope;
    scope.expectedWindowId = 101;
    scope.expectedProcessId = 1;
    auto legacy = scope;
    legacy.kind = DesktopNativeInputKind::Key;
    for (const auto* name : {"Tab", "Escape", "Meta"}) {
        legacy.key = name;
        CHECK(!session->input(legacy));
    }
    legacy.key = "A";
    CHECK(session->input(legacy).code() == MaiErrorCode::NotSupported);
    CHECK(!session->inputEvents({raw(9)}, scope, {}, noRepeat()));
    CHECK(!session->inputEvents({raw(9, true)}, scope, {}, noRepeat()));
    CHECK(session->inputEvents({raw(0x41)}, scope, {}, noRepeat()).code() == MaiErrorCode::NotSupported);
    auto remapped = noRepeat();
    remapped.keyBindings = {{{9, 0x41}}};
    CHECK(session->inputEvents({raw(9)}, scope, {}, remapped).code() == MaiErrorCode::NotSupported);
}
void delaysAndCancellation() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
    auto up = raw(0x41, true);
    up.delayMs = 65;
    const auto before = Clock::now();
    CHECK(!session->inputEvents({up}, {}, {}, noRepeat()));
    CHECK(Clock::now() - before >= std::chrono::milliseconds(55));
    CHECK(!native->events().empty() && native->events().back().delayMs == 0 && session->state().empty());
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
    DesktopNativeInput canceled;
    canceled.isCanceled = [] { return true; };
    up.delayMs = 500;
    const auto started = Clock::now();
    CHECK(!session->inputEvents({up}, canceled, {}, noRepeat()));
    CHECK(Clock::now() - started < std::chrono::milliseconds(100) && session->state().empty());
    auto delayed = relative();
    delayed.delayMs = 200;
    std::atomic<bool> stop{false};
    DesktopNativeInput scope;
    scope.isCanceled = [&] { return stop.load(); };
    MaiError outcome;
    const auto count = native->events().size();
    std::thread pending([&] { outcome = session->inputEvents({delayed}, scope, {}, noRepeat()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    pending.join();
    CHECK(outcome.code() == MaiErrorCode::Canceled && native->events().size() == count);
    const auto largest = relative(2147483647);
    CHECK(!session->inputEvents({largest}, {}, {}, noRepeat()));
    CHECK(native->events().back().deltaX == 2147483647);
}
void optionInheritanceAndEnabledGates() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    auto configured = noRepeat();
    configured.alwaysSendScanCodes = false;
    configured.rightAltToMeta = true;
    CHECK(!session->inputEvents({relative()}, {}, {}, configured));
    CHECK(session->state().empty());
    CHECK(session->state().devices.empty());
    DesktopInputOptions inherited;
    inherited.inheritExisting = true;
    CHECK(!session->inputEvents({relative()}, {}, {}, inherited));
    CHECK(!native->options().alwaysSendScanCodes && native->options().rightAltToMeta &&
          !native->options().inheritExisting);
    CHECK(!session->inputEvents({relative()}, {}, {}, noRepeat()));
    CHECK(native->options().alwaysSendScanCodes && !native->options().rightAltToMeta);
    auto disabled = noRepeat();
    disabled.mouseEnabled = false;
    const auto before = native->events().size();
    CHECK(session->inputEvents({relative()}, {}, {}, disabled).code() == MaiErrorCode::NotSupported);
    CHECK(native->events().size() == before);
    DesktopInputEvent deferred;
    deferred.kind = Kind::GamepadAttach;
    CHECK(session->inputEvents({deferred}, {}, {}, noRepeat()).code() == MaiErrorCode::NotSupported);
}
void literalTextSharesNativeLedger() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    CHECK(!session->inputEvents({raw(0x11)}, {}, {}, noRepeat()));
    DesktopInputEvent text;
    text.kind = Kind::UnicodeText;
    text.text = "owned literal";
    CHECK(session->inputEvents({text}, {}, {}, noRepeat()).code() == MaiErrorCode::Busy);
    CHECK(session->state().empty());
    CHECK(!session->inputEvents({raw(0x11)}, {}, {}, noRepeat()));
    DesktopNativeInput ordinary;
    ordinary.kind = DesktopNativeInputKind::Type;
    ordinary.text = "owned literal";
    CHECK(session->input(ordinary).code() == MaiErrorCode::Busy && session->state().empty());
}
void cleanupKeepsConfigurationAndBusyStateIsNotEmpty() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    auto configured = noRepeat();
    configured.alwaysSendScanCodes = false;
    configured.rightAltToMeta = true;
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, configured));
    CHECK(!session->inputEvents({raw(0x41, true)}, {}, {}, noRepeat()));
    CHECK(!native->options().alwaysSendScanCodes && native->options().rightAltToMeta);
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, configured));
    native->cleanupError = {MaiErrorCode::Busy, "native cleanup is pending"};
    CHECK(session->releaseAll().code() == MaiErrorCode::Busy);
    CHECK(session->state().rawKeys.size() == 1);
    native->cleanupError = {};
    CHECK(!native->releaseAll() && session->state().empty());
}
void legacyEnabledGateAndPostDispatchCancellation() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    auto disabled = noRepeat();
    disabled.keyboardEnabled = false;
    CHECK(!session->inputEvents({relative()}, {}, {}, disabled));
    DesktopNativeInput text;
    text.kind = DesktopNativeInputKind::Type;
    text.text = "owned literal";
    CHECK(session->input(text).code() == MaiErrorCode::NotSupported && native->ordinaryCalls == 0);
    std::atomic<bool> stopped{false};
    DesktopNativeInput scope;
    scope.isCanceled = [&] { return stopped.load(); };
    native->afterTyped = [&] { stopped = true; };
    CHECK(session->inputEvents({raw(0x41)}, scope, {}, noRepeat()).code() == MaiErrorCode::Canceled);
    CHECK(session->state().empty());
    native->afterTyped = {};
}
void operationSerializationAndBusyReset() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    native->blockOrdinary = true;
    MaiError completed;
    std::thread active([&] { completed = session->input({}); });
    {
        std::unique_lock<std::mutex> lock(native->blockMutex);
        CHECK(native->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return native->ordinaryEntered; }));
    }
    CHECK(session->inputEvents({relative()}, {}, {}, noRepeat()).code() == MaiErrorCode::Busy);
    CHECK(native->events().empty());
    CHECK(session->resetControls().code() == MaiErrorCode::Busy && native->resets == 0);
    {
        std::lock_guard<std::mutex> lock(native->blockMutex);
        native->proceed = true;
    }
    native->changed.notify_all();
    active.join();
    CHECK(!completed);
    CHECK(!session->resetControls() && native->resets == 1);
}
void resetStopsRepeatAndKeepsScopeValidation() {
    auto native = std::make_shared<Native>();
    std::atomic<bool> targetValid{true};
    std::mutex repeatBarrier;
    std::condition_variable repeatChanged;
    bool repeatEntered = false, returnRepeat = false;
    auto session = wrap(
        native, [&](const auto&) { return targetValid ? MaiError{} : MaiError{MaiErrorCode::Busy, "target lost"}; });
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    native->afterTyped = [&] {
        if (native->events().size() <= 1) return;
        std::unique_lock<std::mutex> lock(repeatBarrier);
        repeatEntered = true;
        repeatChanged.notify_all();
        repeatChanged.wait(lock, [&] { return returnRepeat; });
    };
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, options));
    CHECK(waitUntil([&] { return native->events().size() > 1; }));
    {
        std::unique_lock<std::mutex> lock(repeatBarrier);
        CHECK(repeatChanged.wait_for(lock, std::chrono::milliseconds(400), [&] { return repeatEntered; }));
    }
    CHECK(session->resetControls().code() == MaiErrorCode::Busy && native->resets == 0);
    {
        std::lock_guard<std::mutex> lock(repeatBarrier);
        returnRepeat = true;
        repeatChanged.notify_all();
    }
    MaiError reset;
    bool settled = false;
    CHECK(waitUntil([&] {
        if (settled) return true;
        reset = session->resetControls();
        settled = reset.code() != MaiErrorCode::Busy ||
                  reset.message() != "another ordinary input operation or cleanup is in flight";
        return settled;
    }));
    CHECK(!reset);
    const auto count = native->events().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(native->events().size() == count && native->resets == 1 && session->state().empty());
    targetValid = false;
    CHECK(session->validate());
    CHECK(!session->resetControls());
    native->afterTyped = {};
}
void scopeAndWatchdogValidationStayOutsidePolicyLock() {
    auto native = std::make_shared<Native>();
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, proceed = false;
    auto session = wrap(native, [&](const auto&) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return proceed; });
        return MaiError{};
    });
    MaiError outcome;
    std::thread input([&] { outcome = session->inputEvents({relative()}, {}, {}, noRepeat()); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return entered; }));
    }
    const auto before = Clock::now();
    CHECK(session->state().empty());
    CHECK(!session->releaseAll());
    CHECK(Clock::now() - before < std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
    }
    changed.notify_all();
    input.join();
    CHECK(outcome.code() == MaiErrorCode::Canceled && native->events().empty());
    CHECK(!session->inputEvents({relative()}, {}, {}, noRepeat()));
    {
        std::lock_guard<std::mutex> lock(mutex);
        entered = false;
        proceed = false;
    }
    std::thread watch([&] { CHECK(!session->validate()); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return entered; }));
    }
    CHECK(session->state().empty());
    {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
    }
    changed.notify_all();
    watch.join();
}
void asyncFailureAndBlockedCleanupKeepStateReadable() {
    auto native = std::make_shared<Native>();
    std::atomic<bool> valid{true};
    auto session =
        wrap(native, [&](const auto&) { return valid ? MaiError{} : MaiError{MaiErrorCode::Busy, "target lost"}; });
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, options));
    native->blockRelease = true;
    valid = false;
    {
        std::unique_lock<std::mutex> lock(native->blockMutex);
        CHECK(native->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return native->releaseEntered; }));
    }
    const auto before = Clock::now();
    CHECK(!session->state().empty());
    CHECK(session->validate());
    CHECK(Clock::now() - before < std::chrono::milliseconds(100));
    CHECK(session->inputEvents({relative()}, {}, {}, noRepeat()).code() == MaiErrorCode::Busy);
    {
        std::lock_guard<std::mutex> lock(native->blockMutex);
        native->releaseProceed = true;
    }
    native->changed.notify_all();
    CHECK(waitUntil([&] { return session->state().empty(); }));
    CHECK(session->validate());
    CHECK(!session->releaseAll());
}
void releaseRevokesSchedulesBeforeNativeCleanupReturns() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, options));
    CHECK(waitUntil([&] { return native->events().size() > 1; }));
    native->blockRelease = true;
    std::thread cleanup([&] { CHECK(!session->releaseAll()); });
    {
        std::unique_lock<std::mutex> lock(native->blockMutex);
        CHECK(native->changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return native->releaseEntered; }));
    }
    const auto count = native->events().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    CHECK(native->events().size() == count);
    CHECK(session->resetControls().code() == MaiErrorCode::Busy);
    {
        std::lock_guard<std::mutex> lock(native->blockMutex);
        native->releaseProceed = true;
    }
    native->changed.notify_all();
    cleanup.join();
    CHECK(session->state().empty());
}
void staleWatchSnapshotCannotRejectAReplacementScope() {
    auto native = std::make_shared<Native>();
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, entered = false, proceed = false;
    auto session = wrap(native, [&](const auto& scope) {
        std::unique_lock<std::mutex> lock(mutex);
        if (block && scope.expectedWindowId == 101) {
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return proceed; });
            return MaiError{MaiErrorCode::Busy, "old scope disappeared"};
        }
        return MaiError{};
    });
    DesktopNativeInput first;
    first.expectedWindowId = 101;
    CHECK(!session->inputEvents({relative()}, first, {}, noRepeat()));
    {
        std::lock_guard<std::mutex> lock(mutex);
        block = true;
    }
    MaiError validation;
    std::thread watch([&] { validation = session->validate(); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return entered; }));
    }
    DesktopNativeInput next;
    next.expectedWindowId = 202;
    CHECK(!session->inputEvents({relative()}, next, {}, noRepeat()));
    {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
    }
    changed.notify_all();
    watch.join();
    CHECK(!validation);
}
}  // namespace

void freshLegacyHoldDoesNotInheritAResolvedAsyncFailure() {
    auto native = std::make_shared<Native>();
    auto session = wrap(native);
    native->failAt = 2;
    DesktopInputOptions options;
    options.repeatDelayMs = 10;
    options.repeatPeriodMs = 10;
    CHECK(!session->inputEvents({raw(0x41)}, {}, {}, options));
    CHECK(waitUntil([&] { return native->releases > 0 && session->state().empty(); }));
    CHECK(session->validate().code() == MaiErrorCode::Internal);
    DesktopNativeInput legacy;
    legacy.kind = DesktopNativeInputKind::KeyDown;
    legacy.key = "Z";
    MaiError admitted;
    bool submitted = false;
    CHECK(waitUntil([&] {
        if (submitted) return true;
        admitted = session->input(legacy);
        submitted = admitted.code() != MaiErrorCode::Busy;
        return submitted;
    }));
    CHECK(!admitted && !session->state().empty());
    CHECK(!session->validate());
    legacy.kind = DesktopNativeInputKind::KeyUp;
    CHECK(!session->input(legacy) && session->state().empty());
}

void fractionalRepeatPolicyAndDefaultReset() {
    for (const double period : {17.5, 1500.25, 2147483647.0, std::numeric_limits<double>::denorm_min()}) {
        auto native = std::make_shared<Native>();
        auto session = wrap(native);
        auto options = noRepeat();
        options.repeatDelayMs = 2147483647;
        options.repeatPeriodMs = static_cast<decltype(options.repeatPeriodMs)>(period);
        const auto configured = session->inputEvents({relative()}, {}, {}, options);
        CHECK(!configured);
        if (configured) continue;
        CHECK(native->options().repeatDelayMs == 2147483647 && native->options().repeatPeriodMs == period);
        DesktopInputOptions inherited;
        inherited.inheritExisting = true;
        CHECK(!session->inputEvents({relative()}, {}, {}, inherited));
        CHECK(native->options().repeatPeriodMs == period && native->options().repeatDelayMs == 2147483647);
        CHECK(!session->inputEvents({relative()}, {}, {}, {}));
        CHECK(native->options().repeatPeriodMs == 1000.0 / 24.9 && native->options().repeatDelayMs == 500);
    }
    for (const double period : {0.5, 1500.25, 2147483647.0, std::numeric_limits<double>::denorm_min()}) {
        auto native = std::make_shared<Native>();
        auto session = wrap(native);
        DesktopInputOptions options;
        options.repeatDelayMs = 1;
        options.repeatPeriodMs = static_cast<decltype(options.repeatPeriodMs)>(period);
        const auto pressed = session->inputEvents({raw(0x41)}, {}, {}, options);
        CHECK(!pressed);
        if (pressed) continue;
        CHECK(waitUntil([&] { return native->events().size() > 1; }));
        CHECK(native->options().repeatPeriodMs == period);
        CHECK(!session->releaseAll() && session->state().empty());
    }
}

void reenterReadInputResetAndRelease(const std::shared_ptr<DesktopNativeInputSession>& session, bool held) {
    CHECK(session->state().empty() != held);
    CHECK(!session->validate());
    CHECK(session->capabilities().baseline == "recording-native");
    CHECK(session->feedback(13, 2).nextSequence == 13);
    CHECK(session->input({}).code() == MaiErrorCode::Busy);
    CHECK(session->inputEvents({relative()}, {}, {}, noRepeat()).code() == MaiErrorCode::Busy);
    CHECK(session->resetControls().code() == MaiErrorCode::Busy);
    CHECK(!session->releaseAll() && session->state().empty());
}
void callerCallbacksCanReenterWithoutOwnedMutexAttempts() {
    for (const bool ordinary : {false, true}) {
        auto native = std::make_shared<Native>();
        int validations = 0, validationsAtRevoke = -1;
        auto session = wrap(native, [&](const auto&) {
            ++validations;
            return MaiError{};
        });
        CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
        const auto sent = native->events().size();
        int calls = 0;
        bool reentered = false;
        DesktopNativeInput scope;
        scope.isCanceled = [&] {
            if (++calls == (ordinary ? 1 : 2)) {
                reentered = true;
                reenterReadInputResetAndRelease(session, true);
                validationsAtRevoke = validations;
            }
            return false;  // Epoch revocation must work independently of this result.
        };
        auto delayed = relative();
        delayed.delayMs = 50;
        const auto result = ordinary ? session->input(scope) : session->inputEvents({delayed}, scope, {}, noRepeat());
        CHECK(reentered && result.code() == MaiErrorCode::Canceled);
        CHECK(validations == validationsAtRevoke);
        CHECK(native->ordinaryCalls == 0 && native->events().size() == sent && session->state().empty());
    }
}
void validatorAndNativeCallbacksCanReenter() {
    {
        auto native = std::make_shared<Native>();
        std::shared_ptr<DesktopNativeInputSession> session;
        bool armed = false, ran = false;
        session = wrap(native, [&](const auto&) {
            if (armed && !ran) {
                ran = true;
                reenterReadInputResetAndRelease(session, true);
            }
            return MaiError{};
        });
        CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
        armed = true;
        const auto before = native->events().size();
        CHECK(session->inputEvents({relative()}, {}, {}, noRepeat()).code() == MaiErrorCode::Canceled);
        CHECK(ran && native->events().size() == before && session->state().empty());
    }
    {
        auto native = std::make_shared<Native>();
        auto session = wrap(native);
        CHECK(!session->inputEvents({raw(0x41)}, {}, {}, noRepeat()));
        const auto before = native->events().size();
        bool ran = false;
        native->afterTyped = [&] {
            if (!ran) {
                ran = true;
                reenterReadInputResetAndRelease(session, true);
            }
        };
        CHECK(session->inputEvents({relative(2), relative(3)}, {}, {}, noRepeat()).code() == MaiErrorCode::Canceled);
        CHECK(ran && native->events().size() == before + 1 && session->state().empty());
        native->afterTyped = {};
        ran = false;
        native->afterReset = [&] {
            if (!ran) {
                ran = true;
                reenterReadInputResetAndRelease(session, false);
            }
        };
        CHECK(!session->resetControls());
        CHECK(ran && native->resets == 1 && session->state().empty());
        native->afterReset = {};
    }
}

int main() {
    forwardingAndCallbackLifetime();
    preflightAndCleanupFailures();
    nativeOwnsAliasesAndFrozenBindings();
    repeatUsesOnlyNativeSourceProjection();
    canonicalAliasesKeepOneFrozenRepeatBinding();
    firstRepeatDelayStartsBeforeSlowNativeDispatch();
    disablingRepeatStopsSchedulingWithoutInventingAnUp();
    policyLeavesSecureIntentToTheNative();
    delaysAndCancellation();
    optionInheritanceAndEnabledGates();
    literalTextSharesNativeLedger();
    cleanupKeepsConfigurationAndBusyStateIsNotEmpty();
    legacyEnabledGateAndPostDispatchCancellation();
    operationSerializationAndBusyReset();
    resetStopsRepeatAndKeepsScopeValidation();
    scopeAndWatchdogValidationStayOutsidePolicyLock();
    asyncFailureAndBlockedCleanupKeepStateReadable();
    releaseRevokesSchedulesBeforeNativeCleanupReturns();
    staleWatchSnapshotCannotRejectAReplacementScope();
    freshLegacyHoldDoesNotInheritAResolvedAsyncFailure();
    callerCallbacksCanReenterWithoutOwnedMutexAttempts();
    fractionalRepeatPolicyAndDefaultReset();
    validatorAndNativeCallbacksCanReenter();
#if defined(MAI_COMMON_MUTEX_AUDIT)
    CHECK(commonMutexReentryAttempts() == 0);
#endif
    std::cout << "Mono native input session failures: " << failures << '\n';
    std::cout << "Mono native input session checks: " << checks << '\n';
    return failures ? 1 : 0;
}
