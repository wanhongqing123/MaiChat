// 文件作用：验证逐请求网络租约、DNS/socket 一致性及代次失效边界。
#include "MaiCurlNetworkRoute.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <curl/curl.h>

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::cerr << __FILE__ << static_cast<char>(58) << __LINE__ << std::endl; \
            std::abort();                                                            \
        }                                                                            \
    } while (false)

int main() {
    for (const char* name :
         {"http_proxy", "https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"}) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    unsigned long long dnsNetwork = 0;
    uint64_t socketNetwork = 0;
    MaiCurlNetworkRoute::Backend backend;
    backend.resolve = [&](uint64_t network, const std::string&,
                          std::vector<std::string>& addresses) {
        dnsNetwork = network;
        addresses = {"203.0.113.9", "2001:db8::9"};
        return true;
    };
    backend.bindSocket = [&](uint64_t network, intptr_t) {
        socketNetwork = network;
        return false;  // Stop before connect; no external traffic is required.
    };
    MaiCurlNetworkRoute::setBackend(backend);
    MaiCurlNetworkRoute::publish(17, 1);
    auto* curl = curl_easy_init();
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://provider.example/responses", false, 1000) == 0);
        CHECK(route.networkHandle() == 17 && dnsNetwork == 17);
        curl_easy_setopt(curl, CURLOPT_URL, "https://provider.example/responses");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 100L);
        CHECK(curl_easy_perform(curl) != CURLE_OK);
        CHECK(socketNetwork == 17 && route.dnsCalls() == 1 && route.socketCalls() > 0);
        // A notification cannot rewrite an in-flight request's captured network.
        MaiCurlNetworkRoute::publish(19, 2);
        CHECK(route.networkHandle() == 17 && route.hasChanged());
        socketNetwork = 0;
        CHECK(curl_easy_perform(curl) != CURLE_OK);
        CHECK(socketNetwork == 0);  // Policy changed before bind; no old-network socket is opened.
    }
    curl_easy_reset(curl);
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://[2001:db8::9]/a", false, 1000) == 0);
        CHECK(route.networkHandle() == 19 && route.dnsCalls() == 0);
    }
    curl_easy_reset(curl);
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "wss://public.test/realtime", false, 1000) == 0);
        CHECK(route.networkHandle() == 19 && route.dnsCalls() == 1);
        curl_easy_setopt(curl, CURLOPT_URL, "wss://public.test/realtime");
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 100L);
        socketNetwork = 0;
        CHECK(curl_easy_perform(curl) != CURLE_OK);
        CHECK(socketNetwork == 19 && route.socketCalls() > 0);
    }
    curl_easy_reset(curl);
    for (const char* url :
         {"https://localhost/a", "https://printer/a", "https://x.local/a", "https://192.168.1.1/a",
          "https://[::1]/a", "https://[fe80::1]/a", "https://[::ffff:192.168.1.1]/a"}) {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, url, false, 1000) == 0);
        CHECK(route.networkHandle() == 0);
    }
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://provider.example/a", true, 1000) == 0);
        CHECK(route.networkHandle() == 0);
    }
    backend.resolve = [](uint64_t, const std::string&, std::vector<std::string>& addresses) {
        addresses = {"10.0.0.2"};
        return true;
    };
    MaiCurlNetworkRoute::setBackend(backend);
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://provider.example/a", false, 1000, [] { return true; }) ==
              CURLE_ABORTED_BY_CALLBACK);
    }
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://office.example/a", false, 1000) == 0);
        CHECK(route.networkHandle() == 0);
    }
    backend.resolve = [](uint64_t, const std::string&, std::vector<std::string>&) { return false; };
    MaiCurlNetworkRoute::setBackend(backend);
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://provider.example/a", false, 1000) ==
              CURLE_COULDNT_RESOLVE_HOST);
    }
    backend.resolve = [](uint64_t, const std::string&, std::vector<std::string>&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return false;
    };
    MaiCurlNetworkRoute::setBackend(backend);
    {
        MaiCurlNetworkRoute route;
        CHECK(route.prepare(curl, "https://provider.example/a", false, 25) ==
              CURLE_OPERATION_TIMEDOUT);
    }
    curl_easy_cleanup(curl);
    MaiCurlNetworkRoute::publish(0, 3);
    MaiCurlNetworkRoute::setBackend({});
    curl_global_cleanup();
    std::cout << "network route contracts passed\n";
}
