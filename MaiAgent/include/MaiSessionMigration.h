// 文件作用：把旧 SQLite 会话库发布为先日志、后索引的可恢复存储。
#pragma once

#include <functional>
#include <string>

#include "MaiError.h"
#include "MaiFilePath.h"

struct sqlite3;

// 同步调用期间借用回调，所有工作均可能阻塞；调用者须关闭旧后端及其他写者。
// storageRoot 是本账户的显式存储边界，sourceIndexPath 必须是其内的普通文件。
// 每个暂存目录的 build 仅调用一次，接收在线备份后重新以 READONLY 打开的自有快照连接；
// 在 stagingRoot/sessions 下生成日志，在 stagedIndexPath 生成无正文索引。
// validate 负责完整语义核对，candidateRoot 的 sessions 可已发布到最终位置。
// 回调不得关闭快照连接；返回前须关闭自己的所有输出连接和文件句柄。
// build/validate 的错误原样返回，不发布未通过核对的产物。
struct MaiSessionMigrationOptions {
    MaiFilePath storageRoot;
    MaiFilePath sourceIndexPath;
    std::function<MaiError(sqlite3*, const MaiFilePath&, const MaiFilePath&)> build;
    std::function<MaiError(sqlite3*, const MaiFilePath&, const MaiFilePath&)> validate;
    std::function<bool()> isCanceled;
    // 默认拒绝 ready 清单生成前的半成品。启用后仅识别本账户、原库身份及暂存节点
    // 均匹配 building.json 的自有目录；完整无覆盖归档为同级 .abandoned-UUID 目录后，
    // 从当前旧库重建全新暂存。所有旧日志、快照及原库均保留，不删除或截断。
    // 已有 ready 清单仍严格恢复发布，不能通过本选项绕过清单或内容损坏。
    bool restartIncompleteBuild = false;
    // 仅用于测试或诊断的故障注入；返回错误保留所有原始及暂存文件。
    // 步骤为 building、snapshot_created、build_complete、ready、logs_published、
    // source_preserved、wal_preserved、shm_preserved、
    // index_published、complete；Apple恢复另有apple_wal_restored、apple_source_restored。
    // 恢复时不再调用已经完成步骤的checkpoint；完整sidecar组不需要逆向恢复。
    std::function<MaiError(const std::string&)> checkpoint;
};

// 不提供旧库写后端。构建全部完成并校验后先发布 sessions，再保留原库与 sidecar，
// 最后以不覆盖目标的移动发布原文件名索引。中断后必须继续调用本入口恢复，
// 不能因 agent.db 暂时缺失而创建空库；旧库备份和元数据清单永久保留，不自动删除。
// 主文件和 WAL 的身份及字节指纹在只读快照前冻结，快照后、构建后及发布前复核；
// 构建期间的旧写者提交不能被构建后重新采集的指纹掩盖。SHM 仅用于原字节保全。
// 指纹核对本身不排斥核对与移动之间的写入，不能取代宿主排除旧版写者的契约。
// Windows 原库 READ/share READ|DELETE 句柄从取快照前持有至发布结束，
// 已打开的旧 RW SQLite 连接使本操作返回 Busy。Apple源库使用独立SQLite连接的
// BEGIN IMMEDIATE写保留，从快照前持有到发布结束；快照使用另一READONLY连接。
// 该连接不改正文，NO_CKPT_ON_CLOSE/PERSIST_WAL防止关闭时改变保全的sidecar。
// Apple发布锁文件及非Apple POSIX源文件保留flock。宿主必须拒绝仍被旧App持有的库；
// 不能将进程扫描或写保留释放后视为原子排斥未来旧写者的保证。
// 每次移动均拒绝已有目标；不宣称多个路径的替换是单个原子事务。
// 持久化使用文件 flush 及支持平台上的目录 fsync；Windows 保证进程中断恢复，
// 不将多个 MoveFileEx 操作宣称为断电原子提交。损坏/替换/歧义均停止并保留证据。
class MaiSessionMigration {
public:
    static MaiError run(const MaiSessionMigrationOptions& options);
    // 仅查询固定迁移目录；存在未完成清单或未完成暂存时返回 true。
    // 无文件写入；最终打开存储前仍须由 run 核对和恢复，不能以此查询替代验证。
    static bool hasPending(const MaiFilePath& storageRoot, const MaiFilePath& sourceIndexPath);
};
