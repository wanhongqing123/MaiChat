// 文件作用：验证内部持久化编码器的字段完整性、原字节保真和损坏记录边界。
#include "MaiSessionRecordCodec.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include <json.hpp>

namespace {

using Json = nlohmann::json;
int failures = 0;
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)
#define SAME(field) CHECK(original.field == restored.field)

std::string rawBytes() {
    std::string bytes;
    for (int value = 0; value < 256; ++value) bytes.push_back(static_cast<char>(value));
    return bytes;
}

MaiMessage sampleMessage() {
    MaiMessage value;
    value.id = "message";
    value.role = MaiRole::Assistant;
    value.created = -31;
    value.completed = 81;
    value.inputRequestId = "request";
    value.pendingInput = true;
    value.contextImportOrigin = MaiContextImportOrigin{"source-key", "source-id"};
    value.turnId = value.id;
    MaiTurnOutcome outcome;
    outcome.turnId = value.id;
    outcome.status = MaiTurnStatus::Failed;
    outcome.resultMessageId = "result";
    outcome.completed = 82;
    outcome.error =
        MaiError(MaiErrorCode::RequestBodyTooLarge, rawBytes()).withRetryAdvice({3, 922, -1, true});
    MaiContextStatus context;
    context.phase = MaiContextPhase::Interrupted;
    context.before = {123, true};
    context.after = {54, false};
    context.windowTokens = 8800;
    context.inputBudgetTokens = 7200;
    context.checkpointRevision = 18;
    context.originalUserMessages = 8;
    context.retainedUserMessages = 3;
    context.reusedCheckpoint = true;
    context.advancedCheckpoint = true;
    context.historyIndexRecovery = true;
    context.error = MaiError(MaiErrorCode::ContextWindowExceeded, rawBytes())
                        .withRetryAdvice({2, 333, 888, false});
    // 这些事实即使没有 reduced/limit 也不能丢失。
    context.requestBodyReduced = false;
    context.requestBodyBytes = 102;
    context.requestBodyLimitBytes = 0;
    context.requestBodyOmittedImages = 2;
    context.requestBodyTruncatedTexts = 3;
    context.outputTokens = 907;
    context.usage.latestSampling.total = {30000, MaiUsageValueSource::Provider};
    context.usage.turn.total = {70000, MaiUsageValueSource::Derived};
    context.usage.thread.total = {90000, MaiUsageValueSource::Derived};
    context.usage.model = "fixture-model";
    context.usage.provider = "fixture-origin-sha256";
    context.usage.turnId = "fixture-sampling-turn";
    context.usage.responseId = "fixture-response";
    context.usage.windowId = "fixture-window";
    context.usage.windowNumber = 3;
    context.usage.checkpointRevision = 18;
    context.usage.windowTokens = 200000;
    context.usage.windowSource = MaiContextWindowSource::Configuration;
    context.usage.samplingValid = true;
    outcome.context = context;
    value.turnOutcome = outcome;

    MaiToolExecution execution;
    execution.status = MaiToolExecutionStatus::Failed;
    execution.effectStatus = MaiToolEffectStatus::Confirmed;
    execution.nextAction = MaiToolNextAction::QueryStatus;
    execution.started = false;
    execution.exitCode = -42;
    execution.timedOut = true;
    execution.canceled = true;
    execution.outputTruncated = true;
    execution.outputId = rawBytes();
    execution.taskId = rawBytes();
    execution.recoveryTool = rawBytes();
    MaiToolPart tool;
    tool.tool = rawBytes();
    tool.callId = "call";
    tool.input = rawBytes();
    tool.output = rawBytes();
    tool.error = rawBytes();
    tool.state = MaiToolState::Error;
    tool.execution = execution;
    tool.modelOutputVersion = 1;
    value.parts = {{"text", MaiTextPart{u8"ordinary \u4f60\u597d \U0001f642"}, 11, true},
                   {"reason",
                    MaiReasoningPart{rawBytes(), MaiModelContinuationState{rawBytes(), rawBytes(),
                                                                           rawBytes(), rawBytes()}},
                    12, true},
                   {"image", MaiImagePart{rawBytes(), rawBytes()}, 13, false},
                   {"tool", tool, 14, false},
                   {"video", MaiVideoPart{rawBytes(), rawBytes()}, 15, false},
                   {"quote", MaiQuotePart{"quoted-message", rawBytes()}, 16, false}};
    return value;
}

