// 文件作用：无损编码内部持久化值，复用现有工具执行事实与轮次状态语义。
#include "MaiSessionRecordCodec.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

#include <json.hpp>

namespace {
using Json = nlohmann::json;

MaiError invalidRecord() {
    return {MaiErrorCode::InvalidInput, "Invalid or unsupported session persistence record"};
}

// 与现有历史/工具输出的严格标量检查一致；非法源字节不能被 replacement 丢失。
bool validUtf8(const std::string& text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        if (lead < 0x80) {
            ++offset;
            continue;
        }
        const std::size_t count = lead >= 0xc2 && lead <= 0xdf   ? 2
                                  : lead >= 0xe0 && lead <= 0xef ? 3
                                  : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                 : 0;
        if (count == 0 || count > text.size() - offset) return false;
        std::uint32_t scalar = lead & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
        for (std::size_t index = 1; index < count; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if ((byte & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        const std::uint32_t minimum = count == 2 ? 0x80 : count == 3 ? 0x800 : 0x10000;
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
        offset += count;
    }
    return true;
}

const char base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encodeBase64(const std::string& bytes) {
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        const auto second =
            index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const auto third =
            index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        result.push_back(base64Alphabet[first >> 2]);
        result.push_back(base64Alphabet[((first & 3) << 4) | (second >> 4)]);
        result.push_back(
            index + 1 < bytes.size() ? base64Alphabet[((second & 15) << 2) | (third >> 6)] : '=');
        result.push_back(index + 2 < bytes.size() ? base64Alphabet[third & 63] : '=');
    }
    return result;
}

bool decodeBase64(const std::string& text, std::string& output) {
    if (text.size() % 4 != 0) return false;
    output.clear();
    for (std::size_t index = 0; index < text.size(); index += 4) {
        unsigned values[4] = {};
        for (std::size_t part = 0; part < 4; ++part) {
            const char byte = text[index + part];
            if (byte == '=') {
                if (part < 2 || index + 4 != text.size()) return false;
                continue;
            }
            if (part > 0 && text[index + part - 1] == '=') return false;
            const char* found = std::char_traits<char>::find(base64Alphabet, 64, byte);
            if (!found) return false;
            values[part] = static_cast<unsigned>(found - base64Alphabet);
        }
        output.push_back(static_cast<char>((values[0] << 2) | (values[1] >> 4)));
        if (text[index + 2] != '=')
            output.push_back(static_cast<char>((values[1] << 4) | (values[2] >> 2)));
        if (text[index + 3] != '=')
            output.push_back(static_cast<char>((values[2] << 6) | values[3]));
    }
    // 校验 padding 的未使用位，拒绝同一原文的非规范别名。
    return encodeBase64(output) == text;
}

void protectStrings(Json& value) {
    if (value.is_string()) {
        const auto bytes = value.get<std::string>();
        if (!validUtf8(bytes)) value = {{"$mai_bytes", "base64"}, {"data", encodeBase64(bytes)}};
    } else if (value.is_array() || value.is_object()) {
        for (auto& child : value) protectStrings(child);
    }
}

bool restoreStrings(Json& value) {
    if (value.is_object() && value.contains("$mai_bytes")) {
        if (value.size() != 2 || value["$mai_bytes"] != "base64" || !value.contains("data") ||
            !value["data"].is_string())
            return false;
        std::string bytes;
        if (!decodeBase64(value["data"].get<std::string>(), bytes) || validUtf8(bytes))
            return false;
        value = std::move(bytes);
    } else if (value.is_array() || value.is_object()) {
        for (auto& child : value)
            if (!restoreStrings(child)) return false;
    }
    return true;
}

// 仅用于原始工具输出的二进制字段；普通文本仍执行 invalid-UTF8-only 包装约束。
// 先恢复这个字段，再处理其他字符串，保留未发布旧格式的普通字符串读取兼容性。
bool restoreToolOutputBytes(Json& record) {
    if (!record.is_object()) return true;
    const auto found = record.find("text");
    if (found == record.end() || !found->is_object() || !found->contains("$mai_binary"))
        return true;
    if (found->size() != 2 || (*found)["$mai_binary"] != "base64" || !found->contains("data") ||
        !(*found)["data"].is_string())
        return false;
    std::string bytes;
    if (!decodeBase64((*found)["data"].get<std::string>(), bytes)) return false;
    *found = std::move(bytes);
    return true;
}

bool read(const Json& object, const char* field, std::string& value) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_string()) return false;
    value = found->get<std::string>();
    return true;
}

