// Native installed-app discovery and literal identity launch; no command shell.
#include "MaiChatTools/DesktopComputerPlatform.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <tlhelp32.h>
#include <appmodel.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename T>
struct ComRelease {
    void operator()(T* value) const {
        if (value) value->Release();
    }
};
template <typename T>
using ComOwner = std::unique_ptr<T, ComRelease<T>>;
struct TaskFree {
    void operator()(void* value) const {
        CoTaskMemFree(value);
    }
};
using TaskOwner = std::unique_ptr<void, TaskFree>;
struct HandleClose {
    void operator()(void* value) const {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
using HandleOwner = std::unique_ptr<void, HandleClose>;

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    bool valid() const {
        return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
};

MaiError canceledError() {
    return {MaiErrorCode::Canceled, "application operation canceled before dispatch"};
}
bool canceled(const std::function<bool()>& callback) {
    return callback && callback();
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (!length) return {};
    std::string output(std::size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), output.data(), length, nullptr,
                        nullptr);
    return output;
}

MaiResult<std::wstring> literalIdentity(const std::string& input) {
    if (input.empty() || input.size() > 32767 || input.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "app must be a registered application identity or absolute .exe path"};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), int(input.size()), nullptr, 0);
    if (!length) return {MaiErrorCode::InvalidInput, "app must contain valid UTF-8"};
    std::wstring output(std::size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), int(input.size()), output.data(), length);
    for (const auto ch : output)
        if (ch < 32 || ch == 127 || ch == L'"')
            return {MaiErrorCode::InvalidInput, "app must be a literal identity without quoting or control characters"};
    return output;
}

bool sameIdentity(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.data(), int(left.size()), right.data(), int(right.size()), TRUE) == CSTR_EQUAL;
}

bool executablePath(const std::wstring& path) {
    const bool drive = path.size() > 3 &&
                       ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                       path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() > 4 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'.' && path[2] != L'?';
    return (drive || unc) && path.size() > 4 && sameIdentity(path.substr(path.size() - 4), L".exe") &&
           path.find_first_of(L"\"\r\n\t<>|?*") == std::wstring::npos &&
           path.find(L':', drive ? 2 : 0) == std::wstring::npos;
}

std::wstring canonicalExecutable(const std::wstring& path) {
    HandleOwner file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return path;
    std::vector<wchar_t> normalized(32768, 0);
    const auto count = GetFinalPathNameByHandleW(file.get(), normalized.data(), DWORD(normalized.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= normalized.size()) return path;
    std::wstring result(normalized.data(), count);
    if (result.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + result.substr(8);
    if (result.compare(0, 4, L"\\\\?\\") == 0) return result.substr(4);
    return result;
}

MaiResult<std::wstring> processIdentity(DWORD processId) {
    HandleOwner process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) return {MaiErrorCode::NotFound, "application process identity is unavailable"};
    // Packaged windows use the same AUMID as AppsFolder, including hosted apps.
    using AppIdFunction = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
    const auto getAppId =
        reinterpret_cast<AppIdFunction>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId"));
    if (getAppId) {
        UINT32 count = 0;
        if (getAppId(process.get(), &count, nullptr) == ERROR_INSUFFICIENT_BUFFER && count > 1 && count <= 32768) {
            std::vector<wchar_t> id(count, 0);
            if (getAppId(process.get(), &count, id.data()) == ERROR_SUCCESS && id.front())
                return std::wstring(id.data());
        }
    }
    std::vector<wchar_t> path(32768, 0);
    DWORD count = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &count))
        return {MaiErrorCode::NotFound, "application executable identity is unavailable"};
    return canonicalExecutable(std::wstring(path.data(), count));
}

std::wstring hostedPackageIdentity(HWND window) {
    ComScope com;
    if (!com.valid()) return {};
    IPropertyStore* rawStore = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&rawStore)))) return {};
    ComOwner<IPropertyStore> store(rawStore);
    PROPVARIANT value{};
    const auto read = store->GetValue(PKEY_AppUserModel_ID, &value);
    std::wstring identity;
    if (SUCCEEDED(read) && value.vt == VT_LPWSTR && value.pwszVal) identity = value.pwszVal;
    PropVariantClear(&value);
    // A hosted packaged window can belong to ApplicationFrameHost rather than
    // its app process. Its native window property supplies the package AUMID.
    // Desktop explicit grouping IDs keep executable identity semantics.
    const auto separator = identity.find(L'!');
    if (separator == std::wstring::npos || separator == 0 || separator + 1 == identity.size()) return {};
    const auto family = identity.substr(0, separator);
    UINT32 packageCount = 0;
    UINT32 bufferLength = 0;
    const auto registered = GetPackagesByPackageFamily(family.c_str(), &packageCount, nullptr, &bufferLength, nullptr);
    return registered == ERROR_INSUFFICIENT_BUFFER && packageCount > 0 ? identity : std::wstring{};
}

struct ApplicationEntry {
    std::wstring identity;
    std::wstring displayName;
    ComOwner<IShellItem2> shellItem;
};

