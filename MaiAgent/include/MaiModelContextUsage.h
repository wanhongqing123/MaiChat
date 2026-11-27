// 文件作用：提供真实模型计量的普通值快照，供实时状态和重开会话共用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "MaiModelClient.h"

class MaiSessionStore;

// 缺失和实测零值不同；Derived 仅为已知计数的算术结果，不是 tokenizer 估计。
struct MaiTokenUsageCount {
    std::size_t tokens = 0;
    MaiUsageValueSource source = MaiUsageValueSource::Missing;
};

struct MaiTokenUsageSnapshot {
    MaiTokenUsageCount input;
    MaiTokenUsageCount cachedInput;
    MaiTokenUsageCount cacheWriteInput;
    MaiTokenUsageCount output;
    MaiTokenUsageCount reasoningOutput;
    MaiTokenUsageCount total;
};

enum class MaiContextWindowSource { Missing, Provider, Configuration };

// 可复制、无资源的普通值。latestSampling 的身份/窗口仅属于最近主采样，
// 摘要仅更新 turn/thread；累计包含摘要和重试，不可作为上下文窗口占用。
// provider 为不透明、无凭据的配置来源指纹；未知来源或窗口变更使 samplingValid=false。
// thread 是有记录以来的累计，不能据此断言首条测量之前的历史费用已知。
struct MaiModelContextUsage {
    MaiTokenUsageSnapshot latestSampling;
    MaiTokenUsageSnapshot turn;
    MaiTokenUsageSnapshot thread;
    std::string latestOperation;
    std::string model;
    std::string provider;
    std::string turnId;
    std::string responseId;
    std::string windowId;
    std::uint64_t windowNumber = 0;
    std::uint64_t checkpointRevision = 0;
    std::size_t windowTokens = 0;
    MaiContextWindowSource windowSource = MaiContextWindowSource::Missing;
    bool samplingValid = false;
};

// 纯算术，无 IO，可并发调用。缓存/推理均为 input/output 的明细，不重复计入总数。
// 输入/输出缺失或加法溢出返回 Missing；缺失缓存只在算术中按零降级，
// 原 cachedInput.source 仍为 Missing，结果标记 Derived；缓存大于输入时钳制为零。
MaiTokenUsageSnapshot maiTokenUsageSnapshot(const MaiModelUsage& usage);
MaiTokenUsageSnapshot maiAccumulateTokenUsage(const MaiTokenUsageSnapshot& previous,
                                              const MaiTokenUsageSnapshot& current);
MaiTokenUsageCount maiNonCachedInput(const MaiTokenUsageSnapshot& usage);
MaiTokenUsageCount maiBlendedTotal(const MaiTokenUsageSnapshot& usage);

// 仅供显示的 Codex 基线由跨端共用；移动宿主不得另造默认模型窗口或基线。
inline constexpr std::size_t kMaiContextUsageDisplayBaselineTokens = 12000;

// 仅供显示的 Codex 口径：使用主采样 total，缓存仍占窗口；baseline 使用共用默认值。
// 未知窗口、缺失计量或失效来源返回 nullopt；已知窗口 <= baseline 返回 0。
// 不参与压缩预算，不使用 before/after 估计，也不对累计费用应用此比例。
std::optional<int> maiContextRemainingPercent(const MaiModelContextUsage& usage,
                                              std::size_t baselineTokens = kMaiContextUsageDisplayBaselineTokens);

// 从同一权威 JSONL 最近记录读取；可与写入并发，由调用方在普通工作线程同步调用。
// 返回值归调用方。无记录成功返回缺失快照；不支持的后端/源损坏保留明确错误。
// expectedModel/provider 任一未知、来源不符、检查点已变化则保留累计并作废窗口计量。
MaiResult<MaiModelContextUsage> maiReadModelContextUsage(const MaiSessionStore& store,
                                                         const std::string& sessionId,
                                                         const std::string& expectedModel,
                                                         const std::string& expectedProvider);
