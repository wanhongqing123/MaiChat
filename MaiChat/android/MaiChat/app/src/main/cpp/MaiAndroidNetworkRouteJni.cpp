// 文件作用：把 Android 的网络句柄及 NDK DNS/socket 操作接到唯一的 curl 网络租约核心。
#include "MaiCurlNetworkRoute.h"

#include <mutex>
#include <string>
#include <vector>

#include <android/multinetwork.h>
#include <arpa/inet.h>
#include <jni.h>
#include <netdb.h>
#include <curl/curl.h>

extern "C" JNIEXPORT void JNICALL Java_com_kongshang_maichat_MaiAndroidNetworkRouter_nativePublish(JNIEnv*, jclass,
                                                                                                   jlong network,
                                                                                                   jlong generation) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        MaiCurlNetworkRoute::Backend backend;
        backend.resolve = [](uint64_t network, const std::string& host, std::vector<std::string>& addresses) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            if (android_getaddrinfofornetwork(network, host.c_str(), nullptr, &hints, &result) != 0) return false;
            for (auto* address = result; address; address = address->ai_next) {
                char text[INET6_ADDRSTRLEN] = {};
                const void* bytes =
                    address->ai_family == AF_INET
                        ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(address->ai_addr)->sin_addr)
                    : address->ai_family == AF_INET6
                        ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(address->ai_addr)->sin6_addr)
                        : nullptr;
                if (bytes && inet_ntop(address->ai_family, bytes, text, sizeof(text))) addresses.emplace_back(text);
            }
            freeaddrinfo(result);
            return !addresses.empty();
        };
        backend.bindSocket = [](uint64_t network, intptr_t socket) {
            return android_setsocknetwork(network, static_cast<int>(socket)) == 0;
        };
        MaiCurlNetworkRoute::setBackend(std::move(backend));
    });
    MaiCurlNetworkRoute::publish(static_cast<uint64_t>(network), static_cast<uint64_t>(generation));
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_kongshang_maichat_MaiAndroidNetworkRouter_nativeProbe(JNIEnv* env, jclass, jstring url, jstring caBundlePath) {
    if (!url || !caBundlePath) return nullptr;
    const char* text = env->GetStringUTFChars(url, nullptr);
    if (!text) return nullptr;
    const std::string target(text);
    env->ReleaseStringUTFChars(url, text);
    const char* caText = env->GetStringUTFChars(caBundlePath, nullptr);
    if (!caText) return nullptr;
    const std::string caPath(caText);
    env->ReleaseStringUTFChars(caBundlePath, caText);
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;
    MaiCurlNetworkRoute route;
    curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t size, size_t count, void*) -> size_t { return size * count; });
    int code = route.prepare(curl, target, false, 5000);
    if (code == CURLE_OK) code = curl_easy_perform(curl);
    jlong values[] = {code, static_cast<jlong>(route.networkHandle()), static_cast<jlong>(route.dnsCalls()),
                      static_cast<jlong>(route.socketCalls()), route.hasChanged() ? 1 : 0};
    curl_easy_cleanup(curl);
    jlongArray result = env->NewLongArray(5);
    if (result) env->SetLongArrayRegion(result, 0, 5, values);
    return result;
}
