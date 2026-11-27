// 文件作用：实现 OpenAI Responses 协议的请求和流式解析。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiPathGuard.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiResponsesClient.h"
#include "MaiCurlNetworkRoute.h"

#include <curl/curl.h>
#include <json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"
#include "MaiModelRetry.h"
#include "MaiModelConnectionPool.h"
#include "MaiModelHttpFraming.h"
#include "MaiCloudModelCatalog.h"

namespace {

using Json = nlohmann::json;

// curl 默认只向初始来源发送 Authorization；逐跳处理时必须保留同一边界。
std::string modelOrigin(const std::string& url) {
    CURLU* parsed = curl_url();
    if (!parsed) return {};
    char* scheme = nullptr;
    char* host = nullptr;
    char* port = nullptr;
    std::string origin;
    if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK) {
        origin = std::string(scheme) + "://" + host + ":" + port;
        std::transform(origin.begin(), origin.end(), origin.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    curl_free(scheme);
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(parsed);
    return origin;
}

std::string stringField(const Json& object, const char* field) {
    return object.is_object() && object.contains(field) && object[field].is_string()
               ? object[field].get<std::string>()
               : std::string{};
}

// 部分供应商用 HTTP 200 返回业务错误。只识别错误信封，不能把合法响应
// 中恰好叫 code 的普通字段当作失败；未知业务码保持 Protocol，不猜重试策略。
MaiError businessError(const Json& object, bool isGlm) {
    if (!isGlm || !object.is_object() || !object.contains("code") || !object.contains("msg") ||
        !stringField(object, "type").empty())
        return {};
    const auto& code = object["code"];
    if (code == 0 || code == "0") return {};
    const bool authentication = code == 1001 || code == "1001";
    return {authentication ? MaiErrorCode::NotConfigured : MaiErrorCode::Protocol,
            "GLM business error " + code.dump() + ": " + stringField(object, "msg")};
}

void readUsageCount(const Json& object, const char* field, std::size_t& count,
                    MaiUsageValueSource& source) {
    source = MaiUsageValueSource::Missing;
    count = 0;
    if (!object.is_object()) return;
    const auto value = object.find(field);
    if (value == object.end() || !value->is_number_integer()) return;
    if (value->is_number_unsigned()) {
        const auto raw = value->get<std::uint64_t>();
        if (raw > std::numeric_limits<std::size_t>::max()) return;
        count = static_cast<std::size_t>(raw);
    } else {
        const auto raw = value->get<std::int64_t>();
        if (raw < 0 || static_cast<std::uint64_t>(raw) > std::numeric_limits<std::size_t>::max())
            return;
        count = static_cast<std::size_t>(raw);
    }
    source = MaiUsageValueSource::Provider;
}

std::optional<MaiModelUsage> responseUsage(const Json& object) {
    if (!object.is_object()) return std::nullopt;
    MaiModelUsage result;
    readUsageCount(object, "input_tokens", result.inputTokens, result.inputSource);
    readUsageCount(object, "output_tokens", result.outputTokens, result.outputSource);
    readUsageCount(object, "total_tokens", result.totalTokens, result.totalSource);
    const Json inputDetails = object.value("input_tokens_details", Json{});
    readUsageCount(inputDetails, "cached_tokens", result.cachedInputTokens,
                   result.cachedInputSource);
    readUsageCount(inputDetails, "cache_write_tokens", result.cacheWriteInputTokens,
                   result.cacheWriteInputSource);
    const Json outputDetails = object.value("output_tokens_details", Json{});
    readUsageCount(outputDetails, "reasoning_tokens", result.reasoningOutputTokens,
                   result.reasoningOutputSource);
    if (result.inputSource == MaiUsageValueSource::Missing &&
        result.outputSource == MaiUsageValueSource::Missing &&
        result.totalSource == MaiUsageValueSource::Missing)
        return std::nullopt;
    // 仅两项都来自供应商时才可推导总数；缺失不是零，reasoning 已含在 output 内，
    // cached/cache_write 是 input 的明细，均不能再加一遍。
    if (result.totalSource == MaiUsageValueSource::Missing &&
        result.inputSource == MaiUsageValueSource::Provider &&
        result.outputSource == MaiUsageValueSource::Provider) {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        if (result.outputTokens <= maximum - result.inputTokens) {
            result.totalTokens = result.inputTokens + result.outputTokens;
            result.totalSource = MaiUsageValueSource::Derived;
        }
    }
    result.sourcePayload = object.dump();
    return result;
}

class LineBuffer {
public:
    void append(const char* bytes, std::size_t size) {
        mBytes.append(bytes, size);
    }

    bool next(std::string& line) {
        const std::size_t end = mBytes.find('\n', mScanned);
        if (end == std::string::npos) {
            mScanned = mBytes.size();
            return false;
        }
        line.assign(mBytes, 0, end);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        mBytes.erase(0, end + 1);
        mScanned = 0;
        return true;
    }

    bool takeRemainder(std::string& line) {
        if (mBytes.empty()) return false;
        line = std::move(mBytes);
        mBytes.clear();
        mScanned = 0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

private:
    std::string mBytes;
    std::size_t mScanned = 0;
};

std::string base64Encode(const std::string& input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second =
            offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto third =
            offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output.push_back(alphabet[first >> 2]);
        output.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        output.push_back(offset + 1 < input.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)]
                                                   : '=');
        output.push_back(offset + 2 < input.size() ? alphabet[third & 0x3f] : '=');
    }
    return output;
}

// 把经工作区路径校验的图片转换为 Responses 接口可接受的数据 URL。
// 不支持的类型或不存在的文件直接失败，避免把缺图请求送给供应商。
MaiResult<std::string> imageDataUrl(const MaiModelRequest& request, const MaiModelImage& image,
                                    const MaiModelImagePreparer& prepareImage) {
    if (request.workingDirectory.empty())
        return {MaiErrorCode::InvalidInput, "an image prompt requires a working directory"};
    if (image.mimeType != "image/jpeg" && image.mimeType != "image/png" &&
        image.mimeType != "image/gif" && image.mimeType != "image/webp")
        return {MaiErrorCode::InvalidInput, "unsupported prompt image type: " + image.mimeType};
    const MaiFilePath candidate = MaiFilePath::fromUtf8(image.path);
    const std::string resolved =
        candidate.isAbsolute() ? MaiFileSystem::resolve(candidate).toUtf8()
                               : maiResolvePathWithinRoot(request.workingDirectory, image.path);
    if (resolved.empty())
        return {MaiErrorCode::InvalidInput,
                "prompt image is outside the working directory: " + image.path};
    constexpr std::uint64_t kMaxSourceBytes = 20u * 1024u * 1024u;
    constexpr std::uint64_t kMaxRequestBytes = 2u * 1024u * 1024u;
    const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
    std::uint64_t sourceSize = 0;
    const bool knownSize = MaiFileSystem::fileSize(path, sourceSize);
    if (knownSize && sourceSize > kMaxSourceBytes)
        return {MaiErrorCode::InvalidInput, "prompt image exceeds the 20 MB limit: " + image.path};
    std::string bytes;
    bool truncated = false;
    const bool largeSource = knownSize && sourceSize > kMaxRequestBytes;
    if (!largeSource) {
        const MaiError readError =
            MaiFileSystem::readFile(path, bytes, kMaxSourceBytes, &truncated);
        if (readError) return readError;
        if (truncated || bytes.empty())
            return {MaiErrorCode::InvalidInput,
                    "prompt image is empty or exceeds the 20 MB limit: " + image.path};
    }
    std::string mime = image.mimeType;
    if (largeSource || bytes.size() > kMaxRequestBytes) {
        if (!prepareImage)
            return {MaiErrorCode::InvalidInput,
                    "image exceeds the 2 MB model request budget; resize it before retrying"};
        MaiResult<std::string> preview = prepareImage(resolved, request.workingDirectory);
        if (!preview) return preview.error();
        if (preview.value() == resolved)
            return {MaiErrorCode::Internal, "the model preview must not reuse the original image"};
        bytes.clear();
        truncated = false;
        const MaiError readError = MaiFileSystem::readFile(MaiFilePath::fromUtf8(preview.value()),
                                                           bytes, kMaxRequestBytes, &truncated);
        if (readError) return readError;
        if (truncated || bytes.empty())
            return {MaiErrorCode::InvalidInput,
                    "the prepared model image is empty or exceeds the 2 MB request budget"};
        mime = "image/jpeg";
    }
    return "data:" + mime + ";base64," + base64Encode(bytes);
}

