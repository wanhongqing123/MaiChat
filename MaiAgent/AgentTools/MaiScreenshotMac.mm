// 文件作用：在常驻工作线程复用 ScreenCaptureKit，会话仅在目标变化/故障时重建。
// 帧回调只缓存有界完整像素，Command 才编码 PNG；请求取消函数从不进入异步生命周期。
#include "MaiScreenshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "MaiBlockingCheck.h"
#include "MaiThread.h"

namespace {

constexpr double kMaximumWidth = 2560.0;
constexpr double kMaximumHeight = 1600.0;
constexpr auto kCaptureTimeout = std::chrono::seconds(20);
constexpr auto kFreshFrameTimeout = std::chrono::seconds(2);

struct MaiCaptureState {
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    MaiError error;
    MaiScreenshot screenshot;
    std::vector<std::string> titles;
};

std::string errorText(NSError* error, const char* fallback) {
    if (!error) return fallback;
    const char* text = error.localizedDescription.UTF8String;
    return text && *text ? std::string(text) : std::string(fallback);
}

void finishCapture(const std::shared_ptr<MaiCaptureState>& state, MaiError error,
                   MaiScreenshot screenshot = {}) {
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (state->finished) return;
        state->error = std::move(error);
        state->screenshot = std::move(screenshot);
        state->finished = true;
    }
    state->condition.notify_one();
}

MaiError waitForCapture(const std::shared_ptr<MaiCaptureState>& state, MaiScreenshot& screenshot) {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, kCaptureTimeout, [&] { return state->finished; })) {
        return MaiError::make(MaiErrorCode::Internal, "screen capture timed out");
    }
    if (state->error.hasError()) return state->error;
    screenshot = std::move(state->screenshot);
    return {};
}

MaiError encodePng(CGImageRef image, MaiScreenshot& screenshot) {
    if (!image || !CGImageGetWidth(image) || !CGImageGetHeight(image)) {
        return MaiError::make(MaiErrorCode::Internal, "screen capture returned no image");
    }
    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
    if (!data) return MaiError::make(MaiErrorCode::Internal, "cannot allocate screenshot data");
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr);
    if (!destination) {
        CFRelease(data);
        return MaiError::make(MaiErrorCode::Internal, "cannot create PNG encoder");
    }
    CGImageDestinationAddImage(destination, image, nullptr);
    const bool saved = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    const CFIndex length = CFDataGetLength(data);
    if (!saved || length <= 0 || length > 24 * 1024 * 1024) {
        CFRelease(data);
        return MaiError::make(MaiErrorCode::Internal, "screen capture PNG is empty or too large");
    }
    screenshot.pngBytes.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + length);
    screenshot.width = static_cast<int>(CGImageGetWidth(image));
    screenshot.height = static_cast<int>(CGImageGetHeight(image));
    CFRelease(data);
    return {};
}

SCWindow* windowWithTitle(SCShareableContent* content, const std::string& utf8, MaiError& error) {
    NSString* requested = [[NSString alloc] initWithBytes:utf8.data()
                                                   length:utf8.size()
                                                 encoding:NSUTF8StringEncoding];
    if (!requested || requested.length == 0) {
        error = MaiError::make(MaiErrorCode::InvalidInput,
                               "windowTitle is empty or is not valid UTF-8");
        return nil;
    }
    NSMutableArray<SCWindow*>* exact = [NSMutableArray array];
    NSMutableArray<SCWindow*>* partial = [NSMutableArray array];
    for (SCWindow* window in content.windows) {
        if (!window.isOnScreen || window.windowLayer != 0 || window.title.length == 0) continue;
        if ([window.title caseInsensitiveCompare:requested] == NSOrderedSame) {
            [exact addObject:window];
        } else if ([window.title rangeOfString:requested options:NSCaseInsensitiveSearch]
                       .location != NSNotFound) {
            [partial addObject:window];
        }
    }
    NSArray<SCWindow*>* matches = exact.count ? exact : partial;
    if (matches.count == 0) {
        error = MaiError::make(MaiErrorCode::NotFound,
                               "no visible top-level window matches windowTitle");
        return nil;
    }
    if (matches.count != 1) {
        error = MaiError::make(MaiErrorCode::InvalidInput,
                               "windowTitle matches more than one visible window");
        return nil;
    }
    return matches.firstObject;
}

SCWindow* windowWithId(SCShareableContent* content, std::uint64_t windowId, MaiError& error) {
    // 原生标识独立于标题；仅使用当前可见的普通窗口，关闭或最小化时直接失败。
    for (SCWindow* window in content.windows) {
        if (window.windowID == windowId && window.isOnScreen && window.windowLayer == 0) {
            return window;
        }
    }
    error = MaiError::make(MaiErrorCode::NotFound,
                           "windowId does not identify a visible non-minimized top-level window");
    return nil;
}

