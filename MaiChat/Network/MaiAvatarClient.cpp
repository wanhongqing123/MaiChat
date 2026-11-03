#include "MaiAvatarClient.h"
#include "MaiFirstPartyRouteInternal.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

struct MaiAvatarCancel {
    std::atomic_bool requested{false};
};

namespace {

using Json = nlohmann::json;
constexpr size_t kMaximumBytes = 2'000'000;
constexpr size_t kMaximumResponse = 32 * 1024;

bool canceled(const MaiAvatarCancel* token) {
    return token && token->requested.load(std::memory_order_relaxed);
}

bool text(const std::string& value, size_t maximum, bool emptyAllowed = false) {
    return value.size() <= maximum && (emptyAllowed || !value.empty()) &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}

struct Url {
    std::string host;
    std::string port;
    std::string path;
    bool query = false;
};

// Parse authority and path with libcurl; credentials and fragments are never
// legal on any of the three URLs, including the temporary signed OSS endpoint.
bool parseHttps(const std::string& raw, Url* output) {
    if (!text(raw, 16384)) return false;
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, raw.c_str(), 0) != CURLUE_OK) return false;
    const auto field = [&](CURLUPart part, std::string* value) {
        char* buffer = nullptr;
        const auto code = curl_url_get(url.get(), part, &buffer, 0);
        if (code == CURLUE_OK && buffer) *value = buffer;
        curl_free(buffer);
        return code == CURLUE_OK;
    };
    std::string scheme, host, port, path, query, unwanted;
    if (!field(CURLUPART_SCHEME, &scheme) || scheme != "https" || !field(CURLUPART_HOST, &host) || host.empty() ||
        !field(CURLUPART_PATH, &path) || field(CURLUPART_USER, &unwanted) || field(CURLUPART_PASSWORD, &unwanted) ||
        field(CURLUPART_FRAGMENT, &unwanted))
        return false;
    field(CURLUPART_PORT, &port);
    const bool hasQuery = field(CURLUPART_QUERY, &query);
    if (hasQuery && query.empty()) return false;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    *output = {host, port.empty() ? "443" : port, path, hasQuery};
    return true;
}

bool accountId(const char* raw) {
    if (!raw) return false;
    const std::string value(raw);
    return value.size() >= 3 && value.size() <= 32 && value[0] >= 'a' && value[0] <= 'z' &&
           std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

bool hostMatches(const std::string& name, const char* rawSuffix) {
    std::string suffix = rawSuffix ? rawSuffix : "aliyuncs.com";
    if (suffix.empty() || suffix.size() > 253 || !std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '.';
        }))
        return false;
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return name == suffix ||
           (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            name[name.size() - suffix.size() - 1] == '.');
}

struct Response {
    std::string body;
    size_t headerBytes = 0;
    bool tooLarge = false;
    const MaiAvatarCancel* cancel = nullptr;
};

size_t writeBody(char* data, size_t size, size_t count, void* context) {
    auto& response = *static_cast<Response*>(context);
    if (canceled(response.cancel) || (size && count > kMaximumResponse / size)) return 0;
    const auto bytes = size * count;
    if (bytes > kMaximumResponse - response.body.size()) {
        response.tooLarge = true;
        return 0;
    }
    try {
        response.body.append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}

size_t readHeader(char*, size_t size, size_t count, void* context) {
    auto& response = *static_cast<Response*>(context);
    if (size && count > 16384 / size) return 0;
    const auto bytes = size * count;
    if (bytes > 16384 - response.headerBytes) return 0;
    response.headerBytes += bytes;
    return bytes;
}

int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return canceled(static_cast<const MaiAvatarCancel*>(context)) ? 1 : 0;
}

