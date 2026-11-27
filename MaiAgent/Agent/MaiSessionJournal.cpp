// 文件作用：正式后端的权威正文、元数据提交日志和统一 SQLite 投影。
#include "MaiSessionJournal.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <json.hpp>
#include <sqlite3.h>
#include <openssl/evp.h>
#include "MaiBlockingCheck.h"
#include "MaiCodexStoreSchema.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiLegacySessionReader.h"
#include "MaiRolloutLog.h"
#include "MaiRolloutPath.h"
#include "MaiRolloutProjection.h"
#include "MaiSessionMigration.h"
#include "MaiSessionRecordCodec.h"
#include "MaiSqliteStore.h"

namespace {
using Json = nlohmann::json;
constexpr int kSchemaVersion = 16;
constexpr std::size_t kOutputChunk = 16384;
MaiError sqlError(sqlite3* db, const char* prefix) {
    return {MaiErrorCode::Internal, std::string(prefix) + ": " + sqlite3_errmsg(db)};
}
MaiError exec(sqlite3* db, const std::string& sql) {
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK) return {};
    return sqlError(db, "Projection SQL failed");
}
std::string text(sqlite3_stmt* s, int i) {
    const auto* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p),
                           static_cast<std::size_t>(sqlite3_column_bytes(s, i)))
             : std::string{};
}
std::string quote(const std::string& value) {
    std::string r = "\"";
    for (char c : value) {
        r += c;
        if (c == '\"') r += c;
    }
    return r + '\"';
}
std::string payloadHash(const std::string& value) {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return {};
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(context, value.data(), value.size()) == 1 &&
              EVP_DigestFinal_ex(context, bytes, &size) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}
std::string pathText(const MaiFilePath& path) {
    auto value = path.toUtf8();
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}
std::string hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string r;
    r.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        r += digits[c >> 4];
        r += digits[c & 15];
    }
    return r;
}
MaiResult<std::string> unhex(const Json& value) {
    if (!value.is_string()) return {MaiErrorCode::InvalidInput, "Invalid journal byte encoding."};
    const auto& s = value.get_ref<const std::string&>();
    if (s.size() % 2) return {MaiErrorCode::InvalidInput, "Invalid journal byte encoding."};
    std::string r;
    r.reserve(s.size() / 2);
    for (std::size_t i = 0; i < s.size(); i += 2) {
        auto digit = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        int a = digit(s[i]), b = digit(s[i + 1]);
        if (a < 0 || b < 0) return {MaiErrorCode::InvalidInput, "Invalid journal byte encoding."};
        r += static_cast<char>((a << 4) | b);
    }
    return r;
}
Json bytesJson(const std::string& bytes) {
    try {
        Json v = bytes;
        (void)v.dump();
        return Json{{"text", bytes}};
    } catch (const Json::exception&) {
        return Json{{"hex", hex(bytes)}};
    }
}
MaiResult<std::string> readBytes(const Json& value) {
    if (!value.is_object()) return {MaiErrorCode::InvalidInput, "Invalid journal body."};
    if (value.contains("text") && value["text"].is_string())
        return value["text"].get<std::string>();
    if (value.contains("hex")) return unhex(value["hex"]);
    return {MaiErrorCode::InvalidInput, "Invalid journal body encoding."};
}
bool isLegacyDomainTable(const std::string& name) {
    static const std::set<std::string> tables{"sessions",
                                              "messages",
                                              "parts",
                                              "specialist_tasks",
                                              "context_checkpoints",
                                              "context_imports",
                                              "context_import_sources",
                                              "context_import_chunks",
                                              "tool_outputs",
                                              "tool_model_outputs"};
    return tables.count(name) != 0;
}
const std::map<std::string, std::set<std::string>>& bodyColumns() {
    static const std::map<std::string, std::set<std::string>> columns{
        {"parts", {"text", "input", "output", "error", "execution", "continuation_payload"}},
        {"messages", {"turn_state"}},
        {"context_checkpoints", {"summary", "reference_text"}},
        {"tool_model_outputs", {"content"}},
        {"specialist_tasks",
         {"intent", "context_summary", "partial_text", "final_text", "error_text"}}};
    return columns;
}
const char* extensionSchema = R"SQL(
CREATE TABLE context_checkpoints(session_id TEXT,revision INTEGER NOT NULL,source_count INTEGER NOT NULL,first_message_id TEXT NOT NULL,through_message_id TEXT NOT NULL,source_fingerprint TEXT NOT NULL,model TEXT NOT NULL,summary_ref TEXT NOT NULL DEFAULT '',summary TEXT GENERATED ALWAYS AS (mai_body(summary_ref)) VIRTUAL,reference_text_ref TEXT NOT NULL DEFAULT '',reference_text TEXT GENERATED ALWAYS AS (mai_body(reference_text_ref)) VIRTUAL,created INTEGER NOT NULL,partial_message_id TEXT NOT NULL DEFAULT '',partial_part_count INTEGER NOT NULL DEFAULT 0,through_part_id TEXT NOT NULL DEFAULT '',partial_source_fingerprint TEXT NOT NULL DEFAULT '',preserved_user_message_id TEXT NOT NULL DEFAULT '',PRIMARY KEY(session_id));
CREATE TABLE context_import_chunks(import_id TEXT NOT NULL,sequence INTEGER NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(import_id,sequence));
CREATE TABLE context_import_sources(session_id TEXT NOT NULL,source_key TEXT NOT NULL,import_id TEXT NOT NULL,PRIMARY KEY(session_id,source_key));
CREATE TABLE context_imports(id TEXT,session_id TEXT NOT NULL,source_key TEXT NOT NULL,snapshot_id TEXT NOT NULL,label TEXT NOT NULL,next_sequence INTEGER NOT NULL DEFAULT 0,record_count INTEGER NOT NULL DEFAULT 0,last_created INTEGER NOT NULL DEFAULT 0,state INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(id));
CREATE TABLE messages(id TEXT,session_id TEXT NOT NULL,role INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL DEFAULT 0,completed INTEGER NOT NULL DEFAULT 0,input_request_id TEXT NOT NULL DEFAULT '',pending_input INTEGER NOT NULL DEFAULT 0,context_import_id TEXT,context_source_id TEXT,turn_state_ref TEXT,turn_state TEXT GENERATED ALWAYS AS (mai_body(turn_state_ref)) VIRTUAL,PRIMARY KEY(id));
CREATE TABLE parts(id TEXT,message_id TEXT NOT NULL,kind INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL DEFAULT 0,text_ref TEXT NOT NULL DEFAULT '',text TEXT GENERATED ALWAYS AS (mai_body(text_ref)) VIRTUAL,tool TEXT NOT NULL DEFAULT '',call_id TEXT NOT NULL DEFAULT '',input_ref TEXT NOT NULL DEFAULT '',input TEXT GENERATED ALWAYS AS (mai_body(input_ref)) VIRTUAL,output_ref TEXT NOT NULL DEFAULT '',output TEXT GENERATED ALWAYS AS (mai_body(output_ref)) VIRTUAL,error_ref TEXT NOT NULL DEFAULT '',error TEXT GENERATED ALWAYS AS (mai_body(error_ref)) VIRTUAL,state INTEGER NOT NULL DEFAULT 0,execution_ref TEXT NOT NULL DEFAULT '',execution TEXT GENERATED ALWAYS AS (mai_body(execution_ref)) VIRTUAL,continuation_format TEXT,continuation_provider TEXT,continuation_model TEXT,continuation_payload_ref TEXT,continuation_payload TEXT GENERATED ALWAYS AS (mai_body(continuation_payload_ref)) VIRTUAL,PRIMARY KEY(id));
CREATE TABLE sessions(id TEXT,title TEXT NOT NULL DEFAULT '',directory TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',agent TEXT NOT NULL DEFAULT 'build',created INTEGER NOT NULL DEFAULT 0,updated INTEGER NOT NULL DEFAULT 0,parent_id TEXT NOT NULL DEFAULT '',depth INTEGER NOT NULL DEFAULT 0,title_edited INTEGER NOT NULL DEFAULT 0,reasoning_effort TEXT NOT NULL DEFAULT '',PRIMARY KEY(id));
CREATE TABLE specialist_tasks(id TEXT,owner_session_id TEXT NOT NULL,specialist_name TEXT NOT NULL,parent_task_id TEXT NOT NULL DEFAULT '',provider_task_id TEXT NOT NULL DEFAULT '',intent_ref TEXT NOT NULL DEFAULT '',intent TEXT GENERATED ALWAYS AS (mai_body(intent_ref)) VIRTUAL,context_summary_ref TEXT NOT NULL DEFAULT '',context_summary TEXT GENERATED ALWAYS AS (mai_body(context_summary_ref)) VIRTUAL,input_reference TEXT NOT NULL DEFAULT '',output_path TEXT NOT NULL DEFAULT '',created INTEGER NOT NULL,status INTEGER NOT NULL DEFAULT 0,partial_text_ref TEXT NOT NULL DEFAULT '',partial_text TEXT GENERATED ALWAYS AS (mai_body(partial_text_ref)) VIRTUAL,final_text_ref TEXT NOT NULL DEFAULT '',final_text TEXT GENERATED ALWAYS AS (mai_body(final_text_ref)) VIRTUAL,error_text_ref TEXT NOT NULL DEFAULT '',error_text TEXT GENERATED ALWAYS AS (mai_body(error_text_ref)) VIRTUAL,last_checked_at INTEGER NOT NULL DEFAULT 0,notification_message_id TEXT NOT NULL DEFAULT '',notification_attempt_at INTEGER NOT NULL DEFAULT 0,notified_at INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(id));
CREATE TABLE tool_model_outputs(session_id TEXT NOT NULL,message_id TEXT NOT NULL,part_id TEXT NOT NULL,model TEXT NOT NULL,source_fingerprint TEXT NOT NULL,content_ref TEXT NOT NULL DEFAULT '',content BLOB GENERATED ALWAYS AS (mai_body(content_ref)) VIRTUAL,PRIMARY KEY(session_id,message_id,part_id,model,source_fingerprint));
CREATE TABLE tool_outputs(id TEXT,session_id TEXT NOT NULL,message_id TEXT NOT NULL,part_id TEXT NOT NULL,call_id TEXT NOT NULL,payload_ref TEXT,payload_size INTEGER NOT NULL DEFAULT 0,truncated INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL,PRIMARY KEY(id));
CREATE INDEX sessions_updated ON sessions(updated DESC);
CREATE INDEX sessions_parent ON sessions(parent_id);
CREATE INDEX messages_session ON messages(session_id,id);
CREATE INDEX parts_message ON parts(message_id,id);
CREATE INDEX parts_unfinished_tool ON parts(kind,state,message_id);
CREATE INDEX specialist_tasks_owner ON specialist_tasks(owner_session_id,id);
CREATE INDEX specialist_tasks_active ON specialist_tasks(status,last_checked_at,created);
CREATE UNIQUE INDEX messages_input_request ON messages(session_id,input_request_id) WHERE input_request_id<>'';
CREATE INDEX messages_pending_input ON messages(session_id,id) WHERE pending_input=1;
CREATE INDEX messages_context_order ON messages(context_import_id,id) WHERE context_import_id IS NOT NULL;
CREATE INDEX messages_ordinary_order ON messages(session_id,id) WHERE context_import_id IS NULL;
CREATE UNIQUE INDEX messages_context_source ON messages(context_import_id,context_source_id) WHERE context_import_id IS NOT NULL;
CREATE INDEX context_imports_source ON context_imports(session_id,source_key,state);
CREATE UNIQUE INDEX context_import_sources_import ON context_import_sources(import_id);
CREATE INDEX tool_outputs_session ON tool_outputs(session_id,created,id);
CREATE INDEX tool_outputs_oldest ON tool_outputs(created,id) WHERE payload_ref IS NOT NULL;
CREATE TABLE mai_session_record_keys(session_id TEXT NOT NULL,source_key TEXT NOT NULL,payload_hash TEXT NOT NULL,PRIMARY KEY(session_id,source_key));
CREATE TABLE mai_session_records(session_id TEXT NOT NULL,type TEXT NOT NULL,path TEXT NOT NULL,byte_offset INTEGER NOT NULL,byte_len INTEGER NOT NULL,ordinal INTEGER NOT NULL,PRIMARY KEY(session_id,type));
CREATE TABLE mai_session_rollouts(session_id TEXT PRIMARY KEY,path TEXT NOT NULL UNIQUE,rollout_id TEXT NOT NULL);
CREATE TABLE mai_journal_cursor(id INTEGER PRIMARY KEY CHECK(id=1),byte_offset INTEGER NOT NULL,next_ordinal INTEGER NOT NULL);
CREATE TABLE session_contexts(session_id TEXT NOT NULL,name TEXT NOT NULL,content_ref TEXT NOT NULL,content TEXT GENERATED ALWAYS AS (mai_body(content_ref)) VIRTUAL,PRIMARY KEY(session_id,name));
PRAGMA user_version=16;
)SQL";
MaiSqliteProjectionMutationOptions mutationOptions(sqlite3* db) {
    MaiSqliteProjectionMutationOptions o;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                           "'sqlite_%' AND name<>'mai_journal_cursor' ORDER BY name",
                           -1, &s, nullptr) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) o.allowedTables.push_back(text(s, 0));
    }
    sqlite3_finalize(s);
    return o;
}
MaiError bindStep(sqlite3* db, const char* sql, const std::vector<std::string>& values) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        return sqlError(db, "Cannot prepare journal metadata");
    for (std::size_t i = 0; i < values.size(); ++i)
        sqlite3_bind_text64(s, static_cast<int>(i + 1), values[i].data(), values[i].size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8);
    int code = sqlite3_step(s);
    sqlite3_finalize(s);
    return code == SQLITE_DONE ? MaiError{} : sqlError(db, "Cannot write journal metadata");
}
}  // namespace

