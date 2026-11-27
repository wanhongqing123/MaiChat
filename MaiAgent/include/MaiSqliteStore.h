// 文件作用：构造 JSONL 权威日志与可重建 SQLite 索引的正式会话后端。
#pragma once

#include <memory>
#include <functional>
#include <string>

#include "MaiError.h"
#include "MaiSessionStore.h"

// 回调在打开/迁移工作线程同步调用，不接管 UI 线程；必须线程安全且不得抛出异常。
// 取消只在安全边界生效，不撤销已经发布的权威数据。对象只在构造期间借用回调。
struct MaiSqliteStoreOpenOptions {
    std::function<bool()> isCanceled;
    std::function<void(const std::string&)> progress;
    // 仅底层诊断入口可迁移；常规产品工厂始终覆盖为 false，不能通过传参恢复旧历史。
    bool allowLegacyMigration = true;
};

// 查询常规产品当前格式索引；本函数同步阻塞，实际启动使用 makeMaiCurrentSessionStore。
// configuredDatabasePathUtf8 是原账户配置路径；默认使用同目录的 <原文件名>.v16，
// 不打开、迁移或改写旧库。已有新版索引或其权威日志优先，索引丢失仍选择同一路径重建。
// 已在原文件名下完整使用 JSONL 的新版记录继续沿用；未完成旧迁移不作为新版来源。
// 本查询只选择路径，不打开 SQLite、不校验库内容；实际打开和格式验证由下方产品工厂负责。
// 不创建目录、不保留连接；与 session 关联的调度索引须使用同一返回路径。
// ":memory:" 原样返回。空路径及嵌入 NUL 拒绝。不得在模型事件同步回调中调用。
MaiResult<std::string> maiCurrentSessionDatabasePath(const std::string& configuredDatabasePathUtf8);

// 常规产品唯一持久化启动入口：旧历史不导入、已有新版继续使用，索引缺失由自己的日志重建。
// 选中目标损坏、版本不支持或含未完成旧迁移时明确失败，不回退另一个空库。
// 同步阻塞并借用 options 回调；忽略 allowLegacyMigration 的传入值，产品启动永远禁止迁移。
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiCurrentSessionStore(
    const std::string& configuredDatabasePathUtf8, const MaiSqliteStoreOpenOptions& options = {});

// databasePathUtf8 是账号根目录中的索引文件；父目录自动创建。
// 消息、工具正文和续接状态只保存到 sessions/YYYY/MM/DD/ 的追加 JSONL，
// SQLite 保存元数据和字节引用；删除索引后使用同账户 sessions/.journal 提交日志重建。
// 本底层入口保留显式迁移/诊断契约；产品启动必须使用 makeMaiCurrentSessionStore，
// 不将旧版 1..15 的历史自动导入新会话。直接传旧库的诊断行为不作为产品升级策略。
// 损坏或未完成来源明确失败，不把正文读取错误伪装成空历史。
//
// ":memory:" 使用内存 SQLite 和自有临时目录中的同一种日志核心；目录仅用于当前对象，
// 析构删除，不跨对象恢复。它仍会临时写磁盘，不能替代无文件 I/O 的 makeMaiMemoryStore。
// 所有构造和迁移均同步阻塞，调用者必须安排在允许文件 I/O 的线程。
// 构造失败返回错误；权威提交后的索引故障通过 lastProjectionError 单独报告，不能重发业务。
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(const std::string& databasePathUtf8);

MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(
    const std::string& databasePathUtf8, const MaiSqliteStoreOpenOptions& options);
