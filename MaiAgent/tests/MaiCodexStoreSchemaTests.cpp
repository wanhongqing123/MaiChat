// Schema contract fixture captured independently from the custom Codex metadata export.
// Baseline SHA256: cf1112822dcced2f5e31b421a0603396200d946a6a1c2a9cd3893b9a924bd980
// Only schema metadata is embedded; no production database or session rows are read.
#include "MaiCodexStoreSchema.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sqlite3.h>

#ifdef _WIN32
#include <share.h>
#endif

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

const char* const expectedBusinessSchema = R"FIXTURE(
CREATE TABLE backfill_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    status TEXT NOT NULL,
    last_watermark TEXT,
    last_success_at INTEGER,
    updated_at INTEGER NOT NULL
);

CREATE TABLE external_agent_config_imports (
    import_id TEXT PRIMARY KEY,
    completed_at_ms INTEGER NOT NULL,
    successes TEXT NOT NULL,
    failures TEXT NOT NULL
, provider_id TEXT);

CREATE TABLE guardian_review_feedback (
    id TEXT PRIMARY KEY NOT NULL,
    thread_id TEXT NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    record BLOB NOT NULL
);

CREATE TABLE project_idempotency_keys (
    key TEXT PRIMARY KEY,
    project_id TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);

CREATE TABLE project_roots (
    project_id TEXT NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    path TEXT NOT NULL,
    PRIMARY KEY (project_id, position)
);

