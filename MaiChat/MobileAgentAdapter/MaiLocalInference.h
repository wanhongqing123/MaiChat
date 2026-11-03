// 文件作用：向移动宿主提供共享本地推理核心的普通 C 入口，不经过 Agent 工具协议。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 是无效 ID。ID 在进程内不复用，防止已销毁 job 的迟到取消影响另一请求。
typedef uint64_t MaiLocalInferenceJob;

typedef enum MaiLocalInferenceStatus {
    MAI_LOCAL_INFERENCE_OK = 0,
    MAI_LOCAL_INFERENCE_INVALID_INPUT = 1,
    MAI_LOCAL_INFERENCE_NOT_FOUND = 2,
    MAI_LOCAL_INFERENCE_BUSY = 3,
    MAI_LOCAL_INFERENCE_NOT_CONFIGURED = 4,
    MAI_LOCAL_INFERENCE_CANCELED = 5,
    MAI_LOCAL_INFERENCE_NOT_SUPPORTED = 6,
    MAI_LOCAL_INFERENCE_INTERNAL = 7
} MaiLocalInferenceStatus;

// bytes 借用到 generate 返回；不要求 NUL 结尾，但拒绝内嵌 NUL 与无效 UTF-8。
// 空的可选字段可使用 {NULL,0}。路径最多 16 KiB，prompt 最多 64 KiB。
typedef struct MaiLocalInferenceUtf8 {
    const char* bytes;
    size_t size;
} MaiLocalInferenceUtf8;

// 路径必须是宿主已授权的本地绝对路径；本接口不授予沙盒访问权、不自动下载权重。
// model_path 与 prompt 必填；projector_path/image_path 同时提供或都为空。
// image_path 是已按显示方向规范化的本地静态位图；本接口不宣称自动处理 HEIC/EXIF 方向。
// 首版仅 CPU。所有数值必须显式设置，可用 request_defaults 得到与共享 runtime 相同默认值。
typedef struct MaiLocalInferenceRequest {
    MaiLocalInferenceUtf8 model_path;
    MaiLocalInferenceUtf8 projector_path;
    MaiLocalInferenceUtf8 image_path;
    MaiLocalInferenceUtf8 prompt;
    uint32_t context_size;
    uint32_t maximum_tokens;
    uint32_t threads;
    float temperature;
    uint32_t seed;
    size_t maximum_output_bytes;
    int use_chat_template;
} MaiLocalInferenceRequest;

typedef enum MaiLocalInferenceStopReason {
    MAI_LOCAL_INFERENCE_END_OF_SEQUENCE = 0,
    MAI_LOCAL_INFERENCE_TOKEN_LIMIT = 1,
    MAI_LOCAL_INFERENCE_OUTPUT_LIMIT = 2
} MaiLocalInferenceStopReason;

// text_utf8 与 error_utf8 由本接口分配，用 result_free 释放（包括失败响应）。
// 成功 text 是完整 UTF-8，长度为 text_size；失败不返回部分生成文本。
// error_utf8 是固定安全说明，不包含路径、提示词或上游日志。OOM 时可为空。
typedef struct MaiLocalInferenceResult {
    MaiLocalInferenceStatus status;
    char* text_utf8;
    size_t text_size;
    uint32_t prompt_tokens;
    uint32_t generated_tokens;
    MaiLocalInferenceStopReason stop_reason;
    int truncated;
    char* error_utf8;
} MaiLocalInferenceResult;

// 不包含路径/提示词；status 失败时其他字段为零。可在 generate 运行时并发快速查询。
typedef struct MaiLocalInferenceRuntimeStatus {
    MaiLocalInferenceStatus status;
    int busy;
    int has_model;
    int has_projector;
    uint32_t context_size;
    uint64_t model_generation;
} MaiLocalInferenceRuntimeStatus;

MaiLocalInferenceRuntimeStatus mai_local_inference_status(void);

// 同步阻塞，只能在工作队列调用。等待共享 runtime 正在执行的 generate 返回，再释放
// projector/context/model；不取消其他调用者的 job。调用者若要中止自己的生成，必须先
// cancel 自己的 job 并等 generate 返回。重复 unload 幂等；model_generation 不倒退或伪增。
MaiLocalInferenceStatus mai_local_inference_unload(void);

// 最多同时持有 64 个尚未 destroy 的 job；达到上限或分配失败返回 0。
MaiLocalInferenceJob mai_local_inference_job_create(void);

// 线程安全，取消为 sticky，可在 generate 开始前调用；ID 已销毁/无效返回 0。
// 不等待解码，不杀线程；共享 runtime 以 CPU abort callback 合作式结束生成。
int mai_local_inference_job_cancel(MaiLocalInferenceJob job);

// 线程安全，立即撤销 ID 并请求取消。已经进入 generate 的调用持有内部状态直至返回，
// 故 cancel 标志不会悬空；后续对此 ID 的调用均无效，重复 destroy 返回 0。
// 本函数不等待解码完成，也不释放其他宿主驻留的模型。宿主仍须等待自己的 generate 返回。
int mai_local_inference_job_destroy(MaiLocalInferenceJob job);

MaiLocalInferenceRequest mai_local_inference_request_defaults(void);

// 同步阻塞，只能在后台工作队列执行。普通入口直接调用同一个全进程共享 runtime。
// 每个 job 仅接受一次 generate（包括失败调用）；并发/重复调用返回 busy，不重跑模型。
// 请求结构体及所有借用 bytes 必须活到返回。运行中 destroy/cancel 安全；结果仍由调用方释放。
// 异常不穿过 C ABI；失败不写文件、不返回伪造结果。无模型时返回明确错误且宿主继续存活。
MaiLocalInferenceResult mai_local_inference_generate(MaiLocalInferenceJob job, const MaiLocalInferenceRequest* request);
void mai_local_inference_result_free(MaiLocalInferenceResult* result);

#ifdef __cplusplus
}
#endif
