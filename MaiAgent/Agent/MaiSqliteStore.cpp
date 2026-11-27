// 文件作用：复用 SQLite 业务事务，将正文及权威提交切换到追加 JSONL。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiSqliteStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiBlockingCheck.h"
#include "MaiSessionJournal.h"
#include "MaiSessionMigration.h"
#include "MaiSessionRecordCodec.h"

// SQLite 业务投影与追加日志共用同一存储锁。
//
// ── 为什么排序靠 id，而不是另开一个 seq 列 ──────────────────────
// MaiIdGenerator 产出的 id 是 前缀 + 10 位时间戳 + 3 位同毫秒序号 +8 位随机，定长，
// 而且用的 base32 字母表在 ASCII 里是递增的（0-9 然后 A..Z 去掉 ILOU）。
// 定长 + 字母表递增 => 字典序等于生成顺序。所以 `ORDER BY id` 就是插入顺序，和内存存储的行为一致。
//
// 这不是巧合，是 id 设计时就冲着这个去的（见 MaiIdGenerator.h）。万一哪天换了 id 方案，
// 这里的排序会跟着错——MaiStoreTests 里有一条用例专门盯着"顺序和插入顺序一致"。
//
// ── 为什么整个连接一把锁 ────────────────────────────────────────
// 多个会话各跑各的线程，都会写这个库。SQLite 自己的 FULLMUTEX 只保证单条语句安全，
// 而 putMessage 是"删旧 part + 插新 part"好几条语句，mutateSession 更是读-改-写。这些必须整体原子，
// 所以锁在我们这一层。
//
// 代价是写入串行化。可以接受：putMessage 的调用频率是"每个工具调用一次加每轮一次"，
// 不是每个 delta 一次（那是当初就定死的，见 MaiTurnRunner）。

namespace {

// 和 MaiRole / MaiToolState 的枚举值绑死。**不要改这些数字**——库里已经存了的行不会跟着变。
// 加新值只能往后追加。
int roleToColumn(MaiRole role) {
    return role == MaiRole::User ? 0 : 1;
}

MaiRole columnToRole(int value) {
    return value == 0 ? MaiRole::User : MaiRole::Assistant;
}

int specialistStatusToColumn(MaiSpecialistTaskStatus status) {
    return static_cast<int>(status);
}

MaiSpecialistTaskStatus columnToSpecialistStatus(int value) {
    if (value >= 0 && value <= static_cast<int>(MaiSpecialistTaskStatus::Canceled))
        return static_cast<MaiSpecialistTaskStatus>(value);
    return MaiSpecialistTaskStatus::Submitted;
}

enum PartKind { kText = 0, kReasoning = 1, kTool = 2, kImage = 3, kVideo = 4, kQuote = 5 };

int toolStateToColumn(MaiToolState state) {
    switch (state) {
        case MaiToolState::Pending: return 0;
        case MaiToolState::Running: return 1;
        case MaiToolState::Completed: return 2;
        case MaiToolState::Error: return 3;
        case MaiToolState::Canceled: return 4;
    }
    return 0;
}

MaiToolState columnToToolState(int value) {
    switch (value) {
        case 1: return MaiToolState::Running;
        case 2: return MaiToolState::Completed;
        case 3: return MaiToolState::Error;
        case 4: return MaiToolState::Canceled;
        default: return MaiToolState::Pending;
    }
}

std::string textColumn(sqlite3_stmt* statement, int index) {
    const unsigned char* text = sqlite3_column_text(statement, index);
    if (!text) return {};
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, index)));
}

// 元数据不进入正文；使用稳定字符串而非枚举序号，旧消息无值保持未知。
std::string turnStateJson(const MaiMessage& message) {
    auto result = MaiSessionRecordCodec::encodeTurnState(message);
    return result ? result.value() : std::string{};
}
void restoreTurnState(const std::string& raw, MaiMessage& message) {
    (void)MaiSessionRecordCodec::decodeTurnState(raw, message);
}

// 两列仅来自同一会话已提交导入账本的 join；不从正文或普通 putMessage 输入推测来源。
void restoreImportOrigin(sqlite3_stmt* statement, MaiMessage& message) {
    const auto sourceKey = textColumn(statement, 6);
    const auto sourceId = textColumn(statement, 7);
    if (!sourceKey.empty() && !sourceId.empty())
        message.contextImportOrigin = MaiContextImportOrigin{sourceKey, sourceId};
    restoreTurnState(textColumn(statement, 8), message);
}

// 元数据按显式长度绑定，不归一化旧字段字节；正文通过 bindSourceText 保存到权威日志。
void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

// 续接状态由 wire 校验；存储层不能归一化带签名/加密字段的不透明字节。
void bindContinuationText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

class SqliteStore final : public MaiSessionStore {
public:
    explicit SqliteStore(sqlite3* database, std::unique_ptr<MaiSessionJournal> journal)
        : mDatabase(database), mJournal(std::move(journal)) {}

    ~SqliteStore() override {
        for (sqlite3_stmt* statement : mCached) sqlite3_finalize(statement);
        mJournal.reset();
        sqlite3_close(mDatabase);
    }

    MaiError invalidateAbandonedImports() {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        if (!executeStatementWithTextArgument(
                "UPDATE context_imports SET state=2 WHERE state=0 AND ?1=''", "") ||
            !executeTransactionStatement("COMMIT")) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        return {};
    }

