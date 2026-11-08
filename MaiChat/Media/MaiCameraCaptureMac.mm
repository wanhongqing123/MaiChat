// 文件作用：AVFoundation 视频预览和原生 JPEG 快门；不依赖 Qt，也不打开麦克风。
// 生命周期：配置只在 sessionQueue 执行，用户回调只在 deliveryQueue 串行交付。
#include "MaiCameraCapture.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace {
struct MacCameraRuntime;

#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
// 仅离线测试替换设备、授权和原生快门。帧转换、状态闸门和回调交付仍用正式实现。
struct OfflineCameraDevice {
    enum class Permission { Granted, Pending, Denied, Restricted };
    Permission permission = Permission::Granted;
    bool jpegSupported = true;
    unsigned configured = 0;
    unsigned stopped = 0;
    unsigned photoRequests = 0;
    std::function<void(bool)> permissionReply;
    std::weak_ptr<MacCameraRuntime> runtime;
};
std::shared_ptr<OfflineCameraDevice> nextOfflineCameraDevice;
#endif

const void* sessionQueueKey = &sessionQueueKey;
const void* deliveryQueueKey = &deliveryQueueKey;

std::string errorText(NSError* error, const char* fallback) {
    const char* detail = error.localizedDescription.UTF8String;
    return detail && *detail ? std::string(detail) : std::string(fallback);
}

bool copyFrame(CVPixelBufferRef pixels, MaiCameraCapture::Frame& output) {
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
        CVPixelBufferIsPlanar(pixels))
        return false;
    const size_t width = CVPixelBufferGetWidth(pixels);
    const size_t height = CVPixelBufferGetHeight(pixels);
    const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    const size_t maximum = std::numeric_limits<std::uint32_t>::max();
    if (!width || !height || width > maximum / 4 || height > maximum || stride < width * 4 ||
        height > std::numeric_limits<size_t>::max() / stride ||
        CVPixelBufferGetDataSize(pixels) < height * stride ||
        CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return false;
    struct Unlock {
        CVPixelBufferRef pixels;
        ~Unlock() {
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        }
    } unlock{pixels};
    const auto* bytes = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
    if (!bytes) return false;
    output.width = static_cast<std::uint32_t>(width);
    output.height = static_cast<std::uint32_t>(height);
    output.stride = static_cast<std::uint32_t>(width * 4);
    // 不把原生行尾 padding 交给宿主；它不属于图像，也不保证已初始化。
    output.pixels.resize(size_t(output.stride) * height);
    for (size_t y = 0; y < height; ++y) {
        auto* row = output.pixels.data() + y * output.stride;
        std::memcpy(row, bytes + y * stride, output.stride);
        for (size_t x = 0; x < width; ++x) row[x * 4 + 3] = 255;
    }
    return true;
}
}  // namespace

@interface MaiCameraVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    std::weak_ptr<MacCameraRuntime> owner;
    std::uint64_t generation;
}
@end

@interface MaiCameraPhotoDelegate : NSObject <AVCapturePhotoCaptureDelegate> {
@public
    std::weak_ptr<MacCameraRuntime> owner;
    std::uint64_t generation;
    std::uint64_t token;
    MaiCameraCapture::Photo result;
    std::mutex resultMutex;
}
- (void)recordData:(NSData*)data error:(NSError*)error;
- (void)finishWithError:(NSError*)error;
- (void)discardProcessedData;
@end

namespace {
struct MacCameraRuntime : std::enable_shared_from_this<MacCameraRuntime> {
    explicit MacCameraRuntime(MaiCameraCapture::Callbacks supplied)
        : callbacks(std::move(supplied)),
          sessionQueue(dispatch_queue_create("maichat.camera.session", DISPATCH_QUEUE_SERIAL)),
          deliveryQueue(dispatch_queue_create("maichat.camera.delivery", DISPATCH_QUEUE_SERIAL)) {
        dispatch_queue_set_specific(sessionQueue, sessionQueueKey, this, nullptr);
        dispatch_queue_set_specific(deliveryQueue, deliveryQueueKey, this, nullptr);
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
        offline = nextOfflineCameraDevice;
#endif
    }

