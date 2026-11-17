// Deterministic atomic-frame contracts. No HWND, device, driver or OS input.
#include "MaiChatTools/DesktopComputerWindowsTouchInternal.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <set>
#include <atomic>
#include <thread>
#include <future>
#include <stdexcept>
#include "MaiThread.h"

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); ++failures; } } while (false)
using Phase = DesktopInputPhase;

class Transport final : public DesktopWindowsTouchTransport {
public:
    MaiError create(std::uint32_t capacity) override {
        std::lock_guard<std::mutex> lock(mutex);
        nativeThreads.insert(std::this_thread::get_id());
        nativeNames.insert(MaiThread::currentName());
        ++creates;
        requestedCapacity = capacity;
        if (failCreate) return {MaiErrorCode::NotConfigured, "fixture create failed"};
        live = true;
        return {};
    }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame& frame) override {
        std::unique_lock<std::mutex> lock(mutex);
        if (!live) ++injectAfterDestroy;
        nativeThreads.insert(std::this_thread::get_id());
        frames.push_back(frame);
        changed.notify_all();
        if (blockNext || blockAtFrame == frames.size()) {
            blockNext = false;
            blocked = true;
            changed.notify_all();
            changed.wait(lock, [&] { return unblock; });
        }
        if (duringInject) duringInject();
        if (throwNext) {
            throwNext = false;
            throw std::runtime_error("fixture possible native submission");
        }
        if (results.empty()) return {};
        const auto result = results.front();
        results.pop_front();
        return result;
    }
    MaiError destroy() override {
        std::lock_guard<std::mutex> lock(mutex);
        nativeThreads.insert(std::this_thread::get_id());
        ++destroys;
        if (failDestroy) return {MaiErrorCode::Internal, "fixture destroy failed"};
        live = false;
        return {};
    }
    std::vector<DesktopWindowsTouchFrame> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return frames;
    }
    bool waitForFrames(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(1), [&] { return frames.size() >= count; });
    }
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<DesktopWindowsTouchFrame> frames;
    std::deque<DesktopWindowsTouchSubmission> results;
    std::function<void()> duringInject;
    bool live = false, failCreate = false, failDestroy = false;
    bool blockNext = false, blocked = false, unblock = false;
    std::size_t blockAtFrame = 0;
    std::set<std::thread::id> nativeThreads;
    std::set<std::string> nativeNames;
    bool throwNext = false;
    int creates = 0, destroys = 0, injectAfterDestroy = 0;
    std::uint32_t requestedCapacity = 0;
};

DesktopInputPort port() {
    DesktopInputPort result;
    result.x = 100;
    result.y = 200;
    result.width = 1001;
    result.height = 501;
    result.environmentWidth = 3840;
    result.environmentHeight = 2160;
    return result;
}

DesktopInputTouch contact(std::uint32_t id, Phase phase = Phase::Down, float x = .5F) {
    DesktopInputTouch result;
    result.pointerId = id;
    result.phase = phase;
    result.x = x;
    result.y = .5F;
    result.pressureOrDistance = .5F;
    return result;
}

auto backend(const std::shared_ptr<Transport>& transport) {
    return makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(0));
}

template <class Predicate>
bool eventually(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

void tenBirthsAndWholePreflight() {
    auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    auto outside = port();
    outside.x = 3000;
    const auto rejected = touch->dispatch({contact(1, Phase::Down, 0), contact(2, Phase::Down, 1)}, outside);
    CHECK(rejected.code() == MaiErrorCode::InvalidInput);
    CHECK(transport->snapshot().empty() && transport->creates == 0 && touch->state().empty());
    CHECK(touch->positions().empty() && !touch->validate());
    std::vector<DesktopInputTouch> inputs;
    for (std::uint32_t id = 0; id < 10; ++id) inputs.push_back(contact(100 + id));
    CHECK(!touch->dispatch(inputs, port()));
    auto frames = transport->snapshot();
    CHECK(frames.size() == 11);
    unsigned births = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(frames[i].points.size() == (i < 2 ? 1 : i));
        unsigned thisBirth = 0;
        for (const auto& point : frames[i].points) {
            CHECK(point.pointerId < 10);
            thisBirth += (point.flags & kDesktopTouchDown) != 0;
        }
        CHECK(thisBirth == (i == 1 ? 0u : 1u));
        births += thisBirth;
    }
    CHECK(births == 10 && touch->state().touchPointers.size() == 10 && touch->positions().size() == 10);
    const auto before = transport->snapshot().size();
    auto bad = port();
    bad.width = std::numeric_limits<double>::infinity();
    CHECK(touch->dispatch({contact(100, Phase::Up), contact(500)}, bad).code() == MaiErrorCode::InvalidInput);
    CHECK(transport->snapshot().size() == before && touch->state().touchPointers.size() == 10);
    CHECK(!touch->releaseAll());
    CHECK(transport->nativeNames == std::set<std::string>{"mai-input-touch"});
}

