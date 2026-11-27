// 文件作用：通过本地 HTTP 回环验证原生补丁工具的请求、续接及失败执行边界。
// 测试凭据只为占位，不访问真实供应商，也不读取用户会话。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiApplyPatchTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiResponsesClient.h"
#include "MaiThread.h"

namespace {
using Json = nlohmann::json;
int failures = 0;
int checks = 0;
#define CHECK(condition)                                        \
    do {                                                        \
        ++checks;                                               \
        if (!(condition)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #condition); \
            ++failures;                                         \
        }                                                       \
    } while (false)

const std::string patch = "*** Begin Patch\n*** Add File: fixture.txt\n+hello\n*** End Patch";
Json callItem() {
    return Json{{"type", "custom_tool_call"}, {"id", "ctc_fixture"}, {"call_id", "call_fixture"},
                {"name", "apply_patch"},      {"input", patch},      {"status", "completed"},
                {"opaque", "preserve"}};
}
std::string frame(const Json& event) {
    return "data: " + event.dump() + "\n\n";
}
std::string terminal(const Json& output, const std::string& type = "response.completed") {
    return frame(Json{{"type", type}, {"response", Json{{"output", output}}}});
}
class Server {
public:
    Server() {
        mServer.Post("/responses",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         std::lock_guard<std::mutex> lock(mMutex);
                         bodies.push_back(Json::parse(request.body));
                         response.set_content(script, "text/event-stream");
                     });
        mPort = mServer.bind_to_any_port("127.0.0.1");
        CHECK(mPort > 0);
        mWorker = std::thread([this] {
            MaiThread::setCurrentName("mai-patch-wire");
            mServer.listen_after_bind();
        });
        for (int attempt = 0; attempt < 200 && !mServer.is_running(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(mServer.is_running());
    }
    ~Server() {
        mServer.stop();
        mWorker.join();
    }
    MaiModelConfig config() {
        MaiModelConfig result;
        result.baseUrl = "http://127.0.0.1:" + std::to_string(mPort);
        result.apiKey = "loopback-placeholder";
        result.maxRetries = 0;
        result.totalTimeoutSeconds = 10;
        result.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
            proxy = {};
            return true;
        };
        return result;
    }
    std::string script = terminal(Json::array());
    std::vector<Json> bodies;

private:
    httplib::Server mServer;
    std::thread mWorker;
    std::mutex mMutex;
    int mPort = 0;
};

void testHistoricalCustomOutput() {
    Server server;
    auto client = makeMaiResponsesClient(server.config());
    MaiModelRequest request;
    request.model = "gpt-6.1-sol";
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.invocations.push_back({"call_fixture", "apply_patch", Json{{"patch", patch}}.dump()});
    assistant.continuation = MaiModelContinuationState{
        "responses.output.v1", "openai", request.model, Json::array({callItem()}).dump()};
    request.messages.push_back(assistant);
    MaiModelMessage result;
    result.role = MaiModelRole::ToolResult;
    result.toolCallId = "call_fixture";
    result.content = "Success. Updated the following files:\nA fixture.txt";
    request.messages.push_back(result);
    std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, {}, cancel));
    CHECK(server.bodies.size() == 1);
    if (server.bodies.empty()) return;
    const Json& input = server.bodies.back()["input"];
    CHECK(input.size() == 2);
    CHECK(input[0] == callItem());
    CHECK(input[1]["type"] == "custom_tool_call_output");
    CHECK(input[1]["call_id"] == "call_fixture");
    CHECK(!server.bodies.back().contains("tools"));
    request.messages[0].continuation.reset();
    CHECK(!client->stream(request, {}, cancel));
    const Json& reconstructed = server.bodies.back()["input"];
    CHECK(reconstructed.size() == 2);
    CHECK(reconstructed[0]["type"] == "custom_tool_call");
    CHECK(reconstructed[0]["input"] == patch);
    CHECK(reconstructed[1]["type"] == "custom_tool_call_output");
}

MaiModelRequest patchPrompt() {
    MaiModelRequest request;
    request.model = "gpt-6.1-sol";
    request.messages.push_back({MaiModelRole::User, "Apply the fixture patch."});
    request.tools.push_back(
        {"apply_patch", "Apply a patch.",
         R"({"type":"object","properties":{"patch":{"type":"string"}},"required":["patch"],"additionalProperties":false})",
         "start: /[\\s\\S]+/"});
    return request;
}