    MaiCameraCapture::Callbacks callbacks;
    dispatch_queue_t sessionQueue;
    dispatch_queue_t deliveryQueue;
    std::mutex mutex;
    std::condition_variable stoppedCondition;
    MaiCameraCapture::State state = MaiCameraCapture::State::Stopped;
    bool stopping = false;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t pendingId = 0;
    std::uint64_t pendingToken = 0;
    std::uint64_t lastToken = 0;
    // 以下原生对象只在 sessionQueue 配置或释放，delegate 仅弱引用 Runtime。
    AVCaptureSession* session = nil;
    AVCaptureDevice* device = nil;
    AVCaptureVideoDataOutput* video = nil;
    AVCapturePhotoOutput* photo = nil;
    MaiCameraVideoDelegate* videoDelegate = nil;
    // PhotoOutput 弱持有 delegate；本请求表持有到最终回调，delegate 反向只弱引用 Runtime。
    // stop 后仍可能有系统 final 回流，旧 token 独立释放；Runtime 销毁时一并释放剩余项。
    NSMutableDictionary<NSNumber*, MaiCameraPhotoDelegate*>* photoDelegates = nil;
    NSMutableArray<id>* observers = nil;
    std::uint64_t nativeGeneration = 0;
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
    std::shared_ptr<OfflineCameraDevice> offline;
#endif

    // 调用者持有 mutex。同代 Error 也必须拒绝帧、授权及快门，不能被迟到事件复活。
    bool acceptsLocked(std::uint64_t expected) const {
        return generation == expected && !stopping &&
               (state == MaiCameraCapture::State::Starting ||
                state == MaiCameraCapture::State::Running);
    }
    bool accepts(std::uint64_t expected) {
        std::lock_guard<std::mutex> lock(mutex);
        return acceptsLocked(expected);
    }
    void emitState(MaiCameraCapture::State value, const std::string& error = {}) {
        try {
            if (callbacks.state) callbacks.state(value, error);
        } catch (...) {
        }
    }
    void emitPhoto(const MaiCameraCapture::Photo& value) {
        try {
            if (callbacks.photo) callbacks.photo(value);
        } catch (...) {
        }
    }

