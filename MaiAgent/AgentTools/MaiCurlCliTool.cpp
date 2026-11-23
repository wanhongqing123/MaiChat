// 文件作用：将 curl 命令行能力适配为 Agent 工具。
// 直接依赖：MaiBlockingCheck.h、MaiFilePath.h、MaiFileSystem.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiCurlCliTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiWebFetchTool.h"
#include "mai_curl_embed.h"

namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kMegabyte = 1024 * 1024;
constexpr std::uint64_t kMaximumTextBytes = 256 * 1024;
constexpr std::uint64_t kMaximumHeaderBytes = 64 * 1024;
constexpr std::size_t kMaximumLogBytes = 16 * 1024;
constexpr std::size_t kMaximumFormFields = 12;
constexpr const char* kSupportedArguments =
    "Supported: one HTTP(S) URL/--url, -X/--request, -H/--header, -d/--data "
    "(literal or @workspace-file), -T/--upload-file, -F/--form (name=value or "
    "name=@workspace-file), --form-string, -A/--user-agent, -b/--cookie (string or "
    "workspace file), -c/--cookie-jar (new workspace file), -L/--location, "
    "--max-redirs (0..5), -I/--head, --compressed, -s/--silent, -i/--include, "
    "-v/--verbose, -S/--show-error. Redirects require -L; cross-origin hops drop "
    "custom headers, user-agent and explicit cookies. Verbose/include keep structured output.";
std::mutex sCurlCommandMutex;

struct TemporaryFiles {
    std::vector<MaiFilePath> paths;
    ~TemporaryFiles() {
        for (const MaiFilePath& path : paths) MaiFileSystem::removeFile(path);
    }
};

bool isHttpUrl(const std::string& text) {
    if (text.empty() || text.size() > 8192 || text.find_first_of("{}[]") != std::string::npos ||
        std::any_of(text.begin(), text.end(),
                    [](unsigned char ch) { return ch <= 32 || ch == 127; }))
        return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char* scheme = nullptr;
    char* host = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    const bool valid = curl_url_set(parsed, CURLUPART_URL, text.c_str(), 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && host &&
                       *host && (std::string(scheme) == "http" || std::string(scheme) == "https") &&
                       curl_url_get(parsed, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK;
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (user) curl_free(user);
    if (password) curl_free(password);
    curl_url_cleanup(parsed);
    return valid;
}

bool validHeader(const std::string& header) {
    const std::size_t colon = header.find(':');
    if (header.empty() || header.size() > 4096 || colon == std::string::npos || colon == 0 ||
        header.front() == '@')
        return false;
    for (std::size_t index = 0; index < colon; ++index) {
        const unsigned char ch = header[index];
        if (!std::isalnum(ch) && std::string("!#$%&'*+-.^_`|~").find(ch) == std::string::npos)
            return false;
    }
    return std::none_of(header.begin(), header.end(),
                        [](unsigned char ch) { return ch == '\r' || ch == '\n' || ch == 0; });
}

bool hasControls(const std::string& value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char ch) { return ch < 32 || ch == 127; });
}

// 间接文件引用仍受宿主授权和工作区边界双重约束，不能被 CLI 再解释为 stdin。
std::string workspacePath(const MaiToolContext& context, const std::string& value) {
    if (value.empty() || value == "-" || value.size() > 8192 || hasControls(value)) return {};
    const std::string resolved = context.resolvePath(value);
    return resolved.empty() ? std::string{} : maiResolvePathWithinRoot(context.root, resolved);
}

bool boundedFile(const std::string& path, std::uint64_t maximum, std::uint64_t& size) {
    return !path.empty() && MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), size) &&
           size <= maximum;
}

