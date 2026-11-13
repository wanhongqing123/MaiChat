// 文件作用：定义不执行模型的外部上下文暂存、原子发布和来源替换契约。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"

// sourceKey 是宿主提供的稳定逻辑来源，不包含连接代次；snapshotId 标识本次完整读取。
// 每个 Session/sourceKey 仅有一个可见快照。正文应由宿主标为带来源的引用资料，
// 不能把外部文本当成系统指令、assistant 回答、工具回执或执行权限。
struct MaiContextImportSpec {
    std::string sessionId;
    std::string sourceKey;
    std::string snapshotId;
    std::string label;
};

// 按宿主确定的原始时间顺序追加。sourceId 在本快照内唯一，重连不得另造相同记录的 ID。
// text 原样保存，无截断；仅生成已完成的 User/Text 上下文，不生成待消费用户输入。
// 原始时间用于来源展示；消息顺序仍由 Core 新生成的单调 ID 决定。
struct MaiContextImportRecord {
    std::string sourceId;
    std::string text;
    std::int64_t originalCreatedAtMillis = 0;
};

struct MaiContextImportProgress {
    std::string importId;
    std::string sessionId;
    std::string sourceKey;
    std::string snapshotId;
    std::uint64_t nextSequence = 0;
    std::uint64_t recordCount = 0;
    bool committed = false;
};

struct MaiContextImportPurgeResult {
    std::size_t removed = 0;
    // 仍有可回收的旧消息或过时导入账本；调用方在既有 worker 的后续周期继续有界清理。
    bool hasMore = false;
};

constexpr std::size_t kMaiContextImportMaxPurgeMessages = 200;

class MaiAgent;

// 可复制到宿主既有 I/O worker 的普通能力，不保留 MaiAgent 指针或延长门面生命周期。
// 内部仅弱引用 Runtime，每次同步操作临时锁定存储依赖；不创建线程或执行器。
// Agent 析构首先撤销能力。撤销后的新调用返回 Canceled；已进入的存储事务可以结束，
// 但不会在门面关闭后发布 ContextImported。默认构造的能力同样返回 Canceled。
// 所有操作可能同步读写数据库，必须在普通后台线程调用，不能在 UI/事件回调调用。
class MaiContextImportWorker {
public:
    MaiContextImportWorker() = default;
    MaiContextImportWorker(const MaiContextImportWorker&) = default;
    MaiContextImportWorker& operator=(const MaiContextImportWorker&) = default;
    MaiContextImportWorker(MaiContextImportWorker&&) = default;
    MaiContextImportWorker& operator=(MaiContextImportWorker&&) = default;

    MaiResult<MaiContextImportProgress> beginContextImport(const MaiContextImportSpec& spec) const;
    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& importId, std::uint64_t sequence,
        const std::vector<MaiContextImportRecord>& records) const;
    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& importId) const;
    MaiError abortContextImport(const std::string& importId) const;
    // 仅回收已撤销或已被新快照替代的导入数据，绝不清除普通、当前或仍暂存的消息。
    // maxMessages 合法范围为 1..200；返回真实删除数，hasMore 可包含过时账本。
    MaiResult<MaiContextImportPurgeResult> purgeContextImports(const std::string& sessionId,
                                                               std::size_t maxMessages) const;

private:
    struct Impl;
    explicit MaiContextImportWorker(std::shared_ptr<Impl> implementation);
    std::shared_ptr<Impl> mImplementation;
    friend class MaiAgent;
};

// 单批上限独立于完整来源大小，防止暂存一次加载全部历史。超过限制整批失败，
// 不截断或发布部分记录。大小按 sourceId/text 原始字节之和计算。
constexpr std::size_t kMaiContextImportMaxBatchRecords = 200;
constexpr std::size_t kMaiContextImportMaxBatchBytes = 512 * 1024;
constexpr std::size_t kMaiContextImportMaxRecordTextBytes = 256 * 1024;

// 两个标准后端共用严格校验和带长度的 SHA-256，防止后端对同一批次产生不同语义。
MaiError maiValidateContextImportSpec(const MaiContextImportSpec& spec);
MaiError maiValidateContextImportBatch(const std::vector<MaiContextImportRecord>& records);
std::string maiContextImportBatchDigest(const std::vector<MaiContextImportRecord>& records);
