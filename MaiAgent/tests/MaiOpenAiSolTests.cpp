// 文件作用：用本地回环服务器验证 Sol 官方能力、Responses 线格式及无状态推理续接。
// 所有凭据均为测试占位符；不访问真实供应商，不加载用户账号或会话数据。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiCloudModelCatalog.h"
#include "MaiResponsesClient.h"
#include "MaiRoutedModelClient.h"
#include "MaiThread.h"

using nlohmann::json;

namespace {
int failures = 0;
int checks = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

constexpr const char* kModel = "gpt-6.1-sol";

std::string completed(const json& output, const json& usage = json()) {
    json response{{"id", "resp_sol_fixture"}, {"status", "completed"}, {"output", output}};
    if (!usage.is_null()) response["usage"] = usage;
    return "data: " + json{{"type", "response.completed"}, {"response", response}}.dump() + "\n\n";
}

struct LoopbackServer {
    httplib::Server server;
    std::thread worker;
    std::mutex mutex;
    std::vector<json> bodies;
    std::vector<std::string> paths;
    std::vector<std::string> authorizations;
    std::string script = completed(json::array());
    int status = 200;
    int port = 0;

    LoopbackServer() {
        server.Post(
            "/v1/responses", [this](const httplib::Request& request, httplib::Response& response) {
                std::string bytes;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    bodies.push_back(json::parse(request.body, nullptr, false));
                    paths.push_back(request.path);
                    authorizations.push_back(request.get_header_value("Authorization"));
                    bytes = script;
                    response.status = status;
                }
                // 分片穿过 JSON 字段边界，实际客户端必须完成 SSE 拼装。
                auto data = std::make_shared<std::string>(std::move(bytes));
                auto offset = std::make_shared<std::size_t>(0);
                response.set_chunked_content_provider(
                    "text/event-stream", [data, offset](std::size_t, httplib::DataSink& sink) {
                        if (*offset == data->size()) {
                            sink.done();
                            return false;
                        }
                        const auto count = std::min<std::size_t>(7, data->size() - *offset);
                        const bool result = sink.write(data->data() + *offset, count);
                        *offset += count;
                        return result;
                    });
            });
        port = server.bind_to_any_port("127.0.0.1");
        CHECK(port > 0);
        worker = std::thread([this] {
            MaiThread::setCurrentName("mai-sol-test");
            server.listen_after_bind();
        });
        for (int attempt = 0; attempt < 200 && !server.is_running(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(server.is_running());
    }

    ~LoopbackServer() {
        server.stop();
        if (worker.joinable()) worker.join();
    }

    MaiModelConfig config() const {
        MaiModelConfig result;
        result.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        result.apiKey = "sol-loopback-placeholder";
        result.maxRetries = 0;
        result.totalTimeoutSeconds = 10;
        result.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
            proxy = {};
            return true;
        };
        return result;
    }

    json lastBody() {
        std::lock_guard<std::mutex> lock(mutex);
        return bodies.empty() ? json() : bodies.back();
    }

