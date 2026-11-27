// 文件作用：用只读 SQLite 在线备份及不可覆盖发布实现一次性迁移。
#include "MaiSessionMigration.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>
#endif
#endif

#include "MaiAppendFile.h"
#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"

namespace {
using Json = nlohmann::json;
struct DatabaseCloser {
    void operator()(sqlite3* database) const {
        if (database) sqlite3_close(database);
    }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

MaiError invalid(const char* message) {
    return {MaiErrorCode::InvalidInput, message};
}

MaiFilePath child(const MaiFilePath& parent, const char* name) {
    return parent.append(MaiFilePath::fromUtf8(name));
}

MaiFilePath suffix(const MaiFilePath& path, const char* value) {
    return MaiFilePath(path.value() + MaiFilePath::fromUtf8(value).value());
}

MaiFilePath stagePath(const MaiFilePath& source) {
    return source.dirName().append(
        MaiFilePath::fromUtf8("." + source.baseName().toUtf8() + ".session-migration"));
}

std::string hexBytes(const unsigned char* data, std::size_t size) {
    const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result += digits[data[index] >> 4];
        result += digits[data[index] & 15];
    }
    return result;
}

// 原生路径按字节编码，不把 POSIX 文件名强制解释为 UTF-8。
std::string pathKey(const MaiFilePath& path) {
    return hexBytes(reinterpret_cast<const unsigned char*>(path.value().data()),
                    path.value().size() * sizeof(MaiFilePath::CharType));
}

bool within(const MaiFilePath& root, const MaiFilePath& path) {
    const auto resolved = MaiFileSystem::resolve(path);
    return !resolved.isEmpty() && root.isParentOf(resolved) && !MaiFileSystem::isSymbolicLink(path);
}

MaiError nativeFailure(const char* operation) {
#ifdef _WIN32
    const auto code = ::GetLastError();
    const auto category = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION
                              ? MaiErrorCode::Busy
                              : MaiErrorCode::Internal;
#else
    const auto code = errno;
    const auto category =
        code == EWOULDBLOCK || code == EAGAIN ? MaiErrorCode::Busy : MaiErrorCode::Internal;
#endif
    return {category, std::string(operation) + ": native " + std::to_string(code)};
}

MaiResult<std::string> fileIdentity(const MaiFilePath& path, bool directory = false) {
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(
        MaiFileSystem::windowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nativeFailure("cannot inspect migration identity");
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = ::GetFileInformationByHandle(handle, &info) &&
                       !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                       ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    ::CloseHandle(handle);
    if (!valid) return invalid("migration identity requires the expected non-link node type");
    return std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) +
           ":" + std::to_string(info.nFileIndexLow);
#else
    struct stat info{};
    if (::lstat(path.value().c_str(), &info) != 0)
        return nativeFailure("cannot inspect migration identity");
    if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))
        return invalid("migration identity requires the expected non-link node type");
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
}

// 仅持有自有句柄。Windows原库允许删除共享；Apple独立SQLite写保留允许只读快照。
// 生命周期覆盖发布及失败返回，析构关闭连接自动回滚未写入的BEGIN并释放写保留。
class MigrationGuard {
public:
    ~MigrationGuard() {
#ifdef _WIN32
        if (mHandle != INVALID_HANDLE_VALUE) ::CloseHandle(mHandle);
#else
#ifdef __APPLE__
        // 先关闭专属SQLite连接，再关闭身份句柄；没有借出的statement或用户正文写入。
        mWriteReservation.reset();
#endif
        if (mHandle >= 0) ::close(mHandle);
#endif
    }