void stopWaitingAndReentrantCallback() {
    auto transport = std::make_shared<Transport>();
    auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(500));
    auto pending = std::async(std::launch::async,
                              [&] { return touch->dispatch({contact(1), contact(2, Phase::Down, .75F)}, port()); });
    CHECK(eventually([&] { return touch->state().touchPointers == std::vector<std::uint32_t>{1}; }));
    const auto beforeStop = std::chrono::steady_clock::now();
    CHECK(touch->releaseAll().code() == MaiErrorCode::Busy);
    CHECK(pending.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready);
    CHECK(pending.get().code() == MaiErrorCode::Canceled);
    CHECK(std::chrono::steady_clock::now() - beforeStop < std::chrono::milliseconds(200));
    CHECK(touch->state().empty() && touch->positions().empty());
    unsigned births = 0;
    for (const auto& frame : transport->snapshot())
        for (const auto& point : frame.points) births += (point.flags & kDesktopTouchDown) != 0;
    CHECK(births == 1);
    CHECK(!touch->releaseAll());
    bool triggered = false;
    CHECK(touch
              ->dispatch({contact(5), contact(6)}, port(),
                         [&] {
                             CHECK(touch->positions().size() <= 1);
                             if (!triggered && !touch->state().touchPointers.empty()) {
                                 triggered = true;
                                 CHECK(touch->dispatch({contact(9)}, port()).code() == MaiErrorCode::Busy);
                                 CHECK(touch->releaseAll().code() == MaiErrorCode::Busy);
                             }
                             return false;  // A false callback cannot adopt a concurrently revoked epoch.
                         })
              .code() == MaiErrorCode::Canceled);
    CHECK(triggered && touch->state().empty());
    CHECK(!touch->releaseAll());
}

void livePositionsAndCallerLifetime() {
    auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    CHECK(!touch->dispatch({contact(1)}, port()));
    const auto original = touch->positions();
    CHECK(original == (std::vector<std::pair<std::int32_t, std::int32_t>>{{600, 450}}));
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        transport->blockNext = true;
    }
    auto pending =
        std::async(std::launch::async, [&] { return touch->dispatch({contact(1, Phase::Move, .75F)}, port()); });
    {
        std::unique_lock<std::mutex> lock(transport->mutex);
        CHECK(transport->changed.wait_for(lock, std::chrono::seconds(1), [&] { return transport->blocked; }));
    }
    auto reader = std::async(std::launch::async, [&] {
        return touch->positions() == original && touch->state().touchPointers == std::vector<std::uint32_t>{1} &&
               !touch->validate();
    });
    CHECK(reader.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready && reader.get());
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        transport->unblock = true;
    }
    transport->changed.notify_all();
    CHECK(!pending.get());
    CHECK(original == (std::vector<std::pair<std::int32_t, std::int32_t>>{{600, 450}}));
    CHECK(touch->positions() == (std::vector<std::pair<std::int32_t, std::int32_t>>{{850, 450}}));
    CHECK(!touch->releaseAll());

    auto periodicTransport = std::make_shared<Transport>();
    auto periodic = makeDesktopComputerWindowsTouchWithTransport(periodicTransport, std::chrono::milliseconds(5));
    std::atomic<unsigned> calls{0};
    auto token = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = token;
    CHECK(!periodic->dispatch({contact(3)}, port(), [token, &calls] {
        ++calls;
        return false;
    }));
    const auto atReturn = calls.load();
    token.reset();
    CHECK(weak.expired());
    CHECK(periodicTransport->waitForFrames(3));
    CHECK(calls == atReturn);
    unsigned releaseCalls = 0;
    MaiError released;
    CHECK(eventually([&] {
        released = periodic->dispatch({contact(3, Phase::Up)}, {}, [&] {
            ++releaseCalls;
            return true;
        });
        // A frame can be visible before the background admission guard drops.
        // Only that exact overlap is retryable; retain every real input error.
        return released.code() != MaiErrorCode::Busy ||
               released.message() != "touch source work or cleanup is in flight";
    }));
    CHECK(!released);
    CHECK(releaseCalls == 0 && periodic->state().empty());
}

void partialFailureAndRetry() {
    auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    transport->results = {{DesktopWindowsTouchSubmitKind::Accepted, {}},
                          {DesktopWindowsTouchSubmitKind::Accepted, {}},
                          {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "second birth rejected"}},
                          {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "cancel rejected"}}};
    transport->failDestroy = true;
    CHECK(touch->dispatch({contact(1), contact(2)}, port()).code() == MaiErrorCode::Internal);
    CHECK(touch->state().touchPointers == std::vector<std::uint32_t>{1});
    CHECK(!touch->state().devices.empty() && touch->validate().code() == MaiErrorCode::Internal);
    CHECK(touch->positions().size() == 1);
    transport->failDestroy = false;
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty() && touch->positions().empty() && !touch->validate());
    CHECK(!touch->dispatch({contact(3)}, port()));
    transport->results = {{DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "up rejected"}}};
    CHECK(touch->dispatch({contact(3, Phase::Up)}, {}).code() == MaiErrorCode::Internal);
    CHECK(touch->state().touchPointers == std::vector<std::uint32_t>{3});
    CHECK(!touch->dispatch({contact(3, Phase::Up)}, {}));
    CHECK(touch->state().empty());
    CHECK(touch->validate().code() == MaiErrorCode::Internal);  // Empty resources are not a cleared fault.
    CHECK(!touch->releaseAll() && !touch->validate());
}

