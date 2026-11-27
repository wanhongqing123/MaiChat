// 文件作用：向当前 SQLite 连接注册受存储根约束的原文投影读取函数。
#pragma once

#include "MaiError.h"
#include "MaiFilePath.h"

struct sqlite3;

// 仅内部持久化连接可调用；函数状态归 SQLite 连接持有并在连接关闭时释放。
// storageRoot 必须是已存在的账号存储目录。回读只允许该目录下 sessions/ 的普通文件，
// 不能把任意 SQL 路径当文件访问授权。所有正文从权威日志按字节读取，不写入数据库。
// 注册前须允许阻塞，调用含投影正文的查询也会阻塞；不得在禁止阻塞的事件回调中执行。
// 提供 mai_rollout_item_json(path,offset,length) 与 mai_rollout_payload(path,offset,length)。
// 前者只读取 item_completed 的 item，后者返回整个 payload。损坏/越界/源缺失明确报错，
// 不以空正文伪装成功。元数据查询不依赖这些函数，普通外部 SQLite 客户端仍可读取索引。
MaiError maiRegisterRolloutProjection(sqlite3* database, const MaiFilePath& storageRoot);
