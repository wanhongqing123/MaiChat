// 文件作用：先处理进程代理/no_proxy 与 URL 规范化，再查询原生配置；不改变系统设置。
#include "MaiSystemProxy.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

#include <curl/curl.h>

#include "MaiNativeSystemProxy.h"
#include "MaiSystemProxyInternal.h"

namespace {

using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

bool part(CURLU* url, CURLUPart key, std::string& output, unsigned flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url, key, &value, flags) != CURLUE_OK) return false;
    output = value;
    curl_free(value);
    return true;
}

bool hasControls(const std::string& value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char character) { return character <= 0x20 || character == 0x7f; });
}

std::string withoutBrackets(std::string value) {
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') value = value.substr(1, value.size() - 2);
    return value;
}

bool unsignedNumber(std::string_view value, unsigned& output) {
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

struct Address {
    std::array<unsigned char, 16> bytes{};
    unsigned bits = 0;
};

Address parseAddress(const std::string& value) {
    Address address;
#if defined(_WIN32)
    if (::InetPtonA(AF_INET, value.c_str(), address.bytes.data()) == 1)
        address.bits = 32;
    else if (::InetPtonA(AF_INET6, value.c_str(), address.bytes.data()) == 1)
        address.bits = 128;
#else
    if (::inet_pton(AF_INET, value.c_str(), address.bytes.data()) == 1)
        address.bits = 32;
    else if (::inet_pton(AF_INET6, value.c_str(), address.bytes.data()) == 1)
        address.bits = 128;
#endif
    return address;
}

bool matchesCidr(const std::string& host, const std::string& pattern) {
    const auto slash = pattern.find('/');
    if (slash == std::string::npos) return false;
    const Address address = parseAddress(host);
    const Address network = parseAddress(withoutBrackets(pattern.substr(0, slash)));
    unsigned prefix = 0;
    if (address.bits == 0 || address.bits != network.bits ||
        !unsignedNumber(std::string_view(pattern).substr(slash + 1), prefix) || prefix > address.bits)
        return false;
    const unsigned bytes = prefix / 8;
    if (!std::equal(address.bytes.begin(), address.bytes.begin() + bytes, network.bytes.begin())) return false;
    const unsigned remaining = prefix % 8;
    const unsigned mask = remaining == 0 ? 0 : 0xffu << (8 - remaining);
    return remaining == 0 || (address.bytes[bytes] & mask) == (network.bytes[bytes] & mask);
}

bool matchesWildcard(const std::string& host, const std::string& pattern) {
    std::size_t cursor = 0, token = 0, star = std::string::npos, retry = 0;
    while (cursor < host.size()) {
        if (token < pattern.size() && (pattern[token] == '?' || pattern[token] == host[cursor])) {
            ++cursor;
            ++token;
        } else if (token < pattern.size() && pattern[token] == '*') {
            star = token++;
            retry = cursor;
        } else if (star != std::string::npos) {
            token = star + 1;
            cursor = ++retry;
        } else
            return false;
    }
    while (token < pattern.size() && pattern[token] == '*') ++token;
    return token == pattern.size();
}

bool matchesBypass(const MaiSystemProxyTarget& target, std::string pattern, bool windowsSyntax) {
    pattern = maiSystemProxyLower(maiSystemProxyTrim(std::move(pattern)));
    if (pattern.empty()) return false;
    if (pattern == "*") return true;
    if (windowsSyntax && pattern == "<local>")
        return target.host.find('.') == std::string::npos && parseAddress(target.host).bits == 0;
    if (pattern.find('/') != std::string::npos) return matchesCidr(target.host, pattern);

    // 方括号内的 IPv6 不把冒号当端口；裸 IPv6 同样只按地址匹配。
    std::string port;
    if (pattern.front() == '[') {
        const auto end = pattern.find(']');
        if (end == std::string::npos) return false;
        if (end + 1 < pattern.size()) {
            if (pattern[end + 1] != ':') return false;
            port = pattern.substr(end + 2);
        }
        pattern = pattern.substr(1, end - 1);
    } else {
        const auto colon = pattern.find(':');
        if (colon != std::string::npos && pattern.find(':', colon + 1) == std::string::npos) {
            port = pattern.substr(colon + 1);
            pattern.resize(colon);
        }
    }
    if (!port.empty()) {
        unsigned number = 0;
        if (!unsignedNumber(port, number) || number != target.port) return false;
    }
    if (pattern.empty()) return false;
    if (windowsSyntax) return matchesWildcard(target.host, pattern);
    while (!pattern.empty() && pattern.front() == '.') pattern.erase(0, 1);
    while (!pattern.empty() && pattern.back() == '.') pattern.pop_back();
    if (pattern.empty()) return false;
    return target.host == pattern ||
           (target.host.size() > pattern.size() &&
            target.host.compare(target.host.size() - pattern.size(), pattern.size(), pattern) == 0 &&
            target.host[target.host.size() - pattern.size() - 1] == '.');
}

std::optional<std::string> environment(const char* name) {
#if defined(_WIN32)
    // Windows 进程环境名按系统规则不区分大小写，不伪造 POSIX 的大小写优先级。
    const std::wstring wideName(name, name + std::char_traits<char>::length(name));
    const DWORD needed = ::GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0) return std::nullopt;
    std::vector<wchar_t> buffer(needed);
    const DWORD length = ::GetEnvironmentVariableW(wideName.c_str(), buffer.data(), needed);
    if (length == 0 || length >= needed) return std::nullopt;
    return maiSystemProxyUtf8(buffer.data());
#else
    const char* value = std::getenv(name);
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::optional<std::string> preferredEnvironment(const char* lower, const char* upper) {
    auto value = environment(lower);
    return value ? value : environment(upper);
}

}  // namespace