std::string lifecycle() {
    Json added = callItem();
    added["input"] = "";
    added["status"] = "in_progress";
    std::string result =
        frame(Json{{"type", "response.output_item.added"}, {"output_index", 0}, {"item", added}});
    for (std::size_t index = 0; index < patch.size(); index += 3)
        result += frame(Json{{"type", "response.custom_tool_call_input.delta"},
                             {"output_index", 0},
                             {"item_id", "ctc_fixture"},
                             {"delta", patch.substr(index, 3)}});
    result += frame(Json{{"type", "response.custom_tool_call_input.done"},
                         {"output_index", 0},
                         {"item_id", "ctc_fixture"},
                         {"input", patch}});
    result += frame(
        Json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", callItem()}});
    return result;
}

void testNativeSchemaAndCompleteDelivery() {
    Server server;
    auto client = makeMaiResponsesClient(server.config());
    auto request = patchPrompt();
    std::atomic<bool> cancel{false};
    for (const auto& script :
         {terminal(Json::array({callItem()})), lifecycle() + terminal(Json::array({callItem()})),
          lifecycle() +
              frame(Json{{"type", "response.completed"}, {"response", Json::object()}})}) {
        server.script = script;
        std::vector<MaiToolInvocation> calls;
        std::vector<MaiModelOutputItem> raw;
        std::optional<MaiModelContinuationState> state;
        MaiStreamSink sink;
        sink.onToolCall = [&](const MaiToolInvocation& call) {
            CHECK(state.has_value());
            calls.push_back(call);
        };
        sink.onContinuationState = [&](const MaiModelContinuationState& value) { state = value; };
        sink.onOutputItemCompleted = [&](const MaiModelOutputItem& item) { raw.push_back(item); };
        CHECK(!client->stream(request, sink, cancel));
        CHECK(calls.size() == 1);
        if (!calls.empty()) {
            CHECK(calls[0].id == "call_fixture");
            CHECK(calls[0].name == "apply_patch");
            CHECK(Json::parse(calls[0].arguments) == Json({{"patch", patch}}));
        }
        CHECK(raw.size() == 1);
        if (!raw.empty()) CHECK(Json::parse(raw[0].payload) == callItem());
        if (state) CHECK(Json::parse(state->payload) == Json::array({callItem()}));
        const auto& tool = server.bodies.back()["tools"][0];
        CHECK(tool["type"] == "custom");
        CHECK(tool["name"] == "apply_patch");
        CHECK(tool["format"] == Json({{"type", "grammar"},
                                      {"syntax", "lark"},
                                      {"definition", request.tools[0].inputGrammar}}));
        CHECK(!tool.contains("parameters"));
    }
    server.script = terminal(Json::array());
    for (const auto* model : {"glm-5.4", "deepseek-v4-pro", "anthropic/claude-opus-5.5"}) {
        request.model = model;
        CHECK(!client->stream(request, {}, cancel));
        CHECK(server.bodies.back()["tools"][0]["type"] == "function");
        CHECK(server.bodies.back()["tools"][0].contains("parameters"));
        CHECK(!server.bodies.back()["tools"][0].contains("format"));
    }
}

void testFailureFences() {
    Server server;
    auto client = makeMaiResponsesClient(server.config());
    const auto good = callItem();
    auto changed = good;
    changed["input"] = patch + "\nchanged";
    auto badInput = good;
    badInput["input"] = Json::object();
    auto badName = good;
    badName["name"] = "other_custom";
    auto badId = good;
    badId.erase("call_id");
    auto badStatus = good;
    badStatus["status"] = "in_progress";
    auto function = good;
    function["type"] = "function_call";
    function["arguments"] = Json{{"patch", patch}}.dump();
    function.erase("input");
    std::vector<std::string> scripts{
        lifecycle(),
        lifecycle() + terminal(Json::array(), "response.incomplete"),
        lifecycle() + terminal(Json::array(), "response.failed"),
        lifecycle() + terminal(Json::array()),
        lifecycle() + terminal(Json::array({changed})),
        terminal(Json::array({badInput})),
        terminal(Json::array({badName})),
        terminal(Json::array({badId})),
        terminal(Json::array({badStatus})),
        terminal(Json::array({function})),
        "data: {broken\n\n" + terminal(Json::array({good})),
        frame(Json{{"type", "response.custom_tool_call_input.delta"},
                   {"output_index", 0},
                   {"delta", "contradiction"}}) +
            terminal(Json::array({good})),
        frame(Json{
            {"type", "response.custom_tool_call_input.done"}, {"output_index", 0}, {"input", ""}}) +
            terminal(Json::array({good})),
        frame(Json{
            {"type", "response.custom_tool_call_input.delta"}, {"output_index", 0}, {"delta", 1}}) +
            terminal(Json::array({good}))};
    scripts.push_back(frame(
        Json{{"type", "response.completed"},
             {"response", Json{{"status", "in_progress"}, {"output", Json::array({good})}}}}));
    scripts.push_back(frame(Json{{"type", "response.output_item.done"}, {"item", good}}) +
                      terminal(Json::array({good})));
    scripts.push_back(
        frame(Json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", nullptr}}) +
        terminal(Json::array({good})));
    for (const auto& malformed : {badId, badStatus})
        scripts.push_back(
            frame(Json{
                {"type", "response.output_item.done"}, {"output_index", 0}, {"item", malformed}}) +
            terminal(Json::array({good})));
    auto malformedAdded = good;
    malformedAdded["input"] = "";
    malformedAdded["status"] = "in_progress";
    malformedAdded["name"] = 42;
    scripts.push_back(frame(Json::object()) + terminal(Json::array({good})));
    scripts.push_back(frame(Json{{"type", "response.output_item.added"},
                                 {"output_index", 0},
                                 {"item", malformedAdded}}) +
                      terminal(Json::array({good})));
    scripts.push_back(frame(Json{{"type", "response.custom_tool_call_input.delta"},
                                 {"output_index", 0},
                                 {"item_id", 42},
                                 {"delta", ""}}) +
                      terminal(Json::array({good})));
    for (const auto& script : scripts) {
        server.script = script;
        std::atomic<bool> cancel{false};
        int calls = 0;
        int states = 0;
        MaiStreamSink sink;
        sink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
        sink.onContinuationState = [&](const MaiModelContinuationState&) { ++states; };
        const auto error = client->stream(patchPrompt(), sink, cancel);
        CHECK(error);
        CHECK(calls == 0);
        CHECK(states == 0);
    }
    server.script = terminal(Json::array({good}));
    auto request = patchPrompt();
    request.tools[0].inputGrammar.clear();
    std::atomic<bool> cancel{false};
    int calls = 0;
    MaiStreamSink sink;
    sink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
    CHECK(client->stream(request, sink, cancel));
    CHECK(calls == 0);
    request = patchPrompt();
    int canceledStates = 0;
    sink.onContinuationState = [&](const MaiModelContinuationState&) { ++canceledStates; };
    sink.onOutputItemCompleted = [&](const MaiModelOutputItem&) { cancel = true; };
    CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::Canceled);
    CHECK(calls == 0);
    CHECK(canceledStates == 0);
    cancel = false;
    sink.onOutputItemCompleted = {};
    sink.onContinuationState = [&](const MaiModelContinuationState&) { cancel = true; };
    CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::Canceled);
    CHECK(calls == 0);
    cancel = false;
    request.tools[0].name = "unsupported_custom";
    const auto count = server.bodies.size();
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::InvalidInput);
    CHECK(server.bodies.size() == count);
    request = patchPrompt();
    for (const auto& malformed : {badId, badStatus}) {
        server.script =
            frame(Json{
                {"type", "response.output_item.done"}, {"output_index", 0}, {"item", malformed}}) +
            terminal(Json::array({good}));
        std::vector<MaiModelOutputItem> facts;
        MaiStreamSink factSink;
        factSink.onOutputItemCompleted = [&](const MaiModelOutputItem& item) {
            facts.push_back(item);
        };
        factSink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
        CHECK(client->stream(request, factSink, cancel));
        CHECK(calls == 0);
        CHECK(facts.size() == 1);
        if (!facts.empty()) CHECK(Json::parse(facts[0].payload) == malformed);
    }
}