void uncertainPositionsRetireWithTheirContact() {
    auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    CHECK(!touch->dispatch({contact(1), contact(2, Phase::Down, .75F)}, port()));
    transport->throwNext = true;
    transport->failDestroy = true;
    transport->results = {{DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "cancel rejected"}}};
    CHECK(touch->dispatch({contact(1, Phase::Move, .9F)}, port()).code() == MaiErrorCode::Internal);
    CHECK(touch->positions().size() >= 3);
    CHECK(touch->state().touchPointers.size() == 2);
    CHECK(!touch->dispatch({contact(1, Phase::Up)}, {}));
    CHECK(touch->positions() == (std::vector<std::pair<std::int32_t, std::int32_t>>{{850, 450}}));
    transport->failDestroy = false;
    CHECK(!touch->releaseAll());
    CHECK(touch->positions().empty() && touch->state().empty());
}

void hoverBirthsAndAsyncFault() {
    auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    CHECK(!touch->dispatch({contact(1, Phase::Hover), contact(2, Phase::Hover, .75F)}, port()));
    const auto initial = transport->snapshot().back().points;
    const auto before = transport->snapshot().size();
    CHECK(!touch->dispatch({contact(1), contact(2, Phase::Down, .75F)}, port()));
    const auto frames = transport->snapshot();
    CHECK(frames.size() == before + 3);
    CHECK(frames[before].points.size() == 2 && frames[before + 1].points.size() == 2);
    CHECK((frames[before].points[0].flags & kDesktopTouchDown) &&
          !(frames[before].points[1].flags & kDesktopTouchInContact));
    CHECK((frames[before + 1].points[0].flags & kDesktopTouchUpdate) &&
          (frames[before + 1].points[0].flags & kDesktopTouchInContact) &&
          !(frames[before + 1].points[1].flags & kDesktopTouchInContact));
    CHECK(!(frames.back().points[0].flags & kDesktopTouchDown) && (frames.back().points[1].flags & kDesktopTouchDown));
    CHECK(frames.back().points[0].pointerId == initial[0].pointerId &&
          frames.back().points[1].pointerId == initial[1].pointerId);
    CHECK(!touch->releaseAll());

    auto failed = std::make_shared<Transport>();
    auto periodic = makeDesktopComputerWindowsTouchWithTransport(failed, std::chrono::milliseconds(5));
    failed->results = {{DesktopWindowsTouchSubmitKind::Accepted, {}},
                       {DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "periodic report rejected"}}};
    failed->blockAtFrame = 3;  // Cleanup is in flight; the fault must already be published.
    failed->failDestroy = true;
    CHECK(!periodic->dispatch({contact(7)}, port()));
    {
        std::unique_lock<std::mutex> lock(failed->mutex);
        CHECK(failed->changed.wait_for(lock, std::chrono::seconds(1), [&] { return failed->blocked; }));
    }
    auto read = std::async(std::launch::async, [&] {
        return periodic->validate().code() == MaiErrorCode::Internal &&
               periodic->state().touchPointers == std::vector<std::uint32_t>{7} && periodic->positions().size() == 1;
    });
    CHECK(read.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready && read.get());
    {
        std::lock_guard<std::mutex> lock(failed->mutex);
        failed->unblock = true;
    }
    failed->changed.notify_all();
    CHECK(eventually([&] { return periodic->state().touchPointers.empty(); }));
    CHECK(!periodic->state().devices.empty() && periodic->validate().code() == MaiErrorCode::Internal);
    failed->failDestroy = false;
    CHECK(!periodic->releaseAll());
    CHECK(periodic->state().empty() && periodic->positions().empty() && !periodic->validate());
}

void periodicReleaseReentry() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(5));
    std::atomic<bool> entered{false}, returned{false};
    std::atomic<int> releaseCode{0}, nestedInputCode{0}, cleanupReleaseCode{0}, cleanupInputCode{0};
    std::atomic<unsigned> cleanupReentries{0};
    transport->duringInject = [&] {
        const auto flags = transport->frames.back().points.front().flags;
        if ((flags & kDesktopTouchCanceled) && entered.load()) {
            cleanupReleaseCode = int(touch->releaseAll().code());
            cleanupInputCode = int(touch->dispatch({contact(98)}, port()).code());
            ++cleanupReentries;
            return;
        }
        if (transport->frames.size() < 2 || entered.exchange(true)) return;
        std::puts("PERIODIC_REENTRY_ENTER");
        std::fflush(stdout);
        releaseCode = int(touch->releaseAll().code());
        nestedInputCode = int(touch->dispatch({contact(99)}, port()).code());
        returned = true;
        std::puts("PERIODIC_REENTRY_RETURN");
        std::fflush(stdout);
    };
    CHECK(!touch->dispatch({contact(1)}, port()));
    CHECK(eventually([&] { return returned.load(); }));
    CHECK(entered && releaseCode == int(MaiErrorCode::Busy) && nestedInputCode == int(MaiErrorCode::Busy));
    CHECK(eventually([&] { return touch->state().empty(); }));
    CHECK(cleanupReentries == 1 && cleanupReleaseCode == int(MaiErrorCode::Busy) &&
          cleanupInputCode == int(MaiErrorCode::Busy));
    unsigned births = 0;
    for (const auto& frame : transport->snapshot())
        for (const auto& point : frame.points) births += (point.flags & kDesktopTouchDown) != 0;
    CHECK(births == 1);
    transport->duringInject = {};
    MaiError finalCleanup;
    CHECK(eventually([&] {
        finalCleanup = touch->releaseAll();
        return finalCleanup.code() != MaiErrorCode::Busy;
    }));
    CHECK(!finalCleanup);
}

