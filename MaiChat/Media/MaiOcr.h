// 文件作用：提供与 UI、工具协议无关的本地文字识别入口。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiOcrEngine MaiOcrEngine;

// Normalized recognition tensor dimensions, not source image pixels.
enum { MAI_OCR_RECOGNITION_HEIGHT = 48, MAI_OCR_MAX_RECOGNITION_WIDTH = 2048 };

typedef enum MaiOcrStatus {
    MAI_OCR_OK = 0,
    MAI_OCR_INVALID_INPUT,
    MAI_OCR_NOT_CONFIGURED,
    MAI_OCR_MODEL_ERROR,
    MAI_OCR_RUNTIME_ERROR,
    MAI_OCR_CANCELED,
    MAI_OCR_LIMIT_EXCEEDED,
    MAI_OCR_OUT_OF_MEMORY
} MaiOcrStatus;

// 当前模型契约为 PP-OCRv5 mobile 的 BGR DB 检测及 BGR CTC 识别。
// runtime_path 是宿主已有 ONNX Runtime 1.26 的 UTF-8 绝对路径；运行时仅从该路径加载。
// Windows 依赖 DLL 只在该运行时目录与 System32 查找。
// 两个模型是完整 ONNX 字节，字典每行一个 UTF-8 条目（可含组合字符）；
// 不含 CTC blank，末尾空格由核心补齐。
// 全部配置只借用到 create 返回；成功后模型会话和字典由 engine 独立持有。
typedef struct MaiOcrModelConfig {
    const char* runtime_path;
    const unsigned char* detection_model;
    size_t detection_model_size;
    const unsigned char* recognition_model;
    size_t recognition_model_size;
    const char* dictionary_utf8;
    size_t dictionary_size;
} MaiOcrModelConfig;

// top-down、非预乘 RGBA8。size 至少覆盖 (height-1)*stride+width*4；stride 必须为正。
// alpha 不参与识别，宿主须先合成透明像素。输入最多 16,000,000 像素、每轴 16,384。
// 原始像素仅借用到 recognize 返回，核心不读取文件或改变输入。
typedef struct MaiOcrImage {
    const unsigned char* rgba;
    size_t size;
    int width;
    int height;
    int stride;
} MaiOcrImage;

// 回调在调用线程轮询，非零表示取消，不保存、不异步调用，也不得抛异常。
// ORT 内部单次推理不能保证立即中断，但取消后绝不发布部分结果。
typedef struct MaiOcrOptions {
    int (*is_canceled)(void* context);
    void* context;
} MaiOcrOptions;

typedef struct MaiOcrPoint {
    float x;
    float y;
} MaiOcrPoint;

// quad 按左上、右上、右下、左下排列；均在原始输入像素坐标内。
// confidence 是 CTC 已输出字符概率的均值，范围 [0,1]，不代表文字所属控件可点击。
// 只发布非空且 confidence >= 0.5 的块；空结果也可能是识别置信度不足。
typedef struct MaiOcrRegion {
    char* text;
    float confidence;
    MaiOcrPoint quad[4];
} MaiOcrRegion;

// 成功的空白图仍有原始 width/height，count 为零、regions 为 NULL。
// 返回文本及数组归调用方所有，独立于 engine 生命周期；只用 maiOcrResultFree 释放。
typedef struct MaiOcrResult {
    int width;
    int height;
    MaiOcrRegion* regions;
    size_t count;
    // Counts source lines incompletely recovered after bounded wide-line
    // subdivision. Partial results may have zero regions. Ordinary confidence
    // filtering alone does not indicate a width-recovery failure.
    int partial;
    size_t skipped_lines;
    // Source lines for which at least two fragment recognitions were attempted.
    size_t segmented_lines;
} MaiOcrResult;

// 同步调用。宿主须用同一把 mutex 串行保护同一个 engine 的 recognize 和 destroy；
// 不同 engine 可以并行。创建失败时 *out_engine 为 NULL。error 可为 NULL；非零容量时
// 始终 NUL 结尾，仅包含模型/运行时错误信息，不记录图像和识别文本。
MaiOcrStatus maiOcrCreate(const MaiOcrModelConfig* config, MaiOcrEngine** out_engine, char* error,
                          size_t error_capacity);
void maiOcrDestroy(MaiOcrEngine* engine);

// out_result 必须已清零或已释放。失败/取消保持全零，不发布已完成的前几个文本块。
// 检测按模型 resize-long 960 后向上对齐 128（最长边 1,024），最多 512 个候选、
// 256 个结果；识别高度 48、宽度最多 2,048（归一化尺寸，非源图像素），
// 过宽行最多分为 8 个不重叠片段；无法安全分割时以 partial/skipped_lines 报告。
// 每图最多 512 次识别推理、30 秒识别阶段；单次 ORT 推理不能立即中断。
// 单块 UTF-8 文本最多 8,192 字节，总文本最多 65,536 字节。超过限制明确失败。
MaiOcrStatus maiOcrRecognize(MaiOcrEngine* engine, const MaiOcrImage* image, const MaiOcrOptions* options,
                             MaiOcrResult* out_result, char* error, size_t error_capacity);
void maiOcrResultFree(MaiOcrResult* result);

#ifdef __cplusplus
}
#endif
