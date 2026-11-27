// 文件作用：验证唯一 Responses 网络入口、业务失败隔离和供应商用量来源。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "MaiResponsesClient.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (false)

class ResponseServer {
public:
    explicit ResponseServer(bool businessError = false,
                            nlohmann::json usage = {{"input_tokens", 8},
                                                    {"output_tokens", 6},
                                                    {"total_tokens", 14}},
                            bool eventEnvelope = false) {
        mServer.Post("/responses", [this, businessError, usage, eventEnvelope](
                                       const httplib::Request& request,
                                       httplib::Response& response) {
            ++requests;
            const auto body = nlohmann::json::parse(request.body);
            CHECK(body.contains("input"));
            CHECK(!body.contains("messages"));
            if (businessError) {
                const std::string error = R"({"code":1001,"msg":"Authorization is required"})";
                response.set_content(eventEnvelope ? "data: " + error + "\n\n" : error,
                                     eventEnvelope ? "text/event-stream" : "application/json");
                return;
            }
            const nlohmann::json item{
                {"id", "msg_owned"},
                {"type", "message"},
                {"role", "assistant"},
                {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", "OK"}}})}};
            const nlohmann::json event{{"type", "response.completed"},
                                       {"response",
                                        {{"id", "resp_owned"},
                                         {"status", "completed"},
                                         {"output", nlohmann::json::array({item})},
                                         {"usage", usage}}}};
            response.set_content("data: " + event.dump() + "\n\n", "text/event-stream");
        });
        const auto port = mServer.bind_to_any_port("127.0.0.1");
        CHECK(port > 0);
        url = "http://127.0.0.1:" + std::to_string(port);
        mThread = std::thread([this] { mServer.listen_after_bind(); });
        for (int attempt = 0; attempt < 1000 && !mServer.is_running(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(mServer.is_running());
    }

    ~ResponseServer() {
        mServer.stop();
        mThread.join();
    }

    std::string url;
    std::atomic<unsigned> requests{0};

private:
    httplib::Server mServer;
    std::thread mThread;
};

void test_default_factory_uses_only_responses() {
    ResponseServer server;
    MaiModelConfig config;
    config.baseUrl = server.url;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    CHECK(client->wireApi() == MaiWireApi::Responses);
    MaiModelRequest request;
    request.model = "owned-model";
    request.messages.push_back({MaiModelRole::User, "Hello"});
    std::string text;
    MaiStreamSink sink;
    sink.onText = [&](std::string_view delta) { text.append(delta); };
    const std::atomic<bool> cancel{false};
    const auto result = client->stream(request, sink, cancel);
    CHECK(!result);
    CHECK(text == "OK");
    CHECK(server.requests == 1);
}

void test_glm_http_200_business_error_is_not_a_successful_response() {
    for (const bool eventEnvelope : {false, true}) {
        ResponseServer server(true, nullptr, eventEnvelope);
        MaiModelConfig config;
        config.baseUrl = server.url;
        config.wire = MaiWireApi::Responses;
        config.maxRetries = 0;
        auto client = makeMaiModelClient(config);
        MaiModelRequest request;
        request.model = "glm-5.3-flash";
        request.messages.push_back({MaiModelRole::User, "Hello"});
        unsigned published = 0;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view) { ++published; };
        sink.onToolCall = [&](const MaiToolInvocation&) { ++published; };
        sink.onContinuationState = [&](const MaiModelContinuationState&) { ++published; };
        sink.onUsage = [&](const MaiModelUsage&) { ++published; };
        const std::atomic<bool> cancel{false};
        const auto result = client->stream(request, sink, cancel);
        if (result.code() != MaiErrorCode::NotConfigured)
            std::printf("Business-error result: code=%d message=%s\n",
                        static_cast<int>(result.code()), result.message().c_str());
        CHECK(result.code() == MaiErrorCode::NotConfigured);
        CHECK(published == 0);
        CHECK(server.requests == 1);
    }
}

void test_usage_provenance_and_opaque_provider_details() {
    using Json = nlohmann::json;
    const Json complete{{"input_tokens", 8},
                        {"output_tokens", 6},
                        {"total_tokens", 14},
                        {"input_tokens_details",
                         {{"cached_tokens", 3},
                          {"cache_write_tokens", 2},
                          {"cache_write_tokens_details", {{"ttl_12h_tokens", 2}}}}},
                        {"output_tokens_details", {{"reasoning_tokens", 4}}}};
    for (const Json& usage : {complete, Json{{"input_tokens", 8}, {"output_tokens", 6}},
                              Json{{"input_tokens", 0},
                                   {"output_tokens", 0},
                                   {"total_tokens", 0},
                                   {"output_tokens_details", {{"reasoning_tokens", 0}}}}}) {
        ResponseServer server(false, usage);
        MaiModelConfig config;
        config.baseUrl = server.url;
        config.maxRetries = 0;
        auto client = makeMaiModelClient(config);
        MaiModelRequest request;
        request.model = "owned-model";
        unsigned received = 0;
        MaiStreamSink sink;
        sink.onUsage = [&](const MaiModelUsage& actual) {
            ++received;
            CHECK(actual.inputSource == MaiUsageValueSource::Provider);
            CHECK(actual.outputSource == MaiUsageValueSource::Provider);
            CHECK(actual.totalTokens == usage.value("total_tokens", 14));
            CHECK(actual.totalSource == (usage.contains("total_tokens")
                                             ? MaiUsageValueSource::Provider
                                             : MaiUsageValueSource::Derived));
            CHECK(actual.reasoningOutputSource == (usage.contains("output_tokens_details")
                                                       ? MaiUsageValueSource::Provider
                                                       : MaiUsageValueSource::Missing));
            CHECK(actual.cachedInputSource == (usage.contains("input_tokens_details")
                                                   ? MaiUsageValueSource::Provider
                                                   : MaiUsageValueSource::Missing));
            CHECK(actual.cachedInputTokens == (usage == complete ? 3 : 0));
            CHECK(actual.cacheWriteInputTokens == (usage == complete ? 2 : 0));
            CHECK(actual.reasoningOutputTokens == (usage == complete ? 4 : 0));
            CHECK(!actual.sourcePayload.empty());
            CHECK(actual.responseId == "resp_owned");
            if (!actual.sourcePayload.empty()) CHECK(Json::parse(actual.sourcePayload) == usage);
        };
        const std::atomic<bool> cancel{false};
        CHECK(!client->stream(request, sink, cancel));
        CHECK(received == 1);
    }
}

void test_missing_null_invalid_and_partial_usage_are_not_provider_zero() {
    using Json = nlohmann::json;
    const Json degraded{
        {"input_tokens", 8},
        {"output_tokens", 6},
        {"total_tokens", nullptr},
        {"input_tokens_details", {{"cached_tokens", nullptr}, {"cache_write_tokens", -2}}},
        {"output_tokens_details", {{"reasoning_tokens", "4"}}}};
    const Json totalOnly{{"total_tokens", 88}};
    for (const Json& usage : {degraded, totalOnly, Json{}, Json(nullptr)}) {
        ResponseServer server(false, usage);
        MaiModelConfig config;
        config.baseUrl = server.url;
        config.maxRetries = 0;
        auto client = makeMaiModelClient(config);
        MaiModelRequest request;
        request.model = "owned-model";
        unsigned received = 0;
        MaiStreamSink sink;
        sink.onUsage = [&](const MaiModelUsage& actual) {
            ++received;
            CHECK(usage == degraded || usage == totalOnly);
            CHECK(actual.cachedInputTokens == 0);
            CHECK(actual.cacheWriteInputTokens == 0);
            CHECK(actual.reasoningOutputTokens == 0);
            CHECK(actual.cachedInputSource == MaiUsageValueSource::Missing);
            CHECK(actual.cacheWriteInputSource == MaiUsageValueSource::Missing);
            CHECK(actual.reasoningOutputSource == MaiUsageValueSource::Missing);
            CHECK(actual.totalTokens == (usage == degraded ? 14 : 88));
            CHECK(actual.totalSource == (usage == degraded ? MaiUsageValueSource::Derived
                                                           : MaiUsageValueSource::Provider));
            CHECK(actual.inputSource == (usage == degraded ? MaiUsageValueSource::Provider
                                                           : MaiUsageValueSource::Missing));
            CHECK(actual.outputSource == actual.inputSource);
            CHECK(Json::parse(actual.sourcePayload) == usage);
        };
        const std::atomic<bool> cancel{false};
        CHECK(!client->stream(request, sink, cancel));
        CHECK(received == (usage == degraded || usage == totalOnly ? 1 : 0));
    }
}
}  // namespace

void test_overflowing_usage_total_is_missing() {
    using nlohmann::json;
    const json usage{{"input_tokens", std::numeric_limits<std::size_t>::max()}, {"output_tokens", 1}};
    ResponseServer server(false, usage);
    MaiModelConfig config;
    config.baseUrl = server.url;
    auto client = makeMaiResponsesClient(config);
    MaiModelRequest request;
    request.model = "owned-overflow";
    request.messages.push_back({MaiModelRole::User, "Owned usage overflow fixture"});
    int observed = 0;
    MaiStreamSink sink;
    sink.onUsage = [&](const MaiModelUsage& value) {
        ++observed;
        CHECK(value.totalSource == MaiUsageValueSource::Missing);
        CHECK(value.totalTokens == 0);
        CHECK(json::parse(value.sourcePayload) == usage);
    };
    const std::atomic<bool> canceled{false};
    CHECK(!client->stream(request, sink, canceled));
    CHECK(observed == 1);
}

int main() {
    test_default_factory_uses_only_responses();
    test_glm_http_200_business_error_is_not_a_successful_response();
    test_usage_provenance_and_opaque_provider_details();
    test_missing_null_invalid_and_partial_usage_are_not_provider_zero();
    test_overflowing_usage_total_is_missing();
    std::printf("Responses protocol failures: %d\n", failures);
    return failures ? 1 : 0;
}