void residentSeedCycle() {
    const auto transport = std::make_shared<Transport>();
    transport->blockAtFrame = 2;
    const auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(10));
    auto pending = std::async(std::launch::async,
                              [&] { return touch->dispatch({contact(41), contact(42, Phase::Down, .75F)}, port()); });
    {
        std::unique_lock<std::mutex> lock(transport->mutex);
        CHECK(transport->changed.wait_for(lock, std::chrono::seconds(2), [&] { return transport->blocked; }));
    }
    const auto before = transport->snapshot();
    CHECK(before.size() == 2);
    CHECK(!before.empty() && before.front().points.size() == 1);
    CHECK(before.size() > 1 && before[1].points.size() == 1);
    CHECK(touch->state().touchPointers == std::vector<std::uint32_t>{41});
    CHECK(pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        transport->unblock = true;
    }
    transport->changed.notify_all();
    CHECK(!pending.get());
    CHECK(touch->state().touchPointers == (std::vector<std::uint32_t>{41, 42}));
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty());
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        CHECK(transport->nativeThreads.size() == 1);
    }
}

void sourceAndDynamicFrames() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    CHECK(touch);
    if (!touch) return;
    CHECK(!touch->dispatch({contact(0xffffffff), contact(7, Phase::Down, .75F)}, port()));
    auto frames = transport->snapshot();
    CHECK(transport->creates == 1 && transport->requestedCapacity == 10);
    CHECK(frames.size() == 3 && frames.front().points.size() == 1 && frames.back().points.size() == 2);
    if (frames.size() < 3 || frames.back().points.size() != 2) return;
    CHECK(frames[0].points[0].flags == (kDesktopTouchDown | kDesktopTouchInRange | kDesktopTouchInContact));
    CHECK(frames[1].points[0].flags == (kDesktopTouchUpdate | kDesktopTouchInRange | kDesktopTouchInContact));
    CHECK(frames[2].points[0].pointerId != frames[2].points[1].pointerId);
    unsigned births = 0;
    for (const auto& initial : frames)
        for (const auto& point : initial.points) {
            CHECK(point.pointerId < 10);
            births += (point.flags & kDesktopTouchDown) != 0;
        }
    CHECK(births == 2);
    CHECK(touch->state().touchPointers.size() == 2 && !touch->state().devices.empty());
    CHECK(!touch->dispatch({contact(9, Phase::Down, .25F)}, port()));
    frames = transport->snapshot();
    CHECK(frames.back().points.size() == 3);
    int downs = 0, updates = 0;
    for (const auto& point : frames.back().points) {
        downs += (point.flags & kDesktopTouchDown) != 0;
        updates += (point.flags & kDesktopTouchUpdate) != 0;
    }
    CHECK(downs == 1 && updates == 2);
    const auto heldBefore = touch->state().touchPointers;
    transport->duringInject = [weak = std::weak_ptr<DesktopComputerWindowsTouch>(touch), heldBefore] {
        if (const auto current = weak.lock()) CHECK(current->state().touchPointers == heldBefore);
    };
    CHECK(!touch->dispatch({contact(7, Phase::Up)}, port()));
    transport->duringInject = {};
    frames = transport->snapshot();
    CHECK(frames.back().points.size() == 3); // UP remains in its accepted frame.
    CHECK(touch->state().touchPointers.size() == 2);
    CHECK(std::count_if(frames.back().points.begin(), frames.back().points.end(),
        [](const auto& point) { return (point.flags & kDesktopTouchUp) != 0; }) == 1);
    const auto count = frames.size();
    CHECK(!touch->dispatch({contact(888, Phase::Up)}, {}));
    CHECK(transport->snapshot().size() == count);
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty() && transport->destroys == 1);
    frames = transport->snapshot();
    for (const auto& point : frames.back().points)
        CHECK((point.flags & (kDesktopTouchCanceled | kDesktopTouchUp)) == (kDesktopTouchCanceled | kDesktopTouchUp));
}