bool read(const Json& object, const char* field, bool& value) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_boolean()) return false;
    value = found->get<bool>();
    return true;
}

template <typename Integer,
          std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>, int> = 0>
bool read(const Json& object, const char* field, Integer& value) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_number_integer()) return false;
    if (found->is_number_unsigned()) {
        const auto number = found->get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) return false;
        value = static_cast<Integer>(number);
    } else {
        const auto number = found->get<std::int64_t>();
        if constexpr (std::is_unsigned_v<Integer>) {
            if (number < 0 ||
                static_cast<std::uint64_t>(number) > std::numeric_limits<Integer>::max())
                return false;
        } else if (number < std::numeric_limits<Integer>::min() ||
                   number > std::numeric_limits<Integer>::max())
            return false;
        value = static_cast<Integer>(number);
    }
    return true;
}

template <typename Enum, typename Name>
Json enumValue(Enum value, std::initializer_list<Enum> known, Name name) {
    for (auto candidate : known)
        if (candidate == value) return name(value);
    return nullptr;
}

template <typename Enum, typename Name>
bool readEnum(const Json& object, const char* field, Enum& result,
              std::initializer_list<Enum> known, Name name) {
    std::string value;
    if (!read(object, field, value)) return false;
    for (auto candidate : known)
        if (value == name(candidate)) {
            result = candidate;
            return true;
        }
    return false;
}

const std::initializer_list<MaiErrorCode> errorCodes = {MaiErrorCode::Ok,
                                                        MaiErrorCode::NotFound,
                                                        MaiErrorCode::Busy,
                                                        MaiErrorCode::InvalidInput,
                                                        MaiErrorCode::NotConfigured,
                                                        MaiErrorCode::Network,
                                                        MaiErrorCode::RateLimited,
                                                        MaiErrorCode::Protocol,
                                                        MaiErrorCode::Canceled,
                                                        MaiErrorCode::NotSupported,
                                                        MaiErrorCode::Internal,
                                                        MaiErrorCode::ContextWindowExceeded,
                                                        MaiErrorCode::RequestBodyTooLarge};
const std::initializer_list<MaiTurnStatus> turnStatuses = {
    MaiTurnStatus::Running, MaiTurnStatus::Completed, MaiTurnStatus::Failed,
    MaiTurnStatus::Interrupted};
const std::initializer_list<MaiContextPhase> contextPhases = {
    MaiContextPhase::Preparing, MaiContextPhase::Summarizing, MaiContextPhase::Ready,
    MaiContextPhase::Deferred,  MaiContextPhase::Failed,      MaiContextPhase::Interrupted};
const std::initializer_list<MaiToolState> toolStates = {
    MaiToolState::Pending, MaiToolState::Running, MaiToolState::Completed, MaiToolState::Error,
    MaiToolState::Canceled};

Json errorJson(const MaiError& value) {
    Json result{{"error_code", enumValue(value.code(), errorCodes, maiErrorCodeToString)},
                {"error", value.message()}};
    if (value.retryAdvice()) {
        const auto& advice = *value.retryAdvice();
        result["retry_advice"] = {{"max_retries", advice.maxRetries},
                                  {"initial_delay_ms", advice.initialDelayMs},
                                  {"retry_after_ms", advice.retryAfterMs},
                                  {"saw_output", advice.sawOutput}};
    }
    return result;
}

bool readError(const Json& value, MaiError& result) {
    MaiErrorCode code;
    std::string message;
    if (!readEnum(value, "error_code", code, errorCodes, maiErrorCodeToString) ||
        !read(value, "error", message))
        return false;
    result = {code, std::move(message)};
    if (const auto found = value.find("retry_advice"); found != value.end()) {
        if (!found->is_object()) return false;
        MaiRetryAdvice advice;
        if (!read(*found, "max_retries", advice.maxRetries) ||
            !read(*found, "initial_delay_ms", advice.initialDelayMs) ||
            !read(*found, "retry_after_ms", advice.retryAfterMs) ||
            !read(*found, "saw_output", advice.sawOutput) || advice.maxRetries < 0 ||
            advice.initialDelayMs < 0 || advice.retryAfterMs < -1)
            return false;
        result.withRetryAdvice(advice);
    }
    return true;
}

