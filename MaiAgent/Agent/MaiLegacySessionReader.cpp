// 文件作用：读取旧库的会话归属快照；不迁移、不写入、不恢复任务执行。
#include "MaiLegacySessionReader.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiSessionRecordCodec.h"

namespace {
using Json = nlohmann::json;

class MaiLegacyReadQuery {
public:
    MaiLegacyReadQuery(sqlite3* database, const std::string& sql) {
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &mStatement, nullptr) != SQLITE_OK)
            return;
        if (!sqlite3_stmt_readonly(mStatement)) {
            sqlite3_finalize(mStatement);
            mStatement = nullptr;
        }
    }
    ~MaiLegacyReadQuery() {
        sqlite3_finalize(mStatement);
    }
    sqlite3_stmt* statement() const {
        return mStatement;
    }

private:
    sqlite3_stmt* mStatement = nullptr;
};

class MaiLegacyReadTransaction {
public:
    explicit MaiLegacyReadTransaction(sqlite3* database) : mDatabase(database) {
        mOwned = sqlite3_get_autocommit(database) != 0;
        mValid = !mOwned || sqlite3_exec(database, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    ~MaiLegacyReadTransaction() {
        if (mOwned && mValid) sqlite3_exec(mDatabase, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    bool isValid() const {
        return mValid;
    }

private:
    sqlite3* mDatabase;
    bool mOwned = false;
    bool mValid = false;
};

const MaiLegacySqlField* field(const MaiLegacySqlRow& row, const char* name) {
    for (const auto& value : row.fields)
        if (value.sourceField == name) return &value;
    return nullptr;
}

MaiLegacySqlRow capture(sqlite3_stmt* statement, const std::string& table) {
    MaiLegacySqlRow row;
    row.sourceTable = table;
    for (int column = 0; column < sqlite3_column_count(statement); ++column) {
        MaiLegacySqlField value;
        value.sourceField = sqlite3_column_name(statement, column);
        if (const char* type = sqlite3_column_decltype(statement, column))
            value.declaredType = type;
        switch (sqlite3_column_type(statement, column)) {
            case SQLITE_NULL: value.type = MaiLegacySqlType::Null; break;
            case SQLITE_INTEGER:
                value.type = MaiLegacySqlType::Integer;
                value.integerValue = sqlite3_column_int64(statement, column);
                break;
            case SQLITE_FLOAT:
                value.type = MaiLegacySqlType::Real;
                value.realValue = sqlite3_column_double(statement, column);
                break;
            default: {
                value.type = sqlite3_column_type(statement, column) == SQLITE_BLOB
                                 ? MaiLegacySqlType::Blob
                                 : MaiLegacySqlType::Text;
                const void* bytes = sqlite3_column_blob(statement, column);
                const int count = sqlite3_column_bytes(statement, column);
                if (count != 0)
                    value.bytes.assign(static_cast<const char*>(bytes),
                                       static_cast<std::size_t>(count));
                break;
            }
        }
        row.fields.push_back(std::move(value));
    }
    return row;
}

class MaiLegacySnapshotReader {
public:
    explicit MaiLegacySnapshotReader(sqlite3* database) : mDatabase(database) {}

    bool initialize() {
        if (!mDatabase || sqlite3_db_readonly(mDatabase, "main") != 1)
            return fail(MaiErrorCode::InvalidInput,
                        "Legacy source must be a read-only main database");
        MaiLegacyReadQuery version(mDatabase, "PRAGMA main.user_version");
        if (!version.statement() || sqlite3_step(version.statement()) != SQLITE_ROW)
            return sqlError();
        mVersion = sqlite3_column_int(version.statement(), 0);
        if (mVersion < 1 || mVersion > 15)
            return fail(MaiErrorCode::NotSupported, "Unsupported legacy database version");
        MaiLegacyReadQuery tables(mDatabase,
                                  "SELECT name,sql FROM main.sqlite_master WHERE type='table'");
        if (!tables.statement()) return sqlError();
        int code;
        while ((code = sqlite3_step(tables.statement())) == SQLITE_ROW) {
            auto row = capture(tables.statement(), "schema");
            mTables[row.fields[0].bytes] = row.fields[1].bytes;
        }
        if (code != SQLITE_DONE) return sqlError();
        for (const auto& entry :
             std::initializer_list<std::pair<const char*, int>>{{"sessions", 1},
                                                                {"messages", 1},
                                                                {"parts", 1},
                                                                {"specialist_tasks", 4},
                                                                {"context_checkpoints", 8},
                                                                {"tool_outputs", 11},
                                                                {"context_imports", 12},
                                                                {"context_import_sources", 12},
                                                                {"context_import_chunks", 12},
                                                                {"tool_model_outputs", 15}})
            if (mVersion >= entry.second && !hasTable(entry.first))
                return fail(MaiErrorCode::InvalidInput, "Required legacy source table is missing");
        return true;
    }

    const MaiError& error() const {
        return mError;
    }
    int version() const {
        return mVersion;
    }
    bool hasTable(const std::string& name) const {
        return mTables.count(name) != 0;
    }
    const std::map<std::string, std::string>& tables() const {
        return mTables;
    }

    bool rows(const std::string& table, const std::string& suffix, const std::string* identity,
              std::vector<MaiLegacySqlRow>& output) {
        if (!hasTable(table)) return true;
        MaiLegacyReadQuery query(mDatabase, "SELECT t.* FROM main." + table + " t " + suffix);
        if (!query.statement()) return sqlError();
        if (identity &&
            sqlite3_bind_text(query.statement(), 1, identity->data(),
                              static_cast<int>(identity->size()), SQLITE_TRANSIENT) != SQLITE_OK)
            return sqlError();
        int code;
        while ((code = sqlite3_step(query.statement())) == SQLITE_ROW)
            output.push_back(capture(query.statement(), table));
        return code == SQLITE_DONE || sqlError();
    }

    bool text(const MaiLegacySqlRow& row, const char* name, std::string& output, int introduced = 1,
              bool nullable = false) {
        const auto* value = field(row, name);
        if (!value) return mVersion < introduced || corrupt();
        if (value->type == MaiLegacySqlType::Null && nullable) {
            output.clear();
            return true;
        }
        if (value->type != MaiLegacySqlType::Text && value->type != MaiLegacySqlType::Blob)
            return corrupt();
        output = value->bytes;
        return true;
    }

    template <typename Integer>
    bool integer(const MaiLegacySqlRow& row, const char* name, Integer& output,
                 int introduced = 1) {
        const auto* value = field(row, name);
        if (!value) return mVersion < introduced || corrupt();
        if (value->type != MaiLegacySqlType::Integer) return corrupt();
        const auto number = value->integerValue;
        if constexpr (std::is_unsigned_v<Integer>) {
            if (number < 0 ||
                static_cast<std::uint64_t>(number) > std::numeric_limits<Integer>::max())
                return corrupt();
        } else if (number < std::numeric_limits<Integer>::min() ||
                   number > std::numeric_limits<Integer>::max())
            return corrupt();
        output = static_cast<Integer>(number);
        return true;
    }
    bool boolean(const MaiLegacySqlRow& row, const char* name, bool& output, int introduced = 1) {
        int number = output ? 1 : 0;
        if (!integer(row, name, number, introduced) || number < 0 || number > 1) return corrupt();
        output = number != 0;
        return true;
    }

    bool session(const MaiLegacySqlRow& row, MaiSession& result) {
        return text(row, "id", result.id) && !result.id.empty() &&
               text(row, "title", result.title) && text(row, "directory", result.directory) &&
               text(row, "model", result.model) && text(row, "agent", result.agent) &&
               integer(row, "created", result.created) && integer(row, "updated", result.updated) &&
               text(row, "parent_id", result.parentId, 2) &&
               integer(row, "depth", result.depth, 2) &&
               boolean(row, "title_edited", result.titleEdited, 6) &&
               text(row, "reasoning_effort", result.reasoningEffort, 10);
    }

    bool message(const MaiLegacySqlRow& row, MaiMessage& result) {
        int role = 0;
        std::string turn;
        if (!text(row, "id", result.id) || result.id.empty() || !integer(row, "role", role) ||
            role < 0 || role > 1 || !integer(row, "created", result.created) ||
            !integer(row, "completed", result.completed) ||
            !text(row, "input_request_id", result.inputRequestId, 7) ||
            !boolean(row, "pending_input", result.pendingInput, 7) ||
            !text(row, "turn_state", turn, 14, true))
            return corrupt();
        result.role = role == 0 ? MaiRole::User : MaiRole::Assistant;
        if (auto error = MaiSessionRecordCodec::decodeTurnState(turn, result)) {
            mError = error;
            return false;
        }
        return true;
    }

    bool part(const MaiLegacySqlRow& row, MaiMessagePart& result) {
        int kind = 0, state = 0;
        std::string textValue, tool, callId, input, output, error, execution;
        if (!text(row, "id", result.id) || result.id.empty() || !integer(row, "kind", kind) ||
            kind < 0 || kind > 5 || !integer(row, "created", result.created) ||
            !text(row, "text", textValue) || !text(row, "tool", tool) ||
            !text(row, "call_id", callId) || !text(row, "input", input) ||
            !text(row, "output", output) || !text(row, "error", error) ||
            !integer(row, "state", state) || !text(row, "execution", execution, 11))
            return corrupt();
        result.modelDraft = (kind == 0 || kind == 1) && state == 1;
        if (kind == 0)
            result.body = MaiTextPart{std::move(textValue)};
        else if (kind == 1) {
            MaiReasoningPart reasoning;
            reasoning.text = std::move(textValue);
            MaiModelContinuationState continuation;
            if (!text(row, "continuation_format", continuation.format, 13, true) ||
                !text(row, "continuation_provider", continuation.provider, 13, true) ||
                !text(row, "continuation_model", continuation.model, 13, true) ||
                !text(row, "continuation_payload", continuation.payload, 13, true))
                return false;
            if (!continuation.format.empty() || !continuation.provider.empty() ||
                !continuation.model.empty() || !continuation.payload.empty())
                reasoning.continuation = std::move(continuation);
            result.body = std::move(reasoning);
        } else if (kind == 2) {
            if (state < 0 || state > 4) return corrupt();
            MaiToolPart body;
            body.tool = std::move(tool);
            body.callId = std::move(callId);
            body.input = std::move(input);
            body.output = std::move(output);
            body.error = std::move(error);
            body.state = static_cast<MaiToolState>(state);
            if (!execution.empty()) {
                const auto metadata = Json::parse(execution, nullptr, false);
                if (!metadata.is_object()) return corrupt();
                if (metadata.contains("model_output_version")) {
                    const auto& version = metadata["model_output_version"];
                    if (!version.is_number_unsigned() || version.get<std::uint64_t>() > 1)
                        return corrupt();
                    body.modelOutputVersion = version.get<unsigned>();
                }
                if (metadata.contains("status")) {
                    MaiToolExecution facts;
                    if (!maiParseToolExecution(execution, facts)) return corrupt();
                    body.execution = std::move(facts);
                } else {
                    for (const char* name :
                         {"effect_status", "next_action", "started", "exit_code", "timed_out",
                          "canceled", "output_truncated", "output_id", "task_id", "recovery_tool"})
                        if (metadata.contains(name)) return corrupt();
                }
            }
            result.body = std::move(body);
        } else if (kind == 3)
            result.body = MaiImagePart{std::move(textValue), std::move(output)};
        else if (kind == 4)
            result.body = MaiVideoPart{std::move(textValue), std::move(output)};
        else
            result.body = MaiQuotePart{std::move(textValue), std::move(output)};
        return true;
    }

    bool checkpoint(const MaiLegacySqlRow& row, MaiContextCheckpoint& result) {
#define TEXT(member, key, intro) \
    if (!text(row, key, result.member, intro)) return false
#define NUMBER(member, key, intro) \
    if (!integer(row, key, result.member, intro)) return false
        TEXT(sessionId, "session_id", 8);
        NUMBER(revision, "revision", 8);
        NUMBER(sourceMessageCount, "source_count", 8);
        TEXT(firstMessageId, "first_message_id", 8);
        TEXT(throughMessageId, "through_message_id", 8);
        TEXT(sourceFingerprint, "source_fingerprint", 8);
        TEXT(model, "model", 8);
        TEXT(summary, "summary", 8);
        TEXT(references, "reference_text", 8);
        NUMBER(created, "created", 8);
        TEXT(partialMessageId, "partial_message_id", 9);
        NUMBER(partialPartCount, "partial_part_count", 9);
        TEXT(throughPartId, "through_part_id", 9);
        TEXT(partialSourceFingerprint, "partial_source_fingerprint", 9);
        TEXT(preservedUserMessageId, "preserved_user_message_id", 9);
#undef TEXT
#undef NUMBER
        return true;
    }

    bool specialist(const MaiLegacySqlRow& row, MaiSpecialistTask& result) {
#define TEXT(member, key, intro) \
    if (!text(row, key, result.member, intro)) return false
#define NUMBER(member, key, intro) \
    if (!integer(row, key, result.member, intro)) return false
        TEXT(id, "id", 4);
        TEXT(ownerSessionId, "owner_session_id", 4);
        TEXT(specialistName, "specialist_name", 4);
        TEXT(parentTaskId, "parent_task_id", 4);
        TEXT(providerTaskId, "provider_task_id", 4);
        TEXT(intent, "intent", 4);
        TEXT(contextSummary, "context_summary", 4);
        TEXT(inputReference, "input_reference", 4);
        TEXT(outputPath, "output_path", 4);
        NUMBER(created, "created", 4);
        int status = 0;
        if (!integer(row, "status", status, 5) || status < 0 || status > 5) return corrupt();
        result.status = static_cast<MaiSpecialistTaskStatus>(status);
        TEXT(partialText, "partial_text", 5);
        TEXT(finalText, "final_text", 5);
        TEXT(errorText, "error_text", 5);
        NUMBER(lastCheckedAt, "last_checked_at", 5);
        TEXT(notificationMessageId, "notification_message_id", 5);
        NUMBER(notificationAttemptAt, "notification_attempt_at", 5);
        NUMBER(notifiedAt, "notified_at", 5);
#undef TEXT
#undef NUMBER
        return !result.id.empty() || corrupt();
    }

    bool corrupt() {
        return fail(MaiErrorCode::InvalidInput, "Invalid legacy source fields or ownership");
    }
    bool fail(MaiErrorCode code, const char* message) {
        if (!mError) mError = {code, message};
        return false;
    }

private:
    bool sqlError() {
        return fail(MaiErrorCode::Internal, "Cannot read legacy source database");
    }
    sqlite3* mDatabase;
    int mVersion = 0;
    std::map<std::string, std::string> mTables;
    MaiError mError;
};

bool buildSnapshot(MaiLegacySnapshotReader& reader, MaiLegacySessionSnapshot& snapshot,
                   const std::string& sessionId) {
    const std::pair<const char*, const char*> queries[] = {
        {"sessions", "WHERE t.id=?1"},
        {"messages", "WHERE t.session_id=?1 ORDER BY t.id"},
        {"parts", "JOIN main.messages m ON m.id=t.message_id WHERE m.session_id=?1 ORDER BY t.id"},
        {"specialist_tasks", "WHERE t.owner_session_id=?1 ORDER BY t.id"},
        {"context_checkpoints", "WHERE t.session_id=?1"},
        {"context_imports", "WHERE t.session_id=?1 ORDER BY t.id"},
        {"context_import_sources",
         "LEFT JOIN main.context_imports i ON i.id=t.import_id "
         "AND i.session_id=t.session_id WHERE t.session_id=?1 ORDER BY t.source_key"},
        {"context_import_chunks",
         "JOIN main.context_imports i ON i.id=t.import_id "
         "WHERE i.session_id=?1 ORDER BY t.import_id,t.sequence"},
        {"tool_outputs",
         "LEFT JOIN main.messages m ON m.id=t.message_id AND m.session_id=t.session_id "
         "LEFT JOIN main.parts p ON p.id=t.part_id AND p.message_id=t.message_id "
         "WHERE t.session_id=?1 ORDER BY t.created,t.id"},
        {"tool_model_outputs",
         "LEFT JOIN main.messages m ON m.id=t.message_id AND m.session_id=t.session_id "
         "LEFT JOIN main.parts p ON p.id=t.part_id AND p.message_id=t.message_id "
         "WHERE t.session_id=?1 ORDER BY t.message_id,t.part_id,t.model,t.source_fingerprint"}};
    for (const auto& query : queries) {
        if (!reader.rows(query.first, query.second, &sessionId, snapshot.sourceRows)) return false;
        const auto found = reader.tables().find(query.first);
        if (found != reader.tables().end())
            snapshot.sourceTables.push_back({found->first, found->second});
    }
    std::map<std::string, MaiMessage> messages;
    std::map<std::string, std::pair<std::string, int>> imports;
    std::set<std::string> partIds;
    std::set<std::string> selected;
    std::map<std::string, std::string> sourceKeys;
    bool hasSession = false;
    for (const auto& row : snapshot.sourceRows) {
        if (row.sourceTable == "sessions") {
            if (hasSession || !reader.session(row, snapshot.session) ||
                snapshot.session.id != sessionId)
                return reader.corrupt();
            hasSession = true;
        } else if (row.sourceTable == "context_imports") {
            std::string id, owner, key;
            int state = 0;
            if (!reader.text(row, "id", id) || !reader.text(row, "session_id", owner) ||
                owner != sessionId || !reader.text(row, "source_key", key) ||
                !reader.integer(row, "state", state) || state < 0 || state > 2 ||
                !imports.emplace(id, std::make_pair(key, state)).second)
                return reader.corrupt();
        } else if (row.sourceTable == "messages") {
            MaiMessage message;
            if (!reader.message(row, message) ||
                !messages.emplace(message.id, std::move(message)).second)
                return reader.corrupt();
        }
    }
    if (!hasSession) return reader.fail(MaiErrorCode::NotFound, "Legacy session does not exist");
    for (const auto& row : snapshot.sourceRows) {
        if (row.sourceTable == "context_import_sources") {
            std::string owner, key, id;
            if (!reader.text(row, "session_id", owner) || owner != sessionId ||
                !reader.text(row, "source_key", key) || !reader.text(row, "import_id", id) ||
                !imports.count(id) || imports[id].first != key || imports[id].second != 1)
                return reader.corrupt();
            selected.insert(id);
            sourceKeys[id] = std::move(key);
        } else if (row.sourceTable == "parts") {
            std::string owner;
            MaiMessagePart part;
            if (!reader.text(row, "message_id", owner) || !messages.count(owner) ||
                !reader.part(row, part) || !partIds.insert(part.id).second)
                return reader.corrupt();
            messages[owner].parts.push_back(std::move(part));
        }
    }
    std::map<std::string, std::pair<std::string, const MaiToolPart*>> tools;
    for (const auto& entry : messages)
        for (const auto& part : entry.second.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                if (!tools.emplace(part.id, std::make_pair(entry.first, tool)).second)
                    return reader.corrupt();

    for (const auto& row : snapshot.sourceRows) {
        if (row.sourceTable == "context_checkpoints") {
            MaiContextCheckpoint checkpoint;
            if (snapshot.checkpoint || !reader.checkpoint(row, checkpoint) ||
                checkpoint.sessionId != sessionId)
                return reader.corrupt();
            snapshot.checkpoint = std::move(checkpoint);
        } else if (row.sourceTable == "tool_outputs" || row.sourceTable == "tool_model_outputs") {
            std::string owner, messageId, partId;
            if (!reader.text(row, "session_id", owner) || owner != sessionId ||
                !reader.text(row, "message_id", messageId) ||
                !reader.text(row, "part_id", partId) || !tools.count(partId) ||
                tools[partId].first != messageId)
                return reader.corrupt();
            if (row.sourceTable == "tool_outputs") {
                MaiLegacyToolOutput output;
                output.record.sessionId = owner;
                output.record.messageId = messageId;
                output.record.partId = partId;
                if (!reader.text(row, "id", output.record.id) ||
                    !reader.text(row, "call_id", output.record.callId) ||
                    output.record.callId != tools[partId].second->callId ||
                    !reader.boolean(row, "truncated", output.record.truncated) ||
                    !reader.integer(row, "created", output.record.created))
                    return reader.corrupt();
                const auto* payload = field(row, "payload");
                if (!payload || (payload->type != MaiLegacySqlType::Null &&
                                 payload->type != MaiLegacySqlType::Blob &&
                                 payload->type != MaiLegacySqlType::Text))
                    return reader.corrupt();
                output.payloadType = payload->type;
                output.payloadAvailable = payload->type != MaiLegacySqlType::Null;
                output.record.text = payload->bytes;
                if (maiValidateToolOutputRecord(output.record)) return reader.corrupt();
                snapshot.toolOutputs.push_back(std::move(output));
            } else {
                MaiLegacyToolModelOutput output;
                output.sessionId = owner;
                output.messageId = messageId;
                output.partId = partId;
                if (!reader.text(row, "model", output.model) ||
                    !reader.text(row, "source_fingerprint", output.sourceFingerprint) ||
                    !reader.text(row, "content", output.content))
                    return false;
                snapshot.toolModelOutputs.push_back(std::move(output));
            }
        } else if (row.sourceTable == "specialist_tasks") {
            MaiSpecialistTask task;
            if (!reader.specialist(row, task) || task.ownerSessionId != sessionId)
                return reader.corrupt();
            snapshot.specialistTasks.push_back(std::move(task));
        }
    }
    std::set<std::string> tasks;
    for (const auto& task : snapshot.specialistTasks) tasks.insert(task.id);
    for (const auto& task : snapshot.specialistTasks)
        if (!task.parentTaskId.empty() && !tasks.count(task.parentTaskId)) return reader.corrupt();

    for (const auto& row : snapshot.sourceRows) {
        if (row.sourceTable != "messages") continue;
        std::string id, importId, sourceId;
        if (!reader.text(row, "id", id) ||
            !reader.text(row, "context_import_id", importId, 12, true) ||
            !reader.text(row, "context_source_id", sourceId, 12, true))
            return false;
        const auto* importField = field(row, "context_import_id");
        const bool imported = importField && importField->type != MaiLegacySqlType::Null;
        auto message = std::move(messages[id]);
        if (!imported) {
            snapshot.messages.push_back(std::move(message));
        } else {
            if (importId.empty() || !imports.count(importId) || sourceId.empty())
                return reader.corrupt();
            if (selected.count(importId)) {
                if (sourceKeys[importId].empty()) return reader.corrupt();
                message.contextImportOrigin =
                    MaiContextImportOrigin{sourceKeys[importId], sourceId};
                snapshot.messages.push_back(std::move(message));
            } else
                snapshot.unpublishedMessages.push_back(std::move(message));
        }
    }
    return true;
}
}  // namespace

MaiResult<std::vector<MaiSession>> MaiLegacySessionReader::readSessions(sqlite3* database) {
    if (!database || sqlite3_db_readonly(database, "main") != 1)
        return {MaiErrorCode::InvalidInput, "Legacy source must be a read-only main database"};
    MaiLegacyReadTransaction transaction(database);
    if (!transaction.isValid())
        return {MaiErrorCode::Internal, "Cannot begin legacy read snapshot"};
    MaiLegacySnapshotReader reader(database);
    if (!reader.initialize()) return reader.error();
    std::vector<MaiLegacySqlRow> rows;
    if (!reader.rows("sessions", "ORDER BY t.updated DESC,t.id", nullptr, rows))
        return reader.error();
    std::vector<MaiSession> result;
    for (const auto& row : rows) {
        MaiSession session;
        if (!reader.session(row, session)) {
            reader.corrupt();
            return reader.error();
        }
        result.push_back(std::move(session));
    }
    return result;
}

MaiResult<MaiLegacySessionSnapshot> MaiLegacySessionReader::readSession(
    sqlite3* database, const std::string& sessionId) {
    if (sessionId.empty() ||
        sessionId.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {MaiErrorCode::InvalidInput, "Invalid legacy session identity"};
    if (!database || sqlite3_db_readonly(database, "main") != 1)
        return {MaiErrorCode::InvalidInput, "Legacy source must be a read-only main database"};
    MaiLegacyReadTransaction transaction(database);
    if (!transaction.isValid())
        return {MaiErrorCode::Internal, "Cannot begin legacy read snapshot"};
    MaiLegacySnapshotReader reader(database);
    if (!reader.initialize()) return reader.error();
    MaiLegacySessionSnapshot snapshot;
    snapshot.sourceVersion = reader.version();
    if (!buildSnapshot(reader, snapshot, sessionId)) return reader.error();
    return snapshot;
}
