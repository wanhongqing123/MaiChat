// 文件作用：无设备/授权的 Mac 相机契约回归，合成 CoreVideo 帧和原生照片交付事件。
// 单独编译本文件即可；包含同一正式实现以访问仅测试构建启用的设备替身，不另链 Mac.mm。
#define MAI_CAMERA_CAPTURE_OFFLINE_TEST 1
#ifndef MAI_CAMERA_MAC_SOURCE
#define MAI_CAMERA_MAC_SOURCE "../MaiCameraCaptureMac.mm"
#endif
#include MAI_CAMERA_MAC_SOURCE

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

// 模拟 PhotoOutput 的弱 delegate 契约，而不是直接制造一个由测试强持有的 delegate。
// 不继承/构造 AVCapturePhotoOutput，也不创建真实 session、设备或授权请求。
@interface MaiWeakNativePhotoOutput : NSObject {
@public
    __weak MaiCameraPhotoDelegate* submittedDelegate;
    unsigned requests;
    bool throwOnRequest;
}
- (NSArray<AVVideoCodecType>*)availablePhotoCodecTypes;
- (void)capturePhotoWithSettings:(AVCapturePhotoSettings*)settings
                        delegate:(id<AVCapturePhotoCaptureDelegate>)delegate;
@end

@implementation MaiWeakNativePhotoOutput
- (NSArray<AVVideoCodecType>*)availablePhotoCodecTypes {
    return @[ AVVideoCodecTypeJPEG ];
}
- (void)capturePhotoWithSettings:(AVCapturePhotoSettings*)settings
                        delegate:(id<AVCapturePhotoCaptureDelegate>)delegate {
    (void)settings;
    ++requests;
    submittedDelegate = (MaiCameraPhotoDelegate*)delegate;
    if (throwOnRequest)
        @throw [NSException exceptionWithName:@"SyntheticPhotoSubmission"
                                       reason:@"Synthetic native photo submission failure"
                                     userInfo:nil];
}
@end

namespace {
unsigned failures = 0;
unsigned checks = 0;
void check(bool value, const char* label) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

struct Fixture {
    std::shared_ptr<OfflineCameraDevice> device = std::make_shared<OfflineCameraDevice>();
    std::unique_ptr<MaiCameraCapture> camera;
    std::shared_ptr<MacCameraRuntime> runtime;
    std::vector<MaiCameraCapture::State> states;
    std::vector<std::string> errors;
    std::vector<MaiCameraCapture::Frame> frames;
    std::vector<MaiCameraCapture::Photo> photos;
    bool callbacksOnQueue = true;
    std::function<void()> frameHook;