void addEntry(std::vector<ApplicationEntry>& entries, ApplicationEntry entry) {
    if (entry.identity.empty() || utf8(entry.identity).empty()) return;
    if (executablePath(entry.identity)) {
        const auto attributes = GetFileAttributesW(entry.identity.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY) return;
        entry.identity = canonicalExecutable(entry.identity);
    }
    const auto existing = std::find_if(entries.begin(), entries.end(),
                                       [&](const auto& value) { return sameIdentity(value.identity, entry.identity); });
    if (existing == entries.end())
        entries.push_back(std::move(entry));
    else {
        if (existing->displayName.empty()) existing->displayName = std::move(entry.displayName);
        if (!existing->shellItem) existing->shellItem = std::move(entry.shellItem);
    }
}

std::wstring itemString(IShellItem2* item, const PROPERTYKEY& key) {
    PWSTR text = nullptr;
    const auto result = item->GetString(key, &text);
    TaskOwner owner(text);
    return SUCCEEDED(result) && text ? text : L"";
}

MaiError enumerateShell(std::vector<ApplicationEntry>& entries, const std::function<bool()>& isCanceled) {
    IShellItem* rawFolder = nullptr;
    const auto opened = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&rawFolder));
    ComOwner<IShellItem> folder(rawFolder);
    if (FAILED(opened)) return {MaiErrorCode::NotSupported, "Windows installed application folder is unavailable"};
    IEnumShellItems* rawItems = nullptr;
    const auto bound = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&rawItems));
    ComOwner<IEnumShellItems> items(rawItems);
    if (FAILED(bound)) return {MaiErrorCode::NotSupported, "Windows installed application folder cannot be enumerated"};
    while (true) {
        if (canceled(isCanceled)) return canceledError();
        IShellItem* rawItem = nullptr;
        const auto next = items->Next(1, &rawItem, nullptr);
        ComOwner<IShellItem> item(rawItem);
        if (next == S_FALSE) break;
        if (FAILED(next)) return {MaiErrorCode::Internal, "Windows application folder enumeration failed"};
        IShellItem2* rawDetails = nullptr;
        if (FAILED(item->QueryInterface(IID_PPV_ARGS(&rawDetails)))) continue;
        ApplicationEntry entry;
        entry.shellItem.reset(rawDetails);
        const auto target = itemString(rawDetails, PKEY_Link_TargetParsingPath);
        entry.identity = executablePath(target) ? target : itemString(rawDetails, PKEY_AppUserModel_ID);
        // Parsing identities are accepted only when Windows registered the item;
        // caller strings never become shell namespace commands.
        if (entry.identity.empty()) {
            PWSTR parsing = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing))) {
                TaskOwner owner(parsing);
                if (parsing && executablePath(parsing)) entry.identity = parsing;
            }
        }
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
            TaskOwner owner(name);
            if (name) entry.displayName = name;
        }
        addEntry(entries, std::move(entry));
    }
    return {};
}

MaiError enumerateRegistry(std::vector<ApplicationEntry>& entries, const std::function<bool()>& isCanceled) {
    // App Paths contains literal executable registrations. Never interpret its
    // values as command lines or use PATH to resolve a caller's executable name.
    for (const HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
        for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0, KEY_READ | view,
                              &key) != ERROR_SUCCESS)
                continue;
            struct KeyOwner {
                HKEY value;
                ~KeyOwner() {
                    RegCloseKey(value);
                }
            } owner{key};
            for (DWORD index = 0;; ++index) {
                if (canceled(isCanceled)) return canceledError();
                wchar_t name[256]{};
                DWORD nameLength = 256;
                const auto next = RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
                if (next == ERROR_NO_MORE_ITEMS) break;
                if (next != ERROR_SUCCESS) return {MaiErrorCode::Internal, "Windows App Paths enumeration failed"};
                DWORD bytes = 0;
                const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
                if (RegGetValueW(key, name, nullptr, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes > 65536)
                    continue;
                std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, 0);
                if (RegGetValueW(key, name, nullptr, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) continue;
                std::wstring path(value.data());
                if (path.size() > 1 && path.front() == L'"' && path.back() == L'"')
                    path = path.substr(1, path.size() - 2);
                if (executablePath(path)) addEntry(entries, {std::move(path), std::wstring(name, nameLength), {}});
            }
        }
    return {};
}

MaiResult<std::vector<ApplicationEntry>> installedApplications(const std::function<bool()>& isCanceled) {
    std::vector<ApplicationEntry> entries;
    const auto shell = enumerateShell(entries, isCanceled);
    if (shell.code() == MaiErrorCode::Canceled) return shell;
    if (const auto registry = enumerateRegistry(entries, isCanceled)) return registry;
    if (shell && entries.empty()) return shell;
    return entries;
}

}  // namespace

