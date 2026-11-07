// 离线白盒只编入独立测试可执行文件；正常 target 仍使用下面的公共 API 原生夹具。
#if defined(MAI_SCREENSHOT_STATE_TEST)
#include "../AgentTools/MaiScreenshotMac.mm"
#include <cstdio>

namespace {
int stateFailures = 0;
#define STATE_CHECK(value)                                                      \
    do {                                                                        \
        if (!(value)) {                                                         \
            ++stateFailures;                                                    \
            std::fprintf(stderr, "state FAIL line %d: %s\n", __LINE__, #value); \
        }                                                                       \
    } while (false)
CMSampleBufferRef stateSample(SCFrameStatus status) {
    CVPixelBufferRef pixels = nullptr;
    if (CVPixelBufferCreate(nullptr, 1, 1, kCVPixelFormatType_32BGRA, nullptr, &pixels) !=
        kCVReturnSuccess)
        return nullptr;
    CVPixelBufferLockBaseAddress(pixels, 0);
    auto* data = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pixels));
    data[0] = 5;
    data[1] = 10;
    data[2] = 200;
    data[3] = 255;
    CVPixelBufferUnlockBaseAddress(pixels, 0);
    CMVideoFormatDescriptionRef format = nullptr;
    CMSampleBufferRef sample = nullptr;
    CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(1, 1), kCMTimeInvalid};
    if (CMVideoFormatDescriptionCreateForImageBuffer(nullptr, pixels, &format) == noErr)
        CMSampleBufferCreateForImageBuffer(nullptr, pixels, true, nullptr, nullptr, format, &timing,
                                           &sample);
    if (format) CFRelease(format);
    CFRelease(pixels);
    if (!sample) return nullptr;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    auto info = static_cast<CFMutableDictionaryRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
    NSInteger raw = status;
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberNSIntegerType, &raw);
    CFDictionarySetValue(info, (__bridge const void*)SCStreamFrameInfoStatus, number);
    CFRelease(number);
    return sample;
}
void inject(const std::shared_ptr<StreamState>& state, SCFrameStatus status) {
    CMSampleBufferRef sample = stateSample(status);
    STATE_CHECK(sample != nullptr);
    const DesktopAvailability available{true, true, true, true, false};
    if (sample) {
        receiveFrame(state, state->generation, sample, &available);
        CFRelease(sample);
    }
}
void stateRace(SCFrameStatus invalid) {
    CaptureCounters counters;
    auto state = std::make_shared<StreamState>(counters);
    inject(state, SCFrameStatusComplete);
    STATE_CHECK(state->pixels != nullptr);
    const auto oldFrame = state->pixels;
    const FrameStamp old{state->generation, state->availabilityEpoch};
    STATE_CHECK(frameStampIsCurrent(*state, old));
    inject(state, SCFrameStatusIdle);
    STATE_CHECK(state->pixels == oldFrame);
    STATE_CHECK(frameStampIsCurrent(*state, old));
    // 模拟 Command 持有旧帧编码期间的失效和恢复，不启动 SCStream，也不读取用户像素。
    inject(state, invalid);
    STATE_CHECK(!frameStampIsCurrent(*state, old));
    inject(state, SCFrameStatusComplete);
    if (invalid == SCFrameStatusStopped) {
        STATE_CHECK(!state->pixels);  // 停止后的迟到帧不能复活同一个 native session。
        STATE_CHECK(!frameStampIsCurrent(*state, old));
        return;
    }
    STATE_CHECK(state->pixels != nullptr);
    STATE_CHECK(state->pixels != oldFrame);
    STATE_CHECK(state->pixels->sequence > oldFrame->sequence);
    STATE_CHECK(!frameStampIsCurrent(*state, old));
}