    Fixture() {
        nextOfflineCameraDevice = device;
        MaiCameraCapture::Callbacks callbacks;
        callbacks.state = [this](MaiCameraCapture::State state, const std::string& error) {
            callbacksOnQueue &= dispatch_get_specific(deliveryQueueKey) == runtime.get();
            states.push_back(state);
            errors.push_back(error);
        };
        callbacks.frame = [this](const MaiCameraCapture::Frame& frame) {
            callbacksOnQueue &= dispatch_get_specific(deliveryQueueKey) == runtime.get();
            frames.push_back(frame);
            if (frameHook) frameHook();
        };
        callbacks.photo = [this](const MaiCameraCapture::Photo& photo) {
            callbacksOnQueue &= dispatch_get_specific(deliveryQueueKey) == runtime.get();
            photos.push_back(photo);
        };
        camera = std::make_unique<MaiCameraCapture>(std::move(callbacks));
        runtime = device->runtime.lock();
        nextOfflineCameraDevice.reset();
    }
    ~Fixture() {
        camera.reset();
    }
    void flush() {
        dispatch_sync(runtime->sessionQueue, ^{
                      });
        dispatch_sync(runtime->deliveryQueue, ^{
                      });
        dispatch_sync(runtime->sessionQueue, ^{
                      });
        dispatch_sync(runtime->deliveryQueue, ^{
                      });
    }
    std::uint64_t generation() {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        return runtime->generation;
    }
    std::uint64_t token() {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        return runtime->pendingToken;
    }
    void inject(CVPixelBufferRef buffer, std::uint64_t epoch = 0) {
        const auto rt = runtime;
        const auto current = epoch ? epoch : generation();
        dispatch_sync(runtime->deliveryQueue, ^{
          rt->receivePixels(current, buffer);
        });
    }
    void startRunning(CVPixelBufferRef buffer) {
        check(camera->start(), "start accepted");
        flush();
        inject(buffer);
        check(!frames.empty(), "valid first frame delivered");
    }
};

CVPixelBufferRef pixels(OSType format = kCVPixelFormatType_32BGRA) {
    CVPixelBufferRef result = nullptr;
    check(CVPixelBufferCreate(kCFAllocatorDefault, 3, 2, format, nullptr, &result) ==
              kCVReturnSuccess,
          "synthetic pixel buffer created");
    if (!result) return nullptr;
    check(CVPixelBufferLockBaseAddress(result, 0) == kCVReturnSuccess, "synthetic pixels locked");
    auto* bytes = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(result));
    const auto stride = CVPixelBufferGetBytesPerRow(result);
    for (size_t y = 0; y < 2; ++y)
        for (size_t x = 0; x < 3; ++x) {
            bytes[y * stride + x * 4] = std::uint8_t(11 + x);
            bytes[y * stride + x * 4 + 1] = std::uint8_t(21 + y);
            bytes[y * stride + x * 4 + 2] = 31;
            bytes[y * stride + x * 4 + 3] = 7;
        }
    CVPixelBufferUnlockBaseAddress(result, 0);
    return result;
}

NSData* jpegFixture() {
    // 合成测试字节模拟原生 PhotoOutput 的独立静态照片；不来自预览，不使用真实设备。
    const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 255};
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba, sizeof(rgba), nullptr);
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(2, 1, 8, 32, 8, color, kCGImageAlphaLast, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    NSMutableData* result = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)result, CFSTR("public.jpeg"), 1, nullptr);
    CGImageDestinationAddImage(destination, image, nullptr);
    check(CGImageDestinationFinalize(destination), "synthetic JPEG encoded");
    CFRelease(destination);
    CGImageRelease(image);
    CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    return result;
}

MaiCameraPhotoDelegate* photoDelegate(Fixture& fixture, std::uint64_t requestId) {
    MaiCameraPhotoDelegate* delegate = [MaiCameraPhotoDelegate new];
    delegate->owner = fixture.runtime;
    delegate->generation = fixture.generation();
    delegate->token = fixture.token();
    delegate->result.requestId = requestId;
    return delegate;
}

