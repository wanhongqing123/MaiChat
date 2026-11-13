// 文件作用：用两种正式后端验证旧上下文副本的有界物理回收，不触碰当前或普通消息。
#include "MaiContextImport.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSessionStore.h"
#include "MaiSqliteStore.h"

#include <sqlite3.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(value)                                    \
    do {                                                \
        if (!(value)) throw std::runtime_error(#value); \
    } while (false)

static std::vector<MaiContextImportRecord> records(std::size_t count) {
    std::vector<MaiContextImportRecord> result;
    for (std::size_t index = 0; index < count; ++index)
        result.push_back({"row-" + std::to_string(index),
                          "owned historical data " + std::to_string(index),
                          static_cast<std::int64_t>(index + 1)});
    return result;
}

static std::string append(MaiSessionStore& store, const std::string& session,
                          const std::string& source, const std::string& snapshot, std::size_t count,
                          bool publish) {
    const auto begun = store.beginContextImport({session, source, snapshot, "Owned local context"});
    CHECK(begun);
    const auto id = begun.value().importId;
    CHECK(store.appendContextImport(id, 0, records(count)));
    if (publish) CHECK(store.commitContextImport(id));
    return id;
}

static int scalar(sqlite3* database, const char* sql) {
    sqlite3_stmt* query = nullptr;
    CHECK(sqlite3_prepare_v2(database, sql, -1, &query, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    const int value = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return value;
}

static void contract(MaiSessionStore& store, const std::string& databasePath) {
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = ".";
    session.title = "Owned context maintenance";
    store.putSession(session);
    MaiMessage ordinary;
    ordinary.id = MaiIdGenerator::newMessageId();
    ordinary.created = ordinary.completed = 1;
    ordinary.parts.push_back(
        {MaiIdGenerator::newPartId(), MaiTextPart{"ordinary data must survive"}, 1});
    store.putMessage(session.id, ordinary);
    std::string oldest;
    for (int revision = 0; revision < 5; ++revision) {
        const auto id =
            append(store, session.id, "source", "snapshot-" + std::to_string(revision), 6, true);
        if (oldest.empty()) oldest = id;
    }
    const auto aborted = append(store, session.id, "aborted-source", "aborted-snapshot", 4, false);
    CHECK(!store.abortContextImport(aborted));
    CHECK(!store.inspectContextImport(aborted));
    const auto pending = append(store, session.id, "still-staging", "pending-snapshot", 3, false);
    CHECK(store.listMessages(session.id).size() == 7);
    CHECK(!store.purgeContextImports(session.id, 0));
    CHECK(!store.purgeContextImports(session.id, kMaiContextImportMaxPurgeMessages + 1));
    const auto missing = store.purgeContextImports("missing-session", 2);
    CHECK(!missing && missing.error().code() == MaiErrorCode::NotFound);

    std::size_t removed = 0;
    bool complete = false;
    for (int pass = 0; pass < 100; ++pass) {
        const auto purged = store.purgeContextImports(session.id, 2);
        CHECK(purged && purged.value().removed <= 2);
        removed += purged.value().removed;
        CHECK(store.listMessages(session.id).size() == 7);
        MaiMessage kept;
        CHECK(store.getMessage(session.id, ordinary.id, kept) && kept.text() == ordinary.text());
        CHECK(store.inspectContextImport(pending));
        if (!purged.value().hasMore) {
            complete = true;
            break;
        }
    }
    CHECK(complete && removed == 28);
    CHECK(!store.commitContextImport(oldest));
    CHECK(store.listMessages(session.id).size() == 7);
    if (!databasePath.empty()) {
        sqlite3* probe = nullptr;
        CHECK(sqlite3_open_v2(databasePath.c_str(), &probe, SQLITE_OPEN_READONLY, nullptr) ==
              SQLITE_OK);
        CHECK(scalar(probe, "SELECT count(*) FROM messages") == 10);
        CHECK(scalar(probe, "SELECT count(*) FROM parts") == 10);
        CHECK(scalar(probe, "SELECT count(*) FROM context_imports") == 2);
        CHECK(scalar(probe, "SELECT count(*) FROM context_import_chunks") == 2);
        sqlite3_close(probe);
    }
    CHECK(store.commitContextImport(pending));
    CHECK(store.listMessages(session.id).size() == 10);
    CHECK(store.purgeContextImports(session.id, 200).value().removed == 0);
    CHECK(store.clearMessages(session.id));
    CHECK(store.listMessages(session.id).empty());
    CHECK(!store.purgeContextImports(session.id, 200).value().hasMore);
}

int main() {
    const auto directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-context-purge-" + MaiIdGenerator::newSessionId()));
    try {
        CHECK(!MaiFileSystem::createDirectories(directory));
        auto memory = makeMaiMemoryStore();
        contract(*memory, {});
        const auto path = directory.append(MaiFilePath::fromUtf8("owned-context.db")).toUtf8();
        auto sqlite = makeMaiSqliteStore(path);
        CHECK(sqlite);
        contract(*sqlite.value(), path);
        sqlite.value().reset();
        // 与原有 v2/v8/v9 升级回归相同，保留当前附加列并回放旧版本门禁。
        // 新迁移须可重复检查列/表，不能因已有自己的列而拒绝打开全部旧历史。
        sqlite3* legacy = nullptr;
        CHECK(sqlite3_open(path.c_str(), &legacy) == SQLITE_OK);
        CHECK(sqlite3_exec(legacy, "PRAGMA user_version=11", nullptr, nullptr, nullptr) ==
              SQLITE_OK);
        sqlite3_close(legacy);
        auto reopened = makeMaiSqliteStore(path);
        if (!reopened)
            std::fprintf(stderr, "Owned migration replay error: %s\n",
                         reopened.error().message().c_str());
        CHECK(reopened);
        reopened.value().reset();
        MaiFileSystem::removeRecursively(directory);
        std::puts(
            "PASS: memory/SQLite bounded context purge reclaims retired payloads and preserves "
            "current, pending and ordinary data");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        MaiFileSystem::removeRecursively(directory);
        return 1;
    }
}
