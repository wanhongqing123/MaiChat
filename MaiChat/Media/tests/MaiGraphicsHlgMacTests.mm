// 验证真实 Presenter 的 HLG 图层、AMVE 和最终 GPU 像素；不依赖录屏权限或用户账号。
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "MaiGraphicsPresenter.h"

// 只记录本测试视图自己绘制的 drawable，不读取其他应用或系统屏幕。
@interface MaiHlgCaptureLayer : CAMetalLayer
@property(atomic, retain) id<CAMetalDrawable> capturedDrawable;
@end
@implementation MaiHlgCaptureLayer
- (id<CAMetalDrawable>)nextDrawable {
    id<CAMetalDrawable> drawable = [super nextDrawable];
    self.capturedDrawable = drawable;
    return drawable;
}
@end

static std::atomic<unsigned> presented{0};
static std::atomic<bool> failed{false};
static std::atomic<bool> captureCommands{false};
static std::mutex commandMutex;
static std::vector<id<MTLCommandBuffer>> submittedCommands;
static IMP originalMakeBuffer = nullptr;

// 只在本测试进程观察 Metal 命令完成；读取自有 drawable，不读取系统屏幕。
static id<MTLCommandBuffer> observeBuffer(id queue, SEL selector) {
    using MakeBuffer = id<MTLCommandBuffer> (*)(id, SEL);
    id<MTLCommandBuffer> buffer = reinterpret_cast<MakeBuffer>(originalMakeBuffer)(queue, selector);
    if (buffer && captureCommands) {
        std::lock_guard<std::mutex> lock(commandMutex);
        submittedCommands.push_back([buffer retain]);
    }
    return buffer;
}

static bool observeQueue(id<MTLDevice> device) {
    id<MTLCommandQueue> queue = [device newCommandQueue];
    Class type = object_getClass(queue);
    SEL selector = @selector(commandBuffer);
    Method method = class_getInstanceMethod(type, selector);
    if (!queue || !method)
        return false;
    originalMakeBuffer = method_getImplementation(method);
    if (!class_addMethod(type, selector, reinterpret_cast<IMP>(observeBuffer),
                         method_getTypeEncoding(method)))
        class_replaceMethod(type, selector, reinterpret_cast<IMP>(observeBuffer),
                            method_getTypeEncoding(method));
    [queue release];
    captureCommands = true;
    return true;
}

static void onPresent(uint64_t, bool success, void *) {
    if (success)
        ++presented;
    else
        failed = true;
}

