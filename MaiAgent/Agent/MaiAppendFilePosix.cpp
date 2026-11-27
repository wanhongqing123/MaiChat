// 文件作用：以 POSIX 原生追加文件和按文件描述绑定的锁实现日志写入。
#include "MaiAppendFile.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "MaiBlockingCheck.h"

namespace {

// OFD 字节锁不覆盖日志数据；同时尊重使用 32 位 off_t 的旧平台实际偏移上限。
constexpr std::uint64_t kWriterLockByte =
    static_cast<std::uint64_t>(std::min<std::int64_t>(std::numeric_limits<std::int64_t>::max(),
                                                      std::numeric_limits<off_t>::max())) -
    1;

MaiError nativeError(const char* operation) {
    const int code = errno;
    MaiErrorCode category = MaiErrorCode::Internal;
    if (code == ENOENT || code == ENOTDIR) category = MaiErrorCode::NotFound;
    if (code == EACCES || code == EPERM || code == ELOOP || code == EISDIR || code == EINVAL)
        category = MaiErrorCode::InvalidInput;
    return {category, std::string(operation) + ": errno " + std::to_string(code)};
}

MaiError missingLock() {
    return {MaiErrorCode::InvalidInput, "append file operation requires the writer lock"};
}

int lockWholeFile(int descriptor, int operation) {
    int result;
    do {
        result = ::flock(descriptor, operation);
    } while (result < 0 && errno == EINTR);
    return result;
}

#if defined(F_OFD_SETLKW) && defined(F_OFD_SETLK)
int lockReservedByte(int descriptor, short type, bool wait) {
    struct flock range{};
    range.l_type = type;
    range.l_whence = SEEK_SET;
    range.l_start = static_cast<off_t>(kWriterLockByte);
    range.l_len = 1;
    int result;
    do {
        result = ::fcntl(descriptor, wait ? F_OFD_SETLKW : F_OFD_SETLK, &range);
    } while (result < 0 && errno == EINTR);
    return result;
}
#endif

}  // namespace

struct MaiAppendFile::State {
    int descriptor = -1;
    bool isLocked = false;
    bool usesWholeFileLock = false;
};

MaiAppendFile::MaiAppendFile(std::unique_ptr<State> state) : mState(std::move(state)) {}

MaiAppendFile::~MaiAppendFile() {
    unlockWriter();
    // close 的 EINTR 不能盲目重试：Linux 可能已经回收编号，重试会关闭别人的新描述符。
    ::close(mState->descriptor);
}

MaiResult<std::unique_ptr<MaiAppendFile>> MaiAppendFile::open(MaiFilePath path) {
    if (path.isEmpty() || path.value().find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "invalid append file path"};
    maiAssertBlockingAllowed("MaiAppendFile::open");
    int descriptor;
    do {
        // O_NONBLOCK 防止特殊文件在 fstat 拒绝之前把 open 永久阻塞；普通文件不受影响。
        // 与 Chromium File::FLAG_APPEND 相同，O_APPEND 使内核的 EOF 定位与每次写入不可分割。
        descriptor = ::open(path.value().c_str(),
                            O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                            S_IRUSR | S_IWUSR);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) return nativeError("cannot open append file");
    struct stat information{};
    if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode)) {
        ::close(descriptor);
        return {MaiErrorCode::InvalidInput, "append requires a non-link regular file"};
    }
    auto state = std::make_unique<State>();
    state->descriptor = descriptor;
    return std::unique_ptr<MaiAppendFile>(new MaiAppendFile(std::move(state)));
}

MaiError MaiAppendFile::lockWriter() {
    if (mState->isLocked) return {MaiErrorCode::Busy, "append writer lock is already held"};
    maiAssertBlockingAllowed("MaiAppendFile::lockWriter");
#if defined(F_OFD_SETLKW) && defined(F_OFD_SETLK)
    if (!mState->usesWholeFileLock) {
        if (lockReservedByte(mState->descriptor, F_WRLCK, true) == 0) {
            mState->isLocked = true;
            return {};
        }
        // Linux 旧内核或文件系统不支持 OFD 时改用 flock；不同描述符依然互斥。
        // 不能回退 F_SETLKW：它按进程记锁，任意读句柄关闭都会撤销该文件的写锁。
        if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP)
            return nativeError("cannot lock append writer");
    }