    MaiError lastReadError() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mLastReadError;
    }
    MaiError lastProjectionError() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mJournal->projectionError();
    }
    MaiError appendSessionRecord(const std::string& sessionId, const std::string& type,
                                 const std::string& payload) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto* owner = prepareLocked("SELECT 1 FROM sessions WHERE id=?1");
        if (!owner) return mLastReadError;
        {
            Reset reset(owner);
            bindText(owner, 1, sessionId);
            if (stepLocked(owner) != SQLITE_ROW)
                return {MaiErrorCode::NotFound, "Session is unavailable."};
        }
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        auto error = mJournal->appendRecord(sessionId, type, payload);
        if (error) {
            executeTransactionStatement("ROLLBACK");
            return error;
        }
        if (!executeTransactionStatement("COMMIT")) return mLastWriteError;
        return {};
    }
    MaiResult<std::string> readLatestSessionRecord(const std::string& sessionId,
                                                   const std::string& type) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        auto result = mJournal->latestRecord(sessionId, type);
        if (!result) mLastReadError = result.error();
        return result;
    }
    MaiResult<std::string> getSessionContext(const std::string& sessionId,
                                             const std::string& name) const override {
        if (auto error = maiValidateSessionContextKey(sessionId, name)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        return readSessionContextLocked(sessionId, name);
    }
    MaiResult<std::string> saveSessionContext(const std::string& sessionId, const std::string& name,
                                              const std::string& content) override {
        if (auto error = maiValidateSessionContextKey(sessionId, name)) return error;
        if (auto error = maiValidateSessionContextContent(content)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        const auto fail = [&](MaiError error) -> MaiResult<std::string> {
            executeTransactionStatement("ROLLBACK");
            return error;
        };
        auto existing = readSessionContextLocked(sessionId, name);
        if (existing) {
            executeTransactionStatement("ROLLBACK");
            return existing;
        }
        if (existing.error().code() != MaiErrorCode::NotFound) return fail(existing.error());
        auto* insert = prepareLocked(
            "INSERT INTO session_contexts(session_id,name,content_ref) SELECT ?1,?2,?3 WHERE "
            "EXISTS(SELECT 1 FROM sessions WHERE id=?1) ON CONFLICT DO NOTHING");
        if (!insert) return fail(mLastWriteError);
        {
            Reset reset(insert);
            bindText(insert, 1, sessionId);
            bindText(insert, 2, name);
            bindSourceText(insert, 3, content, sessionId);
            if (!executePreparedStatement(insert)) return fail(mLastWriteError);
        }
        auto winner = readSessionContextLocked(sessionId, name);
        if (!winner) return fail(winner.error());
        if (!executeTransactionStatement("COMMIT")) return fail(mLastWriteError);
        return winner;
    }

    MaiResult<MaiContextImportProgress> beginContextImport(
        const MaiContextImportSpec& spec) override {
        if (const auto error = maiValidateContextImportSpec(spec)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        ImportErrorScope handledError(mLastWriteError);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return importErrorLocked();
        auto* check = prepareLocked("SELECT 1 FROM sessions WHERE id=?1");
        bool exists = false;
        if (check) {
            Reset reset(check);
            bindText(check, 1, spec.sessionId);
            exists = stepLocked(check) == SQLITE_ROW;
        }
        if (!exists) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::NotFound, "Context import session is unavailable."};
        }
        auto* pending = prepareLocked(
            "SELECT 1 FROM context_imports WHERE session_id=?1 AND source_key=?2 AND state=0");
        bool busy = false;
        if (pending) {
            Reset reset(pending);
            bindText(pending, 1, spec.sessionId);
            bindText(pending, 2, spec.sourceKey);
            busy = stepLocked(pending) == SQLITE_ROW;
        }
        if (!pending) return rollbackImportLocked();
        if (busy) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::Busy, "A context snapshot is already being staged."};
        }
        MaiContextImportProgress progress{MaiIdGenerator::generate("imp"),
                                          spec.sessionId,
                                          spec.sourceKey,
                                          spec.snapshotId,
                                          0,
                                          0,
                                          false};
        auto* insert = prepareLocked(
            "INSERT INTO context_imports(id,session_id,source_key,snapshot_id,label) "
            "VALUES(?1,?2,?3,?4,?5)");
        bool ok = false;
        if (insert) {
            Reset reset(insert);
            bindText(insert, 1, progress.importId);
            bindText(insert, 2, spec.sessionId);
            bindText(insert, 3, spec.sourceKey);
            bindText(insert, 4, spec.snapshotId);
            bindText(insert, 5, spec.label);
            ok = executePreparedStatement(insert);
        }
        if (!ok || !executeTransactionStatement("COMMIT")) return rollbackImportLocked();
        return progress;
    }

    MaiResult<MaiContextImportProgress> inspectContextImport(
        const std::string& importId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return inspectImportLocked(importId);
    }

    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& importId, std::uint64_t sequence,
        const std::vector<MaiContextImportRecord>& records) override {
        if (const auto error = maiValidateContextImportBatch(records)) return error;
        if (sequence >= static_cast<std::uint64_t>(INT64_MAX))
            return {MaiErrorCode::InvalidInput, "Context batch sequence is too large."};
        const auto digest = maiContextImportBatchDigest(records);
        if (digest.empty()) return {MaiErrorCode::Internal, "Cannot fingerprint context batch."};
        std::lock_guard<std::mutex> lock(mMutex);
        ImportErrorScope handledError(mLastWriteError);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return importErrorLocked();
        auto inspected = inspectImportLocked(importId);
        if (!inspected) {
            executeTransactionStatement("ROLLBACK");
            return inspected.error();
        }
        auto progress = inspected.value();
        if (progress.committed) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::InvalidInput, "Context import is already committed."};
        }
        auto* prior = prepareLocked(
            "SELECT digest FROM context_import_chunks WHERE import_id=?1 AND sequence=?2");
        bool hasPrior = false;
        std::string priorDigest;
        if (prior) {
            Reset reset(prior);
            bindText(prior, 1, importId);
            sqlite3_bind_int64(prior, 2, static_cast<sqlite3_int64>(sequence));
            const int code = stepLocked(prior);
            hasPrior = code == SQLITE_ROW;
            if (hasPrior)
                priorDigest = textColumn(prior, 0);
            else if (code != SQLITE_DONE)
                return rollbackImportLocked();
        }
        if (!prior) return rollbackImportLocked();
        if (hasPrior) {
            executeTransactionStatement("ROLLBACK");
            if (priorDigest == digest) return progress;
            return {MaiErrorCode::InvalidInput, "Context batch retry has different content."};
        }
        auto* chronology = prepareLocked("SELECT last_created FROM context_imports WHERE id=?1");
        std::int64_t lastCreated = 0;
        if (chronology) {
            Reset reset(chronology);
            bindText(chronology, 1, importId);
            if (stepLocked(chronology) == SQLITE_ROW)
                lastCreated = sqlite3_column_int64(chronology, 0);
            else
                return rollbackImportLocked();
        }
        if (!chronology) return rollbackImportLocked();
        if (sequence != progress.nextSequence ||
            (progress.recordCount && records.front().originalCreatedAtMillis < lastCreated)) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::InvalidInput, "Context batch sequence or chronology is invalid."};
        }
        auto* duplicate = prepareLocked(
            "SELECT 1 FROM messages WHERE context_import_id=?1 AND context_source_id=?2");
        if (!duplicate) return rollbackImportLocked();
        for (const auto& record : records) {
            Reset reset(duplicate);
            bindText(duplicate, 1, importId);
            bindText(duplicate, 2, record.sourceId);
            const int code = stepLocked(duplicate);
            if (code == SQLITE_ROW) {
                executeTransactionStatement("ROLLBACK");
                return {MaiErrorCode::InvalidInput, "Context source record is duplicated."};
            }
            if (code != SQLITE_DONE) return rollbackImportLocked();
        }
        for (const auto& record : records) {
            const auto messageId = MaiIdGenerator::newMessageId();
            auto* message = prepareLocked(
                "INSERT INTO "
                "messages(id,session_id,role,created,completed,context_import_id,context_source_id)"
                " VALUES(?1,?2,0,?3,?4,?5,?6)");
            bool ok = false;
            if (message) {
                Reset reset(message);
                bindText(message, 1, messageId);
                bindText(message, 2, progress.sessionId);
                sqlite3_bind_int64(message, 3, record.originalCreatedAtMillis);
                sqlite3_bind_int64(message, 4,
                                   std::max<std::int64_t>(1, record.originalCreatedAtMillis));
                bindText(message, 5, importId);
                bindText(message, 6, record.sourceId);
                ok = executePreparedStatement(message);
            }
            if (!ok) return rollbackImportLocked();
            MaiMessagePart part{MaiIdGenerator::newPartId(), MaiTextPart{record.text},
                                record.originalCreatedAtMillis};
            if (!writeImportPartLocked(messageId, part)) return rollbackImportLocked();
        }
        auto* chunk = prepareLocked(
            "INSERT INTO context_import_chunks(import_id,sequence,digest) VALUES(?1,?2,?3)");
        bool ok = false;
        if (chunk) {
            Reset reset(chunk);
            bindText(chunk, 1, importId);
            sqlite3_bind_int64(chunk, 2, static_cast<sqlite3_int64>(sequence));
            bindText(chunk, 3, digest);
            ok = executePreparedStatement(chunk);
        }
        if (!ok) return rollbackImportLocked();
        auto* advance = prepareLocked(
            "UPDATE context_imports SET "
            "next_sequence=next_sequence+1,record_count=record_count+?2,last_created=?3 WHERE "
            "id=?1");
        if (advance) {
            Reset reset(advance);
            bindText(advance, 1, importId);
            sqlite3_bind_int64(advance, 2, static_cast<sqlite3_int64>(records.size()));
            sqlite3_bind_int64(advance, 3, records.back().originalCreatedAtMillis);
            ok = executePreparedStatement(advance);
        } else
            ok = false;
        if (!ok || !executeTransactionStatement("COMMIT")) return rollbackImportLocked();
        ++progress.nextSequence;
        progress.recordCount += records.size();
        return progress;
    }

    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& importId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        ImportErrorScope handledError(mLastWriteError);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return importErrorLocked();
        auto inspected = inspectImportLocked(importId);
        if (!inspected) {
            executeTransactionStatement("ROLLBACK");
            return inspected.error();
        }
        auto progress = inspected.value();
        if (progress.committed) {
            executeTransactionStatement("ROLLBACK");
            return progress;
        }
        auto* publish = prepareLocked(
            "INSERT INTO context_import_sources(session_id,source_key,import_id) VALUES(?1,?2,?3) "
            "ON CONFLICT(session_id,source_key) DO UPDATE SET import_id=excluded.import_id");
        bool ok = false;
        if (publish) {
            Reset reset(publish);
            bindText(publish, 1, progress.sessionId);
            bindText(publish, 2, progress.sourceKey);
            bindText(publish, 3, importId);
            ok = executePreparedStatement(publish);
        }
        ok = ok && executeStatementWithTextArgument(
                       "UPDATE context_imports SET state=1 WHERE id=?1", importId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM context_checkpoints WHERE session_id=?1", progress.sessionId);
        ok = ok && projectVisibleImportLocked(progress);
        if (!ok || !executeTransactionStatement("COMMIT")) return rollbackImportLocked();
        clearRecentLocked(progress.sessionId);
        progress.committed = true;
        return progress;
    }

    MaiError abortContextImport(const std::string& importId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        ImportErrorScope handledError(mLastWriteError);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return importErrorLocked();
        const auto inspected = inspectImportLocked(importId);
        if (!inspected) {
            executeTransactionStatement("ROLLBACK");
            return inspected.error();
        }
        if (inspected.value().committed) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::InvalidInput, "Published context cannot be aborted."};
        }
        // 先常量时间撤销资格；大量隐藏副本由有界普通维护回收，不能在取消/UI 收尾里长扫。
        const bool ok = executeStatementWithTextArgument(
            "UPDATE context_imports SET state=2 WHERE id=?1 AND state=0", importId);
        if (!ok || !executeTransactionStatement("COMMIT")) return rollbackImportLocked();
        clearRecentLocked(inspected.value().sessionId);
        return {};
    }

    MaiResult<MaiContextImportPurgeResult> purgeContextImports(const std::string& sessionId,
                                                               std::size_t maxMessages) override {
        if (maxMessages == 0 || maxMessages > kMaiContextImportMaxPurgeMessages)
            return {MaiErrorCode::InvalidInput, "Context maintenance budget is invalid."};
        std::lock_guard<std::mutex> lock(mMutex);
        ImportErrorScope handledError(mLastWriteError);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return importErrorLocked();
        auto* session = prepareLocked("SELECT 1 FROM sessions WHERE id=?1");
        if (!session) return rollbackImportLocked();
        {
            Reset reset(session);
            bindText(session, 1, sessionId);
            const int code = stepLocked(session);
            if (code != SQLITE_ROW) {
                if (code != SQLITE_DONE) return rollbackImportLocked();
                executeTransactionStatement("ROLLBACK");
                return {MaiErrorCode::NotFound, "Context session is unavailable."};
            }
        }
        const char* eligible =
            "SELECT i.id FROM context_imports i WHERE i.session_id=?1 AND i.state IN(1,2) "
            "AND NOT EXISTS(SELECT 1 FROM context_import_sources s WHERE s.import_id=i.id) LIMIT "
            "?2";
        auto* groups = prepareLocked(eligible);
        if (!groups) return rollbackImportLocked();
        std::vector<std::string> imports;
        {
            Reset reset(groups);
            bindText(groups, 1, sessionId);
            sqlite3_bind_int64(groups, 2, static_cast<sqlite3_int64>(maxMessages));
            int code;
            while ((code = stepLocked(groups)) == SQLITE_ROW)
                imports.push_back(textColumn(groups, 0));
            if (code != SQLITE_DONE) return rollbackImportLocked();
        }
        MaiContextImportPurgeResult result;
        std::size_t metadataBudget = maxMessages;
        for (const auto& importId : imports) {
            std::vector<std::string> ids;
            auto* candidates = prepareLocked(
                "SELECT id FROM messages WHERE context_import_id=?1 ORDER BY id LIMIT ?2");
            if (!candidates) return rollbackImportLocked();
            {
                Reset reset(candidates);
                bindText(candidates, 1, importId);
                sqlite3_bind_int64(candidates, 2,
                                   static_cast<sqlite3_int64>(maxMessages - result.removed));
                int code;
                while ((code = stepLocked(candidates)) == SQLITE_ROW)
                    ids.push_back(textColumn(candidates, 0));
                if (code != SQLITE_DONE) return rollbackImportLocked();
            }
            for (const auto& id : ids) {
                if (!executeStatementWithTextArgument("DELETE FROM parts WHERE message_id=?1", id))
                    return rollbackImportLocked();
                auto* remove =
                    prepareLocked("DELETE FROM messages WHERE id=?1 AND context_import_id=?2");
                if (!remove) return rollbackImportLocked();
                Reset reset(remove);
                bindText(remove, 1, id);
                bindText(remove, 2, importId);
                if (!executePreparedStatement(remove) || mLastStatementChanges != 1)
                    return rollbackImportLocked();
                ++result.removed;
            }
            auto* remaining =
                prepareLocked("SELECT 1 FROM messages WHERE context_import_id=?1 LIMIT 1");
            if (!remaining) return rollbackImportLocked();
            bool hasMessages;
            {
                Reset reset(remaining);
                bindText(remaining, 1, importId);
                const int code = stepLocked(remaining);
                if (code != SQLITE_ROW && code != SQLITE_DONE) return rollbackImportLocked();
                hasMessages = code == SQLITE_ROW;
            }
            if (!hasMessages && metadataBudget != 0) {
                auto* chunks = prepareLocked(
                    "DELETE FROM context_import_chunks WHERE rowid IN(SELECT rowid FROM "
                    "context_import_chunks "
                    "WHERE import_id=?1 LIMIT ?2)");
                if (!chunks) return rollbackImportLocked();
                {
                    Reset reset(chunks);
                    bindText(chunks, 1, importId);
                    sqlite3_bind_int64(chunks, 2, static_cast<sqlite3_int64>(metadataBudget));
                    if (!executePreparedStatement(chunks)) return rollbackImportLocked();
                    metadataBudget -= static_cast<std::size_t>(mLastStatementChanges);
                }
                auto* chunkLeft =
                    prepareLocked("SELECT 1 FROM context_import_chunks WHERE import_id=?1 LIMIT 1");
                if (!chunkLeft) return rollbackImportLocked();
                bool hasChunks;
                {
                    Reset reset(chunkLeft);
                    bindText(chunkLeft, 1, importId);
                    const int code = stepLocked(chunkLeft);
                    if (code != SQLITE_ROW && code != SQLITE_DONE) return rollbackImportLocked();
                    hasChunks = code == SQLITE_ROW;
                }
                if (!hasChunks && metadataBudget != 0) {
                    if (!executeStatementWithTextArgument("DELETE FROM context_imports WHERE id=?1",
                                                          importId))
                        return rollbackImportLocked();
                    --metadataBudget;
                }
            }
            if (result.removed == maxMessages && metadataBudget == 0) break;
        }
        auto* more = prepareLocked(
            "SELECT 1 FROM context_imports i WHERE i.session_id=?1 AND i.state IN(1,2) "
            "AND NOT EXISTS(SELECT 1 FROM context_import_sources s WHERE s.import_id=i.id) LIMIT "
            "1");
        if (!more) return rollbackImportLocked();
        {
            Reset reset(more);
            bindText(more, 1, sessionId);
            const int code = stepLocked(more);
            if (code != SQLITE_ROW && code != SQLITE_DONE) return rollbackImportLocked();
            result.hasMore = code == SQLITE_ROW;
        }
        if (!executeTransactionStatement("COMMIT")) return rollbackImportLocked();
        clearRecentLocked(sessionId);
        return result;
    }

    void putSession(const MaiSession& session) override {
        std::lock_guard<std::mutex> lock(mMutex);
        writeSessionLocked(session);
    }

    bool getSession(const std::string& id, MaiSession& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated, parent_id, depth, "
            "title_edited, reasoning_effort "
            "FROM sessions WHERE id = ?1");
        if (!statement) return false;
        Reset guard(statement);
        bindText(statement, 1, id);
        if (stepLocked(statement) != SQLITE_ROW) return false;
        out = readSession(statement);
        return true;
    }

    std::vector<MaiSession> listSessions() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        std::vector<MaiSession> out;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated, parent_id, depth, "
            "title_edited, reasoning_effort "
            "FROM sessions ORDER BY updated DESC, id DESC");
        if (!statement) return out;
        Reset guard(statement);
        while (stepLocked(statement) == SQLITE_ROW) out.push_back(readSession(statement));
        return out;
    }

    bool removeSession(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return false;

        // 显式逐级删，不靠 ON DELETE CASCADE：外键约束要连接上开了 PRAGMA foreign_keys 才生效，
        // 而那个 pragma 是每连接的，将来谁新开一个连接忘了设，删除就会只删一半、留下孤儿行。
        bool ok =
            executeStatementWithTextArgument("DELETE FROM tool_outputs WHERE session_id = ?1", id);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM tool_model_outputs WHERE session_id = ?1", id);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM parts WHERE message_id IN "
                       "(SELECT id FROM messages WHERE session_id = ?1)",
                       id);
        ok = ok &&
             executeStatementWithTextArgument("DELETE FROM messages WHERE session_id = ?1", id);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM context_checkpoints WHERE session_id = ?1", id);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM specialist_tasks WHERE owner_session_id = ?1", id);
        ok = ok && clearImportsLocked(id);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM session_contexts WHERE session_id=?1", id);
        ok = ok &&
             executeStatementWithTextArgument("DELETE FROM thread_items WHERE thread_id=?1", id);
        ok = ok &&
             executeStatementWithTextArgument("DELETE FROM thread_turns WHERE thread_id=?1", id);
        ok = ok && executeStatementWithTextArgument("DELETE FROM threads WHERE id=?1", id);
        clearRecentLocked(id);
        ok = ok && executeStatementWithTextArgument("DELETE FROM sessions WHERE id = ?1", id);
        const int removed = mLastStatementChanges;

        if (ok && executeTransactionStatement("COMMIT")) return removed > 0;
        executeTransactionStatement("ROLLBACK");
        return false;
    }

    bool clearMessages(const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);

        // 先确认会话存在。不确认的话，对着一个不存在的 id 清空会返回 true，
        // 调用方以为清成功了，其实什么都没发生。
        sqlite3_stmt* check = prepareLocked("SELECT 1 FROM sessions WHERE id = ?1");
        if (check == nullptr) return false;
        bindText(check, 1, sessionId);
        const bool exists = stepLocked(check) == SQLITE_ROW;
        sqlite3_reset(check);
        if (!exists) return false;

        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return false;
        // 和 removeSession 一样显式逐级删，理由见那边的注释。
        bool ok = executeStatementWithTextArgument("DELETE FROM tool_outputs WHERE session_id = ?1",
                                                   sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM tool_model_outputs WHERE session_id = ?1", sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM parts WHERE message_id IN "
                       "(SELECT id FROM messages WHERE session_id = ?1)",
                       sessionId);
        ok = ok && executeStatementWithTextArgument("DELETE FROM messages WHERE session_id = ?1",
                                                    sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM context_checkpoints WHERE session_id = ?1", sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM specialist_tasks WHERE owner_session_id = ?1", sessionId);
        ok = ok && clearImportsLocked(sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM session_contexts WHERE session_id=?1", sessionId);
        ok = ok && executeStatementWithTextArgument("DELETE FROM thread_items WHERE thread_id=?1",
                                                    sessionId);
        ok = ok && executeStatementWithTextArgument("DELETE FROM thread_turns WHERE thread_id=?1",
                                                    sessionId);
        clearRecentLocked(sessionId);
        if (ok && executeTransactionStatement("COMMIT")) return true;
        executeTransactionStatement("ROLLBACK");
        return false;
    }

    MaiResult<std::string> getToolModelOutput(const MaiToolModelOutputKey& key) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        return readToolModelOutputLocked(key);
    }

    MaiResult<std::string> saveToolModelOutput(const MaiToolModelOutputKey& key,
                                               const std::string& content) override {
        if (key.model.empty() || key.sourceFingerprint.empty() ||
            content.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return {MaiErrorCode::InvalidInput, "The model tool output key or content is invalid."};
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        const auto fail = [&](MaiError error) -> MaiResult<std::string> {
            executeTransactionStatement("ROLLBACK");
            return error;
        };
        sqlite3_stmt* owner = prepareLocked(
            "SELECT 1 FROM sessions s JOIN messages m ON m.session_id=s.id "
            "JOIN parts p ON p.message_id=m.id "
            "WHERE s.id=?1 AND m.id=?2 AND p.id=?3 AND p.kind=2");
        if (!owner) return fail(mLastWriteError);
        {
            Reset guard(owner);
            bindText(owner, 1, key.sessionId);
            bindText(owner, 2, key.messageId);
            bindText(owner, 3, key.partId);
            const int code = stepLocked(owner);
            if (code == SQLITE_DONE)
                return fail(
                    {MaiErrorCode::InvalidInput, "The model tool output source is invalid."});
            if (code != SQLITE_ROW) {
                noteError("model tool output ownership read failed: " +
                          std::string(sqlite3_errmsg(mDatabase)));
                return fail(mLastWriteError);
            }
        }
        sqlite3_stmt* insert = prepareLocked(
            "INSERT INTO "
            "tool_model_outputs(session_id,message_id,part_id,model,source_fingerprint,content_ref)"
            " VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT DO NOTHING");
        if (!insert) return fail(mLastWriteError);
        {
            Reset guard(insert);
            bindToolModelOutputKey(insert, key);
            bindSourceText(insert, 6, content, key.sessionId, true);
            if (stepLocked(insert) != SQLITE_DONE) {
                noteError("fixed model tool output write failed: " +
                          std::string(sqlite3_errmsg(mDatabase)));
                return fail(mLastWriteError);
            }
        }
        // 读回获胜字节；多个连接的竞争同样受写事务及联合主键保护。
        auto result = readToolModelOutputLocked(key);
        if (!result) return fail(result.error());
        if (!executeTransactionStatement("COMMIT")) return fail(mLastWriteError);
        return result;
    }

    MaiError putToolOutput(const MaiToolOutputRecord& output) override {
        if (const auto error = maiValidateToolOutputRecord(output)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        auto fail = [&](MaiError error) {
            executeTransactionStatement("ROLLBACK");
            return error;
        };
        sqlite3_stmt* owner = prepareLocked(
            "SELECT 1 FROM sessions s JOIN messages m ON m.session_id=s.id "
            "JOIN parts p ON p.message_id=m.id "
            "WHERE s.id=?1 AND m.id=?2 AND p.id=?3 AND p.kind=2 AND p.call_id=?4");
        if (!owner) return fail(mLastWriteError);
        {
            Reset guard(owner);
            bindText(owner, 1, output.sessionId);
            bindText(owner, 2, output.messageId);
            bindText(owner, 3, output.partId);
            bindText(owner, 4, output.callId);
            const int result = stepLocked(owner);
            if (result == SQLITE_DONE)
                return fail({MaiErrorCode::InvalidInput, "Tool output owner is invalid."});
            if (result != SQLITE_ROW) {
                noteError("tool output ownership read failed: " +
                          std::string(sqlite3_errmsg(mDatabase)));
                return fail(mLastWriteError);
            }
        }
        sqlite3_stmt* insert = prepareLocked(
            "INSERT INTO "
            "tool_outputs(id,session_id,message_id,part_id,call_id,payload_ref,truncated,created,"
            "payload_size) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        if (!insert) return fail(mLastWriteError);
        {
            Reset guard(insert);
            bindText(insert, 1, output.id);
            bindText(insert, 2, output.sessionId);
            bindText(insert, 3, output.messageId);
            bindText(insert, 4, output.partId);
            bindText(insert, 5, output.callId);
            // BLOB 不经过 TEXT 编码净化，NUL/非法 UTF-8 字节必须与捕获时完全一致。
            auto reference = mJournal->appendOutput(output.sessionId, output.text);
            if (!reference) return fail(reference.error());
            bindText(insert, 6, reference.value());
            sqlite3_bind_int64(insert, 9, static_cast<sqlite3_int64>(output.text.size()));
            sqlite3_bind_int(insert, 7, output.truncated ? 1 : 0);
            sqlite3_bind_int64(insert, 8, output.created);
            const int result = stepLocked(insert);
            if (result == SQLITE_CONSTRAINT)
                return fail(
                    {MaiErrorCode::InvalidInput, "Tool output immutable ID already exists."});
            if (result != SQLITE_DONE) {
                noteError("tool output write failed: " + std::string(sqlite3_errmsg(mDatabase)));
                return fail(mLastWriteError);
            }
        }
        if (!evictToolOutputsLocked(output.sessionId, false) ||
            !evictToolOutputsLocked(output.sessionId, true))
            return fail(mLastWriteError);
        sqlite3_stmt* retained =
            prepareLocked("SELECT 1 FROM tool_outputs WHERE id=?1 AND payload_ref IS NOT NULL");
        if (!retained) return fail(mLastWriteError);
        {
            Reset guard(retained);
            bindText(retained, 1, output.id);
            const int result = stepLocked(retained);
            if (result == SQLITE_DONE)
                return fail({MaiErrorCode::InvalidInput,
                             "Tool output is older than the retention window."});
            if (result != SQLITE_ROW) {
                noteError("tool output retention read failed: " +
                          std::string(sqlite3_errmsg(mDatabase)));
                return fail(mLastWriteError);
            }
        }
        if (!executeTransactionStatement("COMMIT")) return fail(mLastWriteError);
        return {};
    }

    MaiResult<MaiToolOutputPage> readToolOutput(const std::string& sessionId,
                                                const MaiToolOutputQuery& query) const override {
        if (const auto error = maiValidateToolOutputQuery(sessionId, query)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        // 偏移最多为单条上限，先完成公共校验再做算术；前后各 peek 3 字节足够识别标量边界。
        const std::size_t windowOffset =
            query.offsetBytes - std::min<std::size_t>(3, query.offsetBytes);
        sqlite3_stmt* statement = prepareLocked(
            "SELECT o.message_id,o.part_id,o.call_id,o.truncated,o.payload_size, "
            "mai_output_slice(o.payload_ref,?3-1,?4) FROM tool_outputs o "
            "JOIN sessions s ON s.id=o.session_id "
            "JOIN messages m ON m.id=o.message_id AND m.session_id=o.session_id "
            "JOIN parts p ON p.id=o.part_id AND p.message_id=o.message_id "
            "AND p.kind=2 AND p.call_id=o.call_id "
            "WHERE o.id=?1 AND o.session_id=?2 AND o.payload_ref IS NOT NULL");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, query.outputId);
        bindText(statement, 2, sessionId);
        sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(windowOffset + 1));
        sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(query.maxBytes + 6));
        const int result = stepLocked(statement);
        if (result == SQLITE_DONE)
            return {MaiErrorCode::NotFound, "Retained tool output is unavailable."};
        if (result != SQLITE_ROW) {
            noteError("tool output read failed: " + std::string(sqlite3_errmsg(mDatabase)));
            return mLastWriteError;
        }
        const sqlite3_int64 length = sqlite3_column_int64(statement, 4);
        if (length < 0 || length > static_cast<sqlite3_int64>(kMaiToolOutputMaxBytes))
            return {MaiErrorCode::Internal, "Retained tool output size is invalid."};
        MaiToolOutputRecord output;
        output.id = query.outputId;
        output.sessionId = sessionId;
        output.messageId = textColumn(statement, 0);
        output.partId = textColumn(statement, 1);
        output.callId = textColumn(statement, 2);
        output.truncated = sqlite3_column_int(statement, 3) != 0;
        const void* data = sqlite3_column_blob(statement, 5);
        const int count = sqlite3_column_bytes(statement, 5);
        const std::string_view bytes(count == 0 ? "" : static_cast<const char*>(data),
                                     static_cast<std::size_t>(count));
        return maiPageToolOutput(output, query, bytes, windowOffset,
                                 static_cast<std::size_t>(length));
    }

    MaiError insertSpecialistTask(const MaiSpecialistTask& task) override {
        if (task.id.empty() || task.ownerSessionId.empty() || task.specialistName.empty())
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist task identity");
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO specialist_tasks(id, owner_session_id, specialist_name, parent_task_id, "
            "provider_task_id, intent_ref, context_summary_ref, input_reference, output_path, "
            "created, "
            "status, notified_at) "
            "SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12 "
            "WHERE EXISTS (SELECT 1 FROM sessions WHERE id = ?2) "
            "AND (?4 = '' OR EXISTS (SELECT 1 FROM specialist_tasks WHERE id = ?4 "
            "AND owner_session_id = ?2 AND specialist_name = ?3))");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, task.id);
        bindText(statement, 2, task.ownerSessionId);
        bindText(statement, 3, task.specialistName);
        bindText(statement, 4, task.parentTaskId);
        bindText(statement, 5, task.providerTaskId);
        bindSourceText(statement, 6, task.intent, task.ownerSessionId);
        bindSourceText(statement, 7, task.contextSummary, task.ownerSessionId);
        bindText(statement, 8, task.inputReference);
        bindText(statement, 9, task.outputPath);
        sqlite3_bind_int64(statement, 10, task.created);
        sqlite3_bind_int(
            statement, 11,
            specialistStatusToColumn(task.outputPath.empty() ? MaiSpecialistTaskStatus::Submitted
                                                             : MaiSpecialistTaskStatus::Succeeded));
        sqlite3_bind_int64(statement, 12, task.outputPath.empty() ? 0 : task.created);
        const int result = stepLocked(statement);
        if (result != SQLITE_DONE) {
            if (result == SQLITE_CONSTRAINT)
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "specialist task ID already exists");
            noteError("specialist task write failed: " + std::string(sqlite3_errmsg(mDatabase)));
            return mLastWriteError;
        }
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist task owner, parent, or ID is invalid");
        return {};
    }

    bool getSpecialistTask(const std::string& id, const std::string& ownerSessionId,
                           MaiSpecialistTask& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, owner_session_id, specialist_name, parent_task_id, provider_task_id, "
            "intent, context_summary, input_reference, output_path, created, status, "
            "partial_text, final_text, error_text, last_checked_at, "
            "notification_message_id, notification_attempt_at, notified_at "
            "FROM specialist_tasks WHERE id = ?1 AND owner_session_id = ?2");
        if (!statement) return false;
        Reset guard(statement);
        bindText(statement, 1, id);
        bindText(statement, 2, ownerSessionId);
        if (stepLocked(statement) != SQLITE_ROW) return false;
        out = readSpecialistTaskLocked(statement);
        return true;
    }

    MaiError deleteCompletedSpecialistTask(const std::string& taskId,
                                           const std::string& ownerSessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "DELETE FROM specialist_tasks WHERE id = ?1 AND owner_session_id = ?2 "
            "AND status NOT IN (0, 1) AND notified_at != 0 "
            "AND NOT EXISTS (SELECT 1 FROM specialist_tasks child "
            "WHERE child.parent_task_id = ?1)");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, taskId);
        bindText(statement, 2, ownerSessionId);
        if (stepLocked(statement) != SQLITE_DONE)
            return MaiError::make(MaiErrorCode::Internal, "specialist task deletion failed");
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist task is absent, active, or awaiting delivery");
        return {};
    }

    MaiError appendSpecialistText(const std::string& taskId, const std::string& ownerSessionId,
                                  const std::string& chunk, MaiMillis checkedAt) override {
        if (chunk.size() > 16 * 1024 || checkedAt == 0)
            return {MaiErrorCode::InvalidInput, "Invalid specialist text."};
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        const auto reject = [&](MaiError error) {
            executeTransactionStatement("ROLLBACK");
            return error;
        };
        auto* read = prepareLocked(
            "SELECT partial_text FROM specialist_tasks WHERE id=?1 AND owner_session_id=?2 AND "
            "status IN(0,1)");
        if (!read) return reject(mLastWriteError);
        std::string complete;
        {
            Reset reset(read);
            bindText(read, 1, taskId);
            bindText(read, 2, ownerSessionId);
            if (stepLocked(read) != SQLITE_ROW)
                return reject({MaiErrorCode::InvalidInput, "Specialist task is unavailable."});
            complete = textColumn(read, 0);
        }
        if (complete.size() + chunk.size() > 65536)
            return reject({MaiErrorCode::InvalidInput, "Specialist text is too large."});
        complete += chunk;
        auto* update = prepareLocked(
            "UPDATE specialist_tasks SET partial_text_ref=?3,status=1,last_checked_at=?4 WHERE "
            "id=?1 AND owner_session_id=?2 AND status IN(0,1)");
        if (!update) return reject(mLastWriteError);
        {
            Reset reset(update);
            bindText(update, 1, taskId);
            bindText(update, 2, ownerSessionId);
            bindSourceText(update, 3, complete, ownerSessionId);
            sqlite3_bind_int64(update, 4, checkedAt);
            if (!executePreparedStatement(update)) return reject(mLastWriteError);
        }
        if (!executeTransactionStatement("COMMIT")) return reject(mLastWriteError);
        return {};
    }

    MaiError updateSpecialistProgress(const std::string& taskId, const std::string& ownerSessionId,
                                      MaiMillis checkedAt) override {
        if (checkedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist check time");
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "UPDATE specialist_tasks SET status = 1, last_checked_at = ?3 "
            "WHERE id = ?1 AND owner_session_id = ?2 AND status IN (0, 1)");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, taskId);
        bindText(statement, 2, ownerSessionId);
        sqlite3_bind_int64(statement, 3, checkedAt);
        if (stepLocked(statement) != SQLITE_DONE)
            return MaiError::make(MaiErrorCode::Internal, "specialist progress write failed");
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task is not running");
        return {};
    }

    MaiError recoverStatusCheckFailures() override {
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "UPDATE specialist_tasks SET status = 1, final_text_ref = '', error_text_ref = '', "
            "output_path = '', last_checked_at = 0, notification_message_id = '', "
            "notification_attempt_at = 0, notified_at = 0 "
            "WHERE status = 4 AND provider_task_id != '' "
            "AND instr(error_text, 'Status check failed:') = 1");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        if (stepLocked(statement) != SQLITE_DONE) {
            noteError("specialist status recovery failed: " +
                      std::string(sqlite3_errmsg(mDatabase)));
            return mLastWriteError;
        }
        return {};
    }

    MaiError finishSpecialistTask(const std::string& taskId, const std::string& ownerSessionId,
                                  MaiSpecialistTaskStatus status, const std::string& finalText,
                                  const std::string& outputPath, MaiMillis finishedAt) override {
        if ((status != MaiSpecialistTaskStatus::Succeeded &&
             status != MaiSpecialistTaskStatus::NeedsInput &&
             status != MaiSpecialistTaskStatus::Failed &&
             status != MaiSpecialistTaskStatus::Canceled) ||
            finalText.size() > 64 * 1024 || outputPath.size() > 4096 || finishedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist reply");
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "UPDATE specialist_tasks SET status = ?3, "
            "final_text_ref = CASE WHEN ?4 = '' THEN partial_text_ref ELSE ?4 END, "
            "error_text_ref = CASE WHEN ?3 IN (4, 5) "
            "THEN CASE WHEN ?4 = '' THEN partial_text_ref ELSE ?4 END ELSE '' END, "
            "output_path = ?5, last_checked_at = ?6 "
            "WHERE id = ?1 AND owner_session_id = ?2 AND status IN (0, 1) "
            "AND (?3 != 3 OR ?4 != '' OR partial_text != '' OR ?5 != '')");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, taskId);
        bindText(statement, 2, ownerSessionId);
        sqlite3_bind_int(statement, 3, specialistStatusToColumn(status));
        bindSourceText(statement, 4, finalText, ownerSessionId);
        bindText(statement, 5, outputPath);
        sqlite3_bind_int64(statement, 6, finishedAt);
        if (stepLocked(statement) != SQLITE_DONE)
            return MaiError::make(MaiErrorCode::Internal, "specialist reply write failed");
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task is already finished");
        return {};
    }

    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        if (limit == 0) return tasks;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, owner_session_id, specialist_name, parent_task_id, provider_task_id, "
            "intent, context_summary, input_reference, output_path, created, status, "
            "partial_text, final_text, error_text, last_checked_at, "
            "notification_message_id, notification_attempt_at, notified_at "
            "FROM specialist_tasks WHERE provider_task_id != '' AND status IN (0, 1) "
            "ORDER BY last_checked_at, created LIMIT ?1");
        if (!statement) return tasks;
        Reset guard(statement);
        sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(limit));
        while (stepLocked(statement) == SQLITE_ROW)
            tasks.push_back(readSpecialistTaskLocked(statement));
        return tasks;
    }

    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        if (limit == 0) return tasks;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, owner_session_id, specialist_name, parent_task_id, provider_task_id, "
            "intent, context_summary, input_reference, output_path, created, status, "
            "partial_text, final_text, error_text, last_checked_at, "
            "notification_message_id, notification_attempt_at, notified_at "
            "FROM specialist_tasks WHERE status IN (2, 3, 4, 5) AND notified_at = 0 "
            "ORDER BY last_checked_at, id LIMIT ?1");
        if (!statement) return tasks;
        Reset guard(statement);
        sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(limit));
        while (stepLocked(statement) == SQLITE_ROW)
            tasks.push_back(readSpecialistTaskLocked(statement));
        return tasks;
    }

    MaiResult<std::string> reserveSpecialistNotification(const std::string& taskId,
                                                         const std::string& ownerSessionId,
                                                         const std::string& proposedMessageId,
                                                         MaiMillis attemptedAt) override {
        if (proposedMessageId.empty() || attemptedAt == 0)
            return {MaiErrorCode::InvalidInput, "invalid specialist notification"};
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* update = prepareLocked(
            "UPDATE specialist_tasks SET "
            "notification_message_id = CASE WHEN notification_message_id = '' "
            "THEN ?3 ELSE notification_message_id END, notification_attempt_at = ?4 "
            "WHERE id = ?1 AND owner_session_id = ?2 AND notified_at = 0 "
            "AND status IN (2, 3, 4, 5)");
        if (!update) return mLastWriteError;
        {
            Reset guard(update);
            bindText(update, 1, taskId);
            bindText(update, 2, ownerSessionId);
            bindText(update, 3, proposedMessageId);
            sqlite3_bind_int64(update, 4, attemptedAt);
            if (stepLocked(update) != SQLITE_DONE)
                return {MaiErrorCode::Internal, "specialist notification reservation failed"};
            if (mLastStatementChanges != 1)
                return {MaiErrorCode::NotFound, "specialist reply is unavailable"};
        }
        sqlite3_stmt* read = prepareLocked(
            "SELECT notification_message_id FROM specialist_tasks "
            "WHERE id = ?1 AND owner_session_id = ?2");
        if (!read) return mLastWriteError;
        Reset guard(read);
        bindText(read, 1, taskId);
        bindText(read, 2, ownerSessionId);
        if (stepLocked(read) != SQLITE_ROW)
            return {MaiErrorCode::Internal, "specialist notification vanished"};
        return textColumn(read, 0);
    }

    MaiError markSpecialistNotified(const std::string& taskId, const std::string& ownerSessionId,
                                    const std::string& messageId, MaiMillis notifiedAt) override {
        if (messageId.empty() || notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist notification");
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "UPDATE specialist_tasks SET notified_at = ?4 WHERE id = ?1 "
            "AND owner_session_id = ?2 AND notification_message_id = ?3 AND notified_at = 0");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, taskId);
        bindText(statement, 2, ownerSessionId);
        bindText(statement, 3, messageId);
        sqlite3_bind_int64(statement, 4, notifiedAt);
        if (stepLocked(statement) != SQLITE_DONE)
            return MaiError::make(MaiErrorCode::Internal, "specialist notification write failed");
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::NotFound, "specialist notification unavailable");
        return {};
    }

    MaiError markSpecialistResultAlreadyDelivered(const std::string& taskId,
                                                  const std::string& ownerSessionId,
                                                  MaiMillis notifiedAt) override {
        if (notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist delivery time");
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "UPDATE specialist_tasks SET notified_at = ?3 WHERE id = ?1 AND owner_session_id = ?2 "
            "AND status = 3 AND provider_task_id != '' AND notified_at = 0");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindText(statement, 1, taskId);
        bindText(statement, 2, ownerSessionId);
        sqlite3_bind_int64(statement, 3, notifiedAt);
        if (stepLocked(statement) != SQLITE_DONE)
            return MaiError::make(MaiErrorCode::Internal,
                                  "specialist delivery deduplication failed");
        if (mLastStatementChanges != 1)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist result cannot be deduplicated");
        return {};
    }

    MaiResult<MaiMessage> readTurnMetadata(const std::string& sessionId,
                                           const std::string& turnId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        const auto read = [&](const std::string& id) -> MaiResult<MaiMessage> {
            auto* statement = prepareLocked(
                id.empty() ? "SELECT id,role,created,completed,turn_state FROM messages WHERE "
                             "session_id=?1 "
                             "AND role=1 AND context_import_id IS NULL ORDER BY id DESC LIMIT 1"
                           : "SELECT id,role,created,completed,turn_state FROM messages WHERE "
                             "session_id=?1 "
                             "AND id=?2 AND context_import_id IS NULL");
            if (!statement) return mLastWriteError;
            Reset guard(statement);
            bindText(statement, 1, sessionId);
            if (!id.empty()) bindText(statement, 2, id);
            const int code = stepLocked(statement);
            if (code == SQLITE_DONE) return {MaiErrorCode::NotFound, "Turn not found."};
            if (code != SQLITE_ROW) return {MaiErrorCode::Internal, "Cannot read turn metadata."};
            MaiMessage result;
            result.id = textColumn(statement, 0);
            result.role = columnToRole(sqlite3_column_int(statement, 1));
            result.created = sqlite3_column_int64(statement, 2);
            result.completed = sqlite3_column_int64(statement, 3);
            restoreTurnState(textColumn(statement, 4), result);
            return result;
        };
        auto result = read(turnId);
        if (result && turnId.empty() && !result.value().turnId.empty() &&
            result.value().turnId != result.value().id)
            return read(result.value().turnId);
        return result;
    }

    MaiError updateTurnContext(const std::string& sessionId, const std::string& turnId,
                               const MaiContextStatus& status) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        MaiMessage message;
        message.id = turnId;
        message.role = MaiRole::Assistant;
        auto* read = prepareLocked(
            "SELECT role,turn_state FROM messages WHERE session_id=?1 AND id=?2 "
            "AND context_import_id IS NULL");
        if (!read) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        bool found = false;
        {
            Reset guard(read);
            bindText(read, 1, sessionId);
            bindText(read, 2, turnId);
            if (stepLocked(read) == SQLITE_ROW && sqlite3_column_int(read, 0) == 1) {
                found = true;
                restoreTurnState(textColumn(read, 1), message);
            }
        }
        if (!found || (!message.turnId.empty() && message.turnId != turnId)) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::NotFound, "An originating assistant turn is required."};
        }
        message.turnId = turnId;
        if (!message.turnOutcome || message.turnOutcome->turnId != turnId) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::NotSupported, "This message is not a recorded turn."};
        }
        if (message.turnOutcome->status != MaiTurnStatus::Running) {
            executeTransactionStatement("ROLLBACK");
            return {MaiErrorCode::Busy, "The originating turn is already terminal."};
        }
        message.turnOutcome->context = status;
        auto* update =
            prepareLocked("UPDATE messages SET turn_state_ref=?1 WHERE session_id=?2 AND id=?3");
        if (!update) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        bool saved;
        {
            Reset guard(update);
            bindSourceText(update, 1, turnStateJson(message), sessionId);
            bindText(update, 2, sessionId);
            bindText(update, 3, turnId);
            saved = executePreparedStatement(update);
        }
        if (saved) {
            if (auto error = mJournal->projectMessage(sessionId, message)) {
                noteError(error.message());
                saved = false;
            }
        }
        if (!saved || !executeTransactionStatement("COMMIT")) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        auto cached = mRecentMessages.find({sessionId, turnId});
        if (cached != mRecentMessages.end()) {
            cached->second.turnId = message.turnId;
            cached->second.turnOutcome = message.turnOutcome;
        }
        return {};
    }

    void putMessage(const std::string& sessionId, const MaiMessage& snapshot) override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto key = std::make_pair(sessionId, snapshot.id);
        MaiMessage display = snapshot;
        display.contextImportOrigin.reset();
        bool visible = true;
        // 来源只由当前已发布且被选中的账本恢复。调用方快照、完成事件和内存缓存都不能授予来源。
        auto* origin = prepareLocked(
            "SELECT "
            "m.session_id,m.context_import_id,i.source_key,m.context_source_id,EXISTS(SELECT 1 "
            "FROM context_import_sources s WHERE s.session_id=m.session_id AND "
            "s.import_id=m.context_import_id) FROM messages m LEFT JOIN context_imports i ON "
            "i.id=m.context_import_id AND i.session_id=m.session_id AND i.state=1 WHERE m.id=?1");
        if (!origin) return;
        {
            Reset reset(origin);
            bindText(origin, 1, snapshot.id);
            int code = stepLocked(origin);
            if (code == SQLITE_ROW) {
                if (textColumn(origin, 0) != sessionId) {
                    noteError("Message belongs to another session.");
                    return;
                }
                const bool imported = sqlite3_column_type(origin, 1) != SQLITE_NULL;
                visible = !imported ||
                          (!textColumn(origin, 2).empty() && sqlite3_column_int(origin, 4) != 0);
                if (imported && visible)
                    display.contextImportOrigin =
                        MaiContextImportOrigin{textColumn(origin, 2), textColumn(origin, 3)};
            } else if (code != SQLITE_DONE)
                return;
        }
        MaiMessage message = display;
        message.parts.erase(
            std::remove_if(message.parts.begin(), message.parts.end(),
                           [](const MaiMessagePart& part) { return part.modelDraft; }),
            message.parts.end());
        auto encoded = MaiSessionRecordCodec::encodeMessage(message);
        if (!encoded) {
            noteError(encoded.error().message());
            return;
        }
        auto previous = mRecentPersisted.find(key);
        if (previous != mRecentPersisted.end() && previous->second == encoded.value()) {
            if (visible)
                mRecentMessages[key] = display;
            else
                mRecentMessages.erase(key);
            return;
        }
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return;

        // context_* 只能由导入事务建立；公开快照中的只读 origin 不参与普通写入。
        sqlite3_stmt* upsert = prepareLocked(
            "INSERT INTO messages(id, session_id, role, created, completed, input_request_id, "
            "pending_input,turn_state_ref) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) ON CONFLICT(id) "
            "DO UPDATE SET role=excluded.role, created=excluded.created, "
            "completed=excluded.completed, input_request_id=excluded.input_request_id, "
            "pending_input=excluded.pending_input,turn_state_ref=excluded.turn_state_ref");
        if (upsert) {
            Reset guard(upsert);
            bindText(upsert, 1, message.id);
            bindText(upsert, 2, sessionId);
            sqlite3_bind_int(upsert, 3, roleToColumn(message.role));
            sqlite3_bind_int64(upsert, 4, message.created);
            sqlite3_bind_int64(upsert, 5, message.completed);
            bindText(upsert, 6, message.inputRequestId);
            sqlite3_bind_int(upsert, 7, message.pendingInput ? 1 : 0);
            bindSourceText(upsert, 8, turnStateJson(message), sessionId);
            executePreparedStatement(upsert);
        }

        // part 整体重写而不是逐条 diff：一条消息的 part 个数是个位数，
        // 而 diff 要处理"改了/删了/顺序变了"三种情况，不值得。
        // 新草稿仅进入有界展示缓存；只有初始元数据和已完成片段进入本事务。
        executeStatementWithTextArgument("DELETE FROM parts WHERE message_id = ?1", message.id);
        for (const auto& part : message.parts) writePartLocked(message.id, part);

        executeStatementWithTextArgument(
            "DELETE FROM tool_model_outputs WHERE message_id=?1 AND NOT EXISTS "
            "(SELECT 1 FROM parts p WHERE p.message_id=tool_model_outputs.message_id "
            "AND p.id=tool_model_outputs.part_id AND p.kind=2)",
            message.id);

        if (!mLastWriteError && visible) {
            if (auto error = mJournal->projectMessage(sessionId, message))
                noteError(error.message());
        }
        if (mLastWriteError) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        if (executeTransactionStatement("COMMIT")) {
            if (mRecentMessages.size() >= 16 && !mRecentMessages.count(key)) {
                mRecentMessages.erase(mRecentMessages.begin());
                mRecentPersisted.erase(mRecentPersisted.begin());
            }
            if (visible)
                mRecentMessages[key] = display;
            else
                mRecentMessages.erase(key);
            mRecentPersisted[key] = encoded.value();
        }
    }

    std::vector<MaiMessage> listMessages(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        return listMessagesLocked(sessionId, std::numeric_limits<std::size_t>::max());
    }

    bool getContextCheckpoint(const std::string& sessionId,
                              MaiContextCheckpoint& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        return readContextCheckpointLocked(sessionId, out);
    }

    MaiError advanceContextCheckpoint(const std::string& sessionId, std::uint64_t expectedRevision,
                                      const MaiContextCheckpoint& checkpoint) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return mLastWriteError;
        const auto reject = [&](MaiErrorCode code, const char* message) {
            executeTransactionStatement("ROLLBACK");
            return MaiError::make(code, message);
        };
        MaiContextCheckpoint previous;
        const std::uint64_t revision =
            readContextCheckpointLocked(sessionId, previous) ? previous.revision : 0;
        if (revision != expectedRevision || checkpoint.revision == 0 ||
            checkpoint.revision - 1 != expectedRevision ||
            checkpoint.revision >
                static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            return reject(MaiErrorCode::Busy, "Context checkpoint changed during compaction.");
        // 写锁覆盖校验与提交；允许在源边界后追加新消息，但不接受旧片段的内容变化。
        if (!checkpoint.partialMessageId.empty() &&
            checkpoint.sourceMessageCount == std::numeric_limits<std::size_t>::max())
            return reject(MaiErrorCode::InvalidInput, "Invalid partial context boundary.");
        const auto source =
            listMessagesLocked(sessionId, checkpoint.sourceMessageCount +
                                              (checkpoint.partialMessageId.empty() ? 0 : 1));
        if (!maiContextCheckpointMatches(checkpoint, sessionId, source))
            return reject(MaiErrorCode::Busy,
                          "Context source changed while preparing its summary.");
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO "
            "context_checkpoints(session_id,revision,source_count,first_message_id,through_message_"
            "id,source_fingerprint,model,summary_ref,reference_text_ref,created,partial_message_id,"
            "partial_part_count,through_part_id,partial_source_fingerprint,preserved_user_message_"
            "id) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15) ON "
            "CONFLICT(session_id) DO UPDATE SET "
            "revision=excluded.revision,source_count=excluded.source_count,first_message_id="
            "excluded.first_message_id,through_message_id=excluded.through_message_id,source_"
            "fingerprint=excluded.source_fingerprint,model=excluded.model,summary_ref=excluded."
            "summary_ref,reference_text_ref=excluded.reference_text_ref,created=excluded.created,"
            "partial_message_id=excluded.partial_message_id,partial_part_count=excluded.partial_"
            "part_count,through_part_id=excluded.through_part_id,partial_source_fingerprint="
            "excluded.partial_source_fingerprint,preserved_user_message_id=excluded.preserved_user_"
            "message_id");
        if (!statement) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        bool saved = false;
        {
            Reset guard(statement);
            bindText(statement, 1, sessionId);
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(checkpoint.revision));
            sqlite3_bind_int64(statement, 3,
                               static_cast<sqlite3_int64>(checkpoint.sourceMessageCount));
            bindText(statement, 4, checkpoint.firstMessageId);
            bindText(statement, 5, checkpoint.throughMessageId);
            bindText(statement, 6, checkpoint.sourceFingerprint);
            bindText(statement, 7, checkpoint.model);
            bindSourceText(statement, 8, checkpoint.summary, sessionId);
            bindSourceText(statement, 9, checkpoint.references, sessionId);
            sqlite3_bind_int64(statement, 10, checkpoint.created);
            bindText(statement, 11, checkpoint.partialMessageId);
            sqlite3_bind_int64(statement, 12,
                               static_cast<sqlite3_int64>(checkpoint.partialPartCount));
            bindText(statement, 13, checkpoint.throughPartId);
            bindText(statement, 14, checkpoint.partialSourceFingerprint);
            bindText(statement, 15, checkpoint.preservedUserMessageId);
            saved = stepLocked(statement) == SQLITE_DONE;
        }
        if (!saved) return reject(MaiErrorCode::Internal, "Cannot save context checkpoint.");
        if (!executeTransactionStatement("COMMIT")) {
            executeTransactionStatement("ROLLBACK");
            return mLastWriteError;
        }
        return {};
    }