static bool waitForFrame(unsigned previous) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:8];
    while (presented <= previous && !failed && [deadline timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    return presented > previous && !failed;
}

static bool readCenter(MaiHlgCaptureLayer *layer, float (&rgb)[3]) {
    // macOS 的 CVDisplayLink 晚于渲染回调交换 drawable；切换 HDR/SDR 时等待新格式。
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:5];
    while ((!layer.capturedDrawable ||
            layer.capturedDrawable.texture.pixelFormat != layer.pixelFormat) &&
           [deadline timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    id<CAMetalDrawable> drawable = layer.capturedDrawable;
    if (!drawable || drawable.texture.pixelFormat != layer.pixelFormat)
        return false;
    // 回调只表示已提交；等待生产者的 GPU 命令完成，再用另一条队列读回实际像素。
    captureCommands = false;
    std::vector<id<MTLCommandBuffer>> pending;
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        pending.swap(submittedCommands);
    }
    if (pending.empty())
        return false;
    bool rendered = true;
    for (id<MTLCommandBuffer> commands : pending) {
        [commands waitUntilCompleted];
        rendered = rendered && commands.status == MTLCommandBufferStatusCompleted;
        [commands release];
    }
    if (!rendered)
        return false;
    id<MTLCommandQueue> queue = [layer.device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBuffer> bytes = [layer.device newBufferWithLength:256
                                                    options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> copy = [commands blitCommandEncoder];
    [copy copyFromTexture:drawable.texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(drawable.texture.width / 2,
                                               drawable.texture.height / 2, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:bytes
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256];
    [copy endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    const bool ok = commands.status == MTLCommandBufferStatusCompleted;
    if (ok && drawable.texture.pixelFormat == MTLPixelFormatRGBA16Float) {
        const auto *pixels = static_cast<const __fp16 *>(bytes.contents);
        for (int channel = 0; channel < 3; ++channel)
            rgb[channel] = pixels[channel];
    } else if (ok && drawable.texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB) {
        const auto *pixels = static_cast<const uint8_t *>(bytes.contents);
        rgb[0] = pixels[2] / 255.0f;
        rgb[1] = pixels[1] / 255.0f;
        rgb[2] = pixels[0] / 255.0f;
    }
    [bytes release];
    [queue release];
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 64, 64)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
        MaiHlgCaptureLayer *layer = [MaiHlgCaptureLayer layer];
        view.wantsLayer = YES;
        view.layer = layer;
        layer.frame = view.bounds;
        window.contentView = view;
        [window orderFrontRegardless];
        if (!maiGraphicsPresenterStart(argv[1], argv[2], onPresent, nullptr))
            return 3;
        const uint64_t id =
            maiGraphicsPresenterAttach((__bridge void *)layer, 64, 64, nullptr, nullptr);
        NSDate *ready = [NSDate dateWithTimeIntervalSinceNow:5];
        while (!layer.device && [ready timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        if (!layer.device || !observeQueue(layer.device))
            return 5;
        // ITU-R BT.2100 的中性 HLG 0.75，以有限范围 P010 表达；AMVE 来自相机公开格式。
        const uint8_t ambient[] = {0x00, 0x2f, 0xe9, 0xa0, 0x3d, 0x13, 0x40, 0x42};
        uint16_t y[] = {721 << 6, 721 << 6, 721 << 6, 721 << 6};
        uint16_t uv[] = {512 << 6, 512 << 6};
        MaiVideoFrame frame{};
        frame.data[0] = reinterpret_cast<const uint8_t *>(y);
        frame.data[1] = reinterpret_cast<const uint8_t *>(uv);
        frame.linesize[0] = frame.linesize[1] = 4;
        frame.width = frame.height = 2;
        frame.format = MAI_VIDEO_PIXEL_P010;
        frame.color_space = MAI_VIDEO_COLOR_BT2020;
        frame.color_range = MAI_VIDEO_RANGE_LIMITED;
        frame.color_transfer = MAI_VIDEO_TRANSFER_HLG;
        bool ok = id && maiGraphicsPresenterSetHlgOutput(id, true, ambient, sizeof(ambient)) &&
                  maiGraphicsPresenterShowVideoFrame(id, &frame, false) && waitForFrame(0);
        const bool hlgLayer = layer.pixelFormat == MTLPixelFormatRGBA16Float &&
                              layer.wantsExtendedDynamicRangeContent && layer.EDRMetadata != nil;
        CGColorSpaceRef expectedSpace =
            CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
        const bool gamut =
            layer.colorspace && expectedSpace &&
            CFEqual(CGColorSpaceGetName(layer.colorspace), CGColorSpaceGetName(expectedSpace));
        if (expectedSpace)
            CGColorSpaceRelease(expectedSpace);
        float rgb[3] = {};
        const bool read = ok && readCenter(layer, rgb);
        // Apple HLG 线性图层要求逆 OETF，0.75 对应约 3.17955；固定 SDR 映射会压到 1 以下。
        const double expected = std::exp((0.75 - 0.55991073) / 0.17883277) + 0.28466892;
        bool pixels = read;
        for (float channel : rgb)
            pixels = pixels && std::abs(channel - expected) < 0.12;
        std::printf("hlg_layer=%d bt2020=%d gpu_read=%d rgb=%.6f,%.6f,%.6f expected=%.6f\n",
                    hlgLayer, gamut, read, rgb[0], rgb[1], rgb[2], expected);
        std::printf("presented=%u failed=%d drawable=%d\n", presented.load(), failed.load(),
                    layer.capturedDrawable != nil);
        ok = ok && hlgLayer && gamut && pixels;
        if (ok) {
            captureCommands = true;
            const unsigned previous = presented;
            const uint8_t green[] = {0, 255, 0, 255, 0, 255, 0, 255,
                                     0, 255, 0, 255, 0, 255, 0, 255};
            ok = maiGraphicsPresenterSetHlgOutput(id, false, nullptr, 0) &&
                 maiGraphicsPresenterShowFrame(id, green, 2, 2, 8, false) && waitForFrame(previous);
            float sdr[3] = {};
            ok = ok && layer.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB &&
                 !layer.wantsExtendedDynamicRangeContent && !layer.EDRMetadata &&
                 readCenter(layer, sdr) && sdr[0] < 0.01 && sdr[1] > 0.99 && sdr[2] < 0.01;
            std::printf("sdr_reset=%d rgb=%.6f,%.6f,%.6f\n", ok, sdr[0], sdr[1], sdr[2]);
        }
        maiGraphicsPresenterDetach(id);
        maiGraphicsPresenterStop();
        [window close];
        return ok ? 0 : 4;
    }
}
