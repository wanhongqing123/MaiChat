#include <cstdio>
#include <string>
#include <sqlite3.h>
#include <json.hpp>
#include "MaiBlockingCheck.h"
#include "MaiRolloutLog.h"
#include "MaiSessionJournal.h"
#include "MaiSessionMigration.h"
#include "MaiAppendFile.h"
#include "support/MaiLegacySessionFixture.h"
#include "MaiSqliteStore.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
namespace {
int additionalFailures = 0;
#define CHECK(value)                                                     \
    do {                                                                 \
        if (!(value)) {                                                  \
            std::printf("FAIL journal line %d: %s\n", __LINE__, #value); \
            ++additionalFailures;                                        \
        }                                                                \
    } while (false)
struct OwnedFixture {
    MaiFilePath root;
    MaiFilePath db;
    OwnedFixture()
        : root(MaiFileSystem::temporaryDirectory().append(
              MaiFilePath::fromUtf8("journal-extra-" + MaiIdGenerator::newSessionId()))),
          db(root.append(MaiFilePath::fromUtf8("agent.db"))) {
        CHECK(!MaiFileSystem::createDirectories(root));
    }
    ~OwnedFixture() {
        if (!additionalFailures)
            MaiFileSystem::removeRecursively(root);
        else
            std::printf("Owned fixture: %s\n", root.toUtf8().c_str());
    }
};
MaiMessage makeOwnedMessage(const std::string& body) {
    MaiMessage m;
    m.id = "msg_owned";
    m.created = 1795185882657;
    m.completed = m.created;
    MaiMessagePart p;
    p.id = "prt_owned";
    p.created = m.created;
    p.body = MaiTextPart{body};
    m.parts.push_back(p);
    return m;
}
void createOwnedSession(MaiSessionStore& store) {
    MaiSession session;
    session.id = "ses_owned";
    session.created = 1795185882657;
    session.updated = session.created;
    session.directory = "/owned-context";
    session.model = "owned-model";
    store.putSession(session);
    CHECK(!store.lastWriteError());
}
void test_exact_bytes_context_native_and_drafts() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    auto& store = *opened.value();
    createOwnedSession(store);
    const std::string body =
        std::string("valid\0invalid", 13) + std::string(1, static_cast<char>(0xff));
    auto message = makeOwnedMessage(body);
    store.putMessage("ses_owned", message);
    CHECK(!store.lastWriteError());
    MaiMessage restored;
    CHECK(store.getMessage("ses_owned", message.id, restored));
    CHECK(restored.parts.size() == 1 && std::get<MaiTextPart>(restored.parts[0].body).text == body);
    auto context =
        store.saveSessionContext("ses_owned", "skills.catalog.prompt.v1", "catalog-7a51");
    CHECK(context && context.value() == "catalog-7a51");
    auto loser = store.saveSessionContext("ses_owned", "skills.catalog.prompt.v1", "loser");
    CHECK(loser && loser.value() == "catalog-7a51");
    CHECK(!store.saveSessionContext("missing", "skills.catalog.prompt.v1", "bad"));
    auto none = store.readLatestSessionRecord("ses_owned", "token_usage_record");
    CHECK(none && none.value().empty());
    CHECK(!store.appendSessionRecord(
        "ses_owned", "token_usage_record",
        R"({"response_id":"owned","total_tokens":27,"unknown":{"nested":true}})"));
    auto latest = store.readLatestSessionRecord("ses_owned", "token_usage_record");
    CHECK(latest && nlohmann::json::parse(latest.value())["total_tokens"] == 27);
    message.id = "msg_draft";
    message.parts[0].id = "prt_draft";
    message.parts[0].modelDraft = true;
    std::get<MaiTextPart>(message.parts[0].body).text = "draft-only-9da71";
    store.putMessage("ses_owned", message);
    CHECK(!store.lastWriteError());
    MaiMessage ordinaryDraft;
    CHECK(store.getMessage("ses_owned", "msg_draft", ordinaryDraft));
    CHECK(ordinaryDraft.parts.size() == 1 && ordinaryDraft.parts[0].modelDraft &&
          std::holds_alternative<MaiTextPart>(ordinaryDraft.parts[0].body));
    const auto liveHistory = store.listMessages("ses_owned");
    bool liveFound = false;
    for (const auto& value : liveHistory)
        if (value.id == "msg_draft")
            liveFound = value.parts.size() == 1 && value.parts[0].modelDraft;
    CHECK(liveFound);
    const auto livePage = store.listMessagesPage("ses_owned", "", 10);
    bool pageFound = false;
    for (const auto& value : livePage)
        if (value.id == "msg_draft")
            pageFound = value.parts.size() == 1 && value.parts[0].modelDraft;
    CHECK(pageFound);
    {
        MaiScopedDisallowBlocking callback;
        MaiMessage draft;
        CHECK(store.getMessage("ses_owned", "msg_draft", draft));
        CHECK(draft.parts.size() == 1 && draft.parts[0].modelDraft);
    }
    opened.value().reset();
    auto reopened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(reopened);
    if (!reopened) return;
    CHECK(reopened.value()->getMessage("ses_owned", "msg_draft", restored));
    CHECK(restored.parts.empty());
    CHECK(reopened.value()->getSessionContext("ses_owned", "skills.catalog.prompt.v1"));
    std::string logs;
    MaiFileSystem::walk(f.root.append(MaiFilePath::fromUtf8("sessions")),
                        [&](const MaiFileEntry& entry) {
                            if (!entry.isDirectory) {
                                std::string bytes;
                                CHECK(!MaiFileSystem::readFile(entry.path, bytes));
                                logs += bytes;
                            }
                            return MaiWalkAction::Continue;
                        });
    CHECK(logs.find("draft-only-9da71") == std::string::npos);
}
int nativeCount(const OwnedFixture& fixture, const char* sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(fixture.db.toUtf8().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &query, nullptr) == SQLITE_OK);
    int result = -1;
    if (query && sqlite3_step(query) == SQLITE_ROW)
        result = sqlite3_column_int(query, 0);
    else
        CHECK(false);
    sqlite3_finalize(query);
    sqlite3_close(db);
    return result;
}
void test_native_import_visibility_follows_committed_ledger() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    auto& store = *opened.value();
    createOwnedSession(store);
    const auto append = [&](const char* snapshot, std::size_t count, bool publish) {
        auto progress = store.beginContextImport({"ses_owned", "source", snapshot, "owned"});
        CHECK(progress);
        if (!progress) return std::string{};
        std::vector<MaiContextImportRecord> rows;
        for (std::size_t i = 0; i < count; ++i) {
            MaiContextImportRecord row;
            row.sourceId = std::to_string(i);
            row.originalCreatedAtMillis = 1;
            row.text = "owned import " + std::to_string(i);
            rows.push_back(row);
        }
        CHECK(store.appendContextImport(progress.value().importId, 0, rows));
        if (publish) CHECK(store.commitContextImport(progress.value().importId));
        return progress.value().importId;
    };
    auto old = append("old", 1, false);
    CHECK(nativeCount(f, "SELECT count(*) FROM thread_items WHERE thread_id='ses_owned'") == 0);
    CHECK(store.commitContextImport(old));
    CHECK(nativeCount(f, "SELECT count(*) FROM thread_items WHERE thread_id='ses_owned'") == 1);
    auto fresh = append("fresh", 2, false);
    CHECK(nativeCount(f, "SELECT count(*) FROM thread_items WHERE thread_id='ses_owned'") == 1);
    CHECK(store.commitContextImport(fresh));
    CHECK(nativeCount(f, "SELECT count(*) FROM thread_items WHERE thread_id='ses_owned'") == 2);
    auto aborted = append("aborted", 1, false);
    CHECK(!store.abortContextImport(aborted));
    CHECK(nativeCount(f, "SELECT count(*) FROM thread_items WHERE thread_id='ses_owned'") == 2);
    CHECK(nativeCount(f,
                      "SELECT count(*) FROM thread_items t JOIN messages m ON m.id=t.item_id LEFT "
                      "JOIN context_import_sources s ON s.import_id=m.context_import_id WHERE "
                      "m.context_import_id IS NOT NULL AND s.import_id IS NULL") == 0);
}
void test_empty_frozen_blob_survives_legacy_migration() {
    OwnedFixture f;
    sqlite3* legacy = nullptr;
    CHECK(sqlite3_open(f.db.toUtf8().c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(legacy, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    CHECK(sqlite3_exec(legacy, "UPDATE tool_model_outputs SET content=zeroblob(0)", nullptr,
                       nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    if (!opened) std::printf("empty blob migration: %s\n", opened.error().message().c_str());
    CHECK(opened);
    if (opened)
        CHECK(
            nativeCount(
                f,
                "SELECT count(*) FROM tool_model_outputs WHERE content_ref='@mai_empty_blob_v1'") >
            0);
}
void test_unknown_legacy_tables_cannot_overwrite_native_state() {
    OwnedFixture f;
    sqlite3* legacy = nullptr;
    CHECK(sqlite3_open(f.db.toUtf8().c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(legacy, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    CHECK(sqlite3_exec(legacy,
                       "CREATE TABLE threads(id TEXT PRIMARY KEY,note TEXT);INSERT INTO threads "
                       "VALUES('opaque-thread','legacy-only-body-7f15');",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    CHECK(nativeCount(f, "SELECT count(*) FROM threads WHERE id='opaque-thread'") == 0);
    opened.value().reset();
    std::string database;
    CHECK(!MaiFileSystem::readFile(f.db, database));
    CHECK(database.find("legacy-only-body-7f15") == std::string::npos);
}
void test_truthful_session_metadata_and_instruction_correlation() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    createOwnedSession(*opened.value());
    const std::string instructions = "full-base-instructions-7ad1";
    const nlohmann::json item{
        {"type", "message"},
        {"role", "developer"},
        {"content",
         nlohmann::json::array({nlohmann::json{{"type", "input_text"}, {"text", instructions}}})}};
    CHECK(!opened.value()->appendSessionRecord("ses_owned", "response_item", item.dump()));
    CHECK(!opened.value()->appendSessionRecord("ses_owned", "response_item", item.dump()));
    auto update = opened.value()->readLatestSessionRecord("ses_owned", "event_msg");
    CHECK(update);
    if (update) {
        auto value = nlohmann::json::parse(update.value());
        CHECK(value["type"] == "session_metadata_updated");
        CHECK(value["base_instructions"]["text"] == instructions);
        CHECK(value["source_response_item"]["ordinal"].is_number_unsigned());
    }
    MaiFilePath source;
    MaiFileSystem::walk(f.root.append(MaiFilePath::fromUtf8("sessions")),
                        [&](const MaiFileEntry& e) {
                            if (!e.isDirectory && e.nameUtf8.compare(0, 8, "rollout-") == 0)
                                source = e.path;
                            return MaiWalkAction::Continue;
                        });
    auto log = MaiRolloutLog::open(source);
    CHECK(log);
    if (log) {
        std::size_t headers = 0;
        auto replay = log.value()->replay({}, [&](const MaiRolloutRecord& record) -> MaiError {
            auto value = nlohmann::json::parse(record.payload);
            if (record.type == "session_meta") {
                CHECK(value["cwd"] == "/owned-context");
                CHECK(value["model"] == "owned-model");
                CHECK(value["base_instructions"].is_null());
                CHECK(value["model_provider"].is_null());
                CHECK(value["creator_user_id"].is_null());
                CHECK(value["creator_account_id"].is_null());
            }
            if (record.type == "response_item") ++headers;
            return {};
        });
        CHECK(replay);
        CHECK(headers == 1);
    }
}
void test_draft_overlay_never_masks_missing_durable_source() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    createOwnedSession(*opened.value());
    auto message = makeOwnedMessage("durable prefix");
    message.parts[0].id = "prt_durable";
    MaiMessagePart draft;
    draft.id = "prt_draft";
    draft.modelDraft = true;
    draft.body = MaiReasoningPart{"RAM-only reasoning"};
    message.parts.push_back(draft);
    opened.value()->putMessage("ses_owned", message);
    CHECK(!opened.value()->lastWriteError());
    sqlite3* index = nullptr;
    CHECK(sqlite3_open(f.db.toUtf8().c_str(), &index) == SQLITE_OK);
    CHECK(sqlite3_create_function(
              index, "mai_body", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, nullptr,
              [](sqlite3_context* context, int, sqlite3_value**) {
                  sqlite3_result_text(context, "", 0, SQLITE_STATIC);
              },
              nullptr, nullptr) == SQLITE_OK);
    CHECK(
        sqlite3_exec(
            index,
            R"(UPDATE parts SET text_ref='["sessions/missing.jsonl",0,1,0,false]' WHERE id='prt_durable')",
            nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(index);
    MaiMessage restored;
    restored.id = "unchanged";
    CHECK(!opened.value()->getMessage("ses_owned", message.id, restored));
    CHECK(restored.id == "unchanged");
    CHECK(opened.value()->lastReadError());
    CHECK(opened.value()->listMessages("ses_owned").empty());
    CHECK(opened.value()->lastReadError());
}
void test_public_origin_cannot_enter_event_cache() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    createOwnedSession(*opened.value());
    auto message = makeOwnedMessage("public origin stays ordinary");
    message.contextImportOrigin = MaiContextImportOrigin{"forged-source", "forged-record"};
    opened.value()->putMessage("ses_owned", message);
    CHECK(!opened.value()->lastWriteError());
    MaiMessage restored;
    CHECK(opened.value()->getMessage("ses_owned", message.id, restored));
    CHECK(!restored.contextImportOrigin);
    {
        MaiScopedDisallowBlocking event;
        CHECK(opened.value()->getMessage("ses_owned", message.id, restored));
        CHECK(!restored.contextImportOrigin);
    }
    auto history = opened.value()->listMessages("ses_owned");
    CHECK(history.size() == 1 && !history[0].contextImportOrigin);
    opened.value().reset();
    auto reopened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(reopened);
    if (reopened) {
        CHECK(reopened.value()->getMessage("ses_owned", message.id, restored));
        CHECK(!restored.contextImportOrigin);
    }
}
void test_missing_source_fails_closed() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    auto& store = *opened.value();
    createOwnedSession(store);
    store.putMessage("ses_owned", makeOwnedMessage("missing-source-body"));
    MaiFilePath source;
    MaiFileSystem::walk(f.root.append(MaiFilePath::fromUtf8("sessions")),
                        [&](const MaiFileEntry& e) {
                            if (!e.isDirectory && e.nameUtf8.compare(0, 8, "rollout-") == 0)
                                source = e.path;
                            return MaiWalkAction::Continue;
                        });
    CHECK(!source.isEmpty());
    // 打开中的写日志句柄在 Windows 上阻止删除；关闭后破坏自有测试源，再重开读取。
    opened.value().reset();
    CHECK(!MaiFileSystem::removeFile(source));
    auto reopened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(reopened);
    if (!reopened) return;
    MaiMessage out;
    out.id = "unchanged";
    CHECK(!reopened.value()->getMessage("ses_owned", "msg_owned", out));
    CHECK(out.id == "unchanged");
    CHECK(reopened.value()->lastReadError());
}
void test_projection_commit_pending_and_partial_tail() {
    OwnedFixture f;
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    {
        auto journal = MaiSessionJournal::open(db, f.root, "owned-memory");
        CHECK(journal);
        if (!journal) {
            sqlite3_close(db);
            return;
        }
        CHECK(!journal.value()->begin());
        CHECK(sqlite3_exec(db, "INSERT INTO sessions(id,title) VALUES('owned','metadata')", nullptr,
                           nullptr, nullptr) == SQLITE_OK);
        sqlite3_commit_hook(db, [](void*) -> int { return 1; }, nullptr);
        CHECK(!journal.value()->commit());
        CHECK(journal.value()->projectionError());
        sqlite3_commit_hook(db, nullptr, nullptr);
        CHECK(!journal.value()->readBarrier());
        sqlite3_stmt* query = nullptr;
        CHECK(sqlite3_prepare_v2(db, "SELECT title FROM sessions WHERE id='owned'", -1, &query,
                                 nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(query) == SQLITE_ROW);
        sqlite3_finalize(query);
    }
    sqlite3_close(db);
    auto source = f.root.append(MaiFilePath::fromUtf8("sessions/.journal/owned-memory.jsonl"));
    {
        auto file = MaiAppendFile::open(source);
        CHECK(file);
        if (file) {
            CHECK(!file.value()->lockWriter());
            CHECK(file.value()->append("{partial"));
            CHECK(!file.value()->flush());
            file.value()->unlockWriter();
        }
    }
    std::string before;
    CHECK(!MaiFileSystem::readFile(source, before));
    CHECK(!MaiRolloutLog::open(source));
    std::string after;
    CHECK(!MaiFileSystem::readFile(source, after));
    CHECK(before == after);
}
void test_uncertain_proof_reads_first_record_with_later_transactions() {
    OwnedFixture f;
    auto source = f.root.append(MaiFilePath::fromUtf8("sessions/proof.jsonl"));
    auto log = MaiRolloutLog::create(source, R"({"id":"proof","session_id":"owned"})", 1);
    CHECK(log);
    if (!log) return;
    auto first = log.value()->append(
        "event_msg",
        R"({"type":"mai_projection_transaction","transaction_id":"first-unique","mutations":{}})",
        2);
    CHECK(first);
    auto later = log.value()->append(
        "event_msg",
        R"({"type":"mai_projection_transaction","transaction_id":"later-unique","mutations":{}})",
        3);
    CHECK(later);
    if (!first || !later) return;
    CHECK(!log.value()->confirmDurable());
    auto proof = MaiSessionJournal::readFirstRecord(source, first.value().byteOffset);
    CHECK(proof);
    if (proof) {
        CHECK(proof.value().location.ordinal == first.value().ordinal);
        CHECK(proof.value().location.byteLength == first.value().byteLength);
        CHECK(nlohmann::json::parse(proof.value().payload)["transaction_id"] == "first-unique");
    }
}
void test_rejected_metadata_append_cannot_acknowledge_rollback() {
    OwnedFixture f;
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    {
        auto journal = MaiSessionJournal::open(db, f.root, "oversize-metadata");
        CHECK(journal);
        if (!journal) {
            sqlite3_close(db);
            return;
        }
        const auto source =
            f.root.append(MaiFilePath::fromUtf8("sessions/.journal/oversize-metadata.jsonl"));
        std::string before;
        CHECK(!MaiFileSystem::readFile(source, before));
        CHECK(!journal.value()->begin());
        sqlite3_stmt* insert = nullptr;
        CHECK(sqlite3_prepare_v2(db, "INSERT INTO sessions(id,title) VALUES('oversize',?1)", -1,
                                 &insert, nullptr) == SQLITE_OK);
        const std::string title(50 * 1024 * 1024, 'v');
        sqlite3_bind_text64(insert, 1, title.data(), title.size(), SQLITE_STATIC, SQLITE_UTF8);
        CHECK(sqlite3_step(insert) == SQLITE_DONE);
        sqlite3_finalize(insert);
        CHECK(journal.value()->commit());
        std::string after;
        CHECK(!MaiFileSystem::readFile(source, after));
        CHECK(before == after);
        sqlite3_stmt* query = nullptr;
        CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM sessions WHERE id='oversize'", -1,
                                 &query, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 0);
        sqlite3_finalize(query);
        CHECK(!journal.value()->begin());
        CHECK(sqlite3_exec(db,
                           "INSERT INTO sessions(id,title) VALUES('valid-after-rejection','small')",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        CHECK(!journal.value()->commit());
    }
    sqlite3_close(db);
}
void test_raw_output_chunks_ownership_and_import() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    auto& store = *opened.value();
    createOwnedSession(store);
    MaiMessage message = makeOwnedMessage("");
    message.role = MaiRole::Assistant;
    MaiToolPart tool;
    tool.tool = "owned";
    tool.callId = "call_owned";
    tool.state = MaiToolState::Completed;
    message.parts[0].body = tool;
    store.putMessage("ses_owned", message);
    CHECK(!store.lastWriteError());
    MaiToolOutputRecord output;
    output.id = "output_owned";
    output.sessionId = "ses_owned";
    output.messageId = message.id;
    output.partId = message.parts[0].id;
    output.callId = "call_owned";
    output.created = 1795185882657;
    output.text = std::string(16 * 1024 * 1024, 'q');
    output.text.replace(16380, 10, "0123456789");
    CHECK(!store.putToolOutput(output));
    CHECK(store.putToolOutput(output).code() == MaiErrorCode::InvalidInput);
    MaiToolOutputQuery query;
    query.outputId = output.id;
    query.offsetBytes = 16380;
    query.maxBytes = 4096;
    auto page = store.readToolOutput("ses_owned", query);
    CHECK(page);
    if (page)
        CHECK(page.value().text == output.text.substr(query.offsetBytes, page.value().text.size()));
    CHECK(!store.readToolOutput("other", query));
    MaiToolModelOutputKey key{"ses_owned", message.id, message.parts[0].id, "model", "fingerprint"};
    auto winner = store.saveToolModelOutput(key, "frozen-body-7251");
    CHECK(winner && winner.value() == "frozen-body-7251");
    auto loser = store.saveToolModelOutput(key, "ignored");
    CHECK(loser && loser.value() == winner.value());
    auto imported = store.beginContextImport({"ses_owned", "source", "snapshot", "owned"});
    CHECK(imported);
    if (imported) {
        MaiContextImportRecord row;
        row.sourceId = "external-one";
        row.originalCreatedAtMillis = 1;
        row.text = "import-one";
        auto batch = store.appendContextImport(imported.value().importId, 0, {row});
        CHECK(batch);
        CHECK(store.commitContextImport(imported.value().importId));
        auto history = store.listMessages("ses_owned");
        bool found = false;
        for (const auto& item : history)
            if (item.contextImportOrigin) {
                found = true;
                CHECK(item.contextImportOrigin->sourceKey == "source");
            }
        CHECK(found);
    }
    opened.value().reset();
    std::string database;
    CHECK(!MaiFileSystem::readFile(f.db, database));
    CHECK(database.find("frozen-body-7251") == std::string::npos);
    CHECK(database.find(std::string(2048, 'q')) == std::string::npos);
    auto reopened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(reopened);
    if (reopened) {
        auto restored = reopened.value()->readToolOutput("ses_owned", query);
        CHECK(restored);
        CHECK(reopened.value()->getToolModelOutput(key));
    }
}
void test_cancelable_migration_and_native_turns() {
    OwnedFixture f;
    sqlite3* source = nullptr;
    CHECK(sqlite3_open(f.db.toUtf8().c_str(), &source) == SQLITE_OK);
    CHECK(sqlite3_exec(source, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(source);
    std::string before;
    CHECK(!MaiFileSystem::readFile(f.db, before));
    bool cancel = false;
    MaiSqliteStoreOpenOptions options;
    options.isCanceled = [&]() { return cancel; };
    options.progress = [&](const std::string& phase) {
        if (phase == "migration_table:parts") cancel = true;
    };
    auto canceled = makeMaiSqliteStore(f.db.toUtf8(), options);
    CHECK(!canceled && canceled.error().code() == MaiErrorCode::Canceled);
    std::string after;
    CHECK(!MaiFileSystem::readFile(f.db, after));
    CHECK(before == after);
    CHECK(MaiSessionMigration::hasPending(f.root, f.db));
    auto restarted = makeMaiSqliteStore(f.db.toUtf8());
    if (!restarted) std::printf("restart: %s\n", restarted.error().message().c_str());
    CHECK(restarted);
    if (!restarted) return;
    createOwnedSession(*restarted.value());
    MaiMessage turn;
    turn.id = "msg_turn_native";
    turn.role = MaiRole::Assistant;
    turn.turnId = turn.id;
    turn.created = 1795185882657;
    turn.turnOutcome = MaiTurnOutcome{turn.id};
    MaiToolPart tool;
    tool.tool = "owned";
    tool.callId = "native_call";
    tool.state = MaiToolState::Pending;
    turn.parts.push_back({"prt_native_tool", tool, turn.created});
    restarted.value()->putMessage("ses_owned", turn);
    CHECK(!restarted.value()->lastWriteError());
    sqlite3* probe = nullptr;
    CHECK(sqlite3_open_v2(f.db.toUtf8().c_str(), &probe, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    sqlite3_stmt* query = nullptr;
    CHECK(
        sqlite3_prepare_v2(probe,
                           "SELECT status,(SELECT count(*) FROM thread_items WHERE "
                           "thread_id='ses_owned'),completed_at,duration_ms,rollout_end_ordinal,"
                           "rollout_end_byte_offset FROM thread_turns WHERE thread_id='ses_owned'",
                           -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(query, 0))) == "running");
    CHECK(sqlite3_column_int(query, 1) == 0);
    CHECK(sqlite3_column_count(query) == 6);
    for (int field = 2; field <= 5; ++field)
        CHECK(sqlite3_column_type(query, field) == SQLITE_NULL);
    sqlite3_finalize(query);
    sqlite3_close(probe);
    MaiContextStatus status;
    status.phase = MaiContextPhase::Ready;
    status.windowTokens = 8192;
    CHECK(!restarted.value()->updateTurnContext("ses_owned", turn.id, status));
    auto turnMetadata = restarted.value()->readTurnMetadata("ses_owned", turn.id);
    CHECK(turnMetadata && turnMetadata.value().turnOutcome &&
          turnMetadata.value().turnOutcome->context);
    restarted.value()->recoverInterruptedTools("owned interrupted", turn.created + 5);
    auto interrupted = restarted.value()->readTurnMetadata("ses_owned", turn.id);
    CHECK(interrupted && interrupted.value().turnOutcome &&
          interrupted.value().turnOutcome->status == MaiTurnStatus::Interrupted);
    std::get<MaiToolPart>(turn.parts[0].body).state = MaiToolState::Completed;
    turn.completed = turn.created + 10;
    turn.turnOutcome->completed = turn.completed;
    turn.turnOutcome->status = MaiTurnStatus::Completed;
    turn.turnOutcome->resultMessageId = turn.id;
    turn.parts.push_back(
        {"prt_native_answer", MaiTextPart{"owned final answer"}, turn.created + 8});
    restarted.value()->putMessage("ses_owned", turn);
    CHECK(!restarted.value()->lastWriteError());
    CHECK(nativeCount(
              f,
              "SELECT count(*) FROM thread_turns t JOIN thread_items i ON i.thread_id=t.thread_id "
              "AND i.turn_id=t.turn_id AND i.item_id=t.final_agent_item_id WHERE "
              "t.thread_id='ses_owned' AND t.final_agent_item_id='prt_native_answer' AND "
              "t.rollout_end_ordinal>=i.updated_at_ordinal") == 1);
}
void test_legacy_fixture_migrates() {
    OwnedFixture f;
    sqlite3* legacy = nullptr;
    CHECK(sqlite3_open(f.db.toUtf8().c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(legacy, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(legacy);
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    if (!opened) std::printf("Migration failure: %s\n", opened.error().message().c_str());
    CHECK(opened);
    if (!opened) return;
    CHECK(!opened.value()->listSessions().empty());
    CHECK(!opened.value()->lastReadError());
    CHECK(nativeCount(f,
                      "SELECT count(*) FROM thread_items t JOIN messages m ON m.id=t.item_id LEFT "
                      "JOIN context_import_sources s ON s.import_id=m.context_import_id WHERE "
                      "m.context_import_id IS NOT NULL AND s.import_id IS NULL") == 0);
    opened.value().reset();
    CHECK(!MaiFileSystem::removeFile(f.db));
    auto rebuilt = makeMaiSqliteStore(f.db.toUtf8());
    if (!rebuilt) std::printf("Migration rebuild failure: %s\n", rebuilt.error().message().c_str());
    CHECK(rebuilt);
    if (rebuilt) CHECK(!rebuilt.value()->listSessions().empty());
}
void test_current_startup_uses_its_own_journal() {
    OwnedFixture f;
    // 旧路径不是 SQLite 也无需读取；只测试新版从空历史开始并且保持选择稳定。
    CHECK(!MaiFileSystem::writeFile(f.db, "untouched-legacy-session-file"));
    const auto selected = maiCurrentSessionDatabasePath(f.db.toUtf8());
    CHECK(selected && selected.value() == f.db.toUtf8() + ".v16");
    if (!selected) return;
    auto opened = makeMaiSqliteStore(selected.value());
    CHECK(opened && opened.value()->listSessions().empty());
    if (!opened) return;
    createOwnedSession(*opened.value());
    opened.value()->putMessage("ses_owned", makeOwnedMessage("new-only-body"));
    CHECK(!opened.value()->lastWriteError());
    opened.value().reset();
    CHECK(!MaiFileSystem::removeFile(MaiFilePath::fromUtf8(selected.value())));
    // 索引和旧文件都丢失也不能改变新版日志身份；新记录仍从同一个提交日志恢复。
    CHECK(!MaiFileSystem::removeFile(f.db));
    const auto rebuiltPath = maiCurrentSessionDatabasePath(f.db.toUtf8());
    CHECK(rebuiltPath && rebuiltPath.value() == selected.value());
    if (!rebuiltPath) return;
    auto rebuilt = makeMaiSqliteStore(rebuiltPath.value());
    MaiMessage restored;
    CHECK(rebuilt && rebuilt.value()->getMessage("ses_owned", "msg_owned", restored));
    if (rebuilt) CHECK(rebuilt.value()->listSessions().size() == 1);
    rebuilt.value().reset();
    CHECK(
        !MaiFileSystem::writeFile(MaiFilePath::fromUtf8(selected.value()), "broken-current-index"));
    CHECK(!makeMaiCurrentSessionStore(f.db.toUtf8()));
}
void test_current_startup_preserves_existing_native_location() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    createOwnedSession(*opened.value());
    opened.value().reset();
    const auto selected = maiCurrentSessionDatabasePath(f.db.toUtf8());
    CHECK(selected && selected.value() == f.db.toUtf8());
    CHECK(!maiCurrentSessionDatabasePath(""));
    CHECK(!maiCurrentSessionDatabasePath(std::string("bad\0path", 8)));
    const auto memory = maiCurrentSessionDatabasePath(":memory:");
    CHECK(memory && memory.value() == ":memory:");
}
void test_current_startup_does_not_resume_partial_legacy_publication() {
    OwnedFixture f;
    auto opened = makeMaiSqliteStore(f.db.toUtf8());
    CHECK(opened);
    if (!opened) return;
    createOwnedSession(*opened.value());
    opened.value().reset();
    // 模拟旧发布已留下日志但没有完成收据；普通启动不恢复该导入现场。
    CHECK(!MaiFileSystem::createDirectories(
        f.root.append(MaiFilePath::fromUtf8(".agent.db.session-migration"))));
    const auto selected = maiCurrentSessionDatabasePath(f.db.toUtf8());
    CHECK(selected && selected.value() == f.db.toUtf8() + ".v16");
    if (!selected) return;
    auto fresh = makeMaiSqliteStore(selected.value());
    CHECK(fresh && fresh.value()->listSessions().empty());
    if (!fresh) return;
    fresh.value().reset();
    // 选中的新版目标格式过新时必须拒绝，不能再找另一空库掩盖错误。
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(selected.value().c_str(), &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "PRAGMA user_version=99", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    const auto unsupported = makeMaiCurrentSessionStore(f.db.toUtf8());
    CHECK(!unsupported && unsupported.error().code() == MaiErrorCode::NotSupported);
}
void test_current_factory_cannot_enable_legacy_import() {
    OwnedFixture f;
    const std::string currentPath = f.db.toUtf8() + ".v16";
    sqlite3* legacy = nullptr;
    CHECK(sqlite3_open(currentPath.c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(legacy, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(legacy);
    std::string before, after;
    CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(currentPath), before));
    MaiSqliteStoreOpenOptions options;
    options.allowLegacyMigration = true;
    auto opened = makeMaiCurrentSessionStore(f.db.toUtf8(), options);
    CHECK(!opened && opened.error().code() == MaiErrorCode::NotSupported);
    CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(currentPath), after));
    CHECK(before == after);
    CHECK(!MaiFileSystem::exists(f.root.append(MaiFilePath::fromUtf8("sessions"))));
}
}  // namespace
int main() {
    test_current_startup_uses_its_own_journal();
    test_current_startup_preserves_existing_native_location();
    test_current_startup_does_not_resume_partial_legacy_publication();
    test_current_factory_cannot_enable_legacy_import();
    test_exact_bytes_context_native_and_drafts();
    test_missing_source_fails_closed();
    test_public_origin_cannot_enter_event_cache();
    test_draft_overlay_never_masks_missing_durable_source();
    test_truthful_session_metadata_and_instruction_correlation();
    test_native_import_visibility_follows_committed_ledger();
    test_empty_frozen_blob_survives_legacy_migration();
    test_unknown_legacy_tables_cannot_overwrite_native_state();
    test_legacy_fixture_migrates();
    test_cancelable_migration_and_native_turns();
    test_projection_commit_pending_and_partial_tail();
    test_rejected_metadata_append_cannot_acknowledge_rollback();
    test_uncertain_proof_reads_first_record_with_later_transactions();
    test_raw_output_chunks_ownership_and_import();
    int failures = additionalFailures;
    auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("journal-contract-" + MaiIdGenerator::newSessionId()));
    MaiFileSystem::createDirectories(root);
    auto db = root.append(MaiFilePath::fromUtf8("agent.db"));
    std::string body = "journal-body-7a438c-must-not-be-copied";
    {
        auto r = makeMaiSqliteStore(db.toUtf8());
        if (!r) return 2;
        MaiSession s;
        s.id = "ses_contract";
        s.created = 1795185882657;
        s.updated = s.created;
        r.value()->putSession(s);
        MaiMessage m;
        m.id = "msg_contract";
        m.created = s.created;
        m.completed = s.created;
        MaiMessagePart p;
        p.id = "prt_contract";
        p.body = MaiTextPart{body};
        m.parts.push_back(p);
        r.value()->putMessage(s.id, m);
        if (r.value()->lastWriteError()) {
            std::printf("write: %s\n", r.value()->lastWriteError().message().c_str());
            ++failures;
        }
        MaiMessage out;
        if (!r.value()->getMessage(s.id, m.id, out) || out.parts.size() != 1 ||
            std::get<MaiTextPart>(out.parts[0].body).text != body) {
            std::puts("FAIL body roundtrip");
            ++failures;
        }
    }
    std::string bytes;
    MaiFileSystem::readFile(db, bytes);
    if (bytes.find(body) != std::string::npos) {
        std::puts("FAIL SQLite contains body");
        ++failures;
    }
    int logs = 0;
    MaiFileSystem::walk(root.append(MaiFilePath::fromUtf8("sessions")), [&](const MaiFileEntry& e) {
        if (!e.isDirectory) ++logs;
        return MaiWalkAction::Continue;
    });
    if (!logs) {
        std::puts("FAIL no session JSONL");
        ++failures;
    }
    MaiFileSystem::removeFile(db);
    {
        auto r = makeMaiSqliteStore(db.toUtf8());
        MaiMessage out;
        if (!r || !r.value()->getMessage("ses_contract", "msg_contract", out) ||
            out.parts.size() != 1) {
            std::puts("FAIL index rebuild");
            ++failures;
        }
    }
    if (!failures)
        MaiFileSystem::removeRecursively(root);
    else
        std::printf("Owned failed fixture: %s\n", root.toUtf8().c_str());
    std::printf("MaiJournalStoreTests failures=%d\n", failures);
    return failures ? 1 : 0;
}
