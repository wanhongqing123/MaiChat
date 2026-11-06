#include "ui/MaiMacVideoPlayback.h"
#include "MaiCloudVod.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#include <QFileInfo>
#include <QGuiApplication>
#include <QWidget>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

// AVFoundation 只请求字节范围；TLS、代理、签名续期、账号隔离和有限缓存仍由
// 正常云盘播放器共用的 C++ VOD 内核处理。取消不会留下下载文件。
@interface MaiMacCloudVideoLoader : NSObject <AVAssetResourceLoaderDelegate> {
   @public
    std::shared_ptr<MaiCloudVod> source;
    std::atomic_bool stopped;
    NSString* contentType;
    dispatch_queue_t queue;
}
- (void)stop;
@end
@implementation MaiMacCloudVideoLoader
- (instancetype)init {
    if ((self = [super init])) {
        stopped.store(false);
        queue = dispatch_queue_create("com.maichat.cloud-video-reader", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (void)stop {
    stopped.store(true);
    if (source)
        mai_cloud_vod_interrupt_read(source.get());
}
- (BOOL)resourceLoader:(AVAssetResourceLoader*)loader
    shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest*)request {
    (void)loader;
    [self retain];
    @autoreleasepool {
        const uint64_t length = source ? mai_cloud_vod_size(source.get()) : 0;
        if (!length || stopped.load() || request.cancelled) {
            [self release];
            return NO;
        }
        AVAssetResourceLoadingContentInformationRequest* info = request.contentInformationRequest;
        if (info) {
            info.contentType = contentType;
            info.contentLength = static_cast<long long>(length);
            info.byteRangeAccessSupported = YES;
        }
        AVAssetResourceLoadingDataRequest* data = request.dataRequest;
        bool success = true;
        if (data) {
            const int64_t requested = data.requestedOffset;
            int64_t position = std::max<int64_t>(requested, data.currentOffset);
            if (requested < 0 || position < 0 || uint64_t(position) > length || data.requestedLength < 0) {
                success = false;
            } else {
                const uint64_t end =
                    data.requestsAllDataToEndOfResource
                        ? length
                        : uint64_t(requested) + std::min<uint64_t>(length - uint64_t(requested), data.requestedLength);
                std::array<uint8_t, 64 * 1024> bytes{};
                while (uint64_t(position) < end && !stopped.load() && !request.cancelled) {
                    size_t count = 0;
                    MaiCloudVodResult result{};
                    const size_t wanted = size_t(std::min<uint64_t>(bytes.size(), end - uint64_t(position)));
                    if (!mai_cloud_vod_read_at(source.get(), uint64_t(position), bytes.data(), wanted, &count,
                                               &result) ||
                        !count || count > wanted) {
                        success = false;
                        break;
                    }
                    @autoreleasepool {
                        [data respondWithData:[NSData dataWithBytes:bytes.data() length:count]];
                    }
                    position += int64_t(count);
                }
            }
        }
        if (!request.cancelled) {
            if (success && !stopped.load())
                [request finishLoading];
            else
                [request finishLoadingWithError:[NSError errorWithDomain:NSURLErrorDomain
                                                                    code:stopped.load() ? NSURLErrorCancelled
                                                                                        : NSURLErrorCannotOpenFile
                                                                userInfo:nil]];
        }
    }
    [self release];
    return YES;
}
- (void)resourceLoader:(AVAssetResourceLoader*)loader didCancelLoadingRequest:(AVAssetResourceLoadingRequest*)request {
    (void)loader;
    (void)request;
    if (source)
        mai_cloud_vod_interrupt_read(source.get());
}
- (void)dealloc {
    [self stop];
    [contentType release];
    dispatch_release(queue);
    [super dealloc];
}
@end

@interface MaiMacVideoState : NSObject {
   @public
    AVPlayer* player;
    AVPlayerLayer* layer;
    MaiMacCloudVideoLoader* loader;
    id endObserver;
    BOOL closed, wantsPlaying, seeking, finished;
    int64_t seekTarget;
    uint64_t seekGeneration;
    double speed;
}
- (void)close;
- (void)seek:(int64_t)position;
@end
@implementation MaiMacVideoState
- (void)close {
    if (closed)
        return;
    closed = YES;
    ++seekGeneration;
    [loader stop];
    if (endObserver) {
        [NSNotificationCenter.defaultCenter removeObserver:endObserver];
        [endObserver release];
        endObserver = nil;
    }
    [player pause];
    [player.currentItem cancelPendingSeeks];
    layer.player = nil;
    [layer removeFromSuperlayer];
    [player replaceCurrentItemWithPlayerItem:nil];
}
- (void)seek:(int64_t)position {
    seeking = YES;
    seekTarget = position;
    finished = NO;
    const uint64_t generation = ++seekGeneration;
    [player.currentItem cancelPendingSeeks];
    [player seekToTime:CMTimeMake(position, 1000000)
          toleranceBefore:kCMTimeZero
           toleranceAfter:kCMTimeZero
        completionHandler:^(BOOL completed) {
          dispatch_async(dispatch_get_main_queue(), ^{
            if (closed || generation != seekGeneration)
                return;
            seeking = NO;
            if (completed && wantsPlaying)
                [player playImmediatelyAtRate:float(speed)];
            else if (!wantsPlaying)
                [player pause];
          });
        }];
}
- (void)dealloc {
    [self close];
    [loader release];
    [layer release];
    [player release];
    [super dealloc];
}
@end

struct MaiMacVideoPlayback::Impl {
    MaiMacVideoState* state = nil;
    QWidget* surface = nullptr;
};

static int64_t mediaMicroseconds(CMTime value) {
    const double seconds = CMTimeGetSeconds(value);
    if (!CMTIME_IS_NUMERIC(value) || !std::isfinite(seconds) || seconds < 0 ||
        seconds > double(std::numeric_limits<int64_t>::max()) / 1000000)
        return 0;
    return int64_t(std::llround(seconds * 1000000));
}

MaiMacVideoPlayback::MaiMacVideoPlayback(QWidget* surface,
                                         const QString& path,
                                         std::shared_ptr<MaiCloudVod> cloudSource)
    : impl_(new Impl) {
    if (!surface || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    impl_->surface = surface;
    auto* state = [[MaiMacVideoState alloc] init];
    impl_->state = state;
    NSURL* url = nil;
    if (cloudSource) {
        state->loader = [[MaiMacCloudVideoLoader alloc] init];
        state->loader->source = std::move(cloudSource);
        const bool mov = QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0;
        state->loader->contentType = [(mov ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4) retain];
        url = [NSURL URLWithString:mov ? @"maicloud-vod://asset/source.mov" : @"maicloud-vod://asset/source.mp4"];
    } else
        url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.toUtf8().constData()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    if (state->loader)
        [asset.resourceLoader setDelegate:state->loader queue:state->loader->queue];
    AVPlayerItem* item = [AVPlayerItem playerItemWithAsset:asset];
    // 原始 Dolby Vision RPU 交给公开的系统显示映射；不是固定地压低亮度。
    if (@available(macOS 11.0, *))
        item.appliesPerFrameHDRDisplayMetadata = YES;
    item.preferredForwardBufferDuration = 3;
    item.audioTimePitchAlgorithm = AVAudioTimePitchAlgorithmSpectral;
    state->player = [[AVPlayer playerWithPlayerItem:item] retain];
    state->layer = [[AVPlayerLayer playerLayerWithPlayer:state->player] retain];
    state->layer.videoGravity = AVLayerVideoGravityResizeAspect;
    NSView* view = reinterpret_cast<NSView*>(surface->winId());
    view.wantsLayer = YES;
    [view.layer addSublayer:state->layer];
    state->endObserver =
        [[NSNotificationCenter.defaultCenter addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                                         object:item
                                                          queue:NSOperationQueue.mainQueue
                                                     usingBlock:^(NSNotification*) {
                                                       if (!state->closed) {
                                                           state->finished = YES;
                                                           state->wantsPlaying = NO;
                                                       }
                                                     }] retain];
    resize();
}

MaiMacVideoPlayback::~MaiMacVideoPlayback() {
    [impl_->state close];
    [impl_->state release];
}
bool MaiMacVideoPlayback::open(int64_t positionUs, double speed, bool playing) {
    auto* state = impl_->state;
    if (!state || state->closed)
        return false;
    state->speed = speed;
    state->wantsPlaying = playing;
    if (positionUs < 0)
        return false;
    if (positionUs > 0 || !playing)
        [state seek:positionUs];
    else if (playing)
        [state->player playImmediatelyAtRate:float(speed)];
    return true;
}
bool MaiMacVideoPlayback::ready() const {
    return impl_->state && impl_->state->layer.readyForDisplay;
}
bool MaiMacVideoPlayback::failed() const {
    return !impl_->state || impl_->state->player.currentItem.status == AVPlayerItemStatusFailed;
}
bool MaiMacVideoPlayback::status(MaiFfplayPlaybackStatus* output) const {
    auto* state = impl_->state;
    if (!output || !state || state->closed || failed())
        return false;
    *output = {};
    output->running = 1;
    output->position_us = mediaMicroseconds(state->player.currentTime);
    output->duration_us = mediaMicroseconds(state->player.currentItem.duration);
    output->paused = !state->wantsPlaying;
    output->speed_percent = int(std::llround(state->speed * 100));
    output->seeking = state->seeking;
    output->seek_target_us = state->seekTarget;
    return true;
}
bool MaiMacVideoPlayback::seek(int64_t positionUs) {
    MaiFfplayPlaybackStatus current{};
    if (!status(&current) || current.duration_us <= 0 || positionUs < 0 || positionUs > current.duration_us)
        return false;
    [impl_->state seek:positionUs];
    return true;
}
bool MaiMacVideoPlayback::setSpeed(double speed) {
    if (!impl_->state || impl_->state->closed)
        return false;
    impl_->state->speed = speed;
    if (impl_->state->wantsPlaying && !impl_->state->seeking)
        [impl_->state->player playImmediatelyAtRate:float(speed)];
    return true;
}
bool MaiMacVideoPlayback::setVolumePercent(int volume) {
    if (!impl_->state || impl_->state->closed || volume < 0 || volume > 100)
        return false;
    impl_->state->player.volume = float(volume) / 100.f;
    return true;
}
bool MaiMacVideoPlayback::setMuted(bool muted) {
    if (!impl_->state || impl_->state->closed)
        return false;
    impl_->state->player.muted = muted;
    return true;
}
bool MaiMacVideoPlayback::audioStatus(int* volume, bool* muted) const {
    if (!impl_->state || impl_->state->closed || !volume || !muted)
        return false;
    *volume = int(std::lround(impl_->state->player.volume * 100));
    *muted = impl_->state->player.muted;
    return true;
}
QSize MaiMacVideoPlayback::videoSize() const {
    if (!impl_->state || impl_->state->closed)
        return {};
    const CGSize size = impl_->state->player.currentItem.presentationSize;
    if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0 ||
        size.width > std::numeric_limits<int>::max() || size.height > std::numeric_limits<int>::max())
        return {};
    return QSize(qRound(size.width), qRound(size.height));
}
bool MaiMacVideoPlayback::command(const QString& command) {
    auto* state = impl_->state;
    if (!state || state->closed)
        return false;
    if (command == QStringLiteral("pause")) {
        state->wantsPlaying = !state->wantsPlaying;
        if (state->wantsPlaying && state->finished)
            [state seek:0];
        else if (state->wantsPlaying && !state->seeking)
            [state->player playImmediatelyAtRate:float(state->speed)];
        else
            [state->player pause];
        return true;
    }
    if (command == QStringLiteral("mute")) {
        state->player.muted = !state->player.muted;
        return true;
    }
    if (command == QStringLiteral("volume_up") || command == QStringLiteral("volume_down")) {
        state->player.volume =
            std::clamp(state->player.volume + (command.endsWith(QStringLiteral("up")) ? .1f : -.1f), 0.f, 1.f);
        return true;
    }
    if (command == QStringLiteral("step")) {
        if (!state->player.currentItem.canStepForward)
            return false;
        state->wantsPlaying = NO;
        [state->player pause];
        [state->player.currentItem stepByCount:1];
        return true;
    }
    int64_t offset = command == QStringLiteral("seek_forward")           ? 10000000
                     : command == QStringLiteral("seek_backward")        ? -10000000
                     : command == QStringLiteral("seek_minute_forward")  ? 60000000
                     : command == QStringLiteral("seek_minute_backward") ? -60000000
                                                                         : 0;
    MaiFfplayPlaybackStatus current{};
    return offset && status(&current) && current.duration_us > 0 &&
           seek(std::clamp((current.seeking ? current.seek_target_us : current.position_us) + offset, int64_t{0},
                           current.duration_us));
}
void MaiMacVideoPlayback::resize() {
    if (!impl_->state || !impl_->surface)
        return;
    NSView* view = reinterpret_cast<NSView*>(impl_->surface->winId());
    impl_->state->layer.frame = view.bounds;
    impl_->state->layer.contentsScale = view.window ? view.window.backingScaleFactor : 1;
}