    std::size_t requests() {
        std::lock_guard<std::mutex> lock(mutex);
        return bodies.size();
    }
};

MaiModelRequest prompt() {
    MaiModelRequest request;
    request.model = kModel;
    request.sessionId = "ses_sol_loopback_fixture";
    request.baseInstructions = "Inspect the fixture and use the declared tool.";
    MaiModelMessage user;
    user.content = "Read the fixture.";
    request.messages.push_back(user);
    request.tools.push_back(
        {"read_fixture", "Read the named fixture.",
         R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})"});
    return request;
}

void testOfficialCapabilities() {
    const auto info = maiCloudModelInfo(kModel);
    CHECK(info.has_value());
    if (info) {
        CHECK(info->model == kModel);
        CHECK(info->provider == "openai");
        CHECK(info->wire == MaiWireApi::Responses);
        CHECK(info->supportsImages);
        CHECK(info->supportsReasoningEffort);
        CHECK(info->contextLimits.contextWindowTokens == 1050000);
        CHECK(info->contextLimits.maxOutputTokens == 128000);
        CHECK(info->contextLimits.defaultOutputReserveTokens == 0);
    }
    CHECK(!maiCloudModelInfo("gpt-6.1-sol-latest"));
    CHECK(!maiCloudModelInfo("openai/gpt-6.1-sol"));
    CHECK(!maiCloudModelInfo("gpt-6.1"));
    const auto limits = maiOfficialModelContextLimits("https://api.openai.com/v1/", kModel);
    CHECK(limits.contextWindowTokens == 1050000);
    CHECK(limits.maxOutputTokens == 128000);
    CHECK(limits.defaultOutputReserveTokens == 0);
    CHECK(maiOfficialModelContextLimits("https://proxy.invalid/v1", kModel).contextWindowTokens ==
          0);
    CHECK(maiOfficialModelContextLimits("https://api.openai.com/v1.evil", kModel)
              .contextWindowTokens == 0);
    CHECK(maiOfficialModelContextLimits("https://api.openai.com/v1", "gpt-6.1-sol-latest")
              .contextWindowTokens == 0);
    const std::vector<std::string> efforts{"low", "medium", "high", "xhigh", "max"};
    CHECK(maiReasoningEffortsForModel(kModel) == efforts);
    CHECK(maiIsValidReasoningEffortForModel(kModel, ""));
    for (const auto& effort : efforts) {
        CHECK(maiIsValidReasoningEffortForModel(kModel, effort));
        CHECK(maiNormalizeReasoningEffortForModel(kModel, effort) == effort);
    }
    for (const auto* invalid : {"none", "minimal", "MAX", "auto", "invalid"})
        CHECK(!maiIsValidReasoningEffortForModel(kModel, invalid));
    CHECK(!maiIsValidReasoningEffort("medium"));
    CHECK(!maiIsValidReasoningEffortForModel("gpt-6.1-sol-latest", "xhigh"));
    CHECK(maiNormalizeReasoningEffortForModel("glm-5.4", "medium") == "high");
    CHECK(maiNormalizeReasoningEffortForModel(kModel, "invalid") == "invalid");
}

void testResponsesBodyAndExactRoute() {
    LoopbackServer fake;
    int solCredentials = 0;
    int otherCredentials = 0;
    MaiModelRoute sol;
    sol.modelNames = {kModel};
    sol.config = fake.config();
    sol.apiKeyProvider = [&] {
        ++solCredentials;
        return "sol-loopback-placeholder";
    };
    MaiModelRoute other;
    other.modelNames = {"glm-5.4"};
    other.config = fake.config();
    other.apiKeyProvider = [&] {
        ++otherCredentials;
        return "other-loopback-placeholder";
    };
    auto client = makeMaiRoutedModelClient({{sol, other}});
    CHECK(client->wireApiForModel(kModel) == MaiWireApi::Responses);
    CHECK(!client->wireApiForModel("gpt-6.1-sol-latest"));
    CHECK(solCredentials == 0 && otherCredentials == 0);
    auto request = prompt();
    const std::atomic<bool> cancel{false};
    for (const auto* effort : {"", "low", "medium", "high", "xhigh", "max"}) {
        request.reasoningEffort = effort;
        CHECK(!client->stream(request, {}, cancel));
        const auto body = fake.lastBody();
        CHECK(body.value("model", "") == kModel);
        CHECK(body.value("store", true) == false);
        CHECK(body.value("stream", false));
        CHECK(body.value("prompt_cache_key", "") == request.sessionId);
        CHECK(body.value("include", json::array()) == json::array({"reasoning.encrypted_content"}));
        CHECK(body.value("instructions", "") == request.baseInstructions);
        CHECK(!body.contains("session_id") && !body.contains("cache_control"));
        CHECK(!body.contains("previous_response_id") && !body.contains("conversation"));
        CHECK(!body.contains("max_output_tokens") && !body.contains("temperature"));
        CHECK(body["tools"][0]["type"] == "function");
        CHECK(body["tools"][0]["name"] == "read_fixture");
        if (request.reasoningEffort.empty())
            CHECK(!body.contains("reasoning"));
        else
            CHECK(body["reasoning"] == json({{"effort", effort}}));
    }
    const auto count = fake.requests();
    for (const auto* invalid : {"none", "minimal", "auto"}) {
        request.reasoningEffort = invalid;
        CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::InvalidInput);
    }
    CHECK(fake.requests() == count);
    request.reasoningEffort.clear();
    request.model = "gpt-6.1-sol-latest";
    const int credentialReads = solCredentials;
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::InvalidInput);
    CHECK(solCredentials == credentialReads && otherCredentials == 0);
    CHECK(fake.requests() == count);
    request.model = kModel;
    request.sessionId.clear();
    request.maxOutputTokens = 1024;
    CHECK(!client->stream(request, {}, cancel));
    CHECK(!fake.lastBody().contains("prompt_cache_key"));
    CHECK(fake.lastBody()["max_output_tokens"] == 1024);
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        for (const auto& path : fake.paths) CHECK(path == "/v1/responses");
        for (const auto& authorization : fake.authorizations)
            CHECK(authorization == "Bearer sol-loopback-placeholder");
        fake.status = 400;
        fake.script = R"({"error":{"message":"fixture failure"}})";
    }
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::Protocol);
    CHECK(fake.requests() == count + 2);
    CHECK(otherCredentials == 0);
}

