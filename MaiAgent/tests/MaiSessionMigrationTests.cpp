// 文件作用：用自有 WAL 数据库和各发布边界故障验证无覆盖迁移及精确恢复。
#include "MaiSessionMigration.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stat.h>
#endif
#endif

#include "MaiFileSystem.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

const char kBody[] = "{\"legacy_body\":\"committed WAL conversation\"}\n";
const char kLog[] = "sessions/2026/11/26/rollout-owned.jsonl";

MaiFilePath at(const MaiFilePath& root, const char* name) {
    return root.append(MaiFilePath::fromUtf8(name));
}

bool sql(sqlite3* database, const char* statement) {
    char* error = nullptr;
    const int result = sqlite3_exec(database, statement, nullptr, nullptr, &error);
    if (result != SQLITE_OK) std::printf("SQL error: %s\n", error ? error : "unknown");
    sqlite3_free(error);
    return result == SQLITE_OK;
}

std::string body(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "SELECT body FROM legacy", -1, &statement, nullptr) !=
        SQLITE_OK)
        return {};
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW)
        value.assign(static_cast<const char*>(sqlite3_column_blob(statement, 0)),
                     static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
    sqlite3_finalize(statement);
    return value;
}

bool makeLegacy(const MaiFilePath& root, int version = 15) {
    CHECK(!MaiFileSystem::createDirectories(root));
    const auto source = at(root, "agent.db");
    sqlite3* database = nullptr;
    if (sqlite3_open(source.toUtf8().c_str(), &database) != SQLITE_OK) return false;
    // 禁止最后一个连接在关闭时 checkpoint，使正文真正只在 WAL 提交帧中。
    int previous = 0;
    sqlite3_db_config(database, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &previous);
    int persist = 1;
    sqlite3_file_control(database, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
    bool ok = sql(database, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    ok = sql(database, "CREATE TABLE legacy(body BLOB);") && ok;
    ok = sql(database, ("PRAGMA user_version=" + std::to_string(version)).c_str()) && ok;
    sqlite3_stmt* insert = nullptr;
    ok = sqlite3_prepare_v2(database, "INSERT INTO legacy VALUES(?)", -1, &insert, nullptr) ==
             SQLITE_OK &&
         ok;
    if (insert) {
        sqlite3_bind_blob(insert, 1, kBody, sizeof(kBody) - 1, SQLITE_STATIC);
        ok = sqlite3_step(insert) == SQLITE_DONE && ok;
        sqlite3_finalize(insert);
    }
    sqlite3_close(database);
    CHECK(MaiFileSystem::exists(at(root, "agent.db-wal")));
    CHECK(MaiFileSystem::exists(at(root, "agent.db-shm")));
    return ok;
}

MaiSessionMigrationOptions options(const MaiFilePath& root, int& builds) {
    MaiSessionMigrationOptions result;
    result.storageRoot = root;
    result.sourceIndexPath = at(root, "agent.db");
    result.build = [&builds](sqlite3* snapshot, const MaiFilePath& stage,
                             const MaiFilePath& index) -> MaiError {
        ++builds;
        CHECK(sqlite3_db_readonly(snapshot, "main") == 1);
        sqlite3_stmt* version = nullptr;
        if (sqlite3_prepare_v2(snapshot, "PRAGMA user_version", -1, &version, nullptr) != SQLITE_OK)
            return {MaiErrorCode::Internal, "version query failed"};
        const int value = sqlite3_step(version) == SQLITE_ROW ? sqlite3_column_int(version, 0) : 0;
        sqlite3_finalize(version);
        if (value < 1 || value > 15)
            return {MaiErrorCode::NotSupported, "unsupported legacy version"};
        const auto contents = body(snapshot);
        if (contents != kBody)
            return {MaiErrorCode::InvalidInput, "WAL snapshot lost committed body"};
        auto error = MaiFileSystem::createDirectories(at(stage, "sessions/2026/11/26"));
        if (!error) error = MaiFileSystem::writeFile(at(stage, kLog), contents);
        if (error) return error;
        sqlite3* output = nullptr;
        if (sqlite3_open(index.toUtf8().c_str(), &output) != SQLITE_OK) {
            sqlite3_close(output);
            return {MaiErrorCode::Internal, "index open failed"};
        }
        const bool ok = sql(
            output,
            "CREATE TABLE index_only(session_id TEXT, path TEXT);"
            "INSERT INTO index_only VALUES('owned','sessions/2026/11/26/rollout-owned.jsonl');");
        sqlite3_close(output);
        return ok ? MaiError{} : MaiError{MaiErrorCode::Internal, "index build failed"};
    };
    result.validate = [](sqlite3* snapshot, const MaiFilePath& candidate,
                         const MaiFilePath& index) -> MaiError {
        CHECK(sqlite3_db_readonly(snapshot, "main") == 1);
        std::string log;
        auto error = MaiFileSystem::readFile(at(candidate, kLog), log);
        if (error) return error;
        if (log != body(snapshot)) return {MaiErrorCode::InvalidInput, "body mismatch"};
        sqlite3* output = nullptr;
        const int status =
            sqlite3_open_v2(index.toUtf8().c_str(), &output, SQLITE_OPEN_READONLY, nullptr);
        if (status != SQLITE_OK) {
            sqlite3_close(output);
            return {MaiErrorCode::Internal, "index validation open failed"};
        }
        sqlite3_stmt* count = nullptr;
        bool ok = sqlite3_prepare_v2(output, "SELECT COUNT(*) FROM index_only", -1, &count,
                                     nullptr) == SQLITE_OK;
        if (ok) ok = sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 1;
        sqlite3_finalize(count);
        sqlite3_close(output);
        return ok ? MaiError{} : MaiError{MaiErrorCode::InvalidInput, "index mismatch"};
    };
    return result;
}

void verifyPublished(const MaiFilePath& root) {
    std::string contents;
    CHECK(!MaiFileSystem::readFile(at(root, kLog), contents));
    CHECK(contents == kBody);
    CHECK(MaiFileSystem::exists(at(root, "agent.db.legacy-session-backup")));
    CHECK(MaiFileSystem::exists(at(root, "agent.db.legacy-session-backup-wal")));
    CHECK(MaiFileSystem::exists(at(root, "agent.db.legacy-session-backup-shm")));
    CHECK(!MaiFileSystem::exists(at(root, "agent.db-wal")));
    CHECK(!MaiFileSystem::exists(at(root, "agent.db-shm")));
    CHECK(!MaiSessionMigration::hasPending(root, at(root, "agent.db")));
    sqlite3* backup = nullptr;
    const auto path = at(root, "agent.db.legacy-session-backup");
    CHECK(sqlite3_open_v2(path.toUtf8().c_str(), &backup, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
    if (backup) CHECK(body(backup) == kBody);
    sqlite3_close(backup);
}

void testEveryPublicationBoundary(const MaiFilePath& parent) {
    for (const char* step : {"ready", "logs_published", "source_preserved", "wal_preserved",
                             "shm_preserved", "index_published", "complete"}) {
        const auto root = at(parent, step);
        CHECK(makeLegacy(root));
        int builds = 0;
        auto request = options(root, builds);
        int injections = 0;
        request.checkpoint = [&](const std::string& actual) -> MaiError {
            if (actual == step) {
                ++injections;
                return {MaiErrorCode::Canceled, "owned injected interruption"};
            }
            return {};
        };
        CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
        CHECK(injections == 1);
        CHECK(builds == 1);
        // 同一个故障回调仍在；已完成的发布步骤不会重新执行或重复注入。
        const auto resumed = MaiSessionMigration::run(request);
        if (resumed) std::printf("resume %s: %s\n", step, resumed.message().c_str());
        CHECK(!resumed);
        CHECK(injections == 1);
        CHECK(builds == 1);
        verifyPublished(root);
    }
}

#ifdef __APPLE__
void testApplePreservesSourceEvidence(const MaiFilePath& parent) {
    const auto root = at(parent, "apple-preserved-evidence");
    CHECK(makeLegacy(root));
    int builds = 0;
    auto request = options(root, builds);
    struct Evidence {
        std::string bytes;
        struct stat info{};
    };
    std::vector<Evidence> original(3), ready(3);
    const std::vector<std::string> extensions{"", "-wal", "-shm"};
    const auto capture = [&](std::vector<Evidence>& evidence) {
        for (std::size_t i = 0; i < extensions.size(); ++i) {
            const auto path = at(root, ("agent.db" + extensions[i]).c_str());
            CHECK(!MaiFileSystem::readFile(path, evidence[i].bytes));
            CHECK(::stat(path.value().c_str(), &evidence[i].info) == 0);
        }
    };
    capture(original);
    request.checkpoint = [&](const std::string& step) -> MaiError {
        if (step == "ready") capture(ready);
        return {};
    };
    CHECK(!MaiSessionMigration::run(request));
    for (std::size_t i = 0; i < extensions.size(); ++i) {
        const auto path = at(root, ("agent.db.legacy-session-backup" + extensions[i]).c_str());
        std::string bytes;
        struct stat after{};
        CHECK(!MaiFileSystem::readFile(path, bytes));
        CHECK(::stat(path.value().c_str(), &after) == 0);
        CHECK(bytes == ready[i].bytes);
        // SHM是读者索引；只比较冻结ready后的保全值。正文主库/WAL从guard前就不变。
        if (i < 2) CHECK(bytes == original[i].bytes);
        CHECK(after.st_ino == original[i].info.st_ino && after.st_dev == original[i].info.st_dev);
        CHECK(after.st_mode == original[i].info.st_mode);
        CHECK(after.st_mtimespec.tv_sec == ready[i].info.st_mtimespec.tv_sec &&
              after.st_mtimespec.tv_nsec == ready[i].info.st_mtimespec.tv_nsec);
    }
    verifyPublished(root);
}

void testAppleReplayInterruptedAgain(const MaiFilePath& parent) {
    // 不改原发布断言；补充逆向rename自身再次中断后的同一请求重开。
    for (const char* initial : {"source_preserved", "wal_preserved"}) {
        for (const char* replay : {"apple_wal_restored", "apple_source_restored"}) {
            if (std::string(initial) == "source_preserved" &&
                std::string(replay) == "apple_wal_restored")
                continue;
            const auto root = at(parent, (std::string(initial) + "-" + replay).c_str());
            CHECK(makeLegacy(root));
            int builds = 0;
            auto request = options(root, builds);
            request.checkpoint = [&](const std::string& step) -> MaiError {
                return step == initial ? MaiError{MaiErrorCode::Canceled, "initial interruption"}
                                       : MaiError{};
            };
            CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
            int replayInterruptions = 0;
            request.checkpoint = [&](const std::string& step) -> MaiError {
                if (step == replay) {
                    ++replayInterruptions;
                    return {MaiErrorCode::Canceled, "replay interruption"};
                }
                return {};
            };
            CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
            CHECK(replayInterruptions == 1);
            CHECK(!MaiSessionMigration::run(request));
            CHECK(replayInterruptions == 1);
            CHECK(builds == 1);
            verifyPublished(root);
        }
    }
}
#endif

void testCorruptionAndForeignSource(const MaiFilePath& parent) {
    const auto corrupt = at(parent, "corrupt");
    CHECK(makeLegacy(corrupt));
    int builds = 0;
    auto request = options(corrupt, builds);
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "ready" ? MaiError{MaiErrorCode::Canceled, "stop before publish"}
                               : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(!MaiFileSystem::writeFile(
        at(corrupt, ".agent.db.session-migration/sessions/2026/11/26/rollout-owned.jsonl"),
        "corrupt"));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 1);
    CHECK(MaiFileSystem::exists(at(corrupt, "agent.db")));
    CHECK(!MaiFileSystem::exists(at(corrupt, "sessions")));

    const auto foreign = at(parent, "foreign-gap");
    CHECK(makeLegacy(foreign));
    int foreignBuilds = 0;
    request = options(foreign, foreignBuilds);
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "source_preserved" ? MaiError{MaiErrorCode::Canceled, "stop in gap"}
                                          : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(!MaiFileSystem::writeFile(at(foreign, "agent.db"), "foreign newly created database"));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    std::string contents;
    CHECK(!MaiFileSystem::readFile(at(foreign, "agent.db"), contents));
    CHECK(contents == "foreign newly created database");
    CHECK(foreignBuilds == 1);
    CHECK(MaiFileSystem::exists(at(foreign, "agent.db.legacy-session-backup")));
}

void testReplacedSourceAndVersion(const MaiFilePath& parent) {
    const auto replaced = at(parent, "replaced");
    CHECK(makeLegacy(replaced));
    int builds = 0;
    auto request = options(replaced, builds);
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "ready" ? MaiError{MaiErrorCode::Canceled, "stop"} : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    // 内容完全相同但节点不同也必须拒绝，不能仅凭 checksum 认领新出现的路径。
    CHECK(!MaiFileSystem::copyFile(at(replaced, "agent.db"), at(replaced, "other.db")));
    CHECK(!MaiFileSystem::removeFile(at(replaced, "agent.db")));
    CHECK(!MaiFileSystem::publishNewFile(at(replaced, "other.db"), at(replaced, "agent.db")));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 1);

    const auto version = at(parent, "version");
    CHECK(makeLegacy(version, 99));
    int versionBuilds = 0;
    auto unsupported = options(version, versionBuilds);
    CHECK(MaiSessionMigration::run(unsupported).code() == MaiErrorCode::NotSupported);
    CHECK(MaiFileSystem::exists(at(version, "agent.db")));
    CHECK(!MaiFileSystem::exists(at(version, "sessions")));
    CHECK(!MaiFileSystem::exists(at(version, "agent.db.legacy-session-backup")));
}

void testAccountBoundaryAndActiveWriter(const MaiFilePath& parent) {
    const auto root = at(parent, "account");
    const auto other = at(parent, "other-account");
    CHECK(makeLegacy(root));
    CHECK(makeLegacy(other));
    int builds = 0;
    auto request = options(root, builds);
    request.sourceIndexPath = at(other, "agent.db");
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 0);
#ifdef _WIN32
    request = options(root, builds);
    sqlite3* active = nullptr;
    CHECK(sqlite3_open(at(root, "agent.db").toUtf8().c_str(), &active) == SQLITE_OK);
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Busy);
    CHECK(builds == 0);
    CHECK(!MaiFileSystem::exists(at(root, ".agent.db.session-migration")));
    sqlite3_close(active);
#endif
}