void framesAndRestart() {
    Fixture f;
    check(!f.camera->capturePhoto(1), "no shutter before start");
    check(f.camera->start(), "initial start accepted");
    check(!f.camera->start(), "duplicate start rejected");
    f.flush();
    check(f.device->configured == 1, "fake backend configured once");
    check(f.states == std::vector<MaiCameraCapture::State>{MaiCameraCapture::State::Starting},
          "not Running before first valid frame");
    check(!f.camera->capturePhoto(1), "no shutter before first frame");
    auto* bgra = pixels();
    auto* wrong = pixels(kCVPixelFormatType_32ARGB);
    f.inject(nullptr);
    f.inject(wrong);
    check(f.frames.empty(), "empty/wrong-format frames ignored");
    f.inject(bgra);
    f.inject(bgra);
    check(f.frames.size() == 2, "two valid frames delivered");
    if (f.frames.size() == 2) {
        const auto& frame = f.frames.front();
        check(frame.width == 3 && frame.height == 2 && frame.stride == 12 &&
                  frame.pixels.size() == 24,
              "owning compact top-down BGRA frame");
        check(frame.sequence == 1 && f.frames[1].sequence == 2, "valid frame sequences increase");
        check(frame.pixels[0] == 11 && frame.pixels[12 + 1] == 22 && frame.pixels[3] == 255 &&
                  frame.pixels[23] == 255,
              "stride conversion preserves rows and forces opaque alpha");
        CVPixelBufferLockBaseAddress(bgra, 0);
        std::memset(CVPixelBufferGetBaseAddress(bgra), 0, CVPixelBufferGetDataSize(bgra));
        CVPixelBufferUnlockBaseAddress(bgra, 0);
        check(frame.pixels[0] == 11, "delivered frame owns its source bytes");
    }
    const auto oldGeneration = f.generation();
    f.camera->stop();
    const auto callbacks = f.states.size();
    f.inject(bgra, oldGeneration);
    f.flush();
    check(f.frames.size() == 2 && f.states.size() == callbacks, "no old frame callback after stop");
    f.camera->stop();
    check(f.states.size() == callbacks, "stop is idempotent");
    check(f.camera->start(), "restart accepted after stop");
    f.flush();
    f.inject(bgra, oldGeneration);
    check(f.frames.size() == 2, "previous generation cannot run new session");
    f.inject(bgra);
    check(f.frames.size() == 3 && f.frames.back().sequence == 1,
          "new generation begins with first frame");
    check(f.callbacksOnQueue, "state/frame callbacks use delivery queue");
    CVPixelBufferRelease(wrong);
    CVPixelBufferRelease(bgra);
}

void permissionCancellation() {
    for (const auto permission :
         {OfflineCameraDevice::Permission::Denied, OfflineCameraDevice::Permission::Restricted}) {
        Fixture f;
        f.device->permission = permission;
        check(f.camera->start(), "denied start reports asynchronously");
        f.flush();
        check(f.device->configured == 0 && f.states.back() == MaiCameraCapture::State::Error &&
                  !f.errors.back().empty(),
              "denied/restricted never opens a device");
    }
    Fixture f;
    f.device->permission = OfflineCameraDevice::Permission::Pending;
    check(f.camera->start(), "pending permission accepted");
    f.flush();
    check(bool(f.device->permissionReply), "asynchronous permission callback installed");
    const auto oldReply = f.device->permissionReply;
    f.camera->stop();
    const auto count = f.states.size();
    oldReply(true);
    f.flush();
    check(f.device->configured == 0 && f.states.size() == count,
          "late permission cannot reopen stopped camera");
    f.device->permission = OfflineCameraDevice::Permission::Granted;
    check(f.camera->start(), "new permission generation starts");
    f.flush();
    oldReply(true);
    f.flush();
    check(f.device->configured == 1, "old permission cannot configure a newer generation");
    f.camera.reset();
    const auto stoppedCount = f.states.size();
    oldReply(true);
    f.flush();
    check(f.states.size() == stoppedCount, "late permission after owner destruction is silent");
}

