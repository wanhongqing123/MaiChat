// 文件作用：离线验证进程代理/no_proxy 优先级和 URL 归一化；native 查询由本测试私有假实现替代。
#include "MaiSystemProxy.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

static int failures = 0;
#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                              \
        }                                                                            \
    } while (false)

namespace {

int nativeCalls = 0;
std::string nativeTarget;
MaiSystemProxyResult nativeResult{MaiSystemProxyMode::Http, "http://native.fixture:3210"};

void setProcessVariable(const char* name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    CHECK(_putenv_s(name, value ? value->c_str() : "") == 0);
#else
    CHECK(value ? ::setenv(name, value->c_str(), 1) == 0 : ::unsetenv(name) == 0);
#endif
}

// 只修改测试子进程环境；保存值留在内存，失败输出只含断言，不打印代理或凭据。
class MaiProxyEnvironment {
public:
    MaiProxyEnvironment() {
        for (const char* name : {"http_proxy", "https_proxy", "all_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                                 "ALL_PROXY", "NO_PROXY"}) {
            const char* value = std::getenv(name);
            mValues.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
        }
        for (const auto& saved : mValues) setProcessVariable(saved.first.c_str(), std::nullopt);
        nativeCalls = 0;
        nativeTarget.clear();
        nativeResult = {MaiSystemProxyMode::Http, "http://native.fixture:3210"};
    }
    ~MaiProxyEnvironment() {
        for (const auto& saved : mValues) setProcessVariable(saved.first.c_str(), saved.second);
    }

private:
    std::vector<std::pair<std::string, std::optional<std::string>>> mValues;
};

std::string proxyPart(const MaiSystemProxyResult& result, CURLUPart part) {
    CURLU* url = curl_url();
    if (!url) return {};
    char* value = nullptr;
    std::string output;
    if (curl_url_set(url, CURLUPART_URL, result.url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK &&
        curl_url_get(url, part, &value, CURLU_URLDECODE) == CURLUE_OK) {
        output = value;
        curl_free(value);
    }
    curl_url_cleanup(url);
    return output;
}

void checkProxy(const MaiSystemProxyResult& result, MaiSystemProxyMode mode, const std::string& scheme,
                const std::string& host, const std::string& port) {
    CHECK(result.mode == mode);
    CHECK(proxyPart(result, CURLUPART_SCHEME) == scheme);
    CHECK(proxyPart(result, CURLUPART_HOST) == host);
    CHECK(proxyPart(result, CURLUPART_PORT) == port);
}

void test_environment_proxy_precedes_native() {
    MaiProxyEnvironment environment;
    setProcessVariable("https_proxy", "http://fixture-user:fixture%40password@proxy.fixture:3128");
    const auto result = maiSystemProxyForUrl("https://example.com/path");
    checkProxy(result, MaiSystemProxyMode::Http, "http", "proxy.fixture", "3128");
    CHECK(proxyPart(result, CURLUPART_USER) == "fixture-user");
    CHECK(proxyPart(result, CURLUPART_PASSWORD) == "fixture@password");
    CHECK(nativeCalls == 0);
}

void test_websocket_schemes_use_http_environment() {
    MaiProxyEnvironment environment;
    setProcessVariable("http_proxy", "http://plain.fixture:8080");
    setProcessVariable("https_proxy", "https://secure.fixture:8443");
    checkProxy(maiSystemProxyForUrl("ws://example.com/devtools"), MaiSystemProxyMode::Http, "http", "plain.fixture",
               "8080");
    checkProxy(maiSystemProxyForUrl("wss://example.com/ws"), MaiSystemProxyMode::Http, "https", "secure.fixture",
               "8443");
    CHECK(nativeCalls == 0);
}

void test_all_proxy_and_variable_precedence() {
    MaiProxyEnvironment environment;
    setProcessVariable("ALL_PROXY", "socks5h://socks.fixture:1080");
    checkProxy(maiSystemProxyForUrl("https://example.com/"), MaiSystemProxyMode::Socks5, "socks5h", "socks.fixture",
               "1080");
    setProcessVariable("https_proxy", "http://specific.fixture:8081");
    checkProxy(maiSystemProxyForUrl("https://example.com/"), MaiSystemProxyMode::Http, "http", "specific.fixture",
               "8081");
#if !defined(_WIN32)
    // Windows 环境变量名不区分大小写；仅在 POSIX 上验证 curl 的大小写优先级。
    setProcessVariable("HTTPS_PROXY", "http://uppercase.fixture:8082");
    checkProxy(maiSystemProxyForUrl("https://example.com/"), MaiSystemProxyMode::Http, "http", "specific.fixture",
               "8081");
#endif
    CHECK(nativeCalls == 0);
}

void test_no_proxy_boundaries_and_cidr() {
    MaiProxyEnvironment environment;
    setProcessVariable("ALL_PROXY", "http://proxy.fixture:8080");
    setProcessVariable("no_proxy", "example.com, .internal.example,10.0.0.0/8,2001:db8::/32,::1");
    for (const std::string target :
         {"https://example.com/", "https://sub.example.com/", "https://node.internal.example/", "http://10.20.30.40/",
          "http://[2001:db8::7]/", "http://[::1]/"}) {
        const auto result = maiSystemProxyForUrl(target);
        CHECK(result.mode == MaiSystemProxyMode::Direct);
        CHECK(result.url.empty());
    }
    checkProxy(maiSystemProxyForUrl("https://notexample.com/"), MaiSystemProxyMode::Http, "http", "proxy.fixture",
               "8080");
    checkProxy(maiSystemProxyForUrl("http://11.20.30.40/"), MaiSystemProxyMode::Http, "http", "proxy.fixture", "8080");
    setProcessVariable("no_proxy", "*");
    const auto direct = maiSystemProxyForUrl("wss://outside.example/ws");
    CHECK(direct.mode == MaiSystemProxyMode::Direct && direct.url.empty());
    CHECK(nativeCalls == 0);
}

void test_normalized_url_reaches_native_and_preserves_failures() {
    MaiProxyEnvironment environment;
    const auto result = maiSystemProxyForUrl("wss://EXAMPLE.COM:443/path?q=1");
    CHECK(result.mode == MaiSystemProxyMode::Http);
    CHECK(result.url == nativeResult.url);
    CHECK(nativeCalls == 1);
    CHECK(nativeTarget == "https://example.com/path?q=1");
    nativeResult = {MaiSystemProxyMode::Unavailable, {}};
    CHECK(maiSystemProxyForUrl("https://example.com/").mode == MaiSystemProxyMode::Unavailable);
    nativeResult = {MaiSystemProxyMode::Unsupported, {}};
    CHECK(maiSystemProxyForUrl("https://example.com/").mode == MaiSystemProxyMode::Unsupported);
    nativeResult = {MaiSystemProxyMode::Direct, {}};
    const auto direct = maiSystemProxyForUrl("https://example.com/");
    CHECK(direct.mode == MaiSystemProxyMode::Direct && direct.url.empty());
}

void test_invalid_input_and_explicit_bad_proxy_do_not_fall_back() {
    MaiProxyEnvironment environment;
    for (const std::string target : {"", "not a url", "ftp://example.com/file", "https:///"}) {
        const auto result = maiSystemProxyForUrl(target);
        CHECK(result.mode == MaiSystemProxyMode::Unavailable || result.mode == MaiSystemProxyMode::Unsupported);
    }
    CHECK(nativeCalls == 0);
    setProcessVariable("https_proxy", "socks4://proxy.fixture:1080");
    CHECK(maiSystemProxyForUrl("https://example.com/").mode == MaiSystemProxyMode::Unsupported);
    CHECK(nativeCalls == 0);
    setProcessVariable("https_proxy", "http://");
    CHECK(maiSystemProxyForUrl("https://example.com/").mode == MaiSystemProxyMode::Unavailable);
    CHECK(nativeCalls == 0);
}

}  // namespace

// 该目标只编译 common 源，不链接原生实现。即使环境配置有误，也绝不查询真实系统或执行 PAC。
MaiSystemProxyResult maiNativeSystemProxyForUrl(const std::string& targetUrl) {
    ++nativeCalls;
    nativeTarget = targetUrl;
    return nativeResult;
}

int main() {
    test_environment_proxy_precedes_native();
    test_websocket_schemes_use_http_environment();
    test_all_proxy_and_variable_precedence();
    test_no_proxy_boundaries_and_cidr();
    test_normalized_url_reaches_native_and_preserves_failures();
    test_invalid_input_and_explicit_bad_proxy_do_not_fall_back();
    if (failures == 0) std::printf("system proxy common tests passed\n");
    return failures == 0 ? 0 : 1;
}