void terminalCannotRevive(int terminal, SCFrameStatus soft) {
    CaptureCounters counts;
    auto state = std::make_shared<StreamState>(counts);
    inject(state, SCFrameStatusComplete);
    if (terminal == 0) {
        MaiScreenshotStreamOutput* output = [[MaiScreenshotStreamOutput alloc] init];
        output->state = state;
        output->generation = state->generation;
        [output stream:nil
            didStopWithError:[NSError errorWithDomain:@"MaiScreenshotTest" code:1 userInfo:nil]];
    } else if (terminal == 1) {
        inject(state, SCFrameStatusStopped);
    } else {
        std::lock_guard<std::mutex> lock(state->mutex);
        rejectDesktopLocked(*state,
                            MaiError::make(MaiErrorCode::NotConfigured, "test unavailable"));
    }
    STATE_CHECK(state->failed);
    const auto received = counts.value.framesReceived;
    inject(state, soft);
    STATE_CHECK(state->failed);
    inject(state, SCFrameStatusComplete);
    STATE_CHECK(state->failed && !state->pixels);
    STATE_CHECK(counts.value.framesReceived == received);
}
}  // namespace
int main() {
    @autoreleasepool {
        stateRace(SCFrameStatusBlank);
        stateRace(SCFrameStatusSuspended);
        stateRace(SCFrameStatusStopped);
        stateRace(SCFrameStatusStarted);
        for (int terminal = 0; terminal < 3; ++terminal) {
            terminalCannotRevive(terminal, SCFrameStatusBlank);
            terminalCannotRevive(terminal, SCFrameStatusSuspended);
        }
        const DesktopAvailability available{true, true, true, true, false};
        STATE_CHECK(desktopCanCapture(available));
        for (int field = 0; field < 5; ++field) {
            auto denied = available;
            if (field == 0) denied.guiSession = false;
            if (field == 1) denied.onConsole = false;
            if (field == 2) denied.loginDone = false;
            if (field == 3) denied.frontmostKnown = false;
            if (field == 4) denied.loginWindow = true;
            STATE_CHECK(!desktopCanCapture(denied));
            CaptureCounters counts;
            auto state = std::make_shared<StreamState>(counts);
            inject(state, SCFrameStatusComplete);
            const FrameStamp old{state->generation, state->availabilityEpoch};
            CMSampleBufferRef sample = stateSample(SCFrameStatusIdle);
            receiveFrame(state, state->generation, sample, &denied);
            CFRelease(sample);
            STATE_CHECK(!state->pixels && !frameStampIsCurrent(*state, old));
        }
        {
            CaptureCounters counts;
            auto state = std::make_shared<StreamState>(counts);
            inject(state, SCFrameStatusComplete);
            const FrameStamp old{state->generation, state->availabilityEpoch};
            MaiScreenshotStreamOutput* output = [[MaiScreenshotStreamOutput alloc] init];
            output->state = state;
            output->generation = state->generation;
            [output stream:nil
                didStopWithError:[NSError errorWithDomain:@"MaiScreenshotTest"
                                                     code:1
                                                 userInfo:nil]];
            inject(state, SCFrameStatusComplete);
            STATE_CHECK(!state->pixels && !frameStampIsCurrent(*state, old));
        }
        std::printf("offline availability tests: failures=%d (no stream/UI/capture)\n",
                    stateFailures);
        return stateFailures ? 1 : 0;
    }
}
#else
// 文件作用：只用本进程 NSWindow 验证原生采集会话复用、取消和停止，不采集用户显示器。
#include "MaiScreenshot.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiThread.h"

@interface MaiScreenshotSessionTestView : NSView
@property(nonatomic) BOOL red;
@end

@implementation MaiScreenshotSessionTestView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [(self.red ? [NSColor colorWithSRGBRed:0.85 green:0.08 blue:0.05 alpha:1.0]
               : [NSColor colorWithSRGBRed:0.05 green:0.08 blue:0.85 alpha:1.0]) setFill];
    NSRectFill(self.bounds);
    [@"OWNED RESIDENT CAPTURE" drawAtPoint:NSMakePoint(20, 20)
                            withAttributes:@{NSForegroundColorAttributeName : NSColor.whiteColor}];
}
@end