SCStreamConfiguration* screenshotConfiguration(SCContentFilter* filter) {
    const double scale = std::max(1.0, double(filter.pointPixelScale));
    const double width = CGRectGetWidth(filter.contentRect) * scale;
    const double height = CGRectGetHeight(filter.contentRect) * scale;
    if (width <= 0 || height <= 0) return nil;
    const double factor = std::min({1.0, kMaximumWidth / width, kMaximumHeight / height});
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = static_cast<size_t>(std::max(1.0, std::round(width * factor)));
    configuration.height = static_cast<size_t>(std::max(1.0, std::round(height * factor)));
    configuration.showsCursor = YES;
    return configuration;
}

struct CaptureCounters {
    std::mutex mutex;
    MaiScreenshotCaptureState value;

    template <class F>
    void update(F change) {
        std::lock_guard<std::mutex> lock(mutex);
        change(value);
    }
};

struct CapturedPixels {
    std::vector<std::uint8_t> bytes;
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t stride = 0;
    std::uint64_t sequence = 0;
    std::int64_t received = 0;
    std::int64_t sourceProducedAtMs = 0;  // 仅内部 fresh 屏障使用；0 表示源时间不可证明。
};

// 两个 CMTime 必须已在 host clock 上；不能把任意 sample PTS 直接当作 mach/Unix 时间。
// Unix 锚点先于 hostNow 读取，年龄向上取整到毫秒，使结果不晚于可证明的源时间。
// 把时钟读取与算术分开，受控测试可覆盖延迟回调、分数毫秒、无效/未来/跨 epoch 和溢出。
bool sourceProducedUnixMilliseconds(CMTime presentationHost, CMTime hostNow,
                                    std::int64_t unixAnchorMs, std::int64_t& producedMs) {
    producedMs = 0;
    if (!CMTIME_IS_NUMERIC(presentationHost) || !CMTIME_IS_NUMERIC(hostNow) ||
        presentationHost.timescale <= 0 || hostNow.timescale <= 0 || unixAnchorMs <= 0 ||
        presentationHost.epoch != hostNow.epoch || CMTimeCompare(presentationHost, hostNow) > 0)
        return false;
    const CMTime age = CMTimeSubtract(hostNow, presentationHost);
    if (!CMTIME_IS_NUMERIC(age)) return false;
    const CMTime ageMs =
        CMTimeConvertScale(age, 1000, kCMTimeRoundingMethod_RoundTowardPositiveInfinity);
    if (!CMTIME_IS_NUMERIC(ageMs) || ageMs.value < 0 || ageMs.value >= unixAnchorMs) return false;
    producedMs = unixAnchorMs - ageMs.value;
    return true;
}

std::int64_t sampleSourceProducedAtMs(CMSampleBufferRef sample, CMClockRef synchronizationClock) {
    if (!synchronizationClock) return 0;
    const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
    if (!CMTIME_IS_NUMERIC(presentation)) return 0;
    const CMClockRef hostClock = CMClockGetHostTimeClock();
    if (!hostClock) return 0;
    // SCStream.synchronizationClock 是官方声明的输出 sample 时间基。显式转换到 host clock，
    // 不依赖 SCK 当前实现恰好使用 mach_absolute_time，也不比较不同时钟的裸 value。
    const CMTime presentationHost =
        CMSyncConvertTime(presentation, synchronizationClock, hostClock);
    const std::int64_t unixAnchorMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    const CMTime hostNow = CMClockGetTime(hostClock);
    std::int64_t producedMs = 0;
    sourceProducedUnixMilliseconds(presentationHost, hostNow, unixAnchorMs, producedMs);
    return producedMs;
}

struct StreamState {
    explicit StreamState(CaptureCounters& counters) : counters(counters) {}
    CaptureCounters& counters;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> stopping{false};
    std::uint64_t generation = 0;
    std::uint64_t availabilityEpoch = 0;
    bool failed = false;
    MaiError error;
    std::shared_ptr<const CapturedPixels> pixels;
};

struct FrameStamp {
    std::uint64_t generation = 0;
    std::uint64_t availabilityEpoch = 0;
};

struct DesktopAvailability {
    bool guiSession = false;
    bool onConsole = false;
    bool loginDone = false;
    bool frontmostKnown = false;
    bool loginWindow = false;
};

bool desktopCanCapture(const DesktopAvailability& value) {
    return value.guiSession && value.onConsole && value.loginDone && value.frontmostKnown &&
           !value.loginWindow;
}

DesktopAvailability readDesktopAvailability() {
    @autoreleasepool {
        DesktopAvailability value;
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        value.guiSession = session != nullptr;
        if (session) {
            value.onConsole =
                CFDictionaryGetValue(session, kCGSessionOnConsoleKey) == kCFBooleanTrue;
            value.loginDone =
                CFDictionaryGetValue(session, kCGSessionLoginDoneKey) == kCFBooleanTrue;
            CFRelease(session);
        }
        NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
        value.frontmostKnown = frontmost != nil;
        value.loginWindow = [frontmost.bundleIdentifier isEqualToString:@"com.apple.loginwindow"];
        return value;
    }
}

// 在 state.mutex 下调用，编码后和交付前共用同一条可用性检查。
bool frameStampIsCurrent(const StreamState& state, const FrameStamp& stamp) {
    return !state.stopping.load() && state.generation == stamp.generation &&
           state.availabilityEpoch == stamp.availabilityEpoch && state.pixels && !state.error &&
           !state.failed;
}