#endif
    // Apple/BSD 没有 Linux OFD 接口。flock 是原生 advisory 锁，不阻塞普通前缀读取；
    // 锁随打开文件描述绑定，不会被另一独立读句柄的 close 意外释放。
    if (lockWholeFile(mState->descriptor, LOCK_EX) < 0)
        return nativeError("cannot lock append writer");
    mState->usesWholeFileLock = true;
    mState->isLocked = true;
    return {};
}

void MaiAppendFile::unlockWriter() {
    if (!mState->isLocked) return;
    int result = -1;
    if (mState->usesWholeFileLock) result = lockWholeFile(mState->descriptor, LOCK_UN);
#if defined(F_OFD_SETLKW) && defined(F_OFD_SETLK)
    else
        result = lockReservedByte(mState->descriptor, F_UNLCK, false);
#endif
    // 与 Windows 保持一致：原生解锁失败则继续记录持锁，析构 close 最终释放。
    if (result == 0) mState->isLocked = false;
}

MaiResult<std::uint64_t> MaiAppendFile::size() const {
    if (!mState->isLocked) return missingLock();
    maiAssertBlockingAllowed("MaiAppendFile::size");
    struct stat information{};
    if (::fstat(mState->descriptor, &information) != 0)
        return nativeError("cannot size append file");
    if (information.st_size < 0 ||
        static_cast<std::uint64_t>(information.st_size) > kWriterLockByte)
        return {MaiErrorCode::InvalidInput, "append file exceeds supported offset range"};
    return static_cast<std::uint64_t>(information.st_size);
}

MaiError MaiAppendFile::readAt(std::uint64_t offset, std::uint64_t maxBytes, std::string& out) {
    out.clear();
    if (!mState->isLocked) return missingLock();
    if (maxBytes == 0 || maxBytes > 1024 * 1024 || offset > kWriterLockByte - maxBytes)
        return {MaiErrorCode::InvalidInput, "invalid append file read range"};
    maiAssertBlockingAllowed("MaiAppendFile::readAt");
    std::string bytes(static_cast<std::size_t>(maxBytes), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t received = ::pread(mState->descriptor, bytes.data() + done,
                                         bytes.size() - done, static_cast<off_t>(offset + done));
        if (received < 0) {
            if (errno == EINTR) continue;
            return nativeError("cannot read append file");
        }
        if (received == 0) break;
        done += static_cast<std::size_t>(received);
    }
    bytes.resize(done);
    out = std::move(bytes);
    return {};
}

MaiResult<std::uint64_t> MaiAppendFile::append(const std::string& bytes) {
    auto before = size();
    if (!before) return before.error();
    if (bytes.size() > kWriterLockByte - before.value())
        return {MaiErrorCode::InvalidInput, "append would exceed supported offset range"};
    maiAssertBlockingAllowed("MaiAppendFile::append");
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = std::min<std::size_t>(bytes.size() - done, 1024 * 1024);
        const ssize_t written = ::write(mState->descriptor, bytes.data() + done, chunk);
        if (written < 0) {
            if (errno == EINTR) continue;
            return nativeError("cannot append file bytes");
        }
        if (written == 0) return {MaiErrorCode::Internal, "append write made no progress"};
        done += static_cast<std::size_t>(written);
    }
    return before.value();
}

MaiError MaiAppendFile::flush() {
    if (!mState->isLocked) return missingLock();
    maiAssertBlockingAllowed("MaiAppendFile::flush");
    int result;
    do {
        result = ::fsync(mState->descriptor);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return nativeError("cannot flush append file");
    return {};
}
