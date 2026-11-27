// 文件作用：用自建旧库验证只读迁移、字段完整性及跨会话读取边界。
#include "MaiLegacySessionReader.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/sha.h>
#include <sqlite3.h>
#ifdef _WIN32
#include <share.h>
#endif

namespace {
// 自建旧 writer 生成的 v15 黄金样本，已与原库按两会话全部字段/存储类别逐项比较。
// 原始收据和 SHA256 位于 .cache/joint-storage-20261125/legacy-own/old-writer-receipt.json。
// 测试只用 SQLite SQL 建库；产品无需保留旧版可写存储实现。
const char frozenV15Fixture[] = R"LEGACY(
CREATE TABLE context_checkpoints(session_id TEXT PRIMARY KEY,revision INTEGER NOT NULL,source_count INTEGER NOT NULL,first_message_id TEXT NOT NULL,through_message_id TEXT NOT NULL,source_fingerprint TEXT NOT NULL,model TEXT NOT NULL,summary TEXT NOT NULL,reference_text TEXT NOT NULL,created INTEGER NOT NULL, partial_message_id TEXT NOT NULL DEFAULT '', partial_part_count INTEGER NOT NULL DEFAULT 0, through_part_id TEXT NOT NULL DEFAULT '', partial_source_fingerprint TEXT NOT NULL DEFAULT '', preserved_user_message_id TEXT NOT NULL DEFAULT '');

CREATE TABLE context_import_chunks(import_id TEXT NOT NULL,sequence INTEGER NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(import_id,sequence));

INSERT INTO "context_import_chunks"("import_id","sequence","digest") VALUES(CAST(X'696d7030314d38394538594333303030484b52304851464d' AS TEXT),0,CAST(X'35343539636336356661643338666539356435393438353464346366353261333933646236653934613930383536343064363563646436383936376632393235' AS TEXT));

INSERT INTO "context_import_chunks"("import_id","sequence","digest") VALUES(CAST(X'696d7030314d38394538594335303030344e4e574a524636' AS TEXT),0,CAST(X'32366262663831396331313230313739376137303266363061346134636465613233616434636132353361613366616630353364303233616161343866663565' AS TEXT));

CREATE TABLE context_import_sources(session_id TEXT NOT NULL,source_key TEXT NOT NULL,import_id TEXT NOT NULL UNIQUE,PRIMARY KEY(session_id,source_key));

INSERT INTO "context_import_sources"("session_id","source_key","import_id") VALUES(CAST(X'746172676574' AS TEXT),CAST(X'736f75726365' AS TEXT),CAST(X'696d7030314d38394538594333303030484b52304851464d' AS TEXT));

CREATE TABLE context_imports(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,source_key TEXT NOT NULL,snapshot_id TEXT NOT NULL,label TEXT NOT NULL,next_sequence INTEGER NOT NULL DEFAULT 0,record_count INTEGER NOT NULL DEFAULT 0,last_created INTEGER NOT NULL DEFAULT 0,state INTEGER NOT NULL DEFAULT 0);

INSERT INTO "context_imports"("id","session_id","source_key","snapshot_id","label","next_sequence","record_count","last_created","state") VALUES(CAST(X'696d7030314d38394538594333303030484b52304851464d' AS TEXT),CAST(X'746172676574' AS TEXT),CAST(X'736f75726365' AS TEXT),CAST(X'736e617073686f74' AS TEXT),CAST(X'6c6162656c' AS TEXT),1,1,70,1);

INSERT INTO "context_imports"("id","session_id","source_key","snapshot_id","label","next_sequence","record_count","last_created","state") VALUES(CAST(X'696d7030314d38394538594335303030344e4e574a524636' AS TEXT),CAST(X'746172676574' AS TEXT),CAST(X'737461676564' AS TEXT),CAST(X'7374616765642d736e617073686f74' AS TEXT),CAST(X'6c6162656c' AS TEXT),1,1,80,0);

CREATE TABLE messages(  id TEXT PRIMARY KEY,  session_id TEXT NOT NULL,  role INTEGER NOT NULL DEFAULT 0,  created INTEGER NOT NULL DEFAULT 0,  completed INTEGER NOT NULL DEFAULT 0, input_request_id TEXT NOT NULL DEFAULT '', pending_input INTEGER NOT NULL DEFAULT 0, context_import_id TEXT, context_source_id TEXT, turn_state TEXT);

INSERT INTO "messages"("id","session_id","role","created","completed","input_request_id","pending_input","context_import_id","context_source_id","turn_state") VALUES(CAST(X'6d657373616765' AS TEXT),CAST(X'746172676574' AS TEXT),1,30,40,CAST(X'' AS TEXT),0,NULL,NULL,CAST(X'7b226f7574636f6d65223a7b22636f6d706c65746564223a34312c22636f6e74657874223a7b22616476616e636564223a66616c73652c226166746572223a302c2261667465725f6578616374223a66616c73652c226265666f7265223a302c226265666f72655f6578616374223a66616c73652c226572726f72223a22222c226572726f725f636f6465223a226f6b222c22686973746f72795f696e646578223a66616c73652c22696e7075745f627564676574223a302c226f726967696e616c5f7573657273223a302c226f75747075745f746f6b656e73223a3831322c227068617365223a22707265706172696e67222c2272657461696e65645f7573657273223a302c22726575736564223a66616c73652c227265766973696f6e223a302c2277696e646f77223a307d2c226572726f72223a22222c226572726f725f636f6465223a226f6b222c22726573756c745f6d6573736167655f6964223a226d657373616765222c22737461747573223a22636f6d706c65746564222c227475726e5f6964223a226d657373616765227d2c227475726e5f6964223a226d657373616765227d' AS TEXT));

INSERT INTO "messages"("id","session_id","role","created","completed","input_request_id","pending_input","context_import_id","context_source_id","turn_state") VALUES(CAST(X'666f726569676e' AS TEXT),CAST(X'6f74686572' AS TEXT),1,30,40,CAST(X'' AS TEXT),0,NULL,NULL,CAST(X'' AS TEXT));

INSERT INTO "messages"("id","session_id","role","created","completed","input_request_id","pending_input","context_import_id","context_source_id","turn_state") VALUES(CAST(X'6d73675f30314d383945385943343030304645325436385137' AS TEXT),CAST(X'746172676574' AS TEXT),0,70,70,CAST(X'' AS TEXT),0,CAST(X'696d7030314d38394538594333303030484b52304851464d' AS TEXT),CAST(X'736f757263652d6964' AS TEXT),NULL);

INSERT INTO "messages"("id","session_id","role","created","completed","input_request_id","pending_input","context_import_id","context_source_id","turn_state") VALUES(CAST(X'6d73675f30314d383945385943353030314e3158313057384e' AS TEXT),CAST(X'746172676574' AS TEXT),0,80,80,CAST(X'' AS TEXT),0,CAST(X'696d7030314d38394538594335303030344e4e574a524636' AS TEXT),CAST(X'7374616765642d6964' AS TEXT),NULL);

CREATE TABLE parts(  id TEXT PRIMARY KEY,  message_id TEXT NOT NULL,  kind INTEGER NOT NULL DEFAULT 0,  created INTEGER NOT NULL DEFAULT 0,  text TEXT NOT NULL DEFAULT '',  tool TEXT NOT NULL DEFAULT '',  call_id TEXT NOT NULL DEFAULT '',  input TEXT NOT NULL DEFAULT '',  output TEXT NOT NULL DEFAULT '',  error TEXT NOT NULL DEFAULT '',  state INTEGER NOT NULL DEFAULT 0, execution TEXT NOT NULL DEFAULT '', continuation_format TEXT, continuation_provider TEXT, continuation_model TEXT, continuation_payload TEXT);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7031' AS TEXT),CAST(X'6d657373616765' AS TEXT),0,31,CAST(X'74657874' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),1,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7032' AS TEXT),CAST(X'6d657373616765' AS TEXT),1,32,CAST(X'726561736f6e' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),1,CAST(X'' AS TEXT),CAST(X'666f726d6174' AS TEXT),CAST(X'70726f7669646572' AS TEXT),CAST(X'6d6f64656c' AS TEXT),CAST(X'6f726967696e616c00ff' AS TEXT));

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7033' AS TEXT),CAST(X'6d657373616765' AS TEXT),3,33,CAST(X'696d616765' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'696d6167652f706e67' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7034' AS TEXT),CAST(X'6d657373616765' AS TEXT),2,34,CAST(X'' AS TEXT),CAST(X'74657374' AS TEXT),CAST(X'63616c6c' AS TEXT),CAST(X'7b7d' AS TEXT),CAST(X'6f7574707574' AS TEXT),CAST(X'' AS TEXT),2,CAST(X'7b2263616e63656c6564223a66616c73652c226566666563745f737461747573223a22636f6e6669726d6564222c22657869745f636f6465223a302c226d6f64656c5f6f75747075745f76657273696f6e223a312c226e6578745f616374696f6e223a226e6f6e65222c226f75747075745f7472756e6361746564223a66616c73652c2273746172746564223a747275652c22737461747573223a22636f6d706c65746564222c2274696d65645f6f7574223a66616c73657d' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7035' AS TEXT),CAST(X'6d657373616765' AS TEXT),4,35,CAST(X'766964656f' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'766964656f2f6d7034' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7036' AS TEXT),CAST(X'6d657373616765' AS TEXT),5,36,CAST(X'71756f746564' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'70726576696577' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'666f726569676e2d70617274' AS TEXT),CAST(X'666f726569676e' AS TEXT),0,0,CAST(X'444f5f4e4f545f524541445f5345434f4e445f53455353494f4e' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7072745f30314d38394538594334303031433654364a395032' AS TEXT),CAST(X'6d73675f30314d383945385943343030304645325436385137' AS TEXT),0,70,CAST(X'696d706f72746564' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

INSERT INTO "parts"("id","message_id","kind","created","text","tool","call_id","input","output","error","state","execution","continuation_format","continuation_provider","continuation_model","continuation_payload") VALUES(CAST(X'7072745f30314d383945385943353030324a355a3444354b46' AS TEXT),CAST(X'6d73675f30314d383945385943353030314e3158313057384e' AS TEXT),0,80,CAST(X'68696464656e' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),NULL,NULL,NULL,NULL);

CREATE TABLE sessions(  id TEXT PRIMARY KEY,  title TEXT NOT NULL DEFAULT '',  directory TEXT NOT NULL DEFAULT '',  model TEXT NOT NULL DEFAULT '',  agent TEXT NOT NULL DEFAULT 'build',  created INTEGER NOT NULL DEFAULT 0,  updated INTEGER NOT NULL DEFAULT 0, parent_id TEXT NOT NULL DEFAULT '', depth INTEGER NOT NULL DEFAULT 0, title_edited INTEGER NOT NULL DEFAULT 0, reasoning_effort TEXT NOT NULL DEFAULT '');

INSERT INTO "sessions"("id","title","directory","model","agent","created","updated","parent_id","depth","title_edited","reasoning_effort") VALUES(CAST(X'746172676574' AS TEXT),CAST(X'6669727374' AS TEXT),CAST(X'6469726563746f7279' AS TEXT),CAST(X'6d6f64656c' AS TEXT),CAST(X'6275696c64' AS TEXT),11,22,CAST(X'' AS TEXT),0,1,CAST(X'68696768' AS TEXT));

INSERT INTO "sessions"("id","title","directory","model","agent","created","updated","parent_id","depth","title_edited","reasoning_effort") VALUES(CAST(X'6f74686572' AS TEXT),CAST(X'6f74686572' AS TEXT),CAST(X'6469726563746f7279' AS TEXT),CAST(X'6d6f64656c' AS TEXT),CAST(X'6275696c64' AS TEXT),11,22,CAST(X'' AS TEXT),0,1,CAST(X'68696768' AS TEXT));

CREATE TABLE specialist_tasks(id TEXT PRIMARY KEY, owner_session_id TEXT NOT NULL, specialist_name TEXT NOT NULL, parent_task_id TEXT NOT NULL DEFAULT '', provider_task_id TEXT NOT NULL DEFAULT '', intent TEXT NOT NULL, context_summary TEXT NOT NULL DEFAULT '', input_reference TEXT NOT NULL DEFAULT '', output_path TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, status INTEGER NOT NULL DEFAULT 0, partial_text TEXT NOT NULL DEFAULT '', final_text TEXT NOT NULL DEFAULT '', error_text TEXT NOT NULL DEFAULT '', last_checked_at INTEGER NOT NULL DEFAULT 0, notification_message_id TEXT NOT NULL DEFAULT '', notification_attempt_at INTEGER NOT NULL DEFAULT 0, notified_at INTEGER NOT NULL DEFAULT 0);

INSERT INTO "specialist_tasks"("id","owner_session_id","specialist_name","parent_task_id","provider_task_id","intent","context_summary","input_reference","output_path","created","status","partial_text","final_text","error_text","last_checked_at","notification_message_id","notification_attempt_at","notified_at") VALUES(CAST(X'7461736b' AS TEXT),CAST(X'746172676574' AS TEXT),CAST(X'7370656369616c' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'696e74656e74' AS TEXT),CAST(X'73756d6d617279' AS TEXT),CAST(X'7265666572656e6365' AS TEXT),CAST(X'6f7574707574' AS TEXT),45,3,CAST(X'' AS TEXT),CAST(X'' AS TEXT),CAST(X'' AS TEXT),0,CAST(X'' AS TEXT),0,45);

CREATE TABLE tool_model_outputs(session_id TEXT NOT NULL,message_id TEXT NOT NULL,part_id TEXT NOT NULL,model TEXT NOT NULL,source_fingerprint TEXT NOT NULL,content BLOB NOT NULL,PRIMARY KEY(session_id,message_id,part_id,model,source_fingerprint));

INSERT INTO "tool_model_outputs"("session_id","message_id","part_id","model","source_fingerprint","content") VALUES(CAST(X'746172676574' AS TEXT),CAST(X'6d657373616765' AS TEXT),CAST(X'7034' AS TEXT),CAST(X'6d6f64656c' AS TEXT),CAST(X'66696e6765727072696e74' AS TEXT),X'6f726967696e616c00ff');

CREATE TABLE tool_outputs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,message_id TEXT NOT NULL,part_id TEXT NOT NULL,call_id TEXT NOT NULL,payload BLOB,truncated INTEGER NOT NULL DEFAULT 0,created INTEGER NOT NULL);