void invalidateFrameLocked(StreamState& state) {
    ++state.availabilityEpoch;
    state.pixels.reset();
}

void rejectDesktopLocked(StreamState& state, const MaiError& error) {
    invalidateFrameLocked(state);
    state.failed = true;
    state.error = error;
    state.counters.update([](auto& counts) { counts.capturing = false; });
}

// 帧回调不持有请求或取消函数。独立复制一份有界像素，避免长期占用 SCK 的 IOSurface 池。
void receiveFrame(const std::weak_ptr<StreamState>& weak, std::uint64_t generation,
                  CMSampleBufferRef sample, const DesktopAvailability* injected = nullptr,
                  CMClockRef synchronizationClock = nullptr) {
    const auto state = weak.lock();
    if (!state || state->stopping.load() || !CMSampleBufferIsValid(sample)) return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    if (!info || CFGetTypeID(info) != CFDictionaryGetTypeID()) return;
    const auto value = static_cast<CFNumberRef>(
        CFDictionaryGetValue(info, (__bridge const void*)SCStreamFrameInfoStatus));
    NSInteger status = -1;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(value, kCFNumberNSIntegerType, &status))
        return;
    const DesktopAvailability desktop = injected ? *injected : readDesktopAvailability();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (generation != state->generation || state->stopping.load()) return;
    // 这里只判断公开可观测的拒绝信号，不把这些属性宣称为完整的官方锁屏/撤权状态 API。
    if (!desktopCanCapture(desktop)) {
        rejectDesktopLocked(*state,
                            MaiError::make(MaiErrorCode::NotConfigured,
                                           "GUI session is unavailable for screen capture"));
        lock.unlock();
        state->condition.notify_all();
        return;
    }
    if (status == SCFrameStatusIdle) return;  // 没有新像素；仍可使用当前完整帧及原时间戳。
    if (state->failed && status == SCFrameStatusComplete) return;  // 迟到帧不能复活失败的 session。
    if (status != SCFrameStatusComplete) {
        invalidateFrameLocked(*state);
        if (status != SCFrameStatusStarted) {
            state->error =
                MaiError::make(MaiErrorCode::Busy, "screen stream has no current complete frame");
            // 同一 generation 的终止状态只能锁存；排队的软失效回调不能让迟到 Complete 复活它。
            state->failed = state->failed || status == SCFrameStatusStopped;
            state->counters.update([](auto& counts) { counts.capturing = false; });
        }
        lock.unlock();
        state->condition.notify_all();
        return;
    }
    const auto rejectPixels = [&](const char* message) {
        // Complete 却没有可读像素不是 Idle；不能继续把上一帧当作当前画面交付。
        invalidateFrameLocked(*state);
        state->failed = true;
        state->error = MaiError::make(MaiErrorCode::Protocol, message);
        state->counters.update([](auto& counts) { counts.capturing = false; });
        lock.unlock();
        state->condition.notify_all();
    };
    CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
    if (!CMSampleBufferDataIsReady(sample) || !buffer ||
        CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA) {
        rejectPixels("native screen frame has no readable BGRA pixels");
        return;
    }
    const std::size_t width = CVPixelBufferGetWidth(buffer);
    const std::size_t height = CVPixelBufferGetHeight(buffer);
    const std::size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    if (!width || !height || width > kMaximumWidth || height > kMaximumHeight ||
        stride < width * 4 || stride > 32 * 1024 || stride * height > 32 * 1024 * 1024) {
        rejectPixels("native screen frame exceeds capture bounds");
        return;
    }
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        rejectPixels("cannot lock native screen frame");
        return;
    }
    struct UnlockPixels {
        CVPixelBufferRef buffer;
        ~UnlockPixels() {
            CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        }
    } unlock{buffer};
    try {
        const auto* begin = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        if (!begin) {
            rejectPixels("native screen frame has no pixel storage");
            return;
        }
        auto frame = std::make_shared<CapturedPixels>();
        frame->bytes.assign(begin, begin + stride * height);
        frame->width = width;
        frame->height = height;
        frame->stride = stride;
        frame->received = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        frame->sourceProducedAtMs = sampleSourceProducedAtMs(sample, synchronizationClock);
        state->counters.update([&](auto& counts) {
            frame->sequence = ++counts.framesReceived;
            counts.capturing = true;
        });
        state->pixels = std::move(frame);
        state->error = {};
    } catch (const std::exception&) {
        invalidateFrameLocked(*state);
        state->failed = true;
        state->error = MaiError::make(MaiErrorCode::Internal, "cannot copy native screen frame");
        state->counters.update([](auto& counts) { counts.capturing = false; });
    }
    lock.unlock();
    state->condition.notify_all();
}

}  // namespace

@interface MaiScreenshotStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::weak_ptr<StreamState> state;
    std::uint64_t generation;
}
@end