// 用 libcurl 自身解析相对 Location 和 origin；协议限制不能阻止自定义凭据头跨域。
std::string resolvedRedirect(const std::string& base, const std::string& location) {
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    char* destination = nullptr;
    std::string result;
    if (curl_url_set(parsed, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
        curl_url_set(parsed, CURLUPART_URL, location.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_URL, &destination, 0) == CURLUE_OK)
        result = destination;
    curl_free(destination);
    curl_url_cleanup(parsed);
    return isHttpUrl(result) ? result : std::string{};
}

std::string urlOrigin(const std::string& url) {
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    std::string origin;
    if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        for (CURLUPart part : {CURLUPART_SCHEME, CURLUPART_HOST, CURLUPART_PORT}) {
            char* value = nullptr;
            if (curl_url_get(parsed, part, &value, CURLU_DEFAULT_PORT) != CURLUE_OK) {
                origin.clear();
                break;
            }
            origin += std::string(value) + "\n";
            curl_free(value);
        }
    }
    curl_url_cleanup(parsed);
    std::transform(origin.begin(), origin.end(), origin.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return origin;
}

int checkCanceled(void* opaque) {
    const auto* context = static_cast<const MaiToolContext*>(opaque);
    return context && context->isCanceled() ? 1 : 0;
}

std::string readLog(FILE* file, bool& truncated) {
    if (!file) return {};
    std::rewind(file);
    std::string result(kMaximumLogBytes + 1, '\0');
    const std::size_t count = std::fread(result.data(), 1, result.size(), file);
    truncated = count > kMaximumLogBytes;
    result.resize(std::min(count, kMaximumLogBytes));
    return result;
}

struct ResponseHeaders {
    int status = 0;
    Json fields = Json::object();
};

ResponseHeaders parseResponseHeaders(const std::string& headers) {
    ResponseHeaders response;
    std::size_t offset = 0;
    while (offset < headers.size()) {
        const std::size_t end = headers.find('\n', offset);
        const std::string line = headers.substr(offset, end - offset);
        if (line.rfind("HTTP/", 0) == 0) {
            response.fields = Json::object();
            const std::size_t separator = line.find(' ');
            if (separator != std::string::npos && separator + 4 <= line.size() &&
                std::isdigit(static_cast<unsigned char>(line[separator + 1])) &&
                std::isdigit(static_cast<unsigned char>(line[separator + 2])) &&
                std::isdigit(static_cast<unsigned char>(line[separator + 3])))
                response.status = (line[separator + 1] - '0') * 100 +
                                  (line[separator + 2] - '0') * 10 + (line[separator + 3] - '0');
        } else {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos && colon < 128) {
                std::string key = line.substr(0, colon);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (key == "content-type" || key == "content-length" || key == "location" ||
                    key == "etag" || key == "last-modified" || key == "cache-control") {
                    const std::size_t first = line.find_first_not_of(" \t", colon + 1);
                    if (first != std::string::npos) {
                        std::string value = line.substr(first);
                        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' ||
                                                  value.back() == '\t'))
                            value.pop_back();
                        response.fields[key] = value;
                    }
                }
            }
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return response;
}

std::string encodeBase64(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto first = static_cast<unsigned char>(input[index]);
        const auto second =
            index + 1 < input.size() ? static_cast<unsigned char>(input[index + 1]) : 0;
        const auto third =
            index + 2 < input.size() ? static_cast<unsigned char>(input[index + 2]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 3) << 4) | (second >> 4)]);
        output.push_back(index + 1 < input.size() ? alphabet[((second & 15) << 2) | (third >> 6)]
                                                  : '=');
        output.push_back(index + 2 < input.size() ? alphabet[third & 63] : '=');
    }
    return output;
}