    MaiError open(const MaiFilePath& path, bool lockFile) {
#ifdef _WIN32
        mHandle = ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(),
                                lockFile ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                lockFile ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, lockFile ? OPEN_ALWAYS : OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (mHandle == INVALID_HANDLE_VALUE) return nativeFailure("cannot acquire migration guard");
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileType(mHandle) != FILE_TYPE_DISK ||
            !::GetFileInformationByHandle(mHandle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return invalid("migration guard requires a non-link ordinary file");
#else
        mHandle =
            ::open(path.value().c_str(),
                   (lockFile ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                   S_IRUSR | S_IWUSR);
        if (mHandle < 0) return nativeFailure("cannot open migration guard");
        struct stat info{};
        if (::fstat(mHandle, &info) != 0 || !S_ISREG(info.st_mode))
            return invalid("migration guard requires a non-link ordinary file");
#ifdef __APPLE__
        if (!lockFile) {
            // Darwin的源文件flock会同时阻塞本进程READONLY backup。使用独立连接的
            // RESERVED写锁阻止旧SQLite提交，快照仍走另一个只读连接。这里只BEGIN，
            // 不修改schema或正文；关闭时也不能checkpoint/删WAL改变已冻结证据。
            sqlite3* database = nullptr;
            const int status = sqlite3_open_v2(
                path.toUtf8().c_str(), &database,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr);
            mWriteReservation.reset(database);
            if (status != SQLITE_OK || sqlite3_db_readonly(database, "main") != 0)
                return {status == SQLITE_BUSY || status == SQLITE_LOCKED ? MaiErrorCode::Busy
                                                                         : MaiErrorCode::Internal,
                        "cannot open writable legacy database for migration exclusion"};
            int previous = 0;
            int persist = 1;
            if (sqlite3_db_config(database, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, &previous) !=
                    SQLITE_OK ||
                sqlite3_file_control(database, "main", SQLITE_FCNTL_PERSIST_WAL, &persist) !=
                    SQLITE_OK)
                return {MaiErrorCode::Internal,
                        "cannot preserve legacy WAL while acquiring migration exclusion"};
            sqlite3_busy_timeout(database, 0);
            const int reserved =
                sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
            if (reserved != SQLITE_OK)
                return {reserved == SQLITE_BUSY || reserved == SQLITE_LOCKED
                            ? MaiErrorCode::Busy
                            : MaiErrorCode::Internal,
                        "legacy session database is busy; close the old app and retry"};
            return {};
        }
#endif
        // 发布锁文件继续用flock；非Apple源文件保留原协作锁，宿主仍须排除旧版写者。
        if (::flock(mHandle, LOCK_EX | LOCK_NB) != 0)
            return nativeFailure("cannot acquire migration guard");
#endif
        return {};
    }

    MaiResult<std::string> identity() const {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(mHandle, &info))
            return nativeFailure("cannot inspect migration guard");
        return std::to_string(info.dwVolumeSerialNumber) + ":" +
               std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
#else
        struct stat info{};
        if (::fstat(mHandle, &info) != 0) return nativeFailure("cannot inspect migration guard");
        return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
    }

private:
#ifdef _WIN32
    HANDLE mHandle = INVALID_HANDLE_VALUE;
#else
    int mHandle = -1;
#ifdef __APPLE__
    Database mWriteReservation;
#endif
#endif
};

MaiError syncDirectory(const MaiFilePath& path) {
#ifdef _WIN32
    // Win32 没有可移植的目录 fsync；移动使用 WRITE_THROUGH，契约限于进程中断恢复。
    (void)path;
    return {};
#else
    const int descriptor =
        ::open(path.value().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return nativeFailure("cannot open migration directory for sync");
    const int status = ::fsync(descriptor);
    const int saved = errno;
    ::close(descriptor);
    errno = saved;
    if (status != 0) return nativeFailure("cannot sync migration directory");
    return {};
#endif
}

MaiError createStageDirectory(const MaiFilePath& root, const MaiFilePath& stage) {
    if (!within(root, stage)) return invalid("migration staging escaped the account root");
    // 不采用已存在也算成功的递归建目录接口，避免把并发出现的外来目录标记为自有。
#ifdef _WIN32
    if (!::CreateDirectoryW(MaiFileSystem::windowsApiPath(stage).c_str(), nullptr))
        return nativeFailure("cannot exclusively create migration staging directory");
#else
    if (::mkdir(stage.value().c_str(), S_IRWXU) != 0)
        return nativeFailure("cannot exclusively create migration staging directory");
#endif
    return syncDirectory(stage.dirName());
}

// 同文件系统无覆盖重命名；不支持原生排他目录移动的平台明确拒绝。
MaiError renameNew(const MaiFilePath& root, const MaiFilePath& source,
                   const MaiFilePath& destination) {
    if (!within(root, source) || !within(root, destination))
        return invalid("migration rename escaped the storage root");
#ifdef _WIN32
    if (!::MoveFileExW(MaiFileSystem::windowsApiPath(source).c_str(),
                       MaiFileSystem::windowsApiPath(destination).c_str(), MOVEFILE_WRITE_THROUGH))
        return nativeFailure("cannot publish migration path without replacement");
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.value().c_str(), AT_FDCWD,
                  destination.value().c_str(), 1u) != 0) {
        if (errno == ENOSYS || errno == EINVAL)
            return {MaiErrorCode::NotSupported, "exclusive migration rename is unavailable"};
        return nativeFailure("cannot publish migration path without replacement");
    }
#elif defined(__APPLE__)
    if (::renamex_np(source.value().c_str(), destination.value().c_str(), RENAME_EXCL) != 0)
        return nativeFailure("cannot publish migration path without replacement");
#else
    return {MaiErrorCode::NotSupported, "exclusive migration rename is unavailable"};
#endif
    auto error = syncDirectory(destination.dirName());
    if (!error && source.dirName() != destination.dirName())
        error = syncDirectory(source.dirName());
    return error;
}