const char* roleName(MaiModelRole role) {
    switch (role) {
        case MaiModelRole::System: return "system";
        case MaiModelRole::User: return "user";
        case MaiModelRole::Assistant: return "assistant";
        case MaiModelRole::ToolResult: return "user";
    }
    return "user";
}

// 仅检查会影响续接和工具执行的结构；厂商未知成员仍由原始快照原样保存。
// 错误只描述字段契约，不把思考、签名、加密状态或工具参数写进诊断信息。
MaiError validateOutput(const Json& output) {
    if (!output.is_array()) return {MaiErrorCode::Protocol, "Responses output must be an array"};
    std::set<std::string> callIds;
    for (const Json& item : output) {
        if (!item.is_object() || stringField(item, "type").empty())
            return {MaiErrorCode::Protocol, "Responses output needs typed objects"};
        const std::string type = stringField(item, "type");
        if (type == "function_call" || type == "custom_tool_call") {
            const std::string callId = stringField(item, "call_id");
            const char* inputField = type == "custom_tool_call" ? "input" : "arguments";
            if (callId.empty() || stringField(item, "name").empty() || !item.contains(inputField) ||
                !item[inputField].is_string() || !callIds.insert(callId).second)
                return {MaiErrorCode::Protocol, "Responses returned an incomplete tool call"};
            if (type == "custom_tool_call" && item.contains("status") &&
                stringField(item, "status") != "completed")
                return {MaiErrorCode::Protocol, "Responses custom call is not completed"};
        } else if (type == "reasoning") {
            for (const char* field : {"content", "summary"}) {
                const auto parts = item.find(field);
                if (parts == item.end() || parts->is_null()) continue;
                if (!parts->is_array())
                    return {MaiErrorCode::Protocol, "Responses reasoning parts must be arrays"};
                for (const Json& part : *parts)
                    if (!part.is_object() || (part.contains("text") && !part["text"].is_string()))
                        return {MaiErrorCode::Protocol,
                                "Responses reasoning text must be a string"};
            }
            for (const char* field : {"signature", "encrypted_content", "format"}) {
                const auto value = item.find(field);
                if (value != item.end() && !value->is_null() && !value->is_string())
                    return {MaiErrorCode::Protocol, "Responses reasoning metadata must be strings"};
            }
        }
    }
    return {};
}

// 把模型消息、图片和工具定义组装为 Responses 请求体。
// 工具结果引用原调用标识；图像准备失败时直接返回错误，不能发送缺媒体的请求。
MaiResult<std::string> buildBody(const MaiModelRequest& request,
                                 const MaiModelImagePreparer& prepareImage) {
    // 显式档位按选中型号校验；不能静默丢弃不支持值或把 Sol/Claude 五档压回旧三档。
    if (!maiIsValidReasoningEffortForModel(request.model, request.reasoningEffort))
        return {MaiErrorCode::InvalidInput, "Unsupported reasoning effort for the selected model"};
    // 路由身份只接受有界不透明标识，不回显非法值，也不以正文或路径生成替代身份。
    if (request.sessionId.size() > 256 ||
        std::any_of(request.sessionId.begin(), request.sessionId.end(),
                    [](unsigned char value) { return value < 0x21 || value > 0x7e; }))
        return {MaiErrorCode::InvalidInput, "Invalid model request session identifier"};
    Json input = Json::array();
    const auto capability = maiCloudModelInfo(request.model);
    const bool isOpenAi = capability && capability->provider == "openai";
    // 调用类型属于历史调用身份；最后一轮可能不再提供 tools，不能据当前声明猜结果类型。
    std::set<std::string> customCallIds;
    std::size_t latestUser = request.messages.size();
    for (std::size_t index = 0; index < request.messages.size(); ++index)
        if (request.messages[index].role == MaiModelRole::User) latestUser = index;
    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const MaiModelMessage& message = request.messages[index];
        if (message.role == MaiModelRole::ToolResult && message.toolCallId.empty())
            return {MaiErrorCode::Protocol, "a tool result needs its call ID"};
        if (!message.images.empty() && message.role != MaiModelRole::User &&
            message.role != MaiModelRole::ToolResult)
            return {MaiErrorCode::InvalidInput,
                    "Responses accepts image parts only in user or tool-result input"};

        if (message.role == MaiModelRole::Assistant && message.continuation && capability &&
            message.continuation->provider == capability->provider &&
            message.continuation->model == request.model) {
            // 完整 output 是唯一续接来源；显示投影和工具投影不得再重建、重复或重排。
            const auto& state = *message.continuation;
            if (state.format != "responses.output.v1")
                return {MaiErrorCode::Protocol, "Responses continuation format is unsupported"};
            const Json output = Json::parse(state.payload, nullptr, false);
            if (const MaiError error = validateOutput(output)) return error;
            for (const Json& item : output) {
                if (stringField(item, "type") == "custom_tool_call") {
                    if (!isOpenAi || stringField(item, "name") != "apply_patch")
                        return {MaiErrorCode::Protocol,
                                "Responses custom continuation is unsupported"};
                    customCallIds.insert(stringField(item, "call_id"));
                }
                input.push_back(item);
            }
            continue;
        }

        Json content;
        if (!message.images.empty()) {
            content = Json::array();
            std::string text = message.content;
            Json images = Json::array();
            for (const MaiModelImage& image : message.images) {
                MaiResult<std::string> encoded = imageDataUrl(request, image, prepareImage);
                if (!encoded) {
                    if (message.role == MaiModelRole::ToolResult || index == latestUser ||
                        encoded.error().code() != MaiErrorCode::NotFound)
                        return encoded.error();
                    if (!text.empty()) text += "\n";
                    text += "[A previously attached image is no longer available.]";
                    continue;
                }
                images.push_back(Json{{"type", "input_image"}, {"image_url", encoded.value()}});
            }
            if (!text.empty()) content.push_back(Json{{"type", "input_text"}, {"text", text}});
            for (Json& image : images) content.push_back(std::move(image));
        } else {
            content = message.content;
        }

        if (message.role == MaiModelRole::ToolResult) {
            input.push_back(
                Json{{"type", customCallIds.count(message.toolCallId) ? "custom_tool_call_output"
                                                                      : "function_call_output"},
                     {"call_id", message.toolCallId},
                     {"output", std::move(content)}});
            continue;
        }
        // OpenAI/OpenRouter 的加密推理只能由原 output 回放，显示文本不能代替原生状态。
        // 仅无续接状态的旧 GLM/DeepSeek 记录保留文本兼容；来源不匹配的现代状态只回放普通投影。
        if (message.role == MaiModelRole::Assistant && !message.continuation &&
            !message.reasoning.empty() &&
            (!capability ||
             (capability->provider != "openrouter" && capability->provider != "openai"))) {
            input.push_back(Json{{"type", "reasoning"},
                                 {"content", Json::array({Json{{"type", "reasoning_text"},
                                                               {"text", message.reasoning}}})}});
        }
        if (!message.content.empty() || message.invocations.empty() || !message.images.empty())
            input.push_back(Json{{"type", "message"},
                                 {"role", roleName(message.role)},
                                 {"content", std::move(content)}});
        for (const MaiToolInvocation& call : message.invocations) {
            if (call.id.empty() || call.name.empty())
                return {MaiErrorCode::Protocol, "a function call needs an ID and name"};
            if (isOpenAi && call.name == "apply_patch") {
                // 413 恢复可移除不透明续接；普通执行参数仍保留完整补丁，重建原生调用。
                const Json arguments = Json::parse(call.arguments, nullptr, false);
                if (!arguments.is_object() || arguments.size() != 1 ||
                    !arguments.contains("patch") || !arguments["patch"].is_string())
                    return {MaiErrorCode::Protocol, "Historical apply_patch input is invalid"};
                customCallIds.insert(call.id);
                input.push_back(Json{{"type", "custom_tool_call"},
                                     {"call_id", call.id},
                                     {"name", call.name},
                                     {"input", arguments["patch"]}});
            } else {
                input.push_back(Json{{"type", "function_call"},
                                     {"call_id", call.id},
                                     {"name", call.name},
                                     {"arguments", call.arguments}});
            }
        }
    }

    Json body{
        {"model", request.model}, {"input", std::move(input)}, {"stream", true}, {"store", false}};
    if (capability && capability->provider == "openai") {
        // 无状态响应默认返回 encrypted_content；显式 include 仍受官方支持，兼容旧接口行为。
        // 原始 output 继续作为唯一续接来源，不依赖服务端保存的 response ID。
        // https://developers.openai.com/api/docs/guides/reasoning#preserve-reasoning-without-stored-responses
        body["include"] = Json::array({"reasoning.encrypted_content"});
    }
    if (!request.sessionId.empty() && capability) {
        // 仅映射型号目录明确声明的供应商字段；不把会话 ID 填进终端用户隔离字段。
        // 空身份保留旧普通调用；自动缓存提示不改变原 input/instructions/tools 或输出预算。
        if (capability->provider == "openrouter") {
            body["session_id"] = request.sessionId;
            // 此显式自动断点是已核 Claude 合同，不能仅凭同为 OpenRouter 推给免费模型。
            if (request.model == "anthropic/claude-fable-5.2" ||
                request.model == "anthropic/claude-opus-5.5")
                body["cache_control"] = Json{{"type", "ephemeral"}};
        } else if (capability->provider == "glm" || capability->provider == "openai") {
            // OpenAI 接受稳定缓存标识；沿用会话身份，不设置额外缓存模式或保留时长。
            // https://developers.openai.com/api/docs/guides/prompt-caching
            body["prompt_cache_key"] = request.sessionId;
        }
        // DeepSeek 自动管理上下文缓存；不发送其明确不支持的缓存路由参数。
    }
    if (request.model == "particle-ai/particle-1:free") {
        // 免费是明确的请求约束，即使普通调用没有 sessionId 也不能省略价格守卫。
        // 不启用付费插件或替代型号；没有零价供应商时直接失败，不接受收费回退。
        // https://openrouter.ai/docs/guides/routing/provider-selection#max-price
        body["provider"] = Json{
            {"max_price", Json{{"prompt", 0}, {"completion", 0}, {"request", 0}, {"image", 0}}},
            {"allow_fallbacks", false}};
        body["plugins"] = Json::array();
    }
    if (request.maxOutputTokens != 0) body["max_output_tokens"] = request.maxOutputTokens;
    if (capability && !request.reasoningEffort.empty())
        body["reasoning"] = Json{{"effort", request.reasoningEffort}};
    if (!request.baseInstructions.empty()) body["instructions"] = request.baseInstructions;
    if (request.temperature >= 0.0) body["temperature"] = request.temperature;
    if (!request.tools.empty()) {
        Json tools = Json::array();
        for (const MaiToolSpec& tool : request.tools) {
            if (!tool.inputGrammar.empty()) {
                if (tool.name != "apply_patch" || !capability)
                    return {MaiErrorCode::InvalidInput,
                            "The native tool input format is unsupported"};
                if (isOpenAi) {
                    tools.push_back(Json{{"type", "custom"},
                                         {"name", tool.name},
                                         {"description", tool.description},
                                         {"format", Json{{"type", "grammar"},
                                                         {"syntax", "lark"},
                                                         {"definition", tool.inputGrammar}}}});
                    continue;
                }
            }
            Json parameters = Json::parse(tool.parametersJson, nullptr, false);
            if (parameters.is_discarded()) parameters = Json::object();
            tools.push_back(Json{{"type", "function"},
                                 {"name", tool.name},
                                 {"description", tool.description},
                                 {"parameters", std::move(parameters)}});
        }
        body["tools"] = std::move(tools);
    }
    return body.dump(-1, ' ', false, Json::error_handler_t::replace);
}