Json contextJson(const MaiContextStatus& value) {
    Json result{{"phase", enumValue(value.phase, contextPhases, maiContextPhaseToString)},
                {"before", value.before.tokens},
                {"after", value.after.tokens},
                {"before_exact", value.before.exact},
                {"after_exact", value.after.exact},
                {"window", value.windowTokens},
                {"input_budget", value.inputBudgetTokens},
                {"revision", value.checkpointRevision},
                {"original_users", value.originalUserMessages},
                {"retained_users", value.retainedUserMessages},
                {"reused", value.reusedCheckpoint},
                {"advanced", value.advancedCheckpoint},
                {"history_index", value.historyIndexRecovery},
                {"output_tokens", value.outputTokens},
                {"request_body",
                 {{"reduced", value.requestBodyReduced},
                  {"bytes", value.requestBodyBytes},
                  {"limit", value.requestBodyLimitBytes},
                  {"omitted_images", value.requestBodyOmittedImages},
                  {"truncated_texts", value.requestBodyTruncatedTexts}}}};
    const auto usage = MaiSessionRecordCodec::encodeContextUsage(value.usage);
    result["usage"] = usage ? Json::parse(usage.value()) : Json();
    result.update(errorJson(value.error));
    return result;
}

bool readContext(const Json& value, MaiContextStatus& result, bool legacy) {
    if (!value.is_object() ||
        !readEnum(value, "phase", result.phase, contextPhases, maiContextPhaseToString))
        return false;
#define FIELD(member, key) \
    if (!read(value, key, result.member)) return false
    FIELD(before.tokens, "before");
    FIELD(after.tokens, "after");
    FIELD(before.exact, "before_exact");
    FIELD(after.exact, "after_exact");
    FIELD(windowTokens, "window");
    FIELD(inputBudgetTokens, "input_budget");
    FIELD(checkpointRevision, "revision");
    FIELD(originalUserMessages, "original_users");
    FIELD(retainedUserMessages, "retained_users");
    FIELD(reusedCheckpoint, "reused");
    FIELD(advancedCheckpoint, "advanced");
    FIELD(historyIndexRecovery, "history_index");
    if (!legacy || value.contains("output_tokens")) {
        FIELD(outputTokens, "output_tokens");
    }
#undef FIELD
    if (!readError(value, result.error)) return false;
    // 旧的 version=1 轮次没有 usage 字段；缺失只能保留未知，不能从估计回填。
    if (const auto usage = value.find("usage"); usage != value.end()) {
        const auto decoded = MaiSessionRecordCodec::decodeContextUsage(usage->dump());
        if (!decoded) return false;
        result.usage = decoded.value();
    }
    const auto found = value.find("request_body");
    if (found == value.end()) return legacy;
    const auto& body = *found;
    return body.is_object() && read(body, "reduced", result.requestBodyReduced) &&
           read(body, "bytes", result.requestBodyBytes) &&
           read(body, "limit", result.requestBodyLimitBytes) &&
           read(body, "omitted_images", result.requestBodyOmittedImages) &&
           read(body, "truncated_texts", result.requestBodyTruncatedTexts);
}

Json turnJson(const MaiMessage& message) {
    Json result{{"codec_version", 1}, {"kind", "turn_state"}, {"turn_id", message.turnId}};
    if (message.turnOutcome) {
        const auto& value = *message.turnOutcome;
        Json outcome{{"turn_id", value.turnId},
                     {"status", enumValue(value.status, turnStatuses, maiTurnStatusToString)},
                     {"result_message_id", value.resultMessageId},
                     {"completed", value.completed}};
        outcome.update(errorJson(value.error));
        if (value.context) outcome["context"] = contextJson(*value.context);
        result["outcome"] = std::move(outcome);
    }
    return result;
}

bool envelope(const Json& value, const char* kind) {
    int version = 0;
    std::string actualKind;
    return value.is_object() && read(value, "codec_version", version) && version == 1 &&
           read(value, "kind", actualKind) && actualKind == kind;
}