bool perform(const MaiAvatarOptions& options, const std::string& url, const unsigned char* data, size_t size,
             const char* contentType, bool control, const MaiAvatarCancel* cancel, Response* response,
             MaiAvatarResult* result) {
    if (canceled(cancel)) {
        result->status = MAI_AVATAR_CANCELED;
        return false;
    }
    static std::once_flag once;
    static CURLcode initialized = CURLE_FAILED_INIT;
    std::call_once(once, [] { initialized = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initialized != CURLE_OK) {
        result->status = MAI_AVATAR_NETWORK_ERROR;
        result->curl_code = initialized;
        return false;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
        result->status = MAI_AVATAR_NETWORK_ERROR;
        result->curl_code = CURLE_FAILED_INIT;
        return false;
    }
    curl_slist* rawHeaders = nullptr;
    const auto append = [&](const std::string& value) {
        auto* next = curl_slist_append(rawHeaders, value.c_str());
        if (!next) return false;
        rawHeaders = next;
        return true;
    };
    bool headersReady = append(std::string("Content-Type: ") + contentType) && append("Expect:");
    if (headersReady && control)
        headersReady = append(std::string("Authorization: Bearer ") + options.account_token_utf8);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
    if (!headersReady) {
        result->status = MAI_AVATAR_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return false;
    }
    response->cancel = cancel;
    CURLcode setup = CURLE_OK;
    const auto set = [&](CURLoption option, auto value) {
        if (setup == CURLE_OK) setup = curl_easy_setopt(curl.get(), option, value);
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_CONNECTTIMEOUT_MS, long(options.connect_timeout_ms > 0 ? options.connect_timeout_ms : 10000));
    set(CURLOPT_TIMEOUT_MS, long(options.transfer_timeout_ms > 0 ? options.transfer_timeout_ms : 60000));
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_CUSTOMREQUEST, control ? "POST" : "PUT");
    set(CURLOPT_POSTFIELDS, reinterpret_cast<const char*>(data));
    set(CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(size));
    set(CURLOPT_WRITEFUNCTION, writeBody);
    set(CURLOPT_WRITEDATA, response);
    set(CURLOPT_HEADERFUNCTION, readHeader);
    set(CURLOPT_HEADERDATA, response);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, progress);
    set(CURLOPT_XFERINFODATA, cancel);
    if (options.ca_path_utf8 && *options.ca_path_utf8) set(CURLOPT_CAINFO, options.ca_path_utf8);
    curl_blob ca{const_cast<char*>(options.ca_pem), options.ca_pem_length, CURL_BLOB_COPY};
    if (options.ca_pem && options.ca_pem_length) set(CURLOPT_CAINFO_BLOB, &ca);

    MaiDownloadProxy selected{options.proxy_url_utf8, options.proxy_username_utf8, options.proxy_password_utf8};
    if (options.proxy_resolver && options.proxy_resolver(url.c_str(), options.proxy_context, &selected) != 1) {
        result->status = MAI_AVATAR_NETWORK_ERROR;
        result->curl_code = CURLE_COULDNT_RESOLVE_PROXY;
        return false;
    }
    const std::string proxy = selected.url_utf8 ? selected.url_utf8 : "";
    const std::string user = selected.username_utf8 ? selected.username_utf8 : "";
    const std::string password = selected.password_utf8 ? selected.password_utf8 : "";
    if (selected.url_utf8) {
        set(CURLOPT_PROXY, proxy.c_str());
        set(CURLOPT_NOPROXY, proxy.empty() ? "*" : "");
    }
    if (selected.username_utf8) set(CURLOPT_PROXYUSERNAME, user.c_str());
    if (selected.password_utf8) set(CURLOPT_PROXYPASSWORD, password.c_str());
    if (control && options.first_party_route_selector && maiFirstPartyRoute::eligible(url, true)) {
        const int route =
            options.first_party_route_selector(url.c_str(), options.ca_path_utf8, options.first_party_route_context);
        if (route < 0)
            setup = CURLE_COULDNT_RESOLVE_HOST;
        else if (setup == CURLE_OK)
            setup = maiFirstPartyRoute::apply(curl.get(), route);
    }
    result->curl_code = setup == CURLE_OK ? curl_easy_perform(curl.get()) : setup;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result->http_status);
    if (canceled(cancel))
        result->status = MAI_AVATAR_CANCELED;
    else if (response->tooLarge)
        result->status = MAI_AVATAR_INVALID_RESPONSE;
    else if (result->curl_code != CURLE_OK)
        result->status = MAI_AVATAR_NETWORK_ERROR;
    else if (result->http_status < 200 || result->http_status >= 300)
        result->status = MAI_AVATAR_HTTP_ERROR;
    else
        result->status = MAI_AVATAR_OK;
    if (control && result->status == MAI_AVATAR_NETWORK_ERROR && options.first_party_route_invalidator)
        options.first_party_route_invalidator(options.first_party_route_context);
    return result->status == MAI_AVATAR_OK;
}