void boundsAndAnalog() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    auto bounds = port();
    bounds.x = -1920;
    bounds.y = -900;
    bounds.width = 1920;
    bounds.height = 1080;
    bounds.environmentX = -1920;
    bounds.environmentY = -1080;
    bounds.environmentWidth = 5760;
    bounds.environmentHeight = 3240;
    auto first = contact(1, Phase::Down, 0);
    first.y = 0;
    first.pressureOrDistance = 1;
    first.rotation = 360;
    first.contactAreaMajor = .1F;
    first.contactAreaMinor = .05F;
    auto second = contact(2, Phase::Down, 1);
    second.y = 1;
    second.pressureOrDistance = 0;
    CHECK(!touch->dispatch({first, second}, bounds));
    const auto frame = transport->snapshot().back();
    CHECK(frame.points.size() == 2);
    bool origin = false, endpoint = false;
    for (const auto& point : frame.points) {
        if (point.screenX == -1920) {
            origin = true;
            CHECK(point.x == 0 && point.y == 180 && point.screenY == -900);
            CHECK(point.pressure == 1024 && point.hasPressure && point.hasOrientation && point.hasArea);
            CHECK(point.orientation < 360 && point.right > point.left && point.bottom > point.top);
        } else {
            endpoint = true;
            CHECK(point.screenX == -1 && point.screenY == 179 && point.x == 1919 && point.y == 1259);
            CHECK(!point.hasPressure && point.pressure == 512 && !point.hasOrientation);
        }
    }
    CHECK(origin && endpoint);
    auto invalidPort = bounds;
    invalidPort.width = std::numeric_limits<double>::infinity();
    const auto before = transport->snapshot().size();
    CHECK(touch->dispatch({contact(3)}, invalidPort).code() == MaiErrorCode::InvalidInput);
    CHECK(transport->snapshot().size() == before);
    CHECK(!touch->dispatch({contact(1, Phase::Cancel)}, invalidPort)); // Cached original binding.
    CHECK(!touch->releaseAll());
}

void hoverAndValidation() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    CHECK(!touch->dispatch({contact(100, Phase::Hover)}, port()));
    auto frame = transport->snapshot().back();
    CHECK(frame.points[0].flags == (kDesktopTouchInRange | kDesktopTouchUpdate));
    const auto slot = frame.points[0].pointerId;
    CHECK(!touch->dispatch({contact(100, Phase::Down)}, port()));
    frame = transport->snapshot().back();
    CHECK(frame.points[0].pointerId == slot && (frame.points[0].flags & kDesktopTouchDown));
    CHECK(!touch->dispatch({contact(100, Phase::Hover)}, port()));
    frame = transport->snapshot().back();
    CHECK(frame.points[0].flags == (kDesktopTouchInRange | kDesktopTouchUp));
    CHECK(!touch->dispatch({contact(100, Phase::HoverLeave)}, {}));
    CHECK(touch->state().empty());
    CHECK(transport->snapshot().back().points[0].flags == kDesktopTouchUpdate);
    CHECK(touch->dispatch({}, port()).code() == MaiErrorCode::InvalidInput);
    CHECK(touch->dispatch({contact(1, Phase::ButtonOnly)}, port()).code() == MaiErrorCode::InvalidInput);
    CHECK(touch->dispatch({contact(1), contact(1)}, port()).code() == MaiErrorCode::InvalidInput);
    CHECK(transport->creates == 1);
    CHECK(!touch->dispatch({contact(100, Phase::Up)}, {}));
    CHECK(transport->creates == 1);
}

void capacityAndFailure() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    std::vector<DesktopInputTouch> contacts;
    for (std::uint32_t id = 0; id < 10; ++id) contacts.push_back(contact(id));
    CHECK(!touch->dispatch(contacts, port()));
    auto frame = transport->snapshot().back();
    std::set<std::uint32_t> slots;
    for (const auto& point : frame.points) slots.insert(point.pointerId);
    CHECK(slots.size() == 10 && *slots.begin() == 0 && *slots.rbegin() == 9);
    const auto count = transport->snapshot().size();
    CHECK(touch->dispatch({contact(10)}, port()).code() == MaiErrorCode::Busy);
    CHECK(transport->snapshot().size() == count);
    CHECK(!touch->dispatch({contact(0, Phase::Up), contact(10)}, port()));
    const auto replaced = transport->snapshot();
    CHECK(replaced.size() == count + 2);
    CHECK(replaced[count].points[0].pointerId == 0 && (replaced[count].points[0].flags & kDesktopTouchUp));
    CHECK(replaced[count + 1].points[0].pointerId == 0 && (replaced[count + 1].points[0].flags & kDesktopTouchDown));
    const auto heldAfterReplace = touch->state().touchPointers;
    CHECK(heldAfterReplace.size() == 10 &&
          std::find(heldAfterReplace.begin(), heldAfterReplace.end(), std::uint32_t(10)) != heldAfterReplace.end());
    CHECK(transport->snapshot().back().points.size() == 10);
    transport->results.push_back({DesktopWindowsTouchSubmitKind::Retry, {MaiErrorCode::Busy, "not ready"}});
    transport->results.push_back({DesktopWindowsTouchSubmitKind::Accepted, {}});
    const auto retried = transport->snapshot().size();
    CHECK(!touch->dispatch({contact(10, Phase::Move, .8F)}, port()));
    const auto frames = transport->snapshot();
    CHECK(frames.size() == retried + 2);
    CHECK(frames[retried].points[0].flags == frames[retried + 1].points[0].flags);
    transport->results.push_back({DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "fixture inject failed"}});
    CHECK(touch->dispatch({contact(0xffffffff)}, port()).code() == MaiErrorCode::Busy); // Capacity preflight first.
    CHECK(touch->dispatch({contact(10, Phase::Up)}, {}).code() == MaiErrorCode::Internal);
    CHECK(!touch->state().empty());
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty());

    transport->failCreate = true;
    CHECK(touch->dispatch({contact(1)}, port()).code() == MaiErrorCode::NotConfigured);
    CHECK(touch->state().empty());
    CHECK(touch->validate().code() == MaiErrorCode::NotConfigured);
    CHECK(!touch->releaseAll());
    transport->failCreate = false;
    CHECK(!touch->dispatch({contact(1)}, port()));
    transport->failDestroy = true;
    CHECK(touch->releaseAll().code() == MaiErrorCode::Internal);
    CHECK(!touch->state().empty());
    transport->failDestroy = false;
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty());
}