bool readTurn(const Json& value, MaiMessage& result, bool allowLegacy) {
    if (!value.is_object()) return false;
    const bool legacy = !value.contains("codec_version") && allowLegacy;
    if (legacy && value.contains("kind")) return false;
    if (!legacy && !envelope(value, "turn_state")) return false;
    result.turnOutcome.reset();
    if (!read(value, "turn_id", result.turnId)) return false;
    const auto found = value.find("outcome");
    if (found == value.end()) return true;
    if (!found->is_object()) return false;
    const auto& outcome = *found;
    MaiTurnOutcome parsed;
    if (!read(outcome, "turn_id", parsed.turnId) ||
        !readEnum(outcome, "status", parsed.status, turnStatuses, maiTurnStatusToString) ||
        !read(outcome, "result_message_id", parsed.resultMessageId) ||
        !read(outcome, "completed", parsed.completed) || !readError(outcome, parsed.error) ||
        result.role != MaiRole::Assistant || parsed.turnId.empty() ||
        parsed.turnId != result.turnId || parsed.turnId != result.id ||
        (parsed.status == MaiTurnStatus::Running && parsed.completed != 0))
        return false;
    if (const auto context = outcome.find("context"); context != outcome.end()) {
        MaiContextStatus status;
        if (!readContext(*context, status, legacy)) return false;
        parsed.context = std::move(status);
    }
    result.turnOutcome = std::move(parsed);
    return true;
}

Json executionJson(const MaiToolExecution& value) {
    if (static_cast<int>(value.status) < 0 ||
        static_cast<int>(value.status) > static_cast<int>(MaiToolExecutionStatus::Unknown) ||
        static_cast<int>(value.effectStatus) < 0 ||
        static_cast<int>(value.effectStatus) > static_cast<int>(MaiToolEffectStatus::Confirmed) ||
        static_cast<int>(value.nextAction) < 0 ||
        static_cast<int>(value.nextAction) > static_cast<int>(MaiToolNextAction::AskUser))
        return nullptr;
    // 复用事实序列化的枚举/可选值语义，但先移除任意字节字段，避免旧 helper 的 replacement。
    MaiToolExecution plain = value;
    plain.outputId.clear();
    plain.taskId.clear();
    plain.recoveryTool.clear();
    Json result = Json::parse(maiSerializeToolExecution(plain), nullptr, false);
    result["output_id"] = value.outputId;
    result["task_id"] = value.taskId;
    result["recovery_tool"] = value.recoveryTool;
    return result;
}

bool readExecution(const Json& value, MaiToolExecution& result) {
    if (!value.is_object()) return false;
    std::string outputId, taskId, recoveryTool;
    if (!read(value, "output_id", outputId) || !read(value, "task_id", taskId) ||
        !read(value, "recovery_tool", recoveryTool))
        return false;
    bool flag = false;
    if (!read(value, "timed_out", flag) || !read(value, "canceled", flag) ||
        !read(value, "output_truncated", flag))
        return false;
    Json plain = value;
    plain["output_id"] = "";
    plain["task_id"] = "";
    plain["recovery_tool"] = "";
    if (!maiParseToolExecution(plain.dump(), result)) return false;
    result.outputId = std::move(outputId);
    result.taskId = std::move(taskId);
    result.recoveryTool = std::move(recoveryTool);
    return true;
}

Json partJson(const MaiMessagePart& value) {
    Json result{{"id", value.id}, {"created", value.created}, {"model_draft", value.modelDraft}};
    std::visit(
        [&](const auto& body) {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, MaiTextPart>) {
                result["body"] = {{"type", "text"}, {"text", body.text}};
            } else if constexpr (std::is_same_v<Body, MaiReasoningPart>) {
                result["body"] = {{"type", "reasoning"}, {"text", body.text}};
                if (body.continuation) {
                    const auto& state = *body.continuation;
                    result["body"]["continuation"] = {{"format", state.format},
                                                      {"provider", state.provider},
                                                      {"model", state.model},
                                                      {"payload", state.payload}};
                }
            } else if constexpr (std::is_same_v<Body, MaiToolPart>) {
                result["body"] = {
                    {"type", "tool"},
                    {"tool", body.tool},
                    {"call_id", body.callId},
                    {"input", body.input},
                    {"output", body.output},
                    {"error", body.error},
                    {"state", enumValue(body.state, toolStates, maiToolStateToString)},
                    {"model_output_version", body.modelOutputVersion}};
                if (body.execution) result["body"]["execution"] = executionJson(*body.execution);
            } else if constexpr (std::is_same_v<Body, MaiQuotePart>) {
                result["body"] = {
                    {"type", "quote"}, {"message_id", body.messageId}, {"preview", body.preview}};
            } else {
                result["body"] = {{"type", std::is_same_v<Body, MaiImagePart> ? "image" : "video"},
                                  {"path", body.path},
                                  {"mime_type", body.mimeType}};
            }
        },
        value.body);
    return result;
}

