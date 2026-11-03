// 文件作用：为 Swift、JNI 提供共用照片修饰及个人训练的薄 C ABI。
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// 配置只复制路径和借用 apiBase；模型延迟到后台首次处理时加载。
// 所有调用内部串行化；不得从 UI 线程处理像素或训练。运行库必须由宿主打包。
void maiPortraitConfigure(const char* modelDirectory, const char* runtimePath, const void* apiBase);
typedef struct MaiPortraitImageResult {
    unsigned char* rgba;
    int width;
    int height;
    char* metadata;
    char* error;
} MaiPortraitImageResult;
typedef struct MaiPortraitRegionResult {
    unsigned char* rgba;
    unsigned char* mask;
    int width;
    int height;
    char* metadata;
    char* error;
} MaiPortraitRegionResult;

// 统一校验模型字段；hostFields=1 时额外接受宿主 operation/_face_hints。
// 合法时返回 NULL，失败返回不含路径的 JSON {code,stage,message,hint}，用 maiPortraitFree 释放。
char* maiPortraitValidateArguments(const char* operation, const char* arguments, int hostFields);
// 宿主全图/窗口检测结果合并，返回 {ok,output:{faces:[...]}}，坐标与输入一致。
char* maiPortraitMergeFaceHints(const char* hints);
// 以原图坐标规划有界 ROI。hints 为宿主检测的 {_face_hints:[...]}；proxy 是 <=256 的全图 RGBA。
// 返回 {ok,output|error}，output 是可交给 region 处理的上下文；宿主须补入 source_fingerprint。
char* maiPortraitPlanFaceRegion(int sourceWidth, int sourceHeight, const char* hints, int subjectIndex,
                                const unsigned char* proxyRgba, int proxyWidth, int proxyHeight, int proxyStride);
// ROI 像素为原生尺寸，不是缩小后的原图；context 来自 plan，不得由模型构造。
// rgba 已完成软边合成，应原位替换同尺寸 ROI；mask 是皮肤编辑 alpha 的诊断输出，不能再混一次。
// mask 为 width*height 灰度8位，metadata 中的坐标均为原图左上像素。每个非NULL指针单独 free。
MaiPortraitRegionResult maiPortraitProcessRegionRgba(const unsigned char* rgba, int width, int height, int stride,
                                                     const char* arguments, const char* profileDirectory,
                                                     const char* sourcePath, const char* regionContext);
// 只发布同目录已经编码、关闭并由宿主校验的完整文件，不替换已有目标。
// 失败仍由调用者清理临时文件；返回 {ok,output|error}，不包含输入路径。
char* maiPortraitPublishOutput(const char* temporaryPath, const char* outputPath);
// profileDirectory 和 sourcePath 由宿主当前账号提供，不接受模型指定。
// arguments 包含业务参数及宿主附加的人脸定位提示。像素为已转正 sRGB RGBA8。
// 成功时返回新像素与待登记元数据；宿主完整保存输出后必须调用 commit，
// 失败不得登记训练样本。rgba、metadata、error 均用 maiPortraitFree 单独释放。
MaiPortraitImageResult maiPortraitProcessRgba(const unsigned char* rgba, int width, int height, int stride,
                                              const char* arguments, const char* profileDirectory,
                                              const char* sourcePath);
// 返回拥有的 UTF-8 JSON {ok,output|error}。commit 输出必须是当前账号已经完整保存的文件。
char* maiPortraitCommitResult(const char* profileDirectory, const char* metadata, const char* outputPath);
// 将过去生成的结果追溯到原图；普通图片返回原路径。返回字符串用 maiPortraitFree 释放。
char* maiPortraitOriginalSource(const char* profileDirectory, const char* path);
// 接受 record_feedback、train、status、rollback 操作；训练有界同步执行。
// 少量反馈仅生成候选模型；响应明确给出是否启用和验证状态，不承诺泛化效果。
char* maiPortraitPersonalizationRequest(const char* profileDirectory, const char* arguments);
void maiPortraitFree(void* pointer);
#ifdef __cplusplus
}
#endif