MaiResult<Json> fingerprint(const MaiFilePath& root, const MaiFilePath& path) {
    if (!within(root, path)) return invalid("migration file escaped the storage root");
    auto file = MaiFileReadHandle::open(path);
    if (!file) return file.error();
    auto before = file.value()->snapshot();
    if (!before) return before.error();
    auto identity = fileIdentity(path);
    if (!identity) return identity.error();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        return {MaiErrorCode::Internal, "cannot initialize migration checksum"};
    std::uint64_t offset = 0;
    bool eof = false;
    while (!eof) {
        std::string bytes;
        auto error = file.value()->readAt(offset, 1024 * 1024, bytes, &eof);
        if (error) return error;
        if (EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1)
            return {MaiErrorCode::Internal, "cannot calculate migration checksum"};
        offset += bytes.size();
    }
    auto error = file.value()->validate(before.value());
    if (error) return error;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &length) != 1)
        return {MaiErrorCode::Internal, "cannot finish migration checksum"};
    return Json{
        {"sha256", hexBytes(digest, length)}, {"bytes", offset}, {"file_id", identity.value()}};
}

MaiResult<Json> inventory(const MaiFilePath& root, const MaiFilePath& directory) {
    if (!within(root, directory) || !MaiFileSystem::isDirectory(directory))
        return invalid("migration sessions tree is missing or outside its account");
    Json entries = Json::object();
    MaiError failure;
    MaiFileSystem::walk(
        directory,
        [&](const MaiFileEntry& entry) {
            if (entries.size() >= 100000) {
                failure = invalid("migration sessions inventory exceeds the metadata entry limit");
                return MaiWalkAction::Stop;
            }
            if (!within(root, entry.path) || MaiFileSystem::isSymbolicLink(entry.path)) {
                failure = invalid("migration sessions tree contains an unsafe path");
                return MaiWalkAction::Stop;
            }
            const auto relative = entry.path.value().substr(directory.value().size());
            const auto key = pathKey(MaiFilePath(relative));
            if (entry.isDirectory) {
                entries[key] = Json{{"directory", true}};
            } else {
                auto value = fingerprint(root, entry.path);
                if (!value) {
                    failure = value.error();
                    return MaiWalkAction::Stop;
                }
                entries[key] = std::move(value.value());
            }
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError& error) { failure = error; });
    if (failure) return failure;
    return entries;
}

MaiError flushFile(const MaiFilePath& root, const MaiFilePath& path) {
    if (!within(root, path)) return invalid("migration flush escaped the storage root");
    MaiRegularFileInfo information;
    if (!MaiFileSystem::regularFileInfo(path, information))
        return invalid("migration flush requires an existing ordinary file");
    auto file = MaiAppendFile::open(path);
    if (!file) return file.error();
    auto error = file.value()->lockWriter();
    if (!error) error = file.value()->flush();
    return error;
}

MaiError flushTree(const MaiFilePath& root, const MaiFilePath& directory) {
    MaiError failure;
    std::vector<MaiFilePath> directories{directory};
    MaiFileSystem::walk(
        directory,
        [&](const MaiFileEntry& entry) {
            if (!within(root, entry.path)) {
                failure = invalid("migration flush tree escaped the storage root");
                return MaiWalkAction::Stop;
            }
            if (entry.isDirectory)
                directories.push_back(entry.path);
            else
                failure = flushFile(root, entry.path);
            return failure ? MaiWalkAction::Stop : MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError& error) { failure = error; });
    if (failure) return failure;
    for (auto iterator = directories.rbegin(); iterator != directories.rend(); ++iterator) {
        failure = syncDirectory(*iterator);
        if (failure) return failure;
    }
    return {};
}

MaiError writeReceipt(const MaiFilePath& root, const MaiFilePath& path, const Json& receipt) {
    if (!within(root, path)) return invalid("migration receipt escaped the storage root");
    const std::string bytes = receipt.dump() + "\n";
    if (bytes.size() > 64 * 1024 * 1024)
        return invalid("migration receipt exceeds the bounded metadata limit");
    const auto pending = suffix(path, ".pending");
    if (!within(root, pending)) return invalid("migration receipt escaped the storage root");
    auto error = MaiFileSystem::createEmptyFile(pending);
    if (error) return error;
    {
        auto file = MaiAppendFile::open(pending);
        if (!file) return file.error();
        error = file.value()->lockWriter();
        if (error) return error;
        auto appended = file.value()->append(bytes);
        if (!appended) return appended.error();
        error = file.value()->flush();
        if (error) return error;
    }
    return renameNew(root, pending, path);
}