@implementation MaiScreenshotStreamOutput
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
    @autoreleasepool {
        if (type == SCStreamOutputTypeScreen)
            receiveFrame(state, generation, sample, nullptr, stream.synchronizationClock);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    const auto shared = state.lock();
    if (!shared || shared->stopping.load()) return;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (generation != shared->generation) return;
        invalidateFrameLocked(*shared);
        shared->failed = true;
        shared->error =
            MaiError::make(MaiErrorCode::Internal, errorText(error, "screen stream stopped"));
        shared->counters.update([](auto& counts) { counts.capturing = false; });
    }
    shared->condition.notify_all();
}
@end

namespace {

struct NativeCompletion {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    MaiError error;
    SCShareableContent* content = nil;
};

void completeNative(const std::shared_ptr<NativeCompletion>& done, NSError* error) {
    {
        std::lock_guard<std::mutex> lock(done->mutex);
        if (error)
            done->error = MaiError::make(MaiErrorCode::Internal,
                                         errorText(error, "native capture operation failed"));
        done->done = true;
    }
    done->condition.notify_all();
}

struct CaptureCommand {
    // 仅复制目标字段；isCanceled 严禁进入此对象或任何异步 callback。
    MaiScreenshotTarget target;
    std::string title;
    std::uint64_t windowId = 0;
    std::uint64_t displayId = 0;
    std::uint64_t afterFrameSequence = 0;
    std::int64_t afterFrameReceivedAtMs = 0;
    std::atomic<bool> canceled{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    MaiError error;
    MaiScreenshot image;
    FrameStamp stamp;
};

struct CaptureTarget {
    bool display = false;
    std::uint32_t id = 0;
    double width = 0;
    double height = 0;
    bool operator==(const CaptureTarget& other) const {
        return display == other.display && id == other.id && width == other.width &&
               height == other.height;
    }
};

bool displayGeometry(CGDirectDisplayID id, CaptureTarget& target) {
    if (!id || !CGDisplayIsActive(id)) return false;
    const CGRect bounds = CGDisplayBounds(id);
    if (!std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width <= 0 || bounds.size.height <= 0)
        return false;
    target = {true, id, bounds.size.width, bounds.size.height};
    return true;
}

bool windowGeometry(std::uint32_t id, CaptureTarget& target) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id);
    bool valid = false;
    if (windows) {
        for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
            const auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
            const auto number =
                static_cast<CFNumberRef>(CFDictionaryGetValue(item, kCGWindowNumber));
            std::uint32_t actual = 0;
            if (!number || CFGetTypeID(number) != CFNumberGetTypeID() ||
                !CFNumberGetValue(number, kCFNumberSInt32Type, &actual) || actual != id)
                continue;
            const auto visible =
                static_cast<CFBooleanRef>(CFDictionaryGetValue(item, kCGWindowIsOnscreen));
            const auto bounds =
                static_cast<CFDictionaryRef>(CFDictionaryGetValue(item, kCGWindowBounds));
            CGRect rect{};
            if (visible && CFGetTypeID(visible) == CFBooleanGetTypeID() &&
                CFBooleanGetValue(visible) && bounds &&
                CGRectMakeWithDictionaryRepresentation(bounds, &rect) && rect.size.width > 0 &&
                rect.size.height > 0) {
                target = {false, id, rect.size.width, rect.size.height};
                valid = true;
            }
        }
        CFRelease(windows);
    }
    return valid;
}

class ResidentCapture {
public:
    explicit ResidentCapture(CaptureCounters& counters)
        : state_(std::make_shared<StreamState>(counters)), thread_([this] { run(); }) {}
    ~ResidentCapture() {
        stop();
    }

    bool submit(const std::shared_ptr<CaptureCommand>& command) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (state_->stopping.load()) return false;
        commands_.push_back(command);
        queueCondition_.notify_one();
        return true;
    }
    void cancel(const std::shared_ptr<CaptureCommand>& command) {
        command->canceled = true;
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            const auto found = std::find(commands_.begin(), commands_.end(), command);
            if (found != commands_.end()) {
                commands_.erase(found);
                removed = true;
            }
        }
        if (removed)
            finish(command, MaiError::make(MaiErrorCode::Canceled, "screen capture canceled"));
        state_->condition.notify_all();
    }
    MaiError takeForDelivery(CaptureCommand& command, MaiScreenshot& image) {
        if (!desktopCanCapture(readDesktopAvailability()) || !CGPreflightScreenCaptureAccess()) {
            const auto error = MaiError::make(MaiErrorCode::NotConfigured,
                                              "screen capture context became unavailable");
            rejectContext(error);
            return error;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping.load())
            return MaiError::make(MaiErrorCode::Canceled, "screen capture stopped");
        if (!frameStampIsCurrent(*state_, command.stamp))
            return MaiError::make(MaiErrorCode::Protocol,
                                  "screen frame became unavailable before delivery");
        // 取结果和校验在同一锁内，失效 callback 不能夹在检查与结果发布之间。
        image = std::move(command.image);
        return {};
    }
    void rejectContext(const MaiError& error) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            rejectDesktopLocked(*state_, error);
        }
        state_->condition.notify_all();
    }
    MaiError stop() {
        std::lock_guard<std::mutex> lock(stopMutex_);
        state_->stopping = true;
        queueCondition_.notify_all();
        state_->condition.notify_all();
        if (thread_.joinable()) thread_.join();
        return stopError_;
    }

