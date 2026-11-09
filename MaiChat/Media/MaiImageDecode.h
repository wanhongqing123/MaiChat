// 文件作用：声明把图片文件解码为供移动端或桌面端绘制的像素缓冲所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 用 FFmpeg 解码第一帧，输出紧密排列的 RGBA8。
// 正数上限在保持宽高比的同时约束输出尺寸；零保留源尺寸。
// rgba 由调用方持有，并用 maiImageDecodeFree 释放。
// icc_profile 非空时指向同一块 rgba 内存，只在该次释放前有效。
// color_primaries 与 color_transfer 使用 FFmpeg 对应枚举的整数值；UNSPECIFIED 为 2。
// 此函数不创建图形表面，也不触碰 UI 线程。
typedef struct MaiImageDecodeResult {
    unsigned char* rgba;
    int width;
    int height;
    int stride;
    int error_code;
    const unsigned char* icc_profile;
    int icc_size;
    int color_primaries;
    int color_transfer;
} MaiImageDecodeResult;

MaiImageDecodeResult maiImageDecodeFile(const char* path, int max_width, int max_height);
void maiImageDecodeFree(void* pixels);

// 严格位图入口按内容识别，不依据扩展名，也不接受视频、播放列表、SVG 或网络 URL。
// HEIF/AVIF 只接受带 primary item 的静态图片；动图只读取完整的第一帧。
// 旧 maiImageDecodeFile 保留视频封面用途；已有 result 布局和释放方式不变。
typedef enum MaiImageFormat {
    MAI_IMAGE_FORMAT_UNKNOWN = 0,
    MAI_IMAGE_FORMAT_PNG,
    MAI_IMAGE_FORMAT_JPEG,
    MAI_IMAGE_FORMAT_WEBP,
    MAI_IMAGE_FORMAT_BMP,
    MAI_IMAGE_FORMAT_GIF,
    MAI_IMAGE_FORMAT_TIFF,
    MAI_IMAGE_FORMAT_ICO,
    MAI_IMAGE_FORMAT_JPEG2000,
    MAI_IMAGE_FORMAT_PNM,
    MAI_IMAGE_FORMAT_HEIF,
    MAI_IMAGE_FORMAT_AVIF
} MaiImageFormat;

// 全零或空指针选用默认策略：编码输入 128 MiB、源图 64,000,000 像素，轴长无额外限制，
// 输出不放大、不缩小。显式限制可放宽默认值（如头像 100MP），硬上限为编码输入 512 MiB、
// 源图 128,000,000 像素；RGBA 输出始终最多 64,000,000 像素。超出硬上限或负轴长参数失败。
// 正数 max_source_width/height 限制变换前源尺寸；正数 max_output_width/height 限制变换后
// 显示尺寸，保持比例且不放大。输入大小、源尺寸和解码器 max_pixels 在像素分配之前检查；
// 缩略图输出限制不能代替源图限制。0 表示上述默认值，不能用于关闭硬上限。
// 所有接口同步、可重入，不派发宿主异步任务，返回后无晚到回调。解码器可能使用内部线程，
// 但 is_canceled 非空时只在调用线程轮询，非零表示取消；不会在 native worker 调用户回调。
// 回调及 context 只须在本次同步调用栈存活，不保留、不异步调用，也不在回调内递归读取。
typedef struct MaiImageReadOptions {
    uint64_t max_encoded_bytes;
    uint64_t max_source_pixels;
    int max_source_width;
    int max_source_height;
    int max_output_width;
    int max_output_height;
    int (*is_canceled)(void* context);
    void* context;
} MaiImageReadOptions;

typedef struct MaiImageInfo {
    MaiImageFormat format;
    int width;
    int height;
    int display_width;
    int display_height;
    int orientation;  // EXIF 1..8，包括镜像；返回的 RGBA 已应用该变换。
    int error_code;   // 0 成功；失败为负 FFmpeg/errno 值，其他字段清零。
} MaiImageInfo;

// Inspect 只检查内容类型、尺寸和策略，成功不代表压缩像素完整有效。Read 成功才交付完整
// 首帧；截断或损坏不得交付局部像素。ICC/色彩元数据随 RGBA 保留，不声称已完成 ICC 转换。
// 文件路径是本地 UTF-8 路径，拒绝协议 URL；文件和内存共用有界、可 seek 的输入适配器。
// 内存借用只在本次调用有效；调用方不得并发修改。Read 的可空 out_info 来自同一次输入
// 快照，避免为呈现元数据另开一次文件；失败时与 result 返回相同错误码。
MaiImageInfo maiImageInspectFile(const char* path, const MaiImageReadOptions* options);
MaiImageInfo maiImageInspectMemory(const unsigned char* bytes, size_t size, const MaiImageReadOptions* options);
MaiImageDecodeResult maiImageReadFile(const char* path, const MaiImageReadOptions* options, MaiImageInfo* out_info);
MaiImageDecodeResult maiImageReadMemory(const unsigned char* bytes, size_t size, const MaiImageReadOptions* options,
                                        MaiImageInfo* out_info);

#ifdef __cplusplus
}
#endif
