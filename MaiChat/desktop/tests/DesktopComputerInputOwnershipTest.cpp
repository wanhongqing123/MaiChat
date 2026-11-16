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
        std::lock_guard<std::mutex> lock(mutex);
        return invalidTarget ? MaiError{MaiErrorCode::Busy, "owned target changed"} : MaiError{};
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
    std::shared_ptr<DesktopNativeInputSession> createInputSession() override {
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
}  // namespace

int main() {
    ownershipAndTargets();
    expiryAndTargetFailure();
    cleanupFailuresAndDestruction();
    std::printf("desktop input ownership: %d failures\n", failures);
    return failures ? 1 : 0;
}