private:
    static void finish(const std::shared_ptr<CaptureCommand>& command, MaiError error,
                       MaiScreenshot image = {}) {
        {
            std::lock_guard<std::mutex> lock(command->mutex);
            command->error = std::move(error);
            command->image = std::move(image);
            command->done = true;
        }
        command->condition.notify_all();
    }
    MaiError waitNative(const std::shared_ptr<NativeCompletion>& completion, bool stopping = false,
                        bool cancelCommand = false) {
        const auto deadline = std::chrono::steady_clock::now() + kCaptureTimeout;
        std::unique_lock<std::mutex> lock(completion->mutex);
        while (!completion->done) {
            if ((!stopping && state_->stopping.load()) ||
                (cancelCommand && activeCommand_ && activeCommand_->canceled.load()))
                return MaiError::make(MaiErrorCode::Canceled, "screen capture stopped");
            if (completion->condition.wait_until(
                    lock, std::min(deadline, std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(20))) ==
                    std::cv_status::timeout &&
                std::chrono::steady_clock::now() >= deadline)
                return MaiError::make(MaiErrorCode::Internal,
                                      "native screen capture operation timed out");
        }
        return completion->error;
    }
    MaiError content(SCShareableContent* __strong& result) {
        const auto completion = std::make_shared<NativeCompletion>();
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent* value,
                                                         NSError* error) {
                                       {
                                           std::lock_guard<std::mutex> lock(completion->mutex);
                                           completion->content = value;
                                       }
                                       completeNative(completion, error);
                                     }];
        MaiError error = waitNative(completion, false, true);
        if (error) return error;
        result = completion->content;
        return result ? MaiError{}
                      : MaiError::make(MaiErrorCode::NotFound, "screen content is unavailable");
    }
    MaiError resolve(const CaptureCommand& command, CaptureTarget& target,
                     SCShareableContent* __strong& available) {
        if (command.target == MaiScreenshotTarget::DisplayById) {
            // 显式显示器沿用同一常驻 SCK 流和队列，目标消失时失败，
            // 不把模型静默转到鼠标所在屏或主屏。
            return displayGeometry(static_cast<CGDirectDisplayID>(command.displayId), target)
                       ? MaiError{}
                       : MaiError::make(MaiErrorCode::NotFound,
                                        "requested display is not active or no longer exists");
        }
        if (command.target == MaiScreenshotTarget::WindowById) {
            return windowGeometry(static_cast<std::uint32_t>(command.windowId), target)
                       ? MaiError{}
                       : MaiError::make(MaiErrorCode::NotFound,
                                        "window is not visible or no longer exists");
        }
        if (command.target == MaiScreenshotTarget::WindowByTitle) {
            MaiError error = content(available);
            if (error) return error;
            SCWindow* window = windowWithTitle(available, command.title, error);
            if (!window) return error;
            return windowGeometry(window.windowID, target)
                       ? MaiError{}
                       : MaiError::make(MaiErrorCode::NotFound,
                                        "requested window is no longer visible");
        }
        CGDirectDisplayID displays[32]{};
        std::uint32_t count = 0;
        if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess || !count)
            return MaiError::make(MaiErrorCode::NotFound, "no active display is available");
        CGEventRef event = CGEventCreate(nullptr);
        const CGPoint point = event ? CGEventGetLocation(event) : CGPointZero;
        if (event) CFRelease(event);
        CGDirectDisplayID selected = CGMainDisplayID();
        for (std::uint32_t i = 0; i < count; ++i)
            if (CGRectContainsPoint(CGDisplayBounds(displays[i]), point)) {
                selected = displays[i];
                break;
            }
        return displayGeometry(selected, target)
                   ? MaiError{}
                   : MaiError::make(MaiErrorCode::NotFound,
                                    "pointer display is not active or no longer exists");
    }
    MaiError closeNative() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->generation;
            invalidateFrameLocked(*state_);
            state_->error = {};
            state_->failed = false;
        }
        state_->counters.update([](auto& counts) { counts.capturing = false; });
        if (!stream_) return {};
        NSError* ignored = nil;
        [stream_ removeStreamOutput:output_ type:SCStreamOutputTypeScreen error:&ignored];
        const auto completion = std::make_shared<NativeCompletion>();
        [stream_ stopCaptureWithCompletionHandler:^(NSError* error) {
          completeNative(completion,
                         error.code == SCStreamErrorAttemptToStopStreamState ? nil : error);
        }];
        const MaiError result = waitNative(completion, true);
        stream_ = nil;
        output_ = nil;
        return result;
    }
    MaiError ensureNative(CaptureTarget& target, SCShareableContent* available) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (stream_ && target == target_ && !state_->failed) return {};
        }
        // 只在目标/尺寸变化或原生 session 故障时重建；普通 Command 不重建流。
        const MaiError closed = closeNative();
        if (closed) return closed;
        if (!available) {
            const MaiError error = content(available);
            if (error) return error;
        }
        SCContentFilter* filter = nil;
        if (target.display) {
            for (SCDisplay* display in available.displays)
                if (display.displayID == target.id)
                    filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        } else {
            MaiError error;
            SCWindow* window = windowWithId(available, target.id, error);
            if (!window) return error;
            // 枚举是异步的；创建流前重新取得同一 ID 的几何，不能把枚举前的尺寸当成当前尺寸。
            if (!windowGeometry(target.id, target))
                return MaiError::make(MaiErrorCode::NotFound,
                                      "requested window is no longer visible");
            filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
        }
        if (!filter) return MaiError::make(MaiErrorCode::NotFound, "capture target is unavailable");
        SCStreamConfiguration* configuration = screenshotConfiguration(filter);
        if (!configuration)
            return MaiError::make(MaiErrorCode::InvalidInput, "capture dimensions are invalid");
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.colorSpaceName = kCGColorSpaceSRGB;
        configuration.minimumFrameInterval = CMTimeMake(1, 5);
        configuration.queueDepth = 3;
        configuration.capturesAudio = NO;
        output_ = [[MaiScreenshotStreamOutput alloc] init];
        output_->state = state_;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            output_->generation = state_->generation;
        }
        stream_ = [[SCStream alloc] initWithFilter:filter
                                     configuration:configuration
                                          delegate:output_];
        NSError* nativeError = nil;
        if (![stream_ addStreamOutput:output_
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:outputQueue_
                                error:&nativeError]) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->failed = true;
            return MaiError::make(MaiErrorCode::Internal,
                                  errorText(nativeError, "cannot attach screen frame output"));
        }
        const auto completion = std::make_shared<NativeCompletion>();
        [stream_ startCaptureWithCompletionHandler:^(NSError* error) {
          completeNative(completion, error);
        }];
        const MaiError started = waitNative(completion);
        if (started) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->failed = true;
            return started;
        }
        target_ = target;
        state_->counters.update([](auto& counts) {
            ++counts.sessionStarts;
            counts.capturing = true;
        });
        return {};
    }
    MaiError process(const std::shared_ptr<CaptureCommand>& command, MaiScreenshot& image) {
        if (command->canceled.load() || state_->stopping.load())
            return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
        if (!desktopCanCapture(readDesktopAvailability())) {
            const auto error = MaiError::make(MaiErrorCode::NotConfigured,
                                              "GUI session is unavailable for screen capture");
            rejectContext(error);
            return error;
        }
        CaptureTarget target;
        SCShareableContent* available = nil;
        MaiError error = resolve(*command, target, available);
        if (error) return error;
        error = ensureNative(target, available);
        if (error) return error;
        std::shared_ptr<const CapturedPixels> frame;
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            const bool fresh =
                command->afterFrameSequence != 0 || command->afterFrameReceivedAtMs != 0;
            const auto deadline =
                std::chrono::steady_clock::now() + (fresh ? kFreshFrameTimeout : kCaptureTimeout);
            const auto ready = [&] {
                return state_->pixels &&
                       (!fresh ||
                        (state_->pixels->sequence > command->afterFrameSequence &&
                         state_->pixels->received > command->afterFrameReceivedAtMs &&
                         state_->pixels->sourceProducedAtMs > command->afterFrameReceivedAtMs));
            };
            // 输入后的观察必须同时越过序号、接收时间和源帧时间边界，排除输入前已生成、
            // 输入后才到 callback 的旧帧；Idle 不伪造新帧，失败仍保留健康常驻流。
            while (!ready() && !state_->error) {
                if (command->canceled.load() || state_->stopping.load())
                    return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
                if (fresh && state_->pixels &&
                    state_->pixels->sequence > command->afterFrameSequence &&
                    state_->pixels->received > command->afterFrameReceivedAtMs &&
                    state_->pixels->sourceProducedAtMs == 0)
                    return MaiError::make(
                        MaiErrorCode::NotSupported,
                        "native screen frame source time is unavailable for a fresh observation");
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return fresh ? MaiError::make(MaiErrorCode::Busy,
                                                  "no newer screen frame arrived after input")
                                 : MaiError::make(MaiErrorCode::Internal,
                                                  "screen capture first frame timed out");
                state_->condition.wait_until(
                    lock, std::min(deadline, now + std::chrono::milliseconds(20)));
            }
            if (state_->error) return state_->error;
            frame = state_->pixels;
            command->stamp = {state_->generation, state_->availabilityEpoch};
        }
        if (command->canceled.load() || state_->stopping.load())
            return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
        CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bitmap = CGBitmapContextCreate(
            const_cast<std::uint8_t*>(frame->bytes.data()), frame->width, frame->height, 8,
            frame->stride, colors, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        CGImageRef native = bitmap ? CGBitmapContextCreateImage(bitmap) : nullptr;
        error = encodePng(native, image);  // 仅 Command 编码，帧回调不创建 PNG。
        if (native) CGImageRelease(native);
        if (bitmap) CGContextRelease(bitmap);
        if (colors) CGColorSpaceRelease(colors);
        if (error) return error;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (command->canceled.load() || state_->stopping.load())
                return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
            if (!frameStampIsCurrent(*state_, command->stamp))
                return MaiError::make(MaiErrorCode::Busy,
                                      "screen frame became unavailable while encoding");
        }
        if (!target.display) {
            CaptureTarget current;
            if (!windowGeometry(target.id, current) || !(current == target))
                return MaiError::make(MaiErrorCode::NotFound,
                                      "window changed during screen capture");
        } else {
            CaptureTarget current;
            if (!displayGeometry(target.id, current) || !(current == target))
                return MaiError::make(MaiErrorCode::NotFound,
                                      "display changed during screen capture");
        }
        image.frameSequence = frame->sequence;
        image.frameReceivedAtMs = frame->received;
        return {};
    }
    void run() {
        MaiThread::setCurrentName("mai-screen");
        state_->counters.update([](auto& counts) {
            ++counts.workerStarts;
            counts.workerRunning = true;
        });
        while (!state_->stopping.load()) {
            std::shared_ptr<CaptureCommand> command;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCondition_.wait(lock,
                                     [&] { return state_->stopping.load() || !commands_.empty(); });
                if (state_->stopping.load()) break;
                command = commands_.front();
                commands_.pop_front();
            }
            @autoreleasepool {
                activeCommand_ = command;
                MaiScreenshot image;
                MaiError error;
                try {
                    error = process(command, image);
                } catch (const std::exception&) {
                    error = MaiError::make(MaiErrorCode::Internal, "screen capture worker failed");
                }
                if (error) image = {};
                finish(command, std::move(error), std::move(image));
                activeCommand_.reset();
            }
        }
        @autoreleasepool {
            stopError_ = closeNative();
        }
        std::deque<std::shared_ptr<CaptureCommand>> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pending.swap(commands_);
        }
        for (const auto& command : pending)
            finish(command, MaiError::make(MaiErrorCode::Canceled, "screen capture stopped"));
        state_->counters.update([](auto& counts) {
            counts.workerRunning = false;
            counts.capturing = false;
        });
    }

    std::shared_ptr<StreamState> state_;
    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::shared_ptr<CaptureCommand>> commands_;
    std::shared_ptr<CaptureCommand>
        activeCommand_;  // 仅 worker 读写；其他线程只写 Command 的 atomic。
    std::mutex stopMutex_;
    MaiError stopError_;
    dispatch_queue_t outputQueue_ =
        dispatch_queue_create("mai-screen-output", DISPATCH_QUEUE_SERIAL);
    SCStream* stream_ = nil;
    MaiScreenshotStreamOutput* output_ = nil;
    CaptureTarget target_;
    std::thread thread_;  // 最后初始化，worker 不观察未构造完成的成员。
};

