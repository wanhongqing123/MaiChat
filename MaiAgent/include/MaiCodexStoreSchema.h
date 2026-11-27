// 文件作用：提供联合业务库和独立运行日志库的内部 SQLite 建表定义。
// 这是内部接口，随存储实现演进；不承担数据库迁移或连接管理。
#pragma once

// 返回进程生命周期内有效的只读 SQL 文本，不分配内存、不执行 IO、不会失败。
// 可并发调用；调用者不得释放或修改返回指针。
// 执行业务 SQL 前，每个 SQLite 连接必须注册 3 参数 mai_rollout_item_json，
// 并声明 SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS；函数须读取已提交的 rollout 片段。
// thread_items / thread_realtime_items 的 item_json 是 TEXT VIRTUAL NOT NULL，
// 只有路径、偏移、长度落库；无法读取正文时回调必须报告错误，不能伪造空正文。
// SQL 不包含迁移元数据，调用者负责业务迁移版本、事务和连接生命周期。
const char* maiCodexBusinessSchemaSql();

// 返回独立日志库的只读 SQL，生命周期、并发和失败契约同上。
// 日志定义不得在业务库执行；不要求注册 rollout 正文函数。
const char* maiCodexLogSchemaSql();

// 返回固定元数据导出文件原始字节的 SHA256 标识（带来源前缀）。
// 仅标识建表定义的采样基线，不代表调用者已完成迁移或通过运行验收。
const char* maiCodexSchemaBaselineId();