CREATE TABLE projects (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    metadata TEXT NOT NULL DEFAULT '{}',
    position INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE remote_control_enrollments (
    websocket_url TEXT NOT NULL,
    account_id TEXT NOT NULL,
    app_server_client_name TEXT NOT NULL,
    server_id TEXT NOT NULL,
    environment_id TEXT NOT NULL,
    server_name TEXT NOT NULL,
    updated_at INTEGER NOT NULL, remote_control_enabled INTEGER,
    PRIMARY KEY (websocket_url, account_id, app_server_client_name)
);

CREATE TABLE rollout_migration_skipped_rollouts (
    migration_id TEXT NOT NULL,
    rollout_path TEXT NOT NULL,
    rollout_size_bytes INTEGER NOT NULL,
    rollout_modified_at_ns INTEGER NOT NULL,
    skip_reason TEXT NOT NULL,
    skipped_at INTEGER NOT NULL,
    PRIMARY KEY (migration_id, rollout_path)
);

CREATE TABLE rollout_migration_state (
    migration_id TEXT PRIMARY KEY,
    last_checked_thread_created_at INTEGER,
    last_checked_thread_id TEXT,
    updated_at INTEGER NOT NULL
);

CREATE TABLE "thread_attachments" (
    id TEXT PRIMARY KEY,
    thread_id TEXT NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    attachment_type TEXT NOT NULL,
    identity_key TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE (thread_id, attachment_type, identity_key)
);

CREATE TABLE thread_dynamic_tools (
    thread_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    name TEXT NOT NULL,
    description TEXT NOT NULL,
    input_schema TEXT NOT NULL, defer_loading INTEGER NOT NULL DEFAULT 0, namespace TEXT,
    PRIMARY KEY(thread_id, position),
    FOREIGN KEY(thread_id) REFERENCES threads(id) ON DELETE CASCADE
);

CREATE TABLE thread_read_receipts (
    thread_id TEXT PRIMARY KEY NOT NULL,
    first_unread_turn TEXT,
    revision TEXT NOT NULL,
    last_published_turn TEXT
);

CREATE TABLE thread_sections (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL
, appearance TEXT);

CREATE TABLE thread_spawn_edges (
    parent_thread_id TEXT NOT NULL,
    child_thread_id TEXT NOT NULL PRIMARY KEY,
    status TEXT NOT NULL
);

CREATE TABLE threads (
    id TEXT PRIMARY KEY,
    rollout_path TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    source TEXT NOT NULL,
    model_provider TEXT NOT NULL,
    cwd TEXT NOT NULL,
    title TEXT NOT NULL,
    sandbox_policy TEXT NOT NULL,
    approval_mode TEXT NOT NULL,
    tokens_used INTEGER NOT NULL DEFAULT 0,
    has_user_event INTEGER NOT NULL DEFAULT 0,
    archived INTEGER NOT NULL DEFAULT 0,
    archived_at INTEGER,
    git_sha TEXT,
    git_branch TEXT,
    git_origin_url TEXT
, cli_version TEXT NOT NULL DEFAULT '', first_user_message TEXT NOT NULL DEFAULT '', agent_nickname TEXT, agent_role TEXT, memory_mode TEXT NOT NULL DEFAULT 'enabled', model TEXT, reasoning_effort TEXT, agent_path TEXT, created_at_ms INTEGER, updated_at_ms INTEGER, thread_source TEXT, preview TEXT NOT NULL DEFAULT '', recency_at INTEGER NOT NULL DEFAULT 0, recency_at_ms INTEGER NOT NULL DEFAULT 0, history_mode TEXT NOT NULL DEFAULT 'legacy', name TEXT, is_pinned INTEGER NOT NULL DEFAULT 0, thread_section_id TEXT
    REFERENCES thread_sections(id) ON DELETE SET NULL, section_position INTEGER, section_entered_at_ms INTEGER, project_id TEXT
    REFERENCES projects(id) ON DELETE SET NULL, originator TEXT, daybreak_enabled BOOLEAN, creator_user_id TEXT, creator_account_id TEXT, attached_thread_parent_id TEXT);

CREATE INDEX idx_projects_position
    ON projects(position ASC, id ASC);

CREATE INDEX idx_thread_attachments_identity_thread
    ON thread_attachments(attachment_type, identity_key, thread_id);

CREATE INDEX idx_thread_attachments_thread_created_id
    ON thread_attachments(thread_id, created_at, id);

CREATE INDEX idx_thread_dynamic_tools_thread ON thread_dynamic_tools(thread_id);

CREATE INDEX idx_thread_spawn_edges_parent_status
    ON thread_spawn_edges(parent_thread_id, status);

CREATE INDEX idx_threads_archive_created_at_ms
    ON threads(archived, created_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archive_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archive_updated_at_ms
    ON threads(archived, updated_at_ms DESC, id DESC)
    WHERE archived = 1;

CREATE INDEX idx_threads_archived ON threads(archived);

CREATE INDEX idx_threads_archived_cwd_created_at_ms ON threads(archived, cwd, created_at_ms DESC, id DESC);

CREATE INDEX idx_threads_archived_cwd_recency_at_ms
    ON threads(archived, cwd, recency_at_ms DESC, id DESC);

CREATE INDEX idx_threads_archived_cwd_updated_at_ms ON threads(archived, cwd, updated_at_ms DESC, id DESC);

CREATE INDEX idx_threads_attached_parent ON threads(attached_thread_parent_id)
    WHERE attached_thread_parent_id IS NOT NULL;

CREATE INDEX idx_threads_created_at ON threads(created_at DESC, id DESC);

CREATE INDEX idx_threads_created_at_ms ON threads(created_at_ms DESC, id DESC);

CREATE INDEX idx_threads_pinned_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE is_pinned = 1 AND preview <> '';

CREATE INDEX idx_threads_project_id
    ON threads(project_id, archived, created_at_ms DESC, id DESC)
    WHERE project_id IS NOT NULL;

CREATE INDEX idx_threads_project_recency
    ON threads(project_id, recency_at_ms DESC)
    WHERE archived = 0 AND project_id IS NOT NULL;

CREATE INDEX idx_threads_provider ON threads(model_provider);

CREATE INDEX idx_threads_recency_at_ms
    ON threads(recency_at_ms DESC, id DESC);

CREATE INDEX idx_threads_section_position
    ON threads(archived, thread_section_id, section_position ASC, id ASC)
    WHERE thread_section_id IS NOT NULL;

CREATE INDEX idx_threads_section_recency_at_ms
    ON threads(archived, thread_section_id, recency_at_ms DESC, id DESC)
    WHERE thread_section_id IS NOT NULL;

CREATE INDEX idx_threads_source ON threads(source);

CREATE INDEX idx_threads_updated_at ON threads(updated_at DESC, id DESC);

CREATE INDEX idx_threads_updated_at_ms ON threads(updated_at_ms DESC, id DESC);

CREATE INDEX idx_threads_visible_created_at_ms
    ON threads(archived, created_at_ms DESC)
    WHERE preview <> '';

CREATE INDEX idx_threads_visible_recency_at_ms
    ON threads(archived, recency_at_ms DESC, id DESC)
    WHERE preview <> '';

CREATE INDEX idx_threads_visible_updated_at_ms
    ON threads(archived, updated_at_ms DESC)
    WHERE preview <> '';

CREATE TABLE thread_history_projection_state (
    thread_id TEXT PRIMARY KEY,
    next_rollout_byte_offset INTEGER NOT NULL,
    next_rollout_ordinal INTEGER NOT NULL
);

CREATE TABLE thread_items (
    thread_id TEXT NOT NULL,
    turn_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    item_json TEXT NOT NULL, item_type TEXT NOT NULL DEFAULT '', updated_at_ordinal INTEGER NOT NULL DEFAULT 0, started_at_ms INTEGER, completed_at_ms INTEGER,
    PRIMARY KEY (thread_id, turn_id, item_id)
);

CREATE TABLE thread_realtime_items (
    thread_id TEXT NOT NULL,
    item_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    item_type TEXT NOT NULL,
    item_json TEXT NOT NULL,
    PRIMARY KEY (thread_id, item_id)
);

CREATE TABLE thread_turns (
    thread_id TEXT NOT NULL,
    turn_id TEXT NOT NULL,
    rollout_ordinal INTEGER NOT NULL,
    status TEXT NOT NULL,
    error_json TEXT,
    started_at INTEGER,
    completed_at INTEGER,
    duration_ms INTEGER,
    first_user_item_id TEXT,
    final_agent_item_id TEXT, rollout_byte_offset INTEGER, rollout_end_ordinal INTEGER, rollout_end_byte_offset INTEGER, root_turn_id TEXT,
    PRIMARY KEY (thread_id, turn_id)
);

CREATE INDEX idx_thread_items_by_turn_page
    ON thread_items(thread_id, turn_id, rollout_ordinal);

CREATE INDEX idx_thread_items_by_turn_updated_page
    ON thread_items(thread_id, turn_id, updated_at_ordinal);

CREATE UNIQUE INDEX idx_thread_items_page
    ON thread_items(thread_id, rollout_ordinal);

CREATE INDEX idx_thread_items_updated_page
    ON thread_items(thread_id, updated_at_ordinal);

CREATE INDEX idx_thread_items_user_messages
    ON thread_items(thread_id, rollout_ordinal)
    WHERE item_type = 'userMessage';

CREATE INDEX idx_thread_realtime_items_boundary
    ON thread_realtime_items(thread_id, rollout_ordinal)
    WHERE item_type IN ('realtime_session_started', 'realtime_session_closed');

CREATE UNIQUE INDEX idx_thread_realtime_items_page
    ON thread_realtime_items(thread_id, rollout_ordinal);

CREATE INDEX idx_thread_turns_end_page
    ON thread_turns(thread_id, rollout_end_ordinal, turn_id)
    WHERE rollout_end_ordinal IS NOT NULL;

CREATE UNIQUE INDEX idx_thread_turns_page
    ON thread_turns(thread_id, rollout_ordinal);

CREATE TABLE thread_goal_continuation_deferrals (
    thread_id TEXT PRIMARY KEY NOT NULL REFERENCES thread_goals(thread_id) ON DELETE CASCADE
);

CREATE TABLE thread_goals (
    thread_id TEXT PRIMARY KEY NOT NULL,
    goal_id TEXT NOT NULL,
    objective TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN (
        'active',
        'paused',
        'blocked',
        'usage_limited',
        'budget_limited',
        'complete'
    )),
    token_budget INTEGER,
    tokens_used INTEGER NOT NULL DEFAULT 0,
    time_used_seconds INTEGER NOT NULL DEFAULT 0,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE consolidation_progress (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    max_thread_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE jobs (
    kind TEXT NOT NULL,
    job_key TEXT NOT NULL,
    status TEXT NOT NULL,
    worker_id TEXT,
    ownership_token TEXT,
    started_at INTEGER,
    finished_at INTEGER,
    lease_until INTEGER,
    retry_at INTEGER,
    retry_remaining INTEGER NOT NULL,
    last_error TEXT,
    input_watermark INTEGER,
    last_success_watermark INTEGER,
    PRIMARY KEY (kind, job_key)
);

CREATE TABLE stage1_outputs (
    thread_id TEXT PRIMARY KEY,
    source_updated_at INTEGER NOT NULL,
    raw_memory TEXT NOT NULL,
    rollout_summary TEXT NOT NULL,
    rollout_slug TEXT,
    generated_at INTEGER NOT NULL,
    usage_count INTEGER,
    last_usage INTEGER,
    selected_for_phase2 INTEGER NOT NULL DEFAULT 0,
    selected_for_phase2_source_updated_at INTEGER
);

CREATE INDEX idx_jobs_kind_status_retry_lease
    ON jobs(kind, status, retry_at, lease_until);

CREATE INDEX idx_stage1_outputs_source_updated_at
    ON stage1_outputs(source_updated_at DESC, thread_id DESC);

CREATE TABLE queued_items (
    id TEXT PRIMARY KEY NOT NULL,
    thread_id TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    queue_order INTEGER NOT NULL,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE TABLE queued_thread_revisions (
    revision INTEGER PRIMARY KEY AUTOINCREMENT,
    thread_id TEXT NOT NULL UNIQUE
);

CREATE UNIQUE INDEX queued_items_thread_order_idx
    ON queued_items(thread_id, queue_order);
)FIXTURE";
const char* const expectedLogSchema = R"FIXTURE(
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    ts_nanos INTEGER NOT NULL,
    level TEXT NOT NULL,
    target TEXT NOT NULL,
    feedback_log_body TEXT,
    module_path TEXT,
    file TEXT,
    line INTEGER,
    thread_id TEXT,
    process_uuid TEXT,
    estimated_bytes INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_logs_process_uuid_threadless_ts ON logs(process_uuid, ts DESC, ts_nanos DESC, id DESC)
WHERE thread_id IS NULL;

CREATE INDEX idx_logs_thread_id ON logs(thread_id);

CREATE INDEX idx_logs_thread_id_ts ON logs(thread_id, ts DESC, ts_nanos DESC, id DESC);

CREATE INDEX idx_logs_ts ON logs(ts DESC, ts_nanos DESC, id DESC);
)FIXTURE";

const char* const bodySentinel = "{\"text\":\"SCHEMA_BODY_SENTINEL_20261125_8a113a\"}";

// A controlled stand-in isolates SQLite's generated-column behavior from filesystem IO.
// The production store must install its real bounded rollout reader on every connection.
void rolloutItem(sqlite3_context* context, int count, sqlite3_value** values) {
    if (count != 3 || sqlite3_value_type(values[0]) == SQLITE_NULL ||
        std::strcmp(reinterpret_cast<const char*>(sqlite3_value_text(values[0])), "stub://body") ||
        sqlite3_value_int64(values[1]) != 7 ||
        sqlite3_value_int64(values[2]) != static_cast<sqlite3_int64>(std::strlen(bodySentinel))) {
        sqlite3_result_null(context);
        return;
    }
    sqlite3_result_text(context, bodySentinel, -1, SQLITE_STATIC);
}

bool execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        std::printf("SQL error (%d): %s\n", result, error ? error : "");
    }
    sqlite3_free(error);
    return result == SQLITE_OK;
}

