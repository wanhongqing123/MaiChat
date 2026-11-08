// 文件作用：仅供 DesktopCameraSession 离线测试链接的 MaiCameraCapture 实现。
#include "DesktopCameraNativeFake.h"

#include <mutex>
#include <thread>
#include <utility>

namespace DesktopCameraNativeFake {
struct Control {
    MaiCameraCapture::Callbacks callbacks;
    MaiCameraCapture::State state = MaiCameraCapture::State::Stopped;
    bool active = false;
    std::uint64_t pending = 0;
};

namespace {
std::mutex mutex;
std::weak_ptr<Control> last;
bool available = true;
bool acceptStart = true;
int starts = 0;
int stops = 0;
int captures = 0;

template <typename Callback>
void deliver(Callback callback) {
    std::thread worker(std::move(callback));
    worker.join();
}
}  // namespace

void reset() {
    std::lock_guard<std::mutex> lock(mutex);
    last.reset();
    available = true;
    acceptStart = true;
    starts = stops = captures = 0;
}

void setAvailable(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    available = value;
}

void setStartAccepted(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    acceptStart = value;
}

Handle latest() {
    std::lock_guard<std::mutex> lock(mutex);
    auto control = last.lock();
    return {control, control ? control->callbacks : MaiCameraCapture::Callbacks{}};
}

int startCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return starts;
}

int stopCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return stops;
}

int captureCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return captures;
}

std::uint64_t pendingRequestId() {
    std::lock_guard<std::mutex> lock(mutex);
    auto control = last.lock();
    return control ? control->pending : 0;
}

void deliverState(const Handle& handle, MaiCameraCapture::State state, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle.control && handle.control->active) handle.control->state = state;
    }
    deliver([&] {
        if (handle.callbacks.state) handle.callbacks.state(state, error);
    });
}

void deliverFrame(const Handle& handle, const MaiCameraCapture::Frame& frame) {
    deliver([&] {
        if (handle.callbacks.frame) handle.callbacks.frame(frame);
    });
}

void deliverFrames(const Handle& handle, const std::vector<MaiCameraCapture::Frame>& frames) {
    deliver([&] {
        for (const auto& frame : frames)
            if (handle.callbacks.frame) handle.callbacks.frame(frame);
    });
}

void deliverPhoto(const Handle& handle, const MaiCameraCapture::Photo& photo) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (handle.control && handle.control->active && handle.control->pending == photo.requestId)
            handle.control->pending = 0;
    }
    deliver([&] {
        if (handle.callbacks.photo) handle.callbacks.photo(photo);
    });
}
}  // namespace DesktopCameraNativeFake

struct MaiCameraCapture::Runtime {
    std::shared_ptr<DesktopCameraNativeFake::Control> control;
};

MaiCameraCapture::MaiCameraCapture(Callbacks callbacks) : mRuntime(std::make_shared<Runtime>()) {
    using namespace DesktopCameraNativeFake;
    mRuntime->control = std::make_shared<Control>();
    mRuntime->control->callbacks = std::move(callbacks);
    std::lock_guard<std::mutex> lock(mutex);
    last = mRuntime->control;
}

MaiCameraCapture::~MaiCameraCapture() {
    stop();
}

bool MaiCameraCapture::isAvailable() {
    std::lock_guard<std::mutex> lock(DesktopCameraNativeFake::mutex);
    return DesktopCameraNativeFake::available;
}

bool MaiCameraCapture::start() {
    using namespace DesktopCameraNativeFake;
    const auto control = mRuntime->control;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!available || !acceptStart || control->active) return false;
        ++starts;
        control->active = true;
        control->state = State::Starting;
    }
    deliverState({control, control->callbacks}, State::Starting);
    return true;
}

bool MaiCameraCapture::capturePhoto(std::uint64_t requestId) {
    using namespace DesktopCameraNativeFake;
    std::lock_guard<std::mutex> lock(mutex);
    const auto control = mRuntime->control;
    if (!control->active || control->state != State::Running || control->pending || !requestId) return false;
    ++captures;
    control->pending = requestId;
    return true;
}

void MaiCameraCapture::stop() {
    using namespace DesktopCameraNativeFake;
    const auto control = mRuntime->control;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!control->active) return;
        ++stops;
        control->active = false;
        control->state = State::Stopped;
        control->pending = 0;
    }
    deliverState({control, control->callbacks}, State::Stopped);
}
