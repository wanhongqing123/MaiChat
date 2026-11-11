// 文件作用：按实际模型窗口增量总结已完成的旧轮次，复用持久检查点并保留原始历史。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "MaiContextBuilder.h"
#include "MaiContextCheckpoint.h"
#include "MaiError.h"
#include "MaiModelClient.h"
#include "MaiSessionStore.h"

struct MaiContextPreparation {
    MaiModelRequest request;
    bool reusedCheckpoint = false;
    bool advancedCheckpoint = false;
    // 明确标为估计/精确计数，不是供应商 usage。包含摘要、原文、基础指令和工具声明。
    MaiModelTokenEstimate inputEstimate;
    // 仅摘要 Protocol 失败且原请求仍适配时延后压缩；request 保持原文/旧检查点，未推进存储。
    bool deferredCompaction = false;
    // 本次请求含宿主生成的部分来源索引（新建或复用），不是模型已成功理解的语义摘要。
    // 调用方须保证 request 的真实工具能力可精确读取本会话原史；不会删除或改写原消息。
    bool usedHistoryIndexRecovery = false;
};

// 普通 C++ 摘要入口，直接调用同一个供应商客户端，不依赖工具协议或服务端代理。
// 实例无共享可变状态，可由不同会话工作线程并发使用；模型/存储须履行其并发契约。
// prepare 在调用线程同步推理，不能在事件/流式回调中调用。调用方保持依赖和源快照有效。
// 先总结尚未覆盖的完整旧轮次。单轮工具结果过长时可总结 assistant 的稳定工具前缀，
// 当前 user、未覆盖的原始观察及 Pending/Running tail 保持原文。工具调用与回执成对封存；
// 大工具组可整体摘要，像素仍保留并带真实组来源，不能声称知道不存在的单图调用归属。
// 超长旧片段分成带精确源 ID/字段/偏移的数据块，全部成功后才提交一个原子检查点。
// 失败、取消、空/无效摘要、源变化或持久化失败不推进；原始消息永不改写或删除。
// 软区摘要 Protocol 失败可返回仍适配的原请求并标记 deferredCompaction。
// 硬区仅在显式保证原史回查能力时尝试部分来源索引；必要原文/保护尾部放不下仍明确失败。
class MaiContextCompactor {
public:
    struct Options {
        // 触发/目标占扣除生成和工具余量后的输入容量百分比，须 0 < target < trigger < 100。
        unsigned int triggerPercent = 80;
        unsigned int targetPercent = 60;
        // 最近的完整用户轮次（包含当前轮）保持原文，至少 1；从不拆开工具调用与回执。
        std::size_t retainRecentTurns = 2;
        // 供应商真实 usage 比本地估计更高时，宿主可提高此倍数；小于 1 按 1。
        // 只影响标为估计的计数，不修改实际 usage 或模型目录容量。
        double estimateMultiplier = 1.0;
        // 同一 Runner 先前已安全延后时，跳过软阈值重试；每次仍重算容量与倍率，
        // 超过硬输入预算必须重新压缩。成功检查点推进后由调用方恢复常规软阈值策略。
        bool deferUntilHardLimit = false;
        // 默认禁用。调用方显式保证本次实际发送的请求有已授权的同会话精确原史回查能力，
        // 才可在硬区摘要 Protocol 失败后安装或复用部分来源索引。索引保留有效旧摘要和新增
        // 用户原文/引用/附件元信息，只把稳定 assistant/tool 大正文留在未改写的原史中。
        // 取消、网络、限流、存储/来源/CAS 冲突不恢复；索引也须满足完整预算和严格缩小检查。
        // 无工具收尾或自定义宿主没有该保证时须保持 false；不隐式调用工具协议。
        bool allowHistoryIndexRecovery = false;
    };

    // 特殊宿主可注入“从消息/片段边界构建原文并保留指定 user”的同线程回调。
    // 不提供时使用 builder.build 的同名参数；回调不得修改源快照/存储。
    using BuildContext =
        std::function<std::vector<MaiModelMessage>(std::size_t firstMessage, std::size_t firstPart,
                                                   const std::string& preservedUserMessageId)>;

    MaiContextCompactor();
    explicit MaiContextCompactor(Options options);
    MaiResult<MaiContextPreparation> prepare(MaiSessionStore& store, MaiModelClient& model,
                                             const MaiContextBuilder& builder,
                                             const std::string& sessionId,
                                             const std::vector<MaiMessage>& history,
                                             MaiModelRequest request,
                                             const std::atomic<bool>& cancel,
                                             const BuildContext& buildContext = {}) const;

private:
    Options mOptions;
};