void nativePhotos() {
    Fixture f;
    auto* buffer = pixels();
    f.startRunning(buffer);
    if (f.frames.empty()) {
        CVPixelBufferRelease(buffer);
        return;
    }
    check(!f.camera->capturePhoto(0), "zero photo id rejected");
    check(f.camera->capturePhoto(42), "native photo accepted");
    check(!f.camera->capturePhoto(43), "only one photo may be pending");
    f.flush();
    check(f.device->photoRequests == 1 && f.photos.empty(), "preview does not synthesize a photo");
    auto* delegate = photoDelegate(f, 42);
    NSData* jpeg = jpegFixture();
    [delegate recordData:jpeg error:nil];
    f.flush();
    check(f.photos.empty(), "processed photo waits for final capture completion");
    [delegate finishWithError:nil];
    f.flush();
    check(
        f.photos.size() == 1 && f.photos.front().requestId == 42 && f.photos.front().error.empty(),
        "native photo completion preserves request id");
    if (!f.photos.empty()) {
        const auto& actual = f.photos.front().jpeg;
        check(actual.size() == jpeg.length &&
                  std::memcmp(actual.data(), jpeg.bytes, jpeg.length) == 0,
              "JPEG bytes preserved exactly from native photo output");
    }
    check(f.camera->capturePhoto(42), "caller may reuse id after completion");
    f.flush();
    [delegate finishWithError:nil];
    f.flush();
    check(f.photos.size() == 1,
          "duplicate native completion cannot consume next shutter with same id");
    auto* failed = photoDelegate(f, 42);
    [failed recordData:jpeg error:nil];
    NSError* error =
        [NSError errorWithDomain:@"offline-photo"
                            code:1
                        userInfo:@{NSLocalizedDescriptionKey : @"Synthetic final capture failure"}];
    [failed finishWithError:error];
    f.flush();
    check(f.photos.size() == 2 && f.photos.back().jpeg.empty() && !f.photos.back().error.empty(),
          "final capture failure discards already processed JPEG");
    check(f.camera->capturePhoto(44), "next shutter accepted after photo failure");
    f.flush();
    auto* late = photoDelegate(f, 44);
    f.camera->stop();
    [late recordData:jpeg error:nil];
    [late finishWithError:nil];
    f.flush();
    check(f.photos.size() == 2, "stop cancels pending native photo without callback");
    f.device->jpegSupported = false;
    f.startRunning(buffer);
    check(f.camera->capturePhoto(45), "unsupported JPEG request completes asynchronously");
    f.flush();
    check(f.photos.size() == 3 && f.photos.back().jpeg.empty() && !f.photos.back().error.empty(),
          "no native JPEG produces explicit failure without preview fallback");
    f.device->jpegSupported = true;
    for (const bool empty : {true, false}) {
        check(f.camera->capturePhoto(empty ? 46 : 47), "invalid native photo request accepted");
        f.flush();
        auto* invalid = photoDelegate(f, empty ? 46 : 47);
        const std::uint8_t png[] = {0x89, 'P', 'N', 'G'};
        [invalid recordData:empty ? nil : [NSData dataWithBytes:png length:sizeof(png)] error:nil];
        [invalid finishWithError:nil];
        f.flush();
        check(f.photos.back().jpeg.empty() && !f.photos.back().error.empty(),
              "empty or non-JPEG native output reports photo failure");
    }
    check(f.callbacksOnQueue, "photo callbacks use delivery queue");
    CVPixelBufferRelease(buffer);
}

void terminalErrorsAndExceptions() {
    Fixture f;
    auto* buffer = pixels();
    f.startRunning(buffer);
    check(f.camera->capturePhoto(90), "pending photo before interruption accepted");
    f.flush();
    auto* delegate = photoDelegate(f, 90);
    const auto epoch = f.generation();
    f.runtime->fail(epoch, "Synthetic device interruption");
    f.flush();
    const auto frames = f.frames.size();
    check(f.photos.size() == 1 && f.photos.front().jpeg.empty() && !f.photos.front().error.empty(),
          "device interruption fails pending photo once");
    check(f.errors.back() == "Synthetic device interruption",
          "asynchronous error text owns temporary input");
    f.inject(buffer, epoch);
    f.runtime->resumeAuthorization(epoch, true);
    [delegate recordData:jpegFixture() error:nil];
    [delegate finishWithError:nil];
    f.flush();
    check(f.frames.size() == frames && f.states.back() == MaiCameraCapture::State::Error,
          "same-generation late frame cannot revive terminal error");
    check(f.device->configured == 1 && f.photos.size() == 1,
          "terminal error rejects late permission and native photo");
    check(!f.camera->capturePhoto(91), "no shutter in terminal error");
    f.camera->stop();
    f.frameHook = [] { throw std::runtime_error("Synthetic callback failure"); };
    f.startRunning(buffer);
    f.inject(buffer);
    check(f.frames.size() == frames + 2,
          "throwing user callback does not terminate delivery queue");
    CVPixelBufferRelease(buffer);
}

