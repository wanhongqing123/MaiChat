// 文件作用：实现 Windows 平台的文件系统操作。
// 主要实现：exists、isDirectory、isSymbolicLink；对应公开接口中的状态和错误处理。
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <limits>
#include <utility>
#include <vector>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiBlockingCheck.h"
#include "MaiThread.h"

// Windows 实现：一律走**宽字符** API。
//
// 窄字符那套（CreateFileA / FindFirstFileA）会把路径按当前 ANSI 代码页
// 解释，中文机器上是 GBK，而我们的路径是 UTF-8——这正是 std::filesystem
// 当初让进程挂掉的同一个原因。

std::wstring MaiFileSystem::windowsApiPath(const MaiFilePath& path) {
    auto native = path.value();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    if (native.empty() || native.compare(0, 4, L"\\\\?\\") == 0 ||
        native.compare(0, 4, L"\\\\.\\") == 0)
        return native;
    if (path.isAbsolute() && native.size() < 248) return native;
    // 扩展前缀会关闭 Win32 的点段规范化，因此必须先取得完整路径。
    // 只在 API 调用边界使用此拼写，避免改变授权根逐段比较的表示。
    const DWORD required = ::GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required > 32767) return native;
    std::wstring full(required, L'\0');
    const DWORD length = ::GetFullPathNameW(native.c_str(), required, full.data(), nullptr);
    if (length == 0 || length >= required) return native;
    full.resize(length);
    if (full.size() < 248) return full;
    if (full.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') return L"\\\\?\\" + full;
    return native;
}

namespace {

// UTF-8 -> UTF-16。空串直接返回空，别让 MultiByteToWideChar 去处理
// 长度 0 的情况（它会返回 0，和失败没法区分）。
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                          needed, nullptr, nullptr);
    return utf8;
}

// 把 Win32 错误码翻成我们的错误码。
//
// 这是自己调系统 API 换来的好处之一：std::filesystem 把这些都糊成一个
// 笼统的 error_code，界面上就只能显示"操作失败"。
MaiErrorCode toErrorCode(DWORD lastError) {
    switch (lastError) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return MaiErrorCode::NotFound;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION: return MaiErrorCode::InvalidInput;
        default: return MaiErrorCode::Internal;
    }
}

std::string describe(DWORD lastError) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string message;
    if (length > 0 && buffer) {
        std::wstring wide(buffer, length);
        // FormatMessage 结尾会带 \r\n，去掉
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) wide.pop_back();
        message = narrow(wide);
    }
    if (buffer) ::LocalFree(buffer);
    if (message.empty()) message = "windows error " + std::to_string(lastError);
    return message;
}

MaiError lastErrorAs(const char* what) {
    const DWORD code = ::GetLastError();
    return MaiError::make(toErrorCode(code), std::string(what) + ": " + describe(code));
}

bool attributesOf(const MaiFilePath& path, WIN32_FILE_ATTRIBUTE_DATA& out) {
    if (path.isEmpty()) return false;
    return ::GetFileAttributesExW(MaiFileSystem::windowsApiPath(path).c_str(),
                                  GetFileExInfoStandard, &out) != FALSE;
}

}  // namespace

// ── MaiFilePath 的边界转换 ──────────────────────────────────────

MaiFilePath MaiFilePath::fromUtf8(const std::string& utf8) {
    return MaiFilePath(widen(utf8));
}

std::string MaiFilePath::toUtf8() const {
    return narrow(mValue);
}

std::string MaiFilePath::toGenericUtf8() const {
    std::wstring copy = mValue;
    for (wchar_t& character : copy) {
        if (character == L'\\') character = L'/';
    }
    return narrow(copy);
}

// ── MaiFileSystem ───────────────────────────────────────────────

bool MaiFileSystem::exists(const MaiFilePath& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return attributesOf(path, data);
}

