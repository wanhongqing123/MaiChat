// 文件作用：在模型客户端协议边界验证工具发现批次、完整历史和取消；此处不声称模型性能。
#include "MaiLlamaToolDiscoveryClient.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiThread.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

using Step = std::function<MaiError(const MaiModelRequest&, const MaiStreamSink&)>;
class MaiScriptedLocalClient final : public MaiModelClient {
public:
    std::vector<Step> steps;
    std::vector<MaiModelRequest> requests;
    MaiWireApi wire = MaiWireApi::Local;

    MaiWireApi wireApi() const override {
        return wire;
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        requests.push_back(request);
        const auto index = requests.size() - 1;
        if (index >= steps.size()) return {MaiErrorCode::Internal, "Unexpected model invocation"};
        return steps[index](request, sink);
    }
};

MaiModelRequest request() {
    MaiModelRequest result;
    result.model = "local:protocol-test";
    result.workingDirectory = "/synthetic/complete-history";
    result.baseInstructions = "Preserve every original system instruction.";
    result.reasoningEffort = "high";
    result.temperature = 0;
    MaiModelMessage first;
    first.content = std::string(10000, 'a') + " original-first-message";
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.reasoning = "original reasoning";
    assistant.invocations.push_back({"old-call", "file_read", R"({"path":"old.txt"})"});
    MaiModelMessage previous;
    previous.role = MaiModelRole::ToolResult;
    previous.toolCallId = "old-call";
    previous.content = "original tool result";
    MaiModelMessage last;
    last.content = "Read the next file.";
    result.messages = {first, assistant, previous, last};
    result.tools = {{"file_read", "Original detailed read description.",
                     " { \"type\": \"object\", \"properties\": {\"path\": {\"type\": "
                     "\"string\"}} } "},
                    {"current_time", "Original time description.", R"({"type":"object"})"}};
    return result;
}

void checkHistory(const MaiModelRequest& actual, const MaiModelRequest& original) {
    CHECK(actual.model == original.model);
    CHECK(actual.baseInstructions == original.baseInstructions);
    CHECK(actual.workingDirectory == original.workingDirectory);
    CHECK(actual.reasoningEffort == original.reasoningEffort);
    CHECK(actual.temperature == original.temperature);
    CHECK(actual.messages.size() == original.messages.size());
    if (actual.messages.size() != original.messages.size()) return;
    for (std::size_t index = 0; index < actual.messages.size(); ++index) {
        const auto& left = actual.messages[index];
        const auto& right = original.messages[index];
        CHECK(left.role == right.role && left.content == right.content);
        CHECK(left.reasoning == right.reasoning && left.toolCallId == right.toolCallId);
        CHECK(left.invocations.size() == right.invocations.size());
        for (std::size_t call = 0; call < left.invocations.size(); ++call) {
            CHECK(left.invocations[call].id == right.invocations[call].id);
            CHECK(left.invocations[call].name == right.invocations[call].name);
            CHECK(left.invocations[call].arguments == right.invocations[call].arguments);
        }
    }
}

Step emitCalls(std::vector<MaiToolInvocation> calls, MaiError error = {}) {
    return [calls = std::move(calls), error](const MaiModelRequest&, const MaiStreamSink& sink) {
        for (const auto& call : calls)
            if (sink.onToolCall) sink.onToolCall(call);
        return error;
    };
}

const MaiToolInvocation search{"search-1", "tool_search", R"({"names":["file_read"]})"};
const MaiToolInvocation action{"action-1", "file_read", R"({"path":"next.txt"})"};

void test_selection_preserves_original_request_and_declarations() {
    auto raw = std::make_unique<MaiScriptedLocalClient>();
    auto* observed = raw.get();
    raw->steps = {emitCalls({search}), emitCalls({action})};
    auto client = makeMaiLlamaToolDiscoveryClient(std::move(raw));
    const auto original = request();
    std::atomic<bool> cancel{false};
    std::vector<MaiToolInvocation> calls;
    MaiStreamSink sink;
    sink.onToolCall = [&](const auto& call) { calls.push_back(call); };
    CHECK(!client->stream(original, sink, cancel));
    CHECK(observed->requests.size() == 2 && calls.size() == 1);
    if (observed->requests.size() != 2) return;
    for (const auto& actual : observed->requests) checkHistory(actual, original);
    const auto& selection = observed->requests[0];
    CHECK(selection.tools.size() == 1 && selection.tools[0].name == "tool_search");
    const auto schema = nlohmann::json::parse(selection.tools[0].parametersJson);
    CHECK(schema["properties"]["names"]["items"]["enum"] ==
          nlohmann::json::array({"file_read", "current_time"}));
    const auto& task = observed->requests[1];
    CHECK(task.tools.size() == 1);
    CHECK(task.tools[0].name == original.tools[0].name);
    CHECK(task.tools[0].description == original.tools[0].description);
    CHECK(task.tools[0].parametersJson == original.tools[0].parametersJson);
    CHECK(calls[0].name == action.name && calls[0].arguments == action.arguments);
}