MaiResult<Json> readReceipt(const MaiFilePath& root, const MaiFilePath& path) {
    if (!within(root, path)) return invalid("migration receipt escaped the storage root");
    std::string bytes;
    bool truncated = false;
    auto error = MaiFileSystem::readFile(path, bytes, 64 * 1024 * 1024, &truncated);
    if (error) return error;
    if (truncated) return invalid("migration receipt exceeds the bounded metadata limit");
    auto parsed = Json::parse(bytes, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return invalid("corrupt migration receipt");
    return parsed;
}

// 仅验证将被完整保留的目录节点，不重读半成品正文；中止时不删除、不截断任何字节。
MaiError validateBuildStage(const MaiSessionMigrationOptions& options, const MaiFilePath& root,
                            const MaiFilePath& stage) {
    std::size_t nodes = 0;
    MaiError failure;
    MaiFileSystem::walk(
        stage,
        [&](const MaiFileEntry& entry) {
            if (options.isCanceled && options.isCanceled()) {
                failure = {MaiErrorCode::Canceled,
                           "session migration canceled during staging inspection"};
                return MaiWalkAction::Stop;
            }
            const auto resolved = MaiFileSystem::resolve(entry.path);
            if (++nodes > 100000 || !within(root, entry.path) || !stage.isParentOf(resolved)) {
                failure = invalid(
                    "incomplete migration staging contains an unsafe or excessive inventory");
                return MaiWalkAction::Stop;
            }
            const auto info = MaiFileSystem::stat(entry.path);
            if (!info) {
                failure = info.error();
                return MaiWalkAction::Stop;
            }
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError& error) { failure = error; });
    return failure;
}

MaiError archiveIncompleteBuild(const MaiSessionMigrationOptions& options, const MaiFilePath& root,
                                const MaiFilePath& source, const MaiFilePath& stage,
                                const std::string& sourceIdentity) {
    if (!options.restartIncompleteBuild)
        return invalid("incomplete migration staging requires inspection or explicit restart");
    const auto marker = readReceipt(root, child(stage, "building.json"));
    if (!marker) return invalid("incomplete migration staging has no valid ownership marker");
    const auto stageIdentity = fileIdentity(stage, true);
    if (!stageIdentity) return stageIdentity.error();
    const Json expected{{"format", 1},
                        {"phase", "building"},
                        {"root", pathKey(root)},
                        {"source", pathKey(source)},
                        {"stage", pathKey(stage)},
                        {"source_identity", sourceIdentity},
                        {"stage_identity", stageIdentity.value()}};
    if (marker.value() != expected)
        return invalid("incomplete migration staging ownership or source identity changed");
    auto error = validateBuildStage(options, root, stage);
    if (error) return error;
    const auto currentSource = fileIdentity(source);
    if (!currentSource || currentSource.value() != sourceIdentity)
        return invalid("legacy source changed before incomplete staging archival");
    unsigned char bytes[16]{};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return {MaiErrorCode::Internal, "cannot generate private migration archive identity"};
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 0x80);
    const auto hexadecimal = hexBytes(bytes, sizeof(bytes));
    const auto uuid = hexadecimal.substr(0, 8) + "-" + hexadecimal.substr(8, 4) + "-" +
                      hexadecimal.substr(12, 4) + "-" + hexadecimal.substr(16, 4) + "-" +
                      hexadecimal.substr(20, 12);
    // 固定前缀和本地 UUID，不接受清单提供的移动目标，也不复用已有归档目录。
    const auto archive =
        child(source.dirName(), (".mai-session-migration.abandoned-" + uuid).c_str());
    if (!within(root, stage) || !within(root, archive) || stage == archive)
        return invalid("incomplete migration archive escaped the account root");
    const auto finalStageIdentity = fileIdentity(stage, true);
    if (!finalStageIdentity || finalStageIdentity.value() != stageIdentity.value())
        return invalid("incomplete migration staging changed before archival");
    if (options.isCanceled && options.isCanceled())
        return {MaiErrorCode::Canceled, "session migration canceled before staging archival"};
    return renameNew(root, stage, archive);
}

MaiResult<Database> openDatabase(const MaiFilePath& path, bool writable) {
    sqlite3* database = nullptr;
    const int flags =
        (writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) | SQLITE_OPEN_NOMUTEX;
    const auto status = sqlite3_open_v2(path.toUtf8().c_str(), &database, flags, nullptr);
    Database owner(database);
    if (status != SQLITE_OK)
        return {status == SQLITE_BUSY || status == SQLITE_LOCKED ? MaiErrorCode::Busy
                                                                 : MaiErrorCode::Internal,
                "cannot open migration SQLite connection"};
    if (!writable && sqlite3_db_readonly(database, "main") != 1)
        return invalid("migration snapshot connection is not read-only");
    return std::move(owner);
}