sqlite3* openDatabase(const char* path = ":memory:") {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path, &database) == SQLITE_OK);
    CHECK(sqlite3_create_function_v2(database, "mai_rollout_item_json", 3,
                                     SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, nullptr,
                                     rolloutItem, nullptr, nullptr, nullptr) == SQLITE_OK);
    return database;
}

using Rows = std::vector<std::vector<std::string>>;

Rows query(sqlite3* database, const std::string& sql) {
    Rows rows;
    sqlite3_stmt* statement = nullptr;
    int prepareResult = sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr);
    CHECK(prepareResult == SQLITE_OK);
    if (prepareResult != SQLITE_OK) {
        std::printf("Query error: %s\n", sqlite3_errmsg(database));
        return rows;
    }
    int result = SQLITE_OK;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int index = 0; index < sqlite3_column_count(statement); ++index) {
            const unsigned char* value = sqlite3_column_text(statement, index);
            row.emplace_back(value ? reinterpret_cast<const char*>(value) : "<NULL>");
        }
        rows.push_back(row);
    }
    CHECK(result == SQLITE_DONE);
    CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    return rows;
}

void verifyMetadata(sqlite3* actual, sqlite3* expected) {
    auto tables = query(expected,
                        "SELECT name FROM sqlite_master WHERE type='table' "
                        "AND name NOT LIKE 'sqlite_%' ORDER BY name");
    CHECK(query(actual,
                "SELECT name FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%' ORDER BY name") == tables);
    for (const auto& table : tables) {
        const std::string& name = table[0];
        auto columns = query(actual, "PRAGMA table_xinfo('" + name + "')");
        auto expectedColumns = query(expected, "PRAGMA table_xinfo('" + name + "')");
        if (name == "thread_items" || name == "thread_realtime_items") {
            CHECK(columns.size() == expectedColumns.size() + 3);
            if (columns.size() == expectedColumns.size() + 3) {
                const std::size_t count = expectedColumns.size();
                CHECK((columns[count] == std::vector<std::string>{std::to_string(count),
                                                                  "mai_rollout_path", "TEXT", "1",
                                                                  "<NULL>", "0", "0"}));
                CHECK((columns[count + 1] == std::vector<std::string>{std::to_string(count + 1),
                                                                      "byte_offset", "INTEGER", "1",
                                                                      "<NULL>", "0", "0"}));
                CHECK((columns[count + 2] == std::vector<std::string>{std::to_string(count + 2),
                                                                      "byte_len", "INTEGER", "1",
                                                                      "<NULL>", "0", "0"}));
            }
            for (auto& column : columns) {
                if (column[1] == "item_json") {
                    CHECK(column[2] == "TEXT");
                    CHECK(column[3] == "1");
                    CHECK(column[6] == "2");
                    column[6] = "0";
                }
            }
            if (columns.size() >= expectedColumns.size()) {
                columns.resize(expectedColumns.size());
            }
        }
        CHECK(columns == expectedColumns);
        CHECK(query(actual, "PRAGMA foreign_key_list('" + name + "')") ==
              query(expected, "PRAGMA foreign_key_list('" + name + "')"));
        CHECK(query(actual, "PRAGMA index_list('" + name + "')") ==
              query(expected, "PRAGMA index_list('" + name + "')"));
    }
    // Explicit index SQL retains sort order, predicates and expression spelling.
    CHECK(query(actual,
                "SELECT name,tbl_name,sql FROM sqlite_master WHERE type='index' "
                "AND sql IS NOT NULL ORDER BY name") ==
          query(expected,
                "SELECT name,tbl_name,sql FROM sqlite_master WHERE type='index' "
                "AND sql IS NOT NULL ORDER BY name"));
    CHECK(query(actual, "SELECT name FROM sqlite_master WHERE name='_sqlx_migrations'").empty());
}