void stationaryAndLifetime() {
    const auto transport = std::make_shared<Transport>();
    auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(50));
    if (!touch) { CHECK(false); return; }
    auto input = contact(22);
    auto bounds = port();
    CHECK(!touch->dispatch({input}, bounds));
    input.x = 1;
    bounds.x = 900; // Refresh must retain its own accepted native binding.
    CHECK(transport->waitForFrames(2));
    auto frames = transport->snapshot();
    CHECK(frames[1].points[0].flags == (kDesktopTouchUpdate | kDesktopTouchInRange | kDesktopTouchInContact));
    CHECK(frames[1].points[0].screenX == 600 && frames[1].points[0].screenY == 450);
    // The frame notification may precede background admission retirement.
    // Retry only Busy; a real cleanup error must remain the test result.
    MaiError cleanup;
    CHECK(eventually([&] {
        cleanup = touch->releaseAll();
        return cleanup.code() != MaiErrorCode::Busy;
    }));
    CHECK(!cleanup);
    const auto ended = transport->snapshot().size();
    touch.reset();
    CHECK(transport->snapshot().size() == ended && transport->injectAfterDestroy == 0);
}

void retryExhaustionAndDestroy() {
    const auto transport = std::make_shared<Transport>();
    auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    for (int count = 0; count < 8; ++count)
        transport->results.push_back({DesktopWindowsTouchSubmitKind::Retry, {MaiErrorCode::Busy, "not ready"}});
    CHECK(touch->dispatch({contact(1)}, port()).code() == MaiErrorCode::Busy);
    CHECK(transport->snapshot().size() == 8 && touch->state().empty());
    CHECK(touch->validate().code() == MaiErrorCode::Busy);
    CHECK(touch->dispatch({contact(1)}, port()).code() == MaiErrorCode::Busy);
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty());
    CHECK(!touch->dispatch({contact(2)}, port()));
    transport->results.push_back({DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "cancel rejected"}});
    CHECK(touch->releaseAll().code() == MaiErrorCode::Internal);
    CHECK(touch->state().empty() && !transport->live); // Destroy still ends this source.
    CHECK(touch->validate().code() == MaiErrorCode::Internal);
    CHECK(!touch->releaseAll());  // Explicitly acknowledge the retained cleanup fault.
    CHECK(!touch->dispatch({contact(3)}, port()));
    touch.reset(); // Destructor must cancel, join and destroy without caller data.
    CHECK(!transport->live && transport->injectAfterDestroy == 0);
    CHECK((transport->snapshot().back().points[0].flags & kDesktopTouchCanceled) != 0);
}

void anglesAndPixelBounds() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    auto bounds = port();
    bounds.x = 10;
    bounds.y = 20;
    bounds.width = 101;
    bounds.height = 201;
    auto sample = contact(1);
    sample.rotation = 90;
    sample.contactAreaMajor = .2F;
    sample.contactAreaMinor = .1F;
    CHECK(!touch->dispatch({sample}, bounds));
    const auto projected = transport->snapshot().back().points.front();
    CHECK(projected.screenX == 60 && projected.screenY == 120);
    CHECK(projected.hasOrientation && projected.orientation == 180 && projected.hasArea);
    CHECK(projected.right - projected.left >= 20 && projected.bottom - projected.top >= 20);
    sample.phase = Phase::CancelAll;
    CHECK(!touch->dispatch({sample}, {}));
    CHECK(touch->state().empty());
    auto onePixel = bounds;
    onePixel.width = onePixel.height = 1;
    sample = contact(0xffffffff, Phase::Down, 1);
    sample.y = 1;
    CHECK(!touch->dispatch({sample}, onePixel));
    const auto one = transport->snapshot().back().points.front();
    CHECK(one.screenX == 10 && one.screenY == 20);
    CHECK(!touch->releaseAll());
    auto outside = bounds;
    outside.x = 5000;
    CHECK(touch->dispatch({contact(1)}, outside).code() == MaiErrorCode::InvalidInput);
    CHECK(touch->state().empty());
    sample = contact(1);
    sample.rotation = 361;
    CHECK(touch->dispatch({sample}, bounds).code() == MaiErrorCode::InvalidInput);
    sample = contact(1);
    sample.pressureOrDistance = std::numeric_limits<float>::quiet_NaN();
    CHECK(touch->dispatch({sample}, bounds).code() == MaiErrorCode::InvalidInput);
}