struct MaiSessionJournal::State {
    sqlite3* db = nullptr;
    MaiFilePath root;
    bool ephemeral = false;
    std::unique_ptr<MaiRolloutLog> metadata;
    std::map<std::string, std::unique_ptr<MaiRolloutLog>> logs;
    std::set<std::string> dirtyLogs;
    // 只缓存本事务刚写入的至多 16 个小字段，避免生成列在同一 INSERT 中反复打开同一源。
    // 提交/回滚即清空；普通历史回读仍验证权威文件，不缓存完整会话正文。
    std::map<std::string, std::string> provisionalBodies;
    MaiSqliteProjectionMutationOptions options;
    std::unique_ptr<MaiSqliteProjectionMutationRecorder> recorder;
    bool capturing = false;
    MaiError projectionError;
    MaiError sourceError;
    MaiError cursor(MaiRolloutCursor& result) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db,
                               "SELECT byte_offset,next_ordinal FROM mai_journal_cursor WHERE id=1",
                               -1, &s, nullptr) != SQLITE_OK)
            return sqlError(db, "Cannot read journal cursor");
        const int code = sqlite3_step(s);
        if (code == SQLITE_ROW) {
            result.byteOffset = static_cast<std::uint64_t>(sqlite3_column_int64(s, 0));
            result.nextOrdinal = static_cast<std::uint64_t>(sqlite3_column_int64(s, 1));
        }
        sqlite3_finalize(s);
        return code == SQLITE_ROW || code == SQLITE_DONE
                   ? MaiError{}
                   : sqlError(db, "Cannot read journal cursor");
    }
    MaiError setCursor(MaiRolloutCursor cursor) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO mai_journal_cursor VALUES(1,?1,?2) ON CONFLICT(id) DO UPDATE SET "
                "byte_offset=excluded.byte_offset,next_ordinal=excluded.next_ordinal",
                -1, &s, nullptr) != SQLITE_OK)
            return sqlError(db, "Cannot prepare journal cursor");
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(cursor.byteOffset));
        sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(cursor.nextOrdinal));
        int code = sqlite3_step(s);
        sqlite3_finalize(s);
        return code == SQLITE_DONE ? MaiError{} : sqlError(db, "Cannot save journal cursor");
    }
    MaiError replay() {
        MaiRolloutCursor start;
        if (auto e = cursor(start)) return e;
        std::uint64_t sourceSize = 0;
        if (!MaiFileSystem::fileSize(metadata->sourcePath(), sourceSize))
            return {MaiErrorCode::Internal, "Cannot stat authoritative commit log."};
        if (sourceSize == start.byteOffset) return {};
        // 重启或其它写者留下的完整可读尾部，仍须成功 flush 才能推进派生索引。
        if (auto durable = metadata->confirmDurable()) return durable;
        auto end = metadata->replay(start, [&](const MaiRolloutRecord& r) -> MaiError {
            if (r.type == "session_meta") return {};
            auto v = Json::parse(r.payload, nullptr, false);
            if (!v.is_object() || v.value("type", std::string{}) != "mai_projection_transaction" ||
                !v.contains("mutations"))
                return {MaiErrorCode::InvalidInput, "Unknown account projection record."};
            std::vector<MaiSqliteProjectionMutation> changes;
            std::string error;
            if (!maiDecodeSqliteProjectionMutations(v["mutations"].dump(), changes, &error) ||
                !maiApplySqliteProjectionMutations(db, changes, options, &error))
                return {MaiErrorCode::Internal, "Cannot replay committed projection: " + error};
            return {};
        });
        if (!end) return end.error();
        if (auto durable = metadata->confirmDurable()) return durable;
        return setCursor(end.value());
    }
    MaiResult<std::string> pathFor(const std::string& sid) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT path FROM mai_session_rollouts WHERE session_id=?1", -1,
                               &s, nullptr) != SQLITE_OK)
            return sqlError(db, "Cannot prepare rollout lookup");
        sqlite3_bind_text64(s, 1, sid.data(), sid.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        int code = sqlite3_step(s);
        std::string path;
        if (code == SQLITE_ROW) path = text(s, 0);
        sqlite3_finalize(s);
        if (code == SQLITE_DONE) return {MaiErrorCode::NotFound, "Session rollout is unavailable."};
        if (code != SQLITE_ROW) return sqlError(db, "Cannot read rollout lookup");
        return path;
    }
    MaiResult<MaiRolloutLog*> logFor(const std::string& sid) {
        auto p = pathFor(sid);
        if (!p) return p.error();
        auto i = logs.find(p.value());
        if (i != logs.end()) return i->second.get();
        auto opened = MaiRolloutLog::open(root.append(MaiFilePath::fromUtf8(p.value())));
        if (!opened) return opened.error();
        auto* result = opened.value().get();
        logs.emplace(p.value(), std::move(opened.value()));
        return result;
    }
    MaiResult<MaiFilePath> sourcePath(const std::string& relative) const {
        if (relative.compare(0, 9, "sessions/") != 0 || relative.find('\\') != std::string::npos ||
            relative.find(':') != std::string::npos || relative.find('\0') != std::string::npos)
            return {MaiErrorCode::InvalidInput, "Invalid body source path."};
        std::size_t at = 0;
        while (at < relative.size()) {
            auto end = relative.find('/', at);
            auto c = relative.substr(at, end - at);
            if (c.empty() || c == "." || c == "..")
                return {MaiErrorCode::InvalidInput, "Invalid body source component."};
            if (end == std::string::npos) break;
            at = end + 1;
        }
        auto p = root.append(MaiFilePath::fromUtf8(relative));
        if (MaiFileSystem::isSymbolicLink(p))
            return {MaiErrorCode::InvalidInput, "Body source cannot be a link."};
        auto resolved = MaiFileSystem::resolve(p);
        if (resolved.isEmpty() || !root.isParentOf(resolved))
            return {MaiErrorCode::InvalidInput, "Body source escaped its root."};
        return resolved;
    }
    MaiResult<std::string> body(const Json& ref) {
        if (!ref.is_array() || ref.size() != 5 || !ref[0].is_string())
            return {MaiErrorCode::InvalidInput, "Invalid body source reference."};
        auto p = sourcePath(ref[0].get<std::string>());
        if (!p) return p.error();
        auto r = MaiRolloutLog::read(p.value(),
                                     {ref[3].get<std::uint64_t>(), ref[1].get<std::uint64_t>(),
                                      ref[2].get<std::uint64_t>()});
        if (!r) return r.error();
        auto payload = Json::parse(r.value().payload, nullptr, false);
        if (r.value().type != "event_msg" || !payload.is_object() ||
            payload.value("type", std::string{}) != "mai_body" || !payload.contains("bytes"))
            return {MaiErrorCode::InvalidInput, "Source location is not a body record."};
        return readBytes(payload["bytes"]);
    }
    static void bodyFunction(sqlite3_context* context, int, sqlite3_value** values) {
        auto* self = static_cast<State*>(sqlite3_user_data(context));
        try {
            if (sqlite3_value_type(values[0]) == SQLITE_NULL) {
                sqlite3_result_null(context);
                return;
            }
            const auto* p = reinterpret_cast<const char*>(sqlite3_value_text(values[0]));
            std::string ref(p ? p : "", static_cast<std::size_t>(sqlite3_value_bytes(values[0])));
            if (ref.empty()) {
                sqlite3_result_text(context, "", 0, SQLITE_STATIC);
                return;
            }
            if (ref == "@mai_empty_blob_v1") {
                sqlite3_result_blob(context, "", 0, SQLITE_STATIC);
                return;
            }
            auto v = Json::parse(ref);
            auto cached = self->provisionalBodies.find(ref);
            auto result = cached == self->provisionalBodies.end()
                              ? self->body(v)
                              : MaiResult<std::string>(cached->second);
            if (!result) {
                self->sourceError = result.error();
                sqlite3_result_error(context, result.error().message().c_str(), -1);
                return;
            }
            if (v[4].get<bool>())
                sqlite3_result_blob64(context, result.value().data(), result.value().size(),
                                      SQLITE_TRANSIENT);
            else
                sqlite3_result_text64(context, result.value().data(), result.value().size(),
                                      SQLITE_TRANSIENT, SQLITE_UTF8);
        } catch (...) {
            self->sourceError = {MaiErrorCode::InvalidInput,
                                 "Invalid authoritative body reference."};
            sqlite3_result_error(context, "Invalid authoritative body reference.", -1);
        }
    }
    static void outputFunction(sqlite3_context* context, int, sqlite3_value** values) {
        auto* self = static_cast<State*>(sqlite3_user_data(context));
        try {
            if (sqlite3_value_type(values[0]) == SQLITE_NULL) {
                sqlite3_result_null(context);
                return;
            }
            auto* p = reinterpret_cast<const char*>(sqlite3_value_text(values[0]));
            auto ref = Json::parse(p, p + sqlite3_value_bytes(values[0]));
            auto size = ref.at("size").get<std::size_t>();
            auto offset = sqlite3_value_int64(values[1]);
            auto count = sqlite3_value_int64(values[2]);
            if (offset < 0 || count < 0 || count > 1024 * 1024) throw std::runtime_error("range");
            std::string out;
            auto first = std::min(static_cast<std::size_t>(offset), size);
            auto end = first + std::min(static_cast<std::size_t>(count), size - first);
            for (std::size_t index = first / kOutputChunk; first < end; ++index) {
                const auto& c = ref.at("chunks").at(index);
                Json bodyRef = Json::array({ref.at("path"), c[0], c[1], c[2], true});
                auto bytes = self->body(bodyRef);
                if (!bytes) {
                    self->sourceError = bytes.error();
                    sqlite3_result_error(context, bytes.error().message().c_str(), -1);
                    return;
                }
                auto inChunk = first % kOutputChunk;
                auto take = std::min(end - first, bytes.value().size() - inChunk);
                if (!take) throw std::runtime_error("chunk");
                out.append(bytes.value(), inChunk, take);
                first += take;
            }
            sqlite3_result_blob64(context, out.data(), out.size(), SQLITE_TRANSIENT);
        } catch (...) {
            self->sourceError = {MaiErrorCode::InvalidInput,
                                 "Invalid authoritative output reference."};
            sqlite3_result_error(context, "Invalid authoritative output reference.", -1);
        }
    }
};

