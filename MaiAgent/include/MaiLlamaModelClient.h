// 文件作用：将已授权的本地 GGUF 作为 MaiAgent 主模型，复用与远端相同的消息和工具回路。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "MaiFilePath.h"
#include "MaiLlamaModelValidation.h"
#include "MaiModelClient.h"

class MaiLlamaRuntime;

// 路径由宿主授权并保持有效；不下载权重、不读取账号密钥，也不回退到远端模型。
// 支持文本对话和模型原生工具模板；projectorPath/图片输入明确返回 NotSupported。
// Apple 默认优先已编入且支持量化矩阵/注意力关键算子的 Metal；能力不足时使用纯 CPU。
// 不能因设备存在或只卸载少量简单层而进入混合后端；其他平台当前使用 CPU。
// GPU 只在解码批次前后、同步与请求清理边界合作取消，不承诺中途抢占 GPU kernel。
// 仅接受 validateMaiLlamaModelConfig 核准的 Qwen2.5/Qwen3.5 原始工具模板，其他明确拒绝。
// 本地固定关闭模板思考模式，远端 reasoningEffort 暂不映射到本地；仍独立解析模型思考片段。
// 完整 system/历史及全部可用工具的原始描述、参数声明一起按模型模板渲染。
// 每次 stream 只调用一次本地推理，不预选工具、不隐藏声明；最终调用仍交现有 Agent 审批/执行。
// 任一真实推理的 token 数超出 contextSize 时返回 InvalidInput，不静默裁掉历史或工具。
// 模型须自带聊天模板，工具请求还须模板支持工具协议。
// 单个请求最多 2 MiB 文本、4096 条消息和 512 个工具；不支持的输入不会被静默省略。
// maximumOutputBytes 限制每次实际推理的原始生成字节，包含思考和工具块；截断不能交付工具调用。
// 整个 stream 的耗时和已交付文本对应这一次真实推理，不另计声明选择阶段。
struct MaiLlamaModelConfig {
    MaiFilePath modelPath;
    MaiFilePath projectorPath;
    std::uint32_t contextSize = 8192;
    // 0 每次推理使用完整渲染后的上下文余量，生成到 EOG；非 0 供普通代码显式预算/测试。
    std::uint32_t maximumTokens = 0;
    std::uint32_t threads = 2;
    float temperature = 0.0f;
    std::uint32_t seed = 0;
    // 内部原始输出内存边界；触及时报错，不静默截断正文或执行半批工具。
    std::size_t maximumOutputBytes = 2 * 1024 * 1024;
    // false 明确使用 CPU，便于受限环境或可复现诊断；不影响独立 generate 的固定 CPU 契约。
    bool preferHardwareAcceleration = true;
};

// 构造不做 I/O；stream 在调用线程阻塞执行，增量回调也在该线程同步触发。
// 默认与普通本地推理/llama 工具共用唯一驻留实例；请求结束清除 KV，不跨账号保存对话。
// 主模型只产生工具调用，由现有 MaiTurnRunner 执行、审批和回灌；客户端不自行执行工具。
// cancel 返回 Canceled，已交付文本仍有效；解析失败、未完成工具块和取消均不交付工具。
std::unique_ptr<MaiModelClient> makeMaiLlamaModelClient(
    MaiLlamaModelConfig config, std::shared_ptr<MaiLlamaRuntime> runtime = {});