void testEncryptedReasoningToolReplayAndUsage() {
    LoopbackServer fake;
    const json output = json::array(
        {json{{"type", "reasoning"},
              {"id", "rs_sol_fixture"},
              {"summary", json::array({json{{"type", "summary_text"}, {"text", "Inspecting."}}})},
              {"encrypted_content", "encrypted-fixture-placeholder"},
              {"future_metadata", json{{"retained", true}}}},
         json{{"type", "function_call"},
              {"id", "fc_sol_fixture"},
              {"call_id", "call_sol_fixture"},
              {"name", "read_fixture"},
              {"arguments", R"({"path":"fixture.txt"})"},
              {"status", "completed"}}});
    const json usage{{"input_tokens", 120},
                     {"output_tokens", 30},
                     {"total_tokens", 150},
                     {"input_tokens_details", json{{"cached_tokens", 0}}},
                     {"output_tokens_details", json{{"reasoning_tokens", 20}}},
                     {"fixture_unknown_usage", json{{"retained", true}}}};
    fake.script = completed(output, usage);
    auto client = makeMaiModelClient(fake.config());
    auto request = prompt();
    MaiModelContinuationState continuation;
    std::vector<MaiModelUsage> usages;
    std::vector<MaiToolInvocation> calls;
    std::vector<std::string> deliveries;
    std::string reasoning;
    MaiStreamSink sink;
    sink.onContinuationState = [&](const MaiModelContinuationState& state) {
        continuation = state;
        deliveries.push_back("state");
    };
    sink.onToolCall = [&](const MaiToolInvocation& call) {
        calls.push_back(call);
        deliveries.push_back("call");
    };
    sink.onReasoning = [&](std::string_view text) { reasoning.append(text); };
    sink.onUsage = [&](const MaiModelUsage& value) { usages.push_back(value); };
    const std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, sink, cancel));
    CHECK(deliveries == std::vector<std::string>({"state", "call"}));
    CHECK(continuation.format == "responses.output.v1");
    CHECK(continuation.provider == "openai");
    CHECK(continuation.model == kModel);
    CHECK(!continuation.payload.empty());
    if (!continuation.payload.empty()) CHECK(json::parse(continuation.payload) == output);
    CHECK(reasoning == "Inspecting.");
    CHECK(calls.size() == 1);
    if (!calls.empty()) {
        CHECK(calls[0].id == "call_sol_fixture");
        CHECK(calls[0].name == "read_fixture");
        CHECK(calls[0].arguments == R"({"path":"fixture.txt"})");
    }
    CHECK(usages.size() == 1);
    if (!usages.empty()) {
        const auto& value = usages[0];
        CHECK(value.inputTokens == 120 && value.outputTokens == 30 && value.totalTokens == 150);
        CHECK(value.inputSource == MaiUsageValueSource::Provider);
        CHECK(value.outputSource == MaiUsageValueSource::Provider);
        CHECK(value.totalSource == MaiUsageValueSource::Provider);
        CHECK(value.cachedInputTokens == 0 &&
              value.cachedInputSource == MaiUsageValueSource::Provider);
        CHECK(value.cacheWriteInputSource == MaiUsageValueSource::Missing);
        CHECK(value.reasoningOutputTokens == 20 &&
              value.reasoningOutputSource == MaiUsageValueSource::Provider);
        CHECK(value.responseId == "resp_sol_fixture");
        CHECK(json::parse(value.sourcePayload) == usage);
    }
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.content = "display-only-fixture";
    assistant.reasoning = "display-reasoning-must-not-be-synthesized";
    assistant.invocations = calls;
    assistant.continuation = continuation;
    request.messages.push_back(assistant);
    MaiModelMessage result;
    result.role = MaiModelRole::ToolResult;
    result.toolCallId = "call_sol_fixture";
    result.content = "fixture contents";
    request.messages.push_back(result);
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        fake.script = completed(json::array(), json{{"input_tokens", 0}, {"output_tokens", 0}});
    }
    usages.clear();
    CHECK(!client->stream(request, sink, cancel));
    const auto body = fake.lastBody();
    CHECK(body["input"].size() == 4);
    CHECK(body["input"][1] == output[0]);
    CHECK(body["input"][2] == output[1]);
    CHECK(body["input"][3] == json({{"type", "function_call_output"},
                                    {"call_id", "call_sol_fixture"},
                                    {"output", "fixture contents"}}));
    CHECK(body.value("prompt_cache_key", "") == request.sessionId);
    CHECK(body.dump().find("display-only-fixture") == std::string::npos);
    CHECK(body.dump().find("display-reasoning-must-not-be-synthesized") == std::string::npos);
    CHECK(usages.size() == 1);
    if (!usages.empty()) {
        CHECK(usages[0].totalTokens == 0 && usages[0].totalSource == MaiUsageValueSource::Derived);
        CHECK(usages[0].cachedInputSource == MaiUsageValueSource::Missing);
        CHECK(usages[0].reasoningOutputSource == MaiUsageValueSource::Missing);
    }
    // 无原生状态的旧记录只能回放可见正文，不能伪造 OpenAI 加密推理项。
    request.messages[1].continuation.reset();
    CHECK(!client->stream(request, {}, cancel));
    CHECK(fake.lastBody().dump().find("display-reasoning-must-not-be-synthesized") ==
          std::string::npos);
    CHECK(fake.lastBody().dump().find("display-only-fixture") != std::string::npos);
    request.messages[1].continuation =
        MaiModelContinuationState{"responses.output.v1", "openrouter", kModel, output.dump()};
    CHECK(!client->stream(request, {}, cancel));
    CHECK(fake.lastBody().dump().find("encrypted-fixture-placeholder") == std::string::npos);
    // 自定义别名不继承 Sol 专属字段。
    request = prompt();
    request.model = "gpt-6.1-sol-latest";
    CHECK(!client->stream(request, {}, cancel));
    CHECK(!fake.lastBody().contains("include"));
    CHECK(!fake.lastBody().contains("prompt_cache_key"));
}
}  // namespace

int main() {
    testOfficialCapabilities();
    testResponsesBodyAndExactRoute();
    testEncryptedReasoningToolReplayAndUsage();
    std::printf("MaiOpenAiSolTests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