private:
    // 导入接口直接返回事务错误，不让已处理且回滚的导入失败污染后续普通轮次。
    // 进入前已存在的普通写入错误仍保留。
    class ImportErrorScope {
    public:
        explicit ImportErrorScope(MaiError& target) : mTarget(target), mBefore(target) {}
        ~ImportErrorScope() {
            mTarget = mBefore;
        }

    private:
        MaiError& mTarget;
        MaiError mBefore;
    };

    bool appendMessagePageLocked(std::vector<MaiMessage>& page, const std::string& sessionId,
                                 const std::string& beforeId, std::size_t limit,
                                 const std::string& importId) const {
        // 两个索引分开查询，隐藏的旧快照再大也不影响普通消息或当前快照的 keyset 分页。
        auto* statement = prepareLocked(
            importId.empty()
                ? "SELECT "
                  "id,role,created,completed,input_request_id,pending_input,NULL,NULL,turn_state "
                  "FROM "
                  "messages "
                  "WHERE session_id=?1 AND context_import_id IS NULL AND id<?2 ORDER BY id DESC "
                  "LIMIT ?3"
                : "SELECT m.id,m.role,m.created,m.completed,m.input_request_id,m.pending_input,"
                  "i.source_key,m.context_source_id,m.turn_state FROM messages m JOIN "
                  "context_imports i "
                  "ON i.id=m.context_import_id AND i.session_id=m.session_id AND i.state=1 "
                  "WHERE m.context_import_id=?1 AND m.id<?2 ORDER BY m.id DESC LIMIT ?3");
        if (!statement) return false;
        Reset reset(statement);
        bindText(statement, 1, importId.empty() ? sessionId : importId);
        bindText(statement, 2, beforeId.empty() ? std::string(1, '\x7f') : beforeId);
        sqlite3_bind_int64(
            statement, 3,
            static_cast<sqlite3_int64>(std::min(limit, static_cast<std::size_t>(INT64_MAX))));
        int code = SQLITE_ROW;
        while ((code = stepLocked(statement)) == SQLITE_ROW) {
            MaiMessage message;
            message.id = textColumn(statement, 0);
            message.role = columnToRole(sqlite3_column_int(statement, 1));
            message.created = sqlite3_column_int64(statement, 2);
            message.completed = sqlite3_column_int64(statement, 3);
            message.inputRequestId = textColumn(statement, 4);
            message.pendingInput = sqlite3_column_int(statement, 5) != 0;
            restoreImportOrigin(statement, message);
            page.push_back(std::move(message));
        }
        return code == SQLITE_DONE;
    }

    bool projectVisibleImportLocked(const MaiContextImportProgress& progress) {
        // 只删除同一来源已被替代的原生行；原始正文、扩展消息和来源账本继续保留。
        auto* retire = prepareLocked(
            "DELETE FROM thread_items WHERE thread_id=?1 AND item_id IN (SELECT m.id FROM messages "
            "m JOIN context_imports i ON i.id=m.context_import_id WHERE m.session_id=?1 AND "
            "i.source_key=?2 AND i.id<>?3 UNION SELECT p.id FROM parts p JOIN messages m ON "
            "m.id=p.message_id JOIN context_imports i ON i.id=m.context_import_id WHERE "
            "m.session_id=?1 AND i.source_key=?2 AND i.id<>?3)");
        if (!retire) return false;
        {
            Reset reset(retire);
            bindText(retire, 1, progress.sessionId);
            bindText(retire, 2, progress.sourceKey);
            bindText(retire, 3, progress.importId);
            if (!executePreparedStatement(retire)) return false;
        }
        std::string before;
        for (;;) {
            std::vector<MaiMessage> page;
            if (!appendMessagePageLocked(page, progress.sessionId, before, 128, progress.importId))
                return false;
            if (page.empty()) break;
            before = page.back().id;
            for (auto& message : page) {
                bool succeeded = false;
                message.parts = readPartsLocked(message.id, &succeeded);
                if (!succeeded) return false;
                if (auto error = mJournal->projectMessage(progress.sessionId, message)) {
                    noteError(error.message());
                    return false;
                }
            }
        }
        return true;
    }

    MaiError importErrorLocked() const {
        return {MaiErrorCode::Internal,
                "Context import storage failed: " + std::string(sqlite3_errmsg(mDatabase))};
    }

    MaiError rollbackImportLocked() {
        const auto error = importErrorLocked();
        executeTransactionStatement("ROLLBACK");
        return error;
    }

    MaiResult<MaiContextImportProgress> inspectImportLocked(const std::string& importId) const {
        auto* query = prepareLocked(
            "SELECT "
            "i.id,i.session_id,i.source_key,i.snapshot_id,i.next_sequence,i.record_count,i.state "
            "FROM context_imports i JOIN sessions s ON s.id=i.session_id WHERE i.id=?1 AND "
            "i.state<>2");
        if (!query) return importErrorLocked();
        Reset reset(query);
        bindText(query, 1, importId);
        const int code = stepLocked(query);
        if (code == SQLITE_DONE) return {MaiErrorCode::NotFound, "Context import is unavailable."};
        if (code != SQLITE_ROW) return importErrorLocked();
        return MaiContextImportProgress{textColumn(query, 0),
                                        textColumn(query, 1),
                                        textColumn(query, 2),
                                        textColumn(query, 3),
                                        static_cast<std::uint64_t>(sqlite3_column_int64(query, 4)),
                                        static_cast<std::uint64_t>(sqlite3_column_int64(query, 5)),
                                        sqlite3_column_int(query, 6) == 1};
    }

    bool writeImportPartLocked(const std::string& messageId, const MaiMessagePart& part) {
        auto* insert = prepareLocked(
            "INSERT INTO parts(id,message_id,kind,created,text_ref) VALUES(?1,?2,0,?3,?4)");
        if (!insert) return false;
        Reset reset(insert);
        bindText(insert, 1, part.id);
        bindText(insert, 2, messageId);
        sqlite3_bind_int64(insert, 3, part.created);
        bindSourceText(insert, 4, std::get<MaiTextPart>(part.body).text,
                       sessionForMessageLocked(messageId));
        return executePreparedStatement(insert);
    }

    bool clearImportsLocked(const std::string& sessionId) {
        bool ok = executeStatementWithTextArgument(
            "DELETE FROM context_import_chunks WHERE import_id IN (SELECT id FROM context_imports "
            "WHERE session_id=?1)",
            sessionId);
        ok = ok && executeStatementWithTextArgument(
                       "DELETE FROM context_import_sources WHERE session_id=?1", sessionId);
        return ok && executeStatementWithTextArgument(
                         "DELETE FROM context_imports WHERE session_id=?1", sessionId);
    }

    std::vector<MaiMessage> listMessagesLocked(const std::string& sessionId,
                                               std::size_t limit) const {
        std::vector<MaiMessage> out;

        sqlite3_stmt* statement = prepareLocked(
            "SELECT m.id,m.role,m.created,m.completed,m.input_request_id,m.pending_input,"
            "i.source_key,m.context_source_id,m.turn_state FROM messages m LEFT JOIN "
            "context_imports i "
            "ON i.id=m.context_import_id AND i.session_id=m.session_id AND i.state=1 "
            "WHERE m.session_id = ?1 AND (m.context_import_id IS NULL OR "
            "(i.id IS NOT NULL AND m.context_import_id IN "
            "(SELECT import_id FROM context_import_sources WHERE session_id=?1))) ORDER BY m.id "
            "LIMIT "
            "?2");
        if (!statement) return out;
        {
            Reset guard(statement);
            bindText(statement, 1, sessionId);
            sqlite3_bind_int64(
                statement, 2,
                static_cast<sqlite3_int64>(std::min(
                    limit, static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max()))));
            while (stepLocked(statement) == SQLITE_ROW) {
                MaiMessage message;
                message.id = textColumn(statement, 0);
                message.role = columnToRole(sqlite3_column_int(statement, 1));
                message.created = sqlite3_column_int64(statement, 2);
                message.completed = sqlite3_column_int64(statement, 3);
                message.inputRequestId = textColumn(statement, 4);
                message.pendingInput = sqlite3_column_int(statement, 5) != 0;
                restoreImportOrigin(statement, message);
                out.push_back(std::move(message));
            }
        }
        for (auto& message : out) {
            bool succeeded = false;
            message.parts = readPartsLocked(message.id, &succeeded);
            if (!succeeded) return {};
            mergeDraftPartsLocked(sessionId, message);
        }
        return out;
    }

    bool readContextCheckpointLocked(const std::string& sessionId,
                                     MaiContextCheckpoint& out) const {
        sqlite3_stmt* statement = prepareLocked(
            "SELECT revision,source_count,first_message_id,through_message_id,source_fingerprint,"
            "model,summary,reference_text,created,partial_message_id,partial_part_count,through_"
            "part_id,"
            "partial_source_fingerprint,preserved_user_message_id FROM context_checkpoints "
            "WHERE session_id=?1 AND EXISTS(SELECT 1 FROM sessions WHERE id=?1)");
        if (!statement) return false;
        Reset guard(statement);
        bindText(statement, 1, sessionId);
        if (stepLocked(statement) != SQLITE_ROW) return false;
        MaiContextCheckpoint checkpoint;
        checkpoint.sessionId = sessionId;
        checkpoint.revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
        checkpoint.sourceMessageCount =
            static_cast<std::size_t>(sqlite3_column_int64(statement, 1));
        checkpoint.firstMessageId = textColumn(statement, 2);
        checkpoint.throughMessageId = textColumn(statement, 3);
        checkpoint.sourceFingerprint = textColumn(statement, 4);
        checkpoint.model = textColumn(statement, 5);
        checkpoint.summary = textColumn(statement, 6);
        checkpoint.references = textColumn(statement, 7);
        checkpoint.created = sqlite3_column_int64(statement, 8);
        checkpoint.partialMessageId = textColumn(statement, 9);
        checkpoint.partialPartCount = static_cast<std::size_t>(sqlite3_column_int64(statement, 10));
        checkpoint.throughPartId = textColumn(statement, 11);
        checkpoint.partialSourceFingerprint = textColumn(statement, 12);
        checkpoint.preservedUserMessageId = textColumn(statement, 13);
        out = std::move(checkpoint);
        return true;
    }