MaiError snapshot(const MaiSessionMigrationOptions& options, const MaiFilePath& root,
                  const MaiFilePath& source, const MaiFilePath& destination) {
    if (!within(root, source) || !within(root, destination))
        return invalid("migration snapshot escaped the storage root");
    auto error = MaiFileSystem::createEmptyFile(destination);
    if (error) return error;
    auto input = openDatabase(source, false);
    if (!input) return input.error();
    auto output = openDatabase(destination, true);
    if (!output) return output.error();
    sqlite3_backup* backup =
        sqlite3_backup_init(output.value().get(), "main", input.value().get(), "main");
    if (!backup) return {MaiErrorCode::Internal, "cannot initialize read-only legacy snapshot"};
    int status = SQLITE_OK;
    // 不重试忙碌写者，也不无限等待；每步只复制 128 页并检查取消。
    while (status == SQLITE_OK) {
        if (options.isCanceled && options.isCanceled()) {
            sqlite3_backup_finish(backup);
            return {MaiErrorCode::Canceled, "session migration canceled during snapshot"};
        }
        status = sqlite3_backup_step(backup, 128);
    }
    const int finished = sqlite3_backup_finish(backup);
    if (status != SQLITE_DONE || finished != SQLITE_OK)
        return {status == SQLITE_BUSY || status == SQLITE_LOCKED ? MaiErrorCode::Busy
                                                                 : MaiErrorCode::Internal,
                "cannot complete read-only legacy snapshot"};
    output.value().reset();
    input.value().reset();
    return flushFile(root, destination);
}

MaiError canceled(const MaiSessionMigrationOptions& options) {
    if (options.isCanceled && options.isCanceled())
        return {MaiErrorCode::Canceled,
                "session migration canceled; retained files permit recovery"};
    return {};
}

MaiError checkpoint(const MaiSessionMigrationOptions& options, const char* step) {
    if (options.checkpoint) return options.checkpoint(step);
    return {};
}

MaiError matches(const MaiFilePath& root, const MaiFilePath& path, const Json& expected) {
    auto actual = fingerprint(root, path);
    if (!actual) return actual.error();
    if (actual.value() != expected) return invalid("migration file checksum changed");
    return {};
}

MaiError matchesTree(const MaiFilePath& root, const MaiFilePath& path, const Json& expected) {
    auto actual = inventory(root, path);
    if (!actual) return actual.error();
    if (actual.value() != expected) return invalid("migration sessions inventory changed");
    return {};
}

bool validReceipt(const Json& receipt, const MaiFilePath& root, const MaiFilePath& source) {
    return receipt.contains("format") && receipt["format"] == 1 && receipt.contains("root") &&
           receipt["root"] == pathKey(root) && receipt.contains("source") &&
           receipt["source"] == pathKey(source) && receipt.contains("source_identity") &&
           receipt["source_identity"].is_string() && receipt.contains("source_files") &&
           receipt["source_files"].is_object() && receipt["source_files"].contains("") &&
           receipt.contains("index") && receipt["index"].is_object() &&
           receipt.contains("sessions") && receipt["sessions"].is_object() &&
           receipt.contains("snapshot") && receipt["snapshot"].is_object();
}

}  // namespace

bool MaiSessionMigration::hasPending(const MaiFilePath& storageRoot,
                                     const MaiFilePath& sourceIndexPath) {
    const auto root = MaiFileSystem::resolve(storageRoot);
    const auto source = MaiFileSystem::resolve(sourceIndexPath);
    if (root.isEmpty() || source.isEmpty() || !root.isParentOf(source)) return true;
    const auto stage = stagePath(source);
    if (!MaiFileSystem::exists(stage)) return false;
    if (!within(root, stage)) return true;
    auto complete = readReceipt(root, child(stage, "complete.json"));
    return !complete ||
           complete.value() !=
               Json{{"format", 1}, {"root", pathKey(root)}, {"source", pathKey(source)}};
}