void physicalAnglesAndEllipseBounds() {
    // The client plane has +X right and +Y down. The same directed pose as
    // pen tilt starts at +Y and rotates toward -X; Win32 starts at +X and
    // increases clockwise. A full direction must survive even though an
    // ellipse's contact box repeats every half turn.
    struct Expected {
        std::uint16_t rotation;
        std::uint32_t orientation;
        std::int32_t halfWidth, halfHeight;
    };
    const Expected cases[] = {{0, 90, 64, 128},  {90, 180, 128, 64},  {180, 270, 64, 128},
                              {270, 0, 128, 64}, {45, 135, 102, 102}, {360, 90, 64, 128}};
    for (const auto& expected : cases) {
        auto transport = std::make_shared<Transport>();
        auto touch = backend(transport);
        auto bounds = port();
        bounds.x = bounds.y = -1024;
        bounds.width = bounds.height = 1024;
        bounds.environmentX = bounds.environmentY = -2048;
        bounds.environmentWidth = bounds.environmentHeight = 4096;
        auto sample = contact(1);
        sample.rotation = expected.rotation;
        sample.contactAreaMajor = .25F;
        sample.contactAreaMinor = .125F;
        CHECK(!touch->dispatch({sample}, bounds));
        const auto frames = transport->snapshot();
        CHECK(!frames.empty() && frames.back().points.size() == 1);
        if (frames.empty() || frames.back().points.empty()) continue;
        const auto& point = frames.back().points.front();
        // Projection rounds the absolute screen coordinate; -512.5 rounds
        // away from zero to -513 before virtual-origin rebasing.
        CHECK(point.screenX == -513 && point.screenY == -513);
        CHECK(point.x == 1535 && point.y == 1535);
        CHECK(point.hasOrientation && point.orientation == expected.orientation);
        CHECK(point.hasArea);
        CHECK(point.left == 1535 - expected.halfWidth && point.right == 1535 + expected.halfWidth);
        CHECK(point.top == 1535 - expected.halfHeight && point.bottom == 1535 + expected.halfHeight);
        CHECK(!touch->releaseAll());
    }
}

void nonuniformViewportAndOptionalEllipseAxes() {
    // Area diameters live in the normalized client plane. Apply viewport
    // width to every horizontal component and height to every vertical one.
    // At 45 degrees this is an ellipse bound, not a rotated rectangle bound.
    struct Expected {
        std::uint16_t rotation;
        float major, minor;
        std::int32_t halfWidth, halfHeight;
        bool area, orientation;
    };
    const Expected cases[] = {{0, .25F, .125F, 64, 256, true, true},   {90, .25F, .125F, 128, 128, true, true},
                              {180, .25F, .125F, 64, 256, true, true}, {270, .25F, .125F, 128, 128, true, true},
                              {45, .25F, .125F, 102, 203, true, true}, {65535, .25F, .125F, 102, 203, true, false},
                              {0, .25F, 0, 128, 256, true, true},      {45, .25F, 0, 128, 256, true, true},
                              {0, 0, .125F, 64, 128, true, true},      {0, 0, 0, 0, 0, false, true},
                              {65535, 0, 0, 0, 0, false, false}};
    for (const auto& expected : cases) {
        auto transport = std::make_shared<Transport>();
        auto touch = backend(transport);
        auto bounds = port();
        bounds.x = -2048;
        bounds.y = -1024;
        bounds.width = 1024;
        bounds.height = 2048;
        bounds.environmentX = -4096;
        bounds.environmentY = -2048;
        bounds.environmentWidth = 8192;
        bounds.environmentHeight = 4096;
        auto sample = contact(1);
        sample.rotation = expected.rotation;
        sample.contactAreaMajor = expected.major;
        sample.contactAreaMinor = expected.minor;
        CHECK(!touch->dispatch({sample}, bounds));
        const auto frames = transport->snapshot();
        CHECK(!frames.empty() && frames.back().points.size() == 1);
        if (frames.empty() || frames.back().points.empty()) continue;
        const auto& point = frames.back().points.front();
        // Both absolute centers are negative half pixels, so llround keeps
        // the same away-from-zero convention as the ordinary pen projection.
        CHECK(point.screenX == -1537 && point.screenY == -1);
        CHECK(point.x == 2559 && point.y == 2047);
        CHECK(point.hasArea == expected.area && point.hasOrientation == expected.orientation);
        // Direction is independent metadata. Stretching or omitting the
        // contact ellipse must not rotate the caller's requested direction.
        if (expected.orientation) CHECK(point.orientation == (expected.rotation + 90U) % 360U);
        CHECK(point.left == 2559 - expected.halfWidth && point.right == 2559 + expected.halfWidth);
        CHECK(point.top == 2047 - expected.halfHeight && point.bottom == 2047 + expected.halfHeight);
        if (!expected.orientation) CHECK(point.orientation == 0);
        CHECK(!touch->releaseAll());
    }
}

