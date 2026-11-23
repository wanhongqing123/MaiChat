// 文件作用：定义上下文整理的进度与容量事实，不携带摘要、原文或供应商原始响应。
#pragma once

#include <cstddef>
#include <cstdint>

#include "MaiError.h"
#include "MaiModelTokenEstimate.h"

enum class MaiContextPhase { Preparing, Summarizing, Ready, Deferred, Failed, Interrupted };

const char* maiContextPhaseToString(MaiContextPhase phase);

// 普通值快照，回调接收后可复制给宿主。窗口为 0 表示未知，不能显示为占用 0%。
// before/after 与容量判断同口径（含保守校正），不是累计消费；exact=false 不能冒称精确余量。
// 原文保护仅限预算内的历史用户文本，非授权提升；未保留项仍可按原史回查。
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
};