    bool start() {
        std::uint64_t current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || state != MaiCameraCapture::State::Stopped) return false;
            state = MaiCameraCapture::State::Starting;
            current = ++generation;
            sequence = 0;
        }
        const auto self = shared_from_this();
        dispatch_async(deliveryQueue, ^{
          if (self->accepts(current)) self->emitState(MaiCameraCapture::State::Starting);
        });
        dispatch_async(sessionQueue, ^{
          self->beginAuthorization(current);
        });
        return true;
    }

    void resumeAuthorization(std::uint64_t current, bool allowed) {
        const auto self = shared_from_this();
        dispatch_async(sessionQueue, ^{
          if (!self->accepts(current)) return;
          if (allowed)
              self->configure(current);
          else
              self->fail(current, "Camera permission was denied");
        });
    }

    void beginAuthorization(std::uint64_t current) {
        if (!accepts(current)) return;
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
        if (offline) {
            if (offline->permission == OfflineCameraDevice::Permission::Pending) {
                const std::weak_ptr<MacCameraRuntime> weak = shared_from_this();
                offline->permissionReply = [weak, current](bool granted) {
                    if (auto self = weak.lock()) self->resumeAuthorization(current, granted);
                };
            } else if (offline->permission == OfflineCameraDevice::Permission::Granted) {
                configure(current);
            } else {
                fail(current, "Camera permission is denied or restricted");
            }
            return;
        }
#endif
        @try {
            // 缺用途说明时直接失败，不能让系统因权限配置错误终止宿主进程。
            id description =
                [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSCameraUsageDescription"];
            if (![description isKindOfClass:NSString.class] || ![description length]) {
                fail(current, "Camera usage description is missing");
                return;
            }
            const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            if (status == AVAuthorizationStatusAuthorized) {
                configure(current);
            } else if (status == AVAuthorizationStatusNotDetermined) {
                const std::weak_ptr<MacCameraRuntime> weak = shared_from_this();
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                         completionHandler:^(BOOL granted) {
                                           if (auto self = weak.lock())
                                               self->resumeAuthorization(current, granted);
                                         }];
            } else {
                fail(current, "Camera permission is denied or restricted");
            }
        } @catch (NSException*) {
            fail(current, "Camera authorization failed");
        }
    }

    static void configureConnection(AVCaptureConnection* connection) {
        if (connection.isVideoMirroringSupported) {
            connection.automaticallyAdjustsVideoMirroring = NO;
            connection.videoMirrored = NO;
        }
        if ([connection isVideoRotationAngleSupported:0]) connection.videoRotationAngle = 0;
    }

    void configure(std::uint64_t current) {
        if (!accepts(current)) return;
        nativeGeneration = current;
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
        if (offline) {
            ++offline->configured;
            return;
        }
#endif
        @try {
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
            NSError* inputError = nil;
            AVCaptureDeviceInput* input =
                device ? [AVCaptureDeviceInput deviceInputWithDevice:device error:&inputError]
                       : nil;
            if (!input) {
                fail(current, errorText(inputError, "No camera device is available"));
                return;
            }
            session = [AVCaptureSession new];
            video = [AVCaptureVideoDataOutput new];
            photo = [AVCapturePhotoOutput new];
            video.alwaysDiscardsLateVideoFrames = YES;
            video.videoSettings =
                @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
            videoDelegate = [MaiCameraVideoDelegate new];
            videoDelegate->owner = shared_from_this();
            videoDelegate->generation = current;
            [video setSampleBufferDelegate:videoDelegate queue:deliveryQueue];
            [session beginConfiguration];
            bool configured = false;
            @try {
                if ([session canAddInput:input]) {
                    [session addInput:input];
                    if ([session canAddOutput:video]) {
                        [session addOutput:video];
                        if ([session canAddOutput:photo]) {
                            [session addOutput:photo];
                            configured = true;
                        }
                    }
                }
                if ([session canSetSessionPreset:AVCaptureSessionPresetPhoto])
                    session.sessionPreset = AVCaptureSessionPresetPhoto;
                else if ([session canSetSessionPreset:AVCaptureSessionPresetHigh])
                    session.sessionPreset = AVCaptureSessionPresetHigh;
                configureConnection([video connectionWithMediaType:AVMediaTypeVideo]);
                configureConnection([photo connectionWithMediaType:AVMediaTypeVideo]);
            } @finally {
                [session commitConfiguration];
            }
            if (!configured) {
                fail(current, "Camera does not support preview and native photos");
                return;
            }
            observers = [NSMutableArray new];
            const std::weak_ptr<MacCameraRuntime> weak = shared_from_this();
            for (NSNotificationName name in @[
                     AVCaptureSessionRuntimeErrorNotification,
                     AVCaptureSessionWasInterruptedNotification,
                     AVCaptureSessionDidStopRunningNotification
                 ]) {
                id observer = [NSNotificationCenter.defaultCenter
                    addObserverForName:name
                                object:session
                                 queue:nil
                            usingBlock:^(NSNotification*) {
                              if (auto self = weak.lock())
                                  self->fail(current, "Camera session stopped or was interrupted");
                            }];
                [observers addObject:observer];
            }
            [observers
                addObject:[NSNotificationCenter.defaultCenter
                              addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                                          object:device
                                           queue:nil
                                      usingBlock:^(NSNotification*) {
                                        if (auto self = weak.lock())
                                            self->fail(current, "Camera device was disconnected");
                                      }]];
            if (!accepts(current)) {
                closeNative(current);
                return;
            }
            [session startRunning];
            if (!session.isRunning) fail(current, "Camera could not start");
        } @catch (NSException*) {
            fail(current, "Camera configuration failed");
        }
    }

    void receivePixels(std::uint64_t current, CVPixelBufferRef pixels) {
        if (!accepts(current)) return;
        MaiCameraCapture::Frame frame;
        try {
            if (!copyFrame(pixels, frame)) return;
        } catch (...) {
            fail(current, "Camera preview could not be copied");
            return;
        }
        bool first;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!acceptsLocked(current)) return;
            first = state != MaiCameraCapture::State::Running;
            state = MaiCameraCapture::State::Running;
            frame.sequence = ++sequence;
        }
        if (first && accepts(current)) emitState(MaiCameraCapture::State::Running);
        if (accepts(current)) {
            try {
                if (callbacks.frame) callbacks.frame(frame);
            } catch (...) {
            }
        }
    }

    bool capturePhoto(std::uint64_t requestId) {
        std::uint64_t current, token;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!requestId || stopping || state != MaiCameraCapture::State::Running || pendingId)
                return false;
            pendingId = requestId;
            pendingToken = token = ++lastToken;
            current = generation;
        }
        const auto self = shared_from_this();
        dispatch_async(sessionQueue, ^{
          self->requestNativePhoto(current, token, requestId);
        });
        return true;
    }

    void requestNativePhoto(std::uint64_t current, std::uint64_t token, std::uint64_t requestId) {
        if (!accepts(current)) return;
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
        if (offline) {
            ++offline->photoRequests;
            if (!offline->jpegSupported)
                finishPhoto(current, token,
                            {requestId, {}, "Native JPEG photo capture is unavailable"});
            return;
        }
#endif
        @try {
            if (!photo || ![photo.availablePhotoCodecTypes containsObject:AVVideoCodecTypeJPEG]) {
                finishPhoto(current, token,
                            {requestId, {}, "Native JPEG photo capture is unavailable"});
                return;
            }
            MaiCameraPhotoDelegate* delegate = [MaiCameraPhotoDelegate new];
            delegate->owner = shared_from_this();
            delegate->generation = current;
            delegate->token = token;
            delegate->result.requestId = requestId;
            if (!photoDelegates) photoDelegates = [NSMutableDictionary new];
            photoDelegates[@(token)] = delegate;
            if (!accepts(current)) {
                releaseNativePhoto(current, token);
                return;
            }
            // 每次使用新的 settings。调用方显式持有 delegate，不能依赖 PhotoOutput 代持。
            AVCapturePhotoSettings* settings = [AVCapturePhotoSettings
                photoSettingsWithFormat:@{AVVideoCodecKey : AVVideoCodecTypeJPEG}];
            [photo capturePhotoWithSettings:settings delegate:delegate];
        } @catch (NSException*) {
            releaseNativePhoto(current, token);
            finishPhoto(current, token, {requestId, {}, "Native photo capture failed"});
        }
    }

    void releaseNativePhoto(std::uint64_t current, std::uint64_t token) {
        MaiCameraPhotoDelegate* retained = photoDelegates[@(token)];
        if (retained && retained->generation == current && retained->token == token)
            [photoDelegates removeObjectForKey:@(token)];
    }

    void finishPhoto(std::uint64_t current, std::uint64_t token, MaiCameraCapture::Photo value) {
        const auto self = shared_from_this();
        // 即使请求已被 stop 失效，最终回流仍须释放同代 token 的 delegate。
        // 请求表只在 sessionQueue 访问，不在任意系统回调线程上改动所有权。
        dispatch_async(sessionQueue, ^{
          self->releaseNativePhoto(current, token);
        });
        dispatch_async(deliveryQueue, ^{
          {
              std::lock_guard<std::mutex> lock(self->mutex);
              // token 区分同一 requestId 的多次快门，重复/迟到结果不能完成下一次请求。
              if (!self->acceptsLocked(current) || self->pendingId != value.requestId ||
                  self->pendingToken != token)
                  return;
              self->pendingId = self->pendingToken = 0;
          }
          self->emitPhoto(value);
        });
    }

    void fail(std::uint64_t current, const std::string& error) {
        // GCD block 捕获 C++ 引用参数时不会延长被引用临时字符串的生命周期。
        // 独立拥有错误正文，避免异步回调只收到空内容或悬空数据。
        const std::string message = error;
        MaiCameraCapture::Photo pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!acceptsLocked(current)) return;
            state = MaiCameraCapture::State::Error;
            pending.requestId = pendingId;
            pending.error = message;
            pendingId = pendingToken = 0;
        }
        const auto self = shared_from_this();
        dispatch_async(deliveryQueue, ^{
          {
              std::lock_guard<std::mutex> lock(self->mutex);
              if (self->generation != current || self->state != MaiCameraCapture::State::Error ||
                  self->stopping)
                  return;
          }
          if (pending.requestId) self->emitPhoto(pending);
          if (self->acceptsError(current)) self->emitState(MaiCameraCapture::State::Error, message);
        });
        dispatch_async(sessionQueue, ^{
          self->closeNative(current);
        });
    }

    bool acceptsError(std::uint64_t current) {
        std::lock_guard<std::mutex> lock(mutex);
        return generation == current && state == MaiCameraCapture::State::Error && !stopping;
    }

    void closeNative(std::uint64_t current) {
        if (!current || nativeGeneration != current) return;
        nativeGeneration = 0;
        // 取消后只保留等待 final 的轻量 delegate，不滞留已处理的大张 JPEG。
        // 不创建 allValues 的 autoreleased 强引用快照，否则 final 释放后仍可能滞留 delegate。
        for (NSNumber* key in photoDelegates) {
            MaiCameraPhotoDelegate* delegate = photoDelegates[key];
            if (delegate->generation == current) [delegate discardProcessedData];
        }
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
        if (offline) {
            ++offline->stopped;
            return;
        }
#endif
        for (id observer in observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
        observers = nil;
        @try {
            [video setSampleBufferDelegate:nil queue:nullptr];
            if (session.isRunning) [session stopRunning];
        } @catch (NSException*) {
            // 发布闸门已经关闭；拆掉 owned session，不能因系统清理异常再次发布成功。
        }
        videoDelegate = nil;
        video = nil;
        photo = nil;
        session = nil;
        device = nil;
    }

    void stop() {
        bool changed;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping) {
                stoppedCondition.wait(lock, [this] { return !stopping; });
                return;
            }
            changed = state != MaiCameraCapture::State::Stopped;
            if (!changed) return;
            stopping = true;
            ++generation;
            state = MaiCameraCapture::State::Stopped;
            pendingId = pendingToken = 0;
        }
        // 不持 mutex 等待交付：用户正在执行的回调可调用 capturePhoto/start，并应立即被拒绝。
        dispatch_sync(sessionQueue, ^{
          closeNative(nativeGeneration);
        });
        dispatch_sync(deliveryQueue, ^{
          if (changed) emitState(MaiCameraCapture::State::Stopped);
        });
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = false;
        }
        stoppedCondition.notify_all();
    }
};
}  // namespace

