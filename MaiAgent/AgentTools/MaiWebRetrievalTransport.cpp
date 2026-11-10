// 文件作用：共用供应商检索的 TLS、取消、有界接收和有限重试，避免两套传输漂移。
#include "MaiWebRetrievalTransport.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "MaiBlockingCheck.h"

namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaximumBody = 2 * 1024 * 1024;
constexpr std::size_t kMaximumToolOutput = 256 * 1024;

bool canceled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

// JSON解析器负责严格UTF-8验证；此函数仅在验证后按Unicode码点计数/截断。
std::size_t characterCount(const std::string& value) {
    return std::count_if(value.begin(), value.end(),
                         [](unsigned char c) { return (c & 0xc0) != 0x80; });
}

bool validText(const std::string& value, std::size_t maximum, bool required) {
    if (value.find('\0') != std::string::npos || value.size() > maximum * 4 ||
        characterCount(value) > maximum)
        return false;
    try {
        (void)Json(value).dump();
    } catch (...) {
        return false;
    }
    if (!required) return true;
    // Unicode White_Space，不能让全角空格/不换行空格绕过“非空查询”校验。
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index++]);
        std::uint32_t point = first;
        unsigned remaining = 0;
        if (first >= 0xf0) {
            point &= 7;
            remaining = 3;
        } else if (first >= 0xe0) {
            point &= 15;
            remaining = 2;
        } else if (first >= 0xc0) {
            point &= 31;
            remaining = 1;
        }
        while (remaining--)
            point = (point << 6) | (static_cast<unsigned char>(value[index++]) & 63);
        if (!((point >= 9 && point <= 13) || point == 0x20 || point == 0x85 || point == 0xa0 ||
              point == 0x1680 || (point >= 0x2000 && point <= 0x200a) || point == 0x2028 ||
              point == 0x2029 || point == 0x202f || point == 0x205f || point == 0x3000))
            return true;
    }
    return false;
}

std::string shorten(const std::string& text, std::size_t characters, bool& truncated) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0) == 0x80) continue;
        if (count++ == characters) {
            truncated = true;
            return text.substr(0, index);
        }
    }
    return text;
}

bool httpUrl(const std::string& value, bool loopbackOnly = false) {
    // curl 的 URL 解析不保证 UTF-8 合法；在读凭据或 JSON 编码之前拒绝普通 API 的非法字节。
    if (value.empty() || value.size() > 8192 || !validText(value, 8192, false)) return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char *scheme = nullptr, *host = nullptr, *user = nullptr, *password = nullptr;
    bool valid = curl_url_set(parsed, CURLUPART_URL, value.c_str(), 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host &&
                 (std::string(scheme) == "https" || std::string(scheme) == "http") &&
                 curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                 curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    if (valid && loopbackOnly)
        valid = (std::string(scheme) == "http" || std::string(scheme) == "https") &&
                (std::string(host) == "127.0.0.1" || std::string(host) == "[::1]" ||
                 std::string(host) == "::1");
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(parsed);
    return valid;
}

MaiWebRetrievalError failure(std::string code, std::string reason, long status = 0,
                             unsigned attempts = 0, bool retryable = false) {
    return {std::move(code), std::move(reason), status, attempts, retryable};
}

bool transientStatus(long status) {
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504;
}

MaiWebRetrievalError httpFailure(long status, unsigned attempts) {
    const bool retryable = transientStatus(status);
    switch (status) {
        case 400:
            return failure("INVALID_REQUEST", "Provider rejected the request parameters", status,
                           attempts);
        case 401:
            return failure("AUTHENTICATION_FAILED", "Provider API credential is missing or invalid",
                           status, attempts);
        case 402:
            return failure("INSUFFICIENT_BALANCE",
                           "Provider account balance is insufficient; do not retry before funding",
                           status, attempts);
        case 403:
            return failure("PROVIDER_SECURITY_POLICY",
                           "Provider rejected this fetch under its security policy; changing the "
                           "request is required",
                           status, attempts);
        case 404:
            return failure("FETCH_UNAVAILABLE",
                           "Provider could not extract usable page content: unavailable URL, site "
                           "policy, origin error, unsupported content or empty page; exact origin "
                           "cause is not supplied",
                           status, attempts);
        case 429:
            return failure("RATE_LIMITED", "Provider retrieval rate limit exceeded", status,
                           attempts, true);
        default:
            return failure("HTTP_ERROR",
                           "Provider retrieval API returned an unsuccessful HTTP status", status,
                           attempts, retryable);
    }
}

struct Body {
    std::string bytes;
    bool oversized = false;
    const std::atomic<bool>* cancel = nullptr;
};

std::size_t receive(char* data, std::size_t size, std::size_t count, void* opaque) {
    auto& body = *static_cast<Body*>(opaque);
    if (size && count > kMaximumBody / size) {
        body.oversized = true;
        return 0;
    }
    const auto length = size * count;
    if (length > kMaximumBody - body.bytes.size()) {
        body.oversized = true;
        return 0;
    }
    body.bytes.append(data, length);
    return length;
}

int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return canceled(static_cast<Body*>(opaque)->cancel) ? 1 : 0;
}

}  // namespace

