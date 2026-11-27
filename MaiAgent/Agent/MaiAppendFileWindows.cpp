// 文件作用：以 Win32 追加权限和原生字节锁实现不可覆盖的日志写入。
#include "MaiAppendFile.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <windows.h>

#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"

namespace {

// 锁放在数据区之外，避免 Windows 的强制字节锁阻塞独立读句柄读取日志前缀。
// 保留最后两个有符号偏移，所有文件长度和范围均在此界限以内。
constexpr std::uint64_t kWriterLockByte =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 1;

OVERLAPPED writerLockRange() {
    OVERLAPPED range{};
    range.Offset = static_cast<DWORD>(kWriterLockByte);
    range.OffsetHigh = static_cast<DWORD>(kWriterLockByte >> 32);
    return range;
}

MaiError nativeError(const char* operation) {
    const DWORD code = ::GetLastError();
    MaiErrorCode category = MaiErrorCode::Internal;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        category = MaiErrorCode::NotFound;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
        code == ERROR_INVALID_NAME || code == ERROR_DIRECTORY)
        category = MaiErrorCode::InvalidInput;
    return {category, std::string(operation) + ": Win32 " + std::to_string(code)};
}

MaiError missingLock() {
    return {MaiErrorCode::InvalidInput, "append file operation requires the writer lock"};
}

}  // namespace

struct MaiAppendFile::State {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool isLocked = false;
};

MaiAppendFile::MaiAppendFile(std::unique_ptr<State> state) : mState(std::move(state)) {}

MaiAppendFile::~MaiAppendFile() {
    unlockWriter();
    ::CloseHandle(mState->handle);
}

MaiResult<std::unique_ptr<MaiAppendFile>> MaiAppendFile::open(MaiFilePath path) {
    if (path.isEmpty() || path.value().find(L'\0') != std::wstring::npos)
        return {MaiErrorCode::InvalidInput, "invalid append file path"};
    maiAssertBlockingAllowed("MaiAppendFile::open");
    // 与 Chromium File::FLAG_APPEND 相同，只给 FILE_APPEND_DATA，不给 FILE_WRITE_DATA。
    // 即使 readAt 移动文件指针，WriteFile 仍由内核定位到 EOF，不能覆盖旧字节。
    // 不共享删除，且检查打开后的句柄，防止末段链接和检查后替换竞态。
    HANDLE handle =
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), GENERIC_READ | FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nativeError("cannot open append file");
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileType(handle) != FILE_TYPE_DISK ||
        !::GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        ::CloseHandle(handle);
        return {MaiErrorCode::InvalidInput, "append requires a non-link regular file"};
    }
    auto state = std::make_unique<State>();
    state->handle = handle;
    return std::unique_ptr<MaiAppendFile>(new MaiAppendFile(std::move(state)));
}

MaiError MaiAppendFile::lockWriter() {
    if (mState->isLocked) return {MaiErrorCode::Busy, "append writer lock is already held"};
    maiAssertBlockingAllowed("MaiAppendFile::lockWriter");
    auto range = writerLockRange();
    // 同步句柄上的 LockFileEx 阻塞等待；不同句柄即使属于同一进程也互斥。
    if (!::LockFileEx(mState->handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range))
        return nativeError("cannot lock append writer");
    mState->isLocked = true;
    return {};
}

void MaiAppendFile::unlockWriter() {
    if (!mState->isLocked) return;
    auto range = writerLockRange();
    // 原生失败时保留持锁状态；后续操作不能误认为已成功交出写权。
    // 析构的 CloseHandle 仍会由内核释放该句柄持有的锁。
    if (::UnlockFileEx(mState->handle, 0, 1, 0, &range)) mState->isLocked = false;
}

MaiResult<std::uint64_t> MaiAppendFile::size() const {
    if (!mState->isLocked) return missingLock();
    maiAssertBlockingAllowed("MaiAppendFile::size");
    LARGE_INTEGER length{};
    if (!::GetFileSizeEx(mState->handle, &length)) return nativeError("cannot size append file");
    if (length.QuadPart < 0 || static_cast<std::uint64_t>(length.QuadPart) > kWriterLockByte)
        return {MaiErrorCode::InvalidInput, "append file exceeds supported offset range"};
    return static_cast<std::uint64_t>(length.QuadPart);
}

MaiError MaiAppendFile::readAt(std::uint64_t offset, std::uint64_t maxBytes, std::string& out) {
    out.clear();
    if (!mState->isLocked) return missingLock();
    if (maxBytes == 0 || maxBytes > 1024 * 1024 || offset > kWriterLockByte - maxBytes)
        return {MaiErrorCode::InvalidInput, "invalid append file read range"};
    maiAssertBlockingAllowed("MaiAppendFile::readAt");
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(mState->handle, position, nullptr, FILE_BEGIN))
        return nativeError("cannot seek append file");
    std::string bytes(static_cast<std::size_t>(maxBytes), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD received = 0;
        if (!::ReadFile(mState->handle, bytes.data() + done,
                        static_cast<DWORD>(bytes.size() - done), &received, nullptr))
            return nativeError("cannot read append file");
        if (received == 0) break;
        done += received;
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
        DWORD written = 0;
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1024 * 1024));
        if (!::WriteFile(mState->handle, bytes.data() + done, chunk, &written, nullptr))
            return nativeError("cannot append file bytes");
        if (written == 0) return {MaiErrorCode::Internal, "append write made no progress"};
        done += written;
    }
    return before.value();
}

MaiError MaiAppendFile::flush() {
    if (!mState->isLocked) return missingLock();
    maiAssertBlockingAllowed("MaiAppendFile::flush");
    if (!::FlushFileBuffers(mState->handle)) return nativeError("cannot flush append file");
    return {};
}
