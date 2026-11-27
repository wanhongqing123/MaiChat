#include "MaiCurlNetworkRoute.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

#include <curl/curl.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#if defined(__ANDROID__)
#include <pthread.h>
#endif
#endif

namespace {
std::mutex routeMutex;
uint64_t routeNetwork = 0;
uint64_t routeGeneration = 0;
MaiCurlNetworkRoute::Backend routeBackend;
std::atomic<int> resolving{0};

bool isLocalAddress(const std::string& address) {
    if (address.find('%') != std::string::npos)
        return isLocalAddress(address.substr(0, address.find('%')));
    in_addr v4{};
    if (inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        const auto value = ntohl(v4.s_addr);
        return (value >> 24) == 0 || (value >> 24) == 10 || (value >> 24) == 127 ||
               (value >> 20) == 0xac1 || (value >> 16) == 0xc0a8 || (value >> 16) == 0xa9fe ||
               (value >> 22) == 0x191 || (value >> 28) >= 14;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, address.c_str(), &v6) != 1) return false;
    const auto* b = v6.s6_addr;
    bool zeroPrefix = true;
    for (int i = 0; i < 10; ++i) zeroPrefix = zeroPrefix && b[i] == 0;
    if (zeroPrefix && b[10] == 255 && b[11] == 255) {
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, b + 12, text, sizeof(text));
        return isLocalAddress(text);
    }
    bool loopback = true;
    for (int i = 0; i < 15; ++i) loopback = loopback && b[i] == 0;
    return (loopback && b[15] <= 1) || (b[0] & 0xfe) == 0xfc ||
           (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || b[0] == 0xff;
}

bool isLocalHost(std::string host) {
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!host.empty() && host.back() == '.') host.pop_back();
    return (host.find('.') == std::string::npos && host.find(':') == std::string::npos) ||
           host == "localhost" ||
           (host.size() >= 6 && host.compare(host.size() - 6, 6, ".local") == 0) ||
           isLocalAddress(host);
}

struct Resolution {
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    bool success = false;
    std::vector<std::string> addresses;
};

// NDK getaddrinfo 是同步 API。限制并发并在租约线程中等待截止/取消，避免把失效 DNS
// 变成无限等待；后台解析仅持有值，不引用调用方或 curl，完成后自行释放。
int resolveBounded(const MaiCurlNetworkRoute::Backend& backend, uint64_t network,
                   const std::string& host, std::chrono::steady_clock::time_point deadline,
                   const std::function<bool()>& isCanceled, std::vector<std::string>& addresses) {
    if (resolving.fetch_add(1) >= 4) {
        resolving.fetch_sub(1);
        return CURLE_COULDNT_RESOLVE_HOST;
    }
    auto result = std::make_shared<Resolution>();
    std::thread([backend, network, host, result] {
#if defined(__ANDROID__)
        pthread_setname_np(pthread_self(), "MaiNetworkDns");
#endif
        std::vector<std::string> resolved;
        const bool success = backend.resolve(network, host, resolved);
        {
            std::lock_guard<std::mutex> lock(result->mutex);
            result->addresses = std::move(resolved);
            result->success = success;
            result->done = true;
        }
        resolving.fetch_sub(1);
        result->ready.notify_one();
    }).detach();
    std::unique_lock<std::mutex> lock(result->mutex);
    while (!result->done) {
        if (isCanceled && isCanceled()) return CURLE_ABORTED_BY_CALLBACK;
        if (std::chrono::steady_clock::now() >= deadline) return CURLE_OPERATION_TIMEDOUT;
        result->ready.wait_for(lock, std::chrono::milliseconds(25));
    }
    addresses = std::move(result->addresses);
    return result->success && !addresses.empty() ? CURLE_OK : CURLE_COULDNT_RESOLVE_HOST;
}
}  // namespace

struct MaiCurlNetworkRoute::State {
    uint64_t generation = 0;
    uint64_t candidate = 0;
    uint64_t network = 0;
    uint64_t dnsCalls = 0;
    uint64_t socketCalls = 0;
    Backend backend;
    curl_slist* addresses = nullptr;
    ~State() {
        curl_slist_free_all(addresses);
    }
    static int bind(void* context, curl_socket_t socket, curlsocktype purpose) {
        auto* state = static_cast<State*>(context);
        if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_ERROR;
        // 已连上的 SSE 保持租约；但在 DNS 等待期间启用 VPN/限制或换掉候选网络时，
        // 尚未 connect 的 socket 必须失败，不能再绑定此前获准的底层蜂窝。
        std::lock_guard<std::mutex> lock(routeMutex);
        if (routeNetwork != state->network) return CURL_SOCKOPT_ERROR;
        ++state->socketCalls;
        return state->backend.bindSocket(state->network, static_cast<intptr_t>(socket))
                   ? CURL_SOCKOPT_OK
                   : CURL_SOCKOPT_ERROR;
    }
};