bool MaiFileSystem::isDirectory(const MaiFilePath& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return false;
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool MaiFileSystem::isSymbolicLink(const MaiFilePath& path) {
    if (path.isEmpty()) return false;
    auto native = path.value();
    while (native.size() > 1 && MaiFilePath::isSeparator(native.back())) {
        if (native.size() == 3 && native[1] == L':') break;
        native.pop_back();
    }
    const DWORD attributes = ::GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool MaiFileSystem::fileSize(const MaiFilePath& path, std::uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return false;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

bool MaiFileSystem::modifiedTime(const MaiFilePath& path, std::int64_t& seconds) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!attributesOf(path, data)) return false;
    ULARGE_INTEGER stamp{};
    stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t kEpochOffset = 116444736000000000ull;
    if (stamp.QuadPart < kEpochOffset) return false;
    seconds = static_cast<std::int64_t>((stamp.QuadPart - kEpochOffset) / 10000000ull);
    return true;
}

namespace {
bool readHandleInfo(HANDLE handle, MaiRegularFileInfo& out) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    const bool valid =
        ::GetFileType(handle) == FILE_TYPE_DISK && ::GetFileInformationByHandle(handle, &info) &&
        ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    constexpr std::uint64_t kEpochOffset = 116444736000000000ull;
    if (!valid || static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart) < kEpochOffset)
        return false;
    MaiRegularFileInfo result;
    result.bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.modified = (basic.LastWriteTime.QuadPart - kEpochOffset) / 10000000ull;
    result.identity = std::to_string(info.dwVolumeSerialNumber) + ":" +
                      std::to_string(info.nFileIndexHigh) + ":" +
                      std::to_string(info.nFileIndexLow) + ":" +
                      std::to_string(basic.LastWriteTime.QuadPart) + ":" +
                      std::to_string(basic.ChangeTime.QuadPart);
    out = std::move(result);
    return true;
}
}  // namespace

bool MaiFileSystem::regularFileInfo(const MaiFilePath& path, MaiRegularFileInfo& out) {
    if (path.isEmpty()) return false;
    const HANDLE handle = ::CreateFileW(
        MaiFileSystem::windowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool valid = readHandleInfo(handle, out);
    ::CloseHandle(handle);
    return valid;
}

MaiFileReadHandle::MaiFileReadHandle(std::intptr_t nativeHandle, MaiFilePath path)
    : mNativeHandle(nativeHandle), mPath(std::move(path)) {
    mStats.openOperations = 1;
}

MaiFileReadHandle::~MaiFileReadHandle() {
    if (mNativeHandle != -1) ::CloseHandle(reinterpret_cast<HANDLE>(mNativeHandle));
}

MaiResult<std::unique_ptr<MaiFileReadHandle>> MaiFileReadHandle::open(const MaiFilePath& path) {
    if (path.isEmpty()) return {MaiErrorCode::InvalidInput, "empty file path"};
    maiAssertBlockingAllowed("MaiFileReadHandle::open");
    // 不共享删除使已验证的最后一级路径在整个分页期间不能被重命名或替换。
    const HANDLE handle =
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open file for reading");
    MaiRegularFileInfo info;
    if (!readHandleInfo(handle, info)) {
        ::CloseHandle(handle);
        return {MaiErrorCode::InvalidInput, "reading requires a non-link regular file"};
    }
    return std::unique_ptr<MaiFileReadHandle>(
        new MaiFileReadHandle(reinterpret_cast<std::intptr_t>(handle), path));
}

MaiResult<MaiRegularFileInfo> MaiFileReadHandle::snapshot() const {
    MaiRegularFileInfo info;
    if (!readHandleInfo(reinterpret_cast<HANDLE>(mNativeHandle), info))
        return {MaiErrorCode::InvalidInput, "cannot query the opened regular file"};
    return info;
}

MaiError MaiFileReadHandle::validate(const MaiRegularFileInfo& expected) const {
    const auto current = snapshot();
    if (!current) return current.error();
    const auto& actual = current.value();
    // 原生句柄不共享删除，路径替换由 Windows 在持有期间拒绝；再检查原路径存在，
    // 使父目录被外部改名等路径失效也不能作为成功分页交付。
    if (expected.bytes != actual.bytes || expected.modified != actual.modified ||
        expected.identity != actual.identity || !MaiFileSystem::exists(mPath) ||
        MaiFileSystem::isSymbolicLink(mPath))
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
        offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - maxBytes)
        return {MaiErrorCode::InvalidInput, "invalid file range"};
    maiAssertBlockingAllowed("MaiFileReadHandle::readAt");
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(reinterpret_cast<HANDLE>(mNativeHandle), position, nullptr, FILE_BEGIN))
        return lastErrorAs("cannot seek file range");
    std::string bytes(static_cast<std::size_t>(maxBytes), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(64 * 1024, bytes.size() - done));
        ++mStats.readOperations;
        if (!::ReadFile(reinterpret_cast<HANDLE>(mNativeHandle), bytes.data() + done, chunk, &got,
                        nullptr))
            return lastErrorAs("cannot read file range");
        mStats.bytesRead += got;
        if (got == 0) {
            if (eof) *eof = true;
            break;
        }
        done += got;
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
    // FILE_SHARE_READ | FILE_SHARE_WRITE：别人正开着这个文件时我们也能读。不给 SHARE_WRITE 的话，
    // 读一个编辑器正打开的文件会失败。
    const HANDLE handle = ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open file for reading");

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        const MaiError error = lastErrorAs("cannot read file size");
        ::CloseHandle(handle);
        return error;
    }

    std::uint64_t toRead = static_cast<std::uint64_t>(size.QuadPart);
    if (maxBytes > 0 && toRead > maxBytes) {
        toRead = maxBytes;
        if (truncated) *truncated = true;
    }

    contents.resize(static_cast<std::size_t>(toRead));
    std::uint64_t done = 0;
    while (done < toRead) {
        // ReadFile 一次最多 DWORD 能表示的量；大文件要分几次。
        const DWORD chunk =
            static_cast<DWORD>((toRead - done) > 0x10000000ull ? 0x10000000ull : (toRead - done));
        DWORD got = 0;
        if (!::ReadFile(handle, contents.data() + done, chunk, &got, nullptr)) {
            const MaiError error = lastErrorAs("read failed");
            ::CloseHandle(handle);
            return error;
        }
        if (got == 0) break;  // 提前到头了（有人同时截断了它）
        done += got;
    }
    contents.resize(static_cast<std::size_t>(done));
    ::CloseHandle(handle);
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
    const HANDLE handle =
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open file for writing");

    std::size_t done = 0;
    while (done < contents.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (contents.size() - done) > 0x10000000u ? 0x10000000u : (contents.size() - done));
        DWORD written = 0;
        if (!::WriteFile(handle, contents.data() + done, chunk, &written, nullptr)) {
            const MaiError error = lastErrorAs("write failed");
            ::CloseHandle(handle);
            return error;
        }
        done += written;
    }
    ::CloseHandle(handle);
    return {};
}