void layoutChangeRetiresPhysicalBinding() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    CHECK(!touch->dispatch({contact(1)}, port()));
    auto movedOrigin = port();
    movedOrigin.environmentX = -1920;
    movedOrigin.environmentWidth = 5760;
    const auto beforeLayout = transport->snapshot().size();
    CHECK(touch->dispatch({contact(2, Phase::Down, .75F)}, movedOrigin).code() == MaiErrorCode::Busy);
    CHECK(transport->snapshot().size() == beforeLayout);  // No coordinate reinterpretation/cancel warp.
    CHECK(touch->state().empty() && touch->positions().empty());
    CHECK(touch->validate().code() == MaiErrorCode::Busy);
    CHECK(!touch->releaseAll());
    auto nativeDpi = port();
    nativeDpi.width = 1501;
    nativeDpi.height = 751;
    CHECK(!touch->dispatch({contact(1)}, nativeDpi));
    const auto point = transport->snapshot().back().points.front();
    CHECK(point.screenX == 850 && point.screenY == 575); // Native pixels, no second DPI scaling.
}

void opaqueIdsKeepNativeOrder() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(20));
    if (!touch) { CHECK(false); return; }
    CHECK(!touch->dispatch({contact(0xffffffff), contact(7, Phase::Down, .75F)}, port()));
    CHECK(transport->waitForFrames(2));
    auto points = transport->snapshot().back().points;
    CHECK(points.size() == 2 && points[0].pointerId == 0 && points[1].pointerId == 1);
    CHECK(points[0].screenX == 600 && points[1].screenX == 850);
    CHECK(!touch->releaseAll());
    points = transport->snapshot().back().points;
    CHECK(points.size() == 2 && points[0].pointerId == 0 && points[1].pointerId == 1);
}

void stopDoesNotWaitForPeriodicIo() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = makeDesktopComputerWindowsTouchWithTransport(transport, std::chrono::milliseconds(5));
    if (!touch) { CHECK(false); return; }
    CHECK(!touch->dispatch({contact(1)}, port()));
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        transport->blockNext = true;
    }
    {
        std::unique_lock<std::mutex> lock(transport->mutex);
        CHECK(transport->changed.wait_for(lock, std::chrono::seconds(1), [&] { return transport->blocked; }));
    }
    const auto stopStarted = std::chrono::steady_clock::now();
    CHECK(touch->releaseAll().code() == MaiErrorCode::Busy);
    CHECK(std::chrono::steady_clock::now() - stopStarted < std::chrono::milliseconds(100));
    CHECK(touch->dispatch({contact(2)}, port()).code() == MaiErrorCode::Busy);
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        CHECK(transport->destroys == 0);
        transport->unblock = true;
    }
    transport->changed.notify_all();
    CHECK(eventually([&] { return touch->state().empty(); }));
    MaiError finalCleanup;
    CHECK(eventually([&] {
        finalCleanup = touch->releaseAll();
        return finalCleanup.code() != MaiErrorCode::Busy;
    }));
    CHECK(!finalCleanup);
    unsigned births = 0;
    for (const auto& frame : transport->snapshot())
        for (const auto& point : frame.points) births += (point.flags & kDesktopTouchDown) != 0;
    CHECK(births == 1);
    CHECK(transport->destroys == 1 && transport->injectAfterDestroy == 0);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--parameter-semantics") {
        physicalAnglesAndEllipseBounds();
        nonuniformViewportAndOptionalEllipseAxes();
        std::printf("touch parameter semantics: %d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--periodic-release-reentry") {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        periodicReleaseReentry();
        std::printf("periodic-only: %d checks, %d failures\\n", checks, failures);
        return failures ? 1 : 0;
    }
    periodicReleaseReentry();
    residentSeedCycle();
    hoverBirthsAndAsyncFault();
    tenBirthsAndWholePreflight();
    stopWaitingAndReentrantCallback();
    livePositionsAndCallerLifetime();
    partialFailureAndRetry();
    uncertainPositionsRetireWithTheirContact();
    // Keep the real factory in the PE so imports verification also covers the
    // production transport, while every test below still uses only a fake.
    using NativeFactory = std::shared_ptr<DesktopComputerWindowsTouch>(*)();
    volatile NativeFactory nativeFactory = &makeDesktopComputerWindowsTouch;
    CHECK(nativeFactory != nullptr);
    sourceAndDynamicFrames();
    boundsAndAnalog();
    hoverAndValidation();
    capacityAndFailure();
    stationaryAndLifetime();
    retryExhaustionAndDestroy();
    anglesAndPixelBounds();
    physicalAnglesAndEllipseBounds();
    nonuniformViewportAndOptionalEllipseAxes();
    stopDoesNotWaitForPeriodicIo();
    layoutChangeRetiresPhysicalBinding();
    opaqueIdsKeepNativeOrder();
    std::printf("DesktopComputerWindowsTouchTest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
