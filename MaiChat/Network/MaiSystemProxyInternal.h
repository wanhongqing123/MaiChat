// 文件作用：共用层与原生 provider 复用的私有 URL/地址/bypass 解析；不是额外公共代理 API。
#pragma once

#include <cstdint>
#include <string>

#include "MaiSystemProxy.h"

struct MaiSystemProxyTarget {
    std::string url;
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
};

std::string maiSystemProxyTrim(std::string value);
std::string maiSystemProxyLower(std::string value);
bool maiSystemProxyTargetForUrl(const std::string& value, MaiSystemProxyTarget& target);
MaiSystemProxyResult maiSystemProxyAddress(const std::string& value, const std::string& defaultScheme = "http");
bool maiSystemProxyBypassMatches(const MaiSystemProxyTarget& target, const std::string& list, bool windowsSyntax);

#if defined(_WIN32)
std::string maiSystemProxyUtf8(const wchar_t* value);
std::wstring maiSystemProxyWide(const std::string& value);
#endif
