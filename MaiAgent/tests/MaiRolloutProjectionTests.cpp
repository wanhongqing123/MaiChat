// 文件作用：使用真实 SQLite 生成列从实际 JSONL 回读正文，并拒绝越界和越权路径。
#include <cstdio>
#include <string>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiCodexStoreSchema.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiRolloutLog.h"
#include "MaiRolloutProjection.h"

namespace {
using Json = nlohmann::json;
int failures = 0;
#define CHECK(value)                                                     \
    do {                                                                \
        if (!(value)) {                                                  \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (false)

void test_actual_log_to_generated_column_and_restricted_paths() {
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("rollout-projection-" + MaiIdGenerator::newSessionId()));
    CHECK(!MaiFileSystem::createDirectories(root));
    const std::string relative = "sessions/owned.jsonl";
    const auto path = root.append(MaiFilePath::fromUtf8(relative));
    auto log = MaiRolloutLog::create(path, R"({"id":"owned","session_id":"ses_owned"})",
                                     1795185882657);
    CHECK(log);
    if (!log) return;
    const Json item{{"type", "agentMessage"}, {"id", "item_owned"},
                    {"text", "owned-body-\u4f60\u597d"},
                    {"vendor", {{"signature", "opaque-signature"}}}};
    const auto location = log.value()->append(
        "event_msg", Json{{"type", "item_completed"}, {"thread_id", "ses_owned"},
                           {"turn_id", "turn_owned"}, {"item", item}}.dump(),
        1795185882658);
    CHECK(location);
    if (!location) return;
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(":memory:", &database) == SQLITE_OK);
    const auto registered = maiRegisterRolloutProjection(database, root);
    CHECK(!registered);
    if (!registered) {
        CHECK(sqlite3_exec(database, "PRAGMA trusted_schema=OFF", nullptr, nullptr, nullptr) == SQLITE_OK);
        CHECK(sqlite3_exec(database, maiCodexBusinessSchemaSql(), nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_stmt* insert = nullptr;
        CHECK(sqlite3_prepare_v2(database,
            "INSERT INTO thread_items(thread_id,turn_id,item_id,rollout_ordinal,created_at_ms,"
            "item_type,updated_at_ordinal,mai_rollout_path,byte_offset,byte_len) "
            "VALUES('ses_owned','turn_owned','item_owned',1,1,'agentMessage',1,?1,?2,?3)",
            -1, &insert, nullptr) == SQLITE_OK);
        if (insert) {
            sqlite3_bind_text(insert, 1, relative.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(insert, 2, static_cast<sqlite3_int64>(location.value().byteOffset));
            sqlite3_bind_int64(insert, 3, static_cast<sqlite3_int64>(location.value().byteLength));
            CHECK(sqlite3_step(insert) == SQLITE_DONE);
            sqlite3_finalize(insert);
        }
        sqlite3_stmt* read = nullptr;
        CHECK(sqlite3_prepare_v2(database, "SELECT item_json FROM thread_items", -1,
                                &read, nullptr) == SQLITE_OK);
        if (read) {
            CHECK(sqlite3_step(read) == SQLITE_ROW);
            const auto text = reinterpret_cast<const char*>(sqlite3_column_text(read, 0));
            CHECK(text != nullptr);
            if (text) CHECK(Json::parse(text) == item);
            sqlite3_finalize(read);
        }
        for (const auto& badPath : {std::string("../outside.jsonl"), path.toUtf8(),
                                    std::string("settings.json"), std::string("sessions/missing.jsonl")}) {
            sqlite3_stmt* invalid = nullptr;
            CHECK(sqlite3_prepare_v2(database, "SELECT mai_rollout_item_json(?1,?2,?3)",
                                    -1, &invalid, nullptr) == SQLITE_OK);
            if (!invalid) continue;
            sqlite3_bind_text(invalid, 1, badPath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(invalid, 2, static_cast<sqlite3_int64>(location.value().byteOffset));
            sqlite3_bind_int64(invalid, 3, static_cast<sqlite3_int64>(location.value().byteLength));
            CHECK(sqlite3_step(invalid) != SQLITE_ROW);
            sqlite3_finalize(invalid);
        }
    }
    CHECK(sqlite3_close(database) == SQLITE_OK);
    log.value().reset();
    MaiDirectoryDeleteOptions cleanup;
    cleanup.recursive = true;
    cleanup.pathAllowed = [&](const MaiFilePath& entry) {
        return entry == root || root.isParentOf(entry);
    };
    CHECK(!MaiFileSystem::deleteDirectory(root, cleanup).error);
}
}  // namespace

int main() {
    test_actual_log_to_generated_column_and_restricted_paths();
    std::printf("Rollout projection failures: %d\n", failures);
    return failures ? 1 : 0;
}