bool maiWebRetrievalValidText(const std::string& value, std::size_t maximum, bool required) {
    return validText(value, maximum, required);
}
std::string maiWebRetrievalShorten(const std::string& text, std::size_t characters,
                                   bool& truncated) {
    return shorten(text, characters, truncated);
}
bool maiWebRetrievalHttpUrl(const std::string& value) {
    return httpUrl(value);
}

MaiWebRetrievalTransportResponse maiWebRetrievalPost(const MaiWebRetrievalTransportOptions& options,
                                                     const std::string& endpoint,
                                                     const std::string& requestBody,
                                                     const std::atomic<bool>* cancel) {
    maiAssertBlockingAllowed("MaiWebRetrieval");
    MaiWebRetrievalTransportResponse result;
    if (canceled(cancel)) {
        result.error = failure("CANCELED", "Retrieval was canceled");
        return result;
    }
    std::string base = options.baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if ((options.officialBaseUrl != "https://api.deepseek.com" &&
         options.officialBaseUrl != "https://open.bigmodel.cn/api/paas/v4") ||
        (base != options.officialBaseUrl &&
         !(options.allowLoopbackTestServer && httpUrl(base, true))) ||
        options.timeoutSeconds < 1 || options.timeoutSeconds > 120) {
        result.error =
            failure("INVALID_CONFIGURATION", "Retrieval requires the official HTTPS API endpoint");
        return result;
    }
    std::string key;
    try {
        if (options.apiKey) key = options.apiKey();
    } catch (...) {
    }
    if (key.empty() || key.size() > 8192 || key.find_first_of("\r\n") != std::string::npos ||
        key.find('\0') != std::string::npos) {
        result.error = failure("NOT_CONFIGURED",
                               "Provider retrieval credential is not available for this account");
        return result;
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    const std::string url = base + endpoint;
    const std::string& encoded = requestBody;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        if (canceled(cancel)) {
            result.error = failure("CANCELED", "Retrieval was canceled", 0, result.attempts);
            return result;
        }
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(),
                                                                   curl_easy_cleanup);
        if (!handle) {
            result.error = failure("INITIALIZATION_FAILED", "HTTP client could not be initialized");
            return result;
        }
        Body body;
        body.cancel = cancel;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        const std::string authorization = "Authorization: Bearer " + key;
        headers = curl_slist_append(headers, authorization.c_str());
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> cleanup(headers,
                                                                            curl_slist_free_all);
        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, encoded.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(encoded.size()));
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, static_cast<long>(options.timeoutSeconds));
        curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &body);
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_blob caBlob{const_cast<char*>(options.caPem.data()), options.caPem.size(),
                         CURL_BLOB_COPY};
        if (!options.caPem.empty())
            curl_easy_setopt(handle.get(), CURLOPT_CAINFO_BLOB, &caBlob);
        else if (!options.caBundlePath.empty())
            curl_easy_setopt(handle.get(), CURLOPT_CAINFO, options.caBundlePath.c_str());
        ++result.attempts;
        const CURLcode status = curl_easy_perform(handle.get());
        long httpStatus = 0;
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &httpStatus);
        if (canceled(cancel) || status == CURLE_ABORTED_BY_CALLBACK) {
            result.error = failure("CANCELED", "Retrieval was canceled", httpStatus, attempt);
            return result;
        }
        if (body.oversized) {
            result.error =
                failure("RESPONSE_TOO_LARGE", "Provider response exceeded the 2 MiB limit",
                        httpStatus, attempt);
            return result;
        }
        bool retry = false;
        if (status != CURLE_OK) {
            retry = status == CURLE_OPERATION_TIMEDOUT || status == CURLE_COULDNT_RESOLVE_HOST ||
                    status == CURLE_COULDNT_CONNECT || status == CURLE_RECV_ERROR;
            const char* code =
                status == CURLE_OPERATION_TIMEDOUT     ? "NETWORK_TIMEOUT"
                : status == CURLE_COULDNT_RESOLVE_HOST ? "DNS_FAILURE"
                : status == CURLE_PEER_FAILED_VERIFICATION || status == CURLE_SSL_CONNECT_ERROR
                    ? "TLS_FAILURE"
                    : "NETWORK_UNREACHABLE";
            result.error =
                failure(code, "Could not complete the connection to the Provider retrieval API",
                        httpStatus, attempt, retry);
        } else if (httpStatus != 200) {
            result.error = httpFailure(httpStatus, attempt);
            retry = result.error.retryable;
        } else {
            result.body = std::move(body.bytes);
            result.error = {};
            return result;
        }
        if (!retry || attempt == 2) return result;
        // 等待仅两百毫秒且定点检查取消，不占UI线程、不实现无限重试。
        for (int index = 0; index < 10 && !canceled(cancel); ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return result;
}