// 真正通过 SQLite 提交，验证快照后的写入不能被构建后的新指纹掩盖。
// Windows 原生共享句柄已经排斥 writer；POSIX 必须拒绝过时快照的发布。
void testWriterAfterSnapshotCannotPublishOldBody(const MaiFilePath& parent) {
    for (const char* boundary : {"snapshot_created", "build_complete"}) {
        const auto root = at(at(parent, "post-snapshot-writer"), boundary);
        CHECK(makeLegacy(root));
        int builds = 0;
        auto request = options(root, builds);
        bool attempted = false;
        bool committed = false;
        request.checkpoint = [&](const std::string& step) -> MaiError {
            if (step != boundary) return {};
            attempted = true;
            sqlite3* writer = nullptr;
            const int opened = sqlite3_open_v2(at(root, "agent.db").toUtf8().c_str(), &writer,
                                               SQLITE_OPEN_READWRITE, nullptr);
            if (opened == SQLITE_OK) {
#ifdef _WIN32
                // SQLite 可通过回退到只读主文件句柄返回打开成功，仍须核对实际写入能力。
                CHECK(sqlite3_db_readonly(writer, "main") == 1);
#endif
                int previous = 0;
                sqlite3_db_config(writer, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &previous);
                int persist = 1;
                sqlite3_file_control(writer, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
                committed = sqlite3_exec(writer, "UPDATE legacy SET body='late committed body';",
                                         nullptr, nullptr, nullptr) == SQLITE_OK;
            }
            sqlite3_close(writer);
            return {};
        };
        const auto error = MaiSessionMigration::run(request);
        CHECK(attempted);
#if defined(_WIN32) || defined(__APPLE__)
        // Windows共享句柄与Apple独立SQLite写保留均拒绝此处的新提交。
        CHECK(!committed);
        CHECK(!error);
        verifyPublished(root);
#else
        CHECK(committed);
        CHECK(error.code() == MaiErrorCode::InvalidInput);
        CHECK(!MaiFileSystem::exists(at(root, "sessions")));
        CHECK(!MaiFileSystem::exists(at(root, "agent.db.legacy-session-backup")));
        // 失败仍保留真实 writer 的新正文和旧快照，不能以回滚迁移之名改写原库。
        sqlite3* original = nullptr;
        CHECK(sqlite3_open_v2(at(root, "agent.db").toUtf8().c_str(), &original,
                              SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        if (original) CHECK(body(original) == "late committed body");
        sqlite3_close(original);
        CHECK(MaiFileSystem::exists(at(root, ".agent.db.session-migration/legacy-snapshot.db")));
#endif
    }
}

void testCancellationAndMalformedReceipt(const MaiFilePath& parent) {
    const auto root = at(parent, "cancel-flag");
    CHECK(makeLegacy(root));
    int builds = 0;
    bool cancel = false;
    auto request = options(root, builds);
    request.isCanceled = [&] { return cancel; };
    request.checkpoint = [&](const std::string& step) -> MaiError {
        if (step == "logs_published") cancel = true;
        return {};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(MaiFileSystem::exists(at(root, "agent.db")));
    CHECK(MaiSessionMigration::hasPending(root, at(root, "agent.db")));
    cancel = false;
    CHECK(!MaiSessionMigration::run(request));
    CHECK(builds == 1);
    verifyPublished(root);

    const auto malformed = at(parent, "malformed");
    CHECK(makeLegacy(malformed));
    int malformedBuilds = 0;
    request = options(malformed, malformedBuilds);
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "ready" ? MaiError{MaiErrorCode::Canceled, "stop"} : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(!MaiFileSystem::writeFile(at(malformed, ".agent.db.session-migration/ready.json"),
                                    "{\"format\":\"unexpected-type\"}"));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(malformedBuilds == 1);
    CHECK(MaiFileSystem::exists(at(malformed, "agent.db")));
}

std::vector<MaiFilePath> abandonedBuilds(const MaiFilePath& root) {
    std::vector<MaiFilePath> result;
    MaiFileSystem::walk(root, [&](const MaiFileEntry& entry) {
        if (entry.isDirectory && entry.nameUtf8.find(".mai-session-migration.abandoned-") == 0)
            result.push_back(entry.path);
        return entry.isDirectory ? MaiWalkAction::SkipDirectory : MaiWalkAction::Continue;
    });
    return result;
}

void testRestartCanceledPrivateBuild(const MaiFilePath& parent) {
    const auto root = at(parent, "restart-canceled-build");
    CHECK(makeLegacy(root));
    std::string originalMain;
    std::string originalWal;
    CHECK(!MaiFileSystem::readFile(at(root, "agent.db"), originalMain));
    CHECK(!MaiFileSystem::readFile(at(root, "agent.db-wal"), originalWal));
    int builds = 0;
    bool cancel = false;
    auto request = options(root, builds);
    request.isCanceled = [&] { return cancel; };
    const auto build = request.build;
    request.build = [&](sqlite3* snapshot, const MaiFilePath& stage,
                        const MaiFilePath& index) -> MaiError {
        const auto error = build(snapshot, stage, index);
        if (error) return error;
        if (builds == 1) cancel = true;
        return {};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(builds == 1);
    CHECK(MaiFileSystem::exists(at(root, ".agent.db.session-migration/building.json")));
    CHECK(!MaiFileSystem::exists(at(root, ".agent.db.session-migration/ready.json")));
    std::string currentMain;
    std::string currentWal;
    CHECK(!MaiFileSystem::readFile(at(root, "agent.db"), currentMain));
    CHECK(!MaiFileSystem::readFile(at(root, "agent.db-wal"), currentWal));
    CHECK(currentMain == originalMain);
    CHECK(currentWal == originalWal);
    cancel = false;
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 1);
    CHECK(abandonedBuilds(root).empty());
    request.restartIncompleteBuild = true;
    const auto resumed = MaiSessionMigration::run(request);
    if (resumed) std::printf("restart canceled build: %s\n", resumed.message().c_str());
    CHECK(!resumed);
    CHECK(builds == 2);
    const auto abandoned = abandonedBuilds(root);
    CHECK(abandoned.size() == 1);
    if (abandoned.size() == 1) {
        std::string bytes;
        CHECK(!MaiFileSystem::readFile(at(abandoned.front(), kLog), bytes));
        CHECK(bytes == kBody);
        CHECK(MaiFileSystem::exists(at(abandoned.front(), "legacy-snapshot.db")));
        CHECK(MaiFileSystem::exists(at(abandoned.front(), "index.db")));
    }
    verifyPublished(root);
    CHECK(!MaiSessionMigration::run(request));
    CHECK(builds == 2);
    CHECK(abandonedBuilds(root).size() == 1);
}

void testRestartBeforeReadyBoundaries(const MaiFilePath& parent) {
    for (const char* boundary : {"building", "snapshot_created", "build_complete"}) {
        const auto root = at(at(parent, "restart-boundaries"), boundary);
        CHECK(makeLegacy(root));
        int builds = 0;
        auto request = options(root, builds);
        request.checkpoint = [&](const std::string& step) -> MaiError {
            return step == boundary ? MaiError{MaiErrorCode::Canceled, "before-ready interruption"}
                                    : MaiError{};
        };
        CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
        CHECK(builds == (std::string(boundary) == "build_complete" ? 1 : 0));
        request.restartIncompleteBuild = true;
        request.checkpoint = {};
        CHECK(!MaiSessionMigration::run(request));
        CHECK(builds == (std::string(boundary) == "build_complete" ? 2 : 1));
        CHECK(abandonedBuilds(root).size() == 1);
        verifyPublished(root);
    }
}

void testRestartRejectsUnownedStage(const MaiFilePath& parent) {
    const auto root = at(parent, "restart-unowned-stage");
    CHECK(makeLegacy(root));
    CHECK(!MaiFileSystem::createDirectories(at(root, ".agent.db.session-migration/sessions")));
    const auto foreign = at(root, ".agent.db.session-migration/sessions/foreign.jsonl");
    CHECK(!MaiFileSystem::writeFile(foreign, "foreign retained bytes"));
    int builds = 0;
    auto request = options(root, builds);
    request.restartIncompleteBuild = true;
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 0);
    CHECK(abandonedBuilds(root).empty());
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(foreign, bytes));
    CHECK(bytes == "foreign retained bytes");
    CHECK(!MaiFileSystem::writeFile(at(root, ".agent.db.session-migration/building.json"),
                                    "{\"format\":1,\"phase\":\"building\",\"root\":\"foreign\"}"));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 0);
    CHECK(abandonedBuilds(root).empty());
}

void testRestartRejectsChangedIdentitiesAndLinks(const MaiFilePath& parent) {
    const auto root = at(parent, "restart-changed-source");
    CHECK(makeLegacy(root));
    int builds = 0;
    auto request = options(root, builds);
    request.restartIncompleteBuild = true;
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "building" ? MaiError{MaiErrorCode::Canceled, "stop before snapshot"}
                                  : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    CHECK(!MaiFileSystem::copyFile(at(root, "agent.db"), at(root, "replacement.db")));
    CHECK(!MaiFileSystem::removeFile(at(root, "agent.db")));
    CHECK(!MaiFileSystem::publishNewFile(at(root, "replacement.db"), at(root, "agent.db")));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(builds == 0);
    CHECK(abandonedBuilds(root).empty());

    const auto changedStage = at(parent, "restart-changed-stage");
    CHECK(makeLegacy(changedStage));
    int changedBuilds = 0;
    request = options(changedStage, changedBuilds);
    request.restartIncompleteBuild = true;
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "building" ? MaiError{MaiErrorCode::Canceled, "stop"} : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    const auto stage = at(changedStage, ".agent.db.session-migration");
    const auto moved = at(changedStage, "saved-owned-stage");
#ifdef _WIN32
    CHECK(::MoveFileExW(stage.value().c_str(), moved.value().c_str(), 0) != 0);
#else
    CHECK(::rename(stage.value().c_str(), moved.value().c_str()) == 0);
#endif
    CHECK(!MaiFileSystem::createDirectories(stage));
    CHECK(!MaiFileSystem::copyFile(at(moved, "building.json"), at(stage, "building.json")));
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
    CHECK(changedBuilds == 0);
    CHECK(abandonedBuilds(changedStage).empty());

    const auto unsafe = at(parent, "restart-unsafe-link");
    CHECK(makeLegacy(unsafe));
    int unsafeBuilds = 0;
    request = options(unsafe, unsafeBuilds);
    request.restartIncompleteBuild = true;
    request.checkpoint = [](const std::string& step) -> MaiError {
        return step == "building" ? MaiError{MaiErrorCode::Canceled, "stop"} : MaiError{};
    };
    CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::Canceled);
    const auto outside = at(parent, "outside-link-target");
    CHECK(!MaiFileSystem::createDirectories(outside));
    CHECK(!MaiFileSystem::writeFile(at(outside, "retained.txt"), "outside unchanged"));
    const auto link = at(unsafe, ".agent.db.session-migration/outside");
#ifdef _WIN32
    const bool created = ::CreateSymbolicLinkW(link.value().c_str(), outside.value().c_str(),
                                               SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != 0;
#else
    const bool created = ::symlink(outside.value().c_str(), link.value().c_str()) == 0;
#endif
    if (created) {
        CHECK(MaiSessionMigration::run(request).code() == MaiErrorCode::InvalidInput);
        CHECK(unsafeBuilds == 0);
        CHECK(abandonedBuilds(unsafe).empty());
        std::string bytes;
        CHECK(!MaiFileSystem::readFile(at(outside, "retained.txt"), bytes));
        CHECK(bytes == "outside unchanged");
    } else {
        std::printf("SKIP symlink fixture: native creation privilege unavailable\n");
    }
}
}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent =
        MaiFileSystem::resolve(MaiFilePath::fromUtf8("./fixtures-" + std::to_string(stamp)));
    CHECK(!MaiFileSystem::createDirectories(parent));
    testEveryPublicationBoundary(parent);
#ifdef __APPLE__
    testAppleReplayInterruptedAgain(parent);
    testApplePreservesSourceEvidence(parent);
#endif
    testCorruptionAndForeignSource(parent);
    testReplacedSourceAndVersion(parent);
    testAccountBoundaryAndActiveWriter(parent);
    testWriterAfterSnapshotCannotPublishOldBody(parent);
    testCancellationAndMalformedReceipt(parent);
    testRestartCanceledPrivateBuild(parent);
    testRestartBeforeReadyBoundaries(parent);
    testRestartRejectsUnownedStage(parent);
    testRestartRejectsChangedIdentitiesAndLinks(parent);
    std::printf("MaiSessionMigrationTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