void checkError(const MaiError& original, const MaiError& restored) {
    CHECK(original.code() == restored.code());
    CHECK(original.message() == restored.message());
    CHECK(original.retryAdvice().has_value() == restored.retryAdvice().has_value());
    if (original.retryAdvice() && restored.retryAdvice()) {
        CHECK(original.retryAdvice()->maxRetries == restored.retryAdvice()->maxRetries);
        CHECK(original.retryAdvice()->initialDelayMs == restored.retryAdvice()->initialDelayMs);
        CHECK(original.retryAdvice()->retryAfterMs == restored.retryAdvice()->retryAfterMs);
        CHECK(original.retryAdvice()->sawOutput == restored.retryAdvice()->sawOutput);
    }
}

void checkTurn(const MaiMessage& originalMessage, const MaiMessage& restoredMessage) {
    CHECK(originalMessage.turnId == restoredMessage.turnId);
    CHECK(originalMessage.turnOutcome.has_value() == restoredMessage.turnOutcome.has_value());
    if (!originalMessage.turnOutcome || !restoredMessage.turnOutcome) return;
    const auto& first = *originalMessage.turnOutcome;
    const auto& second = *restoredMessage.turnOutcome;
    CHECK(first.turnId == second.turnId);
    CHECK(first.status == second.status);
    CHECK(first.resultMessageId == second.resultMessageId);
    CHECK(first.completed == second.completed);
    checkError(first.error, second.error);
    CHECK(first.context.has_value() == second.context.has_value());
    if (first.context && second.context) {
        const auto& original = *first.context;
        const auto& restored = *second.context;
        SAME(phase);
        SAME(before.tokens);
        SAME(before.exact);
        SAME(after.tokens);
        SAME(after.exact);
        SAME(windowTokens);
        SAME(inputBudgetTokens);
        SAME(checkpointRevision);
        SAME(originalUserMessages);
        SAME(retainedUserMessages);
        SAME(reusedCheckpoint);
        SAME(advancedCheckpoint);
        SAME(historyIndexRecovery);
        SAME(requestBodyReduced);
        SAME(requestBodyBytes);
        SAME(requestBodyLimitBytes);
        SAME(requestBodyOmittedImages);
        SAME(requestBodyTruncatedTexts);
        SAME(outputTokens);
        SAME(usage.latestSampling.total.tokens);
        SAME(usage.latestSampling.total.source);
        SAME(usage.turn.total.tokens);
        SAME(usage.thread.total.tokens);
        SAME(usage.model);
        SAME(usage.provider);
        SAME(usage.turnId);
        SAME(usage.responseId);
        SAME(usage.windowId);
        SAME(usage.windowNumber);
        SAME(usage.checkpointRevision);
        SAME(usage.windowTokens);
        SAME(usage.windowSource);
        SAME(usage.samplingValid);
        checkError(original.error, restored.error);
    }
}