bool containsBytes(const std::string& path, const char* sentinel) {
    FILE* file = nullptr;
#ifdef _WIN32
    // SQLite keeps its writer handle open; the scan must allow that existing writer.
    file = _fsopen(path.c_str(), "rb", _SH_DENYNO);
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) {
        CHECK(file != nullptr);
        return false;
    }
    std::string bytes;
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
        bytes.append(buffer, count);
    }
    std::fclose(file);
    return bytes.find(sentinel) != std::string::npos;
}

void testBusinessMetadata() {
    sqlite3* actual = openDatabase();
    sqlite3* expected = openDatabase();
    CHECK(execute(actual, maiCodexBusinessSchemaSql()));
    CHECK(execute(expected, expectedBusinessSchema));
    verifyMetadata(actual, expected);
    CHECK(query(actual, "PRAGMA table_xinfo('threads')").size() == 43);
    CHECK(query(actual, "PRAGMA table_xinfo('thread_turns')").size() == 14);
    CHECK(query(actual, "PRAGMA table_xinfo('thread_items')").size() == 13);
    CHECK(query(actual, "PRAGMA table_xinfo('thread_realtime_items')").size() == 9);
    CHECK(query(actual, "SELECT name FROM sqlite_master WHERE name='logs'").empty());
    CHECK(std::strstr(maiCodexSchemaBaselineId(),
                      "cf1112822dcced2f5e31b421a0603396200d946a6a1c2a9cd3893b9a924bd980") !=
          nullptr);
    sqlite3_close(expected);
    sqlite3_close(actual);
}

