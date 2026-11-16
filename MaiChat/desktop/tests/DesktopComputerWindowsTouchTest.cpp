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

namespace {
int checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); ++failures; } } while (false)
using Phase = DesktopInputPhase;

class Transport final : public DesktopWindowsTouchTransport {
public:
    MaiError create(std::uint32_t capacity) override {
        std::lock_guard<std::mutex> lock(mutex);
        ++creates;
        requestedCapacity = capacity;
        if (failCreate) return {MaiErrorCode::NotConfigured, "fixture create failed"};
        live = true;
        return {};
    }
    DesktopWindowsTouchSubmission inject(const DesktopWindowsTouchFrame& frame) override {
        std::unique_lock<std::mutex> lock(mutex);
        if (!live) ++injectAfterDestroy;
        frames.push_back(frame);
        changed.notify_all();
        if (blockNext) {
            blockNext = false;
            blocked = true;
            changed.notify_all();
            changed.wait(lock, [&] { return unblock; });
        }
        if (duringInject) duringInject();
        if (results.empty()) return {};
        const auto result = results.front();
        results.pop_front();
        return result;
    }
    MaiError destroy() override {
        std::lock_guard<std::mutex> lock(mutex);
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

void atomicAndDynamicFrames() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    CHECK(touch);
    if (!touch) return;
    CHECK(!touch->dispatch({contact(0xffffffff), contact(7, Phase::Down, .75F)}, port()));
    auto frames = transport->snapshot();
    CHECK(transport->creates == 1 && transport->requestedCapacity == 10);
    CHECK(frames.size() == 1 && frames[0].points.size() == 2);
    if (frames.empty() || frames[0].points.size() != 2) return;
    CHECK(frames[0].points[0].pointerId != frames[0].points[1].pointerId);
    for (const auto& point : frames[0].points) {
        CHECK(point.pointerId < 10);
        CHECK(point.flags == (kDesktopTouchDown | kDesktopTouchInRange | kDesktopTouchInContact));
    }
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
    CHECK(touch->dispatch({contact(0, Phase::Up), contact(10)}, port()).code() == MaiErrorCode::Busy);
    CHECK(transport->snapshot().size() == count);
    CHECK(!touch->dispatch({contact(0, Phase::Up)}, {}));
    CHECK(!touch->dispatch({contact(10)}, port()));
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
    CHECK(!touch->releaseAll());
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
    CHECK(transport->snapshot().size() == 8 && !touch->state().empty());
    CHECK(touch->dispatch({contact(1)}, port()).code() == MaiErrorCode::Busy);
    CHECK(!touch->releaseAll());
    CHECK(touch->state().empty());
    CHECK(!touch->dispatch({contact(2)}, port()));
    transport->results.push_back({DesktopWindowsTouchSubmitKind::Failed, {MaiErrorCode::Internal, "cancel rejected"}});
    CHECK(touch->releaseAll().code() == MaiErrorCode::Internal);
    CHECK(touch->state().empty() && !transport->live); // Destroy still ends this source.
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
    CHECK(projected.hasOrientation && projected.orientation == 0 && projected.hasArea);
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

void rebaseKeepsPhysicalBinding() {
    const auto transport = std::make_shared<Transport>();
    const auto touch = backend(transport);
    if (!touch) { CHECK(false); return; }
    CHECK(!touch->dispatch({contact(1)}, port()));
    auto movedOrigin = port();
    movedOrigin.environmentX = -1920;
    movedOrigin.environmentWidth = 5760;
    CHECK(!touch->dispatch({contact(2, Phase::Down, .75F)}, movedOrigin));
    const auto points = transport->snapshot().back().points;
    CHECK(points.size() == 2 && points[0].screenX == 600 && points[0].x == 2520);
    CHECK(points[0].flags == (kDesktopTouchUpdate | kDesktopTouchInRange | kDesktopTouchInContact));
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
    auto points = transport->snapshot()[1].points;
    CHECK(points.size() == 2 && points[0].pointerId == 0 && points[1].pointerId == 1);
    CHECK(points[0].screenX == 600 && points[1].screenX == 850);
    CHECK(!touch->releaseAll());
    points = transport->snapshot().back().points;
    CHECK(points.size() == 2 && points[0].pointerId == 0 && points[1].pointerId == 1);
}

void closeWaitsForRefresh() {
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
    std::atomic<bool> cleaned{false};
    std::thread cleanup([&] { CHECK(!touch->releaseAll()); cleaned = true; });
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        CHECK(transport->destroys == 0 && !cleaned.load());
        transport->unblock = true;
        transport->changed.notify_all();
    }
    cleanup.join();
    CHECK(cleaned.load() && touch->state().empty());
    CHECK(transport->destroys == 1 && transport->injectAfterDestroy == 0);
}
} // namespace

int main() {
    // Keep the real factory in the PE so imports verification also covers the
    // production transport, while every test below still uses only a fake.
    using NativeFactory = std::shared_ptr<DesktopComputerWindowsTouch>(*)();
    volatile NativeFactory nativeFactory = &makeDesktopComputerWindowsTouch;
    CHECK(nativeFactory != nullptr);
    atomicAndDynamicFrames();
    boundsAndAnalog();
    hoverAndValidation();
    capacityAndFailure();
    stationaryAndLifetime();
    retryExhaustionAndDestroy();
    anglesAndPixelBounds();
    closeWaitsForRefresh();
    rebaseKeepsPhysicalBinding();
    opaqueIdsKeepNativeOrder();
    std::printf("DesktopComputerWindowsTouchTest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