void test_no_tool_answer_and_empty_catalog_are_single_calls() {
    for (const bool hasTools : {false, true}) {
        auto raw = std::make_unique<MaiScriptedLocalClient>();
        auto* observed = raw.get();
        raw->steps.push_back([](const auto&, const MaiStreamSink& sink) {
            if (sink.onText) sink.onText("Provider text");
            if (sink.onReasoning) sink.onReasoning("Provider reasoning");
            return MaiError{};
        });
        auto client = makeMaiLlamaToolDiscoveryClient(std::move(raw));
        auto original = request();
        if (!hasTools) original.tools.clear();
        std::string text, reasoning;
        MaiStreamSink sink;
        sink.onText = [&](auto delta) { text += delta; };
        sink.onReasoning = [&](auto delta) { reasoning += delta; };
        std::atomic<bool> cancel{false};
        CHECK(!client->stream(original, sink, cancel));
        CHECK(observed->requests.size() == 1);
        CHECK(text == "Provider text" && reasoning == "Provider reasoning");
        checkHistory(observed->requests[0], original);
    }
}

void test_invalid_partial_mixed_and_canceled_batches_never_deliver_actions() {
    const std::vector<std::vector<Step>> scenarios = {
        {emitCalls({search, action})},
        {emitCalls({{"search", "tool_search", R"({"names":["unknown"]})"}})},
        {emitCalls({{"search", "tool_search", R"({"names":[)"}})},
        {emitCalls({{"search", "tool_search", R"({"names":["file_read"],"extra":1})"}})},
        {emitCalls({search}, {MaiErrorCode::Canceled, "Canceled incomplete discovery"})},
        {emitCalls({search}), emitCalls({action, search})},
        {emitCalls({search}), emitCalls({action, {"other", "current_time", "{}"}})},
        {emitCalls({search}), emitCalls({action, {"half", "file_read", "{"}})},
        {emitCalls({search}), emitCalls({action, action})},
        {emitCalls({search}),
         emitCalls({action}, {MaiErrorCode::Protocol, "Incomplete model response"})},
    };
    for (const auto& scenario : scenarios) {
        auto raw = std::make_unique<MaiScriptedLocalClient>();
        auto* observed = raw.get();
        raw->steps = scenario;
        auto client = makeMaiLlamaToolDiscoveryClient(std::move(raw));
        std::atomic<bool> cancel{false};
        std::size_t delivered = 0;
        MaiStreamSink sink;
        sink.onToolCall = [&](const auto&) { ++delivered; };
        CHECK(client->stream(request(), sink, cancel).hasError());
        CHECK(delivered == 0 && observed->requests.size() <= 2);
    }
    for (const bool cancelDuringTask : {false, true}) {
        auto raw = std::make_unique<MaiScriptedLocalClient>();
        std::atomic<bool> cancel{false};
        if (cancelDuringTask) raw->steps.push_back(emitCalls({search}));
        raw->steps.push_back([&](const auto&, const MaiStreamSink& sink) {
            if (sink.onToolCall) sink.onToolCall(cancelDuringTask ? action : search);
            cancel = true;
            return MaiError{};
        });
        auto client = makeMaiLlamaToolDiscoveryClient(std::move(raw));
        std::size_t delivered = 0;
        MaiStreamSink sink;
        sink.onToolCall = [&](const auto&) { ++delivered; };
        CHECK(client->stream(request(), sink, cancel).code() == MaiErrorCode::Canceled);
        CHECK(delivered == 0);
    }
}

void test_selection_is_not_retained_between_requests() {
    auto raw = std::make_unique<MaiScriptedLocalClient>();
    auto* observed = raw.get();
    raw->steps = {emitCalls({search}), emitCalls({action}), emitCalls({search}),
                  emitCalls({action})};
    auto client = makeMaiLlamaToolDiscoveryClient(std::move(raw));
    std::atomic<bool> cancel{false};
    CHECK(!client->stream(request(), {}, cancel));
    auto second = request();
    second.baseInstructions = "Different account and session instructions.";
    second.messages.back().content = "Different user request.";
    CHECK(!client->stream(second, {}, cancel));
    CHECK(observed->requests.size() == 4);
    CHECK(observed->requests[2].tools.size() == 1);
    CHECK(observed->requests[2].tools[0].name == "tool_search");
    checkHistory(observed->requests[2], second);
}
}  // namespace

int main() {
    MaiThread::setCurrentName("mai-discovery-qa");
    test_selection_preserves_original_request_and_declarations();
    test_no_tool_answer_and_empty_catalog_are_single_calls();
    test_invalid_partial_mixed_and_canceled_batches_never_deliver_actions();
    test_selection_is_not_retained_between_requests();
    std::printf("MaiLlamaToolDiscoveryClientTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