MaiSessionJournal::MaiSessionJournal(std::unique_ptr<State> state) : mState(std::move(state)) {}
MaiSessionJournal::~MaiSessionJournal() {
    mState->recorder.reset();
    mState->logs.clear();
    mState->metadata.reset();
    if (mState->ephemeral) MaiFileSystem::removeRecursively(mState->root);
}
MaiResult<std::unique_ptr<MaiSessionJournal>> MaiSessionJournal::open(sqlite3* db,
                                                                      const MaiFilePath& root,
                                                                      const std::string& indexName,
                                                                      bool ephemeral) {
    maiAssertBlockingAllowed("MaiSessionJournal::open");
    auto state = std::make_unique<State>();
    state->db = db;
    state->root = MaiFileSystem::resolve(root);
    state->ephemeral = ephemeral;
    if (state->root.isEmpty() || !state->root.isAbsolute())
        return {MaiErrorCode::InvalidInput, "Invalid account journal root."};
    auto journal = std::unique_ptr<MaiSessionJournal>(new MaiSessionJournal(std::move(state)));
    for (const auto& f : {std::make_pair("mai_body", 1), std::make_pair("mai_output_slice", 3)}) {
        auto function = f.second == 1 ? State::bodyFunction : State::outputFunction;
        if (sqlite3_create_function_v2(
                db, f.first, f.second, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                journal->mState.get(), function, nullptr, nullptr, nullptr) != SQLITE_OK)
            return sqlError(db, "Cannot register body projections");
    }
    if (auto e = maiRegisterRolloutProjection(db, root)) return e;
    sqlite3_stmt* version = nullptr;
    int v = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &version, nullptr) != SQLITE_OK)
        return sqlError(db, "Cannot read schema version");
    if (sqlite3_step(version) == SQLITE_ROW) v = sqlite3_column_int(version, 0);
    sqlite3_finalize(version);
    if (v != 0 && v != kSchemaVersion)
        return {MaiErrorCode::NotSupported,
                "Legacy database must be migrated before formal journal open."};
    if (v == 0) {
        if (auto e = exec(db, "BEGIN IMMEDIATE")) return e;
        std::string nativeSchema = maiCodexBusinessSchemaSql();
        const auto errorColumn = nativeSchema.find("error_json TEXT,");
        if (errorColumn != std::string::npos)
            nativeSchema.replace(errorColumn, std::strlen("error_json TEXT,"),
                                 "mai_error_ref TEXT,error_json TEXT GENERATED ALWAYS AS "
                                 "(mai_body(mai_error_ref)) VIRTUAL,");
        if (auto e = exec(db, nativeSchema)) {
            exec(db, "ROLLBACK");
            return e;
        }
        if (auto e = exec(db, extensionSchema)) {
            exec(db, "ROLLBACK");
            return e;
        }
        if (auto e = exec(db, "COMMIT")) return e;
    }
    journal->mState->options = mutationOptions(db);
    journal->mState->recorder =
        std::make_unique<MaiSqliteProjectionMutationRecorder>(db, journal->mState->options);
    auto path = root.append(MaiFilePath::fromUtf8("sessions/.journal"))
                    .append(MaiFilePath::fromUtf8(indexName + ".jsonl"));
    if (v == kSchemaVersion && !MaiFileSystem::exists(path))
        return {MaiErrorCode::InvalidInput,
                "Formal projection has lost its authoritative commit log."};
    MaiResult<std::unique_ptr<MaiRolloutLog>> log =
        MaiFileSystem::exists(path) ? MaiRolloutLog::open(path)
                                    : MaiRolloutLog::create(path,
                                                            Json{{"id", "account-projection"},
                                                                 {"session_id", indexName},
                                                                 {"schema", kSchemaVersion}}
                                                                .dump(),
                                                            MaiTime::getCurrentTime());
    if (!log) return log.error();
    journal->mState->metadata = std::move(log.value());
    if (auto e = journal->readBarrier()) return e;
    return journal;
}
MaiResult<MaiRolloutRecord> MaiSessionJournal::readFirstRecord(const MaiFilePath& path,
                                                               std::uint64_t offset) {
    constexpr std::uint64_t limit = 64 * 1024 * 1024;
    auto reader = MaiFileReadHandle::open(path);
    if (!reader) return reader.error();
    for (std::uint64_t scanned = 0; scanned < limit;) {
        std::string bytes;
        if (auto error = reader.value()->readAt(
                offset + scanned, std::min<std::uint64_t>(1024 * 1024, limit - scanned), bytes))
            return error;
        if (bytes.empty())
            return {MaiErrorCode::InvalidInput, "Attempted source record is incomplete."};
        const auto lf = bytes.find('\n');
        if (lf != std::string::npos)
            return MaiRolloutLog::readAt(path, offset,
                                         scanned + static_cast<std::uint64_t>(lf) + 1);
        scanned += bytes.size();
    }
    return {MaiErrorCode::InvalidInput, "Attempted source record exceeds the record limit."};
}