class CaptureRegistry {
public:
    ~CaptureRegistry() {
        stop();
    }
    std::atomic<std::uint64_t> epoch{0};  // 奇数表示 stop 正在回收；调用者不跨 stop 偷启动新服务。
    CaptureCounters counters;
    std::shared_ptr<ResidentCapture> acquire(std::uint64_t expected) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected != epoch.load() || (expected & 1)) return {};
        if (!capture_) capture_ = std::make_shared<ResidentCapture>(counters);
        return capture_;
    }
    void rejectContext(const MaiError& error) {
        std::shared_ptr<ResidentCapture> current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = capture_;
        }
        if (current) current->rejectContext(error);  // 不为拒绝或状态读取启动 worker。
    }
    MaiError stop() {
        std::lock_guard<std::mutex> serialize(stopMutex_);
        ++epoch;
        std::shared_ptr<ResidentCapture> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old.swap(capture_);
        }
        const MaiError error = old ? old->stop() : MaiError{};
        ++epoch;
        return error;
    }

private:
    std::mutex mutex_;
    std::mutex stopMutex_;
    std::shared_ptr<ResidentCapture> capture_;
};

CaptureRegistry& registry() {
    static CaptureRegistry value;
    return value;
}

}  // namespace

bool maiIsScreenshotSupported() {
    if (@available(macOS 14.0, *)) return true;
    return false;
}