struct PendingCall {
    std::string type;
    std::string id;
    std::string itemId;
    std::string name;
    std::string arguments;
    bool complete = false;
    bool invalid = false;
    bool hasInput = false;
};

struct StreamContext {
    const MaiStreamSink* sink = nullptr;
    std::string usageProvider;
    const std::atomic<bool>* cancel = nullptr;
    LineBuffer lines;
    std::map<int, PendingCall> calls;
    std::set<std::string> customTools;
    struct TextItem {
        MaiModelTextItem info;
        std::map<int, std::string> content;
        bool started = false;
        bool completed = false;
    };
    std::map<int, TextItem> textItems;
    // 显示累计和原始快照分开；done/terminal 可以补尾，不能靠“见过”标记丢弃完整尾部。
    std::map<std::tuple<int, int, bool>, std::string> reasoningContent;
    std::map<int, Json> completedOutput;
    std::map<int, std::string> publishedCompletedOutput;
    std::set<int> observedOutputIndices;
    Json terminalOutput;
    bool hasTerminalOutput = false;
    bool malformedEvent = false;
    std::string rawBody;
    std::string terminalError;
    MaiError businessFailure;
    bool isGlm = false;
    MaiModelHttpFraming framing;
    bool transientStreamError = false;
    bool contextLengthError = false;
    // 仅保存已校验的数字秒，-1 表示缺失/无效；原响应头可能带凭据，不能保留或写入诊断。
    // 402 诊断仅接受数字秒；恢复等待另保留支持 HTTP 日期的毫秒值。
    long retryAfterSeconds = -1;
    long retryAfterDelayMs = -1;
    bool sawOutput = false;
    bool done = false;
    bool inactive = false;
    bool progressTimedOut = false;
    bool manualRedirects = false;
    bool redirectStatus = false;
    bool redirectResponse = false;
    long inactivityTimeoutSeconds = 0;
    long progressTimeoutSeconds = 0;
    // 链路活动与生成进展是两个事实；保活证明连接仍有数据，不能证明模型仍在计算。
    std::chrono::steady_clock::time_point lastStreamActivity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastModelProgress = lastStreamActivity;
    std::set<std::tuple<std::string, int, int>> progressEvents;
    std::size_t responseBytes = 0;
    curl_off_t uploadedBytes = 0;
};

void noteProtocolProgress(StreamContext& context, const std::string& type, int outputIndex = -1,
                          int contentIndex = -1) {
    // 同一 HTTP 应答内重复的 in_progress/快照不能永久延长生成期限。
    // 有界保存阶段键即可；超过此数的长输出仍由实际文字/参数增量继续计时。
    if (context.progressEvents.size() < 4096 &&
        context.progressEvents.emplace(type, outputIndex, contentIndex).second)
        context.lastModelProgress = std::chrono::steady_clock::now();
}

