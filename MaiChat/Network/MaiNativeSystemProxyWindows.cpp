// 文件作用：只读 Windows 用户/WinHTTP 代理，执行原生 PAC；纯解析函数与离线测试共用。
#include "MaiNativeSystemProxy.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#endif

#include "MaiSystemProxyInternal.h"

namespace {

// 测试直接包含本源文件，只调用纯解析函数；没有读取 OS 或执行 PAC 的测试 hook。
enum class WindowsProxyAccess { Direct, Named, Unknown };

std::vector<std::string> proxyEntries(const std::string& specification) {
    std::vector<std::string> result;
    std::size_t first = 0;
    while (first < specification.size()) {
        const auto last = specification.find(';', first);
        auto entry = maiSystemProxyTrim(specification.substr(first, last - first));
        if (!entry.empty()) result.push_back(std::move(entry));
        if (last == std::string::npos) break;
        first = last + 1;
    }
    return result;
}

MaiSystemProxyResult proxyEntry(const std::string& value, const std::string& defaultScheme = "http") {
    const auto separator = value.find_first_of(" \t");
    if (separator == std::string::npos) {
        return maiSystemProxyAddress(value, defaultScheme);
    }
    const std::string directive = maiSystemProxyLower(value.substr(0, separator));
    const std::string address = maiSystemProxyTrim(value.substr(separator + 1));
    if (directive == "proxy" || directive == "http") return maiSystemProxyAddress(address, "http");
    if (directive == "https") return maiSystemProxyAddress(address, "https");
    if (directive == "socks5" || directive == "socks5h") return maiSystemProxyAddress(address, "socks5h");
    return {MaiSystemProxyMode::Unsupported, {}};
}

MaiSystemProxyResult manualProxyForUrl(const std::string& targetUrl, const std::string& specification,
                                       const std::string& bypass) {
    MaiSystemProxyTarget target;
    if (!maiSystemProxyTargetForUrl(targetUrl, target)) return {MaiSystemProxyMode::Unavailable, {}};
    if (maiSystemProxyBypassMatches(target, bypass, true)) return {MaiSystemProxyMode::Direct, {}};
    std::optional<std::string> selected, generic, socks;
    bool assigned = false;
    bool unknownAssignment = false;
    for (const auto& entry : proxyEntries(specification)) {
        const auto equal = entry.find('=');
        const auto scheme = entry.find("://");
        if (equal == std::string::npos || (scheme != std::string::npos && scheme < equal)) {
            if (!generic) generic = entry;
            continue;
        }
        assigned = true;
        const std::string name = maiSystemProxyLower(maiSystemProxyTrim(entry.substr(0, equal)));
        const std::string address = maiSystemProxyTrim(entry.substr(equal + 1));
        if (name != "http" && name != "https" && name != "ftp" && name != "socks" && name != "socks5")
            unknownAssignment = true;
        if (name == target.scheme && !selected) selected = address;
        if ((name == "socks" || name == "socks5") && !socks) socks = address;
    }
    if (selected) return proxyEntry(*selected);
    if (generic) return proxyEntry(*generic);
    if (socks) return proxyEntry(*socks, "socks5h");
    if (unknownAssignment) return {MaiSystemProxyMode::Unsupported, {}};
    // 系统明确只为其他协议配置代理，此协议没有条目才是 Direct；解析失败不能冒充无代理。
    return assigned || maiSystemProxyTrim(specification).empty()
               ? MaiSystemProxyResult{MaiSystemProxyMode::Direct, {}}
               : MaiSystemProxyResult{MaiSystemProxyMode::Unavailable, {}};
}

MaiSystemProxyResult autoProxyForUrl(const std::string& targetUrl, WindowsProxyAccess access,
                                     const std::string& specification, const std::string& bypass) {
    MaiSystemProxyTarget target;
    if (!maiSystemProxyTargetForUrl(targetUrl, target) || access == WindowsProxyAccess::Unknown)
        return {MaiSystemProxyMode::Unavailable, {}};
    if (access == WindowsProxyAccess::Direct || maiSystemProxyBypassMatches(target, bypass, true))
        return {MaiSystemProxyMode::Direct, {}};
    const auto entries = proxyEntries(specification);
    if (entries.empty()) return {MaiSystemProxyMode::Unavailable, {}};
    if (maiSystemProxyLower(entries.front()) == "direct") return {MaiSystemProxyMode::Direct, {}};
    if (entries.front().find('=') != std::string::npos) return manualProxyForUrl(target.url, specification, bypass);
    // 保持 PAC/WinHTTP 的首选顺序。未知首项明确失败，不跳到末尾 DIRECT 或重放连接。
    return proxyEntry(entries.front());
}

#if defined(_WIN32)
// 私有纯策略边界：离线 fixture 直接提供 WinHTTP 错误，不读取系统或执行 PAC。
MaiSystemProxyResult fallbackAfterAutoProxyFailure(const std::string& targetUrl, bool explicitPac, bool autoDetect,
                                                   DWORD failure, const std::string& manual,
                                                   const std::string& bypass) {
    // WPAD 未发现网络 PAC 不是脚本执行失败：沿用明确的静态配置，空配置即 Direct。
    // 显式 PAC 及下载、脚本、超时等其他错误仍不可被静态配置或直连掩盖。
    if (!explicitPac && autoDetect && failure == ERROR_WINHTTP_AUTODETECTION_FAILED)
        return manualProxyForUrl(targetUrl, manual, bypass);
    return {MaiSystemProxyMode::Unavailable, {}};
}

struct CurrentUserProxy {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG value{};
    ~CurrentUserProxy() {
        if (value.lpszAutoConfigUrl) ::GlobalFree(value.lpszAutoConfigUrl);
        if (value.lpszProxy) ::GlobalFree(value.lpszProxy);
        if (value.lpszProxyBypass) ::GlobalFree(value.lpszProxyBypass);
    }
};

struct ProxyInformation {
    WINHTTP_PROXY_INFO value{};
    void clear() {
        if (value.lpszProxy) ::GlobalFree(value.lpszProxy);
        if (value.lpszProxyBypass) ::GlobalFree(value.lpszProxyBypass);
        value = {};
    }
    ~ProxyInformation() {
        clear();
    }
};

struct WinHttpSession {
    HINTERNET value = nullptr;
    ~WinHttpSession() {
        if (value) ::WinHttpCloseHandle(value);
    }
};

MaiSystemProxyResult proxyInformationForUrl(const std::string& targetUrl, const WINHTTP_PROXY_INFO& information) {
    WindowsProxyAccess access = WindowsProxyAccess::Unknown;
    if (information.dwAccessType == WINHTTP_ACCESS_TYPE_NO_PROXY)
        access = WindowsProxyAccess::Direct;
    else if (information.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY)
        access = WindowsProxyAccess::Named;
    return autoProxyForUrl(targetUrl, access, maiSystemProxyUtf8(information.lpszProxy),
                           maiSystemProxyUtf8(information.lpszProxyBypass));
}
#endif

}  // namespace

