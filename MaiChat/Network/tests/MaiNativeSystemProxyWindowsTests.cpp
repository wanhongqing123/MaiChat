// 文件作用：直接验证 Windows 正式源内的纯解析函数，不查询注册表、系统代理或执行 PAC。
// 本目标不另行编译同一 cpp，避免重复定义 native 入口；不增加公共测试 hook。
#include "../MaiNativeSystemProxyWindows.cpp"

#include <cstdio>
#include <string>

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

std::string proxyPart(const MaiSystemProxyResult& result, CURLUPart part) {
    CURLU* url = curl_url();
    if (!url) return {};
    char* value = nullptr;
    std::string output;
    if (curl_url_set(url, CURLUPART_URL, result.url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK &&
        curl_url_get(url, part, &value, 0) == CURLUE_OK) {
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

void test_manual_protocol_selection() {
    const std::string specification = "http=plain.fixture:8080;https=secure.fixture:8443;socks=socks.fixture:1080";
    checkProxy(manualProxyForUrl("http://example.com/", specification, ""), MaiSystemProxyMode::Http, "http",
               "plain.fixture", "8080");
    checkProxy(manualProxyForUrl("https://example.com/", specification, ""), MaiSystemProxyMode::Http, "http",
               "secure.fixture", "8443");
    checkProxy(manualProxyForUrl("https://example.com/", "proxy.fixture:3128", ""), MaiSystemProxyMode::Http, "http",
               "proxy.fixture", "3128");
    checkProxy(manualProxyForUrl("https://example.com/", "socks=socks.fixture:1080", ""), MaiSystemProxyMode::Socks5,
               "socks5h", "socks.fixture", "1080");
    const auto unconfiguredScheme = manualProxyForUrl("https://example.com/", "http=plain.fixture:8080", "");
    CHECK(unconfiguredScheme.mode == MaiSystemProxyMode::Direct && unconfiguredScheme.url.empty());
}

void test_manual_bypass_is_host_and_port_aware() {
    const std::string specification = "proxy.fixture:8080";
    const std::string bypass = "*.example.com;EXACT.FIXTURE;<local>;10.0.0.*;[::1];secure.fixture:443";
    for (const std::string target : {"https://node.example.com/", "http://exact.fixture/", "http://printer/",
                                     "http://10.0.0.17/", "http://[::1]/", "https://secure.fixture/"}) {
        const auto result = manualProxyForUrl(target, specification, bypass);
        CHECK(result.mode == MaiSystemProxyMode::Direct && result.url.empty());
    }
    checkProxy(manualProxyForUrl("https://notexample.com/", specification, bypass), MaiSystemProxyMode::Http, "http",
               "proxy.fixture", "8080");
    checkProxy(manualProxyForUrl("https://secure.fixture:444/", specification, bypass), MaiSystemProxyMode::Http,
               "http", "proxy.fixture", "8080");
}

void test_auto_proxy_result_preserves_explicit_direct_and_order() {
    const auto direct = autoProxyForUrl("https://example.com/", WindowsProxyAccess::Direct, "", "");
    CHECK(direct.mode == MaiSystemProxyMode::Direct && direct.url.empty());
    checkProxy(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Named,
                               "first.fixture:8080;second.fixture:8081", ""),
               MaiSystemProxyMode::Http, "http", "first.fixture", "8080");
    checkProxy(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Named, "PROXY pac.fixture:3128; DIRECT", ""),
               MaiSystemProxyMode::Http, "http", "pac.fixture", "3128");
    checkProxy(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Named, "HTTPS tls.fixture:8443", ""),
               MaiSystemProxyMode::Http, "https", "tls.fixture", "8443");
    const auto bypassed =
        autoProxyForUrl("https://node.example.com/", WindowsProxyAccess::Named, "proxy.fixture:8080", "*.example.com");
    CHECK(bypassed.mode == MaiSystemProxyMode::Direct && bypassed.url.empty());
}

void test_invalid_and_unsupported_results_never_become_direct() {
    CHECK(manualProxyForUrl("https://example.com/", "socks4://proxy.fixture:1080", "").mode ==
          MaiSystemProxyMode::Unsupported);
    CHECK(manualProxyForUrl("https://example.com/", "http://", "").mode == MaiSystemProxyMode::Unavailable);
    CHECK(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Named, "", "").mode ==
          MaiSystemProxyMode::Unavailable);
    CHECK(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Unknown, "", "").mode ==
          MaiSystemProxyMode::Unavailable);
    CHECK(autoProxyForUrl("https://example.com/", WindowsProxyAccess::Named, "SOCKS4 proxy.fixture:1080", "").mode ==
          MaiSystemProxyMode::Unsupported);
}

#if defined(_WIN32)
void test_only_missing_wpad_uses_explicit_static_configuration() {
    const std::string target = "https://example.com/";
    const auto direct = fallbackAfterAutoProxyFailure(target, false, true, ERROR_WINHTTP_AUTODETECTION_FAILED, "", "");
    CHECK(direct.mode == MaiSystemProxyMode::Direct && direct.url.empty());
    checkProxy(fallbackAfterAutoProxyFailure(target, false, true, ERROR_WINHTTP_AUTODETECTION_FAILED,
                                             "https=manual.fixture:8443", ""),
               MaiSystemProxyMode::Http, "http", "manual.fixture", "8443");
    const auto bypass = fallbackAfterAutoProxyFailure(target, false, true, ERROR_WINHTTP_AUTODETECTION_FAILED,
                                                      "manual.fixture:8080", "example.com");
    CHECK(bypass.mode == MaiSystemProxyMode::Direct && bypass.url.empty());

    // Explicit PAC and real resolver failures must never fall back to the
    // static configuration, even when it contains a reachable manual proxy.
    for (const DWORD error : {DWORD(ERROR_WINHTTP_AUTODETECTION_FAILED), DWORD(ERROR_WINHTTP_TIMEOUT),
                              DWORD(ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT),
                              DWORD(ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT), DWORD(ERROR_WINHTTP_LOGIN_FAILURE)}) {
        for (const auto* manual : {"", "manual.fixture:8080"}) {
            CHECK(fallbackAfterAutoProxyFailure(target, true, true, error, manual, "").mode ==
                  MaiSystemProxyMode::Unavailable);
            if (error != ERROR_WINHTTP_AUTODETECTION_FAILED)
                CHECK(fallbackAfterAutoProxyFailure(target, false, true, error, manual, "").mode ==
                      MaiSystemProxyMode::Unavailable);
        }
    }
    CHECK(fallbackAfterAutoProxyFailure(target, false, false, ERROR_WINHTTP_AUTODETECTION_FAILED, "manual.fixture:8080",
                                        "")
              .mode == MaiSystemProxyMode::Unavailable);
}
#endif

}  // namespace

int main() {
    test_manual_protocol_selection();
    test_manual_bypass_is_host_and_port_aware();
    test_auto_proxy_result_preserves_explicit_direct_and_order();
    test_invalid_and_unsupported_results_never_become_direct();
#if defined(_WIN32)
    test_only_missing_wpad_uses_explicit_static_configuration();
#endif
    if (failures == 0) std::printf("Windows proxy parser tests passed\n");
    return failures == 0 ? 0 : 1;
}