MaiError MaiSessionMigration::run(const MaiSessionMigrationOptions& options) {
    maiAssertBlockingAllowed("MaiSessionMigration::run");
    const auto root = MaiFileSystem::resolve(options.storageRoot);
    const auto source = MaiFileSystem::resolve(options.sourceIndexPath);
    if (root.isEmpty() || source.isEmpty() || !MaiFileSystem::isDirectory(root) ||
        !root.isParentOf(source) || !options.build || !options.validate ||
        MaiFileSystem::isSymbolicLink(options.sourceIndexPath))
        return invalid("migration requires explicit account paths and build/validate callbacks");
    // 同一账户内的 index 同级暂存和固定备份，跨账户移动从不被清单授权。
    const auto stage = stagePath(source);
    const auto retained = suffix(source, ".legacy-session-backup");
    const auto stagedIndex = child(stage, "index.db");
    const auto snapshotPath = child(stage, "legacy-snapshot.db");
    const auto stagedSessions = child(stage, "sessions");
    const auto finalSessions = child(source.dirName(), "sessions");
    const auto receiptPath = child(stage, "ready.json");
    const auto buildingPath = child(stage, "building.json");
    const auto completePath = child(stage, "complete.json");
    const auto lockPath = suffix(source, ".session-migration.lock");
    for (const auto& path : {stage, retained, stagedIndex, snapshotPath, stagedSessions,
                             finalSessions, receiptPath, buildingPath, completePath, lockPath}) {
        if (!within(root, path)) return invalid("migration paths escaped the account root");
    }
    MigrationGuard publisherGuard;
    auto error = publisherGuard.open(lockPath, true);
    if (error) return error;
    if (MaiFileSystem::exists(completePath)) {
        auto complete = readReceipt(root, completePath);
        if (!complete ||
            complete.value() !=
                Json{{"format", 1}, {"root", pathKey(root)}, {"source", pathKey(source)}})
            return invalid("migration completion receipt is invalid");
        return {};
    }
    error = canceled(options);
    if (error) return error;
    int restoredPreservationSteps = 0;
#ifdef __APPLE__
    // SQLite按主文件名寻找WAL/SHM。中断若落在保全主库与保全sidecar之间，
    // 不能直接打开retained，否则会创建另一组空sidecar。先按ready中的原证据
    // 校验每个节点，再无覆盖地逆序还原已完成的保全步骤；不改DB正文。
    // 每次rename后的中断仍是原发布协议可识别的前缀状态，下一次可继续还原。
    // 这段只由发布锁串行化，不声称排斥此刻新启动的旧进程；宿主先检查外部FD，
    // 还原后立即取得SQLite写保留，后续仍复核冻结指纹。
    if (MaiFileSystem::exists(retained) && MaiFileSystem::exists(stagedIndex)) {
        auto saved = readReceipt(root, receiptPath);
        if (!saved || !validReceipt(saved.value(), root, source))
            return invalid("cannot restore partial migration without a valid receipt");
        error = matches(root, stagedIndex, saved.value()["index"]);
        if (!error) error = matches(root, snapshotPath, saved.value()["snapshot"]);
        if (!error) error = matchesTree(root, finalSessions, saved.value()["sessions"]);
        if (error) return error;
        const auto& files = saved.value()["source_files"];
        for (const char* extension : {"", "-wal", "-shm"}) {
            const auto originalPath = suffix(source, extension);
            const auto backupPath = suffix(retained, extension);
            const bool original = MaiFileSystem::exists(originalPath);
            const bool backup = MaiFileSystem::exists(backupPath);
            if (!files.contains(extension)) {
                if (original || backup) return invalid("unexpected sidecar in partial migration");
            } else {
                if (original == backup) return invalid("ambiguous source in partial migration");
                error = matches(root, backup ? backupPath : originalPath, files[extension]);
                if (error) return error;
            }
        }
        // 仅回退sidecar尚未全部保全的状态；完整备份可按retained名直接打开。
        const bool partial =
            (files.contains("-wal") && !MaiFileSystem::exists(suffix(retained, "-wal"))) ||
            (files.contains("-shm") && !MaiFileSystem::exists(suffix(retained, "-shm")));
        if (partial) {
            int preserved = 0;
            for (const char* extension : {"", "-wal", "-shm"})
                if (MaiFileSystem::exists(suffix(retained, extension))) ++preserved;
            const auto replayPath = child(stage, "apple-replay.json");
            if (MaiFileSystem::exists(replayPath)) {
                auto replay = readReceipt(root, replayPath);
                if (!replay || !replay.value().contains("receipt") ||
                    replay.value()["receipt"] != saved.value() ||
                    !replay.value().contains("steps") ||
                    !replay.value()["steps"].is_number_integer() ||
                    replay.value()["steps"] < preserved || replay.value()["steps"] > 3)
                    return invalid("invalid Apple migration replay boundary");
            } else {
                // 单独无覆盖落盘，不替换原ready收据。回退再次中断仍记得已通知边界。
                error = writeReceipt(root, replayPath,
                                     Json{{"receipt", saved.value()}, {"steps", preserved}});
                if (error) return error;
            }
        }
        if (partial)
            for (const char* extension : {"-shm", "-wal", ""}) {
                const auto backupPath = suffix(retained, extension);
                if (!MaiFileSystem::exists(backupPath)) continue;
                error = renameNew(root, backupPath, suffix(source, extension));
                if (error) return error;
                error = checkpoint(options, std::string(extension) == "-shm" ? "apple_shm_restored"
                                            : std::string(extension) == "-wal"
                                                ? "apple_wal_restored"
                                                : "apple_source_restored");
                if (error) return error;
            }
    }
#endif
    MigrationGuard sourceGuard;
    error = sourceGuard.open(MaiFileSystem::exists(retained) ? retained : source, false);
    if (error) return error;
    const auto identity = sourceGuard.identity();
    if (!identity) return identity.error();
    Json receipt;
    if (MaiFileSystem::exists(receiptPath)) {
        auto parsed = readReceipt(root, receiptPath);
        if (!parsed) return parsed.error();
        receipt = std::move(parsed.value());
#ifdef __APPLE__
        const auto replayPath = child(stage, "apple-replay.json");
        if (MaiFileSystem::exists(replayPath)) {
            auto replay = readReceipt(root, replayPath);
            if (!replay || !replay.value().contains("receipt") ||
                replay.value()["receipt"] != receipt || !replay.value().contains("steps") ||
                !replay.value()["steps"].is_number_integer() || replay.value()["steps"] < 0 ||
                replay.value()["steps"] > 3)
                return invalid("invalid Apple migration replay boundary");
            restoredPreservationSteps = replay.value()["steps"].get<int>();
        }
#endif
        if (!validReceipt(receipt, root, source) ||
            receipt["source_identity"].get<std::string>() != identity.value())
            return invalid("migration source identity or receipt changed");
    } else {
        if (MaiFileSystem::exists(retained) || MaiFileSystem::exists(finalSessions))
            return invalid("incomplete or foreign migration staging requires inspection");
        if (MaiFileSystem::exists(stage)) {
            error = archiveIncompleteBuild(options, root, source, stage, identity.value());
            if (error) return error;
        }
        error = canceled(options);
        if (error) return error;
        error = createStageDirectory(root, stage);
        if (error) return error;
        const auto stageIdentity = fileIdentity(stage, true);
        if (!stageIdentity) return stageIdentity.error();
        // 在第一个快照或正文文件创建之前记录所有权，取消后的重建只认领这个节点。
        error = writeReceipt(root, buildingPath,
                             Json{{"format", 1},
                                  {"phase", "building"},
                                  {"root", pathKey(root)},
                                  {"source", pathKey(source)},
                                  {"stage", pathKey(stage)},
                                  {"source_identity", identity.value()},
                                  {"stage_identity", stageIdentity.value()}});
        if (!error) error = checkpoint(options, "building");
        if (error) return error;
        // 快照前冻结权威源字节，不能在耗时构建后采集新值并误认其已包含在快照中。
        // SHM 只保存可变读者索引；只读 SQLite 连接也可修改它，不作为正文快照证据。
        Json sourceFiles = Json::object();
        for (const char* extension : {"", "-wal"}) {
            const auto path = suffix(source, extension);
            if (!MaiFileSystem::exists(path)) continue;
            auto saved = fingerprint(root, path);
            if (!saved) return saved.error();
            if (std::string(extension).empty() && saved.value()["file_id"] != identity.value())
                return invalid("legacy source was replaced before migration snapshot");
            sourceFiles[extension] = std::move(saved.value());
        }
        const auto verifySnapshotSource = [&]() -> MaiError {
            for (const char* extension : {"", "-wal"}) {
                const auto path = suffix(source, extension);
                const bool present = MaiFileSystem::exists(path);
                if (present != sourceFiles.contains(extension))
                    return invalid("legacy source file set changed after snapshot evidence");
                if (present) {
                    auto changed = matches(root, path, sourceFiles[extension]);
                    if (changed) return changed;
                }
            }
            return {};
        };
        error = snapshot(options, root, source, snapshotPath);
        if (!error) error = verifySnapshotSource();
        if (error) return error;
        error = checkpoint(options, "snapshot_created");
        if (!error) error = canceled(options);
        if (error) return error;
        auto reader = openDatabase(snapshotPath, false);
        if (!reader) return reader.error();
        error = options.build(reader.value().get(), stage, stagedIndex);
        if (!error) error = canceled(options);
        if (!error) error = checkpoint(options, "build_complete");
        if (!error) error = canceled(options);
        if (!error) error = options.validate(reader.value().get(), stage, stagedIndex);
        reader.value().reset();
        if (error) return error;
        error = canceled(options);
        if (error) return error;
        error = flushTree(root, stagedSessions);
        if (!error) error = flushFile(root, stagedIndex);
        if (error) return error;
        // 索引 writer 必须已关闭且完成 checkpoint，不能把 WAL 内容遗漏在发布之外。
        if (MaiFileSystem::exists(suffix(stagedIndex, "-wal")) ||
            MaiFileSystem::exists(suffix(stagedIndex, "-shm")) ||
            MaiFileSystem::exists(suffix(stagedIndex, "-journal")))
            return invalid("staged index still has SQLite sidecars");
        auto tree = inventory(root, stagedSessions);
        if (!tree) return tree.error();
        auto index = fingerprint(root, stagedIndex);
        if (!index) return index.error();
        auto savedSnapshot = fingerprint(root, snapshotPath);
        if (!savedSnapshot) return savedSnapshot.error();
        error = verifySnapshotSource();
        if (error) return error;
        // SHM 仅在这里记录当前保全字节，绝不覆盖快照前冻结的主文件和 WAL 证据。
        const auto sharedMemory = suffix(source, "-shm");
        if (MaiFileSystem::exists(sharedMemory)) {
            auto saved = fingerprint(root, sharedMemory);
            if (!saved) return saved.error();
            sourceFiles["-shm"] = std::move(saved.value());
        }
        receipt = Json{{"format", 1},
                       {"root", pathKey(root)},
                       {"source", pathKey(source)},
                       {"source_identity", identity.value()},
                       {"source_files", sourceFiles},
                       {"index", index.value()},
                       {"snapshot", savedSnapshot.value()},
                       {"sessions", tree.value()}};
        error = writeReceipt(root, receiptPath, receipt);
        if (error) return error;
        error = checkpoint(options, "ready");
        if (error) return error;
    }
    error = matches(root, snapshotPath, receipt["snapshot"]);
    if (error) return error;
    const bool logsAlreadyPublished = !MaiFileSystem::exists(stagedSessions);
    const bool originalAlreadyPreserved = MaiFileSystem::exists(retained);
    const bool indexAlreadyPublished = !MaiFileSystem::exists(stagedIndex);
    if ((originalAlreadyPreserved && !logsAlreadyPublished) ||
        (indexAlreadyPublished && !originalAlreadyPreserved))
        return invalid("migration publication paths have an invalid ordering");
    const auto candidateRoot = logsAlreadyPublished ? source.dirName() : stage;
    const auto candidateIndex = indexAlreadyPublished ? source : stagedIndex;
    error = matchesTree(root, logsAlreadyPublished ? finalSessions : stagedSessions,
                        receipt["sessions"]);
    if (!error) error = matches(root, candidateIndex, receipt["index"]);
    if (error) return error;
    // 先复核所有源文件，原库已移走后绝不覆盖 gap 中新出现的 agent.db。
    for (const char* extension : {"", "-wal", "-shm"}) {
        const auto originalPath = suffix(source, extension);
        const auto backupPath = suffix(retained, extension);
        const bool wasPresent = receipt["source_files"].contains(extension);
        const bool preserved = MaiFileSystem::exists(backupPath);
        if (!wasPresent) {
            if (MaiFileSystem::exists(originalPath) || preserved)
                return invalid("unexpected SQLite sidecar appeared during migration");
            continue;
        }
        if (preserved && MaiFileSystem::exists(originalPath) &&
            !(std::string(extension).empty() && indexAlreadyPublished))
            return invalid("foreign source path appeared during migration publication");
        error = matches(root, preserved ? backupPath : originalPath,
                        receipt["source_files"][extension]);
        if (error) return error;
    }
    {
        auto reader = openDatabase(snapshotPath, false);
        if (!reader) return reader.error();
        error = options.validate(reader.value().get(), candidateRoot, candidateIndex);
        reader.value().reset();
        if (error) return error;
    }
    error = canceled(options);
    if (error) return error;
    if (!logsAlreadyPublished) {
        error = renameNew(root, stagedSessions, finalSessions);
        if (!error) error = checkpoint(options, "logs_published");
        if (error) return error;
    }
    const char* steps[] = {"source_preserved", "wal_preserved", "shm_preserved"};
    int step = 0;
    for (const char* extension : {"", "-wal", "-shm"}) {
        const auto originalPath = suffix(source, extension);
        const auto backupPath = suffix(retained, extension);
        if (receipt["source_files"].contains(extension) && !MaiFileSystem::exists(backupPath)) {
            error = canceled(options);
            if (!error) error = matches(root, originalPath, receipt["source_files"][extension]);
            if (!error) error = renameNew(root, originalPath, backupPath);
            // Apple恢复只重放文件移动，不重复已对外完成的故障/进度边界。
            if (!error && step >= restoredPreservationSteps)
                error = checkpoint(options, steps[step]);
            if (error) return error;
        }
        ++step;
    }
    if (!indexAlreadyPublished) {
        error = canceled(options);
        if (!error) error = matches(root, stagedIndex, receipt["index"]);
        if (!error) error = matchesTree(root, finalSessions, receipt["sessions"]);
        if (!error) error = renameNew(root, stagedIndex, source);
        if (!error) error = checkpoint(options, "index_published");
        if (error) return error;
    }
    error = writeReceipt(root, completePath,
                         Json{{"format", 1}, {"root", pathKey(root)}, {"source", pathKey(source)}});
    if (!error) error = checkpoint(options, "complete");
    return error;
}