public:
    bool getMessage(const std::string& sessionId, const std::string& messageId,
                    MaiMessage& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        if (!maiIsBlockingAllowed()) {
            auto cached = mRecentMessages.find({sessionId, messageId});
            if (cached == mRecentMessages.end()) {
                mLastReadError = {MaiErrorCode::Busy,
                                  "Message snapshot is not in the event overlay."};
                return false;
            }
            out = cached->second;
            return true;
        }
        sqlite3_stmt* statement = prepareLocked(
            "SELECT m.id,m.role,m.created,m.completed,m.input_request_id,m.pending_input,"
            "i.source_key,m.context_source_id,m.turn_state FROM messages m LEFT JOIN "
            "context_imports i "
            "ON i.id=m.context_import_id AND i.session_id=m.session_id AND i.state=1 "
            "WHERE m.session_id = ?1 AND m.id = ?2 AND (m.context_import_id IS NULL OR "
            "(i.id IS NOT NULL AND m.context_import_id IN "
            "(SELECT import_id FROM context_import_sources WHERE session_id=?1)))");
        if (!statement) return false;
        MaiMessage message;
        {
            Reset guard(statement);
            bindText(statement, 1, sessionId);
            bindText(statement, 2, messageId);
            if (stepLocked(statement) != SQLITE_ROW) return false;
            message.id = textColumn(statement, 0);
            message.role = columnToRole(sqlite3_column_int(statement, 1));
            message.created = sqlite3_column_int64(statement, 2);
            message.completed = sqlite3_column_int64(statement, 3);
            message.inputRequestId = textColumn(statement, 4);
            message.pendingInput = sqlite3_column_int(statement, 5) != 0;
            restoreImportOrigin(statement, message);
        }
        // 仅对精确命中的消息加载片段；失败路径不修改调用方原有快照。
        bool partsRead = false;
        message.parts = readPartsLocked(message.id, &partsRead);
        if (!partsRead) return false;
        mergeDraftPartsLocked(sessionId, message);
        out = std::move(message);
        return true;
    }

    std::vector<MaiMessage> listMessagesPage(const std::string& sessionId,
                                             const std::string& beforeId,
                                             std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        std::vector<MaiMessage> page;
        if (limit == 0) return page;
        if (!appendMessagePageLocked(page, sessionId, beforeId, limit, {})) return {};
        auto* sources =
            prepareLocked("SELECT import_id FROM context_import_sources WHERE session_id=?1");
        if (!sources) return {};
        {
            Reset reset(sources);
            bindText(sources, 1, sessionId);
            int code = SQLITE_ROW;
            while ((code = stepLocked(sources)) == SQLITE_ROW)
                if (!appendMessagePageLocked(page, sessionId, beforeId, limit,
                                             textColumn(sources, 0)))
                    return {};
            if (code != SQLITE_DONE) return {};
        }
        // 每个来源只读 limit 个候选，不扫描或排序整份历史快照。
        std::sort(page.begin(), page.end(),
                  [](const auto& left, const auto& right) { return left.id > right.id; });
        if (page.size() > limit) page.resize(limit);
        for (auto& message : page) {
            bool succeeded = false;
            message.parts = readPartsLocked(message.id, &succeeded);
            if (!succeeded) return {};
            mergeDraftPartsLocked(sessionId, message);
        }
        return page;
    }

    bool getInputMessage(const std::string& sessionId, const std::string& requestId,
                         MaiMessage& out) const override {
        if (requestId.empty()) return false;
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        auto* query = prepareLocked(
            "SELECT id, role, created, completed, input_request_id, pending_input "
            "FROM messages WHERE session_id=?1 AND input_request_id=?2 AND role=0");
        if (!query) return false;
        Reset guard(query);
        bindText(query, 1, sessionId);
        bindText(query, 2, requestId);
        if (stepLocked(query) != SQLITE_ROW) return false;
        out.id = textColumn(query, 0);
        out.role = columnToRole(sqlite3_column_int(query, 1));
        out.created = sqlite3_column_int64(query, 2);
        out.completed = sqlite3_column_int64(query, 3);
        out.inputRequestId = textColumn(query, 4);
        out.pendingInput = sqlite3_column_int(query, 5) != 0;
        // 接收凭据属于普通实时输入，复用输出对象时不能残留上一条导入消息的来源。
        out.contextImportOrigin.reset();
        out.turnOutcome.reset();
        out.turnId.clear();
        out.parts = readPartsLocked(out.id);
        return true;
    }

    std::vector<MaiMessage> listQueuedInputs(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastReadError = {};
        std::vector<MaiMessage> result;
        auto* query = prepareLocked(
            "SELECT id, role, created, completed, input_request_id, pending_input "
            "FROM messages WHERE session_id=?1 AND pending_input=1 AND role=0 ORDER BY id");
        if (!query) return result;
        {
            Reset guard(query);
            bindText(query, 1, sessionId);
            while (stepLocked(query) == SQLITE_ROW) {
                MaiMessage message;
                message.id = textColumn(query, 0);
                message.role = columnToRole(sqlite3_column_int(query, 1));
                message.created = sqlite3_column_int64(query, 2);
                message.completed = sqlite3_column_int64(query, 3);
                message.inputRequestId = textColumn(query, 4);
                message.pendingInput = sqlite3_column_int(query, 5) != 0;
                result.push_back(std::move(message));
            }
        }
        for (auto& message : result) message.parts = readPartsLocked(message.id);
        return result;
    }

    void recoverInterruptedTools(const std::string& error, std::int64_t completedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return;

        sqlite3_stmt* messages = prepareLocked(
            "UPDATE messages SET completed = ?1 WHERE completed = 0 AND id IN (SELECT message_id "
            "FROM parts WHERE kind = 2 AND state IN (0, 1))");
        if (!messages) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        bool updatedMessages = false;
        {
            Reset guard(messages);
            sqlite3_bind_int64(messages, 1, completedAt);
            updatedMessages = stepLocked(messages) == SQLITE_DONE;
        }
        if (!updatedMessages) {
            noteError("interrupted message recovery failed: " +
                      std::string(sqlite3_errmsg(mDatabase)));
            executeTransactionStatement("ROLLBACK");
            return;
        }

        sqlite3_stmt* parts = prepareLocked(
            "SELECT id,state,execution FROM parts "
            "WHERE kind = 2 AND state IN (0, 1)");
        if (!parts) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        // 只读取未完成工具的元数据，保留其原始 output 以及已获知的输出/任务句柄。
        std::vector<std::pair<std::string, std::string>> recovered;
        bool readParts = false;
        {
            Reset guard(parts);
            int result = SQLITE_ROW;
            while ((result = stepLocked(parts)) == SQLITE_ROW) {
                const bool pending = sqlite3_column_int(parts, 1) == 0;
                MaiToolExecution execution;
                maiParseToolExecution(textColumn(parts, 2), execution);
                execution.status =
                    pending ? MaiToolExecutionStatus::NotStarted : MaiToolExecutionStatus::Unknown;
                execution.effectStatus =
                    pending ? MaiToolEffectStatus::None : MaiToolEffectStatus::Unknown;
                execution.nextAction = pending ? MaiToolNextAction::None
                                       : !execution.taskId.empty()
                                           ? MaiToolNextAction::QueryStatus
                                           : MaiToolNextAction::InspectState;
                execution.exitCode.reset();
                if (pending) {
                    execution.started = false;
                    execution.timedOut = false;
                    execution.canceled = false;
                } else if (!execution.started.value_or(false))
                    execution.started.reset();
                auto recoveredMetadata =
                    nlohmann::json::parse(maiSerializeToolExecution(execution));
                const auto originalMetadata =
                    nlohmann::json::parse(textColumn(parts, 2), nullptr, false);
                if (originalMetadata.is_object() &&
                    originalMetadata.contains("model_output_version"))
                    recoveredMetadata["model_output_version"] =
                        originalMetadata["model_output_version"];
                recovered.emplace_back(textColumn(parts, 0), recoveredMetadata.dump());
            }
            readParts = result == SQLITE_DONE;
        }
        if (!readParts) {
            noteError("interrupted tool recovery failed: " +
                      std::string(sqlite3_errmsg(mDatabase)));
            executeTransactionStatement("ROLLBACK");
            return;
        }
        sqlite3_stmt* update = prepareLocked(
            "UPDATE parts SET state=3,error_ref=?1,execution_ref=?2 WHERE id=?3 AND kind=2 AND "
            "state IN (0,1)");
        if (!update) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        for (const auto& entry : recovered) {
            Reset guard(update);
            bindSourceText(update, 1, error, sessionForPartLocked(entry.first));
            bindSourceText(update, 2, entry.second, sessionForPartLocked(entry.first));
            bindText(update, 3, entry.first);
            if (!executePreparedStatement(update)) {
                executeTransactionStatement("ROLLBACK");
                return;
            }
        }
        // 即使没有工具，遗留的模型轮次也已失去原工作线程。只修复事实，不重发请求。
        auto* turns = prepareLocked(
            "SELECT id,turn_state,created,completed FROM messages WHERE role=1 AND "
            "turn_state IS NOT NULL AND turn_state<>''");
        if (!turns) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        std::vector<std::pair<std::string, std::string>> interrupted;
        std::vector<MaiMessage> interruptedTurns;
        {
            Reset guard(turns);
            int code;
            while ((code = stepLocked(turns)) == SQLITE_ROW) {
                MaiMessage message;
                message.id = textColumn(turns, 0);
                message.role = MaiRole::Assistant;
                message.created = sqlite3_column_int64(turns, 2);
                message.completed = sqlite3_column_int64(turns, 3) == 0
                                        ? completedAt
                                        : sqlite3_column_int64(turns, 3);
                restoreTurnState(textColumn(turns, 1), message);
                if (!message.turnOutcome || message.turnOutcome->turnId != message.id ||
                    message.turnOutcome->status != MaiTurnStatus::Running)
                    continue;
                message.turnOutcome->status = MaiTurnStatus::Interrupted;
                message.turnOutcome->completed = completedAt;
                message.turnOutcome->error = {MaiErrorCode::Canceled, error};
                if (message.turnOutcome->context &&
                    (message.turnOutcome->context->phase == MaiContextPhase::Preparing ||
                     message.turnOutcome->context->phase == MaiContextPhase::Summarizing)) {
                    message.turnOutcome->context->phase = MaiContextPhase::Interrupted;
                    message.turnOutcome->context->error = {MaiErrorCode::Canceled, error};
                }
                interrupted.emplace_back(message.id, turnStateJson(message));
                interruptedTurns.push_back(message);
            }
            if (code != SQLITE_DONE) {
                noteError("interrupted turn recovery read failed");
                executeTransactionStatement("ROLLBACK");
                return;
            }
        }
        auto* updateTurn = prepareLocked(
            "UPDATE messages SET turn_state_ref=?1,completed=CASE WHEN completed=0 THEN ?2 ELSE "
            "completed END WHERE id=?3");
        if (!updateTurn) {
            executeTransactionStatement("ROLLBACK");
            return;
        }
        for (const auto& entry : interrupted) {
            Reset guard(updateTurn);
            bindSourceText(updateTurn, 1, entry.second, sessionForMessageLocked(entry.first));
            sqlite3_bind_int64(updateTurn, 2, completedAt);
            bindText(updateTurn, 3, entry.first);
            if (!executePreparedStatement(updateTurn)) {
                executeTransactionStatement("ROLLBACK");
                return;
            }
        }
        for (const auto& turn : interruptedTurns)
            if (auto error = mJournal->projectMessage(sessionForMessageLocked(turn.id), turn)) {
                noteError(error.message());
                executeTransactionStatement("ROLLBACK");
                return;
            }
        if (!executeTransactionStatement("COMMIT")) executeTransactionStatement("ROLLBACK");
        mRecentMessages.clear();
        mRecentPersisted.clear();
    }

    MaiError lastWriteError() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mLastWriteError;
    }

    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutator) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!executeTransactionStatement("BEGIN IMMEDIATE")) return false;

        MaiSession session;
        bool found = false;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated, parent_id, depth, "
            "title_edited, reasoning_effort "
            "FROM sessions WHERE id = ?1");
        if (statement) {
            Reset guard(statement);
            bindText(statement, 1, id);
            if (stepLocked(statement) == SQLITE_ROW) {
                session = readSession(statement);
                found = true;
            }
        }
        if (!found) {
            executeTransactionStatement("ROLLBACK");
            return false;
        }

        mutator(session);
        writeSessionLocked(session);
        executeTransactionStatement("COMMIT");
        return true;
    }