void testSession() {
    MaiSession original;
    original.id = "session";
    original.title = rawBytes();
    original.directory = rawBytes();
    original.model = rawBytes();
    original.agent = rawBytes();
    original.parentId = "parent";
    original.depth = 2;
    original.created = -18;
    original.updated = 700;
    original.titleEdited = true;
    original.reasoningEffort = rawBytes();
    const auto encoded = MaiSessionRecordCodec::encodeSession(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    const auto object = Json::parse(encoded.value(), nullptr, false);
    CHECK(object.is_object());
    CHECK(object["codec_version"] == 1);
    CHECK(object["title"].is_object());
    const auto decoded = MaiSessionRecordCodec::decodeSession(encoded.value());
    CHECK(decoded.isOk());
    if (!decoded) return;
    const auto& restored = decoded.value();
    SAME(id);
    SAME(title);
    SAME(directory);
    SAME(model);
    SAME(agent);
    SAME(parentId);
    SAME(depth);
    SAME(created);
    SAME(updated);
    SAME(titleEdited);
    SAME(reasoningEffort);
}

void testMessageAndParts() {
    const auto original = sampleMessage();
    const auto encoded = MaiSessionRecordCodec::encodeMessage(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    const auto decoded = MaiSessionRecordCodec::decodeMessage(encoded.value());
    CHECK(decoded.isOk());
    if (!decoded) return;
    const auto& restored = decoded.value();
    SAME(id);
    SAME(role);
    SAME(created);
    SAME(completed);
    SAME(inputRequestId);
    SAME(pendingInput);
    CHECK(restored.contextImportOrigin.has_value());
    if (restored.contextImportOrigin) {
        SAME(contextImportOrigin->sourceKey);
        SAME(contextImportOrigin->sourceId);
    }
    checkTurn(original, restored);
    CHECK(original.parts.size() == restored.parts.size());
    for (std::size_t index = 0; index < original.parts.size(); ++index) {
        const auto encodedPart = MaiSessionRecordCodec::encodePart(original.parts[index]);
        CHECK(encodedPart.isOk());
        if (!encodedPart) continue;
        const auto decodedPart = MaiSessionRecordCodec::decodePart(encodedPart.value());
        CHECK(decodedPart.isOk());
        if (!decodedPart) continue;
        const auto encodedRestored = MaiSessionRecordCodec::encodePart(restored.parts[index]);
        const auto reencoded = MaiSessionRecordCodec::encodePart(decodedPart.value());
        CHECK(encodedRestored.isOk());
        CHECK(reencoded.isOk());
        if (encodedRestored && reencoded) {
            CHECK(encodedRestored.value() == encodedPart.value());
            CHECK(reencoded.value() == encodedPart.value());
        }
        CHECK(decodedPart.value().id == original.parts[index].id);
        CHECK(decodedPart.value().created == original.parts[index].created);
        CHECK(decodedPart.value().modelDraft == original.parts[index].modelDraft);
    }
    CHECK(std::get<MaiTextPart>(restored.parts[0].body).text ==
          u8"ordinary \u4f60\u597d \U0001f642");
    const auto wire = Json::parse(encoded.value());
    CHECK(wire["parts"][0]["body"]["text"].is_string());
    CHECK(std::get<MaiReasoningPart>(restored.parts[1].body).text == rawBytes());
    const auto state = std::get<MaiReasoningPart>(restored.parts[1].body).continuation;
    CHECK(state.has_value());
    if (state) {
        CHECK(state->format == rawBytes());
        CHECK(state->provider == rawBytes());
        CHECK(state->model == rawBytes());
        CHECK(state->payload == rawBytes());
    }
    CHECK(std::get<MaiImagePart>(restored.parts[2].body).path == rawBytes());
    CHECK(std::get<MaiImagePart>(restored.parts[2].body).mimeType == rawBytes());
    const auto& tool = std::get<MaiToolPart>(restored.parts[3].body);
    CHECK(tool.tool == rawBytes());
    CHECK(tool.callId == "call");
    CHECK(tool.input == rawBytes());
    CHECK(tool.output == rawBytes());
    CHECK(tool.error == rawBytes());
    CHECK(tool.state == MaiToolState::Error);
    CHECK(tool.modelOutputVersion == 1);
    CHECK(tool.execution.has_value());
    if (tool.execution) {
        const auto& facts = *tool.execution;
        CHECK(facts.status == MaiToolExecutionStatus::Failed);
        CHECK(facts.effectStatus == MaiToolEffectStatus::Confirmed);
        CHECK(facts.nextAction == MaiToolNextAction::QueryStatus);
        CHECK(facts.started.has_value());
        CHECK(!facts.started.value_or(true));
        CHECK(facts.exitCode == -42);
        CHECK(facts.timedOut);
        CHECK(facts.canceled);
        CHECK(facts.outputTruncated);
        CHECK(facts.outputId == rawBytes());
        CHECK(facts.taskId == rawBytes());
        CHECK(facts.recoveryTool == rawBytes());
    }
    CHECK(std::get<MaiVideoPart>(restored.parts[4].body).path == rawBytes());
    CHECK(std::get<MaiVideoPart>(restored.parts[4].body).mimeType == rawBytes());
    CHECK(std::get<MaiQuotePart>(restored.parts[5].body).messageId == "quoted-message");
    CHECK(std::get<MaiQuotePart>(restored.parts[5].body).preview == rawBytes());

    MaiMessagePart nul{"nul", MaiTextPart{std::string("a\0b", 3)}, 0, false};
    auto nulEncoded = MaiSessionRecordCodec::encodePart(nul);
    CHECK(nulEncoded.isOk());
    if (nulEncoded) {
        CHECK(Json::parse(nulEncoded.value())["body"]["text"].is_string());
        auto nulDecoded = MaiSessionRecordCodec::decodePart(nulEncoded.value());
        CHECK(nulDecoded.isOk());
        if (nulDecoded)
            CHECK(std::get<MaiTextPart>(nulDecoded.value().body).text == std::string("a\0b", 3));
    }
}

void testCheckpoint() {
    MaiContextCheckpoint original;
    original.sessionId = "session";
    original.revision = std::numeric_limits<std::uint64_t>::max();
    original.sourceMessageCount = 3;
    original.firstMessageId = "first";
    original.throughMessageId = "through";
    original.sourceFingerprint = rawBytes();
    original.model = rawBytes();
    original.summary = rawBytes();
    original.references = rawBytes();
    original.created = -9;
    original.partialMessageId = "partial";
    original.partialPartCount = 2;
    original.throughPartId = "part";
    original.partialSourceFingerprint = rawBytes();
    original.preservedUserMessageId = "user";
    auto encoded = MaiSessionRecordCodec::encodeCheckpoint(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    auto decoded = MaiSessionRecordCodec::decodeCheckpoint(encoded.value());
    CHECK(decoded.isOk());
    if (!decoded) return;
    const auto& restored = decoded.value();
    SAME(sessionId);
    SAME(revision);
    SAME(sourceMessageCount);
    SAME(firstMessageId);
    SAME(throughMessageId);
    SAME(sourceFingerprint);
    SAME(model);
    SAME(summary);
    SAME(references);
    SAME(created);
    SAME(partialMessageId);
    SAME(partialPartCount);
    SAME(throughPartId);
    SAME(partialSourceFingerprint);
    SAME(preservedUserMessageId);
}

void testToolOutput() {
    MaiToolOutputRecord original{"output", "session",  "message", "part",
                                 "call",   rawBytes(), true,      -77};
    auto encoded = MaiSessionRecordCodec::encodeToolOutput(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    auto decoded = MaiSessionRecordCodec::decodeToolOutput(encoded.value());
    CHECK(decoded.isOk());
    if (!decoded) return;
    const auto& restored = decoded.value();
    SAME(id);
    SAME(sessionId);
    SAME(messageId);
    SAME(partId);
    SAME(callId);
    SAME(text);
    SAME(truncated);
    SAME(created);
    original.id = std::string("bad\0id", 6);
    CHECK(!MaiSessionRecordCodec::encodeToolOutput(original));
}

void testTurnState() {
    const auto original = sampleMessage();
    auto encoded = MaiSessionRecordCodec::encodeTurnState(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    MaiMessage restored;
    restored.id = original.id;
    restored.role = original.role;
    restored.inputRequestId = "unchanged";
    CHECK(!MaiSessionRecordCodec::decodeTurnState(encoded.value(), restored));
    CHECK(restored.inputRequestId == "unchanged");
    checkTurn(original, restored);
    const auto savedId = restored.turnId;
    CHECK(MaiSessionRecordCodec::decodeTurnState("{bad", restored).code() ==
          MaiErrorCode::InvalidInput);
    CHECK(restored.turnId == savedId);
    CHECK(!MaiSessionRecordCodec::decodeTurnState("", restored));
    CHECK(restored.turnId.empty());
    CHECK(!restored.turnOutcome);
}

void testCorruptRecords() {
    CHECK(!MaiSessionRecordCodec::decodeSession("{}"));
    CHECK(!MaiSessionRecordCodec::decodeMessage("[]"));
    CHECK(!MaiSessionRecordCodec::decodePart("not-json"));
    CHECK(!MaiSessionRecordCodec::decodeCheckpoint("null"));
    CHECK(!MaiSessionRecordCodec::decodeToolOutput("{}"));
    auto source = MaiSessionRecordCodec::encodeMessage(sampleMessage());
    CHECK(source.isOk());
    if (!source) return;
    auto object = Json::parse(source.value());
    const auto reject = [](Json corrupt) {
        auto result = MaiSessionRecordCodec::decodeMessage(corrupt.dump());
        CHECK(!result);
        CHECK(result.error().code() == MaiErrorCode::InvalidInput);
    };
    auto corrupt = object;
    corrupt["codec_version"] = 2;
    reject(corrupt);
    corrupt = object;
    corrupt.erase("codec_version");
    reject(corrupt);
    corrupt = object;
    corrupt["role"] = "system";
    reject(corrupt);
    corrupt = object;
    corrupt["created"] = 1.5;
    reject(corrupt);
    corrupt = object;
    corrupt["pending_input"] = 1;
    reject(corrupt);
    corrupt = object;
    corrupt["parts"][1]["id"] = corrupt["parts"][0]["id"];
    reject(corrupt);
    corrupt = object;
    corrupt["turn_state"]["outcome"]["turn_id"] = "foreign";
    reject(corrupt);
    corrupt = object;
    corrupt["turn_state"]["outcome"]["status"] = "future";
    reject(corrupt);
    corrupt = object;
    corrupt["turn_state"]["outcome"]["error_code"] = "future";
    reject(corrupt);
    corrupt = object;
    corrupt["parts"][3]["body"]["state"] = "future";
    reject(corrupt);
    corrupt = object;
    corrupt["parts"][3]["body"]["model_output_version"] = 2;
    reject(corrupt);
    corrupt = object;
    corrupt["parts"][3]["body"]["execution"]["status"] = "future";
    reject(corrupt);
    corrupt = object;
    corrupt["turn_state"]["outcome"]["context"]["output_tokens"] = -1;
    reject(corrupt);
    corrupt = object;
    corrupt["parts"][1]["body"]["text"] = {{"$mai_bytes", "base64"}, {"data", "!"}};
    reject(corrupt);

    auto invalid = sampleMessage();
    invalid.role = static_cast<MaiRole>(99);
    CHECK(!MaiSessionRecordCodec::encodeMessage(invalid));
    invalid = sampleMessage();
    std::get<MaiToolPart>(invalid.parts[3].body).state = static_cast<MaiToolState>(99);
    CHECK(!MaiSessionRecordCodec::encodeMessage(invalid));
    invalid = sampleMessage();
    invalid.turnOutcome->error = {static_cast<MaiErrorCode>(99), "invalid"};
    CHECK(!MaiSessionRecordCodec::encodeMessage(invalid));
}

void testLegacyTurnStateAndErrorCodes() {
    const char* legacy =
        "{\"turn_id\":\"initial\",\"outcome\":{\"turn_id\":\"initial\","
        "\"status\":\"failed\",\"result_message_id\":\"result\",\"completed\":42,"
        "\"error_code\":\"network\",\"error\":\"offline\",\"context\":{"
        "\"phase\":\"deferred\",\"before\":100,\"after\":90,\"before_exact\":false,"
        "\"after_exact\":true,\"window\":200,\"input_budget\":180,\"revision\":3,"
        "\"original_users\":4,\"retained_users\":2,\"reused\":true,\"advanced\":false,"
        "\"history_index\":false,\"error_code\":\"busy\",\"error\":\"later\"}}}";
    MaiMessage value;
    value.id = "initial";
    value.role = MaiRole::Assistant;
    CHECK(!MaiSessionRecordCodec::decodeTurnState(legacy, value));
    CHECK(value.turnOutcome.has_value());
    if (value.turnOutcome) {
        CHECK(value.turnOutcome->error.code() == MaiErrorCode::Network);
        CHECK(value.turnOutcome->context.has_value());
        if (value.turnOutcome->context) {
            CHECK(value.turnOutcome->context->phase == MaiContextPhase::Deferred);
            CHECK(value.turnOutcome->context->outputTokens == 0);
            CHECK(value.turnOutcome->context->requestBodyBytes == 0);
            CHECK(value.turnOutcome->context->error.code() == MaiErrorCode::Busy);
        }
    }
    const auto unchanged = value;
    auto corrupt = Json::parse(legacy);
    corrupt["codec_version"] = 18;
    CHECK(MaiSessionRecordCodec::decodeTurnState(corrupt.dump(), value));
    checkTurn(unchanged, value);

    for (auto code : {MaiErrorCode::Ok, MaiErrorCode::NotFound, MaiErrorCode::Busy,
                      MaiErrorCode::InvalidInput, MaiErrorCode::NotConfigured,
                      MaiErrorCode::Network, MaiErrorCode::RateLimited, MaiErrorCode::Protocol,
                      MaiErrorCode::Canceled, MaiErrorCode::NotSupported, MaiErrorCode::Internal,
                      MaiErrorCode::ContextWindowExceeded, MaiErrorCode::RequestBodyTooLarge}) {
        auto original = sampleMessage();
        original.turnOutcome->error = {code, rawBytes()};
        auto encoded = MaiSessionRecordCodec::encodeMessage(original);
        CHECK(encoded.isOk());
        if (!encoded) continue;
        auto decoded = MaiSessionRecordCodec::decodeMessage(encoded.value());
        CHECK(decoded.isOk());
        if (decoded) checkTurn(original, decoded.value());
    }
}

void testNumericBoundsAndTaggedBytes() {
    auto source = MaiSessionRecordCodec::encodeMessage(sampleMessage());
    CHECK(source.isOk());
    if (!source) return;
    auto object = Json::parse(source.value());
    auto corrupt = object;
    corrupt["created"] = std::numeric_limits<std::uint64_t>::max();
    CHECK(!MaiSessionRecordCodec::decodeMessage(corrupt.dump()));
    corrupt = object;
    corrupt["turn_state"]["outcome"]["retry_advice"]["max_retries"] =
        std::numeric_limits<std::uint64_t>::max();
    CHECK(!MaiSessionRecordCodec::decodeMessage(corrupt.dump()));
    for (const auto& tagged : {Json{{"$mai_bytes", "unknown"}, {"data", "/w=="}},
                               Json{{"$mai_bytes", "base64"}, {"data", "/x=="}},
                               Json{{"$mai_bytes", "base64"}, {"data", "YQ=="}},
                               Json{{"$mai_bytes", "base64"}, {"data", "/w=="}, {"extra", 0}}}) {
        corrupt = object;
        corrupt["parts"][1]["body"]["text"] = tagged;
        CHECK(!MaiSessionRecordCodec::decodeMessage(corrupt.dump()));
    }
    corrupt = object;
    corrupt["parts"][1]["body"]["text"] = Json{{"$mai_bytes", "base64"}, {"data", "/w=="}};
    auto decoded = MaiSessionRecordCodec::decodeMessage(corrupt.dump());
    CHECK(decoded.isOk());
    if (decoded)
        CHECK(std::get<MaiReasoningPart>(decoded.value().parts[1].body).text ==
              std::string(1, static_cast<char>(0xff)));
}

void testOptionalFactsAndStableEnums() {
    auto original = sampleMessage();
    original.contextImportOrigin.reset();
    original.turnOutcome.reset();
    original.turnId.clear();
    original.inputRequestId.clear();
    original.pendingInput = false;
    original.role = MaiRole::User;
    auto& reasoning = std::get<MaiReasoningPart>(original.parts[1].body);
    reasoning.continuation.reset();
    auto& tool = std::get<MaiToolPart>(original.parts[3].body);
    tool.modelOutputVersion = 0;
    tool.execution.reset();
    auto encoded = MaiSessionRecordCodec::encodeMessage(original);
    CHECK(encoded.isOk());
    if (encoded) {
        auto decoded = MaiSessionRecordCodec::decodeMessage(encoded.value());
        CHECK(decoded.isOk());
        if (decoded) {
            CHECK(decoded.value().role == MaiRole::User);
            CHECK(!decoded.value().contextImportOrigin);
            CHECK(!decoded.value().turnOutcome);
            CHECK(decoded.value().turnId.empty());
            CHECK(!std::get<MaiReasoningPart>(decoded.value().parts[1].body).continuation);
            CHECK(!std::get<MaiToolPart>(decoded.value().parts[3].body).execution);
            CHECK(std::get<MaiToolPart>(decoded.value().parts[3].body).modelOutputVersion == 0);
        }
    }
    for (auto status : {MaiTurnStatus::Running, MaiTurnStatus::Completed, MaiTurnStatus::Failed,
                        MaiTurnStatus::Interrupted}) {
        auto value = sampleMessage();
        value.turnOutcome->status = status;
        if (status == MaiTurnStatus::Running) value.turnOutcome->completed = 0;
        auto wire = MaiSessionRecordCodec::encodeMessage(value);
        CHECK(wire.isOk());
        if (wire) {
            auto parsed = MaiSessionRecordCodec::decodeMessage(wire.value());
            CHECK(parsed.isOk());
            if (parsed) checkTurn(value, parsed.value());
        }
    }
    for (auto phase :
         {MaiContextPhase::Preparing, MaiContextPhase::Summarizing, MaiContextPhase::Ready,
          MaiContextPhase::Deferred, MaiContextPhase::Failed, MaiContextPhase::Interrupted}) {
        auto value = sampleMessage();
        value.turnOutcome->context->phase = phase;
        auto wire = MaiSessionRecordCodec::encodeMessage(value);
        CHECK(wire.isOk());
        if (wire) {
            auto parsed = MaiSessionRecordCodec::decodeMessage(wire.value());
            CHECK(parsed.isOk());
            if (parsed) checkTurn(value, parsed.value());
        }
    }
    for (bool present : {false, true}) {
        MaiToolExecution facts;
        if (present) {
            facts.started = false;
            facts.exitCode = 0;
        }
        MaiToolPart body;
        body.callId = "call";
        body.execution = facts;
        MaiMessagePart part{"part", body, 0, false};
        auto wire = MaiSessionRecordCodec::encodePart(part);
        CHECK(wire.isOk());
        if (wire) {
            auto parsed = MaiSessionRecordCodec::decodePart(wire.value());
            CHECK(parsed.isOk());
            if (parsed) {
                const auto restored = std::get<MaiToolPart>(parsed.value().body).execution;
                CHECK(restored.has_value());
                if (restored) {
                    CHECK(restored->started == facts.started);
                    CHECK(restored->exitCode == facts.exitCode);
                }
            }
        }
    }
}

void testToolOutputBinaryBudgetAndCompatibility() {
    MaiToolOutputRecord original{"output", "session", "message",
                                 "part",   "call",    std::string(kMaiToolOutputMaxBytes, '\0'),
                                 false,    1};
    auto encoded = MaiSessionRecordCodec::encodeToolOutput(original);
    CHECK(encoded.isOk());
    if (!encoded) return;
    CHECK(encoded.value().size() < 23u * 1024u * 1024u);
    const auto object = Json::parse(encoded.value());
    CHECK(object["text"].is_object());
    if (object["text"].is_object()) CHECK(object["text"]["$mai_binary"] == "base64");
    auto decoded = MaiSessionRecordCodec::decodeToolOutput(encoded.value());
    CHECK(decoded.isOk());
    if (decoded) CHECK(decoded.value().text == original.text);

    // 旧 UTF-8 字符串和旧非法字节包装仍可读；只有原始输出字段接受新二进制包装。
    auto legacy = object;
    legacy["text"] = std::string("legacy\0body", 11);
    auto legacyDecoded = MaiSessionRecordCodec::decodeToolOutput(legacy.dump());
    CHECK(legacyDecoded.isOk());
    if (legacyDecoded) CHECK(legacyDecoded.value().text == std::string("legacy\0body", 11));
    legacy["text"] = {{"$mai_bytes", "base64"}, {"data", "/w=="}};
    legacyDecoded = MaiSessionRecordCodec::decodeToolOutput(legacy.dump());
    CHECK(legacyDecoded.isOk());
    if (legacyDecoded) CHECK(legacyDecoded.value().text == std::string(1, static_cast<char>(0xff)));
    for (const auto& invalid : {Json{{"$mai_binary", "base64"}, {"data", "/x=="}},
                                Json{{"$mai_binary", "unknown"}, {"data", "YQ=="}},
                                Json{{"$mai_binary", "base64"}, {"data", "YQ=="}, {"extra", 0}}}) {
        legacy["text"] = invalid;
        CHECK(!MaiSessionRecordCodec::decodeToolOutput(legacy.dump()));
    }
    auto message = MaiSessionRecordCodec::encodeMessage(sampleMessage());
    CHECK(message.isOk());
    if (message) {
        auto changed = Json::parse(message.value());
        changed["parts"][0]["body"]["text"] = {{"$mai_binary", "base64"}, {"data", "YQ=="}};
        CHECK(!MaiSessionRecordCodec::decodeMessage(changed.dump()));
    }
}

}  // namespace

int main() {
    testSession();
    testMessageAndParts();
    testCheckpoint();
    testToolOutput();
    testTurnState();
    testCorruptRecords();
    testLegacyTurnStateAndErrorCodes();
    testNumericBoundsAndTaggedBytes();
    testOptionalFactsAndStableEnums();
    testToolOutputBinaryBudgetAndCompatibility();
    std::printf("MaiSessionRecordCodecTests: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