INSERT INTO "tool_outputs"("id","session_id","message_id","part_id","call_id","payload","truncated","created") VALUES(CAST(X'617661696c61626c65' AS TEXT),CAST(X'746172676574' AS TEXT),CAST(X'6d657373616765' AS TEXT),CAST(X'7034' AS TEXT),CAST(X'63616c6c' AS TEXT),X'6f726967696e616c00ff',1,50);

INSERT INTO "tool_outputs"("id","session_id","message_id","part_id","call_id","payload","truncated","created") VALUES(CAST(X'676f6e65' AS TEXT),CAST(X'746172676574' AS TEXT),CAST(X'6d657373616765' AS TEXT),CAST(X'7034' AS TEXT),CAST(X'63616c6c' AS TEXT),X'676f6e65',0,51);

PRAGMA user_version=15;
)LEGACY";

int failures = 0;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

std::string raw() {
    return std::string("original\0", 9) + std::string(1, static_cast<char>(0xff));
}
std::string fileHash(const std::string& path) {
#ifdef _WIN32
    FILE* file = _fsopen(path.c_str(), "rb", _SH_DENYNO);
#else
    FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) return "<missing>";
    std::string bytes;
    char buffer[4096];
    for (std::size_t size; (size = std::fread(buffer, 1, sizeof(buffer), file)) != 0;)
        bytes.append(buffer, size);
    std::fclose(file);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    CHECK(SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest) !=
          nullptr);
    std::string result;
    static const char hex[] = "0123456789abcdef";
    for (auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
bool sql(sqlite3* db, const char* text) {
    char* error = nullptr;
    const int code = sqlite3_exec(db, text, nullptr, nullptr, &error);
    if (code != SQLITE_OK) std::printf("SQL error: %s\n", error ? error : "");
    sqlite3_free(error);
    return code == SQLITE_OK;
}
std::string path() {
    return "legacy-reader-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite";
}
struct Fixture {
    std::string filename = path();
    ~Fixture() {
        std::remove(filename.c_str());
        std::remove((filename + "-wal").c_str());
        std::remove((filename + "-shm").c_str());
    }
};
const MaiLegacySqlField* field(const MaiLegacySqlRow& row, const char* name) {
    for (const auto& value : row.fields)
        if (value.sourceField == name) return &value;
    return nullptr;
}
int trace(unsigned, void* context, void* statement, void*) {
    auto* traces = static_cast<std::vector<std::string>*>(context);
    char* expanded = sqlite3_expanded_sql(static_cast<sqlite3_stmt*>(statement));
    if (expanded) traces->emplace_back(expanded);
    sqlite3_free(expanded);
    return 0;
}

struct ReadPolicy {
    bool metadataOnly = true;
    int denied = 0;
};
int authorize(void* context, int action, const char* first, const char* second, const char*,
              const char*) {
    auto& policy = *static_cast<ReadPolicy*>(context);
    if (action == SQLITE_READ && policy.metadataOnly && first &&
        std::strcmp(first, "sessions") != 0 && std::strcmp(first, "sqlite_master") != 0) {
        ++policy.denied;
        return SQLITE_DENY;
    }
    if (action == SQLITE_READ || action == SQLITE_SELECT || action == SQLITE_FUNCTION ||
        action == SQLITE_TRANSACTION || (action == SQLITE_PRAGMA && second == nullptr))
        return SQLITE_OK;
    ++policy.denied;
    return SQLITE_DENY;
}

void testCurrentOwnedFixture() {
    Fixture fixture;
    sqlite3* writer = nullptr;
    CHECK(sqlite3_open(fixture.filename.c_str(), &writer) == SQLITE_OK);
    CHECK(sql(writer, frozenV15Fixture));
    CHECK(sqlite3_close(writer) == SQLITE_OK);
    writer = nullptr;
    CHECK(sqlite3_open(fixture.filename.c_str(), &writer) == SQLITE_OK);
    CHECK(sql(
        writer,
        "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;"
        "UPDATE tool_outputs SET payload=NULL WHERE id='gone';"
        "ALTER TABLE sessions ADD COLUMN future_text TEXT;"
        "ALTER TABLE sessions ADD COLUMN future_blob BLOB;"
        "ALTER TABLE sessions ADD COLUMN future_real REAL;"
        "ALTER TABLE sessions ADD COLUMN future_null BLOB;"
        "UPDATE parts SET continuation_provider=NULL WHERE id='p2';"
        "UPDATE sessions SET future_text='',future_blob=X'',future_real=1.25 WHERE id='target';"));
    sqlite3_stmt* update = nullptr;
    CHECK(sqlite3_prepare_v2(writer, "UPDATE parts SET text=?1 WHERE id='p1'", -1, &update,
                             nullptr) == SQLITE_OK);
    auto bytes = raw();
    CHECK(sqlite3_bind_text(update, 1, bytes.data(), static_cast<int>(bytes.size()),
                            SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(update) == SQLITE_DONE);
    sqlite3_finalize(update);
    CHECK(sql(
        writer,
        "INSERT INTO context_checkpoints(session_id,revision,source_count,first_message_id,"
        "through_message_id,source_fingerprint,model,summary,reference_text,created,"
        "partial_message_id,partial_part_count,through_part_id,partial_source_fingerprint,"
        "preserved_user_message_id) VALUES('target',1,1,'message','message','fingerprint','model',"
        "'summary','reference',99,'',0,'','','');"));
    int persist = 1;
    CHECK(sqlite3_file_control(writer, "main", SQLITE_FCNTL_PERSIST_WAL, &persist) == SQLITE_OK);
    CHECK(sqlite3_close(writer) == SQLITE_OK);
    const auto before = fileHash(fixture.filename), walBefore = fileHash(fixture.filename + "-wal");
    sqlite3* reader = nullptr;
    CHECK(sqlite3_open_v2(fixture.filename.c_str(), &reader, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    std::vector<std::string> traces;
    sqlite3_trace_v2(reader, SQLITE_TRACE_STMT, trace, &traces);
    ReadPolicy policy;
    CHECK(sqlite3_set_authorizer(reader, authorize, &policy) == SQLITE_OK);
    auto sessions = MaiLegacySessionReader::readSessions(reader);
    CHECK(sessions.isOk());
    if (sessions) CHECK(sessions.value().size() == 2);
    policy.metadataOnly = false;
    auto result = MaiLegacySessionReader::readSession(reader, "target");
    CHECK(result.isOk());
    if (result) {
        const auto& snapshot = result.value();
        CHECK(snapshot.sourceVersion == 15);
        CHECK(snapshot.session.id == "target");
        CHECK(snapshot.session.reasoningEffort == "high");
        CHECK(snapshot.session.titleEdited);
        CHECK(snapshot.messages.size() == 2);
        CHECK(snapshot.unpublishedMessages.size() == 1);
        const MaiMessage* ordinary = nullptr;
        const MaiMessage* imported = nullptr;
        for (const auto& item : snapshot.messages) {
            CHECK(item.id != "foreign");
            if (item.id == "message") ordinary = &item;
            if (item.contextImportOrigin) imported = &item;
        }
        CHECK(ordinary != nullptr);
        if (ordinary) {
            CHECK(ordinary->parts.size() == 6);
            CHECK(ordinary->turnOutcome.has_value());
            if (ordinary->turnOutcome) {
                CHECK(ordinary->turnOutcome->context.has_value());
                if (ordinary->turnOutcome->context)
                    CHECK(ordinary->turnOutcome->context->outputTokens == 812);
            }
            CHECK(std::get<MaiTextPart>(ordinary->parts[0].body).text == raw());
            CHECK(ordinary->parts[0].modelDraft);
            CHECK(std::get<MaiReasoningPart>(ordinary->parts[1].body).continuation->payload ==
                  raw());
            CHECK(
                std::get<MaiReasoningPart>(ordinary->parts[1].body).continuation->provider.empty());
            CHECK(std::get<MaiToolPart>(ordinary->parts[3].body).modelOutputVersion == 1);
            CHECK(std::get<MaiToolPart>(ordinary->parts[3].body).execution->exitCode == 0);
        }
        CHECK(imported != nullptr);
        if (imported) CHECK(imported->contextImportOrigin->sourceKey == "source");
        CHECK(snapshot.checkpoint.has_value());
        if (snapshot.checkpoint) CHECK(snapshot.checkpoint->created == 99);
        CHECK(snapshot.toolOutputs.size() == 2);
        for (const auto& output : snapshot.toolOutputs) {
            if (output.record.id == "available") {
                CHECK(output.payloadAvailable);
                CHECK(output.record.text == raw());
            }
            if (output.record.id == "gone") {
                CHECK(!output.payloadAvailable);
                CHECK(output.payloadType == MaiLegacySqlType::Null);
            }
        }
        CHECK(snapshot.toolModelOutputs.size() == 1);
        if (!snapshot.toolModelOutputs.empty())
            CHECK(snapshot.toolModelOutputs[0].content == raw());
        CHECK(snapshot.specialistTasks.size() == 1);
        bool imports = false, chunks = false, sources = false, extras = false;
        for (const auto& row : snapshot.sourceRows) {
            imports |= row.sourceTable == "context_imports";
            chunks |= row.sourceTable == "context_import_chunks";
            sources |= row.sourceTable == "context_import_sources";
            if (row.sourceTable == "sessions") {
                const auto* text = field(row, "future_text");
                const auto* blob = field(row, "future_blob");
                const auto* real = field(row, "future_real");
                const auto* nullValue = field(row, "future_null");
                CHECK(text && blob && real && nullValue);
                if (text && blob && real && nullValue) {
                    CHECK(text->type == MaiLegacySqlType::Text && text->bytes.empty());
                    CHECK(blob->type == MaiLegacySqlType::Blob && blob->bytes.empty());
                    CHECK(real->type == MaiLegacySqlType::Real && real->realValue == 1.25);
                    CHECK(nullValue->type == MaiLegacySqlType::Null);
                    CHECK(blob->declaredType == "BLOB");
                    extras = true;
                }
            }
        }
        CHECK(imports && chunks && sources && extras);
    }
    CHECK(MaiLegacySessionReader::readSession(reader, "missing").error().code() ==
          MaiErrorCode::NotFound);
    CHECK(sqlite3_get_autocommit(reader) == 1);
    CHECK(sql(reader, "BEGIN"));
    CHECK(MaiLegacySessionReader::readSession(reader, "target").isOk());
    CHECK(sqlite3_get_autocommit(reader) == 0);
    CHECK(sql(reader, "ROLLBACK"));
    CHECK(policy.denied == 0);
    for (const auto& query : traces) {
        if (query.find("SELECT t.*") != std::string::npos &&
            query.find("FROM main.sessions t ORDER BY") == std::string::npos) {
            CHECK(query.find("WHERE") != std::string::npos);
            CHECK(query.find("'target'") != std::string::npos ||
                  query.find("'missing'") != std::string::npos);
        }
    }
    CHECK(sqlite3_close(reader) == SQLITE_OK);
    CHECK(fileHash(fixture.filename) == before);
    CHECK(fileHash(fixture.filename + "-wal") == walBefore);
    // 目标行冒领其他会话的工具身份时拒绝整份快照，不读取另一会话正文修补它。
    CHECK(sqlite3_open(fixture.filename.c_str(), &writer) == SQLITE_OK);
    CHECK(sql(writer, "UPDATE tool_outputs SET part_id='foreign-part' WHERE id='available'"));
    CHECK(sqlite3_close(writer) == SQLITE_OK);
    CHECK(sqlite3_open_v2(fixture.filename.c_str(), &reader, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    CHECK(MaiLegacySessionReader::readSession(reader, "target").error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(sqlite3_close(reader) == SQLITE_OK);
}

void testLegacyV1AndReadonlyGate() {
    Fixture fixture;
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(fixture.filename.c_str(), &database) == SQLITE_OK);
    CHECK(sql(
        database,
        "CREATE TABLE sessions(id TEXT PRIMARY KEY,title TEXT,directory TEXT,model TEXT,"
        "agent TEXT,created INTEGER,updated INTEGER);"
        "CREATE TABLE messages(id TEXT PRIMARY KEY,session_id TEXT,role INTEGER,created "
        "INTEGER,completed INTEGER);"
        "CREATE TABLE parts(id TEXT PRIMARY KEY,message_id TEXT,kind INTEGER,created INTEGER,text "
        "TEXT,"
        "tool TEXT,call_id TEXT,input TEXT,output TEXT,error TEXT,state INTEGER);"
        "INSERT INTO sessions VALUES('old','old-title','','','build',1,2);"
        "INSERT INTO messages VALUES('m','old',0,3,4);"
        "INSERT INTO parts VALUES('p','m',0,3,'body','','','','','',0);PRAGMA user_version=1;"));
    CHECK(!MaiLegacySessionReader::readSessions(database));
    sqlite3_close(database);
    CHECK(sqlite3_open_v2(fixture.filename.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    auto result = MaiLegacySessionReader::readSession(database, "old");
    CHECK(result.isOk());
    if (result) {
        CHECK(result.value().session.parentId.empty());
        CHECK(!result.value().session.titleEdited);
        CHECK(result.value().messages.size() == 1);
        CHECK(!result.value().checkpoint);
    }
    sqlite3_close(database);
    CHECK(sqlite3_open(fixture.filename.c_str(), &database) == SQLITE_OK);
    CHECK(sql(database, "PRAGMA user_version=999"));
    sqlite3_close(database);
    CHECK(sqlite3_open_v2(fixture.filename.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    CHECK(MaiLegacySessionReader::readSessions(database).error().code() ==
          MaiErrorCode::NotSupported);
    sqlite3_close(database);
}
}  // namespace
int main() {
    testCurrentOwnedFixture();
    testLegacyV1AndReadonlyGate();
    std::printf("MaiLegacySessionReaderTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