MaiError maiCaptureScreenshot(const MaiScreenshotRequest& request, MaiScreenshot& screenshot) {
    maiAssertBlockingAllowed("maiCaptureScreenshot");
    screenshot = {};
    auto& service = registry();
    const std::uint64_t epoch = service.epoch.load();
    if ((epoch & 1) || (request.isCanceled && request.isCanceled()))
        return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
    if (!maiIsScreenshotSupported())
        return MaiError::make(MaiErrorCode::NotSupported,
                              "ScreenCaptureKit requires macOS 14 or later");
    if (request.target != MaiScreenshotTarget::DisplayUnderPointer &&
        request.target != MaiScreenshotTarget::WindowByTitle &&
        request.target != MaiScreenshotTarget::WindowById &&
        request.target != MaiScreenshotTarget::DisplayById)
        return MaiError::make(MaiErrorCode::InvalidInput, "unsupported native screenshot target");
    if (request.target == MaiScreenshotTarget::WindowById &&
        (!request.windowId || request.windowId > std::numeric_limits<CGWindowID>::max()))
        return MaiError::make(MaiErrorCode::InvalidInput,
                              "windowId must be a non-zero native window identifier");
    if (request.target == MaiScreenshotTarget::DisplayById &&
        (!request.displayId || request.displayId > std::numeric_limits<CGDirectDisplayID>::max()))
        return MaiError::make(MaiErrorCode::InvalidInput,
                              "displayId must be a non-zero native display identifier");
    // 目标有效性不依赖屏幕录制授权。活动表中没有的显式 ID 在 GUI/TCC/worker 之前
    // 拒绝，纯负例校验不会触发系统授权请求；worker 在使用前仍复核同一 ID。
    if (request.target == MaiScreenshotTarget::DisplayById &&
        !CGDisplayIsActive(static_cast<CGDirectDisplayID>(request.displayId)))
        return MaiError::make(MaiErrorCode::NotFound,
                              "requested display is not active or no longer exists");
    if (!desktopCanCapture(readDesktopAvailability())) {
        const auto error = MaiError::make(MaiErrorCode::NotConfigured,
                                          "GUI session is unavailable for screen capture");
        service.rejectContext(error);
        return error;
    }
    if (!CGPreflightScreenCaptureAccess()) {
        service.rejectContext(MaiError::make(MaiErrorCode::NotConfigured,
                                             "screen recording permission is unavailable"));
        CGRequestScreenCaptureAccess();
        return MaiError::make(
            MaiErrorCode::NotConfigured,
            "Allow Screen & System Audio Recording for MaiChat, restart it, and retry");
    }
    const auto worker = service.acquire(epoch);
    if (!worker) return MaiError::make(MaiErrorCode::Canceled, "screen capture stopped");
    const auto command = std::make_shared<CaptureCommand>();
    command->target = request.target;
    command->title = request.windowTitle;
    command->windowId = request.windowId;
    command->displayId = request.displayId;
    command->afterFrameSequence = request.afterFrameSequence;
    command->afterFrameReceivedAtMs = request.afterFrameReceivedAtMs;
    if (!worker->submit(command))
        return MaiError::make(MaiErrorCode::Canceled, "screen capture stopped");
    // callback 只存在于本调用的栈上，异步命令只保存 atomic 取消标记。
    bool canceled = false;
    for (;;) {
        const bool requested = !canceled && request.isCanceled && request.isCanceled();
        if (!canceled && (requested || service.epoch.load() != epoch)) {
            canceled = true;
            worker->cancel(command);
        }
        std::unique_lock<std::mutex> lock(command->mutex);
        if (command->done) {
            if (canceled || service.epoch.load() != epoch)
                return MaiError::make(MaiErrorCode::Canceled, "screen capture canceled");
            if (command->error) return command->error;
            const MaiError delivered = worker->takeForDelivery(*command, screenshot);
            if (service.epoch.load() != epoch) {
                screenshot = {};
                return MaiError::make(MaiErrorCode::Canceled, "screen capture stopped");
            }
            return delivered;
        }
        command->condition.wait_for(lock, std::chrono::milliseconds(10));
    }
}

