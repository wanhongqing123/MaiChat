// 文件作用：记录进程内轮次真实模型边界，不记录流式草稿增量。
#pragma once

#include <optional>
#include <set>
#include <string>

#include "MaiModelClient.h"
#include "MaiSessionStore.h"
#include "MaiTurnContext.h"

// 请求开始处的真实输入计量覆盖边界，仅保存有界指纹/数量，不保存或重复正文。
// version 是哈希字段协议；estimatorVersion 是本地估计器版本，任一不匹配均回退估计。
// source ID 不参与指纹；角色/原文/调用/原始续接/声明顺序参与。路径图片不允许复用。
// 生命周期为普通值快照；测量只有与同条 token_usage_record 原子提交后才可跨轮次使用。
struct MaiModelInputAnchor {
    unsigned int version = 1;
    unsigned int estimatorVersion = 3;
    std::string model;
    std::string provider;
    MaiWireApi wire = MaiWireApi::Responses;
    MaiModelContextLimits limits;
    std::uint64_t checkpointRevision = 0;
    std::size_t messageCount = 0;
    std::size_t toolCount = 0;
    std::size_t localInputTokens = 0;
    std::size_t measuredInputTokens = 0;
    bool localExact = false;
    bool hasPathImages = false;
    std::string frameDigest;
    std::string toolsDigest;
    std::string messagesDigest;
};

// 一个实例属于一个轮次，仅在轮次工作线程同步调用；存储与上下文须活到实例销毁。
// prepare 在模型网络调用之前调用，任何权威读写失败都由调用方终止采样。
// NotSupported 仅保留旧/临时存储兼容；普通错误绝不降级成空历史。
class MaiRolloutRuntimeFacts {
public:
    MaiRolloutRuntimeFacts(MaiSessionStore* store, const MaiTurnContext& context,
                           std::string provider = {}, std::size_t windowTokens = 0);
    // 在任何主请求或内部摘要网络调用前读取既存计量/窗口状态；重复调用为空操作。
    MaiError initialize();
    // 轮次开始与源终态提交后的标准事实；普通失败完成携带 error，取消单独记录中断。
    // 重复调用不追加；没有成功的 started 时不伪造 finished。
    MaiError started();
    MaiError finished(const MaiTurnOutcome& outcome);
    MaiError prepare(const MaiModelRequest& request, bool usedCheckpoint);
    MaiError outputItem(const MaiModelOutputItem& item);
    MaiError usage(const MaiModelUsage& usage, const std::string& operation = "sampling",
                   const MaiModelInputAnchor* inputAnchor = nullptr);
    // 同步普通值快照，无 IO；初始化/usage/prepare 成功之后读取，生命周期独立于实例。
    MaiModelContextUsage contextUsage() const;
    // initialize 读取后返回最近原子提交的真实输入锚点，无 IO；不把它当 UI 实测占用。
    // 旧/不支持版本/损坏的锚点元数据返回 nullopt，不据此判定整段原史损坏。
    std::optional<MaiModelInputAnchor> inputEstimateAnchor() const;

private:
    MaiError append(const std::string& type, const std::string& payload);
    MaiResult<std::string> latest(const std::string& type) const;

    MaiSessionStore* mStore;
    MaiTurnContext mContext;
    bool mPrepared = false;
    bool mInitialized = false;
    bool mStarted = false;
    bool mFinished = false;
    MaiMillis mStartedAt = 0;
    // 仅为当前实例的有界去重缓存；绝不把全历史身份集重复写入每条事实。
    // 重启与缓存淘汰后的最终幂等由后端的稳定源身份/载荷索引保证。
    std::set<std::string> mInputSources;
    std::string mPreviousContext;
    std::string mBaseDigest;
    std::string mToolsDigest;
    std::string mTurnUsage;
    std::string mThreadUsage;
    std::string mLatestUsage;
    std::string mWindowId;
    std::uint64_t mWindowNumber = 0;
    std::uint64_t mCheckpointRevision = 0;
    std::string mProvider;
    std::size_t mWindowTokens = 0;
    MaiModelContextUsage mContextUsage;
    std::optional<MaiModelInputAnchor> mInputEstimateAnchor;
};