bool readPart(const Json& value, MaiMessagePart& result) {
    if (!value.is_object() || !read(value, "id", result.id) || result.id.empty() ||
        !read(value, "created", result.created) || !read(value, "model_draft", result.modelDraft))
        return false;
    const auto found = value.find("body");
    if (found == value.end() || !found->is_object()) return false;
    const auto& body = *found;
    std::string type;
    if (!read(body, "type", type)) return false;
    if (type == "text") {
        MaiTextPart parsed;
        if (!read(body, "text", parsed.text)) return false;
        result.body = std::move(parsed);
    } else if (type == "reasoning") {
        MaiReasoningPart parsed;
        if (!read(body, "text", parsed.text)) return false;
        if (const auto state = body.find("continuation"); state != body.end()) {
            MaiModelContinuationState continuation;
            if (!state->is_object() || !read(*state, "format", continuation.format) ||
                !read(*state, "provider", continuation.provider) ||
                !read(*state, "model", continuation.model) ||
                !read(*state, "payload", continuation.payload))
                return false;
            parsed.continuation = std::move(continuation);
        }
        result.body = std::move(parsed);
    } else if (type == "tool") {
        MaiToolPart parsed;
        if (!read(body, "tool", parsed.tool) || !read(body, "call_id", parsed.callId) ||
            !read(body, "input", parsed.input) || !read(body, "output", parsed.output) ||
            !read(body, "error", parsed.error) ||
            !readEnum(body, "state", parsed.state, toolStates, maiToolStateToString) ||
            !read(body, "model_output_version", parsed.modelOutputVersion) ||
            parsed.modelOutputVersion > 1)
            return false;
        if (const auto execution = body.find("execution"); execution != body.end()) {
            MaiToolExecution facts;
            if (!readExecution(*execution, facts)) return false;
            parsed.execution = std::move(facts);
        }
        result.body = std::move(parsed);
    } else if (type == "quote") {
        MaiQuotePart parsed;
        if (!read(body, "message_id", parsed.messageId) || parsed.messageId.empty() ||
            !read(body, "preview", parsed.preview))
            return false;
        result.body = std::move(parsed);
    } else if (type == "image" || type == "video") {
        std::string path, mime;
        if (!read(body, "path", path) || !read(body, "mime_type", mime)) return false;
        if (type == "image")
            result.body = MaiImagePart{std::move(path), std::move(mime)};
        else
            result.body = MaiVideoPart{std::move(path), std::move(mime)};
    } else
        return false;
    return true;
}

Json toRecord(const MaiSession& value) {
    return {{"codec_version", 1},
            {"kind", "session"},
            {"id", value.id},
            {"title", value.title},
            {"directory", value.directory},
            {"model", value.model},
            {"agent", value.agent},
            {"parent_id", value.parentId},
            {"depth", value.depth},
            {"created", value.created},
            {"updated", value.updated},
            {"title_edited", value.titleEdited},
            {"reasoning_effort", value.reasoningEffort}};
}

bool fromRecord(const Json& value, MaiSession& result) {
    if (!envelope(value, "session")) return false;
#define FIELD(member, key) \
    if (!read(value, key, result.member)) return false
    FIELD(id, "id");
    FIELD(title, "title");
    FIELD(directory, "directory");
    FIELD(model, "model");
    FIELD(agent, "agent");
    FIELD(parentId, "parent_id");
    FIELD(depth, "depth");
    FIELD(created, "created");
    FIELD(updated, "updated");
    FIELD(titleEdited, "title_edited");
    FIELD(reasoningEffort, "reasoning_effort");
#undef FIELD
    return !result.id.empty() && result.depth >= 0 && result.parentId != result.id &&
           (result.parentId.empty() == (result.depth == 0));
}

Json toRecord(const MaiMessagePart& value) {
    auto result = partJson(value);
    result["codec_version"] = 1;
    result["kind"] = "part";
    return result;
}
bool fromRecord(const Json& value, MaiMessagePart& result) {
    return envelope(value, "part") && readPart(value, result);
}