private:
    // sqlite3_stmt 用完必须 reset，否则下次 bind 会接着上一次的状态，
    // 而且会一直占着读锁。用 RAII 保证提前 return 时也复位。
    struct Reset {
        sqlite3_stmt* statement;
        explicit Reset(sqlite3_stmt* s) : statement(s) {}
        ~Reset() {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
    };

    // 调用方持有写事务；只读取长度与编号，淘汰时置 NULL 保留不可复用编号墓碑。
    bool evictToolOutputsLocked(const std::string& sessionId, bool global) {
        sqlite3_stmt* sum =
            prepareLocked(global ? "SELECT COALESCE(SUM(payload_size),0) FROM tool_outputs "
                                   "WHERE payload_ref IS NOT NULL"
                                 : "SELECT COALESCE(SUM(payload_size),0) FROM tool_outputs "
                                   "WHERE session_id=?1 AND payload_ref IS NOT NULL");
        if (!sum) return false;
        sqlite3_int64 bytes = 0;
        {
            Reset guard(sum);
            if (!global) bindText(sum, 1, sessionId);
            if (stepLocked(sum) != SQLITE_ROW) {
                noteError("tool output quota read failed: " +
                          std::string(sqlite3_errmsg(mDatabase)));
                return false;
            }
            bytes = sqlite3_column_int64(sum, 0);
        }
        const auto limit = static_cast<sqlite3_int64>(global ? kMaiToolOutputTotalMaxBytes
                                                             : kMaiToolOutputSessionMaxBytes);
        while (bytes > limit) {
            sqlite3_stmt* oldest = prepareLocked(
                global ? "SELECT id,payload_size FROM tool_outputs WHERE payload_ref IS NOT NULL "
                         "ORDER BY created,id LIMIT 1"
                       : "SELECT id,payload_size FROM tool_outputs WHERE session_id=?1 AND "
                         "payload_ref IS NOT NULL ORDER BY created,id LIMIT 1");
            if (!oldest) return false;
            std::string id;
            sqlite3_int64 removed = 0;
            {
                Reset guard(oldest);
                if (!global) bindText(oldest, 1, sessionId);
                if (stepLocked(oldest) != SQLITE_ROW) {
                    noteError("tool output eviction read failed: " +
                              std::string(sqlite3_errmsg(mDatabase)));
                    return false;
                }
                id = textColumn(oldest, 0);
                removed = sqlite3_column_int64(oldest, 1);
            }
            if (!executeStatementWithTextArgument(
                    "UPDATE tool_outputs SET payload_ref=NULL WHERE id=?1", id))
                return false;
            bytes -= removed;
        }
        return true;
    }

    MaiSession readSession(sqlite3_stmt* statement) const {
        MaiSession session;
        session.id = textColumn(statement, 0);
        session.title = textColumn(statement, 1);
        session.directory = textColumn(statement, 2);
        session.model = textColumn(statement, 3);
        session.agent = textColumn(statement, 4);
        session.parentId = textColumn(statement, 7);
        session.depth = sqlite3_column_int(statement, 8);
        session.titleEdited = sqlite3_column_int(statement, 9) != 0;
        session.reasoningEffort = textColumn(statement, 10);
        session.created = sqlite3_column_int64(statement, 5);
        session.updated = sqlite3_column_int64(statement, 6);
        return session;
    }

    MaiSpecialistTask readSpecialistTaskLocked(sqlite3_stmt* statement) const {
        MaiSpecialistTask task;
        task.id = textColumn(statement, 0);
        task.ownerSessionId = textColumn(statement, 1);
        task.specialistName = textColumn(statement, 2);
        task.parentTaskId = textColumn(statement, 3);
        task.providerTaskId = textColumn(statement, 4);
        task.intent = textColumn(statement, 5);
        task.contextSummary = textColumn(statement, 6);
        task.inputReference = textColumn(statement, 7);
        task.outputPath = textColumn(statement, 8);
        task.created = sqlite3_column_int64(statement, 9);
        task.status = columnToSpecialistStatus(sqlite3_column_int(statement, 10));
        task.partialText = textColumn(statement, 11);
        task.finalText = textColumn(statement, 12);
        task.errorText = textColumn(statement, 13);
        task.lastCheckedAt = sqlite3_column_int64(statement, 14);
        task.notificationMessageId = textColumn(statement, 15);
        task.notificationAttemptAt = sqlite3_column_int64(statement, 16);
        task.notifiedAt = sqlite3_column_int64(statement, 17);
        return task;
    }

    void writeSessionLocked(const MaiSession& session) {
        const bool own = sqlite3_get_autocommit(mDatabase) != 0;
        if (own && !executeTransactionStatement("BEGIN IMMEDIATE")) return;
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO sessions(id, title, directory, model, agent, created, updated, "
            "                     parent_id, depth, title_edited, reasoning_effort) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
            "ON CONFLICT(id) DO UPDATE SET title=excluded.title, directory=excluded.directory, "
            "model=excluded.model, agent=excluded.agent, updated=excluded.updated, "
            "title_edited=excluded.title_edited, reasoning_effort=excluded.reasoning_effort");
        if (!statement) return;
        Reset guard(statement);
        bindText(statement, 1, session.id);
        bindText(statement, 2, session.title);
        bindText(statement, 3, session.directory);
        bindText(statement, 4, session.model);
        bindText(statement, 5, session.agent);
        bindText(statement, 8, session.parentId);
        sqlite3_bind_int(statement, 9, session.depth);
        sqlite3_bind_int(statement, 10, session.titleEdited ? 1 : 0);
        bindText(statement, 11, session.reasoningEffort);
        sqlite3_bind_int64(statement, 6, session.created);
        sqlite3_bind_int64(statement, 7, session.updated);
        const bool saved = executePreparedStatement(statement);
        if (saved) {
            if (auto error = mJournal->projectSession(session)) noteError(error.message());
        }
        if (own) {
            if (saved && !mLastWriteError)
                executeTransactionStatement("COMMIT");
            else
                executeTransactionStatement("ROLLBACK");
        }
    }

    // 在持有存储锁期间写入一条消息片段及其持久化字段。
    // 调用方负责事务边界；此处保持消息与片段的关联一致。
    void writePartLocked(const std::string& messageId, const MaiMessagePart& part) {
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO parts(id, message_id, kind, created, text_ref, tool, call_id, input_ref, "
            "output_ref, error_ref, state, execution_ref, continuation_format, "
            "continuation_provider, continuation_model, continuation_payload_ref) VALUES(?1, ?2, "
            "?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)");
        if (!statement) return;
        Reset guard(statement);

        // 业务字段仍按同一列语义索引；正文绑定为源位置，读取时生成列回读。
        // 完整 DTO 另在应用完成事件中无损保存，SQLite 页面永不复制原始正文。
        int kind = kText;
        std::string text, tool, callId, input, output, error, execution;
        const MaiModelContinuationState* continuation = nullptr;
        int state = 0;
        if (const auto* textBody = std::get_if<MaiTextPart>(&part.body)) {
            kind = kText;
            text = textBody->text;
            // 非工具的 state 原来固定为 0；仅对正文/推理保存模型草稿位。
            // 工具的枚举值和 kind=2 恢复查询保持原契约，无需重写已有表。
            state = part.modelDraft ? 1 : 0;
        } else if (const auto* reasoningBody = std::get_if<MaiReasoningPart>(&part.body)) {
            kind = kReasoning;
            text = reasoningBody->text;
            state = part.modelDraft ? 1 : 0;
            if (reasoningBody->continuation) continuation = &*reasoningBody->continuation;
        } else if (const auto* imageBody = std::get_if<MaiImagePart>(&part.body)) {
            kind = kImage;
            text = imageBody->path;
            output = imageBody->mimeType;
        } else if (const auto* videoBody = std::get_if<MaiVideoPart>(&part.body)) {
            kind = kVideo;
            text = videoBody->path;
            output = videoBody->mimeType;
        } else if (const auto* quoteBody = std::get_if<MaiQuotePart>(&part.body)) {
            kind = kQuote;
            text = quoteBody->messageId;
            output = quoteBody->preview;
        } else if (const auto* toolBody = std::get_if<MaiToolPart>(&part.body)) {
            kind = kTool;
            tool = toolBody->tool;
            callId = toolBody->callId;
            input = toolBody->input;
            output = toolBody->output;
            error = toolBody->error;
            state = toolStateToColumn(toolBody->state);
            if (toolBody->execution) execution = maiSerializeToolExecution(*toolBody->execution);
            if (toolBody->modelOutputVersion != 0) {
                auto metadata =
                    execution.empty() ? nlohmann::json::object() : nlohmann::json::parse(execution);
                metadata["model_output_version"] = toolBody->modelOutputVersion;
                execution = metadata.dump();
            }
        }

        bindText(statement, 1, part.id);
        bindText(statement, 2, messageId);
        sqlite3_bind_int(statement, 3, kind);
        sqlite3_bind_int64(statement, 4, part.created);
        bindSourceText(statement, 5, text, sessionForMessageLocked(messageId));
        bindText(statement, 6, tool);
        bindText(statement, 7, callId);
        bindSourceText(statement, 8, input, sessionForMessageLocked(messageId));
        bindSourceText(statement, 9, output, sessionForMessageLocked(messageId));
        bindSourceText(statement, 10, error, sessionForMessageLocked(messageId));
        sqlite3_bind_int(statement, 11, state);
        bindSourceText(statement, 12, execution, sessionForMessageLocked(messageId));
        if (continuation) {
            bindContinuationText(statement, 13, continuation->format);
            bindContinuationText(statement, 14, continuation->provider);
            bindContinuationText(statement, 15, continuation->model);
            bindSourceText(statement, 16, continuation->payload,
                           sessionForMessageLocked(messageId));
        } else {
            for (int index = 13; index <= 16; ++index) sqlite3_bind_null(statement, index);
        }
        executePreparedStatement(statement);
    }

    std::vector<MaiMessagePart> readPartsLocked(const std::string& messageId,
                                                bool* succeeded = nullptr) const {
        if (succeeded) *succeeded = false;
        std::vector<MaiMessagePart> out;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, kind, created, text, tool, call_id, input, output, error, state, "
            "execution, "
            "continuation_format, continuation_provider, continuation_model, continuation_payload "
            "FROM parts WHERE message_id = ?1 ORDER BY id");
        if (!statement) return out;
        Reset guard(statement);
        bindText(statement, 1, messageId);
        int result;
        while ((result = stepLocked(statement)) == SQLITE_ROW) {
            MaiMessagePart part;
            part.id = textColumn(statement, 0);
            const int kind = sqlite3_column_int(statement, 1);
            part.created = sqlite3_column_int64(statement, 2);
            part.modelDraft =
                (kind == kText || kind == kReasoning) && sqlite3_column_int(statement, 9) == 1;
            if (kind == kReasoning) {
                MaiReasoningPart reasoning{textColumn(statement, 3)};
                MaiModelContinuationState continuation{
                    textColumn(statement, 11), textColumn(statement, 12), textColumn(statement, 13),
                    textColumn(statement, 14)};
                // 旧 NULL/全空列不发明状态；部分字段损坏时也保留原值，让 wire 明确拒绝。
                if (!continuation.format.empty() || !continuation.provider.empty() ||
                    !continuation.model.empty() || !continuation.payload.empty())
                    reasoning.continuation = std::move(continuation);
                part.body = std::move(reasoning);
            } else if (kind == kImage) {
                part.body = MaiImagePart{textColumn(statement, 3), textColumn(statement, 7)};
            } else if (kind == kVideo) {
                part.body = MaiVideoPart{textColumn(statement, 3), textColumn(statement, 7)};
            } else if (kind == kQuote) {
                part.body = MaiQuotePart{textColumn(statement, 3), textColumn(statement, 7)};
            } else if (kind == kTool) {
                MaiToolPart tool;
                tool.tool = textColumn(statement, 4);
                tool.callId = textColumn(statement, 5);
                tool.input = textColumn(statement, 6);
                tool.output = textColumn(statement, 7);
                tool.error = textColumn(statement, 8);
                tool.state = columnToToolState(sqlite3_column_int(statement, 9));
                MaiToolExecution execution;
                if (maiParseToolExecution(textColumn(statement, 10), execution))
                    tool.execution = std::move(execution);
                const auto metadata =
                    nlohmann::json::parse(textColumn(statement, 10), nullptr, false);
                if (metadata.is_object() && metadata.contains("model_output_version") &&
                    metadata["model_output_version"].is_number_unsigned())
                    tool.modelOutputVersion = metadata["model_output_version"].get<unsigned int>();
                part.body = std::move(tool);
            } else {
                part.body = MaiTextPart{textColumn(statement, 3)};
            }
            out.push_back(std::move(part));
        }
        if (succeeded) *succeeded = result == SQLITE_DONE;
        return out;
    }

    // 语句缓存：同一条 SQL 只 prepare 一次。prepare 要解析 SQL 并生成字节码，
    // 放在每次写入的路径上是浪费。
    static void bindToolModelOutputKey(sqlite3_stmt* statement, const MaiToolModelOutputKey& key) {
        bindText(statement, 1, key.sessionId);
        bindText(statement, 2, key.messageId);
        bindText(statement, 3, key.partId);
        bindText(statement, 4, key.model);
        bindText(statement, 5, key.sourceFingerprint);
    }

    MaiResult<std::string> readToolModelOutputLocked(const MaiToolModelOutputKey& key) const {
        sqlite3_stmt* statement = prepareLocked(
            "SELECT o.content FROM tool_model_outputs o "
            "JOIN sessions s ON s.id=o.session_id "
            "JOIN messages m ON m.id=o.message_id AND m.session_id=o.session_id "
            "JOIN parts p ON p.id=o.part_id AND p.message_id=o.message_id AND p.kind=2 "
            "WHERE o.session_id=?1 AND o.message_id=?2 AND o.part_id=?3 "
            "AND o.model=?4 AND o.source_fingerprint=?5");
        if (!statement) return mLastWriteError;
        Reset guard(statement);
        bindToolModelOutputKey(statement, key);
        const int code = stepLocked(statement);
        if (code == SQLITE_DONE)
            return {MaiErrorCode::NotFound, "The fixed model tool output is unavailable."};
        if (code != SQLITE_ROW) {
            noteError("fixed model tool output read failed: " +
                      std::string(sqlite3_errmsg(mDatabase)));
            return mLastWriteError;
        }
        const auto length = sqlite3_column_bytes(statement, 0);
        const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, 0));
        return length == 0 ? std::string{} : std::string(bytes, static_cast<std::size_t>(length));
    }

    sqlite3_stmt* prepareLocked(const char* sql) const {
        if (maiIsBlockingAllowed() && sqlite3_get_autocommit(mDatabase)) {
            if (auto error = mJournal->readBarrier()) {
                mLastReadError = error;
                return nullptr;
            }
        }
        auto it = mStatements.find(sql);
        if (it != mStatements.end()) return it->second;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(mDatabase, sql, -1, &statement, nullptr) != SQLITE_OK) {
            mLastReadError = {MaiErrorCode::Internal, "Cannot prepare session projection: " +
                                                          std::string(sqlite3_errmsg(mDatabase))};
            noteError("prepare failed: " + std::string(sqlite3_errmsg(mDatabase)));
            return nullptr;
        }
        mStatements.emplace(sql, statement);
        mCached.push_back(statement);
        return statement;
    }

    // 推进一条已绑定参数的写入语句，并记录 SQLite 返回的首个错误。
    bool executePreparedStatement(sqlite3_stmt* statement) {
        const int code = stepLocked(statement);
        if (code == SQLITE_DONE || code == SQLITE_ROW) return true;
        noteError("write failed: " + std::string(sqlite3_errmsg(mDatabase)));
        return false;
    }

    // 为单文本参数语句准备、绑定并执行，再用作用域守卫重置缓存句柄。
    bool executeStatementWithTextArgument(const char* sql, const std::string& argument) {
        sqlite3_stmt* statement = prepareLocked(sql);
        if (!statement) return false;
        Reset guard(statement);
        bindText(statement, 1, argument);
        return executePreparedStatement(statement);
    }

    // 执行 BEGIN、COMMIT 或 ROLLBACK 等事务控制语句。
    // SQLite 失败时保留首个写入错误，调用方决定是否继续该批次。
    bool executeTransactionStatement(const char* sql) const {
        MaiError error;
        if (std::strcmp(sql, "BEGIN IMMEDIATE") == 0)
            error = mJournal->begin();
        else if (std::strcmp(sql, "COMMIT") == 0)
            error = mJournal->commit();
        else if (std::strcmp(sql, "ROLLBACK") == 0) {
            mJournal->rollback();
            return true;
        } else
            error = {MaiErrorCode::Internal, "Unknown projection transaction operation."};
        if (error) {
            noteError(error.message());
            return false;
        }
        return true;
    }

    int stepLocked(sqlite3_stmt* statement) const {
        const bool write = sqlite3_stmt_readonly(statement) == 0;
        const bool own = write && sqlite3_get_autocommit(mDatabase) != 0;
        if (own && !executeTransactionStatement("BEGIN IMMEDIATE")) return SQLITE_ERROR;
        const int result = sqlite3_step(statement);
        if (write) mLastStatementChanges = sqlite3_changes(mDatabase);
        if (!write && result != SQLITE_ROW && result != SQLITE_DONE)
            mLastReadError = {MaiErrorCode::Internal, "Cannot read authoritative history: " +
                                                          std::string(sqlite3_errmsg(mDatabase))};
        if (own) {
            sqlite3_reset(statement);
            if (result == SQLITE_DONE) {
                if (!executeTransactionStatement("COMMIT")) return SQLITE_ERROR;
            } else
                executeTransactionStatement("ROLLBACK");
        }
        return result;
    }
    std::string sessionForMessageLocked(const std::string& messageId) const {
        auto* statement = prepareLocked("SELECT session_id FROM messages WHERE id=?1");
        if (!statement) return {};
        Reset reset(statement);
        bindText(statement, 1, messageId);
        return stepLocked(statement) == SQLITE_ROW ? textColumn(statement, 0) : std::string{};
    }
    std::string sessionForPartLocked(const std::string& partId) const {
        auto* statement = prepareLocked(
            "SELECT m.session_id FROM messages m JOIN parts p ON p.message_id=m.id WHERE p.id=?1");
        if (!statement) return {};
        Reset reset(statement);
        bindText(statement, 1, partId);
        return stepLocked(statement) == SQLITE_ROW ? textColumn(statement, 0) : std::string{};
    }
    void bindSourceText(sqlite3_stmt* statement, int index, const std::string& bytes,
                        const std::string& sessionId, bool binary = false) {
        auto ref = mJournal->appendBody(sessionId, bytes, binary);
        if (!ref) {
            noteError(ref.error().message());
            sqlite3_bind_null(statement, index);
            return;
        }
        bindText(statement, index, ref.value());
    }
    MaiResult<std::string> readSessionContextLocked(const std::string& sessionId,
                                                    const std::string& name) const {
        auto* statement = prepareLocked(
            "SELECT c.content FROM session_contexts c JOIN sessions s ON s.id=c.session_id WHERE "
            "c.session_id=?1 AND c.name=?2");
        if (!statement) return mLastReadError.hasError() ? mLastReadError : mLastWriteError;
        Reset reset(statement);
        bindText(statement, 1, sessionId);
        bindText(statement, 2, name);
        int code = stepLocked(statement);
        if (code == SQLITE_DONE) return {MaiErrorCode::NotFound, "Session context is unavailable."};
        if (code != SQLITE_ROW) return mLastReadError;
        return textColumn(statement, 0);
    }
    void mergeDraftPartsLocked(const std::string& sessionId, MaiMessage& message) const {
        const auto cached = mRecentMessages.find({sessionId, message.id});
        if (cached == mRecentMessages.end()) return;
        const auto& live = cached->second;
        // 来源和完成事实始终采用刚验证的持久数据；只补本实例仍属于同一消息阶段的 RAM 草稿。
        if (live.role != message.role || live.created != message.created ||
            live.completed != message.completed || live.turnId != message.turnId)
            return;
        bool added = false;
        for (const auto& part : live.parts) {
            if (!part.modelDraft) continue;
            const auto durable =
                std::find_if(message.parts.begin(), message.parts.end(),
                             [&](const MaiMessagePart& value) { return value.id == part.id; });
            if (durable != message.parts.end()) continue;
            message.parts.push_back(part);
            added = true;
        }
        if (added)
            std::stable_sort(message.parts.begin(), message.parts.end(),
                             [](const MaiMessagePart& left, const MaiMessagePart& right) {
                                 return left.id < right.id;
                             });
    }

    void clearRecentLocked(const std::string& sessionId) {
        for (auto it = mRecentMessages.begin(); it != mRecentMessages.end();)
            if (it->first.first == sessionId)
                it = mRecentMessages.erase(it);
            else
                ++it;
        for (auto it = mRecentPersisted.begin(); it != mRecentPersisted.end();)
            if (it->first.first == sessionId)
                it = mRecentPersisted.erase(it);
            else
                ++it;
    }

    // 只记住第一个错误。后面的多半是它的连锁反应，覆盖掉会让最有用的那条信息消失。
    void noteError(std::string what) const {
        if (!mLastWriteError.hasError())
            mLastWriteError = MaiError::make(MaiErrorCode::Internal, std::move(what));
    }

    sqlite3* mDatabase = nullptr;
    std::unique_ptr<MaiSessionJournal> mJournal;
    mutable int mLastStatementChanges = 0;
    mutable MaiError mLastReadError;
    std::map<std::pair<std::string, std::string>, MaiMessage> mRecentMessages;
    std::map<std::pair<std::string, std::string>, std::string> mRecentPersisted;
    mutable std::mutex mMutex;
    mutable std::map<std::string, sqlite3_stmt*> mStatements;
    mutable std::vector<sqlite3_stmt*> mCached;
    mutable MaiError mLastWriteError;
};