@implementation MaiCameraVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    @autoreleasepool {
        if (auto runtime = owner.lock())
            runtime->receivePixels(generation, CMSampleBufferGetImageBuffer(sample));
    }
}
@end

@implementation MaiCameraPhotoDelegate
- (void)recordData:(NSData*)data error:(NSError*)error {
    std::lock_guard<std::mutex> lock(resultMutex);
    const auto runtime = owner.lock();
    if (!runtime || !runtime->accepts(generation)) return;
    try {
        if (error)
            result.error = errorText(error, "Native photo processing failed");
        else if (data.length < 4 || static_cast<const std::uint8_t*>(data.bytes)[0] != 0xff ||
                 static_cast<const std::uint8_t*>(data.bytes)[1] != 0xd8)
            result.error = "Native photo output did not provide JPEG data";
        else {
            const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
            result.jpeg.assign(bytes, bytes + data.length);
        }
    } catch (...) {
        result.error = "Native photo data could not be copied";
    }
    if (!result.error.empty()) result.jpeg.clear();
}
- (void)finishWithError:(NSError*)error {
    // sessionQueue 可以立即移除最后一个请求表引用；保持 self 活到本次系统回调返回。
    __attribute__((objc_precise_lifetime)) MaiCameraPhotoDelegate* keepAlive = self;
    (void)keepAlive;
    MaiCameraCapture::Photo completed;
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (error) result.error = errorText(error, "Native photo capture failed");
        if (result.jpeg.empty() && result.error.empty())
            result.error = "Native photo output was empty";
        if (!result.error.empty()) result.jpeg.clear();
        completed = std::move(result);
    }
    if (auto runtime = owner.lock()) runtime->finishPhoto(generation, token, std::move(completed));
}
- (void)discardProcessedData {
    std::lock_guard<std::mutex> lock(resultMutex);
    std::vector<std::uint8_t>().swap(result.jpeg);
}
- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishProcessingPhoto:(AVCapturePhoto*)photo
                       error:(NSError*)error {
    (void)output;
    @autoreleasepool {
        [self recordData:error ? nil : photo.fileDataRepresentation error:error];
    }
}
- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)settings
                                  error:(NSError*)error {
    (void)output;
    (void)settings;
    @autoreleasepool {
        [self finishWithError:error];
    }
}
@end

struct MaiCameraCapture::Runtime final : MacCameraRuntime {
    explicit Runtime(Callbacks callbacks) : MacCameraRuntime(std::move(callbacks)) {}
};

MaiCameraCapture::MaiCameraCapture(Callbacks callbacks)
    : mRuntime(std::make_shared<Runtime>(std::move(callbacks))) {
#if defined(MAI_CAMERA_CAPTURE_OFFLINE_TEST)
    if (mRuntime->offline) mRuntime->offline->runtime = mRuntime;
#endif
}
MaiCameraCapture::~MaiCameraCapture() {
    stop();
}
bool MaiCameraCapture::isAvailable() {
    @autoreleasepool {
        @try {
            return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo] != nil;
        } @catch (NSException*) {
            return false;
        }
    }
}
bool MaiCameraCapture::start() {
    return mRuntime->start();
}
bool MaiCameraCapture::capturePhoto(std::uint64_t requestId) {
    return mRuntime->capturePhoto(requestId);
}
void MaiCameraCapture::stop() {
    mRuntime->stop();
}