void testFactorySpec() {
    auto tool = makeMaiApplyPatchTool();
    CHECK(tool->name() == "apply_patch");
    const auto native = tool->specForModel("gpt-6.1-sol");
    CHECK(native.name == "apply_patch");
    CHECK(!native.inputGrammar.empty());
    CHECK(native.description.find("FREEFORM") != std::string::npos);
    const auto compatible = tool->specForModel("glm-5.4");
    CHECK(compatible.name == "apply_patch");
    CHECK(compatible.inputGrammar.empty());
    CHECK(Json::parse(compatible.parametersJson)["properties"]["patch"]["type"] == "string");
}

void testDispatchAndResultRoundTrip() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("mai-patch-wire-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    Server server;
    server.script = lifecycle() + terminal(Json::array({callItem()}));
    auto client = makeMaiResponsesClient(server.config());
    auto request = patchPrompt();
    std::vector<MaiToolInvocation> calls;
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    MaiStreamSink sink;
    sink.onToolCall = [&](const MaiToolInvocation& call) { calls.push_back(call); };
    sink.onContinuationState = [&](const MaiModelContinuationState& state) {
        assistant.continuation = state;
    };
    std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, sink, cancel));
    CHECK(calls.size() == 1);
    if (!calls.empty()) {
        MaiToolContext context;
        context.root = directory.u8string();
        context.sessionId = "ses_patch_wire";
        const auto result = makeMaiApplyPatchTool()->execute(calls[0].arguments, context);
        CHECK(!result.hasError());
        std::string bytes;
        CHECK(!MaiFileSystem::readFile(
            MaiFilePath::fromUtf8((directory / "fixture.txt").u8string()), bytes));
        CHECK(bytes == "hello\n");
        assistant.invocations = calls;
        request.messages.push_back(assistant);
        MaiModelMessage output;
        output.role = MaiModelRole::ToolResult;
        output.toolCallId = calls[0].id;
        output.content = result.output();
        request.messages.push_back(output);
        request.tools.clear();
        server.script = terminal(Json::array());
        CHECK(!client->stream(request, {}, cancel));
        const auto& input = server.bodies.back()["input"];
        CHECK(input.size() == 3);
        CHECK(input[1] == callItem());
        CHECK(input[2]["type"] == "custom_tool_call_output");
        CHECK(input[2]["output"] == result.output());
    }
    std::filesystem::remove_all(directory);
}