Json toRecord(const MaiMessage& value) {
    Json result{
        {"codec_version", 1},
        {"kind", "message"},
        {"id", value.id},
        {"role", enumValue(value.role, {MaiRole::User, MaiRole::Assistant}, maiRoleToString)},
        {"created", value.created},
        {"completed", value.completed},
        {"input_request_id", value.inputRequestId},
        {"pending_input", value.pendingInput},
        {"turn_state", turnJson(value)},
        {"parts", Json::array()}};
    for (const auto& part : value.parts) result["parts"].push_back(partJson(part));
    if (value.contextImportOrigin)
        result["context_import_origin"] = {{"source_key", value.contextImportOrigin->sourceKey},
                                           {"source_id", value.contextImportOrigin->sourceId}};
    return result;
}

bool fromRecord(const Json& value, MaiMessage& result) {
    if (!envelope(value, "message") || !read(value, "id", result.id) || result.id.empty() ||
        !readEnum(value, "role", result.role, {MaiRole::User, MaiRole::Assistant},
                  maiRoleToString) ||
        !read(value, "created", result.created) || !read(value, "completed", result.completed) ||
        !read(value, "input_request_id", result.inputRequestId) ||
        !read(value, "pending_input", result.pendingInput))
        return false;
    const auto parts = value.find("parts");
    const auto turn = value.find("turn_state");
    if (parts == value.end() || !parts->is_array() || turn == value.end() ||
        !readTurn(*turn, result, false))
        return false;
    std::set<std::string> ids;
    for (const auto& partRecord : *parts) {
        MaiMessagePart part;
        if (!readPart(partRecord, part) || !ids.insert(part.id).second) return false;
        result.parts.push_back(std::move(part));
    }
    if (const auto origin = value.find("context_import_origin"); origin != value.end()) {
        MaiContextImportOrigin parsed;
        if (!origin->is_object() || !read(*origin, "source_key", parsed.sourceKey) ||
            !read(*origin, "source_id", parsed.sourceId) || parsed.sourceKey.empty() ||
            parsed.sourceId.empty())
            return false;
        result.contextImportOrigin = std::move(parsed);
    }
    return true;
}

Json toRecord(const MaiContextCheckpoint& value) {
    return {{"codec_version", 1},
            {"kind", "checkpoint"},
            {"session_id", value.sessionId},
            {"revision", value.revision},
            {"source_message_count", value.sourceMessageCount},
            {"first_message_id", value.firstMessageId},
            {"through_message_id", value.throughMessageId},
            {"source_fingerprint", value.sourceFingerprint},
            {"model", value.model},
            {"summary", value.summary},
            {"references", value.references},
            {"created", value.created},
            {"partial_message_id", value.partialMessageId},
            {"partial_part_count", value.partialPartCount},
            {"through_part_id", value.throughPartId},
            {"partial_source_fingerprint", value.partialSourceFingerprint},
            {"preserved_user_message_id", value.preservedUserMessageId}};
}

bool fromRecord(const Json& value, MaiContextCheckpoint& result) {
    if (!envelope(value, "checkpoint")) return false;
#define FIELD(member, key) \
    if (!read(value, key, result.member)) return false
    FIELD(sessionId, "session_id");
    FIELD(revision, "revision");
    FIELD(sourceMessageCount, "source_message_count");
    FIELD(firstMessageId, "first_message_id");
    FIELD(throughMessageId, "through_message_id");
    FIELD(sourceFingerprint, "source_fingerprint");
    FIELD(model, "model");
    FIELD(summary, "summary");
    FIELD(references, "references");
    FIELD(created, "created");
    FIELD(partialMessageId, "partial_message_id");
    FIELD(partialPartCount, "partial_part_count");
    FIELD(throughPartId, "through_part_id");
    FIELD(partialSourceFingerprint, "partial_source_fingerprint");
    FIELD(preservedUserMessageId, "preserved_user_message_id");
#undef FIELD
    if (result.sessionId.empty()) return false;
    if (result.sourceMessageCount != 0 &&
        (result.firstMessageId.empty() || result.throughMessageId.empty()))
        return false;
    if (result.partialMessageId.empty())
        return result.partialPartCount == 0 && result.throughPartId.empty() &&
               result.partialSourceFingerprint.empty() && result.preservedUserMessageId.empty();
    return result.partialPartCount != 0 && !result.throughPartId.empty() &&
           !result.partialSourceFingerprint.empty() && !result.preservedUserMessageId.empty();
}

Json toRecord(const MaiToolOutputRecord& value) {
    return {{"codec_version", 1},
            {"kind", "tool_output"},
            {"id", value.id},
            {"session_id", value.sessionId},
            {"message_id", value.messageId},
            {"part_id", value.partId},
            {"call_id", value.callId},
            {"text", value.text},
            {"truncated", value.truncated},
            {"created", value.created}};
}

