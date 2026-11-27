// 文件作用：定义上下文整理的进度与容量事实，不携带摘要、原文或供应商原始响应。
#pragma once

#include <cstddef>
#include <cstdint>

#include "MaiError.h"
#include "MaiModelContextUsage.h"
#include "MaiModelTokenEstimate.h"

enum class MaiContextPhase { Preparing, Summarizing, Ready, Deferred, Failed, Interrupted };

const char* maiContextPhaseToString(MaiContextPhase phase);

// 普通值快照，回调接收后可复制给宿主。窗口为 0 表示未知，不能显示为占用 0%。
// before/after 仅供容量保护计量，可能使用已实测前缀加新增尾部估计，不是状态栏实际占用。
// usage 独立保存最近主请求和累计的真实计量；缺失/来源失效不能用 before/after 补成百分比。
// 正文保护遵循压缩调用方策略；产品模式保留用户与助手正文，仅缩工具详情和合法旧媒体。
struct MaiContextStatus {
    MaiContextPhase phase = MaiContextPhase::Preparing;
    MaiModelTokenEstimate before;
    MaiModelTokenEstimate after;
    std::size_t windowTokens = 0;
    std::size_t inputBudgetTokens = 0;
    std::uint64_t checkpointRevision = 0;
    std::size_t originalUserMessages = 0;
    std::size_t retainedUserMessages = 0;
    bool reusedCheckpoint = false;
    bool advancedCheckpoint = false;
    bool historyIndexRecovery = false;
    MaiError error;
    // 发送前请求体适配（供应商/网关的 HTTP 请求体字节上限，独立于 token 窗口）。
    // 只描述本次发送的请求副本；原始消息、图片与工具输出均未改动，可按原史回查。
    bool requestBodyReduced = false;
    std::size_t requestBodyBytes = 0;       // 实际发送的编码字节；0 未知
    std::size_t requestBodyLimitBytes = 0;  // 已学到的上限；0 未知
    std::size_t requestBodyOmittedImages = 0;
    std::size_t requestBodyTruncatedTexts = 0;
    // 本轮供应商 usage 实际返回的输出 token 累计；0 表示未知或尚未上报。
    // 仅来自真实 usage，不用估算补齐；与 before/after 的输入口径无关。
    std::size_t outputTokens = 0;
    // 与估计 before/after 分开：最近主采样及轮次/线程累计均来自真实 usage。
    // 追加布局后所有消费者须重新编译；不能与旧静态库的值对象混用。
    MaiModelContextUsage usage;
};