int eventIndex(const Json& event, const char* name, int fallback) {
    const auto value = event.find(name);
    if (value == event.end() || !value->is_number_integer()) return fallback;
    const double index = value->get<double>();
    if (index < 0 || index > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(index);
}

void mergeCall(PendingCall& call, const Json& item, bool finalSnapshot = false) {
    if (!item.is_object()) return;
    if (call.type == "custom_tool_call" || stringField(item, "type") == "custom_tool_call")
        for (const char* field : {"call_id", "id", "name"})
            if (item.contains(field) && !item[field].is_string()) call.invalid = true;
    const std::string id = stringField(item, "call_id");
    const std::string name = stringField(item, "name");
    // 完成快照可补齐增量尾部，不能改写已观察的调用身份、输入前缀或已完成输入。
    const auto mergeIdentity = [&](std::string& stored, const std::string& value) {
        if (value.empty()) return;
        if (!stored.empty() && stored != value) call.invalid = true;
        stored = value;
    };
    mergeIdentity(call.type, stringField(item, "type"));
    mergeIdentity(call.id, id);
    mergeIdentity(call.itemId, stringField(item, "id"));
    mergeIdentity(call.name, name);
    const char* field = call.type == "custom_tool_call" ? "input" : "arguments";
    if (item.contains(field)) {
        if (!item[field].is_string()) {
            call.invalid = true;
            return;
        }
        const std::string full = item[field].get<std::string>();
        // added 通常用空串作为输入占位；完成帧的空串则是实际完整输入。
        if (finalSnapshot || !full.empty() || call.arguments.empty() || call.complete) {
            if ((!call.arguments.empty() &&
                 (full.size() < call.arguments.size() ||
                  full.compare(0, call.arguments.size(), call.arguments) != 0)) ||
                (call.complete && call.hasInput && full != call.arguments))
                call.invalid = true;
            call.arguments = full;
            call.hasInput = true;
        }
    } else if (finalSnapshot)
        call.invalid = true;
}

void updateTextItem(StreamContext::TextItem& state, const Json& item) {
    const std::string id = stringField(item, "id");
    if (!id.empty()) state.info.id = id;
    const std::string phase = stringField(item, "phase");
    if (phase == "commentary")
        state.info.phase = MaiModelTextPhase::Commentary;
    else if (phase == "partial_answer")
        state.info.phase = MaiModelTextPhase::PartialAnswer;
    else if (phase == "final_answer")
        state.info.phase = MaiModelTextPhase::FinalAnswer;
}

void beginTextItem(StreamContext& context, int index) {
    auto& state = context.textItems[index];
    state.info.index = index;
    if (state.started || state.completed) return;
    state.started = true;
    if (context.sink->onTextItemStarted) context.sink->onTextItemStarted(state.info);
}

void completeTextItem(StreamContext& context, int index) {
    auto& state = context.textItems[index];
    if (state.completed) return;
    beginTextItem(context, index);
    state.completed = true;
    if (context.sink->onTextItemCompleted) context.sink->onTextItemCompleted(state.info);
}

// done/terminal 快照可能补足未发送的增量，只交付尚未看到的后缀。
// 若快照与已显示前缀冲突，不重复或改写用户已经看过的正文。
void emitTextSnapshot(StreamContext& context, int index, int contentIndex,
                      const std::string& text) {
    auto& state = context.textItems[index];
    if (text.empty()) return;
    std::string& delivered = state.content[contentIndex];
    if (text.size() <= delivered.size() || text.compare(0, delivered.size(), delivered) != 0)
        return;
    beginTextItem(context, index);
    const std::string suffix = text.substr(delivered.size());
    delivered = text;
    context.sawOutput = true;
    if (context.sink->onTextItemDelta)
        context.sink->onTextItemDelta(state.info, suffix);
    else if (context.sink->onText)
        context.sink->onText(suffix);
}

void emitMessageSnapshot(StreamContext& context, int index, const Json& item) {
    auto& state = context.textItems[index];
    updateTextItem(state, item);
    if (!item.value("content", Json{}).is_array()) return;
    const auto& parts = item["content"];
    for (std::size_t part = 0; part < parts.size(); ++part) {
        if (stringField(parts[part], "type") != "output_text") continue;
        emitTextSnapshot(context, index, static_cast<int>(part), stringField(parts[part], "text"));
    }
}

// 原快照的字符串不能为显示而改写；这里只把未显示的后缀交给 reasoning 投影。
void emitReasoningSnapshot(StreamContext& context, int outputIndex, int partIndex, bool summary,
                           const std::string& text) {
    std::string& delivered = context.reasoningContent[{outputIndex, partIndex, summary}];
    if (text.size() <= delivered.size() || text.compare(0, delivered.size(), delivered) != 0)
        return;
    const std::string suffix = text.substr(delivered.size());
    delivered = text;
    context.sawOutput = true;
    context.lastModelProgress = std::chrono::steady_clock::now();
    if (context.sink->onReasoning) context.sink->onReasoning(suffix);
}

void emitReasoningItemSnapshot(StreamContext& context, int outputIndex, const Json& item) {
    for (const char* field : {"content", "summary"}) {
        const auto parts = item.find(field);
        if (parts == item.end() || !parts->is_array()) continue;
        for (std::size_t index = 0; index < parts->size(); ++index)
            emitReasoningSnapshot(context, outputIndex, static_cast<int>(index),
                                  std::strcmp(field, "summary") == 0,
                                  stringField((*parts)[index], "text"));
    }
}

void publishCompletedOutput(StreamContext& context, int index, const Json& item) {
    if (!item.is_object() || !context.sink->onOutputItemCompleted) return;
    const std::string payload = item.dump(-1, ' ', false, Json::error_handler_t::replace);
    auto& previous = context.publishedCompletedOutput[index];
    if (previous == payload) return;
    previous = payload;
    context.sink->onOutputItemCompleted(
        MaiModelOutputItem{index, stringField(item, "id"), stringField(item, "type"), payload});
}

void emitFullOutput(StreamContext& context, const Json& response) {
    if (!response.is_object() || !response.value("output", Json{}).is_array()) return;
    const Json& output = response["output"];
    for (std::size_t index = 0; index < output.size(); ++index) {
        const Json& item = output[index];
        const std::string type = stringField(item, "type");
        if (type == "function_call" || type == "custom_tool_call") {
            mergeCall(context.calls[static_cast<int>(index)], item, true);
            context.calls[static_cast<int>(index)].complete = true;
            continue;
        }
        if (type == "message") {
            emitMessageSnapshot(context, static_cast<int>(index), item);
            completeTextItem(context, static_cast<int>(index));
            continue;
        }
        if (type == "reasoning") emitReasoningItemSnapshot(context, static_cast<int>(index), item);
    }
}

// 解析单条模型流事件，更新当前请求的增量内容与完成状态。
// 异常或未知事件不能污染已确认的消息和工具调用片段。
void handleEvent(StreamContext& context, const Json& event) {
    if (const auto error = businessError(event, context.isGlm)) {
        context.businessFailure = error;
        context.done = true;
        return;
    }
    const std::string type = stringField(event, "type");
    if (type.empty()) {
        context.malformedEvent = true;
        return;
    }
    // 生命周期帧可以没有内容索引；缺失/空索引不能让异常越过 curl 的 C 回调边界。
    const int outputIndex = eventIndex(event, "output_index", -1);
    const int contentIndex = eventIndex(event, "content_index", 0);
    if (outputIndex >= 0) context.observedOutputIndices.insert(outputIndex);
    if (type == "error" || type == "response.error") {
        const Json error = event.value("error", Json{});
        // 仅认结构化错误码；未知类型不能通过错误文案触发自动重新采样。
        auto code = stringField(error, "code");
        if (code.empty()) code = stringField(event, "code");
        if (code.empty()) code = stringField(error, "type");
        context.contextLengthError = MaiModelRetry::isContextLengthCode(code);
        context.transientStreamError = MaiModelRetry::isTransientProviderCode(code);
        context.terminalError = stringField(error, "message");
        if (context.terminalError.empty()) context.terminalError = stringField(event, "message");
        if (context.terminalError.empty())
            context.terminalError = "Responses stream returned an error";
        context.done = true;
        return;
    }
    if (type == "response.created" || type == "response.in_progress") {
        // 生命周期属于协议进展，但不是助手文字或已交付输出；保留原重试/历史边界。
        noteProtocolProgress(context, type);
        return;
    }
    if (type == "response.content_part.added" || type == "response.content_part.done" ||
        type == "response.reasoning_summary_part.added" ||
        type == "response.reasoning_summary_part.done") {
        if (outputIndex >= 0)
            noteProtocolProgress(context, type, outputIndex,
                                 eventIndex(event, "summary_index", contentIndex));
        return;
    }
    if (type == "response.output_text.delta" || type == "response.reasoning_text.delta" ||
        type == "response.reasoning_summary_text.delta") {
        const std::string delta = stringField(event, "delta");
        if (delta.empty()) return;
        context.sawOutput = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
        if (type == "response.output_text.delta") {
            auto& state = context.textItems[outputIndex];
            if (state.completed) return;
            beginTextItem(context, outputIndex);
            state.content[contentIndex] += delta;
            if (context.sink->onTextItemDelta)
                context.sink->onTextItemDelta(state.info, delta);
            else if (context.sink->onText)
                context.sink->onText(delta);
        } else {
            const bool summary = type == "response.reasoning_summary_text.delta";
            context.reasoningContent[{
                outputIndex, summary ? eventIndex(event, "summary_index", 0) : contentIndex,
                summary}] += delta;
            if (context.sink->onReasoning) context.sink->onReasoning(delta);
        }
        return;
    }
    if (type == "response.output_text.done" || type == "response.reasoning_text.done" ||
        type == "response.reasoning_summary_text.done") {
        const std::string text = stringField(event, "text");
        if (text.empty()) return;
        if (type == "response.output_text.done") {
            emitTextSnapshot(context, outputIndex, contentIndex, text);
        } else {
            const bool summary = type == "response.reasoning_summary_text.done";
            emitReasoningSnapshot(context, outputIndex,
                                  summary ? eventIndex(event, "summary_index", 0) : contentIndex,
                                  summary, text);
        }
        noteProtocolProgress(context, type, outputIndex, contentIndex);
        return;
    }
    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const Json item = event.value("item", Json::object());
        const std::string itemType = stringField(item, "type");
        if (!item.is_object() || itemType.empty()) context.malformedEvent = true;
        if (itemType == "custom_tool_call" && outputIndex < 0) {
            context.terminalError = "Responses custom call needs an output index";
            return;
        }
        if (itemType == "custom_tool_call" && type == "response.output_item.done") {
            // done 本身已宣告完整，后续终态不能替缺字段或错误状态的完成帧补办资格。
            if (const MaiError error = validateOutput(Json::array({item}))) {
                context.terminalError = error.message();
            }
        }
        if (type == "response.output_item.done" && outputIndex >= 0) {
            // 记录完整 item，包含晚于正文到达的签名。快照重复只替换同一槽位，不重复追加。
            context.completedOutput[outputIndex] = item;
        }
        if (outputIndex >= 0 && (itemType == "message" || itemType == "reasoning" ||
                                 itemType == "function_call" || itemType == "custom_tool_call" ||
                                 itemType == "web_search_call" || itemType == "tool_search_call"))
            noteProtocolProgress(context, type, outputIndex);
        if ((itemType == "function_call" || itemType == "custom_tool_call") && outputIndex >= 0) {
            PendingCall& call = context.calls[outputIndex];
            mergeCall(call, item, type == "response.output_item.done");
            if (type == "response.output_item.done") call.complete = true;
            context.sawOutput = true;
        } else if (itemType == "message" && outputIndex >= 0) {
            updateTextItem(context.textItems[outputIndex], item);
            beginTextItem(context, outputIndex);
            emitMessageSnapshot(context, outputIndex, item);
            if (type == "response.output_item.done") completeTextItem(context, outputIndex);
        } else if (itemType == "reasoning" && outputIndex >= 0 &&
                   type == "response.output_item.done") {
            emitReasoningItemSnapshot(context, outputIndex, item);
        }
        // 原始完成事实与整批执行资格分开；先补齐显示投影，再保存完整不透明 item。
        if (type == "response.output_item.done" && outputIndex >= 0)
            publishCompletedOutput(context, outputIndex, item);
        return;
    }
    if (type == "response.function_call_arguments.delta" ||
        type == "response.custom_tool_call_input.delta") {
        if (outputIndex < 0) {
            context.terminalError = "Responses tool input needs an output index";
            return;
        }
        PendingCall& call = context.calls[outputIndex];
        if (type == "response.custom_tool_call_input.delta" && event.contains("item_id") &&
            !event["item_id"].is_string())
            call.invalid = true;
        mergeCall(call,
                  Json{{"type", type == "response.custom_tool_call_input.delta" ? "custom_tool_call"
                                                                                : "function_call"},
                       {"id", stringField(event, "item_id")}});
        const std::string delta = stringField(event, "delta");
        if (!event.contains("delta") || !event["delta"].is_string()) call.invalid = true;
        if (!call.complete && !delta.empty()) {
            call.arguments += delta;
            call.hasInput = true;
            context.lastModelProgress = std::chrono::steady_clock::now();
        } else if (call.complete && !delta.empty())
            call.invalid = true;
        context.sawOutput = true;
        return;
    }
    if (type == "response.function_call_arguments.done" ||
        type == "response.custom_tool_call_input.done") {
        if (outputIndex < 0) {
            context.terminalError = "Responses tool input needs an output index";
            return;
        }
        PendingCall& call = context.calls[outputIndex];
        if (type == "response.custom_tool_call_input.done" && event.contains("item_id") &&
            !event["item_id"].is_string())
            call.invalid = true;
        const bool custom = type == "response.custom_tool_call_input.done";
        const char* field = custom ? "input" : "arguments";
        if (!event.contains(field) || !event[field].is_string()) call.invalid = true;
        Json snapshot{{"type", custom ? "custom_tool_call" : "function_call"},
                      {"id", stringField(event, "item_id")}};
        if (event.contains(field)) snapshot[field] = event[field];
        mergeCall(call, snapshot, true);
        call.complete = true;
        context.sawOutput = true;
        noteProtocolProgress(context, type, outputIndex);
        return;
    }
    if (type == "response.web_search_call.in_progress" ||
        type == "response.web_search_call.searching" ||
        type == "response.web_search_call.completed") {
        if (outputIndex >= 0) noteProtocolProgress(context, type, outputIndex);
        return;
    }
    if (type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed") {
        const Json response = event.value("response", Json::object());
        if (!response.is_object()) {
            context.terminalError = "Responses terminal snapshot must be an object";
            context.done = true;
            return;
        }
        const std::string status = stringField(response, "status");
        if (type == "response.completed" && response.contains("status") && status != "completed") {
            context.terminalError = "Responses terminal status contradicts its completed event";
            context.done = true;
            return;
        }
        if (response.contains("output")) {
            context.terminalOutput = response["output"];
            context.hasTerminalOutput = true;
            if (const MaiError error = validateOutput(context.terminalOutput)) {
                context.terminalError = error.message();
                context.done = true;
                return;
            }
        }
        if (auto usage = responseUsage(response.value("usage", Json{}));
            usage && context.sink->onUsage) {
            usage->responseId = stringField(response, "id");
            usage->provider = context.usageProvider;
            context.sink->onUsage(*usage);
        }
        emitFullOutput(context, response);
        // 有些兼容服务只发送正文增量而不发送 item.done 或终止 output。
        // 仍须关闭已开始的可见发言，让调用方持久化尾部。
        for (const auto& entry : context.textItems) completeTextItem(context, entry.first);
        if (type != "response.completed") {
            const Json error = response.value("error", Json::object());
            const Json details = response.value("incomplete_details", Json::object());
            // max_output_tokens/content_filter 等 incomplete 是语义失败，不能当断流。
            if (type == "response.failed") {
                context.contextLengthError =
                    MaiModelRetry::isContextLengthCode(stringField(error, "code"));
                context.transientStreamError =
                    MaiModelRetry::isTransientProviderCode(stringField(error, "code"));
            }
            context.terminalError = stringField(error, "message");
            if (context.terminalError.empty())
                context.terminalError = stringField(details, "reason");
            if (context.terminalError.empty()) context.terminalError = type;
        }
        context.done = true;
        context.lastModelProgress = std::chrono::steady_clock::now();
    }
}

