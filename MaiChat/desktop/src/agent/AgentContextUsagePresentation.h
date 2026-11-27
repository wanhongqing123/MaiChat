// 将核心的实际用量快照转换为本地/远端共用的展示内容。
#pragma once

#include <QString>
#include <optional>

#include "MaiModelContextUsage.h"

struct AgentContextUsagePresentation {
    QString text;
    QString detail;
    std::optional<int> usedPercent;
};

// 仅使用实际 usage 或明确标记的算术推导。未知窗口、陈旧测量和缺失总量不显示百分比。
// baselineTokens 只用于显示，不参与发送预算；不接收或回退到 tokenizer 估算。
AgentContextUsagePresentation presentAgentContextUsage(const MaiModelContextUsage& usage,
                                                       std::size_t baselineTokens = 12000);
