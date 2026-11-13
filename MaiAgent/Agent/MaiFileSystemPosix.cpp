// 文件作用：实现 POSIX 平台的文件系统操作。
// 主要实现：exists、isDirectory、isSymbolicLink；对应公开接口中的状态和错误处理。
#include <pthread.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <atomic>
#endif
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include "MaiAndroidFileOperations.h"
#endif
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiBlockingCheck.h"
#include "MaiThread.h"

// POSIX 实现。
//
// **这份没有在真机上跑过**——本项目目前只在 Windows 上构建。写得尽量守规矩（EINTR 重试、O_CLOEXEC、
// 不跟进符号链接），但第一次在 Linux/mac 上跑之前，不要假设它是对的。
// MaiFileSystemTests 是可移植的，到那边先跑那一套。
//
// 路径在 POSIX 上就是字节序列，**不保证是合法 UTF-8**。所以这里 fromUtf8 / toUtf8 都是原样搬运：
// 擅自做编码转换反而会改写文件名。这正是内部不统一用 UTF-8 的理由（见 MaiFilePath.h）。

namespace {

#if defined(__ANDROID__)
std::atomic<MaiAndroidPublishNewFileCallback> sAndroidPublishNewFile{nullptr};
#endif

MaiErrorCode toErrorCode(int savedErrno) {
    switch (savedErrno) {
        case ENOENT:
        case ENOTDIR: return MaiErrorCode::NotFound;
        case EACCES:
        case EPERM:
        case EISDIR: return MaiErrorCode::InvalidInput;
        default: return MaiErrorCode::Internal;
    }
}

MaiError errnoAs(const char* what) {
    const int saved = errno;
    return MaiError::make(toErrorCode(saved), std::string(what) + ": " + std::strerror(saved));
}

bool statOf(const MaiFilePath& path, struct stat& out) {
    if (path.isEmpty()) return false;
    return ::stat(path.value().c_str(), &out) == 0;
}

}  // namespace

#if defined(__ANDROID__)
void maiSetAndroidPublishNewFileCallback(MaiAndroidPublishNewFileCallback callback) {
    sAndroidPublishNewFile.store(callback, std::memory_order_release);
}

int maiAndroidPublishNewFile(const char* source, const char* destination) {
    const auto callback = sAndroidPublishNewFile.load(std::memory_order_acquire);
    return callback != nullptr ? callback(source, destination) : ENOSYS;
}
#endif

// ── MaiFilePath 的边界转换 ──────────────────────────────────────
// POSIX 上文件名就是字节，原样进出。

MaiFilePath MaiFilePath::fromUtf8(const std::string& utf8) {
    return MaiFilePath(utf8);
}

std::string MaiFilePath::toUtf8() const {
    return mValue;
}

std::string MaiFilePath::toGenericUtf8() const {
    return mValue;  // 分隔符本来就是 '/'
}

// ── MaiFileSystem ───────────────────────────────────────────────

bool MaiFileSystem::exists(const MaiFilePath& path) {
    struct stat info{};
    return statOf(path, info);
}

bool MaiFileSystem::isDirectory(const MaiFilePath& path) {
    struct stat info{};
    if (!statOf(path, info)) return false;
    return S_ISDIR(info.st_mode);
}