void upload(const MaiAvatarOptions* options, const unsigned char* data, size_t size, const char* contentType,
            const MaiAvatarCancel* cancel, MaiAvatarResult* result) {
    if (canceled(cancel)) {
        result->status = MAI_AVATAR_CANCELED;
        return;
    }
    if (!options || !data || size == 0 || size > kMaximumBytes || !contentType ||
        !accountId(options->account_id_utf8) || !options->account_token_utf8 ||
        !text(options->account_token_utf8, 8192))
        return;
    const bool jpeg = std::strcmp(contentType, "image/jpeg") == 0;
    const bool png = std::strcmp(contentType, "image/png") == 0;
    if ((!jpeg && !png) || (jpeg && (size < 3 || data[0] != 0xff || data[1] != 0xd8 || data[2] != 0xff)) ||
        (png && (size < 8 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0)))
        return;
    std::string base = options->media_base_url_utf8 ? options->media_base_url_utf8 : "";
    while (!base.empty() && base.back() == '/') base.pop_back();
    Url origin;
    if (!parseHttps(base, &origin) || origin.query || origin.path.find('%') != std::string::npos ||
        origin.path.find("..") != std::string::npos)
        return;
    const std::string request =
        Json{{"filename", jpeg ? "avatar.jpg" : "avatar.png"}, {"size_bytes", size}, {"purpose", "im_avatar"}}.dump();
    Response signedResponse;
    if (!perform(*options, base + "/sign-upload", reinterpret_cast<const unsigned char*>(request.data()),
                 request.size(), "application/json", true, cancel, &signedResponse, result))
        return;
    const Json signedUpload = Json::parse(signedResponse.body, nullptr, false);
    if (!signedUpload.is_object()) {
        result->status = MAI_AVATAR_INVALID_RESPONSE;
        return;
    }
    const auto stringField = [&](const char* key) {
        return signedUpload.contains(key) && signedUpload[key].is_string() ? signedUpload[key].get<std::string>()
                                                                           : std::string{};
    };
    const auto object = stringField("object_key");
    const auto avatar = stringField("avatar_url");
    const auto target = stringField("upload_url");
    const std::string prefix = std::string("avatars/") + options->account_id_utf8 + "/";
    Url avatarUrl, uploadUrl;
    const auto validHeaders = signedUpload.find("upload_headers");
    if (object.compare(0, prefix.size(), prefix) != 0 || object.size() > 256 ||
        object.find("..") != std::string::npos || object.find('%') != std::string::npos ||
        object.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-/.") != std::string::npos ||
        object.size() < 4 || object.substr(object.size() - 4) != (jpeg ? ".jpg" : ".png") ||
        !parseHttps(avatar, &avatarUrl) || avatarUrl.query || avatarUrl.host != origin.host ||
        avatarUrl.port != origin.port || avatar != base + "/" + object || !parseHttps(target, &uploadUrl) ||
        !uploadUrl.query || !hostMatches(uploadUrl.host, options->signed_upload_host_suffix_utf8) ||
        uploadUrl.path != "/" + object || validHeaders == signedUpload.end() || !validHeaders->is_object() ||
        validHeaders->size() != 1 || !validHeaders->contains("Content-Type") ||
        (*validHeaders)["Content-Type"] != contentType) {
        result->status = MAI_AVATAR_INVALID_RESPONSE;
        return;
    }
    Response uploaded;
    if (!perform(*options, target, data, size, contentType, false, cancel, &uploaded, result)) return;
    result->avatar_url_utf8 = static_cast<char*>(std::malloc(avatar.size() + 1));
    if (!result->avatar_url_utf8) {
        result->status = MAI_AVATAR_NETWORK_ERROR;
        result->curl_code = CURLE_OUT_OF_MEMORY;
        return;
    }
    std::memcpy(result->avatar_url_utf8, avatar.c_str(), avatar.size() + 1);
}

}  // namespace

MaiAvatarCancel* mai_avatar_cancel_create(void) {
    return new (std::nothrow) MaiAvatarCancel;
}
void mai_avatar_cancel_request(MaiAvatarCancel* cancel) {
    if (cancel) cancel->requested.store(true, std::memory_order_relaxed);
}
void mai_avatar_cancel_destroy(MaiAvatarCancel* cancel) {
    delete cancel;
}
void mai_avatar_upload(const MaiAvatarOptions* options, const unsigned char* data, size_t size, const char* contentType,
                       const MaiAvatarCancel* cancel, MaiAvatarResult* result) {
    if (!result) return;
    *result = {MAI_AVATAR_INVALID_ARGUMENT, 0, 0, nullptr};
    try {
        upload(options, data, size, contentType, cancel, result);
    } catch (...) {
        mai_avatar_result_free(result);
        result->status = MAI_AVATAR_INVALID_RESPONSE;
    }
}
void mai_avatar_result_free(MaiAvatarResult* result) {
    if (!result) return;
    std::free(result->avatar_url_utf8);
    result->avatar_url_utf8 = nullptr;
}