bool fromRecord(const Json& value, MaiToolOutputRecord& result) {
    if (!envelope(value, "tool_output")) return false;
#define FIELD(member, key) \
    if (!read(value, key, result.member)) return false
    FIELD(id, "id");
    FIELD(sessionId, "session_id");
    FIELD(messageId, "message_id");
    FIELD(partId, "part_id");
    FIELD(callId, "call_id");
    FIELD(text, "text");
    FIELD(truncated, "truncated");
    FIELD(created, "created");
#undef FIELD
    return !maiValidateToolOutputRecord(result);
}

template <typename Value>
MaiResult<std::string> encode(const Value& value) {
    try {
        auto record = toRecord(value);
        Value validated;
        if (!fromRecord(record, validated)) return invalidRecord();
        // 原始捕获按二进制计算容量，不让 NUL 等合法 UTF-8 字节产生六倍 JSON 膨胀。
        if constexpr (std::is_same_v<Value, MaiToolOutputRecord>) {
            record["text"] = {{"$mai_binary", "base64"}, {"data", encodeBase64(value.text)}};
        }
        protectStrings(record);
        return record.dump();
    } catch (const Json::exception&) {
        return invalidRecord();
    }
}

template <typename Value>
MaiResult<Value> decode(const std::string& source) {
    try {
        auto record = Json::parse(source, nullptr, false);
        if constexpr (std::is_same_v<Value, MaiToolOutputRecord>) {
            if (!restoreToolOutputBytes(record)) return invalidRecord();
        }
        if (record.is_discarded() || !restoreStrings(record)) return invalidRecord();
        Value value;
        if (!fromRecord(record, value)) return invalidRecord();
        return value;
    } catch (const Json::exception&) {
        return invalidRecord();
    }
}
}  // namespace

MaiResult<std::string> MaiSessionRecordCodec::encodeSession(const MaiSession& value) {
    return encode(value);
}
MaiResult<MaiSession> MaiSessionRecordCodec::decodeSession(const std::string& source) {
    return decode<MaiSession>(source);
}
MaiResult<std::string> MaiSessionRecordCodec::encodeMessage(const MaiMessage& value) {
    return encode(value);
}
MaiResult<MaiMessage> MaiSessionRecordCodec::decodeMessage(const std::string& source) {
    return decode<MaiMessage>(source);
}
MaiResult<std::string> MaiSessionRecordCodec::encodePart(const MaiMessagePart& value) {
    return encode(value);
}
MaiResult<MaiMessagePart> MaiSessionRecordCodec::decodePart(const std::string& source) {
    return decode<MaiMessagePart>(source);
}
MaiResult<std::string> MaiSessionRecordCodec::encodeCheckpoint(const MaiContextCheckpoint& value) {
    return encode(value);
}
MaiResult<MaiContextCheckpoint> MaiSessionRecordCodec::decodeCheckpoint(const std::string& source) {
    return decode<MaiContextCheckpoint>(source);
}
MaiResult<std::string> MaiSessionRecordCodec::encodeToolOutput(const MaiToolOutputRecord& value) {
    return encode(value);
}
MaiResult<MaiToolOutputRecord> MaiSessionRecordCodec::decodeToolOutput(const std::string& source) {
    return decode<MaiToolOutputRecord>(source);
}
MaiResult<std::string> MaiSessionRecordCodec::encodeTurnState(const MaiMessage& message) {
    try {
        auto record = turnJson(message);
        auto checked = message;
        if (!readTurn(record, checked, false)) return invalidRecord();
        protectStrings(record);
        return record.dump();
    } catch (const Json::exception&) {
        return invalidRecord();
    }
}
MaiError MaiSessionRecordCodec::decodeTurnState(const std::string& source, MaiMessage& message) {
    if (source.empty()) {
        message.turnId.clear();
        message.turnOutcome.reset();
        return {};
    }
    try {
        auto record = Json::parse(source, nullptr, false);
        if (record.is_discarded() || !restoreStrings(record)) return invalidRecord();
        auto result = message;
        if (!readTurn(record, result, true)) return invalidRecord();
        message.turnId = std::move(result.turnId);
        message.turnOutcome = std::move(result.turnOutcome);
        return {};
    } catch (const Json::exception&) {
        return invalidRecord();
    }
}