void handleLine(StreamContext& context, const std::string& line) {
    if (line.empty() || line[0] == ':' || line.rfind("data:", 0) != 0) return;
    std::string payload = line.substr(5);
    if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
    const Json event = Json::parse(payload, nullptr, false);
    if (event.is_object())
        handleEvent(context, event);
    else if (payload != "[DONE]")
        context.malformedEvent = true;
}

// 402 的既有诊断契约只记录已校验的数字秒，不扩大付费错误元数据。
long parseRetryAfterSeconds(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return -1;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (!std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return -1;
    const long delay = MaiModelRetry::parseDelayMilliseconds(value);
    return delay < 0 ? -1 : delay / 1000;
}

std::size_t headerCallback(char* bytes, std::size_t size, std::size_t count, void* user) {
    auto& context = *static_cast<StreamContext*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const auto length = size * count;
    context.framing.onHeader(std::string_view(bytes, length));
    if (length >= 5 && std::memcmp(bytes, "HTTP/", 5) == 0) {
        // 100 Continue、代理与重定向都可带来多个响应；后续响应不能继承前一个 Retry-After。
        context.retryAfterSeconds = -1;
        context.retryAfterDelayMs = -1;
        if (context.manualRedirects) {
            const std::string line(bytes, length);
            const auto space = line.find(' ');
            const int status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
            context.redirectStatus = status >= 300 && status < 400;
            context.redirectResponse = false;
        }
    } else if (context.manualRedirects && context.redirectStatus && !context.redirectResponse &&
               length > 9) {
        const std::string name(bytes, 9);
        if (std::equal(name.begin(), name.end(),
                       "location:", [](unsigned char left, unsigned char right) {
                           return std::tolower(left) == right;
                       })) {
            const std::string value(bytes + 9, length - 9);
            context.redirectResponse = value.find_first_not_of(" \t\r\n") != std::string::npos;
        }
    }
    constexpr std::size_t retryAfterNameLength = 12;
    if (length >= retryAfterNameLength &&
        std::equal(bytes, bytes + retryAfterNameLength,
                   "retry-after:", [](unsigned char left, unsigned char right) {
                       return std::tolower(left) == right;
                   })) {
        const std::string_view value(bytes + retryAfterNameLength, length - retryAfterNameLength);
        context.retryAfterSeconds = parseRetryAfterSeconds(value);
        context.retryAfterDelayMs = MaiModelRetry::parseDelayMilliseconds(value);
    }
    // 响应头到达也是真实通信；除已校验秒数外只记时间，不保存可能带凭据的原头字段。
    if (length > 0) context.lastStreamActivity = std::chrono::steady_clock::now();
    return context.cancel->load(std::memory_order_relaxed) ? 0 : length;
}

std::size_t writeCallback(char* bytes, std::size_t size, std::size_t count, void* user) {
    auto& context = *static_cast<StreamContext*>(user);
    if (count != 0 && size > SIZE_MAX / count) return 0;
    const std::size_t length = size * count;
    // 手动逐跳时，中间重定向正文不能被当成供应商的流式输出。
    if (context.redirectResponse) return length;
    context.responseBytes += length;
    context.framing.received(length);
    if (length > 0) context.lastStreamActivity = std::chrono::steady_clock::now();
    if (context.cancel->load(std::memory_order_relaxed)) return 0;
    constexpr std::size_t kMaxErrorBody = 64 * 1024;
    if (context.rawBody.size() < kMaxErrorBody)
        context.rawBody.append(bytes, std::min(length, kMaxErrorBody - context.rawBody.size()));
    context.lines.append(bytes, length);
    std::string line;
    while (context.lines.next(line)) handleLine(context, line);
    // 只在已确认完整 HTTP 实体时正常收尾；不为连接复用等待未知长度的尾流。
    return context.done && !context.framing.isComplete() ? 0 : length;
}

int progressCallback(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t uploadNow) {
    auto& context = *static_cast<StreamContext*>(user);
    if (context.cancel->load(std::memory_order_relaxed)) return 1;
    const auto now = std::chrono::steady_clock::now();
    // 最后一个参数才是上传字节；下载/上传总量不能冒充本次传输增长。
    if (uploadNow > context.uploadedBytes) {
        context.lastStreamActivity = now;
        // 请求体尚在实际上传时，服务端还未获得完整输入，不把这段生产性等待算作停滞。
        context.lastModelProgress = now;
    }
    context.uploadedBytes = uploadNow;
    if (context.inactivityTimeoutSeconds > 0 &&
        now - context.lastStreamActivity >=
            std::chrono::seconds(context.inactivityTimeoutSeconds)) {
        context.inactive = true;
        return 1;
    }
    if (context.progressTimeoutSeconds > 0 &&
        now - context.lastModelProgress >= std::chrono::seconds(context.progressTimeoutSeconds)) {
        context.progressTimedOut = true;
        return 1;
    }
    return 0;
}

bool isRetryable(CURLcode code) {
    return code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING;
}

std::string providerError(const std::string& body) {
    const Json parsed = Json::parse(body, nullptr, false);
    if (!parsed.is_object()) return {};
    const Json error = parsed.value("error", Json{});
    if (error.is_string()) return error.get<std::string>();
    const std::string message = stringField(error, "message");
    const std::string code = stringField(error, "code");
    return message.empty() ? code : code.empty() ? message : message + " (" + code + ")";
}

// 只输出匹配已核合同的静态分类值，不回显任意 metadata、remedy_hint 或原响应头。
// RateLimited 用于告诉调用方这是临时预算竞争；它不改变 HTTP 402 的自动重试策略。
std::string openRouterCreditDiagnostic(const std::string& body, long retryAfterSeconds,
                                       MaiErrorCode& code) {
    const Json parsed = Json::parse(body, nullptr, false);
    if (!parsed.is_object()) return {};
    const Json error = parsed.value("error", Json{});
    if (!error.is_object()) return {};
    const Json metadata = error.value("metadata", Json{});
    const std::string reason = stringField(metadata, "reason");
    const std::string source = stringField(metadata, "limit_source");
    if (reason == "in_flight_budget_exhausted" && source == "openrouter_in_flight_budget") {
        code = MaiErrorCode::RateLimited;
        std::string diagnostic =
            " [OpenRouter limit_source=openrouter_in_flight_budget "
            "reason=in_flight_budget_exhausted";
        if (retryAfterSeconds >= 0)
            diagnostic += " retry_after_seconds=" + std::to_string(retryAfterSeconds);
        return diagnostic + "]";
    }
    if (reason == "weight_exceeds_budget" && source == "openrouter_credits")
        return " [OpenRouter category=single_request_budget_exceeded "
               "limit_source=openrouter_credits reason=weight_exceeds_budget]";
    return {};
}

class ResponsesClient final : public MaiModelClient {
public:
    explicit ResponsesClient(MaiModelConfig config) : mConfig(std::move(config)) {}

    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }

    MaiModelContextLimits contextLimits(const std::string& model) const override {
        return mConfig.contextLimits ? mConfig.contextLimits(model)
                                     : maiOfficialModelContextLimits(mConfig.baseUrl, model);
    }

    std::string usageProviderForModel(const std::string&) const override {
        return maiModelUsageProviderForConfig(mConfig);
    }

    // 建立 Responses 流并逐事件更新上层，保留取消与传输失败的明确结果。
    // 只在供应商确认完成后结束流，不从连接关闭推断模型已经完成。
    MaiResult<std::size_t> requestBodyBytes(const MaiModelRequest& request) override {
        const auto body =
            buildBody(request, [this](const std::string& source, const std::string& workspace) {
                return prepareImage(source, workspace);
            });
        if (!body) return body.error();
        return body.value().size();
    }

    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (cancel.load(std::memory_order_relaxed))
            return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
        MaiResult<std::string> built =
            buildBody(request, [this](const std::string& source, const std::string& workspace) {
                return prepareImage(source, workspace);
            });
        if (!built) return built.error();
        const std::string body = std::move(built.value());
        std::string url = mConfig.baseUrl;
        if (!url.empty() && url.back() == '/') url.pop_back();
        url += "/responses";
        const std::string originalOrigin = modelOrigin(url);
        if (mConfig.proxyResolver && originalOrigin.empty())
            return MaiError::make(MaiErrorCode::InvalidInput, "model URL is invalid");
        for (int attempt = 0;; ++attempt) {
            const auto networkGeneration = MaiCurlNetworkRoute::generation();
            std::string currentUrl = url;
            bool post = true;
            const auto attemptStarted = std::chrono::steady_clock::now();
            auto lastStreamActivity = attemptStarted;
            auto lastModelProgress = attemptStarted;
            StreamContext context;
            const auto requestCapability = maiCloudModelInfo(request.model);
            char transportError[CURL_ERROR_SIZE] = {};
            CURLcode transfer = CURLE_OK;
            long status = 0;
            for (int redirects = 0;; ++redirects) {
                if (cancel.load(std::memory_order_relaxed))
                    return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                MaiModelProxy proxy;
                if (mConfig.proxyResolver) {
                    const bool resolved = mConfig.proxyResolver(currentUrl, proxy);
                    if (cancel.load(std::memory_order_relaxed))
                        return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                    if (!resolved)
                        return MaiError::make(MaiErrorCode::Network,
                                              "model proxy selection failed");
                }
                MaiCurlNetworkRoute networkRoute;
                auto connection = mConnections.acquire(networkRoute.capturedGeneration());
                CURL* curl = static_cast<CURL*>(connection.nativeHandle());
                if (!curl) return MaiError::make(MaiErrorCode::Internal, "curl_easy_init failed");
                curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                headers = curl_slist_append(headers, "Accept: text/event-stream");
                headers = curl_slist_append(headers, "Cache-Control: no-cache");
                if (!mConfig.apiKey.empty() && modelOrigin(currentUrl) == originalOrigin)
                    headers = curl_slist_append(
                        headers, ("Authorization: Bearer " + mConfig.apiKey).c_str());
                context = StreamContext{};
                context.usageProvider = usageProviderForModel(request.model);
                context.isGlm = requestCapability && requestCapability->provider == "glm";
                if (requestCapability && requestCapability->provider == "openai")
                    for (const auto& tool : request.tools)
                        if (tool.name == "apply_patch" && !tool.inputGrammar.empty())
                            context.customTools.insert(tool.name);
                context.manualRedirects = bool(mConfig.proxyResolver);
                context.sink = &sink;
                context.cancel = &cancel;
                if (mConfig.proxyResolver) {
                    context.lastStreamActivity = lastStreamActivity;
                    context.lastModelProgress = lastModelProgress;
                }
                context.inactivityTimeoutSeconds = mConfig.inactivityTimeoutSeconds;
                context.progressTimeoutSeconds = mConfig.progressTimeoutSeconds;
                transportError[0] = 0;
                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transportError);
                curl_easy_setopt(curl, CURLOPT_URL, currentUrl.c_str());
#if defined(_WIN32)
                // 内嵌 OpenSSL 没有随包 PEM 时使用 Windows 信任根；显式 CA 路径仍保持独立。
                if (mConfig.caBundlePath.empty())
                    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#endif
                if (!mConfig.caBundlePath.empty())
                    curl_easy_setopt(curl, CURLOPT_CAINFO, mConfig.caBundlePath.c_str());
                if (post) {
                    curl_easy_setopt(curl, CURLOPT_POST, 1L);
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                     static_cast<curl_off_t>(body.size()));
                } else {
                    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
                }
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
                curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
                curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, mConfig.connectTimeoutSeconds);
                if (mConfig.totalTimeoutSeconds > 0 && !mConfig.proxyResolver) {
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, mConfig.totalTimeoutSeconds);
                } else if (mConfig.totalTimeoutSeconds > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - attemptStarted)
                                             .count();
                    const auto remaining = mConfig.totalTimeoutSeconds * 1000LL - elapsed;
                    if (remaining <= 0) {
                        curl_slist_free_all(headers);
                        connection.finish(false);
                        // 与 curl 传输内的同一超时一致，仍由外层决定是否重试。
                        transfer = CURLE_OPERATION_TIMEDOUT;
                        break;
                    }
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
                }
                if (mConfig.proxyResolver) {
                    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.url.c_str());
                    curl_easy_setopt(curl, CURLOPT_NOPROXY, proxy.url.empty() ? "*" : "");
                    curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy.username.c_str());
                    curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy.password.c_str());
                }
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, mConfig.proxyResolver ? 0L : 1L);
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
                curl_easy_setopt(curl, CURLOPT_USERAGENT, "MaiAgent/0.1");
                auto routeCode = static_cast<CURLcode>(networkRoute.prepare(
                    curl, currentUrl, !proxy.url.empty(), mConfig.connectTimeoutSeconds * 1000L,
                    [&] { return cancel.load(std::memory_order_relaxed); }));
                if (routeCode == CURLE_OK && mConfig.totalTimeoutSeconds > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - attemptStarted).count();
                    const auto remaining = mConfig.totalTimeoutSeconds * 1000LL - elapsed;
                    routeCode = remaining > 0
                        ? curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining))
                        : CURLE_OPERATION_TIMEDOUT;
                }
                const bool manualRedirects = mConfig.proxyResolver || networkRoute.networkHandle();
                context.manualRedirects = manualRedirects;
                transfer = routeCode == CURLE_OK ? curl_easy_perform(curl) : routeCode;
                std::string remainder;
                if (context.lines.takeRemainder(remainder)) handleLine(context, remainder);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
                char* target = nullptr;
                if (manualRedirects) curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &target);
                const std::string nextUrl = target ? target : "";
                lastStreamActivity = context.lastStreamActivity;
                lastModelProgress = context.lastModelProgress;
                curl_slist_free_all(headers);
                connection.finish(transfer == CURLE_OK ||
                                  (context.done && transfer == CURLE_WRITE_ERROR &&
                                   !cancel.load(std::memory_order_relaxed)));
                if (!manualRedirects || transfer != CURLE_OK || nextUrl.empty() ||
                    cancel.load(std::memory_order_relaxed))
                    break;
                // 与 curl 默认限制一致：最多 30 跳，且只允许默认重定向协议。
                const std::string nextOrigin = modelOrigin(nextUrl);
                if (redirects >= 30 || nextOrigin.empty() ||
                    (nextOrigin.rfind("http://", 0) != 0 && nextOrigin.rfind("https://", 0) != 0 &&
                     nextOrigin.rfind("ftp://", 0) != 0 && nextOrigin.rfind("ftps://", 0) != 0))
                    return MaiError::make(MaiErrorCode::Protocol,
                                          "model redirect is invalid or exceeds its limit");
                // 307/308 会携带原始对话正文；即使去掉认证，也不能将其降级到明文链路。
                if (modelOrigin(currentUrl).rfind("https://", 0) == 0 &&
                    nextOrigin.rfind("https://", 0) != 0)
                    return MaiError::make(
                        MaiErrorCode::Protocol,
                        "model redirect cannot downgrade an encrypted connection");
                if (status == 301 || status == 302 || status == 303) post = false;
                currentUrl = nextUrl;
            }

            const bool canRetry = MaiModelRetry::isTransient(
                status, transfer != CURLE_OK && isRetryable(transfer), context.rawBody);
            const auto advise = [&](MaiError error, bool retryable) {
                // 网络通知只影响下一次用户请求。失败的 POST 可能已被供应商接收，
                // 即使尚无文本输出，也不能因换网产生新的自动重投建议。
                return retryable && networkGeneration == MaiCurlNetworkRoute::generation()
                           ? MaiModelRetry::advise(std::move(error), mConfig, context.sawOutput,
                                                   context.retryAfterDelayMs)
                           : error;
            };
            const MaiError failure = [&]() -> MaiError {
                if (cancel.load(std::memory_order_relaxed))
                    return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                if (context.inactive)
                    return advise(
                        MaiError::make(
                            MaiErrorCode::Network,
                            "model stream transport inactive: no data or keepalive for " +
                                std::to_string(mConfig.inactivityTimeoutSeconds) + " seconds"),
                        true);
                if (context.progressTimedOut)
                    return advise(
                        MaiError::make(MaiErrorCode::Network,
                                       "model stream made no progress for " +
                                           std::to_string(mConfig.progressTimeoutSeconds) +
                                           " seconds"),
                        true);
                if (transfer != CURLE_OK && status != 402 &&
                    !(context.done && transfer == CURLE_WRITE_ERROR))
                    return advise(MaiError::make(MaiErrorCode::Network,
                                                 std::string("curl: ") +
                                                     (transfer == CURLE_PEER_FAILED_VERIFICATION &&
                                                              transportError[0]
                                                          ? transportError
                                                          : curl_easy_strerror(transfer))),
                                  canRetry);
                if (status == 400 && MaiModelRetry::isContextLengthError(context.rawBody))
                    return {MaiErrorCode::ContextWindowExceeded,
                            "The provider rejected the model context length."};
                if (status == 413)
                    return {MaiErrorCode::RequestBodyTooLarge,
                            "HTTP 413: The provider rejected the request body (" +
                                std::to_string(body.size()) + " bytes). " +
                                providerError(context.rawBody)};
                if (status >= 400) {
                    const std::string detail = providerError(context.rawBody);
                    std::string message =
                        "HTTP " + std::to_string(status) + (detail.empty() ? "" : ": " + detail);
                    MaiErrorCode code = status == 401 || status == 403 ? MaiErrorCode::NotConfigured
                                        : status == 429                ? MaiErrorCode::RateLimited
                                        : status >= 500                ? MaiErrorCode::Network
                                                                       : MaiErrorCode::Protocol;
                    const auto capability = maiCloudModelInfo(request.model);
                    if (status == 402 && capability && capability->provider == "openrouter") {
                        const std::string diagnostic = openRouterCreditDiagnostic(
                            context.rawBody, context.retryAfterSeconds, code);
                        message += diagnostic;
                        // 截断 JSON 无法提供分类，但已校验的等待头是独立的 HTTP 事实。
                        // 只保留数字秒，不能从半截正文猜预算来源或触发自动重试。
                        if (transfer != CURLE_OK && diagnostic.empty() &&
                            context.retryAfterSeconds >= 0)
                            message += " [OpenRouter retry_after_seconds=" +
                                       std::to_string(context.retryAfterSeconds) + "]";
                    }
                    return advise(MaiError::make(code, message), canRetry);
                }
                if (context.businessFailure) return context.businessFailure;
                const Json businessBody = Json::parse(context.rawBody, nullptr, false);
                if (const auto error = businessError(businessBody, context.isGlm)) return error;
                if (!context.done)
                    return advise(MaiError::make(MaiErrorCode::Protocol,
                                                 "Responses stream ended without a terminal event"),
                                  true);
                if (context.contextLengthError)
                    return {MaiErrorCode::ContextWindowExceeded, context.terminalError};
                if (!context.terminalError.empty())
                    return advise(MaiError::make(MaiErrorCode::Protocol, context.terminalError),
                                  context.transientStreamError);

                return {};
            }();
            if (failure) {
                const auto& advice = failure.retryAdvice();
                // 直接调用只恢复无输出尝试；Agent 请求由 TurnRunner 重建上下文后恢复。
                if (!request.callerManagesRetries && advice && !advice->sawOutput &&
                    attempt < advice->maxRetries) {
                    if (MaiModelRetry::wait(MaiModelRetry::delayMilliseconds(*advice, attempt),
                                            cancel) == MaiRetryWaitResult::Canceled)
                        return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                    continue;
                }
                return failure;
            }
            Json output = context.hasTerminalOutput ? context.terminalOutput : Json::array();
            bool hasReplayableOutput = context.hasTerminalOutput;
            if (!context.hasTerminalOutput && !context.completedOutput.empty()) {
                // 一些兼容服务只在 item.done 提供完整项，终态不重复 output。
                // 仅字段缺失才允许此回退，显式终态数组（包括空数组）始终权威。
                // 完成前缀连续仍可能漏掉尾部，必须覆盖全部已观察索引才算完整快照。
                for (const int index : context.observedOutputIndices)
                    if (context.completedOutput.find(index) == context.completedOutput.end())
                        return {MaiErrorCode::Protocol,
                                "Responses output snapshot has unfinished items"};
                output = Json::array();
                int nextIndex = 0;
                for (const auto& [index, item] : context.completedOutput) {
                    if (index != nextIndex++)
                        return {MaiErrorCode::Protocol,
                                "Responses output snapshot has missing items"};
                    output.push_back(item);
                }
                hasReplayableOutput = true;
            }
            if (hasReplayableOutput) {
                if (const MaiError error = validateOutput(output)) return error;
                if (output.empty() && context.sawOutput) {
                    // 显式空终态不能复活此前已完成的 item 或工具；矛盾响应不发布状态或调用。
                    if (!context.completedOutput.empty() || !context.calls.empty())
                        return {MaiErrorCode::Protocol,
                                "Responses terminal output conflicts with completed items"};
                    // 仅有显示增量、没有原 item 的旧兼容响应仍可显示，不能让空快照替代真实历史。
                    hasReplayableOutput = false;
                }
            }
            for (const auto& [_, call] : context.calls) {
                if (!call.complete || call.invalid || !call.hasInput || call.id.empty() ||
                    call.name.empty() ||
                    (call.type == "custom_tool_call" &&
                     (!context.customTools.count(call.name) || context.malformedEvent)) ||
                    (call.type == "function_call" && context.customTools.count(call.name)))
                    return MaiError::make(MaiErrorCode::Protocol,
                                          "Responses returned an invalid or undeclared tool call");
            }
            if (hasReplayableOutput) {
                std::size_t callIndex = 0;
                auto projected = context.calls.begin();
                for (const Json& item : output) {
                    const std::string type = stringField(item, "type");
                    if (type != "function_call" && type != "custom_tool_call") continue;
                    if (projected == context.calls.end() || projected->second.type != type ||
                        projected->second.id != stringField(item, "call_id") ||
                        projected->second.name != stringField(item, "name") ||
                        projected->second.arguments !=
                            stringField(item, type == "custom_tool_call" ? "input" : "arguments"))
                        return {MaiErrorCode::Protocol,
                                "Responses output does not match its function calls"};
                    ++projected;
                    ++callIndex;
                }
                if (callIndex != context.calls.size())
                    return {MaiErrorCode::Protocol,
                            "Responses output does not contain every function call"};
            }
            if (cancel.load(std::memory_order_relaxed))
                return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
            // 只有终态快照的供应商在完整校验后补交原始完成项。
            // 已交付的 item.done 由规范快照去重，不重复记录相同对象。
            if (hasReplayableOutput)
                for (std::size_t index = 0; index < output.size(); ++index)
                    publishCompletedOutput(context, static_cast<int>(index), output[index]);
            // 原始完成事实的持久化回调也可能触发取消；此后不能再发布可重放成功状态。
            if (cancel.load(std::memory_order_relaxed))
                return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
            const auto capability = maiCloudModelInfo(request.model);
            if (hasReplayableOutput && capability && sink.onContinuationState) {
                const MaiModelContinuationState state{"responses.output.v1", capability->provider,
                                                      request.model, output.dump()};
                sink.onContinuationState(state);
            }
            // 持久化回调期间也可能取消，不能在它返回后继续交付任何可执行工具。
            if (cancel.load(std::memory_order_relaxed))
                return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
            for (const auto& [_, call] : context.calls) {
                if (cancel.load(std::memory_order_relaxed))
                    return MaiError::make(MaiErrorCode::Canceled, "canceled by user");
                if (sink.onToolCall)
                    sink.onToolCall(MaiToolInvocation{call.id, call.name,
                                                      call.type == "custom_tool_call"
                                                          ? Json{{"patch", call.arguments}}.dump()
                                                          : call.arguments});
            }
            return {};
        }
    }