MaiCurlNetworkRoute::MaiCurlNetworkRoute() : mState(std::make_unique<State>()) {
    std::lock_guard<std::mutex> lock(routeMutex);
    mState->generation = routeGeneration;
    mState->candidate = routeNetwork;
    mState->backend = routeBackend;
}
MaiCurlNetworkRoute::~MaiCurlNetworkRoute() = default;
void MaiCurlNetworkRoute::publish(uint64_t network, uint64_t generation) {
    std::lock_guard<std::mutex> lock(routeMutex);
    routeNetwork = network;
    routeGeneration = generation;
}
void MaiCurlNetworkRoute::setBackend(Backend backend) {
    std::lock_guard<std::mutex> lock(routeMutex);
    routeBackend = std::move(backend);
}
uint64_t MaiCurlNetworkRoute::generation() {
    std::lock_guard<std::mutex> lock(routeMutex);
    return routeGeneration;
}
uint64_t MaiCurlNetworkRoute::networkHandle() const {
    return mState->network;
}
uint64_t MaiCurlNetworkRoute::capturedGeneration() const {
    return mState->generation;
}
uint64_t MaiCurlNetworkRoute::dnsCalls() const {
    return mState->dnsCalls;
}
uint64_t MaiCurlNetworkRoute::socketCalls() const {
    return mState->socketCalls;
}
bool MaiCurlNetworkRoute::hasChanged() const {
    return mState->generation != generation();
}

int MaiCurlNetworkRoute::prepare(void* nativeHandle, const std::string& url, bool hasProxy,
                                 long timeoutMs, const std::function<bool()>& isCanceled) {
    auto* curl = static_cast<CURL*>(nativeHandle);
    if (isCanceled && isCanceled()) return CURLE_ABORTED_BY_CALLBACK;
    if (!mState->candidate || hasProxy || !mState->backend.resolve || !mState->backend.bindSocket)
        return CURLE_OK;
    // 环境代理仍由 curl 管理；不能因显式绑定绕过现有代理语义。
    for (const char* name :
         {"http_proxy", "https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"}) {
        const char* value = std::getenv(name);
        if (value && *value) return CURLE_OK;
    }
    CURLU* parsed = curl_url();
    if (!parsed) return CURLE_OUT_OF_MEMORY;
    char* hostValue = nullptr;
    char* portValue = nullptr;
    const bool parsedOk =
        curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &hostValue, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PORT, &portValue, CURLU_DEFAULT_PORT) == CURLUE_OK;
    std::string host = hostValue ? hostValue : "";
    const std::string port = portValue ? portValue : "";
    curl_free(hostValue);
    curl_free(portValue);
    curl_url_cleanup(parsed);
    if (!parsedOk) return CURLE_URL_MALFORMAT;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (isLocalHost(host)) return CURLE_OK;
    in_addr numericV4{};
    in6_addr numericV6{};
    const bool numeric = inet_pton(AF_INET, host.c_str(), &numericV4) == 1 ||
                         inet_pton(AF_INET6, host.c_str(), &numericV6) == 1;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 10000);
    std::vector<std::string> addresses;
    int code = CURLE_OK;
    if (!numeric) {
        // 先识别默认网络解析的内网名称；不将企业/LAN FQDN 偷换到蜂窝。
        code = resolveBounded(mState->backend, 0, host,
                              (std::min)(deadline, std::chrono::steady_clock::now() +
                                                       std::chrono::milliseconds(1000)),
                              isCanceled, addresses);
        if (code == CURLE_ABORTED_BY_CALLBACK) return code;
        if (std::any_of(addresses.begin(), addresses.end(), isLocalAddress)) return CURLE_OK;
        addresses.clear();
        ++mState->dnsCalls;
        code = resolveBounded(mState->backend, mState->candidate, host, deadline, isCanceled,
                              addresses);
        if (code != CURLE_OK) return code;
        if (std::any_of(addresses.begin(), addresses.end(), isLocalAddress)) return CURLE_OK;
        std::string entry = host + ":" + port + ":";
        for (const auto& address : addresses) {
            if (entry.back() != ':') entry += ',';
            entry += address.find(':') != std::string::npos ? "[" + address + "]" : address;
        }
        mState->addresses = curl_slist_append(nullptr, entry.c_str());
        if (!mState->addresses) return CURLE_OUT_OF_MEMORY;
    }
    mState->network = mState->candidate;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) return CURLE_OPERATION_TIMEDOUT;
    code = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining));
    if (code != CURLE_OK) return code;
    // 备选路由不复用默认网络建立的 socket，也不把它留给后续默认网络请求。
    for (auto option : {CURLOPT_FRESH_CONNECT, CURLOPT_FORBID_REUSE}) {
        code = curl_easy_setopt(curl, option, 1L);
        if (code != CURLE_OK) return code;
    }
    if (mState->addresses) code = curl_easy_setopt(curl, CURLOPT_RESOLVE, mState->addresses);
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, State::bind);
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, mState.get());
    // 重定向由调用方逐跳重新租用，禁止未知下一域名借用当前网络的普通 DNS。
    if (code == CURLE_OK) code = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    return code;
}