int readUserVersion(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) != SQLITE_OK)
        return -1;
    int version = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
    sqlite3_finalize(statement);
    return version;
}
}  // namespace

MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(
    const std::string& databasePathUtf8) {
    return makeMaiSqliteStore(databasePathUtf8, {});
}
MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(
    const std::string& databasePathUtf8, const MaiSqliteStoreOpenOptions& options) {
    const auto canceled = [&]() { return options.isCanceled && options.isCanceled(); };
    const auto progress = [&](const std::string& phase) {
        if (options.progress) options.progress(phase);
    };
    if (canceled()) return {MaiErrorCode::Canceled, "Session storage opening was canceled."};
    progress("inspecting");
    maiAssertBlockingAllowed("makeMaiSqliteStore");
    if (databasePathUtf8.empty() || databasePathUtf8.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "Invalid database path."};
    const bool ephemeral = databasePathUtf8 == ":memory:";
    const auto path = MaiFileSystem::resolve(MaiFilePath::fromUtf8(databasePathUtf8));
    auto root = ephemeral ? MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8(
                                "mai-journal-" + MaiIdGenerator::newSessionId()))
                          : path.dirName();
    if (auto error = MaiFileSystem::createDirectories(root)) return error;
    // 未完成发布必须先恢复；不能因正式索引暂时不存在而建立空库。
    if (!ephemeral && MaiSessionMigration::hasPending(root, path)) {
        if (auto error = MaiSessionJournal::migrateLegacy(path, root, options)) return error;
    }
    if (!ephemeral && MaiFileSystem::exists(path)) {
        sqlite3* probe = nullptr;
        const int opened = sqlite3_open_v2(path.toUtf8().c_str(), &probe,
                                           SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
        const int version = opened == SQLITE_OK ? readUserVersion(probe) : -1;
        sqlite3_close(probe);
        if (version < 0)
            return {MaiErrorCode::Internal, "Cannot inspect the existing storage index."};
        if (version > 0 && version < 16) {
            if (auto error = MaiSessionJournal::migrateLegacy(path, root, options)) return error;
        } else if (version != 0 && version != 16)
            return {MaiErrorCode::NotSupported, "Unsupported session storage version."};
    }
    if (canceled()) return {MaiErrorCode::Canceled, "Session storage opening was canceled."};
    progress("opening_projection");
    sqlite3* database = nullptr;
    if (sqlite3_open_v2((ephemeral ? databasePathUtf8 : path.toUtf8()).c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "Out of memory";
        sqlite3_close(database);
        return {MaiErrorCode::Internal, "Cannot open projection index: " + message};
    }
    sqlite3_busy_timeout(database, 5000);
    sqlite3_exec(database, "PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;", nullptr, nullptr,
                 nullptr);
    auto journal = MaiSessionJournal::open(
        database, root, ephemeral ? "memory" : path.baseName().toUtf8(), ephemeral);
    if (!journal) {
        sqlite3_close(database);
        return journal.error();
    }
    auto store =
        std::unique_ptr<SqliteStore>(new SqliteStore(database, std::move(journal.value())));
    if (canceled()) return {MaiErrorCode::Canceled, "Session storage opening was canceled."};
    if (auto error = store->invalidateAbandonedImports()) return error;
    progress("ready");
    return std::unique_ptr<MaiSessionStore>(std::move(store));
}