private:
    MaiResult<std::string> prepareImage(const std::string& source, const std::string& workspace) {
        if (!mConfig.prepareImage)
            return {MaiErrorCode::InvalidInput,
                    "image exceeds the 2 MB model request budget; resize it before retrying"};
        const std::string key = workspace + "\n" + source;
        const auto path = MaiFilePath::fromUtf8(source);
        MaiRegularFileInfo before;
        const bool known = MaiFileSystem::regularFileInfo(path, before);
        const auto same = [](const MaiRegularFileInfo& a, const MaiRegularFileInfo& b) {
            return a.bytes == b.bytes && a.modified == b.modified && a.identity == b.identity;
        };
        std::lock_guard<std::mutex> lock(mPreviewMutex);
        const auto existing = mPreviewCache.find(key);
        if (known && existing != mPreviewCache.end() && same(before, existing->second.first) &&
            MaiFileSystem::exists(MaiFilePath::fromUtf8(existing->second.second)))
            return existing->second.second;
        mPreviewCache.erase(key);
        MaiResult<std::string> result = mConfig.prepareImage(source, workspace);
        MaiRegularFileInfo after;
        // 跨请求复用必须核对真实文件身份；生成期间改变或无法确认的来源不能缓存。
        // 长寿命客户端限制缓存条目，不能因历史附件无限增长。
        if (result && known && MaiFileSystem::regularFileInfo(path, after) && same(before, after)) {
            // 图片多的会话每次请求都重发全部历史图片；整表清空会让每次请求都重新转码。
            // 条目只含文件元数据与路径，放宽上限并逐个淘汰。
            if (mPreviewCache.size() >= 1024) mPreviewCache.erase(mPreviewCache.begin());
            mPreviewCache[key] = {after, result.value()};
        }
        return result;
    }

    MaiModelConfig mConfig;
    MaiModelConnectionPool mConnections;
    std::mutex mPreviewMutex;
    std::unordered_map<std::string, std::pair<MaiRegularFileInfo, std::string>> mPreviewCache;
};

}  // namespace

