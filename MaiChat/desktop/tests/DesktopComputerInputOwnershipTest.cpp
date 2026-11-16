// Deterministic ownership/cleanup contracts. No OS input or capture is used.
#include "MaiChatTools/DesktopComputerInputOwnership.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(value)                                                 \
    do {                                                             \
        if (!(value)) {                                              \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                              \
        }                                                            \
    } while (false)

class NativeSession final : public DesktopNativeInputSession {
public:
    mutable std::mutex mutex;
    std::condition_variable released;
    DesktopNativeInputState held;
    bool refuseRelease = false;
    bool invalidTarget = false;
    std::function<MaiError()> onValidate;
    int releaseCalls = 0;
    MaiError input(const DesktopNativeInput& request) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (request.kind == DesktopNativeInputKind::KeyDown) held.keysDown.push_back(request.key);
        return {};
    }
    DesktopNativeInputState state() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return held;
    }
    MaiError validate() const override {
        std::function<MaiError()> callback;
        bool invalid = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = onValidate;
            invalid = invalidTarget;
        }
        if (callback) return callback();
        return invalid ? MaiError{MaiErrorCode::Busy, "owned target changed"} : MaiError{};
    }
    MaiError releaseAll() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++releaseCalls;
        if (refuseRelease) return {MaiErrorCode::Busy, "fixture release refused"};
        held = {};
        released.notify_all();
        return {};
    }
    bool waitForRelease() {
        std::unique_lock<std::mutex> lock(mutex);
        return released.wait_for(lock, std::chrono::seconds(2), [&] { return held.empty() && releaseCalls > 0; });
    }
};

class Backend final : public DesktopComputerBackend {
public:
    std::shared_ptr<NativeSession> last;
    int creations = 0;
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
        ++creations;
        last = std::make_shared<NativeSession>();
        return last;
    }
    MaiResult<std::vector<DesktopNativeWindow>> windows() override {
        return std::vector<DesktopNativeWindow>{};
    }
    MaiResult<std::vector<DesktopNativeElement>> elements(const DesktopNativeWindow&) override {
        return std::vector<DesktopNativeElement>{};
    }
    MaiError capture(const DesktopNativeWindow&, MaiScreenshot&) override {
        return {MaiErrorCode::NotSupported, "no capture"};
    }
    MaiError activate(const DesktopNativeWindow&) override {
        return {};
    }
    MaiResult<bool> isFocused(const DesktopNativeWindow&) override {
        return true;
    }
    MaiResult<bool> secureInput() override {
        return false;
    }
    MaiError input(const DesktopNativeInput&) override {
        return {MaiErrorCode::Internal, "legacy input was not expected"};
    }
    MaiResult<std::string> focusedText(const DesktopNativeWindow&) override {
        return std::string();
    }
};

DesktopComputerAction action(std::string turn = "turn-a", std::uint64_t target = 11) {
    DesktopComputerAction result;
    result.action = "key_down";
    result.desktopScope = true;
    result.displayId = target;
    result.turnId = std::move(turn);
    return result;
}

std::shared_ptr<DesktopNativeInputSession> hold(DesktopComputerInputOwnership& owner,
                                                const DesktopComputerAction& target = action()) {
    const auto prepared = owner.prepare("session", target, true);
    CHECK(prepared);
    if (!prepared) return {};
    DesktopNativeInput down;
    down.kind = DesktopNativeInputKind::KeyDown;
    down.key = "Ctrl";
    CHECK(!prepared.value()->input(down));
    return prepared.value();
}

void ownershipAndTargets() {
    auto backend = std::make_shared<Backend>();
    int invalidations = 0;
    DesktopComputerInputOwnership owner(backend, [&] { ++invalidations; });
    const auto native = hold(owner);
    CHECK(native && !owner.state("session").empty());
    owner.touch("session");
    CHECK(owner.state("session").keysDown == std::vector<std::string>{"Ctrl"});
    CHECK(owner.state("session").idleRemainingMs > 0);
    CHECK(!owner.prepare("session", action("turn-a", 22), false));
    CHECK(!owner.prepare("session", action("turn-b"), false));
    CHECK(!owner.prepare("other-session", action(), false));
    auto otherBackend = std::make_shared<Backend>();
    DesktopComputerInputOwnership other(otherBackend, [] {});
    CHECK(!other.prepare("session", action(), false));
    other.touch("session");  // Observation does not acquire or release global input.
    CHECK(!native->state().empty());
    CHECK(!owner.release("session", "old-turn"));
    CHECK(!native->state().empty());
    CHECK(!owner.release("session", "turn-a"));
    CHECK(native->state().empty() && invalidations == 1);
    CHECK(other.prepare("session", action(), false));
    const auto replacement = hold(owner, action("turn-b"));
    CHECK(!owner.release("session", "turn-a"));
    CHECK(replacement && !replacement->state().empty());
    CHECK(!owner.release("session", "turn-b"));
}

