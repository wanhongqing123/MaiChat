// 文件作用：为正式会话存储协调追加日志、正文引用与可重建元数据投影。
#pragma once
#include <memory>
#include <string>
#include "MaiError.h"
#include "MaiFilePath.h"
#include "MaiMessage.h"
#include "MaiRolloutLog.h"
#include "MaiSession.h"
#include "MaiSqliteProjectionMutation.h"
struct sqlite3;
struct MaiSqliteStoreOpenOptions;
struct sqlite3_stmt;

// 内部接口：调用者独占 SQLite 连接，并以存储锁串行调用；所有方法均可能阻塞。
// SQLite 只保存正文引用；权威元数据事务在 .journal 内按提交顺序持久化。
// commit 成功刷日志后即不可撤销；SQLite 提交失败只设置 projectionError，不能重发业务。
// 临时模式使用自有临时目录，析构删除；不提供旧版正文 SQLite 写入路径。
class MaiSessionJournal {
public:
    static MaiResult<std::unique_ptr<MaiSessionJournal>> open(sqlite3* database,
                                                              const MaiFilePath& root,
                                                              const std::string& indexName,
                                                              bool ephemeral = false);
    ~MaiSessionJournal();
    // 从已知 EOF 读取第一条完整记录，忽略之后正常追加的记录；有界扫描且不改源。
    // 仅供不确定提交的身份核对；成功不等同于持久化，调用者仍须 confirmDurable。
    static MaiResult<MaiRolloutRecord> readFirstRecord(const MaiFilePath& path,
                                                       std::uint64_t byteOffset);
    MaiError begin();
    MaiError commit();
    void rollback();
    MaiError readBarrier();
    MaiError ensureSession(const std::string& sessionId, MaiMillis created = 0,
                           const MaiSession* snapshot = nullptr);
    MaiResult<std::string> appendBody(const std::string& sessionId, const std::string& bytes,
                                      bool binary = false);
    MaiResult<std::string> appendOutput(const std::string& sessionId, const std::string& bytes);
    MaiError appendRecord(const std::string& sessionId, const std::string& type,
                          const std::string& payload);
    MaiResult<std::string> latestRecord(const std::string& sessionId, const std::string& type);
    MaiError projectSession(const MaiSession& session);
    MaiError projectMessage(const std::string& sessionId, const MaiMessage& message);
    MaiError projectionError() const;
    MaiError sourceError() const;
    const MaiFilePath& root() const;
    static MaiError migrateLegacy(const MaiFilePath& databasePath, const MaiFilePath& root,
                                  const MaiSqliteStoreOpenOptions& options);

private:
    struct State;
    explicit MaiSessionJournal(std::unique_ptr<State> state);
    std::unique_ptr<State> mState;
};