bool MaiFileSystem::isSymbolicLink(const MaiFilePath& path) {
    if (path.isEmpty()) return false;
    struct stat info{};
    return ::lstat(path.value().c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

bool MaiFileSystem::fileSize(const MaiFilePath& path, std::uint64_t& size) {
    struct stat info{};
    if (!statOf(path, info)) return false;
    if (S_ISDIR(info.st_mode)) return false;
    size = static_cast<std::uint64_t>(info.st_size);
    return true;
}

bool MaiFileSystem::modifiedTime(const MaiFilePath& path, std::int64_t& seconds) {
    if (path.isEmpty()) return false;
    struct stat info{};
    if (::lstat(path.value().c_str(), &info) != 0) return false;
    seconds = static_cast<std::int64_t>(info.st_mtime);
    return true;
}

namespace {
MaiRegularFileInfo readStatInfo(const struct stat& info) {
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    MaiRegularFileInfo result;
    result.bytes = static_cast<std::uint64_t>(info.st_size);
    result.modified = static_cast<std::int64_t>(info.st_mtime);
    result.identity = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
                      std::to_string(modified.tv_sec) + ":" + std::to_string(modified.tv_nsec) +
                      ":" + std::to_string(changed.tv_sec) + ":" + std::to_string(changed.tv_nsec);
    return result;
}
}  // namespace

bool MaiFileSystem::regularFileInfo(const MaiFilePath& path, MaiRegularFileInfo& out) {
    struct stat info{};
    if (path.isEmpty() || ::lstat(path.value().c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    out = readStatInfo(info);
    return true;
}

MaiFileReadHandle::MaiFileReadHandle(std::intptr_t nativeHandle, MaiFilePath path)
    : mNativeHandle(nativeHandle), mPath(std::move(path)) {
    mStats.openOperations = 1;
}

MaiFileReadHandle::~MaiFileReadHandle() {
    if (mNativeHandle != -1) ::close(static_cast<int>(mNativeHandle));
}

MaiResult<std::unique_ptr<MaiFileReadHandle>> MaiFileReadHandle::open(const MaiFilePath& path) {
    if (path.isEmpty()) return {MaiErrorCode::InvalidInput, "empty file path"};
    maiAssertBlockingAllowed("MaiFileReadHandle::open");
    // 非阻塞打开先守住可能被替换成 FIFO 的路径，再通过 fstat 确认普通文件。
    const int descriptor =
        ::open(path.value().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return errnoAs("cannot open file for reading");
    struct stat info{};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(descriptor);
        return {MaiErrorCode::InvalidInput, "reading requires a non-link regular file"};
    }
    return std::unique_ptr<MaiFileReadHandle>(new MaiFileReadHandle(descriptor, path));
}

MaiResult<MaiRegularFileInfo> MaiFileReadHandle::snapshot() const {
    struct stat info{};
    if (::fstat(static_cast<int>(mNativeHandle), &info) != 0)
        return errnoAs("cannot stat opened file");
    if (!S_ISREG(info.st_mode)) return {MaiErrorCode::InvalidInput, "opened file is not regular"};
    return readStatInfo(info);
}

MaiError MaiFileReadHandle::validate(const MaiRegularFileInfo& expected) const {
    const auto current = snapshot();
    if (!current) return current.error();
    MaiRegularFileInfo pathInfo;
    if (!MaiFileSystem::regularFileInfo(mPath, pathInfo) ||
        expected.bytes != current.value().bytes || expected.modified != current.value().modified ||
        expected.identity != current.value().identity || expected.bytes != pathInfo.bytes ||
        expected.modified != pathInfo.modified || expected.identity != pathInfo.identity)
        return {MaiErrorCode::InvalidInput, "file changed during reading; restart the read"};
    return {};
}

MaiFileReadStats MaiFileReadHandle::stats() const {
    return mStats;
}

MaiError MaiFileReadHandle::readAt(std::uint64_t offset, std::uint64_t maxBytes,
                                   std::string& contents, bool* eof) {
    contents.clear();
    if (eof) *eof = false;
    if (maxBytes == 0 || maxBytes > 1024 * 1024 ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - maxBytes)
        return {MaiErrorCode::InvalidInput, "invalid file range"};
    maiAssertBlockingAllowed("MaiFileReadHandle::readAt");
    std::string bytes(static_cast<std::size_t>(maxBytes), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = std::min<std::size_t>(64 * 1024, bytes.size() - done);
        ++mStats.readOperations;
        const ssize_t got = ::pread(static_cast<int>(mNativeHandle), bytes.data() + done, chunk,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return errnoAs("cannot read file range");
        }
        mStats.bytesRead += static_cast<std::uint64_t>(got);
        if (got == 0) {
            if (eof) *eof = true;
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    bytes.resize(done);
    contents = std::move(bytes);
    return {};
}

MaiError MaiFileSystem::readFile(const MaiFilePath& path, std::string& contents,
                                 std::uint64_t maxBytes, bool* truncated) {
    contents.clear();
    if (truncated) *truncated = false;
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::readFile");
    const int fd = ::open(path.value().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errnoAs("cannot open file for reading");

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const MaiError error = errnoAs("cannot stat file");
        ::close(fd);
        return error;
    }
    if (S_ISDIR(info.st_mode)) {
        ::close(fd);
        return MaiError::make(MaiErrorCode::InvalidInput, "path is a directory");
    }

    std::uint64_t toRead = static_cast<std::uint64_t>(info.st_size);
    if (maxBytes > 0 && toRead > maxBytes) {
        toRead = maxBytes;
        if (truncated) *truncated = true;
    }

    contents.resize(static_cast<std::size_t>(toRead));
    std::uint64_t done = 0;
    while (done < toRead) {
        const ssize_t got = ::read(fd, &contents[static_cast<std::size_t>(done)],
                                   static_cast<std::size_t>(toRead - done));
        if (got < 0) {
            if (errno == EINTR) continue;  // 被信号打断，重来
            const MaiError error = errnoAs("read failed");
            ::close(fd);
            return error;
        }
        if (got == 0) break;
        done += static_cast<std::uint64_t>(got);
    }
    contents.resize(static_cast<std::size_t>(done));
    ::close(fd);
    return {};
}

MaiError MaiFileSystem::readFileRange(const MaiFilePath& path, std::uint64_t offset,
                                      std::uint64_t maxBytes, std::string& contents) {
    contents.clear();
    auto opened = MaiFileReadHandle::open(path);
    if (!opened) return opened.error();
    const auto before = opened.value()->snapshot();
    if (!before) return before.error();
    const auto error = opened.value()->readAt(offset, maxBytes, contents);
    if (error) return error;
    const auto validated = opened.value()->validate(before.value());
    if (validated) contents.clear();
    return validated;
}

MaiError MaiFileSystem::writeFile(const MaiFilePath& path, const std::string& contents) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::writeFile");
    const int fd = ::open(path.value().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return errnoAs("cannot open file for writing");

    std::size_t done = 0;
    while (done < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + done, contents.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            const MaiError error = errnoAs("write failed");
            ::close(fd);
            return error;
        }
        done += static_cast<std::size_t>(written);
    }
    ::close(fd);
    return {};
}

MaiError MaiFileSystem::createEmptyFile(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::createEmptyFile");
    const int fd = ::open(path.value().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return errnoAs("cannot create file");
    if (::close(fd) != 0) return errnoAs("cannot close file after creation");
    return {};
}

MaiError MaiFileSystem::removeFile(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::removeFile");
    if (::unlink(path.value().c_str()) == 0) return {};
    return errnoAs("cannot delete file");
}

MaiError MaiFileSystem::publishNewFile(const MaiFilePath& source, const MaiFilePath& destination) {
    if (source.isEmpty() || destination.isEmpty())
        return MaiError::make(MaiErrorCode::InvalidInput, "empty publish path");
    maiAssertBlockingAllowed("MaiFileSystem::publishNewFile");
#if defined(__ANDROID__)
    const int status =
        maiAndroidPublishNewFile(source.value().c_str(), destination.value().c_str());
    if (status == 0) return {};
    errno = status;
    return errnoAs("cannot publish file");
#else
    if (::link(source.value().c_str(), destination.value().c_str()) != 0)
        return errnoAs("cannot publish file");
    if (::unlink(source.value().c_str()) == 0) return {};
    const int saved = errno;
    ::unlink(destination.value().c_str());
    errno = saved;
    return errnoAs("cannot remove published temporary file");
#endif
}

MaiError MaiFileSystem::createDirectories(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");
    if (isDirectory(path)) return {};

    const MaiFilePath parent = path.dirName();
    if (!parent.isEmpty() && parent != path) {
        const MaiError error = createDirectories(parent);
        if (error.hasError()) return error;
    }

    if (::mkdir(path.value().c_str(), 0755) == 0) return {};
    const int savedErrno = errno;
    if (savedErrno == EEXIST && isDirectory(path)) return {};  // 并发时别人刚好建好了
    errno = savedErrno;
    return errnoAs("cannot create directory");
}

void MaiFileSystem::walk(const MaiFilePath& root,
                         const std::function<MaiWalkAction(const MaiFileEntry&)>& visit,
                         const std::function<void(const MaiFilePath&, const MaiError&)>& onError) {
    if (root.isEmpty() || !visit) return;

    std::vector<MaiFilePath> pending{root};
    while (!pending.empty()) {
        const MaiFilePath directory = pending.back();
        pending.pop_back();

        DIR* handle = ::opendir(directory.value().c_str());
        if (!handle) {
            const auto error = errnoAs("cannot open directory for enumeration");
            if (onError) onError(directory, error);
            continue;
        }

        while (true) {
            // readdir 的空返回同时表示结束和失败；每次清 errno，避免访问回调或 lstat
            // 留下的旧错误把正常空目录误报为扫描失败。
            errno = 0;
            struct dirent* item = ::readdir(handle);
            if (!item) {
                if (errno != 0) {
                    const auto error = errnoAs("directory enumeration failed");
                    if (onError) onError(directory, error);
                }
                break;
            }
            const std::string name = item->d_name;
            if (name == "." || name == "..") continue;

            MaiFileEntry entry;
            entry.path = directory.append(MaiFilePath(name));
            entry.nameUtf8 = name;

            // lstat 不是 stat：要看的是这一项**自身**是不是符号链接，而不是它指向的东西。
            // 用 stat 的话符号链接会被当成它的目标，于是指回上级的链接会让遍历无限转下去。
            struct stat info{};
            if (::lstat(entry.path.value().c_str(), &info) != 0) {
                const auto error = errnoAs("cannot inspect directory entry");
                if (onError) onError(entry.path, error);
                continue;
            }
            const bool isSymlink = S_ISLNK(info.st_mode);
            entry.isDirectory = S_ISDIR(info.st_mode);
            entry.size = static_cast<std::uint64_t>(info.st_size);

            const MaiWalkAction action = visit(entry);
            if (action == MaiWalkAction::Stop) {
                ::closedir(handle);
                return;
            }
            if (!entry.isDirectory || isSymlink || action == MaiWalkAction::SkipDirectory) continue;

            pending.push_back(entry.path);
        }
        ::closedir(handle);
    }
}

MaiFilePath MaiFileSystem::resolve(const MaiFilePath& path) {
    if (path.isEmpty()) return {};

    // realpath 会解析符号链接，但要求路径存在。
    if (char* resolved = ::realpath(path.value().c_str(), nullptr)) {
        MaiFilePath out((std::string(resolved)));
        ::free(resolved);
        return out;
    }

    // 不存在（write 要新建的文件就是这种）：把存在的那一段解析掉，剩下的词法拼回去。只做词法的话，
    // root 里一个符号链接就能绕过越界检查。
    const MaiFilePath parent = path.dirName();
    const MaiFilePath leaf = path.baseName();
    if (parent.isEmpty() || parent == path || leaf.isEmpty()) return {};

    const MaiFilePath resolvedParent = resolve(parent);
    if (resolvedParent.isEmpty()) return {};
    return resolvedParent.append(leaf);
}

MaiFilePath MaiFileSystem::temporaryDirectory() {
    if (const char* fromEnvironment = ::getenv("TMPDIR")) {
        if (*fromEnvironment) return MaiFilePath(std::string(fromEnvironment));
    }
    return MaiFilePath(std::string("/tmp"));
}

void MaiFileSystem::removeRecursively(const MaiFilePath& path) {
    if (path.isEmpty() || !exists(path)) return;

    if (!isDirectory(path)) {
        ::unlink(path.value().c_str());
        return;
    }

    std::vector<MaiFilePath> files;
    std::vector<MaiFilePath> directories;
    walk(path, [&](const MaiFileEntry& entry) {
        if (entry.isDirectory) {
            directories.push_back(entry.path);
        } else {
            files.push_back(entry.path);
        }
        return MaiWalkAction::Continue;
    });

    for (const auto& file : files) ::unlink(file.value().c_str());
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        ::rmdir(it->value().c_str());
    ::rmdir(path.value().c_str());
}

// ── MaiThread ───────────────────────────────────────────────────

namespace {
thread_local std::string tThreadName;
}  // namespace

void MaiThread::setCurrentName(const std::string& name) {
    tThreadName = name;

#if defined(__APPLE__)
    // macOS 的是单参数版，只能给自己起名；上限 63 字节。
    ::pthread_setname_np(name.substr(0, 63).c_str());
#elif defined(__linux__)
    // Linux 上限是 **15 字节 + NUL**，超了整个调用会失败（不是截断），所以这里先自己截。
    ::pthread_setname_np(::pthread_self(), name.substr(0, 15).c_str());
#else
    (void)name;
#endif
}

std::string MaiThread::currentName() {
    return tThreadName;
}
