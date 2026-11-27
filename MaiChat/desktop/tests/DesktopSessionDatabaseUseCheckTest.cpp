// 自建DB及自有子进程验证普通启动入口；不读取账号目录，不终止用户进程。
#include "agent/DesktopAgentStoragePreparation.h"
#include "agent/DesktopSessionDatabaseUseCheck.h"

#include <QApplication>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <sqlite3.h>
#include "MaiFileSystem.h"

extern char** environ;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}
std::string bytes(const std::string& path) {
    std::string result;
    check(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(path), result), "read owned bytes");
    return result;
}
int writer(const char* path, int ready, int release) {
    sqlite3* db = nullptr;
    int status = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, nullptr);
    if (status == SQLITE_OK) {
        int previous = 0, persist = 1;
        sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &previous);
        sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
        status = sqlite3_exec(db, "SELECT value FROM owned", nullptr, nullptr, nullptr);
    }
    char value = status == SQLITE_OK ? '1' : '0';
    if (::write(ready, &value, 1) != 1) return 3;
    if (::read(release, &value, 1) != 1) return 4;
    return sqlite3_close(db) == SQLITE_OK && status == SQLITE_OK ? 0 : 5;
}
}  // namespace
int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--writer")
        return writer(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
    char directory[] = "./owned-host-db-XXXXXX";
    if (!::mkdtemp(directory)) return 2;
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8(directory));
    const auto path = root.append(MaiFilePath::fromUtf8("agent.db"));
    const std::string filename = path.toUtf8();
    auto seeded = makeMaiSqliteStore(filename);
    check(bool(seeded), "prepare actual persistent owned index");
    if (!seeded) return 6;
    seeded.value().reset();
    sqlite3* db = nullptr;
    check(sqlite3_open(filename.c_str(), &db) == SQLITE_OK, "seed open");
    int persist = 1, previous = 0;
    sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &previous);
    sqlite3_file_control(db, "main", SQLITE_FCNTL_PERSIST_WAL, &persist);
    check(sqlite3_exec(db,
                       "PRAGMA journal_mode=WAL; CREATE TABLE owned(id INTEGER PRIMARY KEY, value "
                       "TEXT); INSERT INTO owned VALUES(1,'kept');",
                       nullptr, nullptr, nullptr) == SQLITE_OK,
          "seed committed body");
    check(sqlite3_close(db) == SQLITE_OK, "seed close");
    int ready[2], release[2];
    if (::pipe(ready) || ::pipe(release)) return 3;
    const auto readyArg = std::to_string(ready[1]), releaseArg = std::to_string(release[0]);
    const pid_t child = ::fork();
    if (child < 0) return 4;
    if (!child) {
        ::close(ready[0]);
        ::close(release[1]);
        ::execl(argv[0], argv[0], "--writer", filename.c_str(), readyArg.c_str(),
                releaseArg.c_str(), nullptr);
        ::_exit(127);
    }
    ::close(ready[1]);
    ::close(release[0]);
    char value = 0;
    check(::read(ready[0], &value, 1) == 1 && value == '1', "external SQLite process ready");
    QApplication app(argc, argv);
    const auto mainBefore = bytes(filename), walBefore = bytes(filename + "-wal");
    const auto shmBefore = bytes(filename + "-shm");
    check(checkDesktopSessionDatabaseUse(path).code() == MaiErrorCode::Busy,
          "idle external DB handle is Busy");
    const auto alias = MaiFilePath::fromUtf8(filename + ".alias");
    check(::link(filename.c_str(), alias.value().c_str()) == 0, "owned hardlink");
    check(checkDesktopSessionDatabaseUse(alias).code() == MaiErrorCode::Busy,
          "same inode alias is Busy");
    const auto missing = root.append(MaiFilePath::fromUtf8("unrelated.db"));
    check(!checkDesktopSessionDatabaseUse(missing), "unrelated account path is not blocked");
    auto blocked = prepareDesktopAgentStorage(QString::fromUtf8(filename.c_str()));
    check(!blocked.store && blocked.error.contains(QStringLiteral("退出")) &&
              blocked.error.contains(QStringLiteral("重试")),
          "ordinary startup refuses and explains retry");
    check(bytes(filename) == mainBefore && bytes(filename + "-wal") == walBefore &&
              bytes(filename + "-shm") == shmBefore,
          "Busy check preserves source and both sidecars");
    const std::string backup = filename + ".legacy-session-backup";
    check(::rename(filename.c_str(), backup.c_str()) == 0, "owned retained main rename");
    check(checkDesktopSessionDatabaseUse(path).code() == MaiErrorCode::Busy,
          "retained inode handle is Busy");
    check(::rename(backup.c_str(), filename.c_str()) == 0, "restore owned main name");
    value = 'x';
    check(::write(release[1], &value, 1) == 1, "release owned writer normally");
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "owned writer exited without termination");
    ::close(ready[0]);
    ::close(release[1]);
    check(!checkDesktopSessionDatabaseUse(path), "writer exit permits retry");
    check(sqlite3_open_v2(filename.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
          "own read handle");
    check(!checkDesktopSessionDatabaseUse(path), "current process handle ignored");
    sqlite3_close(db);
    // 真实默认opener，避免只验证helper而漏掉宿主准备入口。
    const auto retryPath = path;
    auto opened = prepareDesktopAgentStorage(QString::fromUtf8(retryPath.toUtf8().c_str()));
    if (!opened.error.isEmpty())
        std::printf("owned retry error: %s\n", opened.error.toUtf8().constData());
    check(opened.store && opened.error.isEmpty(),
          "ordinary startup continues after external writer exit");
    std::printf("DesktopSessionDatabaseUseCheckTest failures=%d\n", failures);
    return failures ? 1 : 0;
}
