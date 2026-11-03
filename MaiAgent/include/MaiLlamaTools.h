// 文件作用：共享的本地 llama.cpp 文本与单图推理，以及复用它的 Agent 工具。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

class MaiTool;
class MaiToolRegistry;

// 一次独立推理；路径必须是已经由宿主授权的本地绝对路径，核心再核对文件类型与可读性。
// Agent 工具会先用 MaiToolContext::resolvePath 处理每条路径；普通调用不经过工具协议。
// modelPath 必填；单图推理须同时提供 projectorPath 与 imagePath。权重和投影文件不自动下载。
// 请求不延续上次 KV、采样器或对话；useChatTemplate 使用模型自带的受支持模板生成回复前缀。
// 首版固定 CPU，确保取消能进入 llama.cpp 的真实 abort callback；不声明 GPU 抢占取消。
// 上限：prompt 64 KiB、contextSize 32768、maximumTokens 4096、threads 64、输出 64 KiB。
// 图片限 20 MiB、单边 8192 像素且不超过 8 MP；宿主须先按显示方向准备图片。
// STB 解码不应用 EXIF 方向，也不修改源文件。视频须先由宿主现有媒体能力抽帧。
// 驻留期间权重文件须保持不变；缓存仅比较规范路径、大小和秒级修改时间，并非内容强身份。
// 同路径同大小的快速替换可能无法识别；替换文件前应先 unload，再提交下一次请求。
struct MaiLlamaRequest {
    MaiFilePath modelPath;
    MaiFilePath projectorPath;
    MaiFilePath imagePath;
    std::string prompt;
    std::uint32_t contextSize = 4096;
    std::uint32_t maximumTokens = 256;
    std::uint32_t threads = 2;
    float temperature = 0.0f;
    std::uint32_t seed = 0;
    std::size_t maximumOutputBytes = 64 * 1024;
    bool useChatTemplate = true;
};

enum class MaiLlamaStopReason { EndOfSequence, TokenLimit, OutputLimit };

// text 是完整 UTF-8；字节上限只在字符边界截断。截断由 stopReason/truncated 如实标明。
// promptTokens 包含实际送入 decoder 的图片 embedding token，generatedTokens 不含 EOG。
// 取消或失败返回 MaiError，不把未完成的输出伪装成成功结果。
struct MaiLlamaResponse {
    std::string text;
    std::uint32_t promptTokens = 0;
    std::uint32_t generatedTokens = 0;
    MaiLlamaStopReason stopReason = MaiLlamaStopReason::TokenLimit;
    bool truncated = false;
};

// 不包含提示词、文件路径或权重内容，可在推理期间快速查询，不等待长时间的推理锁。
// modelGeneration 每次成功载入一个驻留配置后递增；重复调用同一配置不会改变该值。
struct MaiLlamaStatus {
    bool busy = false;
    bool hasModel = false;
    bool hasProjector = false;
    std::uint32_t contextSize = 0;
    std::uint64_t modelGeneration = 0;
};

// 全进程只有 getSharedMaiLlamaRuntime() 创建的一个实例，普通入口与工具必须共用它。
// generate/unload 互斥串行，切换配置先释放 mtmd、llama_context、model，再载入新配置。
// generate 是阻塞调用，只能在允许阻塞的工作线程执行；不在内部新建线程或调用进程 CLI。
// cancel 可为空；非空时由调用方持有到 generate 返回。加载进度、prefill 与逐 token 解码
// 都检查取消，CPU decode 使用真实 abort callback，清理完成后才返回 Canceled。
// vision encode 在前后及后端 eval callback 合作停止，不承诺即时抢占。
// 所有失败使用 MaiResult/MaiError；实现捕获上游异常，不让它穿过宿主 ABI。
// InvalidInput 表示参数、媒体或 token 预算非法；NotFound 表示文件不可用，NotConfigured
// 表示 GGUF/projector/context 无法加载，NotSupported 表示模板不受支持，其余引擎错误为 Internal。
// status 可与 generate/cancel 并发。unload 会等待当前 generate 完成，不取消其他调用者。
class MaiLlamaRuntime {
public:
    ~MaiLlamaRuntime();
    MaiLlamaRuntime(const MaiLlamaRuntime&) = delete;
    MaiLlamaRuntime& operator=(const MaiLlamaRuntime&) = delete;

    MaiResult<MaiLlamaResponse> generate(const MaiLlamaRequest& request,
                                         const std::atomic<bool>* cancel = nullptr);
    MaiLlamaStatus status() const;
    MaiError unload();

private:
    friend std::shared_ptr<MaiLlamaRuntime> getSharedMaiLlamaRuntime();
    MaiLlamaRuntime();
    class Implementation;
    std::unique_ptr<Implementation> mImplementation;
};

// 返回同一个进程驻留实例；调用者释放 shared_ptr 不会绕过其他宿主的使用权。
std::shared_ptr<MaiLlamaRuntime> getSharedMaiLlamaRuntime();

// 工具名为 llama；仅接受 arguments 字符串数组。支持本地模型、prompt、生成长度、上下文、
// CPU 线程、温度、seed、单图/mmproj 及独立 --status/--unload；严格拒绝未知根字段和参数。
// --unload 在观察到 busy 时返回 Busy；空闲时释放驻留实例，不取消任何请求。
// 检查后若并行请求抢先开始，unload 会等其完成，不承诺无等待或抢占。
// 它复用上面的普通 API，不代表已安全嵌入 llama-cli/mtmd-cli 的进程级入口。
std::unique_ptr<MaiTool> makeMaiLlamaTool(std::shared_ptr<MaiLlamaRuntime> runtime = {});
void registerMaiLlamaTools(MaiToolRegistry& registry,
                           std::shared_ptr<MaiLlamaRuntime> runtime = {});