void testSeparateLogsMetadata() {
    sqlite3* actual = openDatabase();
    sqlite3* expected = openDatabase();
    CHECK(execute(actual, maiCodexLogSchemaSql()));
    CHECK(execute(expected, expectedLogSchema));
    verifyMetadata(actual, expected);
    CHECK(query(actual, "SELECT name FROM sqlite_master WHERE name='threads'").empty());
    sqlite3_close(expected);
    sqlite3_close(actual);
}

void testVirtualBodyHasNoDatabaseCopy() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string path = "mai-schema-contract-" + std::to_string(nonce) + ".sqlite";
    sqlite3* database = openDatabase(path.c_str());
    CHECK(execute(database,
                  "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; "
                  "PRAGMA trusted_schema=OFF; PRAGMA foreign_keys=ON;"));
    CHECK(execute(database, maiCodexBusinessSchemaSql()));
    const std::string insert =
        "INSERT INTO thread_items(thread_id,turn_id,item_id,rollout_ordinal,"
        "created_at_ms,mai_rollout_path,byte_offset,byte_len) "
        "VALUES('thread','turn','item',3,5,'stub://body',7," +
        std::to_string(std::strlen(bodySentinel)) + ")";
    CHECK(execute(database, insert.c_str()));
    const std::string realtimeInsert =
        "INSERT INTO thread_realtime_items(thread_id,item_id,rollout_ordinal,created_at_ms,"
        "item_type,mai_rollout_path,byte_offset,byte_len) "
        "VALUES('thread','realtime',4,6,'message','stub://body',7," +
        std::to_string(std::strlen(bodySentinel)) + ")";
    CHECK(execute(database, realtimeInsert.c_str()));
    CHECK(query(database, "SELECT item_json FROM thread_realtime_items") == Rows{{bodySentinel}});
    CHECK((query(database, "SELECT item_json,typeof(item_json) FROM thread_items") ==
           Rows{{bodySentinel, "text"}}));
    CHECK(query(database, "PRAGMA integrity_check") == Rows{{"ok"}});
    CHECK(!containsBytes(path, bodySentinel));
    CHECK(!containsBytes(path + "-wal", bodySentinel));

    // NOT NULL must still reject a rollout callback that cannot resolve its body.
    const char* invalidInsert =
        "INSERT INTO thread_items(thread_id,turn_id,item_id,rollout_ordinal,created_at_ms,"
        "mai_rollout_path,byte_offset,byte_len) VALUES('t','t','missing',0,0,'missing',0,1)";
    CHECK(sqlite3_exec(database, invalidInsert, nullptr, nullptr, nullptr) == SQLITE_CONSTRAINT);
    // Direct writes cannot sneak a second persisted JSON body into the generated column.
    CHECK(sqlite3_exec(database, "UPDATE thread_items SET item_json='duplicated-body'", nullptr,
                       nullptr, nullptr) == SQLITE_ERROR);
    CHECK(sqlite3_close(database) == SQLITE_OK);
    CHECK(!containsBytes(path, bodySentinel));

    database = openDatabase(path.c_str());
    CHECK(query(database, "SELECT item_json FROM thread_items") == Rows{{bodySentinel}});
    CHECK(sqlite3_close(database) == SQLITE_OK);

    // An ordinary read-only SQLite connection can inspect all metadata without the reader.
    // Only selecting the virtual body requires the account's authorized rollout callback.
    database = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    CHECK(execute(database, "PRAGMA trusted_schema=OFF"));
    CHECK(!query(database, "SELECT name,sql FROM sqlite_master").empty());
    CHECK(query(database, "PRAGMA table_xinfo('thread_items')").size() == 13);
    CHECK(query(database, "PRAGMA table_xinfo('thread_realtime_items')").size() == 9);
    CHECK((query(database, "SELECT thread_id,item_id FROM thread_items") ==
           Rows{{"thread", "item"}}));
    CHECK(query(database, "SELECT count(*) FROM thread_items") == Rows{{"1"}});
    CHECK((query(database, "SELECT thread_id,item_id FROM thread_realtime_items") ==
           Rows{{"thread", "realtime"}}));
    CHECK(query(database, "SELECT count(*) FROM thread_realtime_items") == Rows{{"1"}});
    CHECK(query(database, "SELECT id,title FROM threads").empty());
    CHECK(query(database, "SELECT id,name FROM projects").empty());
    for (const char* table : {"thread_items", "thread_realtime_items"}) {
        sqlite3_stmt* statement = nullptr;
        const std::string sql = std::string("SELECT item_json FROM ") + table;
        CHECK(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_ERROR);
        CHECK(std::strstr(sqlite3_errmsg(database), "unknown function: mai_rollout_item_json()") !=
              nullptr);
        CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    }
    CHECK(sqlite3_close(database) == SQLITE_OK);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

}  // namespace

int main() {
    testBusinessMetadata();
    testSeparateLogsMetadata();
    testVirtualBodyHasNoDatabaseCopy();
    std::printf("MaiCodexStoreSchemaTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