namespace {
int failures = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                               \
        }                                                                             \
    } while (false)

void pump() {
    @autoreleasepool {
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        NSEvent* event = nil;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:NSDate.distantPast
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]))
            [NSApp sendEvent:event];
        [NSApp updateWindows];
    }
}

bool waitForOwnedWindow(NSWindow* window) {
    CGRect previous{};
    auto stable = std::chrono::steady_clock::now();
    const auto deadline = stable + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        CFArrayRef entries = CGWindowListCopyWindowInfo(
            kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>(window.windowNumber));
        CGRect current{};
        bool visible = false;
        if (entries && CFArrayGetCount(entries)) {
            const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(entries, 0));
            const auto shown =
                static_cast<CFBooleanRef>(CFDictionaryGetValue(entry, kCGWindowIsOnscreen));
            const auto bounds =
                static_cast<CFDictionaryRef>(CFDictionaryGetValue(entry, kCGWindowBounds));
            visible = shown && CFBooleanGetValue(shown) && bounds &&
                      CGRectMakeWithDictionaryRepresentation(bounds, &current);
        }
        if (entries) CFRelease(entries);
        if (!visible || !CGRectEqualToRect(current, previous))
            stable = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - stable >= std::chrono::milliseconds(300))
            return true;
        previous = current;
        pump();
    }
    return false;
}

template <class T>
bool ready(std::future<T>& future, int milliseconds = 25000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        pump();
    }
    return true;
}

struct Capture {
    MaiError error;
    MaiScreenshot image;
};

std::future<Capture> capture(MaiScreenshotRequest request) {
    return std::async(std::launch::async, [request = std::move(request)] {
        MaiThread::setCurrentName("mai-shot-test");
        Capture result;
        result.error = maiCaptureScreenshot(request, result.image);
        return result;
    });
}

bool redDominates(const MaiScreenshot& image, bool red) {
    CFDataRef data = CFDataCreate(nullptr, image.pngBytes.data(), image.pngBytes.size());
    CGImageSourceRef source = data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;
    CGImageRef decoded = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    unsigned char rgba[4]{};
    CGColorSpaceRef colors = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        rgba, 1, 1, 8, 4, colors, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    if (decoded && context) {
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
        CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), decoded);
    }
    const bool valid = decoded && context &&
                       (red ? int(rgba[0]) > int(rgba[2]) + 80 : int(rgba[2]) > int(rgba[0]) + 80);
    if (context) CGContextRelease(context);
    if (colors) CGColorSpaceRelease(colors);
    if (decoded) CGImageRelease(decoded);
    if (source) CFRelease(source);
    if (data) CFRelease(data);
    return valid;
}

void save(const char* name, const MaiScreenshot& image) {
    const char* directory = std::getenv("MAI_SCREENSHOT_SESSION_EVIDENCE");
    if (!directory || image.pngBytes.empty()) return;
    const auto root = MaiFilePath::fromUtf8(directory);
    CHECK(!MaiFileSystem::createDirectories(root));
    const std::string bytes(reinterpret_cast<const char*>(image.pngBytes.data()),
                            image.pngBytes.size());
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8(name)), bytes));
}

