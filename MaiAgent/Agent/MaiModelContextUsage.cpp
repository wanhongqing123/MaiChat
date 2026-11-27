// 文件作用：把实测模型计量投影为状态及重开会话共用的值，不使用本地估计补齐。
#include "MaiModelContextUsage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <json.hpp>

#include "MaiSessionRecordCodec.h"
#include "MaiSessionStore.h"

namespace {
using Json = nlohmann::json;

constexpr std::array<const char*, 6> kCountNames{
    "input_tokens",  "cached_input_tokens",     "cache_write_input_tokens",
    "output_tokens", "reasoning_output_tokens", "total_tokens"};

const char* sourceName(MaiUsageValueSource source) {
    switch (source) {
        case MaiUsageValueSource::Missing: return "missing";
        case MaiUsageValueSource::Provider: return "provider";
        case MaiUsageValueSource::Derived: return "derived";
    }
    return nullptr;
}

Json snapshotJson(const MaiTokenUsageSnapshot& usage) {
    const std::array<MaiTokenUsageCount, 6> counts{usage.input,           usage.cachedInput,
                                                   usage.cacheWriteInput, usage.output,
                                                   usage.reasoningOutput, usage.total};
    Json result = Json::object();
    for (std::size_t index = 0; index < counts.size(); ++index) {
        const auto* name = kCountNames[index];
        const auto& count = counts[index];
        const char* source = sourceName(count.source);
        if (!source) return Json();
        result[name] = count.source == MaiUsageValueSource::Missing ? Json() : Json(count.tokens);
        result[std::string(name) + "_source"] = source;
    }
    return result;
}

bool readSize(const Json& value, std::size_t& result) {
    if (!value.is_number_unsigned() &&
        (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        return false;
    const auto count = value.get<std::uint64_t>();
    if (count > std::numeric_limits<std::size_t>::max()) return false;
    result = static_cast<std::size_t>(count);
    return true;
}

bool readSnapshot(const Json& value, MaiTokenUsageSnapshot& result) {
    if (!value.is_object()) return false;
    const std::array<MaiTokenUsageCount*, 6> counts{&result.input,           &result.cachedInput,
                                                    &result.cacheWriteInput, &result.output,
                                                    &result.reasoningOutput, &result.total};
    for (std::size_t index = 0; index < counts.size(); ++index) {
        const auto* name = kCountNames[index];
        const auto source = value.find(std::string(name) + "_source");
        const auto count = value.find(name);
        if (source == value.end() || !source->is_string() || count == value.end()) return false;
        auto& output = *counts[index];
        if (*source == "missing") {
            if (!count->is_null()) return false;
            output = {};
        } else {
            if (*source == "provider")
                output.source = MaiUsageValueSource::Provider;
            else if (*source == "derived")
                output.source = MaiUsageValueSource::Derived;
            else
                return false;
            if (!readSize(*count, output.tokens)) return false;
        }
    }
    return true;
}

MaiError invalidUsage() {
    return {MaiErrorCode::InvalidInput, "Invalid context usage record"};
}

MaiTokenUsageCount add(const MaiTokenUsageCount& left, const MaiTokenUsageCount& right) {
    if (left.source == MaiUsageValueSource::Missing ||
        right.source == MaiUsageValueSource::Missing ||
        right.tokens > std::numeric_limits<std::size_t>::max() - left.tokens)
        return {};
    return {left.tokens + right.tokens, MaiUsageValueSource::Derived};
}
}  // namespace

MaiTokenUsageSnapshot maiTokenUsageSnapshot(const MaiModelUsage& usage) {
    MaiTokenUsageSnapshot result;
    result.input = {usage.inputTokens, usage.inputSource};
    result.cachedInput = {usage.cachedInputTokens, usage.cachedInputSource};
    result.cacheWriteInput = {usage.cacheWriteInputTokens, usage.cacheWriteInputSource};
    result.output = {usage.outputTokens, usage.outputSource};
    result.reasoningOutput = {usage.reasoningOutputTokens, usage.reasoningOutputSource};
    result.total = {usage.totalTokens, usage.totalSource};
    if (result.total.source == MaiUsageValueSource::Missing)
        result.total = add(result.input, result.output);
    return result;
}

MaiTokenUsageSnapshot maiAccumulateTokenUsage(const MaiTokenUsageSnapshot& previous,
                                              const MaiTokenUsageSnapshot& current) {
    return {add(previous.input, current.input),
            add(previous.cachedInput, current.cachedInput),
            add(previous.cacheWriteInput, current.cacheWriteInput),
            add(previous.output, current.output),
            add(previous.reasoningOutput, current.reasoningOutput),
            add(previous.total, current.total)};
}

MaiTokenUsageCount maiNonCachedInput(const MaiTokenUsageSnapshot& usage) {
    if (usage.input.source == MaiUsageValueSource::Missing) return {};
    const auto cached =
        usage.cachedInput.source == MaiUsageValueSource::Missing ? 0 : usage.cachedInput.tokens;
    return {usage.input.tokens - std::min(usage.input.tokens, cached),
            MaiUsageValueSource::Derived};
}

MaiTokenUsageCount maiBlendedTotal(const MaiTokenUsageSnapshot& usage) {
    return add(maiNonCachedInput(usage), usage.output);
}

std::optional<int> maiContextRemainingPercent(const MaiModelContextUsage& usage,
                                              std::size_t baselineTokens) {
    if (!usage.samplingValid || usage.windowTokens == 0 ||
        usage.windowSource == MaiContextWindowSource::Missing ||
        usage.latestSampling.total.source == MaiUsageValueSource::Missing)
        return std::nullopt;
    if (usage.windowTokens <= baselineTokens) return 0;
    const auto effective = usage.windowTokens - baselineTokens;
    const auto total = usage.latestSampling.total.tokens;
    const auto used = total - std::min(total, baselineTokens);
    const auto remaining = effective - std::min(effective, used);
    return static_cast<int>(std::round(static_cast<double>(remaining) / effective * 100.0));
}

MaiResult<std::string> MaiSessionRecordCodec::encodeContextUsage(
    const MaiModelContextUsage& usage) {
    const auto last = snapshotJson(usage.latestSampling);
    const auto turn = snapshotJson(usage.turn);
    const auto thread = snapshotJson(usage.thread);
    const char* windowSource = usage.windowSource == MaiContextWindowSource::Missing    ? "missing"
                               : usage.windowSource == MaiContextWindowSource::Provider ? "provider"
                               : usage.windowSource == MaiContextWindowSource::Configuration
                                   ? "configuration"
                                   : nullptr;
    if (last.is_null() || turn.is_null() || thread.is_null() || !windowSource)
        return invalidUsage();
    try {
        return Json{{"version", 1},
                    {"latest_sampling", last},
                    {"turn_token_usage", turn},
                    {"thread_token_usage", thread},
                    {"latest_operation", usage.latestOperation},
                    {"model", usage.model},
                    {"provider", usage.provider},
                    {"turn_id", usage.turnId},
                    {"response_id", usage.responseId},
                    {"window_id", usage.windowId},
                    {"window_number", usage.windowNumber},
                    {"checkpoint_revision", usage.checkpointRevision},
                    {"window_tokens", usage.windowTokens},
                    {"window_source", windowSource},
                    {"sampling_valid", usage.samplingValid}}
            .dump();
    } catch (const Json::exception&) {
        return invalidUsage();
    }
}

MaiResult<MaiModelContextUsage> MaiSessionRecordCodec::decodeContextUsage(
    const std::string& source) {
    const auto value = Json::parse(source, nullptr, false);
    MaiModelContextUsage usage;
    try {
        if (!value.is_object() || value.at("version") != 1 ||
            !readSnapshot(value.at("latest_sampling"), usage.latestSampling) ||
            !readSnapshot(value.at("turn_token_usage"), usage.turn) ||
            !readSnapshot(value.at("thread_token_usage"), usage.thread))
            return invalidUsage();
        usage.latestOperation = value.at("latest_operation").get<std::string>();
        usage.model = value.at("model").get<std::string>();
        usage.provider = value.at("provider").get<std::string>();
        usage.turnId = value.at("turn_id").get<std::string>();
        usage.responseId = value.at("response_id").get<std::string>();
        usage.windowId = value.at("window_id").get<std::string>();
        std::size_t number = 0, revision = 0;
        if (!readSize(value.at("window_number"), number) ||
            !readSize(value.at("checkpoint_revision"), revision) ||
            !readSize(value.at("window_tokens"), usage.windowTokens))
            return invalidUsage();
        usage.windowNumber = number;
        usage.checkpointRevision = revision;
        const auto windowSource = value.at("window_source").get<std::string>();
        if (windowSource == "missing")
            usage.windowSource = MaiContextWindowSource::Missing;
        else if (windowSource == "provider")
            usage.windowSource = MaiContextWindowSource::Provider;
        else if (windowSource == "configuration")
            usage.windowSource = MaiContextWindowSource::Configuration;
        else
            return invalidUsage();
        usage.samplingValid = value.at("sampling_valid").get<bool>();
        return usage;
    } catch (const Json::exception&) {
        return invalidUsage();
    }
}

MaiResult<MaiModelContextUsage> maiReadModelContextUsage(const MaiSessionStore& store,
                                                         const std::string& sessionId,
                                                         const std::string& expectedModel,
                                                         const std::string& expectedProvider) {
    const auto record = store.readLatestSessionRecord(sessionId, "token_usage_record");
    if (!record) return record.error();
    if (record.value().empty()) return MaiModelContextUsage{};
    const auto value = Json::parse(record.value(), nullptr, false);
    if (!value.is_object()) return invalidUsage();
    MaiModelContextUsage usage;
    const auto snapshot = value.find("context_usage");
    if (snapshot != value.end()) {
        const auto decoded = MaiSessionRecordCodec::decodeContextUsage(snapshot->dump());
        if (!decoded) return decoded.error();
        usage = decoded.value();
    } else {
        // 旧事实没有可验证的采样来源，仍可显示已有累计，绝不拿旧窗口猜占用。
        if (value.contains("turn_token_usage") &&
            !readSnapshot(value["turn_token_usage"], usage.turn))
            return invalidUsage();
        if (value.contains("thread_token_usage") &&
            !readSnapshot(value["thread_token_usage"], usage.thread))
            return invalidUsage();
    }
    if (expectedModel.empty() || expectedProvider.empty() || usage.model != expectedModel ||
        usage.provider != expectedProvider)
        usage.samplingValid = false;
    if (!usage.turnId.empty()) {
        const auto origin = store.readTurnMetadata(sessionId, usage.turnId);
        if (!origin && origin.error().code() == MaiErrorCode::NotFound)
            usage.samplingValid = false;
        else if (!origin && origin.error().code() != MaiErrorCode::NotSupported)
            return origin.error();
    }
    const auto compacted = store.readLatestSessionRecord(sessionId, "compacted");
    if (!compacted) return compacted.error();
    if (!compacted.value().empty()) {
        const auto window = Json::parse(compacted.value(), nullptr, false);
        try {
            if (!window.is_object() || !window.at("window_id").is_string()) return invalidUsage();
            if (window.at("window_id") != usage.windowId ||
                window.at("checkpoint_revision") != usage.checkpointRevision)
                usage.samplingValid = false;
        } catch (const Json::exception&) {
            return invalidUsage();
        }
    }
    MaiContextCheckpoint checkpoint;
    const bool hasCheckpoint = store.getContextCheckpoint(sessionId, checkpoint);
    if (const auto error = store.lastReadError()) return error;
    if ((hasCheckpoint && checkpoint.revision != usage.checkpointRevision) ||
        (!hasCheckpoint && usage.checkpointRevision != 0))
        usage.samplingValid = false;
    return usage;
}