MaiError MaiFileSystem::createEmptyFile(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::createEmptyFile");
    const HANDLE handle =
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastErrorAs("cannot create file");
    ::CloseHandle(handle);
    return {};
}

MaiError MaiFileSystem::removeFile(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");

    maiAssertBlockingAllowed("MaiFileSystem::removeFile");
    if (::DeleteFileW(MaiFileSystem::windowsApiPath(path).c_str())) return {};
    return lastErrorAs("cannot delete file");
}

MaiError MaiFileSystem::publishNewFile(const MaiFilePath& source, const MaiFilePath& destination) {
    if (source.isEmpty() || destination.isEmpty())
        return MaiError::make(MaiErrorCode::InvalidInput, "empty publish path");
    maiAssertBlockingAllowed("MaiFileSystem::publishNewFile");
    if (::MoveFileExW(MaiFileSystem::windowsApiPath(source).c_str(),
                      MaiFileSystem::windowsApiPath(destination).c_str(), MOVEFILE_WRITE_THROUGH))
        return {};
    return lastErrorAs("cannot publish file");
}

namespace {
class MaiOwnedWindowsHandle {
public:
    explicit MaiOwnedWindowsHandle(HANDLE handle);
    ~MaiOwnedWindowsHandle();
    HANDLE get() const;

private:
    HANDLE mHandle;
};
MaiOwnedWindowsHandle::MaiOwnedWindowsHandle(HANDLE handle) : mHandle(handle) {}
MaiOwnedWindowsHandle::~MaiOwnedWindowsHandle() {
    if (mHandle != INVALID_HANDLE_VALUE) ::CloseHandle(mHandle);
}
HANDLE MaiOwnedWindowsHandle::get() const {
    return mHandle;
}

MaiResult<MaiFileInfo> objectInfo(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION data{};
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandle(handle, &data) ||
        !::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        return lastErrorAs("cannot inspect file");
    if (::GetFileType(handle) != FILE_TYPE_DISK ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return {MaiErrorCode::InvalidInput, "requires a non-link ordinary file or directory"};
    constexpr std::uint64_t epochOffset = 116444736000000000ull;
    if (static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart) < epochOffset)
        return {MaiErrorCode::NotSupported, "modification time precedes supported epoch"};
    MaiFileInfo info;
    info.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.sizeKnown = !info.isDirectory;
    info.bytes = info.sizeKnown
                     ? (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow
                     : 0;
    info.modified = (basic.LastWriteTime.QuadPart - epochOffset) / 10000000ull;
    info.identity = std::to_string(data.dwVolumeSerialNumber) + ":" +
                    std::to_string(data.nFileIndexHigh) + ":" + std::to_string(data.nFileIndexLow) +
                    ":" + std::to_string(basic.LastWriteTime.QuadPart) + ":" +
                    std::to_string(basic.ChangeTime.QuadPart);
    return info;
}

bool isWindowsFilesystemRoot(const MaiFilePath& path) {
    auto native = path.value();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    if (native.compare(0, 4, L"\\\\?\\") == 0 || native.compare(0, 4, L"\\\\.\\") == 0) {
        native.erase(0, 4);
        const bool unc = native.size() >= 4 && (native[0] == L'U' || native[0] == L'u') &&
                         (native[1] == L'N' || native[1] == L'n') &&
                         (native[2] == L'C' || native[2] == L'c') && native[3] == L'\\';
        if (unc)
            native = L"\\\\" + native.substr(4);
        else if (native.size() < 2 || native[1] != L':')
            return true;
    }
    while (native.size() > 1 && native.back() == L'\\') native.pop_back();
    if (native.empty() || native == L"\\" || (native.size() == 2 && native[1] == L':')) return true;
    if (native.compare(0, 2, L"\\\\") != 0) return false;
    // FilePath 的 UNC 前缀只认双斜杠，不能靠 dirName 识别共享根。这里纯词法拒绝，
    // 在任何解析/网络 I/O 或递归枚举前挡住 server/share（也包括扩展 UNC 和 . / ..）。
    std::vector<std::wstring> components;
    for (std::size_t start = 2; start < native.size();) {
        const auto end = native.find(L'\\', start);
        const auto part = native.substr(start, end == std::wstring::npos ? end : end - start);
        if (part == L"..") {
            if (components.size() <= 2) return true;
            components.pop_back();
        } else if (!part.empty() && part != L".")
            components.push_back(part);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return components.size() <= 2;
}

MaiError validateTransfer(const MaiFilePath& source, const MaiFilePath& destination) {
    if (source.isEmpty() || destination.isEmpty() ||
        MaiFileSystem::resolve(source) == MaiFileSystem::resolve(destination))
        return {MaiErrorCode::InvalidInput,
                "source and destination must be distinct nonempty paths"};
    if (MaiFileSystem::exists(destination) || MaiFileSystem::isSymbolicLink(destination))
        return {MaiErrorCode::InvalidInput, "destination already exists; nothing was overwritten"};
    if (MaiFileSystem::isSymbolicLink(source) ||
        MaiFileSystem::isSymbolicLink(destination.dirName()))
        return {MaiErrorCode::InvalidInput, "file transfer refuses symbolic links and junctions"};
    return {};
}

MaiError deleteWindowsNode(const MaiFilePath& path, const MaiDirectoryDeleteOptions& options,
                           MaiDirectoryDeleteResult& result, bool root, unsigned depth) {
    if (depth > 256) return {MaiErrorCode::InvalidInput, "directory depth exceeds delete limit"};
    if (options.isCanceled && options.isCanceled())
        return {MaiErrorCode::Canceled, "directory deletion canceled; prior removals are retained"};
    const auto realPath = MaiFileSystem::resolve(path);
    if (realPath.isEmpty() || isWindowsFilesystemRoot(realPath) || realPath == realPath.dirName() ||
        (options.pathAllowed && !options.pathAllowed(realPath)))
        return {MaiErrorCode::InvalidInput, "directory deletion path was denied"};
    // 不共享删除，防止枚举期间该节点被重命名或替换；最终也通过同一已验证句柄标记删除。
    MaiOwnedWindowsHandle handle(
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), DELETE | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open delete target");
    auto info = objectInfo(handle.get());
    if (!info) return info.error();
    if (root && (!info.value().isDirectory || (!options.expectedIdentity.empty() &&
                                               options.expectedIdentity != info.value().identity)))
        return {MaiErrorCode::InvalidInput,
                "directory identity changed after approval or target is not a directory"};
    if (info.value().isDirectory && options.recursive) {
        WIN32_FIND_DATAW entry{};
        const auto pattern = path.append(MaiFilePath(L"*"));
        const HANDLE finder =
            ::FindFirstFileW(MaiFileSystem::windowsApiPath(pattern).c_str(), &entry);
        if (finder == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_NOT_FOUND)
            return lastErrorAs("cannot enumerate delete directory");
        if (finder != INVALID_HANDLE_VALUE) {
            MaiError error;
            do {
                if (std::wcscmp(entry.cFileName, L".") == 0 ||
                    std::wcscmp(entry.cFileName, L"..") == 0)
                    continue;
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    error = {MaiErrorCode::InvalidInput,
                             "recursive deletion refuses symbolic links and junctions"};
                    break;
                }
                error = deleteWindowsNode(path.append(MaiFilePath(entry.cFileName)), options,
                                          result, false, depth + 1);
                if (error) break;
            } while (::FindNextFileW(finder, &entry));
            const auto last = ::GetLastError();
            ::FindClose(finder);
            if (error) return error;
            if (last != ERROR_NO_MORE_FILES) {
                ::SetLastError(last);
                return lastErrorAs("cannot finish delete enumeration");
            }
        }
    }
    if (options.isCanceled && options.isCanceled())
        return {MaiErrorCode::Canceled, "directory deletion canceled; prior removals are retained"};
    if (MaiFileSystem::resolve(path) != realPath ||
        (options.pathAllowed && !options.pathAllowed(realPath)))
        return {MaiErrorCode::InvalidInput, "delete path changed or access was denied"};
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!::SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                      sizeof(disposition)))
        return lastErrorAs("cannot delete node (directory must be empty unless recursive is true)");
    if (info.value().isDirectory)
        ++result.removedDirectories;
    else
        ++result.removedFiles;
    return {};
}
}  // namespace