void sameSession(const MaiScreenshotCaptureState& expected) {
    const auto actual = maiScreenshotCaptureState();
    CHECK(actual.workerRunning && actual.capturing);
    CHECK(actual.workerStarts == expected.workerStarts);
    CHECK(actual.deviceCreations == 0);  // Mac 不将 SCStream 当作 GPU device 计数。
    CHECK(actual.sessionStarts == expected.sessionStarts);
}
}  // namespace

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        if (!maiIsScreenshotSupported() || !CGPreflightScreenCaptureAccess()) {
            std::printf("SKIP: native screen recording is unavailable; no capture attempted\n");
            return 77;
        }
        CHECK(!maiStopScreenshotCapture());
        const auto cold = maiScreenshotCaptureState();
        CHECK(!cold.workerRunning && !cold.capturing);
        CHECK(maiScreenshotCaptureState().workerStarts == cold.workerStarts);

        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 420, 280)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        window.title = [@"MaiChat owned resident " stringByAppendingString:NSUUID.UUID.UUIDString];
        window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace;
        MaiScreenshotSessionTestView* view =
            [[MaiScreenshotSessionTestView alloc] initWithFrame:window.contentView.bounds];
        view.red = YES;
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [view display];
        for (int i = 0; i < 20; ++i) pump();
        CHECK(waitForOwnedWindow(window));
        MaiScreenshotRequest request;
        request.target = MaiScreenshotTarget::WindowById;
        request.windowId = static_cast<std::uint64_t>(window.windowNumber);
        CHECK(request.windowId != 0);

        MaiScreenshotRequest canceled = request;
        canceled.isCanceled = [] { return true; };
        auto rejected = capture(canceled);
        CHECK(ready(rejected));
        const auto rejectedResult = rejected.get();
        CHECK(rejectedResult.error.code() == MaiErrorCode::Canceled);
        CHECK(rejectedResult.image.pngBytes.empty());
        CHECK(maiScreenshotCaptureState().workerStarts == cold.workerStarts);

        auto first = capture(request);
        CHECK(ready(first));
        auto original = first.get();
        if (original.error)
            std::fprintf(stderr, "owned capture error: %d %s (window=%llu)\n",
                         int(original.error.code()), original.error.message().c_str(),
                         static_cast<unsigned long long>(request.windowId));
        CHECK(!original.error);
        CHECK(!original.image.pngBytes.empty());
        CHECK(redDominates(original.image, true));
        std::printf("owned PNG: width=%d height=%d bytes=%zu sequence=%llu received=%lld\n",
                    original.image.width, original.image.height, original.image.pngBytes.size(),
                    static_cast<unsigned long long>(original.image.frameSequence),
                    static_cast<long long>(original.image.frameReceivedAtMs));
        CHECK(original.image.frameSequence > 0 && original.image.frameReceivedAtMs > 0);
        const auto warm = maiScreenshotCaptureState();
        CHECK(warm.workerRunning && warm.capturing);
        CHECK(warm.workerStarts == cold.workerStarts + 1);
        CHECK(warm.deviceCreations == 0);
        CHECK(warm.sessionStarts == cold.sessionStarts + 1);
        CHECK(warm.framesReceived > cold.framesReceived);
        save("resident-red.png", original.image);

        for (int i = 0; i < 3; ++i) {
            auto next = capture(request);
            CHECK(ready(next));
            auto result = next.get();
            CHECK(!result.error);
            CHECK(redDominates(result.image, true));
            CHECK(result.image.frameSequence >= original.image.frameSequence);
            if (result.image.frameSequence == original.image.frameSequence)
                CHECK(result.image.frameReceivedAtMs == original.image.frameReceivedAtMs);
            sameSession(warm);
        }

        view.red = NO;
        view.needsDisplay = YES;
        [view display];
        Capture blue;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            auto changed = capture(request);
            CHECK(ready(changed));
            blue = changed.get();
            if (!blue.error && redDominates(blue.image, false)) break;
            for (int i = 0; i < 5; ++i) pump();
        } while (std::chrono::steady_clock::now() < deadline);
        CHECK(!blue.error && redDominates(blue.image, false));
        CHECK(blue.image.frameSequence > original.image.frameSequence);
        CHECK(blue.image.frameReceivedAtMs >= original.image.frameReceivedAtMs);
        sameSession(warm);
        save("resident-blue.png", blue.image);

        // 取消仅影响本次 Command，且 callback 不能被常驻后端保留到返回之后。
        auto alive = std::make_shared<std::atomic<bool> >(true);
        auto late = std::make_shared<std::atomic<int> >(0);
        auto cancelChecks = std::make_shared<std::atomic<int> >(0);
        canceled.isCanceled = [alive, late, cancelChecks] {
            if (!alive->load()) ++*late;
            return ++*cancelChecks >= 2;  // 第一遍允许入队，随后取消该 Command。
        };
        auto canceledWarm = capture(canceled);
        CHECK(ready(canceledWarm));
        const auto canceledResult = canceledWarm.get();
        alive->store(false);
        CHECK(canceledResult.error.code() == MaiErrorCode::Canceled);
        CHECK(canceledResult.image.pngBytes.empty());
        for (int i = 0; i < 10; ++i) pump();
        CHECK(late->load() == 0);
        sameSession(warm);

        std::vector<std::future<Capture> > parallel;
        for (int i = 0; i < 6; ++i) parallel.push_back(capture(request));
        for (auto& future : parallel) {
            CHECK(ready(future));
            const auto result = future.get();
            CHECK(!result.error && redDominates(result.image, false));
        }
        sameSession(warm);

        // 调用者在自己的取消回调内暂停；stop 不能等这个 callback，更不能在返回后交付旧帧。
        auto entered = std::make_shared<std::atomic<bool> >(false);
        auto release = std::make_shared<std::atomic<bool> >(false);
        auto checks = std::make_shared<std::atomic<int> >(0);
        auto stoppingRequest = request;
        stoppingRequest.isCanceled = [entered, release, checks] {
            if (++*checks == 2) {
                entered->store(true);
                while (!release->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        };
        auto duringStop = capture(stoppingRequest);
        const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!entered->load() && std::chrono::steady_clock::now() < stopDeadline) pump();
        CHECK(entered->load());
        for (int i = 0; i < 100; ++i) pump();  // 允许后台完成 PNG，但调用者仍未返回。
        CHECK(!maiStopScreenshotCapture());
        release->store(true);
        CHECK(ready(duringStop));
        const auto interrupted = duringStop.get();
        CHECK(interrupted.error.code() == MaiErrorCode::Canceled);
        CHECK(interrupted.image.pngBytes.empty());
        CHECK(!maiStopScreenshotCapture());
        const auto stopped = maiScreenshotCaptureState();
        CHECK(!stopped.workerRunning && !stopped.capturing);
        CHECK(stopped.workerStarts == warm.workerStarts);
        CHECK(stopped.sessionStarts == warm.sessionStarts);
        for (int i = 0; i < 20; ++i) pump();
        CHECK(maiScreenshotCaptureState().framesReceived == stopped.framesReceived);
        auto restart = capture(request);
        CHECK(ready(restart));
        const auto restarted = restart.get();
        CHECK(!restarted.error && redDominates(restarted.image, false));
        CHECK(maiScreenshotCaptureState().workerStarts == warm.workerStarts + 1);
        CHECK(maiScreenshotCaptureState().sessionStarts == warm.sessionStarts + 1);
        CHECK(restarted.image.frameSequence > blue.image.frameSequence);
        CHECK(!maiStopScreenshotCapture());
        const auto beforeParallelStart = maiScreenshotCaptureState();
        parallel.clear();
        for (int i = 0; i < 6; ++i) parallel.push_back(capture(request));
        for (auto& future : parallel) {
            CHECK(ready(future));
            const auto result = future.get();
            CHECK(!result.error && redDominates(result.image, false));
        }
        const auto afterParallelStart = maiScreenshotCaptureState();
        CHECK(afterParallelStart.workerStarts == beforeParallelStart.workerStarts + 1);
        CHECK(afterParallelStart.sessionStarts == beforeParallelStart.sessionStarts + 1);
        CHECK(!maiStopScreenshotCapture());
        [window close];
        std::printf("resident session tests: failures=%d workers=%llu sessions=%llu frames=%llu\n",
                    failures,
                    static_cast<unsigned long long>(maiScreenshotCaptureState().workerStarts),
                    static_cast<unsigned long long>(maiScreenshotCaptureState().sessionStarts),
                    static_cast<unsigned long long>(maiScreenshotCaptureState().framesReceived));
        return failures ? 1 : 0;
    }
}

#endif  // MAI_SCREENSHOT_STATE_TEST