class CurlCliTool final : public MaiTool {
public:
    explicit CurlCliTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}

    std::string name() const override {
        return "curl";
    }
    std::string description() const override {
        // 这是受限的进程内 curl，不是任意命令行：只放行已列明的 HTTP 参数，
        // 上传文件必须来自工作区；输出和日志都有上限。用途是文件传输、API/自建后台通信、
        // HTTP状态/响应头与连通性诊断；公开网页搜索改走专用web_search，不抓搜索引擎HTML。
        return "Transfer files, call HTTP APIs (including the user's own backend), and inspect "
               "connectivity, HTTP status and headers using the vendored in-process curl. "
               "For web search use web_search; for article extraction use web_fetch. Do not use "
               "curl to scrape search-engine result pages or bypass anti-bot challenges. Supported "
               "arguments are bounded and listed below. Use output_path for a new downloaded file; "
               "otherwise "
               "a bounded response body is returned. Set text_only=true to extract readable "
               "text from HTML. Approval follows the Agent policy; full access does not prompt. " +
               std::string(kSupportedArguments);
    }
    std::string parametersSchema() const override {
        // arguments 是受限 curl 参数列表；output_path 指向新下载文件，text_only
        // 控制 HTML 文本化；max_size_mb 和 timeout_s 分别限制返回体及等待时间。
        return R"({"type":"object","properties":{"arguments":{"type":"array",)"
               R"("items":{"type":"string"},"minItems":1,"maxItems":32},"output_path":{"type":"string)"
               R"("},"text_only":{"type":"boolean"},"max_size_mb":{"type":"integer","minimum":1,)"
               R"("maximum":1024},"timeout_s":{"type":"integer","minimum":1,"maximum":600}},)"
               R"("required":["arguments"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return false;
    }

    std::string approvalKey(const std::string& raw) const override {
        const Json arguments = Json::parse(raw, nullptr, false);
        return "curl:" + (arguments.is_discarded() ? raw : arguments.dump());
    }

    // 解析模型提供的参数并执行受工具权限约束的 curl 请求。
    // 成功和失败都转成明确的工具结果；中间状态不能冒充最终产物。
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json request = Json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.value("arguments", Json{}).is_array() ||
            request["arguments"].empty() || request["arguments"].size() > 32 ||
            (request.contains("output_path") && !request["output_path"].is_string()) ||
            (request.contains("text_only") && !request["text_only"].is_boolean()) ||
            (request.contains("max_size_mb") && !request["max_size_mb"].is_number_integer()) ||
            (request.contains("timeout_s") && !request["timeout_s"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid curl arguments");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "Agent workspace is unavailable");
        const int maximumMb = request.value("max_size_mb", 100);
        const int timeout = request.value("timeout_s", 60);
        if (maximumMb < 1 || maximumMb > 1024 || timeout < 1 || timeout > 600)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "curl size or timeout is out of range");

        std::vector<std::string> options;
        std::vector<std::string> customHeaders;
        std::vector<std::string> cookieOptions;
        std::vector<std::string> bodyOptions;
        std::string url;
        std::string method;
        std::string cookieJar;
        bool hasData = false;
        bool hasUpload = false;
        bool hasHead = false;
        bool follow = false;
        int maximumRedirects = 5;
        std::size_t formFields = 0;
        std::uint64_t formBytes = 0;
        const Json& arguments = request["arguments"];
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!arguments[index].is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "every curl argument must be a string");
            const std::string option = arguments[index].get<std::string>();
            if (option.empty() || option.size() > 8192 || option.find('\0') != std::string::npos)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid curl argument");
            if (option == "--url" || option == "-X" || option == "--request" || option == "-H" ||
                option == "--header" || option == "-d" || option == "--data" || option == "-T" ||
                option == "--upload-file" || option == "-F" || option == "--form" ||
                option == "--form-string" || option == "-A" || option == "--user-agent" ||
                option == "-b" || option == "--cookie" || option == "-c" ||
                option == "--cookie-jar" || option == "--max-redirs") {
                if (++index >= arguments.size() || !arguments[index].is_string())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "curl option needs one string value");
                const std::string value = arguments[index].get<std::string>();
                if (value.find('\0') != std::string::npos || value.size() > kMaximumTextBytes)
                    return MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "curl option value contains NUL or exceeds 256 KiB");
                if (option == "--url") {
                    if (!url.empty())
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "only one URL is allowed");
                    url = value;
                } else if (option == "-X" || option == "--request") {
                    if (!method.empty() ||
                        (value != "GET" && value != "HEAD" && value != "POST" && value != "PUT" &&
                         value != "PATCH" && value != "DELETE"))
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "unsupported HTTP method");
                    method = value;
                } else if (option == "-H" || option == "--header") {
                    if (!validHeader(value))
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "invalid HTTP header");
                    customHeaders.insert(customHeaders.end(), {"--header", value});
                } else if (option == "-d" || option == "--data") {
                    if (hasData || value.size() > 256 * 1024)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "literal request body is too large");
                    hasData = true;
                    if (!value.empty() && value.front() == '@') {
                        const std::string source = workspacePath(context, value.substr(1));
                        std::uint64_t size = 0;
                        if (!boundedFile(source, kMaximumTextBytes, size))
                            return MaiToolResult::failure(
                                MaiErrorCode::InvalidInput,
                                "data file is outside workspace, inaccessible or exceeds 256 KiB");
                        // 保留文件引用，二进制与换行由 curl 读取，不将文件内容复制进 argv。
                        bodyOptions.insert(bodyOptions.end(), {"--data-binary", "@" + source});
                    } else {
                        bodyOptions.insert(bodyOptions.end(), {"--data-raw", value});
                    }
                } else if (option == "-T" || option == "--upload-file") {
                    if (hasUpload)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "only one upload file is allowed");
                    const std::string source = workspacePath(context, value);
                    std::uint64_t size = 0;
                    if (source.empty() ||
                        !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(source), size) ||
                        size == 0 || size > static_cast<std::uint64_t>(maximumMb) * kMegabyte)
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "upload file is inaccessible or too large");
                    hasUpload = true;
                    bodyOptions.insert(bodyOptions.end(), {"--upload-file", source});
                } else if (option == "-F" || option == "--form" || option == "--form-string") {
                    const std::size_t equals = value.find('=');
                    if (++formFields > kMaximumFormFields || equals == std::string::npos ||
                        equals == 0 || equals > 128 || hasControls(value) ||
                        !std::all_of(value.begin(), value.begin() + equals, [](unsigned char ch) {
                            return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' ||
                                   ch == '[' || ch == ']';
                        }))
                        return MaiToolResult::failure(
                            MaiErrorCode::InvalidInput,
                            "form requires name=value or name=@file; at most 12 fields");
                    const std::string field = value.substr(0, equals + 1);
                    const std::string content = value.substr(equals + 1);
                    if (option != "--form-string" && !content.empty() && content.front() == '@') {
                        const std::string source = workspacePath(context, content.substr(1));
                        std::uint64_t size = 0;
                        // 不开放 curl multipart 的逗号列表和分号参数，避免隐藏的文件读取。
                        if (source.find_first_of(",;\"") != std::string::npos ||
                            !boundedFile(source, static_cast<std::uint64_t>(maximumMb) * kMegabyte,
                                         size))
                            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                          "form file is inaccessible, too large or "
                                                          "uses unsupported multipart parameters");
                        formBytes += size;
                        bodyOptions.insert(bodyOptions.end(),
                                           {"--form", field + "@\"" + source + "\""});
                    } else {
                        if (option != "--form-string" && !content.empty() && content.front() == '<')
                            return MaiToolResult::failure(
                                MaiErrorCode::InvalidInput,
                                "form file contents via < are unsupported; use @file or "
                                "--form-string");
                        formBytes += content.size();
                        bodyOptions.insert(bodyOptions.end(), {"--form-string", value});
                    }
                    if (formBytes > static_cast<std::uint64_t>(maximumMb) * kMegabyte)
                        return MaiToolResult::failure(
                            MaiErrorCode::InvalidInput,
                            "combined multipart body exceeds max_size_mb");
                } else if (option == "-A" || option == "--user-agent") {
                    if (value.size() > 1024 || hasControls(value))
                        return MaiToolResult::failure(
                            MaiErrorCode::InvalidInput,
                            "user-agent must be at most 1024 bytes without controls");
                    customHeaders.insert(customHeaders.end(), {"--user-agent", value});
                } else if (option == "-b" || option == "--cookie") {
                    if (!cookieOptions.empty() || value.empty() || value.size() > 4096 ||
                        hasControls(value))
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "cookie requires one bounded string or "
                                                      "workspace file without controls");
                    std::string cookie = value;
                    if (value.find('=') == std::string::npos) {
                        cookie = workspacePath(context, value);
                        std::uint64_t size = 0;
                        if (!boundedFile(cookie, kMaximumTextBytes, size))
                            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                          "cookie file is outside workspace, "
                                                          "inaccessible or exceeds 256 KiB");
                    }
                    cookieOptions = {"--cookie", cookie};
                } else if (option == "-c" || option == "--cookie-jar") {
                    if (!cookieJar.empty())
                        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                      "only one cookie jar is allowed");
                    cookieJar = workspacePath(context, value);
                    if (cookieJar.empty() ||
                        MaiFileSystem::exists(MaiFilePath::fromUtf8(cookieJar)))
                        return MaiToolResult::failure(
                            MaiErrorCode::InvalidInput,
                            "cookie jar requires a new authorized workspace path");
                } else {
                    if (value.size() != 1 || value.front() < '0' || value.front() > '5')
                        return MaiToolResult::failure(
                            MaiErrorCode::InvalidInput,
                            "--max-redirs must be an integer from 0 to 5");
                    maximumRedirects = value.front() - '0';
                }
            } else if (option == "-I" || option == "--head" || option == "--compressed") {
                if (option != "--compressed") hasHead = true;
                options.push_back(option == "-I" ? "--head" : option);
            } else if (option == "-L" || option == "--location") {
                follow = true;
            } else if (option == "--silent" || option == "--include" || option == "--verbose" ||
                       option == "--show-error" ||
                       (option.size() > 1 && option.size() <= 16 && option.front() == '-' &&
                        option.find_first_not_of("sSivL", 1) == std::string::npos)) {
                // JSON 已分别提供状态、头与响应体；不打开原生 verbose 的凭据转储。
                if (option.find('L') != std::string::npos) follow = true;
            } else if (option[0] != '-' && url.empty()) {
                url = option;
            } else {
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unsupported curl CLI argument at index " +
                                                  std::to_string(index) + ". " +
                                                  kSupportedArguments);
            }
        }
        if (!isHttpUrl(url) || (hasData && hasUpload) || (formFields && (hasData || hasUpload)) ||
            ((method == "HEAD" || hasHead) && (hasData || hasUpload || formFields)))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid URL or incompatible curl options");
        if (method == "HEAD" && !hasHead) {
            hasHead = true;
            options.push_back("--head");
        }
        const std::string requested = request.value("output_path", std::string{});
        if (!requested.empty() && MaiFilePath::fromUtf8(requested).isAbsolute())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "output_path must be relative");

        const std::string id = MaiIdGenerator::generate("curl_");
        const MaiFilePath temporary =
            MaiFilePath::fromUtf8(context.resolvePath(".mai-curl-" + id + ".body"));
        const MaiFilePath headers =
            MaiFilePath::fromUtf8(context.resolvePath(".mai-curl-" + id + ".headers"));
        if (temporary.isEmpty() || headers.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "workspace temporary path is inaccessible");
        TemporaryFiles cleanup{{temporary, headers}};
        if (MaiFileSystem::createEmptyFile(temporary) || MaiFileSystem::createEmptyFile(headers))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot create curl output files");
        const MaiFilePath cookieTemporary =
            MaiFilePath::fromUtf8(workspacePath(context, ".mai-curl-" + id + ".cookies"));
        const bool keepCookies = follow || !cookieJar.empty();
        if (keepCookies) {
            if (cookieTemporary.isEmpty() || MaiFileSystem::createEmptyFile(cookieTemporary))
                return MaiToolResult::failure(MaiErrorCode::Internal,
                                              "cannot create private curl cookie jar");
            cleanup.paths.push_back(cookieTemporary);
        }
        const std::uint64_t maximumOutput = requested.empty()
                                                ? kMaximumTextBytes
                                                : static_cast<std::uint64_t>(maximumMb) * kMegabyte;
        std::vector<std::string> command = {"curl",
                                            "-q",
                                            "--silent",
                                            "--show-error",
                                            "--proto",
                                            "=http,https",
                                            "--proto-redir",
                                            "=http,https",
                                            "--max-redirs",
                                            "5",
                                            "--connect-timeout",
                                            "10",
                                            "--max-filesize",
                                            std::to_string(maximumOutput),
                                            "--output",
                                            temporary.toUtf8(),
                                            "--dump-header",
                                            headers.toUtf8()};
        if (!mCaBundlePath.empty()) {
            command.insert(command.end(), {"--cacert", mCaBundlePath});
        }
