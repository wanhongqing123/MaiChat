#include "MaiRolloutRuntimeFacts.h"

#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

#include <json.hpp>
#include <openssl/evp.h>

#include "MaiIdGenerator.h"
#include "MaiCloudModelCatalog.h"
#include "MaiSessionRecordCodec.h"

namespace {
using Json = nlohmann::json;

Json parsed(const std::string& text) {
    const auto value = Json::parse(text, nullptr, false);
    return value.is_discarded() ? Json() : value;
}

std::string encoded(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string digest(const std::string& value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int count = 0;
    if (EVP_Digest(value.data(), value.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1)
        return {};
    std::ostringstream result;
    for (unsigned int index = 0; index < count; ++index)
        result << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(bytes[index]);
    return result.str();
}

bool validAnchor(const MaiModelInputAnchor& anchor) {
    const auto hash = [](const std::string& value) {
        return value.size() == 64 &&
               value.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    return anchor.version == 1 && anchor.estimatorVersion == 3 && !anchor.model.empty() &&
           anchor.model.size() <= 256 && !anchor.provider.empty() &&
           anchor.provider.size() <= 256 &&
           (anchor.wire == MaiWireApi::Responses || anchor.wire == MaiWireApi::Local) &&
           !anchor.localExact && !anchor.hasPathImages && anchor.messageCount <= 1000000 &&
           anchor.toolCount <= 65536 && hash(anchor.frameDigest) && hash(anchor.toolsDigest) &&
           hash(anchor.messagesDigest);
}

Json anchorValue(const MaiModelInputAnchor& anchor) {
    return Json{{"version", anchor.version},
                {"estimator_version", anchor.estimatorVersion},
                {"model", anchor.model},
                {"provider", anchor.provider},
                {"wire", maiWireApiToString(anchor.wire)},
                {"window_tokens", anchor.limits.contextWindowTokens},
                {"max_output_tokens", anchor.limits.maxOutputTokens},
                {"default_output_reserve_tokens", anchor.limits.defaultOutputReserveTokens},
                {"checkpoint_revision", anchor.checkpointRevision},
                {"message_count", anchor.messageCount},
                {"tool_count", anchor.toolCount},
                {"local_input_tokens", anchor.localInputTokens},
                {"measured_input_tokens", anchor.measuredInputTokens},
                {"measured_input_source", "provider"},
                {"local_exact", anchor.localExact},
                {"has_path_images", anchor.hasPathImages},
                {"frame_digest", anchor.frameDigest},
                {"tools_digest", anchor.toolsDigest},
                {"messages_digest", anchor.messagesDigest}};
}

std::optional<MaiModelInputAnchor> readAnchor(const Json& value) {
    if (!value.is_object() || value.size() != 19) return {};
    // 锚点是可选优化元数据，任何类型/版本/边界不符均关闭复用，不掩盖原史本身的错误。
    for (const char* key : {"version", "estimator_version", "window_tokens", "max_output_tokens",
                            "default_output_reserve_tokens", "checkpoint_revision", "message_count",
                            "tool_count", "local_input_tokens", "measured_input_tokens"})
        if (!value.contains(key) || !value[key].is_number_unsigned()) return {};
    for (const char* key :
         {"window_tokens", "max_output_tokens", "default_output_reserve_tokens", "message_count",
          "tool_count", "local_input_tokens", "measured_input_tokens"})
        if (value[key].get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) return {};
    for (const char* key : {"model", "provider", "wire", "measured_input_source", "frame_digest",
                            "tools_digest", "messages_digest"})
        if (!value.contains(key) || !value[key].is_string() ||
            value[key].get_ref<const std::string&>().size() > 256)
            return {};
    if (!value.contains("local_exact") || !value["local_exact"].is_boolean() ||
        !value.contains("has_path_images") || !value["has_path_images"].is_boolean() ||
        value["version"] != 1 || value["estimator_version"] != 3 ||
        value["measured_input_source"] != "provider" ||
        (value["wire"] != "responses" && value["wire"] != "local") || encoded(value).size() > 2048)
        return {};
    MaiModelInputAnchor anchor;
    anchor.model = value["model"].get<std::string>();
    anchor.provider = value["provider"].get<std::string>();
    anchor.wire = value["wire"] == "responses" ? MaiWireApi::Responses : MaiWireApi::Local;
    anchor.limits = {value["window_tokens"].get<std::size_t>(),
                     value["max_output_tokens"].get<std::size_t>(),
                     value["default_output_reserve_tokens"].get<std::size_t>()};
    anchor.checkpointRevision = value["checkpoint_revision"].get<std::uint64_t>();
    anchor.messageCount = value["message_count"].get<std::size_t>();
    anchor.toolCount = value["tool_count"].get<std::size_t>();
    anchor.localInputTokens = value["local_input_tokens"].get<std::size_t>();
    anchor.measuredInputTokens = value["measured_input_tokens"].get<std::size_t>();
    anchor.localExact = value["local_exact"].get<bool>();
    anchor.hasPathImages = value["has_path_images"].get<bool>();
    anchor.frameDigest = value["frame_digest"].get<std::string>();
    anchor.toolsDigest = value["tools_digest"].get<std::string>();
    anchor.messagesDigest = value["messages_digest"].get<std::string>();
    return validAnchor(anchor) ? std::optional<MaiModelInputAnchor>{std::move(anchor)}
                               : std::nullopt;
}

Json usageValue(const MaiModelUsage& usage) {
    const auto measured = maiTokenUsageSnapshot(usage);
    Json result{{"input_tokens", usage.inputTokens},
                {"output_tokens", usage.outputTokens},
                {"total_tokens", measured.total.tokens},
                {"cached_input_tokens", usage.cachedInputTokens},
                {"cache_write_input_tokens", usage.cacheWriteInputTokens},
                {"reasoning_output_tokens", usage.reasoningOutputTokens}};
    const std::array<std::pair<const char*, MaiUsageValueSource>, 6> sources{
        {{"input_tokens", usage.inputSource},
         {"output_tokens", usage.outputSource},
         {"total_tokens", measured.total.source},
         {"cached_input_tokens", usage.cachedInputSource},
         {"cache_write_input_tokens", usage.cacheWriteInputSource},
         {"reasoning_output_tokens", usage.reasoningOutputSource}}};
    for (const auto& [name, source] : sources) {
        result[std::string(name) + "_source"] = maiUsageValueSourceToString(source);
        if (source == MaiUsageValueSource::Missing) result[name] = nullptr;
    }
    const auto raw = parsed(usage.sourcePayload);
    if (!raw.is_null()) result["source_usage"] = raw;
    return result;
}

Json accumulated(const Json& previous, const Json& current) {
    if (!previous.is_object()) return current;
    Json result = current;
    result.erase("source_usage");
    for (const char* name : {"input_tokens", "output_tokens", "total_tokens", "cached_input_tokens",
                             "cache_write_input_tokens", "reasoning_output_tokens"}) {
        const std::string source = std::string(name) + "_source";
        if (!previous.value(name, Json()).is_number_unsigned() ||
            !current.value(name, Json()).is_number_unsigned()) {
            result[name] = nullptr;
            result[source] = "missing";
        } else {
            const auto before = previous[name].get<std::size_t>();
            const auto added = current[name].get<std::size_t>();
            if (added > std::numeric_limits<std::size_t>::max() - before) {
                result[name] = nullptr;
                result[source] = "missing";
                result[std::string(name) + "_error"] = "overflow";
            } else {
                result[name] = before + added;
                result[source] = "derived";
            }
        }
        const auto error = previous.find(std::string(name) + "_error");
        if (error != previous.end()) result[std::string(name) + "_error"] = *error;
    }
    return result;
}

Json toolsHeader(const MaiModelRequest& request) {
    Json tools = Json::array();
    const auto capability = maiCloudModelInfo(request.model);
    for (const auto& tool : request.tools) {
        if (capability && capability->provider == "openai" && tool.name == "apply_patch" &&
            !tool.inputGrammar.empty()) {
            tools.push_back(Json{{"type", "custom"},
                                 {"name", tool.name},
                                 {"description", tool.description},
                                 {"format", Json{{"type", "grammar"},
                                                 {"syntax", "lark"},
                                                 {"definition", tool.inputGrammar}}}});
            continue;
        }
        auto parameters = parsed(tool.parametersJson);
        if (!parameters.is_object()) parameters = Json::object();
        tools.push_back(Json{{"type", "function"},
                             {"name", tool.name},
                             {"description", tool.description},
                             {"parameters", parameters}});
    }
    return Json{{"type", "additional_tools"}, {"tools", tools}};
}

Json canonicalMessage(const MaiModelMessage& message, const std::set<std::string>& customCalls) {
    if (message.role == MaiModelRole::ToolResult)
        return Json{{"type", customCalls.count(message.toolCallId) ? "custom_tool_call_output"
                                                                   : "function_call_output"},
                    {"call_id", message.toolCallId},
                    {"output", message.content}};
    Json content = Json::array();
    if (!message.content.empty())
        content.push_back(
            Json{{"type", message.role == MaiModelRole::Assistant ? "output_text" : "input_text"},
                 {"text", message.content}});
    for (const auto& image : message.images)
        content.push_back(Json{
            {"type", "input_image"}, {"sourceRef", image.path}, {"mime_type", image.mimeType}});
    return Json{{"type", "message"},
                {"role", message.role == MaiModelRole::Assistant ? "assistant"
                         : message.role == MaiModelRole::System  ? "developer"
                                                                 : "user"},
                {"content", content}};
}

Json nativeOutput(const MaiModelMessage& message, const MaiModelRequest& request) {
    const auto capability = maiCloudModelInfo(request.model);
    if (!message.continuation || !capability ||
        message.continuation->format != "responses.output.v1" ||
        message.continuation->provider != capability->provider ||
        message.continuation->model != request.model)
        return Json();
    const auto output = parsed(message.continuation->payload);
    return output.is_array() ? output : Json();
}

// 原生续接优先；旧记录失去续接时仍与 wire 一样恢复 canonical apply_patch。
// 不推断未知调用或改写原始历史，只为当前请求的规范投影选择对应 item 类型。
std::set<std::string> customCallIds(const MaiModelRequest& request) {
    std::set<std::string> result;
    const auto capability = maiCloudModelInfo(request.model);
    if (!capability || capability->provider != "openai") return result;
    for (const auto& message : request.messages) {
        const auto output = nativeOutput(message, request);
        if (output.is_array()) {
            for (const auto& item : output)
                if (item.is_object() && item.value("type", "") == "custom_tool_call" &&
                    item.value("call_id", Json()).is_string())
                    result.insert(item["call_id"].get<std::string>());
        } else {
            for (const auto& call : message.invocations)
                if (call.name == "apply_patch") result.insert(call.id);
        }
    }
    return result;
}

Json replacementHistory(const MaiModelRequest& request) {
    const auto customCalls = customCallIds(request);
    Json result = Json::array(
        {toolsHeader(request),
         Json{{"type", "message"},
              {"role", "developer"},
              {"content",
               Json::array({Json{{"type", "input_text"}, {"text", request.baseInstructions}}})}}});
    for (const auto& message : request.messages) {
        const auto output = nativeOutput(message, request);
        if (output.is_array()) {
            for (const auto& item : output) result.push_back(item);
        } else {
            result.push_back(canonicalMessage(message, customCalls));
            for (const auto& call : message.invocations) {
                const auto arguments = parsed(call.arguments);
                if (customCalls.count(call.id) && call.name == "apply_patch" &&
                    arguments.is_object() && arguments.size() == 1 &&
                    arguments.value("patch", Json()).is_string())
                    result.push_back(Json{{"type", "custom_tool_call"},
                                          {"call_id", call.id},
                                          {"name", call.name},
                                          {"input", arguments["patch"]}});
                else
                    result.push_back(Json{{"type", "function_call"},
                                          {"call_id", call.id},
                                          {"name", call.name},
                                          {"arguments", call.arguments}});
            }
        }
    }
    return result;
}
}  // namespace

MaiRolloutRuntimeFacts::MaiRolloutRuntimeFacts(MaiSessionStore* store,
                                               const MaiTurnContext& context, std::string provider,
                                               std::size_t windowTokens)
    : mStore(store),
      mContext(context),
      mProvider(std::move(provider)),
      mWindowTokens(windowTokens) {}

MaiModelContextUsage MaiRolloutRuntimeFacts::contextUsage() const {
    return mContextUsage;
}

std::optional<MaiModelInputAnchor> MaiRolloutRuntimeFacts::inputEstimateAnchor() const {
    return mInputEstimateAnchor;
}

MaiError MaiRolloutRuntimeFacts::append(const std::string& type, const std::string& payload) {
    const auto error = mStore->appendSessionRecord(mContext.sessionId, type, payload);
    return error.code() == MaiErrorCode::NotSupported ? MaiError{} : error;
}

MaiResult<std::string> MaiRolloutRuntimeFacts::latest(const std::string& type) const {
    auto result = mStore->readLatestSessionRecord(mContext.sessionId, type);
    if (!result && result.error().code() == MaiErrorCode::NotSupported) return std::string{};
    return result;
}

MaiError MaiRolloutRuntimeFacts::initialize() {
    if (!mInitialized) {
        auto previous = latest("turn_context");
        if (!previous) return previous.error();
        mPreviousContext = previous.value();
        const auto previousContext = parsed(mPreviousContext);
        auto usage = latest("token_usage_record");
        if (!usage) return usage.error();
        mLatestUsage = usage.value();
        const auto savedUsage = parsed(mLatestUsage);
        if (savedUsage.is_object()) {
            mThreadUsage = encoded(savedUsage.value("thread_token_usage", Json()));
            mInputEstimateAnchor = readAnchor(savedUsage.value("request_input_anchor", Json()));
        }
        const auto restored =
            maiReadModelContextUsage(*mStore, mContext.sessionId, mContext.model, mProvider);
        if (restored) {
            mContextUsage = restored.value();
            // 新轮次累计尚未开始；历史采样保留它自己的 turnId，不误贴当前轮次标签。
            mContextUsage.turn = {};
            if (mContextUsage.windowTokens != mWindowTokens) mContextUsage.samplingValid = false;
            // 哈希元数据不能自证真实用量；必须与同条记录恢复出的供应商 sampling input
            // 及其来源/窗口身份一致。摘要累计、total 或独立修改的计数均不能成为锚点。
            if (mInputEstimateAnchor &&
                (!mContextUsage.samplingValid ||
                 mContextUsage.latestSampling.input.source != MaiUsageValueSource::Provider ||
                 mInputEstimateAnchor->measuredInputTokens !=
                     mContextUsage.latestSampling.input.tokens ||
                 mInputEstimateAnchor->model != mContextUsage.model ||
                 mInputEstimateAnchor->provider != mContextUsage.provider ||
                 mInputEstimateAnchor->checkpointRevision != mContextUsage.checkpointRevision ||
                 mInputEstimateAnchor->limits.contextWindowTokens != mContextUsage.windowTokens))
                mInputEstimateAnchor.reset();
        } else if (restored.error().code() != MaiErrorCode::NotSupported)
            return restored.error();
        auto window = latest("compacted");
        if (!window) return window.error();
        const auto savedWindow = parsed(window.value());
        if (savedWindow.is_object()) {
            mWindowId = savedWindow.value("window_id", "");
            mWindowNumber = savedWindow.value("window_number", std::uint64_t{0});
            mCheckpointRevision = savedWindow.value("checkpoint_revision", std::uint64_t{0});
        }
        if (previousContext.is_object()) {
            mBaseDigest = previousContext.value("base_header_digest", "");
            mToolsDigest = previousContext.value("tools_header_digest", "");
        }
        if (const auto error = mStore->lastReadError()) return error;
        mInitialized = true;
    }
    return {};
}

MaiError MaiRolloutRuntimeFacts::prepare(const MaiModelRequest& request, bool usedCheckpoint) {
    if (auto error = initialize()) return error;
    const auto previousContext = parsed(mPreviousContext);
    const auto tools = toolsHeader(request);
    const auto baseDigest = digest(request.baseInstructions);
    const auto toolsDigest = digest(encoded(tools));
    if (baseDigest.empty() || toolsDigest.empty())
        return {MaiErrorCode::Internal, "cannot fingerprint canonical model headers"};
    if (mToolsDigest != toolsDigest) {
        if (auto error = append("response_item", encoded(tools))) return error;
        mToolsDigest = toolsDigest;
    }
    if (mBaseDigest != baseDigest) {
        if (auto error =
                append("response_item",
                       encoded(Json{
                           {"type", "message"},
                           {"role", "developer"},
                           {"content", Json::array({Json{{"type", "input_text"},
                                                         {"text", request.baseInstructions}}})}})))
            return error;
        mBaseDigest = baseDigest;
    }
    const auto customCalls = customCallIds(request);
    for (const auto& message : request.messages) {
        if (message.role != MaiModelRole::User && message.role != MaiModelRole::ToolResult)
            continue;
        // 摘要/宿主临时提示没有源身份，不冒充新用户输入，也不反复记录完整输入前缀。
        if (message.sourceMessageId.empty()) continue;
        const auto source =
            encoded(Json::array({message.sourceMessageId, message.sourcePartId,
                                 message.role == MaiModelRole::ToolResult ? "tool" : "user"}));
        if (mInputSources.count(source)) continue;
        auto item = canonicalMessage(message, customCalls);
        item["source_message_id"] = message.sourceMessageId;
        item["source_part_id"] = message.sourcePartId;
        if (auto error = append("response_item", encoded(item))) return error;
        constexpr std::size_t kMaxCachedInputSources = 4096;
        if (mInputSources.size() == kMaxCachedInputSources)
            mInputSources.erase(mInputSources.begin());
        mInputSources.insert(source);
    }
    if (!mPrepared) {
        const Json world{{"cwd", request.workingDirectory},
                         {"model", request.model},
                         {"reasoning_effort", request.reasoningEffort}};
        Json worldRecord;
        if (!previousContext.is_object())
            worldRecord = Json{{"kind", "full"}, {"state", world}};
        else {
            const Json oldWorld{
                {"cwd", previousContext.value("cwd", "")},
                {"model", previousContext.value("model", "")},
                {"reasoning_effort", previousContext.value("reasoning_effort", "")}};
            if (world != oldWorld)
                worldRecord = Json{{"kind", "diff"}, {"changes", Json::diff(oldWorld, world)}};
        }
        if (!worldRecord.is_null())
            if (auto error = append("world_state", encoded(worldRecord))) return error;
        Json context{
            {"turn_id", mContext.turnId},
            {"root_turn_id", mContext.turnId},
            {"session_id", mContext.sessionId},
            {"model", request.model},
            {"cwd", request.workingDirectory},
            {"reasoning_effort", request.reasoningEffort},
            {"effort", request.reasoningEffort.empty() ? Json() : Json(request.reasoningEffort)},
            {"disabled_plugin_ids", nullptr},
            {"approval_policy", nullptr},
            {"approvals_reviewer", nullptr},
            {"sandbox_policy", Json{{"type", nullptr}}},
            {"permission_profile", nullptr},
            {"active_permission_profile", nullptr},
            {"comp_hash", nullptr},
            {"collaboration_mode", nullptr},
            {"multi_agent_version", nullptr},
            {"realtime_active", nullptr},
            {"summary", nullptr},
            {"canonical_representation", "responses-items-v1"},
            {"base_header_digest", mBaseDigest},
            {"tools_header_digest", mToolsDigest}};
        if (auto error = append("turn_context", encoded(context))) return error;
        mPrepared = true;
    }
    if (!usedCheckpoint && mCheckpointRevision != 0) {
        MaiContextCheckpoint checkpoint;
        const bool exists = mStore->getContextCheckpoint(mContext.sessionId, checkpoint);
        if (const auto error = mStore->lastReadError()) return error;
        if (!exists) {
            const auto nextWindow = MaiIdGenerator::newPartId();
            Json record{{"message", ""},
                        {"replacement_history", replacementHistory(request)},
                        {"window_id", nextWindow},
                        {"window_number", mWindowNumber + 1},
                        {"previous_window_id", mWindowId},
                        {"checkpoint_revision", 0},
                        {"reason", "checkpoint_reset"},
                        {"latest_usage", parsed(mLatestUsage)}};
            if (auto error = append("compacted", encoded(record))) return error;
            mWindowId = nextWindow;
            ++mWindowNumber;
            mCheckpointRevision = 0;
            mContextUsage.samplingValid = false;
        }
    }
    if (usedCheckpoint) {
        MaiContextCheckpoint checkpoint;
        const bool exists = mStore->getContextCheckpoint(mContext.sessionId, checkpoint);
        if (const auto error = mStore->lastReadError()) return error;
        if (exists && checkpoint.revision != mCheckpointRevision) {
            const auto previousWindow = mWindowId;
            const auto nextWindow = MaiIdGenerator::newPartId();
            Json retained{{"source_message_count", checkpoint.sourceMessageCount},
                          {"through_message_id", checkpoint.throughMessageId},
                          {"partial_message_id", checkpoint.partialMessageId},
                          {"through_part_id", checkpoint.throughPartId},
                          {"preserved_user_message_id", checkpoint.preservedUserMessageId},
                          {"references", parsed(checkpoint.references)}};
            Json record{{"message", ""},
                        {"replacement_history", replacementHistory(request)},
                        {"window_id", nextWindow},
                        {"window_number", mWindowNumber + 1},
                        {"previous_window_id", previousWindow},
                        {"checkpoint_revision", checkpoint.revision},
                        {"retained_context", retained},
                        {"latest_usage", parsed(mLatestUsage)},
                        {"resume_metadata", Json{{"turn_id", mContext.turnId},
                                                 {"session_id", mContext.sessionId},
                                                 {"model", request.model},
                                                 {"cwd", request.workingDirectory},
                                                 {"reasoning_effort", request.reasoningEffort}}}};
            if (auto error = append("compacted", encoded(record))) return error;
            mWindowId = nextWindow;
            ++mWindowNumber;
            mCheckpointRevision = checkpoint.revision;
            // 检查点生效后，旧测量只属于旧窗口；下次真实主采样才能恢复占用值。
            mContextUsage.samplingValid = false;
        }
    }
    return {};
}

MaiError MaiRolloutRuntimeFacts::outputItem(const MaiModelOutputItem& item) {
    // 记录原对象，不把应用元数据混入供应商 item，也不据此派发任何工具。
    return append("response_item", item.payload);
}

MaiError MaiRolloutRuntimeFacts::started() {
    if (mStarted) return {};
    const auto now = MaiTime::getCurrentTime();
    const Json record{{"type", "task_started"},
                      {"turn_id", mContext.turnId},
                      {"root_turn_id", mContext.turnId},
                      {"started_at", now / 1000},
                      {"model_context_window", nullptr}};
    if (auto error = append("event_msg", encoded(record))) return error;
    mStartedAt = now;
    mStarted = true;
    return {};
}

MaiError MaiRolloutRuntimeFacts::finished(const MaiTurnOutcome& outcome) {
    if (!mStarted || mFinished) return {};
    const bool interrupted = outcome.status == MaiTurnStatus::Interrupted;
    Json record{{"type", interrupted ? "turn_aborted" : "task_complete"},
                {"turn_id", mContext.turnId},
                {"root_turn_id", mContext.turnId},
                {"status", maiTurnStatusToString(outcome.status)},
                {"result_message_id", outcome.resultMessageId},
                {"started_at", mStartedAt / 1000},
                {"completed_at", outcome.completed / 1000},
                {"duration_ms",
                 outcome.completed >= mStartedAt ? Json(outcome.completed - mStartedAt) : Json()}};
    if (interrupted)
        record["reason"] = "interrupted";
    else
        record["last_agent_message"] = nullptr;
    if (outcome.error)
        record["error"] = Json{{"message", outcome.error.message()}, {"codex_error_info", nullptr}};
    if (auto error = append("event_msg", encoded(record))) return error;
    mFinished = true;
    return {};
}

MaiError MaiRolloutRuntimeFacts::usage(const MaiModelUsage& usage, const std::string& operation,
                                       const MaiModelInputAnchor* inputAnchor) {
    if (auto error = initialize()) return error;
    const auto current = usageValue(usage);
    const auto turn = accumulated(parsed(mTurnUsage), current);
    auto thread = accumulated(parsed(mThreadUsage), current);
    thread["history_before_first_measurement_known"] = false;
    auto snapshot = mContextUsage;
    const auto currentSnapshot = maiTokenUsageSnapshot(usage);
    snapshot.turn = mTurnUsage.empty() ? currentSnapshot
                                       : maiAccumulateTokenUsage(snapshot.turn, currentSnapshot);
    snapshot.thread = mThreadUsage.empty()
                          ? currentSnapshot
                          : maiAccumulateTokenUsage(snapshot.thread, currentSnapshot);
    snapshot.latestOperation = operation;
    if (operation == "sampling") {
        snapshot.latestSampling = currentSnapshot;
        snapshot.model = mContext.model;
        snapshot.provider = usage.provider.empty() ? mProvider : usage.provider;
        snapshot.turnId = mContext.turnId;
        snapshot.responseId = usage.responseId;
        snapshot.windowId = mWindowId;
        snapshot.windowNumber = mWindowNumber;
        snapshot.checkpointRevision = mCheckpointRevision;
        snapshot.windowTokens = mWindowTokens;
        snapshot.windowSource = mWindowTokens == 0 ? MaiContextWindowSource::Missing
                                                   : MaiContextWindowSource::Configuration;
        snapshot.samplingValid = !snapshot.provider.empty() && snapshot.provider == mProvider;
    }
    const auto snapshotRecord = MaiSessionRecordCodec::encodeContextUsage(snapshot);
    if (!snapshotRecord) return snapshotRecord.error();
    // 摘要是独立模型请求，其用量进入轮次/线程累计；不能冒充主上下文的窗口占用。
    const auto last = parsed(mLatestUsage);
    const Json windowUsage = operation == "sampling" ? current
                             : last.is_object()      ? last.value("context_window_usage", Json())
                                                     : Json();
    auto nextAnchor = mInputEstimateAnchor;
    if (operation == "sampling") {
        nextAnchor.reset();
        if (inputAnchor && usage.inputSource == MaiUsageValueSource::Provider &&
            (usage.provider.empty() || usage.provider == inputAnchor->provider) &&
            validAnchor(*inputAnchor)) {
            auto measured = *inputAnchor;
            measured.measuredInputTokens = usage.inputTokens;
            if (encoded(anchorValue(measured)).size() <= 2048) nextAnchor = std::move(measured);
        }
    }
    Json record{{"turn_id", mContext.turnId},
                {"model", mContext.model},
                {"response_id", usage.responseId},
                {"usage", current},
                {"turn_token_usage", turn},
                {"thread_token_usage", thread},
                {"context_window_usage", windowUsage},
                {"context_usage", parsed(snapshotRecord.value())},
                {"window_id", mWindowId},
                {"window_number", mWindowNumber},
                {"operation", operation}};
    record["request_input_anchor"] = nextAnchor ? anchorValue(*nextAnchor) : Json();
    if (auto error = append("token_usage_record", encoded(record))) return error;
    mTurnUsage = encoded(turn);
    mThreadUsage = encoded(thread);
    mLatestUsage = encoded(record);
    mContextUsage = std::move(snapshot);
    mInputEstimateAnchor = std::move(nextAnchor);
    return {};
}