MaiSystemProxyResult maiNativeSystemProxyForUrl(const std::string& targetUrl) {
#if defined(_WIN32)
    MaiSystemProxyTarget target;
    if (!maiSystemProxyTargetForUrl(targetUrl, target)) return {MaiSystemProxyMode::Unavailable, {}};
    CurrentUserProxy settings;
    if (!::WinHttpGetIEProxyConfigForCurrentUser(&settings.value)) {
        ProxyInformation defaults;
        if (!::WinHttpGetDefaultProxyConfiguration(&defaults.value)) return {MaiSystemProxyMode::Unavailable, {}};
        return proxyInformationForUrl(target.url, defaults.value);
    }
    const std::string manual = maiSystemProxyUtf8(settings.value.lpszProxy);
    const std::string bypass = maiSystemProxyUtf8(settings.value.lpszProxyBypass);
    const bool explicitPac = settings.value.lpszAutoConfigUrl && *settings.value.lpszAutoConfigUrl;
    if (!explicitPac && !settings.value.fAutoDetect) return manualProxyForUrl(target.url, manual, bypass);
    const std::wstring url = maiSystemProxyWide(target.url);
    if (url.empty()) return {MaiSystemProxyMode::Unavailable, {}};
    WinHttpSession session{::WinHttpOpen(L"MaiChat system proxy", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !::WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000))
        return {MaiSystemProxyMode::Unavailable, {}};
    WINHTTP_AUTOPROXY_OPTIONS options{};
    if (explicitPac) {
        options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        options.lpszAutoConfigUrl = settings.value.lpszAutoConfigUrl;
    } else {
        options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
        options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    }
    ProxyInformation resolved;
    BOOL success = ::WinHttpGetProxyForUrl(session.value, url.c_str(), &options, &resolved.value);
    DWORD failure = success ? ERROR_SUCCESS : ::GetLastError();
    // 仅在原生 PAC 服务明确要求登录时重试集成身份；不读取、返回或记录任何用户凭据。
    if (!success && failure == ERROR_WINHTTP_LOGIN_FAILURE) {
        resolved.clear();
        options.fAutoLogonIfChallenged = TRUE;
        success = ::WinHttpGetProxyForUrl(session.value, url.c_str(), &options, &resolved.value);
        failure = success ? ERROR_SUCCESS : ::GetLastError();
    }
    if (success) return proxyInformationForUrl(target.url, resolved.value);
    return fallbackAfterAutoProxyFailure(target.url, explicitPac, settings.value.fAutoDetect != FALSE, failure, manual,
                                         bypass);
#else
    (void)targetUrl;
    return {MaiSystemProxyMode::Unavailable, {}};
#endif
}
