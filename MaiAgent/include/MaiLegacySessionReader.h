// 文件作用：只读提取旧版 MaiChat agent.db，用于一次性迁移，不提供写后端。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "MaiContextCheckpoint.h"
#include "MaiError.h"
#include "MaiMessage.h"
#include "MaiSession.h"
#include "MaiSpecialistTask.h"
#include "MaiToolOutput.h"

struct sqlite3;

// SQLite 原始存储类别与声明类型分开；NULL、空 TEXT、空 BLOB 不合并。
// bytes 仅在 Text/Blob 时有意义，保留非法 UTF-8 和 NUL；整数/浮点使用独立值。
enum class MaiLegacySqlType { Null, Integer, Real, Text, Blob };
struct MaiLegacySqlField {
    std::string sourceField;
    std::string declaredType;
    MaiLegacySqlType type = MaiLegacySqlType::Null;
    std::int64_t integerValue = 0;
    double realValue = 0;
    std::string bytes;
};
struct MaiLegacySqlRow {
    std::string sourceTable;
    std::vector<MaiLegacySqlField> fields;
};
struct MaiLegacySqlTable {
    std::string name;
    std::string definition;
};

struct MaiLegacyToolOutput {
    MaiToolOutputRecord record;
    bool payloadAvailable = false;
    MaiLegacySqlType payloadType = MaiLegacySqlType::Null;
};
struct MaiLegacyToolModelOutput {
    std::string sessionId;
    std::string messageId;
    std::string partId;
    std::string model;
    std::string sourceFingerprint;
    std::string content;
};

struct MaiLegacySessionSnapshot {
    int sourceVersion = 0;
    MaiSession session;
    // 与旧 listMessages 的可见性一致；未发布/被替代/撤销的导入消息独立保留。
    std::vector<MaiMessage> messages;
    std::vector<MaiMessage> unpublishedMessages;
    std::optional<MaiContextCheckpoint> checkpoint;
    std::vector<MaiLegacyToolOutput> toolOutputs;
    std::vector<MaiLegacyToolModelOutput> toolModelOutputs;
    std::vector<MaiSpecialistTask> specialistTasks;
    // 包含该会话所有原表行，包含隐藏消息及所有已知/未知列，不把额外字段悄悄丢弃。
    // context_imports / sources / chunks 的完整账本也在这里，不通过普通 putMessage 导入。
    std::vector<MaiLegacySqlRow> sourceRows;
    std::vector<MaiLegacySqlTable> sourceTables;
};

// 仅接受 sqlite3_db_readonly(database,"main")==1 的调用者连接，支持版本 1..15。
// 不打开/关闭连接、不迁移、不执行写语句或改变 PRAGMA；只在没有事务时开启/结束读事务。
// 调用者保证同步独占该连接并维持生命周期；返回快照归调用方持有，无共享缓存。
// readSessions 仅读会话元数据；readSession 严格按目标会话及归属连接读取各表。
// 不存在返回 NotFound；版本不支持返回 NotSupported；损坏/归属不符返回 InvalidInput，
// SQL 读取错误返回 Internal；失败不交付部分快照，不修改源数据或恢复执行。
// 只读来源仅从同会话已发布且当前选中的导入账本恢复；调用者仍须验证权威来源、
// 账户及会话所有权，不能将此读取接口当作权限/执行事实导入授权。
class MaiLegacySessionReader {
public:
    static MaiResult<std::vector<MaiSession>> readSessions(sqlite3* database);
    static MaiResult<MaiLegacySessionSnapshot> readSession(sqlite3* database,
                                                           const std::string& sessionId);
};