MaiError MaiSessionJournal::begin() {
    auto& s = *mState;
    if (s.capturing) return {MaiErrorCode::Internal, "Nested projection transaction."};
    if (auto e = exec(s.db, "BEGIN IMMEDIATE")) return e;
    if (auto e = s.replay()) {
        exec(s.db, "ROLLBACK");
        s.projectionError = e;
        return e;
    }
    if (!s.recorder->beginCapture()) {
        exec(s.db, "ROLLBACK");
        return {MaiErrorCode::Internal, s.recorder->error()};
    }
    s.sourceError = {};
    s.capturing = true;
    return {};
}
MaiError MaiSessionJournal::commit() {
    auto& s = *mState;
    s.provisionalBodies.clear();
    if (!s.capturing) return {MaiErrorCode::Internal, "No captured projection transaction."};
    s.capturing = false;
    if (!s.recorder->endCapture()) {
        exec(s.db, "ROLLBACK");
        return {MaiErrorCode::Internal, s.recorder->error()};
    }
    if (s.sourceError) {
        exec(s.db, "ROLLBACK");
        return s.sourceError;
    }
    // 正文在暂存期间允许批量追加；任何业务确认之前先持久化所有实际来源文件。
    for (const auto& path : s.dirtyLogs) {
        auto found = s.logs.find(path);
        if (found == s.logs.end()) {
            exec(s.db, "ROLLBACK");
            return {MaiErrorCode::Internal, "Buffered source handle is unavailable."};
        }
        if (auto error = found->second->confirmDurable()) {
            exec(s.db, "ROLLBACK");
            s.sourceError = error;
            return error;
        }
    }
    s.dirtyLogs.clear();
    s.provisionalBodies.clear();
    const auto& changes = s.recorder->capturedChanges();
    if (changes.empty()) return exec(s.db, "COMMIT");
    std::string payload, error;
    if (!maiEncodeSqliteProjectionMutations(changes, payload, &error)) {
        exec(s.db, "ROLLBACK");
        return {MaiErrorCode::Internal, error};
    }
    const auto commitPayload = Json{
        {"type", "mai_projection_transaction"},
        {"transaction_id", MaiIdGenerator::generate("txn")},
        {"mutations",
         Json::parse(payload)}}.dump();
    MaiRolloutCursor beforeAppend;
    if (auto cursorError = s.cursor(beforeAppend)) {
        exec(s.db, "ROLLBACK");
        return cursorError;
    }
    auto appended = s.metadata->append("event_msg", commitPayload, MaiTime::getCurrentTime());
    MaiRolloutLocation committedLocation;
    if (!appended) {
        // 保持 SQLite 写锁直到证明完成，禁止其它 Store 在不确定操作解决前投影并追加。
        // 失败不表示没写；但重新 flush 旧前缀也不表示本事务存在。
        if (auto uncertain = s.metadata->confirmDurable()) {
            exec(s.db, "ROLLBACK");
            s.sourceError = uncertain;
            return uncertain;
        }
        std::uint64_t sourceSize = 0;
        if (!MaiFileSystem::fileSize(s.metadata->sourcePath(), sourceSize) ||
            sourceSize <= beforeAppend.byteOffset) {
            exec(s.db, "ROLLBACK");
            s.sourceError = appended.error();
            return appended.error();
        }
        auto attempted = readFirstRecord(s.metadata->sourcePath(), beforeAppend.byteOffset);
        if (!attempted || attempted.value().location.ordinal != beforeAppend.nextOrdinal ||
            attempted.value().type != "event_msg" || attempted.value().payload != commitPayload) {
            exec(s.db, "ROLLBACK");
            s.sourceError = appended.error();
            return appended.error();
        }
        committedLocation = attempted.value().location;
    } else
        committedLocation = appended.value();
    const auto& at = committedLocation;
    auto e = s.setCursor({at.byteOffset + at.byteLength, at.ordinal + 1});
    if (!e) e = exec(s.db, "COMMIT");
    if (e) {
        exec(s.db, "ROLLBACK");
        s.projectionError = e;
        return {};
    }
    s.projectionError = {};
    return {};
}
void MaiSessionJournal::rollback() {
    auto& s = *mState;
    s.provisionalBodies.clear();
    if (s.capturing) {
        s.recorder->endCapture();
        s.capturing = false;
    }
    if (!sqlite3_get_autocommit(s.db)) exec(s.db, "ROLLBACK");
}
MaiError MaiSessionJournal::readBarrier() {
    auto& s = *mState;
    if (!sqlite3_get_autocommit(s.db)) return {};
    MaiRolloutCursor cursor;
    if (auto e = s.cursor(cursor)) return e;
    std::uint64_t size = 0;
    if (!MaiFileSystem::fileSize(s.metadata->sourcePath(), size))
        return {MaiErrorCode::Internal, "Cannot stat authoritative metadata journal."};
    if (size == cursor.byteOffset) return {};
    if (auto e = exec(s.db, "BEGIN IMMEDIATE")) return e;
    auto e = s.replay();
    if (!e) e = exec(s.db, "COMMIT");
    if (e) {
        exec(s.db, "ROLLBACK");
        s.projectionError = e;
        return e;
    }
    s.projectionError = {};
    return {};
}
MaiError MaiSessionJournal::ensureSession(const std::string& sid, MaiMillis created,
                                          const MaiSession* snapshot) {
    auto& s = *mState;
    if (sid.empty()) return {MaiErrorCode::InvalidInput, "Session identity is required."};
    auto existing = s.pathFor(sid);
    if (existing) return {};
    if (existing.error().code() != MaiErrorCode::NotFound) return existing.error();
    auto id = MaiRolloutPath::create(created > 0 ? created : MaiTime::getCurrentTime());
    if (!id) return id.error();
    MaiSession persisted;
    bool metadataKnown = snapshot != nullptr;
    if (snapshot)
        persisted = *snapshot;
    else {
        persisted.id = sid;
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare_v2(s.db,
                               "SELECT directory,model,agent,parent_id,depth,reasoning_effort FROM "
                               "sessions WHERE id=?1",
                               -1, &query, nullptr) != SQLITE_OK)
            return sqlError(s.db, "Cannot inspect initial session metadata");
        sqlite3_bind_text64(query, 1, sid.data(), sid.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        if (sqlite3_step(query) == SQLITE_ROW) {
            metadataKnown = true;
            persisted.directory = text(query, 0);
            persisted.model = text(query, 1);
            persisted.agent = text(query, 2);
            persisted.parentId = text(query, 3);
            persisted.depth = sqlite3_column_int(query, 4);
            persisted.reasoningEffort = text(query, 5);
        }
        sqlite3_finalize(query);
    }
    Json metadata{
        {"id", id.value().id},
        {"session_id", sid},
        {"mai_schema_version", kSchemaVersion},
        {"cwd", metadataKnown ? Json(persisted.directory) : Json(nullptr)},
        {"model", metadataKnown ? Json(persisted.model) : Json(nullptr)},
        {"agent", metadataKnown ? Json(persisted.agent) : Json(nullptr)},
        {"parent_session_id", metadataKnown ? Json(persisted.parentId) : Json(nullptr)},
        {"depth", metadataKnown ? Json(persisted.depth) : Json(nullptr)},
        {"reasoning_effort", metadataKnown ? Json(persisted.reasoningEffort) : Json(nullptr)},
        {"source", "maiagent"},
        {"model_provider", nullptr},
        {"base_instructions", nullptr},
        {"creator_user_id", nullptr},
        {"creator_account_id", nullptr},
        {"originator", nullptr},
        {"cli_version", nullptr}};
    // 创建时尚无请求配置或账号身份；不根据目录、模型名或主机环境补造这些事实。
    // 真实 developer 头到达后，以同一源中的关联更新保存全文，不覆盖这个不可变前缀。
    auto log = MaiRolloutLog::create(s.root.append(id.value().relativePath), metadata.dump(),
                                     created > 0 ? created : MaiTime::getCurrentTime());
    if (!log) return log.error();
    auto e = bindStep(s.db, "INSERT INTO mai_session_rollouts VALUES(?1,?2,?3)",
                      {sid, pathText(id.value().relativePath), id.value().id});
    if (e) return e;
    s.logs.emplace(pathText(id.value().relativePath), std::move(log.value()));
    return {};
}
MaiResult<std::string> MaiSessionJournal::appendBody(const std::string& sid,
                                                     const std::string& bytes, bool binary) {
    if (bytes.empty()) return binary ? std::string("@mai_empty_blob_v1") : std::string{};
    auto& s = *mState;
    if (auto e = ensureSession(sid)) {
        s.sourceError = e;
        return e;
    }
    auto log = s.logFor(sid);
    if (!log) {
        s.sourceError = log.error();
        return log.error();
    }
    auto r = log.value()->appendBuffered(
        "event_msg",
        Json{{"type", "mai_body"}, {"bytes", binary ? Json{{"hex", hex(bytes)}} : bytesJson(bytes)}}
            .dump(),
        MaiTime::getCurrentTime());
    if (!r) {
        s.sourceError = r.error();
        return r.error();
    }
    auto path = s.pathFor(sid);
    s.dirtyLogs.insert(path.value());
    const auto& at = r.value();
    auto reference =
        Json::array({path.value(), at.byteOffset, at.byteLength, at.ordinal, binary}).dump();
    if (bytes.size() <= 65536) {
        if (s.provisionalBodies.size() >= 16)
            s.provisionalBodies.erase(s.provisionalBodies.begin());
        s.provisionalBodies[reference] = bytes;
    }
    return reference;
}
MaiResult<std::string> MaiSessionJournal::appendOutput(const std::string& sid,
                                                       const std::string& bytes) {
    Json chunks = Json::array();
    for (std::size_t offset = 0; offset < bytes.size(); offset += kOutputChunk) {
        auto r = appendBody(
            sid, bytes.substr(offset, std::min(kOutputChunk, bytes.size() - offset)), true);
        if (!r) return r.error();
        auto ref = Json::parse(r.value());
        chunks.push_back(Json::array({ref[1], ref[2], ref[3]}));
    }
    auto path = mState->pathFor(sid);
    if (!path) return path.error();
    return Json{{"path", path.value()},
                {"size", bytes.size()},
                {"chunk_size", kOutputChunk},
                {"chunks", chunks}}
        .dump();
}
MaiError MaiSessionJournal::appendRecord(const std::string& sid, const std::string& type,
                                         const std::string& payload) {
    auto& s = *mState;
    if (auto e = ensureSession(sid)) return e;
    std::string sourceKey, hash;
    if (type == "response_item") {
        auto item = Json::parse(payload, nullptr, false);
        if (!item.is_object())
            return {MaiErrorCode::InvalidInput, "Native response item must be an object."};
        const auto kind = item.value("type", std::string{}),
                   role = item.value("role", std::string{});
        const auto message = item.value("source_message_id", std::string{}),
                   part = item.value("source_part_id", std::string{}),
                   id = item.value("id", std::string{});
        if (!message.empty())
            sourceKey = Json::array({"source", kind, role, message, part}).dump();
        else if (!id.empty())
            sourceKey = Json::array({"native", kind, id}).dump();
        else if (kind == "additional_tools" || kind == "developer" || role == "developer")
            sourceKey = Json::array({"header", kind, role}).dump();
        if (!sourceKey.empty()) {
            hash = payloadHash(payload);
            if (hash.empty()) return {MaiErrorCode::Internal, "Cannot fingerprint native source."};
            sqlite3_stmt* prior = nullptr;
            if (sqlite3_prepare_v2(s.db,
                                   "SELECT payload_hash FROM mai_session_record_keys WHERE "
                                   "session_id=?1 AND source_key=?2",
                                   -1, &prior, nullptr) != SQLITE_OK)
                return sqlError(s.db, "Cannot prepare native source identity");
            sqlite3_bind_text64(prior, 1, sid.data(), sid.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            sqlite3_bind_text64(prior, 2, sourceKey.data(), sourceKey.size(), SQLITE_TRANSIENT,
                                SQLITE_UTF8);
            int code = sqlite3_step(prior);
            bool same = code == SQLITE_ROW && text(prior, 0) == hash;
            sqlite3_finalize(prior);
            if (same) return {};
            if (code != SQLITE_ROW && code != SQLITE_DONE)
                return sqlError(s.db, "Cannot read native source identity");
        }
    }
    auto log = s.logFor(sid);
    if (!log) return log.error();
    auto r = log.value()->appendBuffered(type, payload, MaiTime::getCurrentTime());
    if (!r) {
        s.sourceError = r.error();
        return r.error();
    }
    auto path = s.pathFor(sid);
    s.dirtyLogs.insert(path.value());
    const auto& at = r.value();
    if (auto error = bindStep(
            s.db,
            "INSERT INTO mai_session_records(session_id,type,path,byte_offset,byte_len,ordinal) "
            "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(session_id,type) DO UPDATE SET "
            "path=excluded.path,byte_offset=excluded.byte_offset,byte_len=excluded.byte_len,"
            "ordinal=excluded.ordinal",
            {sid, type, path.value(), std::to_string(at.byteOffset), std::to_string(at.byteLength),
             std::to_string(at.ordinal)}))
        return error;
    if (type == "response_item") {
        auto item = Json::parse(payload, nullptr, false);
        if (item.is_object() && item.value("role", std::string{}) == "developer") {
            Json instructions = nullptr;
            if (item.contains("content") && item["content"].is_string())
                instructions = item["content"];
            else if (item.contains("content") && item["content"].is_array()) {
                std::string fullText;
                bool known = true;
                for (const auto& part : item["content"]) {
                    if (!part.is_object() || !part.contains("text") || !part["text"].is_string()) {
                        known = false;
                        break;
                    }
                    fullText += part["text"].get<std::string>();
                }
                if (known) instructions = std::move(fullText);
            }
            const Json update{{"type", "session_metadata_updated"},
                              {"session_id", sid},
                              {"base_instructions", {{"text", instructions}}},
                              {"source_response_item",
                               {{"ordinal", at.ordinal},
                                {"byte_offset", at.byteOffset},
                                {"byte_len", at.byteLength}}}};
            auto updated =
                log.value()->appendBuffered("event_msg", update.dump(), MaiTime::getCurrentTime());
            if (!updated) {
                s.sourceError = updated.error();
                return updated.error();
            }
            const auto& updateAt = updated.value();
            if (auto error = bindStep(
                    s.db,
                    "INSERT INTO "
                    "mai_session_records(session_id,type,path,byte_offset,byte_len,ordinal) "
                    "VALUES(?1,'event_msg',?2,?3,?4,?5) ON CONFLICT(session_id,type) DO UPDATE SET "
                    "path=excluded.path,byte_offset=excluded.byte_offset,byte_len=excluded.byte_"
                    "len,ordinal=excluded.ordinal",
                    {sid, path.value(), std::to_string(updateAt.byteOffset),
                     std::to_string(updateAt.byteLength), std::to_string(updateAt.ordinal)}))
                return error;
        }
    }
    if (!sourceKey.empty())
        return bindStep(s.db,
                        "INSERT INTO mai_session_record_keys(session_id,source_key,payload_hash) "
                        "VALUES(?1,?2,?3) ON CONFLICT(session_id,source_key) DO UPDATE SET "
                        "payload_hash=excluded.payload_hash",
                        {sid, sourceKey, hash});
    // 原生用量只保存累计计数，原始细节及窗口仍由同一条权威记录提供。
    if (type == "token_usage_record") {
        auto value = Json::parse(payload, nullptr, false);
        if (value.is_object()) {
            const auto total =
                value.value("thread_token_usage", Json::object()).value("total_tokens", Json{});
            if (total.is_number_unsigned() || total.is_number_integer())
                return bindStep(s.db, "UPDATE threads SET tokens_used=?2 WHERE id=?1",
                                {sid, std::to_string(total.get<std::uint64_t>())});
        }
    }
    return {};
}
MaiResult<std::string> MaiSessionJournal::latestRecord(const std::string& sid,
                                                       const std::string& type) {
    auto& s = *mState;
    if (auto e = readBarrier()) return e;
    sqlite3_stmt* query = nullptr;
    if (sqlite3_prepare_v2(s.db,
                           "SELECT path,byte_offset,byte_len,ordinal FROM mai_session_records "
                           "WHERE session_id=?1 AND type=?2",
                           -1, &query, nullptr) != SQLITE_OK)
        return sqlError(s.db, "Cannot prepare native source lookup");
    sqlite3_bind_text64(query, 1, sid.data(), sid.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    sqlite3_bind_text64(query, 2, type.data(), type.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    int result = sqlite3_step(query);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(query);
        return std::string{};
    }
    if (result != SQLITE_ROW) {
        sqlite3_finalize(query);
        return sqlError(s.db, "Cannot read native source lookup");
    }
    auto path = s.sourcePath(text(query, 0));
    MaiRolloutLocation at{static_cast<std::uint64_t>(sqlite3_column_int64(query, 3)),
                          static_cast<std::uint64_t>(sqlite3_column_int64(query, 1)),
                          static_cast<std::uint64_t>(sqlite3_column_int64(query, 2))};
    sqlite3_finalize(query);
    if (!path) return path.error();
    auto record = MaiRolloutLog::read(path.value(), at);
    if (!record) return record.error();
    if (record.value().type != type)
        return {MaiErrorCode::InvalidInput, "Native source type does not match its index."};
    return record.value().payload;
}
MaiError MaiSessionJournal::projectSession(const MaiSession& session) {
    auto& s = *mState;
    if (auto e = ensureSession(session.id, session.created, &session)) return e;
    auto path = s.pathFor(session.id);
    return bindStep(
        s.db,
        "INSERT INTO "
        "threads(id,rollout_path,created_at,updated_at,source,model_provider,cwd,title,sandbox_"
        "policy,approval_mode,model,reasoning_effort,created_at_ms,updated_at_ms,history_mode) "
        "VALUES(?1,?2,?3,?4,'maichat','',?5,?6,'unknown','unknown',?7,?8,?9,?10,'rollout') ON "
        "CONFLICT(id) DO UPDATE SET "
        "updated_at=excluded.updated_at,cwd=excluded.cwd,title=excluded.title,model=excluded.model,"
        "reasoning_effort=excluded.reasoning_effort,updated_at_ms=excluded.updated_at_ms",
        {session.id, path.value(), std::to_string(session.created / 1000),
         std::to_string(session.updated / 1000), session.directory, session.title, session.model,
         session.reasoningEffort, std::to_string(session.created),
         std::to_string(session.updated)});
}
MaiError MaiSessionJournal::projectMessage(const std::string& sid, const MaiMessage& message) {
    auto& s = *mState;
    if (auto e = ensureSession(sid)) return e;
    auto log = s.logFor(sid);
    if (!log) return log.error();
    auto path = s.pathFor(sid);
    if (!path) return path.error();
    const auto turn = message.turnId.empty() ? message.id : message.turnId;
    MaiRolloutLocation lastLocation;
    bool hasLocation = false;
    if (message.turnOutcome) {
        auto state = MaiSessionRecordCodec::encodeTurnState(message);
        if (!state) return state.error();
        auto location = log.value()->appendBuffered("event_msg",
                                                    Json{{"type", "mai_turn_metadata"},
                                                         {"thread_id", sid},
                                                         {"message_id", message.id},
                                                         {"state", Json::parse(state.value())}}
                                                        .dump(),
                                                    MaiTime::getCurrentTime());
        if (!location) return location.error();
        s.dirtyLogs.insert(path.value());
        const auto& at = location.value();
        lastLocation = at;
        hasLocation = true;
        std::string errorReference;
        if (message.turnOutcome->error) {
            auto encodedError = appendBody(
                sid, Json{{"code", maiErrorCodeToString(message.turnOutcome->error.code())},
                          {"message", bytesJson(message.turnOutcome->error.message())}}
                         .dump());
            if (!encodedError) return encodedError.error();
            errorReference = encodedError.value();
            const auto reference = Json::parse(errorReference);
            lastLocation = {reference[3].get<std::uint64_t>(), reference[1].get<std::uint64_t>(),
                            reference[2].get<std::uint64_t>()};
        }
        auto error = bindStep(
            s.db,
            "INSERT INTO "
            "thread_turns(thread_id,turn_id,rollout_ordinal,status,mai_error_ref,started_at,"
            "completed_at,duration_ms,final_agent_item_id,rollout_byte_offset,rollout_end_ordinal,"
            "rollout_end_byte_offset,root_turn_id) "
            "VALUES(?1,?2,?3,?4,NULLIF(?5,''),NULLIF(CAST(?6 AS INTEGER),0),NULLIF(CAST(?7 AS "
            "INTEGER),0),CASE WHEN CAST(?8 AS INTEGER)<0 THEN NULL "
            "ELSE ?8 END,NULL,?10,CASE WHEN ?4='running'  THEN NULL ELSE ?3 END,CASE WHEN "
            "?4='running'  "
            "THEN NULL ELSE ?11 END,?2) ON CONFLICT(thread_id,turn_id) DO UPDATE SET "
            "status=excluded.status,mai_error_ref=excluded.mai_error_ref,completed_at=excluded."
            "completed_at,duration_ms=excluded.duration_ms,final_agent_item_id=excluded.final_"
            "agent_item_id,rollout_end_ordinal=excluded.rollout_end_ordinal,rollout_end_byte_"
            "offset=excluded.rollout_end_byte_offset",
            {sid, turn, std::to_string(at.ordinal),
             maiTurnStatusToString(message.turnOutcome->status), errorReference,
             std::to_string(message.created / 1000), std::to_string(message.completed / 1000),
             std::to_string(message.completed > 0 && message.created > 0
                                ? message.completed - message.created
                                : -1),
             message.turnOutcome->resultMessageId, std::to_string(at.byteOffset),
             std::to_string(at.byteOffset + at.byteLength)});
        if (error) return error;
    }
    std::vector<Json> items;
    if (message.role == MaiRole::User) {
        auto encoded = MaiSessionRecordCodec::encodeMessage(message);
        if (!encoded) return encoded.error();
        items.push_back(Json{{"id", message.id},
                             {"type", "userMessage"},
                             {"mai_message", Json::parse(encoded.value())}});
    } else
        for (const auto& part : message.parts) {
            if (part.modelDraft || maiIsResponseBoundaryPart(part)) continue;
            std::string kind = "agentMessage";
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                if (tool->state == MaiToolState::Pending || tool->state == MaiToolState::Running)
                    continue;
                kind = "dynamicToolCall";
            } else if (std::holds_alternative<MaiReasoningPart>(part.body))
                kind = "reasoning";
            else if (std::holds_alternative<MaiImagePart>(part.body))
                kind = "image";
            else if (std::holds_alternative<MaiVideoPart>(part.body))
                kind = "video";
            else if (std::holds_alternative<MaiQuotePart>(part.body))
                kind = "quote";
            auto encoded = MaiSessionRecordCodec::encodePart(part);
            if (!encoded) return encoded.error();
            items.push_back(Json{{"id", part.id},
                                 {"type", kind},
                                 {"mai_message_id", message.id},
                                 {"mai_part", Json::parse(encoded.value())}});
        }
    for (const auto& item : items) {
        const auto itemId = item.at("id").get<std::string>();
        const auto hash = payloadHash(item.dump());
        if (hash.empty())
            return {MaiErrorCode::Internal, "Cannot fingerprint completed application item."};
        const auto sourceKey = Json::array({"application_item", turn, itemId}).dump();
        sqlite3_stmt* prior = nullptr;
        if (sqlite3_prepare_v2(s.db,
                               "SELECT payload_hash FROM mai_session_record_keys WHERE "
                               "session_id=?1 AND source_key=?2",
                               -1, &prior, nullptr) != SQLITE_OK)
            return sqlError(s.db, "Cannot prepare completed item identity");
        sqlite3_bind_text64(prior, 1, sid.data(), sid.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        sqlite3_bind_text64(prior, 2, sourceKey.data(), sourceKey.size(), SQLITE_TRANSIENT,
                            SQLITE_UTF8);
        const int priorCode = sqlite3_step(prior);
        bool same = priorCode == SQLITE_ROW && text(prior, 0) == hash;
        sqlite3_finalize(prior);
        if (same) continue;
        if (priorCode != SQLITE_ROW && priorCode != SQLITE_DONE)
            return sqlError(s.db, "Cannot read completed item identity");
        auto location = log.value()->appendBuffered(
            "event_msg",
            Json{{"type", "item_completed"}, {"thread_id", sid}, {"turn_id", turn}, {"item", item}}
                .dump(),
            MaiTime::getCurrentTime());
        if (!location) return location.error();
        s.dirtyLogs.insert(path.value());
        const auto& at = location.value();
        lastLocation = at;
        hasLocation = true;
        if (auto error = bindStep(
                s.db,
                "INSERT INTO "
                "thread_items(thread_id,turn_id,item_id,rollout_ordinal,created_at_ms,item_type,"
                "updated_at_ordinal,started_at_ms,completed_at_ms,mai_rollout_path,byte_offset,"
                "byte_len) VALUES(?1,?2,?3,?4,?5,?6,?4,?5,NULLIF(CAST(?7 AS INTEGER),0),?8,?9,?10) "
                "ON "
                "CONFLICT(thread_id,turn_id,item_id) DO UPDATE SET "
                "updated_at_ordinal=excluded.updated_at_ordinal,completed_at_ms=excluded.completed_"
                "at_ms,mai_rollout_path=excluded.mai_rollout_path,byte_offset=excluded.byte_offset,"
                "byte_len=excluded.byte_len",
                {sid, turn, itemId, std::to_string(at.ordinal), std::to_string(message.created),
                 item.at("type").get<std::string>(), std::to_string(message.completed),
                 path.value(), std::to_string(at.byteOffset), std::to_string(at.byteLength)}))
            return error;
        if (auto error =
                bindStep(s.db,
                         "INSERT INTO mai_session_record_keys(session_id,source_key,payload_hash) "
                         "VALUES(?1,?2,?3) ON CONFLICT(session_id,source_key) DO UPDATE SET "
                         "payload_hash=excluded.payload_hash",
                         {sid, sourceKey, hash}))
            return error;
        if (auto error =
                bindStep(s.db,
                         "INSERT INTO "
                         "thread_history_projection_state(thread_id,next_rollout_byte_offset,next_"
                         "rollout_ordinal) VALUES(?1,?2,?3) ON CONFLICT(thread_id) DO UPDATE SET "
                         "next_rollout_byte_offset=excluded.next_rollout_byte_offset,next_rollout_"
                         "ordinal=excluded.next_rollout_ordinal",
                         {sid, std::to_string(at.byteOffset + at.byteLength),
                          std::to_string(at.ordinal + 1)}))
            return error;
    }
    if (message.turnOutcome && message.turnOutcome->status != MaiTurnStatus::Running &&
        hasLocation) {
        // outcome 的结果编号属于消息；原生最终 item 编号属于片段，不能把两者直接混用。
        if (auto error =
                bindStep(s.db,
                         "UPDATE thread_turns SET final_agent_item_id=(SELECT item_id FROM "
                         "thread_items WHERE thread_id=?1 AND turn_id=?2 AND "
                         "item_type='agentMessage' ORDER BY updated_at_ordinal DESC LIMIT "
                         "1),rollout_end_ordinal=?3,rollout_end_byte_offset=?4 WHERE thread_id=?1 "
                         "AND turn_id=?2",
                         {sid, turn, std::to_string(lastLocation.ordinal),
                          std::to_string(lastLocation.byteOffset + lastLocation.byteLength)}))
            return error;
    }
    return {};
}
MaiError MaiSessionJournal::projectionError() const {
    return mState->projectionError;
}
MaiError MaiSessionJournal::sourceError() const {
    return mState->sourceError;
}
const MaiFilePath& MaiSessionJournal::root() const {
    return mState->root;
}

namespace {
struct CloseDatabase {
    void operator()(sqlite3* db) const {
        if (db) sqlite3_close(db);
    }
};
using Database = std::unique_ptr<sqlite3, CloseDatabase>;
MaiSqliteProjectionValue legacyValue(sqlite3_stmt* row, int i) {
    MaiSqliteProjectionValue v;
    switch (sqlite3_column_type(row, i)) {
        case SQLITE_INTEGER:
            v.type = MaiSqliteProjectionValueType::Integer;
            v.integer = sqlite3_column_int64(row, i);
            break;
        case SQLITE_FLOAT:
            v.type = MaiSqliteProjectionValueType::Real;
            v.real = sqlite3_column_double(row, i);
            break;
        case SQLITE_TEXT:
            v.type = MaiSqliteProjectionValueType::Text;
            v.bytes = text(row, i);
            break;
        case SQLITE_BLOB: {
            v.type = MaiSqliteProjectionValueType::Blob;
            auto* p = static_cast<const char*>(sqlite3_column_blob(row, i));
            auto n = sqlite3_column_bytes(row, i);
            if (n) v.bytes.assign(p, static_cast<std::size_t>(n));
            break;
        }
        default: break;
    }
    return v;
}
void bindValue(sqlite3_stmt* s, int i, const MaiSqliteProjectionValue& v) {
    switch (v.type) {
        case MaiSqliteProjectionValueType::Null: sqlite3_bind_null(s, i); break;
        case MaiSqliteProjectionValueType::Integer: sqlite3_bind_int64(s, i, v.integer); break;
        case MaiSqliteProjectionValueType::Real: sqlite3_bind_double(s, i, v.real); break;
        case MaiSqliteProjectionValueType::Text:
            sqlite3_bind_text64(s, i, v.bytes.data(), v.bytes.size(), SQLITE_TRANSIENT,
                                SQLITE_UTF8);
            break;
        case MaiSqliteProjectionValueType::Blob:
            sqlite3_bind_blob64(s, i, v.bytes.data(), v.bytes.size(), SQLITE_TRANSIENT);
            break;
    }
}
MaiError importLegacy(sqlite3* source, sqlite3* target, MaiSessionJournal& journal,
                      const MaiSqliteStoreOpenOptions& options) {
    const auto canceled = [&]() { return options.isCanceled && options.isCanceled(); };
    const auto cancelError = []() {
        return MaiError{MaiErrorCode::Canceled, "Legacy session migration was canceled."};
    };
    if (canceled()) return cancelError();
    auto sessions = MaiLegacySessionReader::readSessions(source);
    if (!sessions) return sessions.error();
    // 先按旧领域读取器验证所有权及来源关系；未知列仍在后续原始行归档中逐字节保留。
    for (const auto& session : sessions.value()) {
        if (canceled()) return cancelError();
        auto snapshot = MaiLegacySessionReader::readSession(source, session.id);
        if (!snapshot) return snapshot.error();
    }
    sqlite3_stmt* tables = nullptr;
    if (sqlite3_prepare_v2(source,
                           "SELECT name,sql FROM sqlite_master WHERE type='table' AND name NOT "
                           "LIKE 'sqlite_%' ORDER BY name",
                           -1, &tables, nullptr) != SQLITE_OK)
        return sqlError(source, "Cannot enumerate legacy tables");
    std::vector<std::pair<std::string, std::string>> names;
    while (sqlite3_step(tables) == SQLITE_ROW) names.emplace_back(text(tables, 0), text(tables, 1));
    sqlite3_finalize(tables);
    if (auto e = journal.begin()) return e;
    const auto reject = [&](MaiError e) {
        journal.rollback();
        return e;
    };
    for (const auto& session : sessions.value())
        if (auto e = journal.ensureSession(session.id, session.created, &session)) return reject(e);
    const std::string archiveSession = "mai_legacy_archive";
    if (auto e = journal.ensureSession(archiveSession)) return reject(e);
    for (const auto& table : names) {
        if (canceled()) return reject(cancelError());
        if (options.progress) options.progress("migration_table:" + table.first);
        sqlite3_stmt* fields = nullptr;
        std::set<std::string> writable;
        if (sqlite3_prepare_v2(target, ("PRAGMA table_xinfo(" + quote(table.first) + ")").c_str(),
                               -1, &fields, nullptr) != SQLITE_OK)
            return reject(sqlError(target, "Cannot inspect target fields"));
        while (sqlite3_step(fields) == SQLITE_ROW)
            if (sqlite3_column_int(fields, 6) == 0) writable.insert(text(fields, 1));
        sqlite3_finalize(fields);
        if (!isLegacyDomainTable(table.first)) writable.clear();
        sqlite3_stmt* rows = nullptr;
        if (sqlite3_prepare_v2(source, ("SELECT * FROM " + quote(table.first)).c_str(), -1, &rows,
                               nullptr) != SQLITE_OK)
            return reject(sqlError(source, "Cannot prepare legacy rows"));
        int code;
        while ((code = sqlite3_step(rows)) == SQLITE_ROW) {
            if (canceled()) {
                sqlite3_finalize(rows);
                return reject(cancelError());
            }
            MaiSqliteProjectionMutation archive;
            archive.table = table.first;
            std::map<std::string, MaiSqliteProjectionValue> values;
            for (int i = 0; i < sqlite3_column_count(rows); ++i) {
                std::string name = sqlite3_column_name(rows, i);
                archive.columns.push_back(name);
                archive.newValues.push_back(legacyValue(rows, i));
                values.emplace(name, archive.newValues.back());
            }
            std::string encoded, error;
            if (!maiEncodeSqliteProjectionMutations({archive}, encoded, &error)) {
                sqlite3_finalize(rows);
                return reject({MaiErrorCode::Internal, error});
            }
            std::string sid = archiveSession;
            for (const char* field : {"session_id", "owner_session_id"}) {
                auto i = values.find(field);
                if (i != values.end() && i->second.type == MaiSqliteProjectionValueType::Text)
                    sid = i->second.bytes;
            }
            if (table.first == "sessions") sid = values["id"].bytes;
            if (table.first == "parts") {
                auto id = values["message_id"].bytes;
                sqlite3_stmt* owner = nullptr;
                sqlite3_prepare_v2(source, "SELECT session_id FROM messages WHERE id=?1", -1,
                                   &owner, nullptr);
                sqlite3_bind_text64(owner, 1, id.data(), id.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
                if (sqlite3_step(owner) == SQLITE_ROW) sid = text(owner, 0);
                sqlite3_finalize(owner);
            }
            if (auto e = journal.appendRecord(sid, "event_msg",
                                              Json{{"type", "mai_legacy_row"},
                                                   {"table_definition", table.second},
                                                   {"row", Json::parse(encoded)}}
                                                  .dump())) {
                sqlite3_finalize(rows);
                return reject(e);
            }
            if (writable.empty()) continue;
            std::vector<std::pair<std::string, MaiSqliteProjectionValue>> insertValues;
            for (const auto& original : values) {
                std::pair<std::string, MaiSqliteProjectionValue> entry(original.first,
                                                                       original.second);
                const auto bodies = bodyColumns().find(table.first);
                const bool body =
                    bodies != bodyColumns().end() && bodies->second.count(entry.first);
                if (body) {
                    if (entry.second.type != MaiSqliteProjectionValueType::Null) {
                        auto ref = journal.appendBody(
                            sid, entry.second.bytes,
                            entry.second.type == MaiSqliteProjectionValueType::Blob);
                        if (!ref) {
                            sqlite3_finalize(rows);
                            return reject(ref.error());
                        }
                        entry.second.type = MaiSqliteProjectionValueType::Text;
                        entry.second.bytes = ref.value();
                    }
                    entry.first += "_ref";
                }
                if (table.first == "tool_outputs" && entry.first == "payload") {
                    MaiSqliteProjectionValue size;
                    size.type = MaiSqliteProjectionValueType::Integer;
                    size.integer = static_cast<std::int64_t>(entry.second.bytes.size());
                    insertValues.emplace_back("payload_size", size);
                    if (entry.second.type != MaiSqliteProjectionValueType::Null) {
                        auto ref = journal.appendOutput(sid, entry.second.bytes);
                        if (!ref) {
                            sqlite3_finalize(rows);
                            return reject(ref.error());
                        }
                        entry.second.type = MaiSqliteProjectionValueType::Text;
                        entry.second.bytes = ref.value();
                    }
                    entry.first = "payload_ref";
                }
                if (writable.count(entry.first)) insertValues.push_back(std::move(entry));
            }
            std::string sql = "INSERT INTO " + quote(table.first) + "(";
            for (std::size_t i = 0; i < insertValues.size(); ++i) {
                if (i) sql += ',';
                sql += quote(insertValues[i].first);
            }
            sql += ") VALUES(";
            for (std::size_t i = 0; i < insertValues.size(); ++i) {
                if (i) sql += ',';
                sql += '?' + std::to_string(i + 1);
            }
            sql += ')';
            sqlite3_stmt* insert = nullptr;
            if (sqlite3_prepare_v2(target, sql.c_str(), -1, &insert, nullptr) != SQLITE_OK) {
                sqlite3_finalize(rows);
                return reject(sqlError(target, "Cannot prepare legacy projection"));
            }
            for (std::size_t i = 0; i < insertValues.size(); ++i)
                bindValue(insert, static_cast<int>(i + 1), insertValues[i].second);
            int result = sqlite3_step(insert);
            sqlite3_finalize(insert);
            if (result != SQLITE_DONE) {
                sqlite3_finalize(rows);
                return reject(sqlError(target, "Cannot insert legacy projection"));
            }
        }
        sqlite3_finalize(rows);
        if (code != SQLITE_DONE) return reject(sqlError(source, "Cannot read legacy rows"));
    }
    for (const auto& session : sessions.value()) {
        if (auto e = journal.projectSession(session)) return reject(e);
        auto snapshot = MaiLegacySessionReader::readSession(source, session.id);
        if (!snapshot) return reject(snapshot.error());
        // 隐藏导入仍完整存在原始归档与扩展索引；原生历史仅投影当前可见快照。
        for (const auto& message : snapshot.value().messages)
            if (auto error = journal.projectMessage(session.id, message)) return reject(error);
    }
    return journal.commit();
}
}  // namespace
MaiError MaiSessionJournal::migrateLegacy(const MaiFilePath& path, const MaiFilePath& root,
                                          const MaiSqliteStoreOpenOptions& openOptions) {
    if (openOptions.progress) openOptions.progress("migrating");
    MaiSessionMigrationOptions options;
    options.storageRoot = root;
    options.sourceIndexPath = path;
    options.isCanceled = openOptions.isCanceled;
    options.restartIncompleteBuild = true;
    options.checkpoint = [&](const std::string& phase) -> MaiError {
        if (openOptions.progress) openOptions.progress("migration_" + phase);
        return {};
    };
    const auto indexName = path.baseName().toUtf8();
    options.build = [&](sqlite3* source, const MaiFilePath& stage,
                        const MaiFilePath& index) -> MaiError {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(index.toUtf8().c_str(), &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                            nullptr) != SQLITE_OK) {
            if (raw) sqlite3_close(raw);
            return {MaiErrorCode::Internal, "Cannot create staged projection."};
        }
        Database db(raw);
        auto opened = MaiSessionJournal::open(raw, stage, indexName);
        if (!opened) return opened.error();
        if (openOptions.progress) openOptions.progress("migration_build");
        auto e = importLegacy(source, raw, *opened.value(), openOptions);
        opened.value().reset();
        return e;
    };
    options.validate = [&](sqlite3* source, const MaiFilePath& candidateRoot,
                           const MaiFilePath& candidateIndex) -> MaiError {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(candidateIndex.toUtf8().c_str(), &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            if (raw) sqlite3_close(raw);
            return {MaiErrorCode::Internal, "Cannot open staged projection for validation."};
        }
        Database db(raw);
        auto journal = MaiSessionJournal::open(raw, candidateRoot, indexName);
        if (!journal) return journal.error();
        if (openOptions.isCanceled && openOptions.isCanceled())
            return {MaiErrorCode::Canceled, "Legacy migration validation was canceled."};
        if (openOptions.progress) openOptions.progress("migration_validate");
        // 对每个已知普通列逐行验证，不只核对可见消息数；未知字段的原始类型/字节已归档。
        sqlite3_stmt* tables = nullptr;
        if (sqlite3_prepare_v2(
                source,
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
                -1, &tables, nullptr) != SQLITE_OK)
            return sqlError(source, "Cannot enumerate validation tables");
        std::vector<std::string> names;
        while (sqlite3_step(tables) == SQLITE_ROW) names.push_back(text(tables, 0));
        sqlite3_finalize(tables);
        for (const auto& table : names) {
            if (!isLegacyDomainTable(table)) continue;
            sqlite3_stmt* info = nullptr;
            std::set<std::string> columns;
            std::vector<std::string> keys;
            sqlite3_prepare_v2(raw, ("PRAGMA table_xinfo(" + quote(table) + ")").c_str(), -1, &info,
                               nullptr);
            while (sqlite3_step(info) == SQLITE_ROW) {
                columns.insert(text(info, 1));
                if (sqlite3_column_int(info, 5) > 0) keys.push_back(text(info, 1));
            }
            sqlite3_finalize(info);
            if (columns.empty()) continue;
            sqlite3_stmt* rows = nullptr;
            sqlite3_prepare_v2(source, ("SELECT * FROM " + quote(table)).c_str(), -1, &rows,
                               nullptr);
            int code;
            std::size_t count = 0;
            while ((code = sqlite3_step(rows)) == SQLITE_ROW) {
                ++count;
                std::map<std::string, MaiSqliteProjectionValue> values;
                for (int i = 0; i < sqlite3_column_count(rows); ++i)
                    values.emplace(sqlite3_column_name(rows, i), legacyValue(rows, i));
                std::vector<std::string> compare;
                for (const auto& v : values)
                    if (columns.count(v.first) || table == "tool_outputs" && v.first == "payload")
                        compare.push_back(v.first);
                std::string sql = "SELECT ";
                for (std::size_t i = 0; i < compare.size(); ++i) {
                    if (i) sql += ',';
                    sql += table == "tool_outputs" && compare[i] == "payload" ? "payload_ref"
                                                                              : quote(compare[i]);
                }
                sql += " FROM " + quote(table) + " WHERE ";
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i) sql += " AND ";
                    sql += quote(keys[i]) + " IS ?" + std::to_string(i + 1);
                }
                sqlite3_stmt* candidate = nullptr;
                if (keys.empty() ||
                    sqlite3_prepare_v2(raw, sql.c_str(), -1, &candidate, nullptr) != SQLITE_OK) {
                    sqlite3_finalize(rows);
                    return sqlError(raw, "Cannot prepare value validation");
                }
                for (std::size_t i = 0; i < keys.size(); ++i)
                    bindValue(candidate, static_cast<int>(i + 1), values[keys[i]]);
                if (sqlite3_step(candidate) != SQLITE_ROW) {
                    sqlite3_finalize(candidate);
                    sqlite3_finalize(rows);
                    return {MaiErrorCode::InvalidInput, "Migrated source row is missing."};
                }
                for (std::size_t i = 0; i < compare.size(); ++i) {
                    auto actual = legacyValue(candidate, static_cast<int>(i));
                    const auto& expected = values[compare[i]];
                    if (table == "tool_outputs" && compare[i] == "payload" &&
                        expected.type != MaiSqliteProjectionValueType::Null) {
                        auto ref = Json::parse(actual.bytes);
                        std::string restored;
                        for (std::size_t at = 0; at < expected.bytes.size(); at += kOutputChunk) {
                            sqlite3_stmt* slice = nullptr;
                            sqlite3_prepare_v2(raw, "SELECT mai_output_slice(?1,?2,?3)", -1, &slice,
                                               nullptr);
                            sqlite3_bind_text64(slice, 1, actual.bytes.data(), actual.bytes.size(),
                                                SQLITE_TRANSIENT, SQLITE_UTF8);
                            sqlite3_bind_int64(slice, 2, static_cast<sqlite3_int64>(at));
                            sqlite3_bind_int(slice, 3, static_cast<int>(kOutputChunk));
                            if (sqlite3_step(slice) != SQLITE_ROW) {
                                sqlite3_finalize(slice);
                                sqlite3_finalize(candidate);
                                sqlite3_finalize(rows);
                                return {MaiErrorCode::InvalidInput,
                                        "Migrated output source cannot be read."};
                            }
                            restored += legacyValue(slice, 0).bytes;
                            sqlite3_finalize(slice);
                        }
                        actual = expected;
                        actual.bytes = std::move(restored);
                    }
                    if (actual.type != expected.type || actual.integer != expected.integer ||
                        actual.real != expected.real || actual.bytes != expected.bytes) {
                        sqlite3_finalize(candidate);
                        sqlite3_finalize(rows);
                        return {MaiErrorCode::InvalidInput,
                                "Migrated source fields differ from the read-only snapshot."};
                    }
                }
                sqlite3_finalize(candidate);
            }
            sqlite3_finalize(rows);
            if (code != SQLITE_DONE) return sqlError(source, "Cannot read validation source");
            sqlite3_stmt* total = nullptr;
            sqlite3_prepare_v2(raw, ("SELECT count(*) FROM " + quote(table)).c_str(), -1, &total,
                               nullptr);
            bool same = sqlite3_step(total) == SQLITE_ROW &&
                        static_cast<std::size_t>(sqlite3_column_int64(total, 0)) == count;
            sqlite3_finalize(total);
            if (!same) return {MaiErrorCode::InvalidInput, "Migrated source row counts differ."};
        }
        return {};
    };
    return MaiSessionMigration::run(options);
}