MaiResult<std::string> maiDesktopApplicationId(const DesktopNativeWindow& window) {
    DWORD currentPid = 0;
    // When this is a real window, reject PID reuse or a stale native handle.
    if (!IsWindow(reinterpret_cast<HWND>(std::uintptr_t(window.id))) ||
        !GetWindowThreadProcessId(reinterpret_cast<HWND>(std::uintptr_t(window.id)), &currentPid) ||
        currentPid != window.processId)
        return {MaiErrorCode::NotFound, "window application identity is no longer available"};
    const auto hosted = hostedPackageIdentity(reinterpret_cast<HWND>(std::uintptr_t(window.id)));
    if (!hosted.empty()) return utf8(hosted);
    const auto identity = processIdentity(currentPid);
    if (!identity) return identity.error();
    return utf8(identity.value());
}

MaiResult<std::vector<DesktopNativeApplication>> maiDesktopApplications(const std::function<bool()>& isCanceled) {
    if (canceled(isCanceled)) return canceledError();
    ComScope com;
    if (!com.valid()) return {MaiErrorCode::Internal, "could not initialize Windows application discovery"};
    auto installed = installedApplications(isCanceled);
    if (!installed) return installed.error();
    auto windows = maiDesktopWindows();
    if (!windows) return windows.error();
    std::vector<DesktopNativeApplication> output;
    for (const auto& entry : installed.value()) {
        DesktopNativeApplication app;
        app.id = utf8(entry.identity);
        if (!entry.displayName.empty()) app.displayName = utf8(entry.displayName);
        output.push_back(std::move(app));
    }
    // Correlate by native PID facts, never by a display name. Windowless running
    // apps still come from AppsFolder/App Paths; usage statistics stay unknown.
    for (const auto& window : windows.value()) {
        if (canceled(isCanceled)) return canceledError();
        const auto nativeIdentity = maiDesktopApplicationId(window);
        if (!nativeIdentity) continue;
        const auto identity = literalIdentity(nativeIdentity.value());
        if (!identity) continue;
        auto existing = std::find_if(output.begin(), output.end(), [&](const auto& app) {
            const auto converted = literalIdentity(app.id);
            return converted && sameIdentity(converted.value(), identity.value());
        });
        if (existing == output.end()) {
            DesktopNativeApplication app;
            app.id = utf8(identity.value());
            output.push_back(std::move(app));
            existing = output.end() - 1;
        }
        existing->isRunning = true;
        existing->windows.push_back(window);
    }
    // A catalog alone cannot prove that an app without windows is stopped.
    // Read accessible process identities and leave inaccessible negatives unknown.
    HandleOwner snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W process{};
        process.dwSize = sizeof(process);
        if (Process32FirstW(snapshot.get(), &process)) do {
                if (canceled(isCanceled)) return canceledError();
                const auto identity = processIdentity(process.th32ProcessID);
                if (!identity) continue;
                for (auto& app : output) {
                    const auto converted = literalIdentity(app.id);
                    if (converted && sameIdentity(converted.value(), identity.value())) app.isRunning = true;
                }
            } while (Process32NextW(snapshot.get(), &process));
    }
    if (canceled(isCanceled)) return canceledError();
    return output;
}

MaiError maiDesktopLaunchApplication(const std::string& app, const std::function<bool()>& isCanceled) {
    if (canceled(isCanceled)) return canceledError();
    const auto identity = literalIdentity(app);
    if (!identity) return identity.error();
    const auto& path = identity.value();
    if (executablePath(path)) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY)
            return {MaiErrorCode::NotFound, "application executable does not exist"};
        if (canceled(isCanceled)) return canceledError();
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::wstring commandLine = L"\"" + path + L"\"";
        // Explicit lpApplicationName plus a single quoted argv[0] prevents path
        // ambiguity; no caller arguments, shell, file associations, or fallback.
        if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                            &process))
            return {MaiErrorCode::Internal,
                    "Windows rejected the application launch (error " + std::to_string(GetLastError()) + ")"};
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return {};  // Native dispatch happened: later cancellation cannot undo it.
    }
    if (path.find_first_of(L"\\/: \t") != std::wstring::npos ||
        (path.size() >= 4 && sameIdentity(path.substr(path.size() - 4), L".exe")))
        return {MaiErrorCode::InvalidInput,
                "app must be a registered identity or an absolute .exe path without arguments"};
    ComScope com;
    if (!com.valid()) return {MaiErrorCode::Internal, "could not initialize Windows application launch"};
    auto installed = installedApplications(isCanceled);
    if (!installed) return installed.error();
    for (const auto& entry : installed.value())
        if (sameIdentity(entry.identity, path) && entry.shellItem) {
            PIDLIST_ABSOLUTE rawId = nullptr;
            if (FAILED(SHGetIDListFromObject(entry.shellItem.get(), &rawId)))
                return {MaiErrorCode::NotFound, "registered application launch identity is unavailable"};
            TaskOwner id(rawId);
            if (canceled(isCanceled)) return canceledError();
            SHELLEXECUTEINFOW launch{};
            launch.cbSize = sizeof(launch);
            launch.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            launch.lpIDList = rawId;
            launch.lpVerb = L"open";
            launch.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&launch))
                return {MaiErrorCode::Internal, "Windows rejected the registered application launch (error " +
                                                    std::to_string(GetLastError()) + ")"};
            return {};  // Never retry an already submitted native launch.
        }
    return {MaiErrorCode::NotFound, "registered application identity was not found"};
}