void expiryAndTargetFailure() {
    auto backend = std::make_shared<Backend>();
    std::atomic<int> invalidations{0};
    DesktopComputerInputOwnership owner(backend, [&] { ++invalidations; }, std::chrono::milliseconds(15));
    const auto native = hold(owner);
    CHECK(backend->last->waitForRelease());
    // Native release notification precedes the coordinator's invalidation.
    // Synchronize through its public state before asserting that transaction.
    CHECK(owner.state("session").empty());
    CHECK(native->state().empty() && invalidations.load() > 0);
    CHECK(!owner.prepare("session", action(), false));
    owner.touch("session");
    CHECK(owner.prepare("session", action(), false));

    auto backend2 = std::make_shared<Backend>();
    DesktopComputerInputOwnership targetOwner(backend2, [] {});
    const auto target = hold(targetOwner);
    {
        std::lock_guard<std::mutex> lock(backend2->last->mutex);
        backend2->last->invalidTarget = true;
    }
    CHECK(backend2->last->waitForRelease());
    CHECK(target->state().empty());
}

void cleanupFailuresAndDestruction() {
    auto backend = std::make_shared<Backend>();
    std::shared_ptr<DesktopNativeInputSession> native;
    {
        DesktopComputerInputOwnership owner(backend, [] {});
        native = hold(owner);
        {
            std::lock_guard<std::mutex> lock(backend->last->mutex);
            backend->last->refuseRelease = true;
        }
        CHECK(owner.release("session"));
        CHECK(!native->state().empty());
        {
            std::lock_guard<std::mutex> lock(backend->last->mutex);
            backend->last->refuseRelease = false;
        }
    }
    CHECK(native->state().empty());
}