MaiResult<MaiFileInfo> MaiFileSystem::stat(const MaiFilePath& path) {
    maiAssertBlockingAllowed("MaiFileSystem::stat");
    if (path.isEmpty()) return {MaiErrorCode::InvalidInput, "empty stat path"};
    if (isSymbolicLink(path))
        return {MaiErrorCode::InvalidInput, "stat refuses symbolic links and junctions"};
    MaiOwnedWindowsHandle handle(::CreateFileW(
        MaiFileSystem::windowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return lastErrorAs("cannot stat path");
    return objectInfo(handle.get());
}

MaiError MaiFileSystem::publishNewDirectory(const MaiFilePath& source,
                                              const MaiFilePath& destination) {
    maiAssertBlockingAllowed("MaiFileSystem::publishNewDirectory");
    if (source.isEmpty() || destination.isEmpty() || isWindowsFilesystemRoot(source) ||
        isWindowsFilesystemRoot(destination))
        return {MaiErrorCode::InvalidInput, "directory publication refuses empty paths and roots"};
    const auto sourceResolved = resolve(source);
    const auto destinationResolved = resolve(destination);
    if (sourceResolved.isEmpty() || destinationResolved.isEmpty() ||
        isWindowsFilesystemRoot(sourceResolved) || isWindowsFilesystemRoot(destinationResolved) ||
        sourceResolved == destinationResolved)
        return {MaiErrorCode::InvalidInput,
                "directory publication requires distinct non-root paths"};
    // 句柄查询拒绝 reparse point，包含长路径下 isSymbolicLink 无法识别的情况。
    MaiOwnedWindowsHandle handle(::CreateFileW(
        windowsApiPath(source).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open staged directory");
    const auto info = objectInfo(handle.get());
    if (!info) return info.error();
    if (!info.value().isDirectory)
        return {MaiErrorCode::InvalidInput, "directory publication requires a non-link directory"};
    // 不设置 REPLACE_EXISTING 或 COPY_ALLOWED；目标竞争与跨卷失败由内核原子裁决。
    if (::MoveFileExW(windowsApiPath(source).c_str(), windowsApiPath(destination).c_str(),
                      MOVEFILE_WRITE_THROUGH))
        return {};
    const auto saved = ::GetLastError();
    if (saved == ERROR_NOT_SAME_DEVICE)
        return {MaiErrorCode::NotSupported, "directory publication requires the same filesystem"};
    if (saved == ERROR_ALREADY_EXISTS || saved == ERROR_FILE_EXISTS)
        return {MaiErrorCode::InvalidInput, "destination already exists; nothing was overwritten"};
    ::SetLastError(saved);
    return lastErrorAs("cannot publish directory");
}

MaiError MaiFileSystem::setUserExecutable(const MaiFilePath& path) {
    maiAssertBlockingAllowed("MaiFileSystem::setUserExecutable");
    if (path.isEmpty()) return {MaiErrorCode::InvalidInput, "empty executable path"};
    MaiOwnedWindowsHandle handle(::CreateFileW(
        windowsApiPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open executable file");
    MaiRegularFileInfo info;
    if (!readHandleInfo(handle.get(), info))
        return {MaiErrorCode::InvalidInput,
                "executable permissions require a non-link ordinary file"};
    // Windows 无 POSIX 所有者执行位；验证类型即可，不改变现有 ACL。
    return {};
}

MaiError MaiFileSystem::moveFile(const MaiFilePath& source, const MaiFilePath& destination,
                                 const std::string& expectedIdentity,
                                 const std::function<bool()>& isCanceled) {
    maiAssertBlockingAllowed("MaiFileSystem::moveFile");
    auto error = validateTransfer(source, destination);
    if (error) return error;
    MaiOwnedWindowsHandle handle(
        ::CreateFileW(MaiFileSystem::windowsApiPath(source).c_str(), DELETE | FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return lastErrorAs("cannot open move source");
    const auto info = objectInfo(handle.get());
    if (!info) return info.error();
    if (info.value().isDirectory ||
        (!expectedIdentity.empty() && expectedIdentity != info.value().identity))
        return {MaiErrorCode::InvalidInput, "move requires an unchanged ordinary file"};
    if (isCanceled && isCanceled())
        return {MaiErrorCode::Canceled, "file move canceled before commit"};
    const auto target = resolve(destination).value();
    std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::copy(target.begin(), target.end(), rename->FileName);
    if (::SetFileInformationByHandle(handle.get(), FileRenameInfo, rename,
                                     static_cast<DWORD>(storage.size())))
        return {};
    if (::GetLastError() == ERROR_NOT_SAME_DEVICE)
        return {MaiErrorCode::NotSupported,
                "cross-filesystem move is not supported; source was retained"};
    return lastErrorAs("cannot move file without overwriting");
}

MaiError MaiFileSystem::copyFile(const MaiFilePath& source, const MaiFilePath& destination,
                                 const std::string& expectedIdentity,
                                 const std::function<bool()>& isCanceled) {
    maiAssertBlockingAllowed("MaiFileSystem::copyFile");
    auto error = validateTransfer(source, destination);
    if (error) return error;
    auto sourceHandle = MaiFileReadHandle::open(source);
    if (!sourceHandle) return sourceHandle.error();
    const auto before = sourceHandle.value()->snapshot();
    if (!before) return before.error();
    if (!expectedIdentity.empty() && before.value().identity != expectedIdentity)
        return {MaiErrorCode::InvalidInput, "copy source identity changed after approval"};
    MaiFilePath temporary;
    HANDLE output = INVALID_HANDLE_VALUE;
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary = destination.dirName().append(
            MaiFilePath::fromUtf8(".mai-copy-" + std::to_string(::GetCurrentProcessId()) + "-" +
                                  std::to_string(++sequence)));
        output = ::CreateFileW(MaiFileSystem::windowsApiPath(temporary).c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return lastErrorAs("cannot create private copy staging file");
    }
    if (output == INVALID_HANDLE_VALUE)
        return lastErrorAs("cannot allocate private copy staging file");
    std::uint64_t offset = 0;
    bool eof = false;
    while (!error && !eof) {
        if (isCanceled && isCanceled()) {
            error = {MaiErrorCode::Canceled, "file copy canceled before publication"};
            break;
        }
        std::string bytes;
        error = sourceHandle.value()->readAt(offset, 64 * 1024, bytes, &eof);
        if (error) break;
        std::size_t done = 0;
        while (done < bytes.size()) {
            DWORD written = 0;
            if (!::WriteFile(output, bytes.data() + done, static_cast<DWORD>(bytes.size() - done),
                             &written, nullptr) ||
                written == 0) {
                error = lastErrorAs("cannot write copied bytes");
                break;
            }
            done += written;
        }
        offset += bytes.size();
    }
    if (!error) error = sourceHandle.value()->validate(before.value());
    if (!error && !::FlushFileBuffers(output)) error = lastErrorAs("cannot flush copied bytes");
    if (!::CloseHandle(output) && !error) error = lastErrorAs("cannot close copied file");
    if (!error && isCanceled && isCanceled())
        error = {MaiErrorCode::Canceled, "file copy canceled before publication"};
    if (!error) error = publishNewFile(temporary, destination);
    if (error && !::DeleteFileW(MaiFileSystem::windowsApiPath(temporary).c_str()) &&
        ::GetLastError() != ERROR_FILE_NOT_FOUND)
        return {error.code(),
                error.message() + "; could not remove private staging file: " + temporary.toUtf8()};
    return error;
}

MaiDirectoryDeleteResult MaiFileSystem::deleteDirectory(const MaiFilePath& path,
                                                        const MaiDirectoryDeleteOptions& options) {
    maiAssertBlockingAllowed("MaiFileSystem::deleteDirectory");
    MaiDirectoryDeleteResult result;
    if (isWindowsFilesystemRoot(path))
        result.error = {MaiErrorCode::InvalidInput,
                        "directory deletion refuses filesystem and UNC share roots"};
    else if (isSymbolicLink(path))
        result.error = {MaiErrorCode::InvalidInput,
                        "directory deletion refuses symbolic links and junctions"};
    else
        result.error = deleteWindowsNode(path, options, result, true, 0);
    return result;
}

MaiError MaiFileSystem::createNewDirectory(const MaiFilePath& path) {
    maiAssertBlockingAllowed("MaiFileSystem::createNewDirectory");
    if (path.isEmpty() || isWindowsFilesystemRoot(path))
        return {MaiErrorCode::InvalidInput, "exclusive directory creation refuses empty paths and roots"};
    // CreateDirectory 的成功本身证明本次创建的所有权，不能把已有目录当作成功。
    if (::CreateDirectoryW(windowsApiPath(path).c_str(), nullptr)) return {};
    const auto saved = ::GetLastError();
    if (saved == ERROR_ALREADY_EXISTS || saved == ERROR_FILE_EXISTS)
        return {MaiErrorCode::InvalidInput, "directory creation target already exists"};
    ::SetLastError(saved);
    return lastErrorAs("cannot exclusively create directory");
}

MaiError MaiFileSystem::createDirectories(const MaiFilePath& path) {
    if (path.isEmpty()) return MaiError::make(MaiErrorCode::InvalidInput, "empty path");
    if (isDirectory(path)) return {};

    // 先建父目录再建自己。递归比循环好写，而路径深度是有上限的，不会把栈用光。
    const MaiFilePath parent = path.dirName();
    if (!parent.isEmpty() && parent != path) {
        const MaiError error = createDirectories(parent);
        if (error.hasError()) return error;
    }

    if (::CreateDirectoryW(MaiFileSystem::windowsApiPath(path).c_str(), nullptr)) return {};
    // 并发时别人可能刚好抢先建好了，那不算错。
    const DWORD code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS && isDirectory(path)) return {};
    ::SetLastError(code);
    return lastErrorAs("cannot create directory");
}

void MaiFileSystem::walk(const MaiFilePath& root,
                         const std::function<MaiWalkAction(const MaiFileEntry&)>& visit,
                         const std::function<void(const MaiFilePath&, const MaiError&)>& onError) {
    if (root.isEmpty() || !visit) return;

    // 自己维护待遍历队列，不用递归：目录树可能很深，
    // 而且这样 SkipDirectory 的实现就是"不往队列里放"，一目了然。
    std::vector<MaiFilePath> pending{root};

    while (!pending.empty()) {
        const MaiFilePath directory = pending.back();
        pending.pop_back();

        const MaiFilePath pattern = directory.append(MaiFilePath(L"*"));
        WIN32_FIND_DATAW data{};
        // FindExInfoBasic：不要 cAlternateFileName（8.3 短名），少一次查询。
        // 遍历大目录时这个差别是实打实的。
        const HANDLE handle =
            ::FindFirstFileExW(MaiFileSystem::windowsApiPath(pattern).c_str(), FindExInfoBasic,
                               &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD saved = ::GetLastError();
            // FindFirstFile 在正常空目录返回 FILE_NOT_FOUND；须另查目录仍存在，
            // 不能把已消失或无权访问的目录也误报为成功扫描。
            const bool empty = (saved == ERROR_FILE_NOT_FOUND || saved == ERROR_NO_MORE_FILES) &&
                               isDirectory(directory);
            if (!empty && onError)
                onError(directory,
                        {toErrorCode(saved), "cannot enumerate directory: " + describe(saved)});
            continue;
        }

        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;

            MaiFileEntry entry;
            entry.path = directory.append(MaiFilePath(name));
            entry.nameUtf8 = narrow(name);
            entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

            const MaiWalkAction action = visit(entry);
            if (action == MaiWalkAction::Stop) {
                ::FindClose(handle);
                return;
            }
            if (!entry.isDirectory || action == MaiWalkAction::SkipDirectory) continue;

            // 不跟进重解析点（符号链接、目录联接）。
            // 跟进的话一个指回上级的联接就能让遍历无限转下去。
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

            pending.push_back(entry.path);
        } while (::FindNextFileW(handle, &data));

        const DWORD saved = ::GetLastError();
        ::FindClose(handle);
        if (saved != ERROR_NO_MORE_FILES && onError)
            onError(directory,
                    {toErrorCode(saved), "directory enumeration failed: " + describe(saved)});
    }
}

MaiFilePath MaiFileSystem::resolve(const MaiFilePath& path) {
    if (path.isEmpty()) return {};

    // 存在的话用 GetFinalPathNameByHandleW：它会解析符号链接和联接，
    // 这是判断越界时必须做的一步——root 里放一个指向 C:\ 的联接，光靠词法规范化是看不出来的。
    //
    // FILE_FLAG_BACKUP_SEMANTICS 是打开**目录**句柄所必需的，少了它 CreateFileW 对目录一律失败。
    const HANDLE handle =
        ::CreateFileW(MaiFileSystem::windowsApiPath(path).c_str(), 0,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        std::wstring buffer(1024, L'\0');
        DWORD length =
            ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length > buffer.size()) {
            buffer.resize(length);
            length = ::GetFinalPathNameByHandleW(handle, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        ::CloseHandle(handle);
        if (length > 0) {
            buffer.resize(length);
            // 返回的是 "\\?\C:\..." 形式。那个前缀是给 API 用的，
            // 留着会让后面逐段比对时两边形状对不上，剥掉。
            constexpr wchar_t kPrefix[] = L"\\\\?\\";
            constexpr wchar_t kUncPrefix[] = L"\\\\?\\UNC\\";
            if (buffer.rfind(kUncPrefix, 0) == 0)
                buffer = L"\\\\" + buffer.substr(8);
            else if (buffer.rfind(kPrefix, 0) == 0)
                buffer.erase(0, 4);
            return MaiFilePath(buffer);
        }
    }

    // 不存在（write 要新建的文件就是这种）：尽力而为——用 GetFullPathNameW 做词法规范化，
    // 它会消掉 "." 和 ".."，并把相对路径按当前目录补全。符号链接解析不了，但路径都还不存在，
    // 也就无从链接起。
    std::wstring buffer(1024, L'\0');
    DWORD length = ::GetFullPathNameW(path.value().c_str(), static_cast<DWORD>(buffer.size()),
                                      buffer.data(), nullptr);
    if (length > buffer.size()) {
        buffer.resize(length);
        length = ::GetFullPathNameW(path.value().c_str(), static_cast<DWORD>(buffer.size()),
                                    buffer.data(), nullptr);
    }
    if (length == 0) return {};
    buffer.resize(length);
    return MaiFilePath(buffer);
}

MaiFilePath MaiFileSystem::temporaryDirectory() {
    std::wstring buffer(MAX_PATH + 1, L'\0');
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) return MaiFilePath(L".");
    buffer.resize(length);
    while (!buffer.empty() && MaiFilePath::isSeparator(buffer.back())) buffer.pop_back();
    return MaiFilePath(buffer);
}

void MaiFileSystem::removeRecursively(const MaiFilePath& path) {
    if (path.isEmpty() || !exists(path)) return;

    if (!isDirectory(path)) {
        ::DeleteFileW(MaiFileSystem::windowsApiPath(path).c_str());
        return;
    }

    // 先收集再删。一边遍历一边删同一个目录，FindNextFileW 的行为是未定义的。
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

    for (const auto& file : files) ::DeleteFileW(MaiFileSystem::windowsApiPath(file).c_str());
    // 子目录一定排在父目录后面删：walk 是先父后子给出来的，倒着来即可。
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        ::RemoveDirectoryW(MaiFileSystem::windowsApiPath(*it).c_str());
    ::RemoveDirectoryW(MaiFileSystem::windowsApiPath(path).c_str());
}

// ── MaiThread ───────────────────────────────────────────────────
// 放在这个文件里而不是单开一个：它就两个函数，而且和上面一样是"平台相关的一小段"，
// 单开文件反而让人多找一次。

namespace {

// 自己留一份。Windows 有 GetThreadDescription 可以读回来，但它是 Win10 1607+ 才有的，
// 而且要 LocalFree 释放，为了一句日志不值当。
thread_local std::string tThreadName;

}  // namespace

namespace {

// 只收裸指针，函数体里不出现任何带析构函数的对象——否则 MSVC 报 C2712。
void raiseThreadNameException(const char* name) {
    constexpr DWORD kVisualStudioThreadNameException = 0x406D1388;
#pragma pack(push, 8)
    struct ThreadNameInfo {
        DWORD type;      // 必须是 0x1000
        LPCSTR name;     // 指向名字，窄字符
        DWORD threadId;  // -1 表示当前线程
        DWORD flags;     // 保留，必须是 0
    };
#pragma pack(pop)
    ThreadNameInfo info{0x1000, name, static_cast<DWORD>(-1), 0};

    __try {
        ::RaiseException(kVisualStudioThreadNameException, 0, sizeof(info) / sizeof(ULONG_PTR),
                         reinterpret_cast<const ULONG_PTR*>(&info));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 没人接就算了
    }
}

}  // namespace

void MaiThread::setCurrentName(const std::string& name) {
    tThreadName = name;

    // SetThreadDescription 是 Win10 1607 才有的，动态取。它的好处是**不挂调试器也生效**——抓 dump、
    // 看任务管理器、事后用 WinDbg 打开都能看见名字。
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto setThreadDescription = reinterpret_cast<SetThreadDescriptionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (setThreadDescription) {
        setThreadDescription(::GetCurrentThread(), widen(name).c_str());
    }

    // 老系统上的兜底：MSVC 调试器约定的那个魔法异常。只有挂着调试器时才有人接，没挂的话白抛一次，
    // 所以先问一句。
    if (!::IsDebuggerPresent()) return;
    raiseThreadNameException(name.c_str());
}

std::string MaiThread::currentName() {
    return tThreadName;
}