#if defined(_WIN32)
        else {
            // Windows 的 OpenSSL 后端不会自动读取系统根证书；默认与模型客户端保持一致。
            // 显式 CA 仍是独立信任策略，不能叠加 native 后掩盖错误或缺失的 CA 文件。
            command.push_back("--ca-native");
        }
#endif
        std::unique_ptr<FILE, decltype(&std::fclose)> errors(std::tmpfile(), std::fclose);
        if (!errors)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot create curl diagnostic stream");
        maiAssertBlockingAllowed("curl");
        int exitStatus = 0;
        ResponseHeaders response;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        bool loadPreviousCookies = false;
        for (int redirects = 0;; ++redirects) {
            const double remaining =
                std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0 || context.isCanceled()) {
                exitStatus =
                    context.isCanceled() ? CURLE_ABORTED_BY_CALLBACK : CURLE_OPERATION_TIMEDOUT;
                break;
            }
            std::vector<std::string> hop = command;
            hop.insert(hop.end(), {"--max-time", std::to_string(remaining)});
            hop.insert(hop.end(), options.begin(), options.end());
            hop.insert(hop.end(), customHeaders.begin(), customHeaders.end());
            hop.insert(hop.end(), cookieOptions.begin(), cookieOptions.end());
            hop.insert(hop.end(), bodyOptions.begin(), bodyOptions.end());
            if (!method.empty()) hop.insert(hop.end(), {"--request", method});
            if (keepCookies) {
                if (loadPreviousCookies)
                    hop.insert(hop.end(), {"--cookie", cookieTemporary.toUtf8()});
                hop.insert(hop.end(), {"--cookie-jar", cookieTemporary.toUtf8()});
            }
            hop.push_back(url);
            std::vector<char*> argv;
            argv.reserve(hop.size() + 1);
            for (std::string& option : hop) argv.push_back(option.data());
            argv.push_back(nullptr);
            {
                std::lock_guard<std::mutex> lock(sCurlCommandMutex);
                mai_curl_set_cancel_check(checkCanceled, const_cast<MaiToolContext*>(&context));
                mai_curl_set_error_file(errors.get());
                exitStatus = mai_curl_execute(static_cast<int>(hop.size()), argv.data());
                mai_curl_set_error_file(nullptr);
                mai_curl_set_cancel_check(nullptr, nullptr);
            }
            if (exitStatus != 0) break;
            std::string headerBytes;
            bool headerTruncated = false;
            const MaiError headerError = MaiFileSystem::readFile(
                headers, headerBytes, kMaximumHeaderBytes, &headerTruncated);
            response = headerError ? ResponseHeaders{} : parseResponseHeaders(headerBytes);
            if (headerError || headerTruncated || response.status == 0)
                return MaiToolResult::failure(MaiErrorCode::Protocol,
                                              "curl returned no complete HTTP status");
            if (keepCookies) {
                std::uint64_t bytes = 0;
                if (!boundedFile(cookieTemporary.toUtf8(), kMaximumTextBytes, bytes))
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "curl cookie jar exceeds 256 KiB");
            }
            const std::string location = response.fields.value("location", std::string{});
            if (!follow || location.empty() ||
                (response.status != 301 && response.status != 302 && response.status != 303 &&
                 response.status != 307 && response.status != 308))
                break;
            if (redirects >= maximumRedirects) {
                exitStatus = CURLE_TOO_MANY_REDIRECTS;
                break;
            }
            const std::string next = resolvedRedirect(url, location);
            if (next.empty())
                return MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "redirect destination must be a valid HTTP(S) URL without credentials");
            const std::string previousOrigin = urlOrigin(url);
            const std::string nextOrigin = urlOrigin(next);
            loadPreviousCookies = !previousOrigin.empty() && previousOrigin == nextOrigin;
            if (!loadPreviousCookies) {
                // 任意自定义头均可能含凭据，跨 origin 后永久移除，包括返回原站点的后续跳转。
                customHeaders.clear();
                cookieOptions.clear();
            }
            if ((response.status == 303 && method != "HEAD" && !hasHead) ||
                ((response.status == 301 || response.status == 302) && !hasUpload)) {
                bodyOptions.clear();
                // 与原生 curl -X 一致：显式方法跨跳转保留；只有推导出的 POST 变为 GET。
            }
            url = next;
        }
        bool logTruncated = false;
        readLog(errors.get(), logTruncated);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "curl was canceled");
        if (exitStatus != 0)
            return MaiToolResult::failure(
                MaiErrorCode::Network,
                Json{// stderr 可含 URL、代理或 cookie 警告，不将其作为模型可见诊断返回。
                     {"exit_status", exitStatus},
                     {"log_tail", "curl transfer failed; raw diagnostics withheld"},
                     {"log_truncated", logTruncated}}
                    .dump());
        const int httpStatus = response.status;
        if (hasHead) {
            // curl --head 会把原始头也写入 body 文件；只向模型交付过滤后的结构化头。
            if (MaiFileSystem::writeFile(temporary, {}))
                return MaiToolResult::failure(MaiErrorCode::Internal,
                                              "cannot finalize structured HEAD response");
        }
        if (!cookieJar.empty()) {
            const MaiFilePath destination = MaiFilePath::fromUtf8(cookieJar);
            const MaiError directoryError = MaiFileSystem::createDirectories(destination.dirName());
            if (directoryError)
                return MaiToolResult::failure(directoryError.code(),
                                              "cannot create cookie jar directory");
            const MaiError published = MaiFileSystem::publishNewFile(cookieTemporary, destination);
            if (published)
                return MaiToolResult::failure(published.code(), "cannot publish new cookie jar");
        }
        std::uint64_t received = 0;
        if (!MaiFileSystem::fileSize(temporary, received) || received > maximumOutput)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "curl response exceeds the output limit");

        if (!requested.empty()) {
            if (httpStatus < 200 || httpStatus >= 300)
                return MaiToolResult::failure(
                    MaiErrorCode::Network,
                    "curl download returned HTTP " + std::to_string(httpStatus));
            const std::string output = context.resolvePath(requested);
            if (output.empty() || MaiFileSystem::exists(MaiFilePath::fromUtf8(output)))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "output_path is unavailable or already exists");
            const MaiFilePath destination = MaiFilePath::fromUtf8(output);
            const MaiError directoryError = MaiFileSystem::createDirectories(destination.dirName());
            if (directoryError)
                return MaiToolResult::failure(directoryError.code(), directoryError.message());
            const MaiError published = MaiFileSystem::publishNewFile(temporary, destination);
            if (published) return MaiToolResult::failure(published.code(), published.message());
            return MaiToolResult::success(Json{
                {"status", httpStatus},
                {"headers", response.fields},
                {"path", output},
                {"bytes", received},
                {"exit_status", exitStatus}}.dump());
        }
        std::string body;
        bool truncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(temporary, body, kMaximumTextBytes, &truncated);
        if (readError || truncated)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot read the complete curl response");
        Json result = {{"status", httpStatus},
                       {"headers", response.fields},
                       {"bytes", received},
                       {"exit_status", exitStatus}};
        if (request.value("text_only", false) &&
            response.fields.value("content-type", std::string{}).find("text/html") !=
                std::string::npos)
            body = maiHtmlToText(std::move(body));
        try {
            Json text = body;
            text.dump();
            result["body"] = std::move(text);
        } catch (const Json::type_error&) {
            result["body_base64"] = encodeBase64(body);
        }
        return MaiToolResult::success(result.dump());
    }

private:
    std::string mCaBundlePath;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCurlCliTool(std::string caBundlePath) {
    return std::make_unique<CurlCliTool>(std::move(caBundlePath));
}
