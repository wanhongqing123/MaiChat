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
};

// databasePathUtf8 是账号根目录中的索引文件；父目录自动创建。
// 消息、工具正文和续接状态只保存到 sessions/YYYY/MM/DD/ 的追加 JSONL，
// SQLite 保存元数据和字节引用；删除索引后使用同账户 sessions/.journal 提交日志重建。
// 旧版 1..15 数据库只读迁移，先验证自有快照与新日志/索引再发布，保留旧文件。
// 损坏或未完成来源明确失败，不把正文读取错误伪装成空历史。
//
// ":memory:" 使用内存 SQLite 和自有临时目录中的同一种日志核心；目录仅用于当前对象，
// 析构删除，不跨对象恢复。它仍会临时写磁盘，不能替代无文件 I/O 的 makeMaiMemoryStore。
// 所有构造和迁移均同步阻塞，调用者必须安排在允许文件 I/O 的线程。
// 构造失败返回错误；权威提交后的索引故障通过 lastProjectionError 单独报告，不能重发业务。
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(const std::string& databasePathUtf8);

MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(
    const std::string& databasePathUtf8, const MaiSqliteStoreOpenOptions& options);
