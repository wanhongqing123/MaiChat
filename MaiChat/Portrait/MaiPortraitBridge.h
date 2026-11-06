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

// 不透明计划持有复制后的感知遮罩、有效参数和统计；不借用配置中的模型或宿主像素。
// 分析、区域细化、面部保护、顺序处理各 tile、finish、free 必须在同一后台串行任务中完成。
// Protect 内部一次准备共享净肤/LUT 工作场；宿主不能逐 tile 重新估计它。
// v8 美白保留本次平滑/提亮/暖度后的输入色相并限制增色；暗部曲线参数随请求固定，
// 不用图片均值反馈，也不能扩大最终人物/五官保护范围。
typedef struct MaiPortraitSkinContext MaiPortraitSkinContext;
typedef struct MaiPortraitSkinAnalysisResult {
    MaiPortraitSkinContext* context;
    char* metadata;
    char* error;
} MaiPortraitSkinAnalysisResult;
// proxy 为按原图比例解码的全图，最长边 <=512；globalRgba 为原流程的 64x64 全图特征。
// hints 是原图坐标 {_face_hints:[...]}；fingerprint 来自完整原图，不是缩略图。
// metadata 为 {ok,output:{tiles:[{sample_rect,write_rect}],face_roi,face_plan,
// effective_parameters,geometry_parameters,...}}，所有矩形均为原图左上像素。
// whitening_color_policy 是实际颜色规则；whitening_shadow_amount 为 L* 曲线幅度 D，
// 不是每像素最终亮度差。两字段由 finish/commit 透传，不属于模型输入参数。
MaiPortraitSkinAnalysisResult maiPortraitAnalyzeSkinRgba(const unsigned char* proxyRgba, int proxyWidth,
                                                         int proxyHeight, int proxyStride, int sourceWidth,
                                                         int sourceHeight, const char* hints, const char* arguments,
                                                         const char* profileDirectory, const char* sourcePath,
                                                         const char* sourceFingerprint, const unsigned char* globalRgba,
                                                         int globalStride);
// Analyze 给出 skin_refinement_request 时必须提交一次：像素来自其绑定的原图区域，
// 分辨率必须与请求一致（最长边 <=1024）。只能在 Protect 和 tile0 之前调用。
// 不接受调用者坐标、参数或主体替换；失败使计划失效，不发布半成品。
// 返回拥有的 {ok,output:{applied,source_rect,proxy_width,proxy_height}}，用 maiPortraitFree。
char* maiPortraitRefineSkinRegionRgba(MaiPortraitSkinContext* context, const unsigned char* rgba, int width, int height,
                                      int stride);
// 在处理 tile 前提交 face_roi 的原生像素；mesh 仅保护眼眉唇，失败不猜测嘴部位置。
// 面部不可可靠保护时跳过面部颜色，并仅在仍有可靠身体皮肤时继续；返回拥有的 JSON。
// remove_body_hair=true 需要可靠 class2 身体范围；仅脸部计划必须明确失败。
char* maiPortraitProtectSkinFaceRegion(MaiPortraitSkinContext* context, const unsigned char* rgba, int width,
                                       int height, int stride);
// tileIndex 必须从 0 顺序递增；输入严格匹配 sample_rect，输出仍为 sample_rect 大小。
// 宿主只合入 write_rect；halo 不重复计数。失败计划不能 finish 或继续发布。
MaiPortraitRegionResult maiPortraitProcessSkinTile(MaiPortraitSkinContext* context, int tileIndex,
                                                   const unsigned char* rgba, int width, int height, int stride);
// 全部 tile 完成后返回 {ok,output:<兼容 commit 的汇总元数据>}，不登记结果或发布文件。
char* maiPortraitFinishSkin(MaiPortraitSkinContext* context);
void maiPortraitFreeSkinContext(MaiPortraitSkinContext* context);

// 统一校验模型字段；remove_body_hair 仅 beautify 接受严格 bool，省略为 auto。
// effective_parameters 仅返显显式 bool；auto 时省略它，body_cleanup_request_mode 独立说明请求模式。
// 该开关独立于八项训练 values；hostFields=1 时额外接受宿主 operation/_face_hints。
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