void watchdogValidationAllowsGuiOwnerStateReads() {
    auto backend = std::make_shared<Backend>();
    DesktopComputerInputOwnership owner(backend, [] {});
    auto native = hold(owner);
    struct Probe {
        std::atomic<bool> started{false}, finished{false}, observed{false};
    };
    auto probe = std::make_shared<Probe>();
    std::thread gui;
    {
        std::lock_guard<std::mutex> lock(backend->last->mutex);
        backend->last->onValidate = [&, probe] {
            if (probe->started.exchange(true)) return MaiError{};
            auto done = std::make_shared<std::atomic<bool>>(false);
            gui = std::thread([&, done] {
                owner.state("session");
                done->store(true);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
            while (!done->load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            probe->observed.store(done->load());
            const auto result =
                probe->observed.load() ? MaiError{} : MaiError{MaiErrorCode::Busy, "GUI owner read blocked"};
            probe->finished.store(true);
            return result;
        };
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!probe->finished.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(probe->finished.load());
    if (gui.joinable()) gui.join();
    CHECK(probe->observed.load());
    CHECK(native && !native->state().empty());
    CHECK(!owner.release("session"));
    {
        std::lock_guard<std::mutex> lock(backend->last->mutex);
        backend->last->onValidate = {};
    }
}

void emptyNativeContextRetainsPolicyWithoutOwningTheDesktop() {
    auto backend = std::make_shared<Backend>();
    DesktopComputerInputOwnership owner(backend, [] {});
    const auto first = owner.prepare("empty-context", action(), true);
    CHECK(first && first.value()->state().empty());
    owner.touch("empty-context", "turn-a");
    const auto next = owner.prepare("empty-context", action(), true);
    CHECK(next && first && next.value() == first.value());
    CHECK(backend->creations == 1);

    // Retaining options/remainders is not a synthetic physical hold. Another
    // owner can still acquire input, and cleanup of this empty context is safe.
    auto otherBackend = std::make_shared<Backend>();
    DesktopComputerInputOwnership other(otherBackend, [] {});
    const auto held = hold(other);
    CHECK(held && !held->state().empty());
    CHECK(!owner.release("empty-context", "old-turn"));
    CHECK(!other.release("session", "turn-a"));

    CHECK(!owner.reset("empty-context", "turn-a"));
    const auto resetContext = owner.prepare("empty-context", action(), true);
    CHECK(resetContext && first && resetContext.value() == first.value());
    CHECK(!owner.release("empty-context", "turn-a"));
    const auto fresh = owner.prepare("empty-context", action(), true);
    CHECK(fresh && first && fresh.value() != first.value());
    CHECK(!owner.release("empty-context", "turn-a"));
}

void emptyContextDoesNotCrossTaskTargetOrIdleExpiry() {
    auto backend = std::make_shared<Backend>();
    DesktopComputerInputOwnership owner(backend, [] {}, std::chrono::milliseconds(20));
    const auto first = owner.prepare("context", action(), true);
    const auto changedTurn = owner.prepare("context", action("turn-b"), true);
    CHECK(first && changedTurn && first.value() != changedTurn.value());
    const auto changedTarget = owner.prepare("context", action("turn-b", 22), true);
    CHECK(changedTurn && changedTarget && changedTurn.value() != changedTarget.value());
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    const auto afterIdle = owner.prepare("context", action("turn-b", 22), true);
    CHECK(changedTarget && afterIdle && changedTarget.value() != afterIdle.value());
    CHECK(!owner.release("context", "turn-b"));
}

void observationCannotRenewAnExpiredEmptyContext() {
    struct Probe {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool resume = false;
    };
    auto probe = std::make_shared<Probe>();
    auto backend = std::make_shared<Backend>();
    DesktopComputerInputOwnership owner(backend, [] {}, std::chrono::milliseconds(100));
    const auto oldContext = owner.prepare("empty-context", action(), true);
    const auto oldNative = backend->last;
    const auto blocker = owner.prepare("watch-blocker", action(), true);
    CHECK(oldContext && blocker);
    if (!oldContext || !blocker) return;
    const auto blockingNative = backend->last;
    {
        std::lock_guard<std::mutex> lock(blockingNative->mutex);
        blockingNative->onValidate = [probe] {
            std::unique_lock<std::mutex> lock(probe->mutex);
            probe->entered = true;
            probe->changed.notify_all();
            probe->changed.wait(lock, [&] { return probe->resume; });
            return MaiError{};
        };
    }
    DesktopNativeInput down;
    down.kind = DesktopNativeInputKind::KeyDown;
    down.key = "Ctrl";
    CHECK(!blocker.value()->input(down));
    owner.touch("watch-blocker", "turn-a");
    {
        std::unique_lock<std::mutex> lock(probe->mutex);
        CHECK(probe->changed.wait_for(lock, std::chrono::seconds(2), [&] { return probe->entered; }));
    }
    // Keep the watchdog deliberately outside its owner lock. Observation must
    // enforce the idle boundary itself even before that watchdog returns.
    std::this_thread::sleep_for(std::chrono::milliseconds(130));
    owner.touch("empty-context", "turn-a");
    CHECK(!owner.release("watch-blocker", "turn-a"));
    const auto refreshed = owner.prepare("empty-context", action(), true);
    CHECK(refreshed && refreshed.value() != oldContext.value());
    CHECK(oldNative->releaseCalls > 0);
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->resume = true;
    }
    probe->changed.notify_all();
    CHECK(!owner.release("empty-context", "turn-a"));
}
}  // namespace

int main() {
    ownershipAndTargets();
    expiryAndTargetFailure();
    cleanupFailuresAndDestruction();
    watchdogValidationAllowsGuiOwnerStateReads();
    emptyNativeContextRetainsPolicyWithoutOwningTheDesktop();
    emptyContextDoesNotCrossTaskTargetOrIdleExpiry();
    observationCannotRenewAnExpiredEmptyContext();
    std::printf("desktop input ownership: %d failures\n", failures);
    return failures ? 1 : 0;
}