std::string maiSystemProxyTrim(std::string value) {
    const auto space = [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), space).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string maiSystemProxyLower(std::string value) {
    for (char& character : value)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return value;
}

bool maiSystemProxyTargetForUrl(const std::string& value, MaiSystemProxyTarget& target) {
    if (value.empty() || hasControls(value)) return false;
    CurlUrl url(curl_url(), &curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        return false;
    MaiSystemProxyTarget parsed;
    if (!part(url.get(), CURLUPART_SCHEME, parsed.scheme)) return false;
    parsed.scheme = maiSystemProxyLower(std::move(parsed.scheme));
    if (parsed.scheme == "ws")
        parsed.scheme = "http";
    else if (parsed.scheme == "wss")
        parsed.scheme = "https";
    if (parsed.scheme != "http" && parsed.scheme != "https") return false;
    std::string host;
    if (!part(url.get(), CURLUPART_HOST, host) || host.empty()) return false;
    host = maiSystemProxyLower(std::move(host));
    if (curl_url_set(url.get(), CURLUPART_SCHEME, parsed.scheme.c_str(), 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK)
        return false;
    std::string port;
    unsigned number = 0;
    if (!part(url.get(), CURLUPART_PORT, port, CURLU_DEFAULT_PORT) || !unsignedNumber(port, number) || number == 0 ||
        number > std::numeric_limits<std::uint16_t>::max() ||
        !part(url.get(), CURLUPART_URL, parsed.url, CURLU_NO_DEFAULT_PORT))
        return false;
    parsed.port = static_cast<std::uint16_t>(number);
    parsed.host = withoutBrackets(host);
    while (parsed.host.size() > 1 && parsed.host.back() == '.') parsed.host.pop_back();
    target = std::move(parsed);
    return true;
}

MaiSystemProxyResult maiSystemProxyAddress(const std::string& value, const std::string& defaultScheme) {
    std::string address = maiSystemProxyTrim(value);
    if (address.empty() || hasControls(address)) return {MaiSystemProxyMode::Unavailable, {}};
    if (address.find("://") == std::string::npos) address = defaultScheme + "://" + address;
    CurlUrl url(curl_url(), &curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, address.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        return {MaiSystemProxyMode::Unavailable, {}};
    std::string scheme, host, path, query, fragment;
    if (!part(url.get(), CURLUPART_SCHEME, scheme) || !part(url.get(), CURLUPART_HOST, host) || host.empty())
        return {MaiSystemProxyMode::Unavailable, {}};
    scheme = maiSystemProxyLower(std::move(scheme));
    const bool socks = scheme == "socks5" || scheme == "socks5h";
    if (!socks && scheme != "http" && scheme != "https") return {MaiSystemProxyMode::Unsupported, {}};
    if (socks) scheme = "socks5h";
    if ((part(url.get(), CURLUPART_PATH, path) && path != "/" && !path.empty()) ||
        (part(url.get(), CURLUPART_QUERY, query) && !query.empty()) ||
        (part(url.get(), CURLUPART_FRAGMENT, fragment) && !fragment.empty()))
        return {MaiSystemProxyMode::Unavailable, {}};
    std::string port;
    unsigned number = 0;
    if (part(url.get(), CURLUPART_PORT, port) &&
        (!unsignedNumber(port, number) || number == 0 || number > std::numeric_limits<std::uint16_t>::max()))
        return {MaiSystemProxyMode::Unavailable, {}};
    host = maiSystemProxyLower(std::move(host));
    if (curl_url_set(url.get(), CURLUPART_SCHEME, scheme.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK ||
        !part(url.get(), CURLUPART_URL, address))
        return {MaiSystemProxyMode::Unavailable, {}};
    return {socks ? MaiSystemProxyMode::Socks5 : MaiSystemProxyMode::Http, std::move(address)};
}

bool maiSystemProxyBypassMatches(const MaiSystemProxyTarget& target, const std::string& list, bool windowsSyntax) {
    std::size_t first = 0;
    while (first < list.size()) {
        const auto last = list.find_first_of(windowsSyntax ? ";, \t\r\n" : ",", first);
        if (matchesBypass(target, list.substr(first, last - first), windowsSyntax)) return true;
        if (last == std::string::npos) break;
        first = last + 1;
    }
    return false;
}

#if defined(_WIN32)
std::string maiSystemProxyUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string output(static_cast<std::size_t>(length), '\0');
    if (!::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, output.data(), length, nullptr, nullptr))
        return {};
    output.pop_back();
    return output;
}

std::wstring maiSystemProxyWide(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos ||
        value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {};
    const int length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                               output.data(), length))
        return {};
    return output;
}
#endif

MaiSystemProxyResult maiSystemProxyForUrl(const std::string& targetUrl) {
    MaiSystemProxyTarget target;
    if (!maiSystemProxyTargetForUrl(targetUrl, target)) return {MaiSystemProxyMode::Unavailable, {}};
    const auto bypass = preferredEnvironment("no_proxy", "NO_PROXY");
    if (bypass && maiSystemProxyBypassMatches(target, *bypass, false)) return {MaiSystemProxyMode::Direct, {}};
    // curl 在 POSIX 上刻意不接受大写 HTTP_PROXY；Windows 名称本身不区分大小写。
    auto proxy =
        target.scheme == "http" ? environment("http_proxy") : preferredEnvironment("https_proxy", "HTTPS_PROXY");
    if (!proxy) proxy = preferredEnvironment("all_proxy", "ALL_PROXY");
    return proxy ? maiSystemProxyAddress(*proxy) : maiNativeSystemProxyForUrl(target.url);
}