void testCompatibleFunctionCall() {
    Server server;
    auto function = callItem();
    function["type"] = "function_call";
    function["arguments"] = Json{{"patch", patch}}.dump();
    function.erase("input");
    server.script = terminal(Json::array({function}));
    auto client = makeMaiResponsesClient(server.config());
    auto request = patchPrompt();
    request.model = "glm-5.4";
    request.tools[0] = makeMaiApplyPatchTool()->specForModel(request.model);
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    MaiStreamSink sink;
    sink.onToolCall = [&](const MaiToolInvocation& call) { assistant.invocations.push_back(call); };
    sink.onContinuationState = [&](const MaiModelContinuationState& state) {
        assistant.continuation = state;
    };
    std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, sink, cancel));
    CHECK(assistant.invocations.size() == 1);
    if (!assistant.invocations.empty())
        CHECK(assistant.invocations[0].arguments == function["arguments"]);
    request.messages.push_back(assistant);
    MaiModelMessage result;
    result.role = MaiModelRole::ToolResult;
    result.toolCallId = "call_fixture";
    result.content = "success";
    request.messages.push_back(result);
    request.tools.clear();
    server.script = terminal(Json::array());
    CHECK(!client->stream(request, {}, cancel));
    const auto& input = server.bodies.back()["input"];
    CHECK(input.size() == 3);
    CHECK(input[1] == function);
    CHECK(input[2]["type"] == "function_call_output");
}
}  // namespace

int main() {
    testHistoricalCustomOutput();
    testNativeSchemaAndCompleteDelivery();
    testFailureFences();
    testFactorySpec();
    testDispatchAndResultRoundTrip();
    testCompatibleFunctionCall();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