void lateDelegateAndThrowingCallbacks() {
    MaiCameraPhotoDelegate* late = nil;
    std::weak_ptr<MacCameraRuntime> expired;
    {
        Fixture f;
        auto* buffer = pixels();
        f.startRunning(buffer);
        check(f.camera->capturePhoto(100), "destruction fixture shutter accepted");
        f.flush();
        late = photoDelegate(f, 100);
        expired = f.runtime;
        CVPixelBufferRelease(buffer);
    }
    check(expired.expired(), "native photo delegate does not retain destroyed runtime");
    [late recordData:jpegFixture() error:nil];
    [late finishWithError:nil];
    check(expired.expired(), "late native completion cannot resurrect owner");

    auto device = std::make_shared<OfflineCameraDevice>();
    nextOfflineCameraDevice = device;
    unsigned stateCalls = 0, photoCalls = 0;
    MaiCameraCapture::Callbacks callbacks;
    callbacks.state = [&](MaiCameraCapture::State, const std::string&) {
        ++stateCalls;
        throw std::runtime_error("Synthetic state callback failure");
    };
    callbacks.photo = [&](const MaiCameraCapture::Photo&) {
        ++photoCalls;
        throw std::runtime_error("Synthetic photo callback failure");
    };
    MaiCameraCapture camera(std::move(callbacks));
    nextOfflineCameraDevice.reset();
    auto runtime = device->runtime.lock();
    check(camera.start(), "throwing callback fixture starts");
    dispatch_sync(runtime->sessionQueue, ^{
                  });
    auto* buffer = pixels();
    const auto epoch = runtime->generation;
    dispatch_sync(runtime->deliveryQueue, ^{
      runtime->receivePixels(epoch, buffer);
    });
    device->jpegSupported = false;
    check(camera.capturePhoto(101), "throwing photo callback fixture captures");
    dispatch_sync(runtime->sessionQueue, ^{
                  });
    dispatch_sync(runtime->deliveryQueue, ^{
                  });
    camera.stop();
    check(stateCalls == 3 && photoCalls == 1,
          "state/photo callback exceptions stay inside delivery boundary");
    CVPixelBufferRelease(buffer);
}

void stopJoinsInFlightCallback() {
    Fixture f;
    auto* buffer = pixels();
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, release = false;
    std::atomic<bool> stopReturned{false};
    f.frameHook = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
        check(!f.camera->capturePhoto(11), "shutter from in-flight callback rejected during stop");
        check(!f.camera->start(), "start from in-flight callback rejected during stop");
    };
    check(f.camera->start(), "join fixture starts");
    f.flush();
    std::thread input([&] { f.inject(buffer); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
              "callback entered");
    }
    std::thread stop([&] {
        f.camera->stop();
        stopReturned = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool stopping = false;
    while (!stopping && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(f.runtime->mutex);
            stopping = f.runtime->stopping;
        }
        std::this_thread::yield();
    }
    check(stopping && !stopReturned.load(), "stop waits for in-flight user callback");
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    input.join();
    stop.join();
    check(stopReturned.load(), "stop returns after callback exits");
    const auto count = f.frames.size();
    f.inject(buffer);
    f.flush();
    check(f.frames.size() == count, "no callback after joined stop returns");
    CVPixelBufferRelease(buffer);
}

void useWeakNativePhotoOutput(Fixture& fixture, MaiWeakNativePhotoOutput* output) {
    const auto runtime = fixture.runtime;
    dispatch_sync(runtime->sessionQueue, ^{
      runtime->photo = (AVCapturePhotoOutput*)output;
      // 设备启动已由合成输入完成；此后真实 requestNativePhoto 分支仅调用上面的弱替身。
      // 不再经过原来提前 return 的离线快门分支，因此能观测 request 局部变量释放。
      runtime->offline.reset();
    });
}

