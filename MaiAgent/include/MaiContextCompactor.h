// 文件作用：按实际模型窗口增量总结已完成的旧轮次，复用持久检查点并保留原始历史。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "MaiContextBuilder.h"
#include "MaiContextCheckpoint.h"
#include "MaiContextStatus.h"
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
    MaiContextStatus status;
};

// 普通 C++ 摘要入口，直接调用同一个供应商客户端，不依赖工具协议或服务端代理。
// 实例无共享可变状态，可由不同会话工作线程并发使用；模型/存储须履行其并发契约。
// prepare 在调用线程同步推理，不能在事件/流式回调中调用。调用方保持依赖和源快照有效。
// 有原史回查能力时，每轮先逐步缩减工具详情，约回收触发前完整输入的20%即停止。
// 保留未选中的工具详情/正文/像素；字段预算写入检查点，下一轮不会恢复已省略的字节。
// 产品 preserveDialogue 模式下，工具到必要骨架后也不裁正文；超硬限则显式失败。
// 仅普通调用者未启用正文保护时，才允许选择旧正文做其显式语义归档。
// 正文保护会从原史补回旧语义检查点省略的正文，不恢复已省略的工具明细；
// 新检查点成功前旧检查点保持不变。当前用户/保护尾部不裁减，原始字节参与来源校验。
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
        // 触发按原始窗口计算；无工具投影能力时沿用固定目标，须0 < target < trigger < 100。
        // 默认90%触发、95%输入硬限。允许工具优先时改用priorityReductionPercent相对目标，
        // 不再把整份上下文一次缩到窗口60%；显式生成预留仍可进一步收紧硬预算。
        unsigned int triggerPercent = 90;
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
        // 用户原文/引用/附件元信息。完整原文索引放不下时只保留有界来源元数据，历史用户/
        // assistant/tool 正文均在 DB 原史精确回查；当前 user 和保护尾部仍保持原文。
        // 取消、网络、限流、存储/来源/CAS 冲突不恢复；索引也须满足完整预算和严格缩小检查。
        // 无工具收尾或自定义宿主没有该保证时须保持 false；不隐式调用工具协议。
        bool allowHistoryIndexRecovery = false;
        // 已确认供应商拒绝容量时忽略软阈值，只尝试一次；仍保留原文/工具配对及 CAS 边界。
        bool force = false;
        // 开始真实摘要调用或安装实际工具优先整理时，每次 prepare 至多通知一次。
        // 在准备线程同步调用；候选探测/无变化复用不通知，不能执行阻塞宿主操作。
        std::function<void()> onCompactionStarted;
        // 仅当宿主保证当前真实请求可授权读取本会话原史时启用。Runner 按普通入口/工具绑定
        // 自动设置；没有该能力时不安装依赖补读的工具详情投影，也不复用此类检查点。
        // 此开关声明回查能力，不改变工具执行权限，不允许模型传入。
        bool allowToolProjection = false;
        // 工具优先策略每次相对触发前完整输入回收约此百分比；不是窗口的固定目标占比。
        // 达标就停止，后续检查点继续缩减剩余工具；仍须满足真实硬输入预算。
        unsigned int priorityReductionPercent = 20;
        // 产品 Runner 固定启用：用户和助手正文逐字保留，仅缩减工具详情/旧工具图片。
        // 无足够工具内容可缩时保留正文；硬限放不下则失败，不自动做正文语义摘要。
        // 普通摘要调用者可保持 false，显式采用其已有的语义归档契约；模型不能设置此项。
        bool preserveDialogue = false;
        // 摘要请求的初始编码字节预算（通常来自该模型已学到的 413 上限）；0 表示未知。
        // 只约束整理自己发出的摘要请求，不参与主请求的 token 容量判断；主请求体由
        // MaiRequestBodyFitter 在发送前适配。摘要请求仍被 413 拒绝时自动重新分块。
        std::size_t requestByteBudget = 0;
        // 在 prepare 的调用线程、模型流回调之外交付进度。不得抛异常；需要异步处理须
        // 复制快照，不能保留引用。开始/完成/延后/失败均有状态，容量未知保持 0。
        std::function<void(const MaiContextStatus&)> onStatus;
        // 内部真实摘要请求的供应商 usage，同一 prepare 线程同步交付，不合成缺失用量。
        // 可为空；返回源持久化错误时终止整理，不能降级成一次成功的模型计量。
        // 调用方在 prepare 前加载累计状态；摘要与主采样分开标记操作范围。
        std::function<MaiError(const MaiModelUsage&)> onUsage;
    };

    // 特殊宿主可注入“从消息/片段边界构建原文并保留指定 user”的同线程回调。
    // 不提供时使用 builder.build 的同名参数；回调不得修改源快照/存储。
    using BuildContext =
        std::function<std::vector<MaiModelMessage>(std::size_t firstMessage, std::size_t firstPart,
                                                   const std::string& preservedUserMessageId)>;

    MaiContextCompactor();
    explicit MaiContextCompactor(Options options);
    // sessionId 是本次上下文的权威会话；request.sessionId 为空时补齐，非空不一致时
    // 返回 InvalidInput 且不调用模型。路由元数据不改变输入正文、工具或 token 估算。
    MaiResult<MaiContextPreparation> prepare(MaiSessionStore& store, MaiModelClient& model,
                                             const MaiContextBuilder& builder,
                                             const std::string& sessionId,
                                             const std::vector<MaiMessage>& history,
                                             MaiModelRequest request,
                                             const std::atomic<bool>& cancel,
                                             const BuildContext& buildContext = {}) const;

    // 可信宿主在同一会话撤下工具、只生成最后补答时使用。复用通过来源校验的既有
    // 检查点（含先前合法生成的工具投影），不扩大工具权限，也不创建或推进检查点。
    // 新尾部仍按原文追加；超过真实输入硬限时失败并保留检查点，不退回整段摘要。
    MaiResult<MaiContextPreparation> prepareFinalResponse(
        MaiSessionStore& store, MaiModelClient& model, const MaiContextBuilder& builder,
        const std::string& sessionId, const std::vector<MaiMessage>& history,
        MaiModelRequest request, const std::atomic<bool>& cancel,
        const BuildContext& buildContext = {}) const;

private:
    MaiResult<MaiContextPreparation> prepareWithMode(
        MaiSessionStore& store, MaiModelClient& model, const MaiContextBuilder& builder,
        const std::string& sessionId, const std::vector<MaiMessage>& history,
        MaiModelRequest request, const std::atomic<bool>& cancel, const BuildContext& buildContext,
        bool reuseOnly) const;
    MaiResult<MaiContextPreparation> prepareImpl(
        MaiSessionStore& store, MaiModelClient& model, const MaiContextBuilder& builder,
        const std::string& sessionId, const std::vector<MaiMessage>& history,
        MaiModelRequest request, const std::atomic<bool>& cancel, const BuildContext& buildContext,
        MaiContextStatus& status, bool reuseOnly) const;
    Options mOptions;
};