MaiScreenshotCaptureState maiScreenshotCaptureState() {
    auto& counters = registry().counters;
    std::lock_guard<std::mutex> lock(counters.mutex);
    return counters.value;
}

MaiError maiStopScreenshotCapture() {
    maiAssertBlockingAllowed("maiStopScreenshotCapture");
    return registry().stop();
}

MaiError maiListCaptureWindows(std::vector<std::string>& titles) {
    maiAssertBlockingAllowed("maiListCaptureWindows");
    titles.clear();
    if (!maiIsScreenshotSupported()) {
        return MaiError::make(MaiErrorCode::NotSupported,
                              "ScreenCaptureKit requires macOS 14 or later");
    }
    if (!CGPreflightScreenCaptureAccess()) {
        return MaiError::make(MaiErrorCode::NotConfigured,
                              "Allow Screen & System Audio Recording for MaiChat and restart it");
    }
    const auto state = std::make_shared<MaiCaptureState>();
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* content, NSError* error) {
                                   @autoreleasepool {
                                       if (!content) {
                                           finishCapture(
                                               state,
                                               MaiError::make(
                                                   MaiErrorCode::Internal,
                                                   errorText(error,
                                                             "screen content is unavailable")));
                                           return;
                                       }
                                       std::vector<std::string> found;
                                       for (SCWindow* window in content.windows) {
                                           if (!window.isOnScreen || window.windowLayer != 0 ||
                                               window.title.length == 0)
                                               continue;
                                           const char* utf8 = window.title.UTF8String;
                                           if (utf8 && *utf8) found.emplace_back(utf8);
                                       }
                                       std::sort(found.begin(), found.end());
                                       {
                                           std::lock_guard<std::mutex> guard(state->mutex);
                                           state->titles = std::move(found);
                                           state->finished = true;
                                       }
                                       state->condition.notify_one();
                                   }
                                 }];
    MaiScreenshot unused;
    const MaiError result = waitForCapture(state, unused);
    if (result.hasError()) return result;
    titles = std::move(state->titles);
    return {};
}