void weakNativeDelegateSurvivesRequestScope() {
    Fixture f;
    auto* buffer = pixels();
    f.startRunning(buffer);
    MaiWeakNativePhotoOutput* first = [MaiWeakNativePhotoOutput new];
    useWeakNativePhotoOutput(f, first);
    check(f.camera->capturePhoto(201), "weak native photo request accepted");
    f.flush();
    check(first->requests == 1, "real requestNativePhoto submitted to weak native output");
    __weak MaiCameraPhotoDelegate* old = first->submittedDelegate;
    check(old != nil, "native weak delegate survives request scope without test retention");
    if (!old) {
        f.camera->stop();
        CVPixelBufferRelease(buffer);
        return;
    }
    f.camera->stop();
    check(old != nil, "canceled native delegate remains alive until final native callback");
    // 同一对象重启并复用 public ID；旧代 final 只能释放旧 token，不能影响新代。
    const auto runtime = f.runtime;
    const auto device = f.device;
    dispatch_sync(runtime->sessionQueue, ^{
      runtime->offline = device;
    });
    f.startRunning(buffer);
    MaiWeakNativePhotoOutput* second = [MaiWeakNativePhotoOutput new];
    useWeakNativePhotoOutput(f, second);
    check(f.camera->capturePhoto(201), "new generation may reuse canceled photo id");
    f.flush();
    __weak MaiCameraPhotoDelegate* current = second->submittedDelegate;
    check(current != nil, "new native photo delegate is retained independently");
    @autoreleasepool {
        [first->submittedDelegate recordData:jpegFixture() error:nil];
        [first->submittedDelegate finishWithError:nil];
    }
    f.flush();
    check(old == nil, "old native final callback releases old delegate");
    check(current != nil && f.photos.empty(), "old native final preserves new pending photo");
    @autoreleasepool {
        [second->submittedDelegate recordData:jpegFixture() error:nil];
        [second->submittedDelegate finishWithError:nil];
    }
    f.flush();
    check(current == nil, "successful native final callback releases retained delegate");
    check(f.photos.size() == 1 && f.photos.front().requestId == 201 &&
              !f.photos.front().jpeg.empty() && f.photos.front().error.empty(),
          "retained native delegate delivers exactly one JPEG for new generation");
    second->throwOnRequest = true;
    check(f.camera->capturePhoto(202), "throwing native submission is accepted asynchronously");
    f.flush();
    check(second->submittedDelegate == nil && f.photos.size() == 2 &&
              !f.photos.back().error.empty() && f.photos.back().jpeg.empty(),
          "native submission exception releases delegate and reports photo failure");
    CVPixelBufferRelease(buffer);
}

void nativeDelegateDoesNotCycleRuntime() {
    MaiWeakNativePhotoOutput* output = [MaiWeakNativePhotoOutput new];
    std::weak_ptr<MacCameraRuntime> expired;
    __weak MaiCameraPhotoDelegate* delegate;
    {
        Fixture f;
        auto* buffer = pixels();
        f.startRunning(buffer);
        useWeakNativePhotoOutput(f, output);
        check(f.camera->capturePhoto(203), "native owner-destruction request accepted");
        f.flush();
        expired = f.runtime;
        delegate = output->submittedDelegate;
        check(delegate != nil, "native delegate retained before owner destruction");
        CVPixelBufferRelease(buffer);
    }
    check(expired.expired(),
          "native delegate retention has no Runtime cycle after owner destruction");
    check(delegate == nil, "owner destruction releases remaining native photo delegate");
}
}  // namespace

int main() {
    @autoreleasepool {
        framesAndRestart();
        permissionCancellation();
        nativePhotos();
        terminalErrorsAndExceptions();
        lateDelegateAndThrowingCallbacks();
        stopJoinsInFlightCallback();
        weakNativeDelegateSurvivesRequestScope();
        nativeDelegateDoesNotCycleRuntime();
        std::printf("Mac camera offline contracts: %u checks, %u failures; no device, permission "
                    "prompt or Qt\n",
                    checks, failures);
        return failures ? 1 : 0;
    }
}
