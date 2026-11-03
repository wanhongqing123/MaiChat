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