std::unique_ptr<MaiModelClient> makeMaiResponsesClient(MaiModelConfig config) {
    // libcurl 的全局初始化在所有远端入口共用，不能依赖已经移除的旧协议工厂。
    struct CurlGlobal {
        CurlGlobal() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        }
        ~CurlGlobal() {
            curl_global_cleanup();
        }
    };
    static const CurlGlobal global;
    (void)global;
    if (config.wire != MaiWireApi::Responses) {
        class InvalidRemoteConfiguration final : public MaiModelClient {
        public:
            explicit InvalidRemoteConfiguration(MaiWireApi wire) : mWire(wire) {}
            MaiWireApi wireApi() const override {
                return mWire;
            }
            MaiError stream(const MaiModelRequest&, const MaiStreamSink&,
                            const std::atomic<bool>& cancel) override {
                if (cancel.load()) return {MaiErrorCode::Canceled, "Model request canceled."};
                return mWire == MaiWireApi::Local
                           ? MaiError{MaiErrorCode::NotConfigured,
                                      "Local models require makeMaiLlamaModelClient and a GGUF "
                                      "file."}
                           : MaiError{MaiErrorCode::InvalidInput,
                                      "Mixed describes routes, not a remote transport."};
            }

        private:
            MaiWireApi mWire;
        };
        return std::make_unique<InvalidRemoteConfiguration>(config.wire);
    }
    return std::make_unique<ResponsesClient>(std::move(config));
}

std::unique_ptr<MaiModelClient> makeMaiModelClient(MaiModelConfig config) {
    return makeMaiResponsesClient(std::move(config));
}

std::string maiModelUsageProviderForConfig(const MaiModelConfig& config) {
    if (config.wire != MaiWireApi::Responses) return {};
    const auto origin = modelOrigin(config.baseUrl);
    if (origin.empty()) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int count = 0;
    if (EVP_Digest(origin.data(), origin.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1)
        return {};
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result = "origin-sha256:";
    for (unsigned int index = 0; index < count; ++index) {
        result += hexadecimal[bytes[index] >> 4];
        result += hexadecimal[bytes[index] & 15];
    }
    return result;
}
