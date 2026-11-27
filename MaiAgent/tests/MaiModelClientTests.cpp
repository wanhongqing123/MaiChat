// 文件作用：验证“定义模型请求、流式响应和客户端接口契约”的正确行为和边界条件。
// 验证入口：test_text_stream_one_byte_at_a_time、test_tool_call_fragments、
// test_view_image_tool_pixels_reach_next_http_request；检查实际线格式和工具图片回灌。
// LLM 客户端的测试。
//
// 默认不打真实的大模型；可显式启用无账号 Key 的 TLS 探针，服务端拒绝后不会计费。
// 这里起一个假的 Responses 服务端，
// 喂**对抗性分片**的 SSE ——把工具调用的 arguments 一个字符一个字符地切、切在 JSON 的引号中间、
// 把多个事件塞进同一个 TCP 包、再掺几个心跳注释行和畸形行。
//
// 这正是真实服务端会干的事，也是这一层唯一真正难写的地方。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiModelClient.h"
#include "MaiRoutedModelClient.h"
#include "MaiCloudModelCatalog.h"
#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiResponsesClient.h"
#include "MaiViewImageTool.h"

using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 把一段完整的 SSE 文本按给定块大小切开发出去，模拟网络分片。
struct FakeServer {
    httplib::Server server;
    std::thread th;
    int port = 0;
    std::string script;
    // 连续工具请求需要不同的响应；未指定时仍使用单次响应脚本。
    std::vector<std::string> scripts;
    std::size_t chunk = 1;
    int status = 200;
    std::string retryAfter;
    int transientFailures = 0;
    int transientStatus = 503;
    std::string transientBody = R"({"error":{"message":"temporarily unavailable"}})";
    bool stallWithoutBytes = false;
    bool sendOnlyHeartbeats = false;
    // 重复有效但不推进的协议事件，用于证明服务端状态/空 delta 不会无限续期。
    std::string repeatedEvent;
    bool keepOpenAfterDone = false;
    int stallRequests = 0;
    std::vector<std::string> delayedChunks;
    int delayedChunkMs = 0;
    std::atomic<std::size_t> delayedChunksSent{0};
    std::atomic<bool> releaseStall{false};
    std::atomic<int> requestCount{0};

    // 收到的请求体和鉴权头。测线格式要看**真正发出去的字节**，
    // 不能拿我们自己的结构体去对——那只能证明"符合我的理解"。
    std::mutex mutex;
    std::string lastBody;
    std::string lastAuthorization;
    std::vector<std::string> requestBodies;
    std::vector<int> clientPorts;
    bool fixedBody = false;

    // 本地假服务器记录真实请求字节，并按测试配置模拟断流、心跳和重试；客户端协议断言不能只对自身数据结构做比较。
    void start() {
        auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            const int requestNumber = ++requestCount;
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastBody = request.body;
                lastAuthorization = request.get_header_value("Authorization");
                requestBodies.push_back(request.body);
                clientPorts.push_back(request.remote_port);
            }
            if (requestNumber <= transientFailures) {
                response.status = transientStatus;
                if (!retryAfter.empty()) response.set_header("Retry-After", retryAfter);
                response.set_content(transientBody, "application/json");
                return;
            }
            if (status != 200) {
                response.status = status;
                if (!retryAfter.empty()) response.set_header("Retry-After", retryAfter);
                response.set_content(script, "application/json");
                return;
            }
            if (fixedBody) {
                response.set_content(script, "text/event-stream");
                return;
            }
            if (keepOpenAfterDone || requestNumber <= stallRequests) {
                auto sent = std::make_shared<bool>(false);
                response.set_chunked_content_provider(
                    "text/event-stream", [this, sent](std::size_t, httplib::DataSink& sink) {
                        if (!*sent) {
                            *sent = true;
                            return sink.write(script.data(), script.size());
                        }
                        if (releaseStall.load()) {
                            sink.done();
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;
                    });
                return;
            }
            if (!delayedChunks.empty()) {
                auto index = std::make_shared<std::size_t>(0);
                response.set_chunked_content_provider(
                    "text/event-stream", [this, index](std::size_t, httplib::DataSink& sink) {
                        if (*index >= delayedChunks.size()) {
                            sink.done();
                            return false;
                        }
                        const std::string& chunk = delayedChunks[(*index)++];
                        ++delayedChunksSent;
                        const bool written = sink.write(chunk.data(), chunk.size());
                        std::this_thread::sleep_for(std::chrono::milliseconds(delayedChunkMs));
                        return written;
                    });
                return;
            }
            if (stallWithoutBytes || sendOnlyHeartbeats || !repeatedEvent.empty()) {
                response.set_chunked_content_provider(
                    "text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
                        if (releaseStall.load()) {
                            sink.done();
                            return false;
                        }
                        if (sendOnlyHeartbeats && !sink.write(": keepalive\n\n", 13)) return false;
                        if (!repeatedEvent.empty() &&
                            !sink.write(repeatedEvent.data(), repeatedEvent.size()))
                            return false;
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        return true;
                    });
                return;
            }
            const std::size_t scriptIndex = static_cast<std::size_t>(requestNumber - 1);
            auto text = std::make_shared<std::string>(
                scripts.empty() ? script : scripts[std::min(scriptIndex, scripts.size() - 1)]);
            auto pos = std::make_shared<std::size_t>(0);
            const std::size_t step = chunk;
            response.set_chunked_content_provider(
                "text/event-stream", [text, pos, step](std::size_t, httplib::DataSink& sink) {
                    if (*pos >= text->size()) {
                        sink.done();
                        return false;
                    }
                    const std::size_t n = std::min(step, text->size() - *pos);
                    const bool ok = sink.write(text->data() + *pos, n);
                    *pos += n;
                    return ok;
                });
        };
        server.Post("/responses", handle);
        port = server.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeServer() {
        releaseStall.store(true);
        server.stop();
        if (th.joinable()) th.join();
    }

    std::string base() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

struct Collected {
    std::string text;
    std::string reasoning;
    std::vector<MaiToolInvocation> calls;
    std::vector<MaiModelTextItem> startedItems;
    std::vector<MaiModelTextItem> completedItems;
    std::vector<MaiModelUsage> usage;
    std::vector<MaiModelContinuationState> continuations;
    std::vector<std::string> deliveries;
    std::string error;
    MaiErrorCode code = MaiErrorCode::Ok;
    bool done = false;
};

Collected runAgainst(FakeServer& fake, const MaiModelRequest& request) {
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    config.wire = MaiWireApi::Responses;
    auto client = makeMaiModelClient(config);

    Collected collected;
    MaiStreamSink sink;
    sink.onUsage = [&](const MaiModelUsage& usage) { collected.usage.push_back(usage); };
    sink.onTextItemStarted = [&](const MaiModelTextItem& item) {
        collected.startedItems.push_back(item);
    };
    sink.onTextItemCompleted = [&](const MaiModelTextItem& item) {
        collected.completedItems.push_back(item);
    };
    sink.onText = [&collected](std::string_view text) { collected.text.append(text); };
    sink.onReasoning = [&collected](std::string_view text) { collected.reasoning.append(text); };
    sink.onToolCall = [&collected](const MaiToolInvocation& invocation) {
        collected.deliveries.push_back("call:" + invocation.id);
        collected.calls.push_back(invocation);
    };
    sink.onContinuationState = [&](const MaiModelContinuationState& state) {
        collected.deliveries.push_back("state");
        collected.continuations.push_back(state);
    };

    const std::atomic<bool> cancel{false};
    const MaiError error = client->stream(request, sink, cancel);
    collected.error = error.message();
    collected.code = error.code();
    collected.done = !error;
    return collected;
}

Collected run(const std::string& script, std::size_t chunk) {
    FakeServer fake;
    fake.script = script;
    fake.chunk = chunk;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    auto client = makeMaiModelClient(config);

    MaiModelRequest request;
    request.model = "glm-5.4";
    MaiModelMessage t;
    t.role = MaiModelRole::User;
    t.content = "run the tests";
    request.messages.push_back(t);

    Collected c;
    MaiStreamSink h;
    h.onText = [&c](std::string_view s) { c.text.append(s); };
    h.onReasoning = [&c](std::string_view s) { c.reasoning.append(s); };
    h.onToolCall = [&c](const MaiToolInvocation& t) { c.calls.push_back(t); };

    const std::atomic<bool> cancel{false};
    // 错误现在是返回值而不是回调：调用方不会漏接，而且能拿到错误码来区分"网断了"和"用户按了停"。
    const MaiError err = client->stream(request, h, cancel);
    c.error = err.message();
    c.code = err.code();
    c.done = !err;
    return c;
}

// ── 用 JSON 库构造 SSE 帧 ───────────────────────────────────────
// 不手写多层转义。arguments 本身是一段 JSON 文本，外面再套一层 JSON，
// 手数反斜杠几乎必错——第一版就多写了一层，测试红了，查了半天才发现是测试自己写错，
// 不是被测代码的问题。转义交给库最省事。
std::string sse(const json& j) {
    return "data: " + j.dump() + "\n\n";
}

// 旧实现缺少新字段时应产生 CHECK 失败，不能由 const operator[] 的缺失键断言中止整个 RED。
json wireValue(const json& document, const char* path) {
    const json::json_pointer pointer(path);
    return document.contains(pointer) ? document.at(pointer) : json{};
}

// 直接构造 Responses 原生事件；调用身份、参数增量和完成快照各有独立的协议字段。
json function_item(const char* callId, const char* name, const char* arguments) {
    return json{{"type", "function_call"},
                {"id", std::string("fc_") + callId},
                {"call_id", callId},
                {"name", name},
                {"arguments", arguments}};
}

std::string function_added(int index, const char* callId, const char* name) {
    return sse(json{{"type", "response.output_item.added"},
                    {"output_index", index},
                    {"item", function_item(callId, name, "")}});
}

std::string function_arguments_delta(int index, const char* arguments) {
    return sse(json{{"type", "response.function_call_arguments.delta"},
                    {"output_index", index},
                    {"delta", arguments}});
}

std::string function_done(int index, const char* callId, const char* name, const char* arguments) {
    return sse(json{{"type", "response.output_item.done"},
                    {"output_index", index},
                    {"item", function_item(callId, name, arguments)}});
}

std::string response_text_delta(const char* content) {
    return sse(json{{"type", "response.output_text.delta"},
                    {"output_index", 0},
                    {"content_index", 0},
                    {"delta", content}});
}

const char* kCompleted = "data: {\"type\":\"response.completed\"}\n\n";

// 这三段合起来是 "你好，世界 🙂"。
//
// 写成 \u 转义而不是直接的汉字，是因为规范要求代码里除注释外不出现中文；
// 但这个用例**测的就是非 ASCII**——多字节字符被切在 chunk 边界上还能不能拼回来，
// 所以字节本身一个都不能改。
//
//   \u4f60\u597d       你好
//   \uff0c\u4e16\u754c  ，世界
//   \U0001F642        🙂
const char* kGreetingPart1 = "\u4f60\u597d";
const char* kGreetingPart2 = "\uff0c\u4e16\u754c";
const char* kGreetingPart3 = " \U0001F642";

std::string greeting() {
    return std::string(kGreetingPart1) + kGreetingPart2 + kGreetingPart3;
}

std::string text_script() {
    return std::string(": ping\n\n") + response_text_delta(kGreetingPart1) +
           response_text_delta(kGreetingPart2) + response_text_delta(kGreetingPart3) + kCompleted;
}

// 工具调用：arguments 切成三段，切点故意落在 JSON 的引号和冒号中间。
// 完整 item.done 快照也必须与这些增量保持同一个调用身份，不能重复交付。
std::string tool_script() {
    return function_added(0, "call_abc", "bash") + function_arguments_delta(0, "{\"comm") +
           function_arguments_delta(0, "and\":\"npm ") + function_arguments_delta(0, "test\"}") +
           function_done(0, "call_abc", "bash", R"({"command":"npm test"})") + kCompleted;
}

// 两个并行调用，index 交错到达：先来 1 的参数，再来 0 的。
std::string two_tools_script() {
    return function_added(0, "c0", "read") + function_added(1, "c1", "grep") +
           function_arguments_delta(1, "{\"q\":1}") + function_arguments_delta(0, "{\"p\":2}") +
           function_done(1, "c1", "grep", R"({"q":1})") +
           function_done(0, "c0", "read", R"({"p":2})") + kCompleted;
}

// ── 用例 ────────────────────────────────────────────────────────

void test_text_stream_one_byte_at_a_time() {
    const auto c = run(text_script(), 1);  // 最恶劣的分片
    CHECK(c.error.empty());
    CHECK(c.done);
    CHECK(c.text == greeting());
    CHECK(c.calls.empty());
}

void test_text_stream_all_at_once() {
    const auto c = run(text_script(), 100000);  // 另一个极端：全挤一个包
    CHECK(c.error.empty());
    CHECK(c.text == greeting());
}

void test_tool_call_fragments() {
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{100000}}) {
        const auto c = run(tool_script(), chunk);
        CHECK(c.error.empty());
        CHECK(c.done);
        CHECK(c.calls.size() == 1);
        if (c.calls.size() == 1) {
            CHECK(c.calls[0].id == "call_abc");
            CHECK(c.calls[0].name == "bash");
            if (c.calls[0].arguments != "{\"command\":\"npm test\"}")
                std::printf("  chunk=%zu actual arguments = %s\n", chunk,
                            c.calls[0].arguments.c_str());
            CHECK(c.calls[0].arguments == "{\"command\":\"npm test\"}");
            // 拼出来的必须是合法 JSON——这才是工具实现拿得到的东西
            CHECK(!json::parse(c.calls[0].arguments, nullptr, false).is_discarded());
        }
    }
}

// 完整 item 快照可以补回参数，因此仅检查最终 arguments 看不出 delta 解析是否失效。
// 把增量间隔拉到超过总进度期限，证明参数片段本身推进期限，并且终态前不交付工具。
void test_function_argument_deltas_extend_progress_without_early_delivery() {
    FakeServer fake;
    fake.delayedChunks = {function_added(0, "call_progress", "bash"),
                          function_arguments_delta(0, "{\"comm"),
                          function_arguments_delta(0, "and\":\"npm "),
                          function_arguments_delta(0, "test\"}"),
                          function_done(0, "call_progress", "bash", R"({"command":"npm test"})"),
                          kCompleted};
    fake.delayedChunkMs = 700;
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.inactivityTimeoutSeconds = 2;
    config.progressTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 8;
    config.maxRetries = 0;
    MaiModelRequest request;
    request.model = "test-model";
    std::vector<MaiToolInvocation> calls;
    MaiStreamSink sink;
    sink.onToolCall = [&](const MaiToolInvocation& invocation) {
        CHECK(fake.delayedChunksSent.load() == fake.delayedChunks.size());
        calls.push_back(invocation);
    };
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    CHECK(!makeMaiModelClient(config)->stream(request, sink, cancel));
    CHECK(std::chrono::steady_clock::now() - started > std::chrono::seconds(2));
    CHECK(fake.requestCount.load() == 1);
    CHECK(calls.size() == 1);
    if (calls.size() == 1) {
        CHECK(calls[0].id == "call_progress");
        CHECK(calls[0].name == "bash");
        CHECK(calls[0].arguments == R"({"command":"npm test"})");
    }
}

void test_two_tool_calls_interleaved() {
    const auto c = run(two_tools_script(), 3);
    CHECK(c.error.empty());
    CHECK(c.calls.size() == 2);
    if (c.calls.size() == 2) {
        // 必须按 index 排序，不是按到达顺序
        CHECK(c.calls[0].id == "c0");
        CHECK(c.calls[0].name == "read");
        CHECK(c.calls[0].arguments == "{\"p\":2}");
        CHECK(c.calls[1].id == "c1");
        CHECK(c.calls[1].name == "grep");
        CHECK(c.calls[1].arguments == "{\"q\":1}");
    }
}

void test_malformed_lines_are_ignored() {
    const std::string script = std::string(": heartbeat\n\n") + "data: not json at all\n\n" +
                               "data: {\"type\":\"unknown.event\"}\n\n" +
                               response_text_delta("ok") + "garbage line with no prefix\n\n" +
                               kCompleted;
    const auto c = run(script, 5);
    CHECK(c.error.empty());  // 畸形行不该升级成错误
    CHECK(c.text == "ok");   // 正常那条仍要收到
    CHECK(c.done);
}

void test_server_error_inside_stream() {
    // HTTP 200 也能携带 Responses 的明确失败事件，必须保留供应商错误。
    const auto c =
        run(sse(json{{"type", "response.failed"},
                     {"response",
                      {{"error", {{"code", "quota_exhausted"}, {"message", "quota exhausted"}}}}}}),
            4);
    CHECK(c.error == "quota exhausted");
    CHECK(c.code == MaiErrorCode::Protocol);
    CHECK(!c.done);
}

void test_reasoning_delta() {
    const auto c = run(sse(json{{"type", "response.reasoning_text.delta"},
                                {"output_index", 1},
                                {"content_index", 0},
                                {"delta", "let me think"}}) +
                           response_text_delta("the answer") + kCompleted,
                       2);
    CHECK(c.reasoning == "let me think");
    CHECK(c.text == "the answer");
}

void test_http_error_preserves_provider_message() {
    FakeServer fake;
    fake.status = 429;
    fake.script = json{{"error", {{"message", "quota exhausted"}, {"code", "usage_limit"}}}}.dump();
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.4";
    const Collected result = runAgainst(fake, request);
    CHECK(result.code == MaiErrorCode::RateLimited);
    CHECK(result.error.find("HTTP 429") != std::string::npos);
    CHECK(result.error.find("quota exhausted") != std::string::npos);
    CHECK(result.error.find("usage_limit") != std::string::npos);
}

// 合成限额 metadata 用于诊断，不代表用户余额；只有已知来源/原因组合改变错误分类。
void test_openrouter_402_classifies_in_flight_budget_without_retry() {
    FakeServer fake;
    fake.status = 402;
    fake.retryAfter = "7";
    const std::string providerMessage = "Synthetic provider reservation rejection.";
    fake.script = json{{"error",
                        {{"code", 402},
                         {"message", providerMessage},
                         {"metadata",
                          {{"reason", "in_flight_budget_exhausted"},
                           {"limit_source", "openrouter_in_flight_budget"},
                           {"remedy_hint", "synthetic-metadata-secret\nInjected"},
                           {"debug", "synthetic-debug-secret"}}}}}}
                      .dump();
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    const Collected answer = runAgainst(fake, request);
    CHECK(answer.code == MaiErrorCode::RateLimited);
    CHECK(answer.error.find("HTTP 402") != std::string::npos);
    CHECK(answer.error.find(providerMessage) != std::string::npos);
    CHECK(answer.error.find("limit_source=openrouter_in_flight_budget") != std::string::npos);
    CHECK(answer.error.find("reason=in_flight_budget_exhausted") != std::string::npos);
    CHECK(answer.error.find("retry_after_seconds=7") != std::string::npos);
    CHECK(answer.error.find("synthetic-metadata-secret") == std::string::npos);
    CHECK(answer.error.find("synthetic-debug-secret") == std::string::npos);
    CHECK(answer.error.find('\n') == std::string::npos);
    CHECK(answer.calls.empty() && answer.continuations.empty());
    CHECK(fake.requestCount.load() == 1);
}

void test_openrouter_402_limits_do_not_infer_missing_or_foreign_metadata() {
    struct LimitCase {
        const char* model;
        json metadata;
        bool singleRequest;
    };
    const LimitCase cases[] = {
        {"anthropic/claude-fable-5.2",
         {{"reason", "weight_exceeds_budget"},
          {"limit_source", "openrouter_credits"},
          {"debug", "synthetic-metadata-secret"}},
         true},
        {"anthropic/claude-fable-5.2", nullptr, false},
        {"anthropic/claude-fable-5.2",
         {{"reason", "synthetic-unknown-reason\nInjected"},
          {"limit_source", "synthetic-source-secret"}},
         false},
        {"anthropic/claude-fable-5.2",
         {{"reason", "in_flight_budget_exhausted"}, {"limit_source", "openrouter_credits"}},
         false},
        {"glm-5.4",
         {{"reason", "in_flight_budget_exhausted"},
          {"limit_source", "openrouter_in_flight_budget"}},
         false},
        {"deepseek-flash",
         {{"reason", "weight_exceeds_budget"}, {"limit_source", "openrouter_credits"}},
         false},
    };
    const std::string providerMessage = "Synthetic provider reservation rejection.";
    for (const auto& item : cases) {
        FakeServer fake;
        fake.status = 402;
        fake.retryAfter = "7";
        json error{{"code", 402}, {"message", providerMessage}};
        if (!item.metadata.is_null()) error["metadata"] = item.metadata;
        fake.script = json{{"error", error}}.dump();
        fake.start();
        MaiModelRequest request;
        request.model = item.model;
        const Collected answer = runAgainst(fake, request);
        CHECK(answer.code == MaiErrorCode::Protocol);
        CHECK(answer.error.find(providerMessage) != std::string::npos);
        if (item.singleRequest) {
            CHECK(answer.error.find("category=single_request_budget_exceeded") !=
                  std::string::npos);
            CHECK(answer.error.find("reason=weight_exceeds_budget") != std::string::npos);
            CHECK(answer.error.find("limit_source=openrouter_credits") != std::string::npos);
        } else {
            CHECK(answer.error == "HTTP 402: " + providerMessage);
        }
        CHECK(answer.error.find("retry_after_seconds=") == std::string::npos);
        CHECK(answer.error.find("synthetic-metadata-secret") == std::string::npos);
        CHECK(answer.error.find("synthetic-source-secret") == std::string::npos);
        CHECK(answer.error.find("synthetic-unknown-reason") == std::string::npos);
        CHECK(answer.error.find('\n') == std::string::npos);
        CHECK(fake.requestCount.load() == 1);
    }
}

// 只接受有界非负整数秒；HTTP-date 本轮不解析，任意原头值不得进入错误字符串。
void test_openrouter_402_retry_after_only_accepts_bounded_numeric_seconds() {
    struct RetryCase {
        const char* header;
        const char* expected;
    };
    const RetryCase cases[] = {
        {"0", "retry_after_seconds=0"},
        {" 0007 ", "retry_after_seconds=7"},
        {"86400", "retry_after_seconds=86400"},
        {"86401", nullptr},
        {"999999999999999999999999999999", nullptr},
        {"-1", nullptr},
        {"+7", nullptr},
        {"7.5", nullptr},
        {"7e1", nullptr},
        {"synthetic-header-secret", nullptr},
        {"Wed, 21 Oct 2015 07:28:00 GMT", nullptr},
        {"", nullptr},
    };
    for (const auto& item : cases) {
        FakeServer fake;
        fake.status = 402;
        fake.retryAfter = item.header;
        fake.script = json{{"error",
                            {{"code", 402},
                             {"message", "Synthetic provider reservation rejection."},
                             {"metadata",
                              {{"reason", "in_flight_budget_exhausted"},
                               {"limit_source", "openrouter_in_flight_budget"}}}}}}
                          .dump();
        fake.start();
        MaiModelRequest request;
        request.model = "anthropic/claude-fable-5.2";
        const Collected answer = runAgainst(fake, request);
        CHECK(answer.code == MaiErrorCode::RateLimited);
        if (item.expected)
            CHECK(answer.error.find(item.expected) != std::string::npos);
        else
            CHECK(answer.error.find("retry_after_seconds=") == std::string::npos);
        CHECK(answer.error.find("synthetic-header-secret") == std::string::npos);
        CHECK(answer.error.find("Wed, 21 Oct") == std::string::npos);
        CHECK(fake.requestCount.load() == 1);
    }
}

// 完整 402 头之后的 RST 仍须保留已知状态和有界等待头；截断 metadata 不能推断预算来源。
// 不能把接收失败降级成纯 Network、成功回复或自动重试，也不能回显任意头/半截正文。
void test_openrouter_402_truncated_reset_response_is_not_retried() {
    class ResetServer final : public httplib::Server {
    public:
        std::atomic<int> requests{0};
        std::atomic<int> resets{0};
        std::atomic<int> socketFailures{0};
        std::thread listener;
        int port = 0;
        std::string retryAfter;

        void start() {
            port = bind_to_any_port("127.0.0.1");
            listener = std::thread([this] { listen_after_bind(); });
            for (int attempt = 0; attempt < 200 && !is_running(); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ~ResetServer() {
            stop();
            if (listener.joinable()) listener.join();
        }

    private:
        bool process_and_close_socket(socket_t socket) override {
            // 先读完小 POST，避免客户端仍在上传时被重置而误测为 SEND_ERROR。
            std::string request;
            bool requestComplete = false;
            while (request.size() < 16384) {
                if (httplib::detail::select_read(socket, 2, 0) <= 0) break;
                char buffer[2048];
                const auto received =
                    httplib::detail::read_socket(socket, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                request.append(buffer, static_cast<std::size_t>(received));
                const auto headerEnd = request.find("\r\n\r\n");
                if (headerEnd == std::string::npos) continue;
                std::string headers = request.substr(0, headerEnd);
                std::transform(
                    headers.begin(), headers.end(), headers.begin(),
                    [](unsigned char character) { return char(std::tolower(character)); });
                const auto field = headers.find("\r\ncontent-length:");
                if (field == std::string::npos) break;
                const char* value = headers.c_str() + field + 17;
                char* end = nullptr;
                const auto contentLength = std::strtoull(value, &end, 10);
                if (end == value || contentLength > 16384) break;
                if (request.size() >= headerEnd + 4 + contentLength) {
                    requestComplete = true;
                    break;
                }
            }
            if (!requestComplete) {
                ++socketFailures;
                httplib::detail::close_socket(socket);
                return false;
            }
            ++requests;
            std::string response =
                "HTTP/1.1 402 Payment Required\r\nContent-Type: application/json\r\n"
                "Content-Length: 4096\r\nConnection: close\r\n";
            if (!retryAfter.empty()) response += "Retry-After: " + retryAfter + "\r\n";
            // 即使字段文字恰好匹配已知预算类型，不完整 JSON 也不能用字符串搜索补出分类。
            response +=
                "\r\n{\"error\":{\"code\":402,\"message\":\"Synthetic truncated error\","
                "\"metadata\":{\"reason\":\"in_flight_budget_exhausted\","
                "\"limit_source\":\"openrouter_in_flight_budget\","
                "\"debug\":\"synthetic-truncated-body-secret\"";
            std::size_t sent = 0;
            while (sent < response.size()) {
                const auto written = httplib::detail::send_socket(socket, response.data() + sent,
                                                                  response.size() - sent, 0);
                if (written <= 0) break;
                sent += static_cast<std::size_t>(written);
            }
            // 给 curl 一次接收完整状态行/响应头的机会，再强制 RST，不能用普通 FIN 假冒接收错误。
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            struct linger reset = {1, 0};
            if (sent == response.size() &&
                setsockopt(socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&reset),
                           static_cast<int>(sizeof(reset))) == 0)
                ++resets;
            else
                ++socketFailures;
            httplib::detail::close_socket(socket);
            return false;
        }
    };
    struct RetryCase {
        const char* header;
        const char* expected;
    };
    for (const RetryCase item :
         {RetryCase{"7", "retry_after_seconds=7"}, RetryCase{"86400", "retry_after_seconds=86400"},
          RetryCase{"86401", nullptr}, RetryCase{"synthetic-reset-header-secret", nullptr}}) {
        ResetServer server;
        server.retryAfter = item.header;
        server.start();
        CHECK(server.port > 0);
        if (server.port <= 0) return;
        MaiModelConfig config;
        config.baseUrl = "http://127.0.0.1:" + std::to_string(server.port);
        config.wire = MaiWireApi::Responses;
        config.apiKey = "synthetic-reset-key";
        config.totalTimeoutSeconds = 5;
        config.maxRetries = 2;
        config.retryInitialDelayMs = 1;
        config.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
            proxy.url.clear();
            return true;
        };
        MaiModelRequest request;
        request.model = "anthropic/claude-fable-5.2";
        unsigned textEvents = 0, reasoningEvents = 0, toolEvents = 0, continuationEvents = 0;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view) { ++textEvents; };
        sink.onReasoning = [&](std::string_view) { ++reasoningEvents; };
        sink.onToolCall = [&](const MaiToolInvocation&) { ++toolEvents; };
        sink.onContinuationState = [&](const MaiModelContinuationState&) { ++continuationEvents; };
        const std::atomic<bool> cancel{false};
        const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
        CHECK(error);
        // 元数据不可解析时仍按未知的 HTTP 拒绝处理，不能标成已知 in-flight/RateLimited。
        CHECK(error.code() == MaiErrorCode::Protocol);
        CHECK(error.message().rfind("HTTP 402", 0) == 0);
        CHECK(error.message().find("reason=") == std::string::npos);
        CHECK(error.message().find("limit_source=") == std::string::npos);
        CHECK(error.message().find("category=") == std::string::npos);
        if (item.expected) {
            const auto start = error.message().find("retry_after_seconds=");
            CHECK(start != std::string::npos);
            if (start != std::string::npos) {
                const auto end = error.message().find_first_of(" \t\r\n])};,", start);
                const auto field = error.message().substr(
                    start, end == std::string::npos ? std::string::npos : end - start);
                CHECK(field == item.expected);
            }
        } else {
            CHECK(error.message().find("retry_after_seconds=") == std::string::npos);
        }
        CHECK(error.message().find("synthetic-reset-header-secret") == std::string::npos);
        CHECK(error.message().find("synthetic-truncated-body-secret") == std::string::npos);
        CHECK(error.message().find('\n') == std::string::npos);
        CHECK(textEvents == 0 && reasoningEvents == 0 && toolEvents == 0 &&
              continuationEvents == 0);
        CHECK(server.socketFailures.load() == 0);
        CHECK(server.resets.load() >= 1);
        CHECK(server.requests.load() == 1);
    }
}

void test_output_token_limit_is_not_reported_as_success() {
    const Collected result =
        run(sse(json{{"type", "response.incomplete"},
                     {"response", {{"incomplete_details", {{"reason", "max_output_tokens"}}}}}}),
            3);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error.find("max_output_tokens") != std::string::npos);
}

void test_final_unterminated_sse_line_is_processed() {
    const json event{{"type", "response.failed"},
                     {"response", {{"error", {{"message", "last line error"}}}}}};
    const Collected result = run("data: " + event.dump(), 2);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error == "last line error");
}

void test_transient_http_failure_retries_before_streaming() {
    FakeServer fake;
    fake.transientFailures = 1;
    fake.script = response_text_delta("recovered") + kCompleted;
    fake.chunk = 4;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.4";
    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    CHECK(result.text == "recovered");
    CHECK(fake.requestCount.load() == 2);
}

void test_responses_item_done_requires_terminal_before_reporting_success() {
    FakeServer fake;
    const json item{{"type", "message"},
                    {"id", "msg_partial"},
                    {"role", "assistant"},
                    {"content", json::array({json{{"type", "output_text"}, {"text", "partial"}}})}};
    fake.script =
        response_text_delta("partial") +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", item}});
    fake.start();
    MaiModelRequest request;
    request.model = "test-model";
    const Collected result = runAgainst(fake, request);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error.find("without a terminal event") != std::string::npos);
    CHECK(result.text == "partial");
    CHECK(result.calls.empty() && result.continuations.empty());
    CHECK(fake.requestCount.load() == 1);
}

void test_responses_does_not_release_completed_tools_without_terminal() {
    FakeServer fake;
    fake.script = function_added(0, "call_abc", "read") +
                  function_done(0, "call_abc", "read", R"({"path":"a.txt"})");
    fake.start();
    MaiModelRequest request;
    request.model = "test-model";
    const Collected result = runAgainst(fake, request);
    CHECK(result.code == MaiErrorCode::Protocol);
    CHECK(result.error.find("without a terminal event") != std::string::npos);
    CHECK(result.calls.empty() && result.continuations.empty());
    CHECK(fake.requestCount.load() == 1);
}

void test_heartbeats_do_not_extend_model_progress_deadline() {
    FakeServer fake;
    fake.sendOnlyHeartbeats = true;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.inactivityTimeoutSeconds = 1;
    config.progressTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 6;
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    const MaiError result = client->stream(request, sink, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.code() == MaiErrorCode::Network);
    CHECK(result.message().find("made no progress") != std::string::npos);
    CHECK(result.message().find("Reduce image size") == std::string::npos);
    CHECK(elapsed >= std::chrono::milliseconds(1500));
    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(fake.requestCount.load() == 1);
}

std::string response_event(const nlohmann::json& event) {
    return "data: " + event.dump() + "\n\n";
}

void test_protocol_liveness_and_progress_are_distinct() {
    // 连接确有保活，等待超过短传输期限仍能收到正文；不是把有效请求按无正文直接杀掉。
    {
        FakeServer fake;
        fake.delayedChunks = {": keepalive\n\n",
                              ": keepalive\n\n",
                              ": keepalive\n\n",
                              ": keepalive\n\n",
                              response_text_delta("ready"),
                              kCompleted};
        fake.delayedChunkMs = 450;
        fake.start();
        MaiModelConfig config;
        config.baseUrl = fake.base();
        config.wire = MaiWireApi::Responses;
        config.inactivityTimeoutSeconds = 1;
        config.progressTimeoutSeconds = 4;
        config.totalTimeoutSeconds = 7;
        config.maxRetries = 0;
        auto client = makeMaiModelClient(config);
        MaiModelRequest request;
        request.model = "test-model";
        MaiStreamSink sink;
        std::string output;
        sink.onText = [&](std::string_view text) { output.append(text); };
        const std::atomic<bool> cancel{false};
        CHECK(!client->stream(request, sink, cancel));
        CHECK(output == "ready");
    }
    // 同一状态重复只说明链路活跃；无实质推进仍超时，并遵守显式的一次恢复预算。
    FakeServer fake;
    fake.repeatedEvent =
        response_event({{"type", "response.in_progress"}, {"output_index", nullptr}}) +
        response_event(
            {{"type", "response.content_part.added"}, {"output_index", 0}, {"content_index", 0}}) +
        response_event({{"type", "response.output_text.delta"}, {"delta", ""}});
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.wire = MaiWireApi::Responses;
    config.inactivityTimeoutSeconds = 1;
    config.progressTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 6;
    config.maxRetries = 1;
    config.retryInitialDelayMs = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const auto result = client->stream(request, sink, cancel);
    CHECK(result.code() == MaiErrorCode::Network);
    CHECK(result.message().find("made no progress") != std::string::npos);
    CHECK(fake.requestCount.load() == 2);
}

void test_responses_lifecycle_advances_without_synthetic_output() {
    FakeServer fake;
    fake.delayedChunks = {
        response_event({{"type", "response.created"}}),
        response_event({{"type", "response.in_progress"}}),
        response_event({{"type", "response.output_item.added"},
                        {"output_index", 0},
                        {"item", {{"type", "message"}}}}),
        response_event(
            {{"type", "response.content_part.added"}, {"output_index", 0}, {"content_index", 0}}),
        response_event(
            {{"type", "response.content_part.done"}, {"output_index", 0}, {"content_index", 0}}),
        response_event({{"type", "response.output_text.delta"}, {"delta", "answer"}}),
        response_event({{"type", "response.completed"}})};
    fake.delayedChunkMs = 550;
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.wire = MaiWireApi::Responses;
    config.inactivityTimeoutSeconds = 2;
    config.progressTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 7;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    std::string output, reasoning;
    sink.onText = [&](std::string_view text) { output.append(text); };
    sink.onReasoning = [&](std::string_view text) { reasoning.append(text); };
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    CHECK(!client->stream(request, sink, cancel));
    CHECK(output == "answer" && reasoning.empty());
    CHECK(std::chrono::steady_clock::now() - started > std::chrono::seconds(2));
}

void test_silent_transport_still_times_out() {
    FakeServer fake;
    fake.stallWithoutBytes = true;
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.wire = MaiWireApi::Responses;
    config.inactivityTimeoutSeconds = 1;
    config.progressTimeoutSeconds = 5;
    config.totalTimeoutSeconds = 7;
    // 单次超时之外最多再连一次，不能把无输出超时漏在恢复分支之外。
    config.maxRetries = 1;
    config.retryInitialDelayMs = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const auto result = client->stream(request, sink, cancel);
    CHECK(result.code() == MaiErrorCode::Network);
    CHECK(result.message().find("transport inactive") != std::string::npos);
    CHECK(fake.requestCount.load() == 2);
}

void test_real_model_progress_extends_idle_deadline() {
    FakeServer fake;
    fake.delayedChunks = {response_text_delta("first"), response_text_delta("second"), kCompleted};
    fake.delayedChunkMs = 1100;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.inactivityTimeoutSeconds = 2;
    config.progressTimeoutSeconds = 2;
    config.totalTimeoutSeconds = 6;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    std::string output;
    MaiStreamSink sink;
    sink.onText = [&output](std::string_view text) { output.append(text); };
    const std::atomic<bool> cancel{false};
    const MaiError result = client->stream(request, sink, cancel);
    CHECK(!result);
    CHECK(output == "firstsecond");
}

void test_completed_ends_stream_without_waiting_for_socket_close() {
    FakeServer fake;
    fake.keepOpenAfterDone = true;
    fake.script = response_text_delta("complete") + kCompleted;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.totalTimeoutSeconds = 3;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    std::string output;
    MaiStreamSink sink;
    sink.onText = [&output](std::string_view text) { output.append(text); };
    const std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    const MaiError result = client->stream(request, sink, cancel);
    CHECK(!result);
    CHECK(output == "complete");
    CHECK(fake.requestCount.load() == 1);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void test_cancel_interrupts_silent_model_stream() {
    FakeServer fake;
    fake.stallWithoutBytes = true;
    fake.start();

    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.totalTimeoutSeconds = 4;
    config.inactivityTimeoutSeconds = 10;
    config.maxRetries = 0;
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    std::atomic<bool> cancel{false};
    MaiError result;
    std::thread worker([&] { result = client->stream(request, sink, cancel); });
    for (int i = 0; i < 200 && fake.requestCount.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto canceledAt = std::chrono::steady_clock::now();
    cancel.store(true);
    worker.join();
    CHECK(result.code() == MaiErrorCode::Canceled);
    CHECK(fake.requestCount.load() == 1);
    CHECK(std::chrono::steady_clock::now() - canceledAt < std::chrono::seconds(2));
}

// ── 线格式 ──────────────────────────────────────────────────────
//
// 这一组断言的是**真正发到 socket 上的 JSON**，对照 OpenAI Responses 的规范，
// 不是对照我们自己的结构体。
//
// 函数调用和结果必须通过协议规定的 call_id 关联；不能用内部结构体字段名自证输出。
void test_wire_shape_of_request() {
    FakeServer fake;
    fake.script = std::string(": ping\n\n") + kCompleted;
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-4.6";
    request.baseInstructions = "Return valid Markdown.";

    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "read a.txt";
    request.messages.push_back(user);

    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.invocations.push_back(MaiToolInvocation{"call_1", "read", R"({"path":"a.txt"})"});
    request.messages.push_back(assistant);

    MaiModelMessage toolResult;
    toolResult.role = MaiModelRole::ToolResult;
    toolResult.toolCallId = "call_1";
    toolResult.content = "file body";
    request.messages.push_back(toolResult);

    MaiToolSpec spec;
    spec.name = "read";
    spec.description = "read a file";
    spec.parametersJson = R"({"type":"object","properties":{}})";
    request.tools.push_back(spec);

    runAgainst(fake, request);

    std::string body;
    std::string authorization;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
        authorization = fake.lastAuthorization;
    }
    CHECK(authorization == "Bearer test-key");

    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (sent.is_discarded()) return;

    CHECK(sent.value("model", "") == "glm-4.6");
    CHECK(sent.value("stream", false) == true);  // 不开流就收不到增量
    CHECK(sent["instructions"] == request.baseInstructions);
    CHECK(!sent.contains("messages"));
    CHECK(sent["input"].size() == 3);
    CHECK(sent["input"][0]["role"] == "user");
    CHECK(sent["input"][0]["content"] == "read a.txt");

    // 函数调用和结果是独立 input item，通过 call_id 关联，参数保持 JSON 字符串。
    const json& call = sent["input"][1];
    CHECK(call["type"] == "function_call");
    CHECK(call["call_id"] == "call_1");
    CHECK(call["name"] == "read");
    CHECK(call["arguments"].is_string());
    CHECK(call["arguments"] == R"({"path":"a.txt"})");
    const json& result = sent["input"][2];
    CHECK(result["type"] == "function_call_output");
    CHECK(result["call_id"] == "call_1");
    CHECK(result["output"] == "file body");

    // Responses 工具声明直接包含 name / description / parameters。
    CHECK(sent["tools"].size() == 1);
    if (sent["tools"].size() == 1) {
        CHECK(sent["tools"][0]["type"] == "function");
        CHECK(sent["tools"][0]["name"] == "read");
        CHECK(sent["tools"][0]["description"] == "read a file");
        CHECK(sent["tools"][0]["parameters"].is_object());
        CHECK(!sent["tools"][0].contains("function"));
    }
}

void test_no_tools_field_when_empty() {
    FakeServer fake;
    fake.script = std::string(": ping\n\n") + kCompleted;
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.4";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "chat";
    request.messages.push_back(user);

    runAgainst(fake, request);

    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    // 没工具就不带 tools 字段，而不是带一个空数组——有的服务端见到空数组会报错。
    CHECK(!sent.is_discarded() && !sent.contains("tools"));
}

void test_user_image_is_sent_as_multimodal_content() {
    FakeServer fake;
    fake.script = response_text_delta("ok") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8("mai-model-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("photo.png"));
    CHECK(!MaiFileSystem::writeFile(image, std::string("\x89PNG", 4)));

    MaiModelRequest request;
    request.model = "glm-5.4";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({"photo.png", "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["input"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 2);
        if (content.size() == 2) {
            CHECK(content[0]["type"] == "input_text");
            CHECK(content[0]["text"] == "inspect");
            CHECK(content[1]["type"] == "input_image");
            CHECK(content[1]["image_url"] == "data:image/png;base64,iVBORw==");
        }
    }
    MaiFileSystem::removeRecursively(root);
}

// 按实际 HTTP 数据 URL 解码，和独立构造的有效 PNG 比较，避免只验证路径或 MIME 前缀。
std::string decodeBase64Image(const std::string& encoded) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    unsigned int buffered = 0;
    int bits = 0;
    for (const char character : encoded) {
        if (character == '=') break;
        const std::size_t digit = alphabet.find(character);
        CHECK(digit != std::string::npos);
        if (digit == std::string::npos) return {};
        buffered = (buffered << 6) | static_cast<unsigned int>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffered >> bits) & 0xff));
        }
    }
    return decoded;
}

// 真实 view_image、Agent 工具循环和线格式客户端共同走两次本地 HTTP 请求。
// 第一轮没有附图，第二轮必须包含工具实际读取的 PNG 像素，而非只有成功回执或文件路径。
void test_view_image_tool_pixels_reach_next_http_request() {
    // 有效的 1x1 RGBA PNG，含完整 IHDR/IDAT/IEND 和 CRC；不复用被测编码器构造期望值。
    const char pngBytes[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
        "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        "\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\x60\x68\xf8\xff\x1f"
        "\x00\x04\x82\x02\x7f\xe7\xd8\x5a\x6c\x00\x00\x00\x00\x49\x45\x4e"
        "\x44\xae\x42\x60\x82";
    const std::string png(pngBytes, sizeof(pngBytes) - 1);
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-tool-image-wire_")));
    const MaiError directoryError = MaiFileSystem::createDirectories(root);
    CHECK(!directoryError);
    if (directoryError) return;
    const MaiFilePath image = root.append(MaiFilePath::fromUtf8("photo.png"));
    const MaiError writeError = MaiFileSystem::writeFile(image, png);
    CHECK(!writeError);
    if (writeError) {
        MaiFileSystem::removeRecursively(root);
        return;
    }
    const std::string imagePath = MaiFileSystem::resolve(image).toUtf8();

    const std::string arguments = json{{"path", "photo.png"}}.dump();
    FakeServer fake;
    fake.chunk = 100000;
    const json call{{"type", "function_call"},
                    {"id", "fc_view_image"},
                    {"call_id", "call_view_image"},
                    {"name", "view_image"},
                    {"arguments", arguments}};
    const json answer{
        {"type", "message"},
        {"id", "msg_image_answer"},
        {"role", "assistant"},
        {"content", json::array({json{{"type", "output_text"}, {"text", "Pixels received."}}})}};
    fake.scripts = {
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array({call})}}}}),
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array({answer})}}}})};
    fake.start();

    std::vector<MaiMessage> history;
    {
        MaiModelConfig config;
        config.baseUrl = fake.base();
        config.wire = MaiWireApi::Responses;
        config.maxRetries = 0;
        config.connectTimeoutSeconds = 5;
        config.totalTimeoutSeconds = 10;
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(makeMaiViewImageTool());
        MaiAgent::Options options;
        options.defaultModel = "deepseek-flash";
        options.maxToolIterations = 3;
        MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config), std::move(tools), options);
        const auto session = agent.submit(MaiCreateSession{root.toUtf8(), "", ""});
        CHECK(session);
        if (session) {
            CHECK(agent.submit(MaiSendPrompt{session.value(), "Inspect photo.png."}));
            agent.waitIdle();
            history = agent.listMessages(session.value());
        }
    }
    // Agent 已退出，临时输入不再被后台线程引用；所有断言只读取已经接收的 HTTP 字节。
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::exists(root));
    std::vector<std::string> bodies;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        bodies = fake.requestBodies;
    }
    CHECK(fake.requestCount.load() == 2);
    CHECK(bodies.size() == 2);
    CHECK(history.size() == 2);
    if (history.size() == 2) {
        CHECK(history.back().completed != 0);
        CHECK(history.back().text() == "Pixels received.");
        bool completedTool = false;
        bool persistedImage = false;
        for (const MaiMessagePart& part : history.back().parts) {
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                completedTool |=
                    tool->tool == "view_image" && tool->state == MaiToolState::Completed;
            if (const auto* pixels = std::get_if<MaiImagePart>(&part.body))
                persistedImage |= pixels->path == imagePath && pixels->mimeType == "image/png";
        }
        CHECK(completedTool && persistedImage);
    }
    if (bodies.size() != 2) return;
    CHECK(bodies.front().find("data:image/") == std::string::npos);
    const json first = json::parse(bodies.front(), nullptr, false);
    const json second = json::parse(bodies.back(), nullptr, false);
    CHECK(!first.is_discarded() && !second.is_discarded());
    if (first.is_discarded() || second.is_discarded()) return;
    CHECK(first["model"] == "deepseek-flash");
    CHECK(second["model"] == "deepseek-flash");
    CHECK(first["tools"].size() == 1);
    if (first["tools"].size() == 1) CHECK(first["tools"][0]["name"] == "view_image");
    const json& messages = second["input"];
    CHECK(messages.is_array() && messages.size() == 4);
    if (!messages.is_array() || messages.size() != 4) return;
    CHECK(messages[0]["role"] == "user");
    CHECK(messages[1]["type"] == "function_call");
    CHECK(messages[1]["call_id"] == "call_view_image");
    CHECK(messages[1]["name"] == "view_image");
    CHECK(messages[1]["arguments"] == arguments);
    CHECK(messages[2]["type"] == "function_call_output");
    CHECK(messages[2]["call_id"] == "call_view_image");
    CHECK(messages[2]["output"].get<std::string>().find("photo.png") != std::string::npos);
    CHECK(messages[3]["type"] == "message");
    CHECK(messages[3]["role"] == "user");
    const json& observation = messages[3]["content"];
    CHECK(observation.is_array() && observation.size() == 2);
    if (!observation.is_array() || observation.size() != 2) return;
    CHECK(observation[0]["type"] == "input_text");
    // 工具图片在整组回执后追加，wire 只能声明真实组来源；原像素字节断言继续独立验证。
    const json source = json::parse(observation[0]["text"].get<std::string>(), nullptr, false);
    CHECK(source.is_object());
    if (source.is_object()) {
        CHECK(source["type"] == "tool_group_image_observation");
        CHECK(source["ownership"] == "tool_group_only");
        CHECK(source["tool_count"] == 1 && source["archived_tool_count"] == 0);
        if (history.size() == 2) {
            std::string toolPartId, imagePartId;
            for (const auto& part : history.back().parts) {
                if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                    if (tool->callId == "call_view_image") toolPartId = part.id;
                } else if (std::holds_alternative<MaiImagePart>(part.body))
                    imagePartId = part.id;
            }
            CHECK(!toolPartId.empty() && !imagePartId.empty());
            CHECK(source["source_message_id"] == history.back().id);
            CHECK(source["image_part_id"] == imagePartId);
            CHECK(source["first_tool_part_id"] == toolPartId);
            CHECK(source["through_tool_part_id"] == toolPartId);
        }
    }
    CHECK(observation[1]["type"] == "input_image");
    const json& encoded = observation[1]["image_url"];
    CHECK(encoded.is_string());
    if (!encoded.is_string()) return;
    const std::string dataUrl = encoded.get<std::string>();
    const std::string prefix = "data:image/png;base64,";
    CHECK(dataUrl.compare(0, prefix.size(), prefix) == 0);
    if (dataUrl.compare(0, prefix.size(), prefix) == 0)
        CHECK(decodeBase64Image(dataUrl.substr(prefix.size())) == png);
}

// 验证历史大图在模型请求中使用有界缓存预览，避免反复解码完整原图。
// 假服务器只接收本地请求，不产生外部模型费用。
void test_large_historical_image_uses_cached_bounded_preview() {
    FakeServer fake;
    fake.script = response_text_delta("ok") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root =
        MaiFileSystem::temporaryDirectory().append(MaiFilePath::fromUtf8("mai-model-preview-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));
    const MaiFilePath preview = root.append(MaiFilePath::fromUtf8("preview.jpg"));
    std::string large(3u * 1024u * 1024u, 'a');
    large.replace(0, 4, "\x89PNG", 4);
    std::vector<MaiFilePath> sources;
    for (int index = 0; index < 4; ++index) {
        sources.push_back(
            root.append(MaiFilePath::fromUtf8("historical-" + std::to_string(index) + ".png")));
        CHECK(!MaiFileSystem::writeFile(sources.back(), large));
    }
    CHECK(!MaiFileSystem::writeFile(preview, std::string("\xff\xd8\xff", 3)));

    int prepared = 0;
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.prepareImage = [&](const std::string& path,
                              const std::string& workspace) -> MaiResult<std::string> {
        ++prepared;
        bool matchesSource = false;
        for (const auto& source : sources)
            if (path == MaiFileSystem::resolve(source).toUtf8()) matchesSource = true;
        CHECK(matchesSource);
        CHECK(workspace == root.toUtf8());
        return preview.toUtf8();
    };
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "glm-5.3-flash";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect the prior frame";
    for (int index = 0; index < 4; ++index)
        user.images.push_back({"historical-" + std::to_string(index) + ".png", "image/png"});
    request.messages.push_back(user);
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    CHECK(!client->stream(request, sink, cancel));
    CHECK(!client->stream(request, sink, cancel));
    CHECK(prepared == 4);
    // 同路径同尺寸的新内容也必须使预览失效；其他历史图片继续命中缓存。
    large[5] = 'b';
    CHECK(!MaiFileSystem::writeFile(sources.front(), large));
    CHECK(!client->stream(request, sink, cancel));
    CHECK(prepared == 5);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    CHECK(body.size() < 3000);
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["input"][0]["content"];
        CHECK(content.size() == 5);
        for (std::size_t index = 1; index < content.size(); ++index) {
            const std::string url = content[index]["image_url"];
            CHECK(url.rfind("data:image/jpeg;base64,", 0) == 0);
        }
    }
    MaiFileSystem::removeRecursively(root);
}

void test_absolute_desktop_image_can_be_outside_the_workspace() {
    FakeServer fake;
    fake.script = response_text_delta("ok") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath temporary = MaiFileSystem::temporaryDirectory();
    const MaiFilePath workspace =
        temporary.append(MaiFilePath::fromUtf8("mai-model-absolute-image-workspace"));
    const MaiFilePath external =
        temporary.append(MaiFilePath::fromUtf8("mai-model-absolute-image-external"));
    MaiFileSystem::removeRecursively(workspace);
    MaiFileSystem::removeRecursively(external);
    CHECK(!MaiFileSystem::createDirectories(workspace));
    CHECK(!MaiFileSystem::createDirectories(external));
    const MaiFilePath image = external.append(MaiFilePath::fromUtf8("photo.png"));
    CHECK(!MaiFileSystem::writeFile(image, std::string("\x89PNG", 4)));

    MaiModelRequest request;
    request.model = "glm-5.4";
    request.workingDirectory = workspace.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({image.toUtf8(), "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["input"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 2);
        if (content.size() == 2) CHECK(content[1]["image_url"] == "data:image/png;base64,iVBORw==");
    }
    MaiFileSystem::removeRecursively(workspace);
    MaiFileSystem::removeRecursively(external);
}

void test_missing_historical_image_does_not_break_later_turns() {
    FakeServer fake;
    fake.script = response_text_delta("ok") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-model-missing-history-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));

    MaiModelRequest request;
    request.model = "glm-5.4";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage oldUser;
    oldUser.role = MaiModelRole::User;
    oldUser.content = "old image";
    oldUser.images.push_back({"removed.png", "image/png"});
    request.messages.push_back(oldUser);
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.content = "previous response";
    request.messages.push_back(assistant);
    MaiModelMessage currentUser;
    currentUser.role = MaiModelRole::User;
    currentUser.content = "continue";
    request.messages.push_back(currentUser);

    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    if (!sent.is_discarded()) {
        const json& content = sent["input"][0]["content"];
        CHECK(content.is_array());
        CHECK(content.size() == 1);
        if (content.size() == 1) {
            CHECK(content[0]["type"] == "input_text");
            CHECK(content[0]["text"].get<std::string>().find("no longer available") !=
                  std::string::npos);
        }
        CHECK(sent["input"].back()["content"] == "continue");
    }
    MaiFileSystem::removeRecursively(root);
}

void test_missing_current_image_is_reported() {
    FakeServer fake;
    fake.script = response_text_delta("unexpected") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-model-missing-current-image-test"));
    MaiFileSystem::removeRecursively(root);
    CHECK(!MaiFileSystem::createDirectories(root));

    MaiModelRequest request;
    request.model = "glm-5.4";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "inspect";
    user.images.push_back({"missing.png", "image/png"});
    request.messages.push_back(user);

    const Collected result = runAgainst(fake, request);
    CHECK(!result.done);
    CHECK(result.code != MaiErrorCode::Ok);
    CHECK(result.error.find("cannot open file for reading") != std::string::npos);
    CHECK(fake.requestCount.load() == 0);
    MaiFileSystem::removeRecursively(root);
}

void test_invalid_utf8_history_is_replaced_before_serialization() {
    FakeServer fake;
    fake.script = response_text_delta("ok") + std::string(kCompleted);
    fake.chunk = 100000;
    fake.start();

    MaiModelRequest request;
    request.model = "glm-5.4";
    MaiModelMessage toolResult;
    toolResult.role = MaiModelRole::ToolResult;
    toolResult.toolCallId = "call_legacy";
    // 这是 Windows Shell 错误开头的 GBK 字节。旧数据库可能保存这种原始形式，
    // 因为重定向的 cmd.exe 输出当时没有转成 UTF-8。
    toolResult.content.assign("\xCF\xB5\xCD\xB3\xD5\xD2", 6);
    request.messages.push_back(toolResult);

    bool threw = false;
    Collected result;
    try {
        result = runAgainst(fake, request);
    } catch (...) {
        threw = true;
    }

    CHECK(!threw);
    CHECK(result.done);
    std::string body;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        body = fake.lastBody;
    }
    const json sent = json::parse(body, nullptr, false);
    CHECK(!sent.is_discarded());
    CHECK(sent["input"].size() == 1);
    CHECK(sent["input"][0]["type"] == "function_call_output");
    CHECK(sent["input"][0]["output"].get<std::string>().find("\xEF\xBF\xBD") != std::string::npos);
}

// 验证 Responses 流中推理文本与函数调用按事件顺序分别回传。
// 工具参数未完整结束前不能上报为可执行的完整调用。
void test_responses_streams_reasoning_text_and_function_calls() {
    FakeServer fake;
    fake.chunk = 1;
    fake.script =
        sse(json{{"type", "response.created"}}) +
        sse(json{{"type", "response.reasoning_text.delta"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "Think"}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 1},
                 {"content_index", 0},
                 {"delta", "Ready"}}) +
        sse(json{
            {"type", "response.output_item.added"},
            {"output_index", 2},
            {"item", {{"type", "function_call"}, {"call_id", "call_7"}, {"name", "status"}}}}) +
        sse(json{{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "{\"pa"}}) +
        sse(json{{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "th\":\"a\"}"}}) +
        sse(json{{"type", "response.function_call_arguments.done"},
                 {"output_index", 2},
                 {"arguments", R"({"path":"a"})"}}) +
        sse(json{
            {"type", "response.completed"},
            {"response",
             {{"status", "completed"},
              {"output", json::array({json{{"type", "reasoning"},
                                           {"content", json::array({json{{"type", "reasoning_text"},
                                                                         {"text", "Think"}}})}},
                                      json{{"type", "message"},
                                           {"content", json::array({json{{"type", "output_text"},
                                                                         {"text", "Ready"}}})}},
                                      json{{"type", "function_call"},
                                           {"call_id", "call_7"},
                                           {"name", "status"},
                                           {"arguments", R"({"path":"a"})"}}})}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "deepseek-flash";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Check status";
    request.messages.push_back(std::move(user));
    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    CHECK(result.text == "Ready");
    CHECK(result.reasoning == "Think");
    CHECK(result.calls.size() == 1);
    if (result.calls.size() == 1) {
        CHECK(result.calls[0].id == "call_7");
        CHECK(result.calls[0].name == "status");
        CHECK(result.calls[0].arguments == R"({"path":"a"})");
    }
}

// item.done 的完整正文也是可见输出；不能依赖供应商重复发送终止 output，
// 已经流出的前缀则不能因 done 快照再次显示。
void test_responses_item_done_recovers_missing_text_without_duplicates() {
    FakeServer fake;
    const json first{{"id", "msg_progress"},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"phase", "commentary"},
                     {"content", json::array({json{{"type", "output_text"}, {"text", "ABC"}}})}};
    const json second{{"id", "msg_final"},
                      {"type", "message"},
                      {"role", "assistant"},
                      {"phase", "final_answer"},
                      {"content", json::array({json{{"type", "output_text"}, {"text", "Done"}}})}};
    fake.script =
        sse(json{{"type", "response.output_item.added"},
                 {"output_index", 0},
                 {"item", json{{"id", "msg_progress"},
                               {"type", "message"},
                               {"role", "assistant"},
                               {"phase", "commentary"}}}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "A"}}) +
        sse(json{{"type", "response.output_text.done"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"text", "AB"}}) +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}}) +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 1}, {"item", second}}) +
        sse(json{{"type", "response.completed"}});
    fake.start();
    MaiModelRequest request;
    request.model = "fake";
    const Collected result = runAgainst(fake, request);
    CHECK(result.done);
    CHECK(result.text == "ABCDone");
    CHECK(result.reasoning.empty());
    CHECK(result.startedItems.size() == 2);
    CHECK(result.completedItems.size() == 2);
    if (result.startedItems.size() == 2) {
        CHECK(result.startedItems[0].id == "msg_progress");
        CHECK(result.startedItems[0].phase == MaiModelTextPhase::Commentary);
        CHECK(result.startedItems[1].phase == MaiModelTextPhase::FinalAnswer);
    }
}

// 同一个 HTTP 响应里的两条 assistant message 仍是两段发言，
// 重复的完成快照不得把它们拼成一个 part 或重复正文。
void test_responses_message_items_remain_distinct_in_agent_history() {
    FakeServer fake;
    const json first{{"id", "msg_a"},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"phase", "commentary"},
                     {"content", json::array({json{{"type", "output_text"}, {"text", "A"}}})}};
    const json second{{"id", "msg_b"},
                      {"type", "message"},
                      {"role", "assistant"},
                      {"phase", "final_answer"},
                      {"content", json::array({json{{"type", "output_text"}, {"text", "B"}}})}};
    fake.script =
        sse(json{{"type", "response.output_item.added"},
                 {"output_index", 0},
                 {"item", json{{"id", "msg_a"},
                               {"type", "message"},
                               {"role", "assistant"},
                               {"phase", "commentary"},
                               {"content", json::array()}}}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 0},
                 {"content_index", 0},
                 {"delta", "A"}}) +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}}) +
        sse(json{{"type", "response.output_item.added"},
                 {"output_index", 1},
                 {"item", json{{"id", "msg_b"},
                               {"type", "message"},
                               {"role", "assistant"},
                               {"phase", "final_answer"},
                               {"content", json::array()}}}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 1},
                 {"content_index", 0},
                 {"delta", "B"}}) +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 1}, {"item", second}}) +
        sse(json{{"type", "response.completed"},
                 {"response", {{"output", json::array({first, second})}}}});
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    config.wire = MaiWireApi::Responses;
    MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config));
    const std::string session = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent.submit(MaiSendPrompt{session, "check"});
    agent.waitIdle();
    const auto messages = agent.listMessages(session);
    CHECK(messages.size() == 2);
    if (messages.size() != 2) return;
    std::vector<const MaiMessagePart*> textParts;
    for (const auto& part : messages[1].parts)
        if (std::holds_alternative<MaiTextPart>(part.body)) textParts.push_back(&part);
    CHECK(textParts.size() == 2);
    if (textParts.size() != 2) return;
    const auto* a = std::get_if<MaiTextPart>(&textParts[0]->body);
    const auto* b = std::get_if<MaiTextPart>(&textParts[1]->body);
    CHECK(a && a->text == "A");
    CHECK(b && b->text == "B");
    CHECK(textParts[0]->id != textParts[1]->id);
}

// 兼容服务可能只在 terminal 提供最终快照；补尾必须归还原来的 item，
// 包括 item.done 中正文缺失以及多条消息未发送 item.done 的情况。
void test_responses_terminal_snapshots_update_their_original_items() {
    const auto item = [](const char* id, const char* text) {
        return json{{"id", id},
                    {"type", "message"},
                    {"role", "assistant"},
                    {"content", json::array({json{{"type", "output_text"}, {"text", text}}})}};
    };
    const auto runCase = [&](const std::string& prefix, const json& output,
                             const std::vector<std::string>& expected) {
        FakeServer fake;
        fake.script =
            prefix + sse(json{{"type", "response.completed"}, {"response", {{"output", output}}}});
        fake.start();
        MaiModelConfig config;
        config.baseUrl = fake.base();
        config.apiKey = "test-key";
        config.wire = MaiWireApi::Responses;
        MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config));
        const std::string session = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
        agent.submit(MaiSendPrompt{session, "check terminal recovery"});
        agent.waitIdle();
        std::vector<std::string> actual;
        for (const auto& message : agent.listMessages(session))
            if (message.role == MaiRole::Assistant)
                for (const auto& part : message.parts)
                    if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                        actual.push_back(text->text);
        CHECK(actual == expected);
    };
    runCase(
        sse(json{{"type", "response.output_item.done"},
                 {"output_index", 0},
                 {"item", json{{"id", "msg_a"}, {"type", "message"}, {"content", json::array()}}}}),
        json::array({item("msg_a", "AA")}), {"AA"});
    const std::string deltas = sse(json{{"type", "response.output_text.delta"},
                                        {"output_index", 0},
                                        {"content_index", 0},
                                        {"delta", "A"}}) +
                               sse(json{{"type", "response.output_text.delta"},
                                        {"output_index", 1},
                                        {"content_index", 0},
                                        {"delta", "B"}});
    runCase(deltas, json::array({item("msg_a", "AA"), item("msg_b", "BB")}), {"AA", "BB"});
}

// 验证历史图片和扁平工具定义被序列化为供应商要求的 Responses 结构。
// 与假服务器收到的请求体比较，避免只对照本地构造函数自证正确。
void test_responses_serializes_history_images_and_flat_tools() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-responses-image-test"));
    CHECK(!MaiFileSystem::createDirectories(root));
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("photo.png")),
                                    std::string("\x89PNG", 4)));
    MaiModelRequest request;
    request.model = "deepseek-flash";
    request.reasoningEffort = "max";
    request.baseInstructions = "Follow the user request.";
    request.workingDirectory = root.toUtf8();
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Inspect this";
    user.images.push_back({"photo.png", "image/png"});
    request.messages.push_back(std::move(user));
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.reasoning = "I should check the value.";
    assistant.invocations.push_back({"call_8", "lookup", R"({"query":"x"})"});
    request.messages.push_back(std::move(assistant));
    MaiModelMessage result;
    result.role = MaiModelRole::ToolResult;
    result.toolCallId = "call_8";
    result.content = "found";
    request.messages.push_back(std::move(result));
    request.tools.push_back({"lookup", "Look up a value", R"({"type":"object"})"});
    const Collected answer = runAgainst(fake, request);
    CHECK(answer.done);
    std::string wire;
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        wire = fake.lastBody;
    }
    const json body = json::parse(wire);
    CHECK(body["model"] == "deepseek-flash");
    CHECK(body["reasoning"]["effort"] == "max");
    CHECK(body["instructions"] == "Follow the user request.");
    CHECK(body["input"].size() == 4);
    CHECK(body["input"][0]["role"] == "user");
    CHECK(body["input"][0]["content"][1]["type"] == "input_image");
    CHECK(body["input"][0]["content"][1]["image_url"].get<std::string>().find(
              "data:image/png;base64,") == 0);
    CHECK(body["input"][1]["type"] == "reasoning");
    CHECK(body["input"][1]["content"][0]["text"] == "I should check the value.");
    CHECK(body["input"][2]["type"] == "function_call");
    CHECK(body["input"][2]["call_id"] == "call_8");
    CHECK(body["input"][3]["type"] == "function_call_output");
    CHECK(body["input"][3]["call_id"] == "call_8");
    CHECK(body["tools"][0]["name"] == "lookup");
    CHECK(!body["tools"][0].contains("function"));
    MaiFileSystem::removeRecursively(root);
}

// 续接必须保留整个 output 的顺序，而不是把 reasoning、message、function_call 分组重建。
// 所有签名和加密值均为合成数据；未知成员和显式 null 也属于需要原样往返的响应快照。
void test_responses_continuation_replays_original_output_before_tool_results() {
    const std::string reasoning = "Inspect\r\n\xE7\x95\x8C";
    const json thought{
        {"id", "rs_signed"},
        {"type", "reasoning"},
        {"summary", json::array()},
        {"content", json::array({json{{"type", "reasoning_text"}, {"text", reasoning}}})},
        {"signature", "synthetic-signature+/=="},
        {"encrypted_content", nullptr},
        {"format", "anthropic-claude-v1"},
        {"provider_extension", {{"nullable", nullptr}, {"values", {1, true, "x"}}}}};
    const json commentary{{"id", "msg_commentary"},
                          {"type", "message"},
                          {"role", "assistant"},
                          {"status", "completed"},
                          {"phase", "commentary"},
                          {"content", json::array({json{{"type", "output_text"},
                                                        {"text", "Checking"},
                                                        {"annotations", json::array()}}})}};
    const json firstCall{{"id", "fc_item_a"},
                         {"type", "function_call"},
                         {"call_id", "call_a"},
                         {"name", "lookup"},
                         {"arguments", R"({"path":"a"})"}};
    const json encrypted{{"id", "rs_encrypted"},
                         {"type", "reasoning"},
                         {"summary", json::array()},
                         {"content", json::array()},
                         {"encrypted_content", "synthetic-encrypted+/=="},
                         {"signature", nullptr}};
    const json secondCall{{"id", "fc_item_b"},
                          {"type", "function_call"},
                          {"call_id", "call_b"},
                          {"name", "lookup"},
                          {"arguments", R"({"path":"b"})"}};
    const json output = json::array({thought, commentary, firstCall, encrypted, secondCall});
    for (const std::size_t chunk : {std::size_t(1), std::size_t(17), std::size_t(8192)}) {
        FakeServer fake;
        fake.chunk = chunk;
        fake.script =
            ": OPENROUTER PROCESSING\n\n" +
            sse(json{{"type", "response.reasoning_text.delta"},
                     {"output_index", 0},
                     {"content_index", 0},
                     {"item_id", "rs_signed"},
                     {"delta", "Inspect\r\n"}}) +
            sse(json{{"type", "response.reasoning_text.done"},
                     {"output_index", 0},
                     {"content_index", 0},
                     {"item_id", "rs_signed"},
                     {"text", reasoning}}) +
            sse(json{{"type", "response.output_text.delta"},
                     {"output_index", 1},
                     {"content_index", 0},
                     {"delta", "Check"}}) +
            sse(json{
                {"type", "response.output_item.done"}, {"output_index", 0}, {"item", thought}}) +
            sse(json{{"type", "response.output_item.added"},
                     {"output_index", 4},
                     {"item",
                      {{"type", "function_call"},
                       {"id", "fc_item_b"},
                       {"call_id", "call_b"},
                       {"name", "lookup"},
                       {"arguments", ""}}}}) +
            sse(json{{"type", "response.output_item.added"},
                     {"output_index", 2},
                     {"item",
                      {{"type", "function_call"},
                       {"id", "fc_item_a"},
                       {"call_id", "call_a"},
                       {"name", "lookup"},
                       {"arguments", ""}}}}) +
            sse(json{{"type", "response.function_call_arguments.delta"},
                     {"output_index", 4},
                     {"delta", "{\"path\":"}}) +
            sse(json{{"type", "response.function_call_arguments.delta"},
                     {"output_index", 2},
                     {"delta", R"({"path":"a"})"}}) +
            sse(json{{"type", "response.function_call_arguments.delta"},
                     {"output_index", 4},
                     {"delta", "\"b\"}"}}) +
            sse(json{
                {"type", "response.output_item.done"}, {"output_index", 4}, {"item", secondCall}}) +
            sse(json{
                {"type", "response.output_item.done"}, {"output_index", 2}, {"item", firstCall}}) +
            sse(json{
                {"type", "response.output_item.done"}, {"output_index", 1}, {"item", commentary}}) +
            sse(json{
                {"type", "response.output_item.done"}, {"output_index", 3}, {"item", encrypted}}) +
            sse(json{{"type", "response.completed"},
                     {"response", {{"status", "completed"}, {"output", output}}}});
        fake.start();
        MaiModelRequest request;
        request.model = "anthropic/claude-fable-5.2";
        request.reasoningEffort = "max";
        const Collected answer = runAgainst(fake, request);
        CHECK(answer.done);
        CHECK(answer.text == "Checking");
        CHECK(answer.reasoning == reasoning);
        CHECK(answer.continuations.size() == 1);
        CHECK(answer.deliveries ==
              std::vector<std::string>({"state", "call:call_a", "call:call_b"}));
        CHECK(answer.calls.size() == 2);
        if (answer.continuations.size() != 1 || answer.calls.size() != 2) continue;
        const auto& state = answer.continuations.front();
        CHECK(state.format == "responses.output.v1");
        CHECK(state.provider == "openrouter");
        CHECK(state.model == request.model);
        CHECK(json::parse(state.payload) == output);
        MaiModelMessage user;
        user.content = "Check both files";
        request.messages.push_back(user);
        MaiModelMessage assistant;
        assistant.role = MaiModelRole::Assistant;
        assistant.content = "Display projection must not be duplicated";
        assistant.reasoning = "Display projection is not signed reasoning";
        assistant.invocations = answer.calls;
        assistant.continuation = state;
        request.messages.push_back(assistant);
        for (const auto& call : answer.calls) {
            MaiModelMessage result;
            result.role = MaiModelRole::ToolResult;
            result.toolCallId = call.id;
            result.content = "Result for " + call.id;
            request.messages.push_back(result);
        }
        CHECK(runAgainst(fake, request).done);
        std::lock_guard<std::mutex> lock(fake.mutex);
        const json body = json::parse(fake.lastBody);
        CHECK(wireValue(body, "/store") == false);
        CHECK(!body.contains("previous_response_id"));
        CHECK(wireValue(body, "/reasoning/effort") == "max");
        CHECK(!body.contains("thinking"));
        CHECK(!body.contains("reasoning_effort"));
        const json input = wireValue(body, "/input");
        CHECK(input.is_array() && input.size() == 1 + output.size() + 2);
        if (!input.is_array() || input.size() != 1 + output.size() + 2) continue;
        for (std::size_t index = 0; index < output.size(); ++index)
            CHECK(input[index + 1] == output[index]);
        CHECK(wireValue(body, "/input/6/type") == "function_call_output");
        CHECK(wireValue(body, "/input/6/call_id") == "call_a");
        CHECK(wireValue(body, "/input/7/call_id") == "call_b");
    }
}

// item.done 是独立的完整快照；终态不重复 output 时也不能丢掉签名、摘要或加密数据。
void test_responses_continuation_uses_completed_items_without_terminal_output() {
    const json thought{
        {"id", "rs_summary"},
        {"type", "reasoning"},
        {"summary", json::array({json{{"type", "summary_text"}, {"text", "Summary"}}})},
        {"encrypted_content", "synthetic-encrypted-only"},
        {"signature", "synthetic-late-signature"}};
    FakeServer fake;
    fake.script =
        sse(json{{"type", "response.reasoning_summary_text.delta"},
                 {"output_index", 0},
                 {"summary_index", 0},
                 {"delta", "Sum"}}) +
        sse(json{{"type", "response.reasoning_summary_text.done"},
                 {"output_index", 0},
                 {"summary_index", 0},
                 {"text", "Summary"}}) +
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", thought}}) +
        sse(json{{"type", "response.completed"}, {"response", {{"status", "completed"}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    const Collected answer = runAgainst(fake, request);
    CHECK(answer.done);
    CHECK(answer.reasoning == "Summary");
    CHECK(answer.text.empty());
    CHECK(answer.continuations.size() == 1);
    if (answer.continuations.size() == 1)
        CHECK(json::parse(answer.continuations.front().payload) == json::array({thought}));
}

// 显式终态数组是权威结果；此前 item.done 不能把终态已删除的调用重新变成可执行工具。
void test_responses_explicit_empty_terminal_does_not_recover_old_tool() {
    const json call{{"type", "function_call"},
                    {"id", "fc_removed"},
                    {"call_id", "call_removed"},
                    {"name", "lookup"},
                    {"arguments", "{}"}};
    FakeServer fake;
    fake.script =
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", call}}) +
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    const Collected answer = runAgainst(fake, request);
    CHECK(!answer.done);
    CHECK(answer.code == MaiErrorCode::Protocol);
    CHECK(answer.continuations.empty());
    CHECK(answer.calls.empty());
    CHECK(answer.deliveries.empty());
}

// 已完成 reasoning 前缀不能证明整份响应完整；漏掉后面的可见 item 会让下轮回放丢掉答案。
void test_responses_item_snapshot_fallback_rejects_unfinished_visible_item() {
    const json thought{{"type", "reasoning"},
                       {"id", "rs_prefix"},
                       {"summary", json::array()},
                       {"content", json::array()},
                       {"signature", "synthetic-prefix-signature"},
                       {"encrypted_content", "synthetic-prefix-encrypted"}};
    FakeServer fake;
    fake.script =
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", thought}}) +
        sse(json{{"type", "response.output_text.delta"},
                 {"output_index", 1},
                 {"content_index", 0},
                 {"item_id", "msg_unfinished"},
                 {"delta", "Visible answer"}}) +
        sse(json{{"type", "response.completed"}, {"response", {{"status", "completed"}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    const Collected answer = runAgainst(fake, request);
    CHECK(answer.text == "Visible answer");
    CHECK(!answer.done);
    CHECK(answer.code == MaiErrorCode::Protocol);
    CHECK(answer.continuations.empty());
    CHECK(answer.calls.empty());
    CHECK(answer.deliveries.empty());
}

// 有一个调用无效时也不能先交付前面的有效调用或可重放状态；失败草稿不属于成功响应。
void test_responses_never_publishes_continuation_or_tools_from_unsuccessful_output() {
    const json call{{"type", "function_call"},
                    {"id", "fc_valid"},
                    {"call_id", "call_valid"},
                    {"name", "lookup"},
                    {"arguments", "{}"}};
    const std::string prefix =
        sse(json{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", call}});
    const json partialCall{{"type", "function_call"},
                           {"id", "fc_invalid"},
                           {"call_id", "call_invalid"},
                           {"name", "lookup"}};
    const std::vector<std::string> scripts{
        prefix,
        prefix + "data: [DONE]\n\n",
        prefix + sse(json{{"type", "response.incomplete"},
                          {"response",
                           {{"output", json::array({call})},
                            {"incomplete_details", {{"reason", "max_output_tokens"}}}}}}),
        prefix + sse(json{{"type", "response.failed"},
                          {"response",
                           {{"output", json::array({call})},
                            {"error", {{"message", "synthetic failure"}}}}}}),
        prefix + sse(json{{"type", "error"}, {"error", {{"message", "synthetic stream error"}}}}),
        prefix + sse(json{{"type", "response.completed"},
                          {"response", {{"status", "completed"}, {"output", "invalid"}}}}),
        prefix +
            sse(json{{"type", "response.completed"},
                     {"response",
                      {{"status", "completed"}, {"output", json::array({call, partialCall})}}}}),
    };
    for (const auto& script : scripts) {
        FakeServer fake;
        fake.script = script;
        fake.start();
        MaiModelRequest request;
        request.model = "anthropic/claude-fable-5.2";
        const Collected answer = runAgainst(fake, request);
        CHECK(!answer.done);
        CHECK(answer.calls.empty());
        CHECK(answer.continuations.empty());
    }
}

// state 持久化回调可以接受取消；返回后不能继续交付已生成的工具调用。
void test_responses_cancel_during_state_callback_prevents_tool_delivery() {
    const json call{{"type", "function_call"},
                    {"id", "fc_cancel"},
                    {"call_id", "call_cancel"},
                    {"name", "lookup"},
                    {"arguments", "{}"}};
    FakeServer fake;
    fake.script =
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array({call})}}}});
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.wire = MaiWireApi::Responses;
    config.apiKey = "synthetic-cancel-key";
    auto client = makeMaiModelClient(config);
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    std::atomic<bool> cancel{false};
    int states = 0, calls = 0;
    MaiStreamSink sink;
    sink.onContinuationState = [&](const MaiModelContinuationState&) {
        ++states;
        cancel.store(true);
    };
    sink.onToolCall = [&](const MaiToolInvocation&) { ++calls; };
    CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::Canceled);
    CHECK(states == 1);
    CHECK(calls == 0);
}

// 续接状态只属于原 provider/model；同来源损坏数据必须在发 HTTP 前报错，不能静默降级。
void test_responses_continuation_checks_source_and_rejects_corruption() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.content = "Visible history";
    assistant.reasoning = "Unsigned display history";
    assistant.continuation =
        MaiModelContinuationState{"responses.output.v1", "openrouter", request.model, "not-json"};
    request.messages.push_back(assistant);
    for (const char* payload :
         {"not-json", "{}", "[null]", R"([{"type":"reasoning","content":"invalid"}])"}) {
        request.messages[0].continuation->payload = payload;
        const Collected answer = runAgainst(fake, request);
        CHECK(!answer.done);
        CHECK(answer.code == MaiErrorCode::Protocol);
        CHECK(fake.requestCount.load() == 0);
    }
    request.messages[0].continuation->provider = "deepseek";
    CHECK(runAgainst(fake, request).done);
    request.messages[0].continuation->provider = "openrouter";
    request.messages[0].continuation->model = "anthropic/other-model";
    CHECK(runAgainst(fake, request).done);
    request.messages[0].continuation.reset();
    CHECK(runAgainst(fake, request).done);
    std::lock_guard<std::mutex> lock(fake.mutex);
    for (const auto& wire : fake.requestBodies) {
        const json body = json::parse(wire);
        CHECK(wireValue(body, "/input").size() == 1);
        CHECK(wireValue(body, "/input/0/type") == "message");
        CHECK(wireValue(body, "/input/0/content") == "Visible history");
        CHECK(wire.find("Unsigned display history") == std::string::npos);
        CHECK(wire.find("invalid") == std::string::npos);
    }
}

// 现代状态不匹配时也不能把它的显示思考转成另一个供应商的未签名 reasoning item。
// 无状态旧记录的兼容行为由原 GLM/DeepSeek fixture 单独覆盖。
void test_responses_foreign_continuation_uses_only_ordinary_history() {
    const json signedOutput = json::array(
        {json{{"type", "reasoning"},
              {"id", "rs_foreign"},
              {"summary", json::array()},
              {"content",
               json::array({json{{"type", "reasoning_text"}, {"text", "Foreign signed text"}}})},
              {"signature", "synthetic-foreign-signature"},
              {"encrypted_content", "synthetic-foreign-encrypted"}}});
    struct RouteCase {
        const char* sourceProvider;
        const char* sourceModel;
        const char* targetModel;
    };
    const RouteCase cases[] = {
        {"openrouter", "anthropic/claude-fable-5.2", "glm-5.4"},
        {"openrouter", "anthropic/claude-fable-5.2", "deepseek-flash"},
        {"deepseek", "deepseek-flash", "deepseek-v4-pro"},
    };
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    for (const auto& route : cases) {
        MaiModelRequest request;
        request.model = route.targetModel;
        MaiModelMessage assistant;
        assistant.role = MaiModelRole::Assistant;
        assistant.content = "Visible history";
        assistant.reasoning = "Foreign display reasoning";
        assistant.invocations = {{"call_foreign", "lookup", "{}"}};
        assistant.continuation = MaiModelContinuationState{
            "responses.output.v1", route.sourceProvider, route.sourceModel, signedOutput.dump()};
        request.messages.push_back(assistant);
        MaiModelMessage result;
        result.role = MaiModelRole::ToolResult;
        result.toolCallId = "call_foreign";
        result.content = "Retained tool result";
        request.messages.push_back(result);
        CHECK(runAgainst(fake, request).done);
        std::lock_guard<std::mutex> lock(fake.mutex);
        const json body = json::parse(fake.lastBody);
        CHECK(wireValue(body, "/input").size() == 3);
        CHECK(wireValue(body, "/input/0/type") == "message");
        CHECK(wireValue(body, "/input/0/content") == "Visible history");
        CHECK(wireValue(body, "/input/1/type") == "function_call");
        CHECK(wireValue(body, "/input/1/call_id") == "call_foreign");
        CHECK(wireValue(body, "/input/2/type") == "function_call_output");
        CHECK(wireValue(body, "/input/2/call_id") == "call_foreign");
        CHECK(fake.lastBody.find("Foreign display reasoning") == std::string::npos);
        CHECK(fake.lastBody.find("Foreign signed text") == std::string::npos);
        CHECK(fake.lastBody.find("synthetic-foreign-signature") == std::string::npos);
        CHECK(fake.lastBody.find("synthetic-foreign-encrypted") == std::string::npos);
    }
}

// GLM Responses 官方终态无需额外哨兵；空文本和加密快照仍需发布独立状态。
void test_glm_responses_completes_without_done_and_keeps_metadata_only_state() {
    const json thought{{"type", "reasoning"},
                       {"id", "glm_reasoning"},
                       {"content", json::array()},
                       {"summary", json::array()},
                       {"encrypted_content", "synthetic-glm-state"}};
    FakeServer fake;
    fake.script =
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array({thought})}}}});
    fake.start();
    for (const char* model : {"glm-5.4", "glm-5.3-flash"}) {
        MaiModelRequest request;
        request.model = model;
        const Collected answer = runAgainst(fake, request);
        CHECK(answer.done);
        CHECK(answer.text.empty() && answer.reasoning.empty());
        CHECK(answer.continuations.size() == 1);
        if (answer.continuations.size() == 1) {
            CHECK(answer.continuations[0].provider == "glm");
            CHECK(answer.continuations[0].model == model);
            CHECK(json::parse(answer.continuations[0].payload) == json::array({thought}));
        }
        std::lock_guard<std::mutex> lock(fake.mutex);
        const json body = json::parse(fake.lastBody);
        CHECK(wireValue(body, "/store") == false);
        CHECK(body.contains("input") && !body.contains("messages"));
    }
}

// Opus 使用正式客户端入口核对型号能力与真实请求，不借用其它型号的声明或凭据。
void test_opus_55_catalog_and_responses_request() {
    const std::string model = "anthropic/claude-opus-5.5";
    const auto info = maiCloudModelInfo(model);
    CHECK(info && info->provider == "openrouter" && info->wire == MaiWireApi::Responses);
    CHECK(info && info->supportsImages && info->supportsReasoningEffort);
    CHECK(info && info->contextLimits.contextWindowTokens == 1000000);
    CHECK(info && info->contextLimits.maxOutputTokens == 128000);
    const auto limits = maiOfficialModelContextLimits("https://openrouter.ai/api/v1", model);
    CHECK(limits.contextWindowTokens == 1000000 && limits.maxOutputTokens == 128000);
    CHECK(!maiCloudModelInfo(model + "-preview"));
    CHECK(maiReasoningEffortsForModel(model) ==
          std::vector<std::string>({"low", "medium", "high", "xhigh", "max"}));
    CHECK(maiNormalizeReasoningEffortForModel(model, "medium") == "medium");
    CHECK(maiNormalizeReasoningEffortForModel(model, "xhigh") == "xhigh");
    CHECK(maiNormalizeReasoningEffortForModel("deepseek-flash", "xhigh") == "high");
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    MaiModelRequest request;
    request.model = model;
    request.sessionId = "owned_opus_session";
    request.baseInstructions = "Keep this fixed instruction.";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Check the selected model.";
    request.messages.push_back(user);
    request.tools = {{"lookup", "Read an owned fixture", R"({"type":"object"})"}};
    for (const char* effort : {"low", "medium", "high", "xhigh", "max"}) {
        request.reasoningEffort = effort;
        CHECK(maiIsValidReasoningEffortForModel(model, effort));
        CHECK(runAgainst(fake, request).done);
        std::lock_guard<std::mutex> lock(fake.mutex);
        const auto body = json::parse(fake.lastBody);
        CHECK(wireValue(body, "/model") == model);
        CHECK(wireValue(body, "/reasoning/effort") == effort);
        CHECK(wireValue(body, "/session_id") == request.sessionId);
        CHECK(wireValue(body, "/cache_control") == json({{"type", "ephemeral"}}));
        CHECK(wireValue(body, "/instructions") == request.baseInstructions);
        CHECK(wireValue(body, "/tools/0/name") == "lookup");
        CHECK(wireValue(body, "/store") == false);
    }
    const auto before = fake.requestCount.load();
    for (const char* effort : {"none", "invalid"}) {
        request.reasoningEffort = effort;
        CHECK(runAgainst(fake, request).code == MaiErrorCode::InvalidInput);
    }
    CHECK(fake.requestCount == before);
}

// 会话提示只增加已核供应商的顶层字段；工具 schema 本身含同名字段，不能对原始 JSON 搜索判定。
void test_responses_cache_session_hints_preserve_the_original_request() {
    struct ProviderCase {
        const char* model;
        int hints;  // 0: 无提示；1: OpenRouter；2: GLM。
    };
    const ProviderCase cases[] = {
        {"anthropic/claude-fable-5.2", 1},
        {"anthropic/claude-opus-5.5", 1},
        {"glm-5.4", 2},
        {"glm-5.3-flash", 2},
        {"deepseek-flash", 0},
        {"deepseek-v4-pro", 0},
        {"unknown-fixture", 0},
        {"local:synthetic", 0},
    };
    const auto withoutHints = [](json body) {
        body.erase("session_id");
        body.erase("cache_control");
        body.erase("prompt_cache_key");
        return body;
    };
    for (const auto& item : cases) {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.completed"},
                               {"response", {{"status", "completed"}, {"output", json::array()}}}});
        fake.start();
        const auto body = [&] {
            std::lock_guard<std::mutex> lock(fake.mutex);
            return json::parse(fake.lastBody, nullptr, false);
        };
        MaiModelRequest request;
        request.model = item.model;
        request.reasoningEffort = "high";
        request.maxOutputTokens = 273;
        request.baseInstructions = "Preserve these stable instructions.";
        request.messages = {{MaiModelRole::User, "First input"},
                            {MaiModelRole::Assistant, "Earlier answer"},
                            {MaiModelRole::User, "Current input"}};
        const std::string parameters =
            R"({"type":"object","properties":{"session_id":{"type":"string"},"cache_control":{"type":"string"},"prompt_cache_key":{"type":"string"}}})";
        // 刻意逆字典序，缓存元数据不能顺带重排原工具声明。
        request.tools = {{"z_lookup", "Read retained data", parameters},
                         {"a_lookup", "Read another retained source", parameters}};
        CHECK(runAgainst(fake, request).done);
        const json original = body();
        CHECK(original.is_object());
        if (!original.is_object()) continue;
        CHECK(!original.contains("session_id"));
        CHECK(!original.contains("cache_control"));
        CHECK(!original.contains("prompt_cache_key"));
        CHECK(wireValue(original, "/store") == false);
        CHECK(wireValue(original, "/max_output_tokens") == 273);
        CHECK(wireValue(original, "/instructions") == request.baseInstructions);
        const json expectedInput = json::array({
            json{{"type", "message"}, {"role", "user"}, {"content", "First input"}},
            json{{"type", "message"}, {"role", "assistant"}, {"content", "Earlier answer"}},
            json{{"type", "message"}, {"role", "user"}, {"content", "Current input"}},
        });
        CHECK(wireValue(original, "/input") == expectedInput);
        CHECK(wireValue(original, "/tools/0/parameters") == json::parse(parameters));
        json sameSession;
        const std::vector<std::string> sessions{"session_alpha", "session_alpha", "session_beta",
                                                std::string(256, 'x'), "!~\"\\/#"};
        for (std::size_t index = 0; index < sessions.size(); ++index) {
            request.sessionId = sessions[index];
            CHECK(runAgainst(fake, request).done);
            const json actual = body();
            CHECK(actual.is_object());
            if (!actual.is_object()) continue;
            CHECK(withoutHints(actual) == original);
            CHECK(wireValue(actual, "/input") == expectedInput);
            CHECK(wireValue(actual, "/tools") == wireValue(original, "/tools"));
            CHECK(wireValue(actual, "/instructions") == request.baseInstructions);
            if (item.hints == 1) {
                CHECK(wireValue(actual, "/session_id") == sessions[index]);
                CHECK(wireValue(actual, "/cache_control") == json({{"type", "ephemeral"}}));
                CHECK(!actual.contains("prompt_cache_key"));
            } else if (item.hints == 2) {
                CHECK(wireValue(actual, "/prompt_cache_key") == sessions[index]);
                CHECK(!actual.contains("session_id"));
                CHECK(!actual.contains("cache_control"));
            } else {
                CHECK(!actual.contains("session_id"));
                CHECK(!actual.contains("cache_control"));
                CHECK(!actual.contains("prompt_cache_key"));
                CHECK(actual == original);
            }
            if (index == 0) sameSession = actual;
            if (index == 1) CHECK(actual == sameSession);
            if (index == 2 && item.hints != 0) CHECK(actual != sameSession);
        }
        request.sessionId.clear();
        CHECK(runAgainst(fake, request).done);
        CHECK(body() == original);
        // 默认输出/推理设置不受会话提示影响，非零显式值和原始默认值都需保持。
        request.maxOutputTokens = 0;
        request.reasoningEffort.clear();
        CHECK(runAgainst(fake, request).done);
        const json defaults = body();
        CHECK(defaults.is_object());
        if (!defaults.is_object()) continue;
        CHECK(!defaults.contains("session_id") && !defaults.contains("cache_control") &&
              !defaults.contains("prompt_cache_key"));
        CHECK(!defaults.contains("max_output_tokens") && !defaults.contains("reasoning"));
        request.sessionId = "session_alpha";
        CHECK(runAgainst(fake, request).done);
        CHECK(withoutHints(body()) == defaults);
    }
}

// 所有模型先验证同一输入契约，即使供应商不使用 sessionId 也不能把坏 ID 带入 HTTP 或错误文本。
void test_responses_cache_session_rejects_bad_ids_before_http() {
    const std::string marker = "synthetic-session-id-secret";
    const std::vector<std::string> badIds{
        marker + std::string(257 - marker.size(), 'x'),
        " " + marker,
        marker + " ",
        marker + "\t",
        marker + "\n",
        marker + "\r",
        marker + std::string(1, '\0'),
        marker + std::string(1, '\x1f'),
        marker + std::string(1, '\x7f'),
        marker + "\xC3\xA9",
        marker + "\xE7\x95\x8C",
    };
    for (const char* model :
         {"anthropic/claude-fable-5.2", "glm-5.4", "glm-5.3-flash", "deepseek-flash",
          "deepseek-v4-pro", "unknown-fixture", "local:synthetic"}) {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.completed"},
                               {"response", {{"status", "completed"}, {"output", json::array()}}}});
        fake.start();
        for (const auto& id : badIds) {
            MaiModelRequest request;
            request.model = model;
            request.sessionId = id;
            const Collected answer = runAgainst(fake, request);
            CHECK(!answer.done);
            CHECK(answer.code == MaiErrorCode::InvalidInput);
            CHECK(fake.requestCount.load() == 0);
            CHECK(answer.error.find(marker) == std::string::npos);
            CHECK(answer.text.empty() && answer.reasoning.empty());
            CHECK(answer.calls.empty() && answer.continuations.empty());
        }
    }
}

// 免费主模型只使用精确免费 ID；目录能力不能靠前缀猜测或沿用付费 Fable 五档。
void test_particle_free_primary_catalog_uses_the_exact_free_model() {
    const std::string model = "particle-ai/particle-1:free";
    const auto info = maiCloudModelInfo(model);
    CHECK(info.has_value());
    if (info) {
        CHECK(info->model == model);
        CHECK(info->provider == "openrouter");
        CHECK(info->wire == MaiWireApi::Responses);
        CHECK(info->supportsImages);
        CHECK(info->supportsReasoningEffort);
        CHECK(info->contextLimits.contextWindowTokens == 1048576);
        CHECK(info->contextLimits.maxOutputTokens == 131072);
    }
    CHECK(!maiCloudModelInfo("particle-ai/particle-1"));
    CHECK(!maiCloudModelInfo("particle-ai/particle-1:free-extra"));
    const auto limits = maiOfficialModelContextLimits("https://openrouter.ai/api/v1", model);
    CHECK(limits.contextWindowTokens == 1048576);
    CHECK(limits.maxOutputTokens == 131072);
    const auto custom = maiOfficialModelContextLimits("https://synthetic.invalid/v1", model);
    CHECK(custom.contextWindowTokens == 0 && custom.maxOutputTokens == 0);
    CHECK(maiReasoningEffortsForModel(model) == std::vector<std::string>({"low", "high", "max"}));
    for (const char* effort : {"", "low", "high", "max"})
        CHECK(maiIsValidReasoningEffortForModel(model, effort));
    for (const char* effort : {"medium", "xhigh", "none", "invalid"})
        CHECK(!maiIsValidReasoningEffortForModel(model, effort));
}

// 即使普通调用没有 sessionId，免费请求仍须带完整零价约束并显式关闭增值插件。
// 缓存路由只携带现有 ID；Claude 专用断点不能顺带发给另一供应商的模型。
void test_particle_free_primary_wire_keeps_zero_price_guards_without_a_session() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "particle-ai/particle-1:free";
    request.reasoningEffort = "low";
    request.maxOutputTokens = 321;
    request.baseInstructions = "Retain the ordinary primary model request.";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Owned free primary check";
    request.messages.push_back(std::move(user));
    request.tools = {{"z_owned", "Owned no-effect tool", R"({"type":"object"})"},
                     {"a_owned", "Another owned no-effect tool", R"({"type":"object"})"}};
    const json expectedProvider{
        {"max_price", {{"prompt", 0}, {"completion", 0}, {"request", 0}, {"image", 0}}},
        {"allow_fallbacks", false}};
    json first;
    for (const std::string& id : {std::string{}, std::string{"owned_free_session"}}) {
        request.sessionId = id;
        const Collected answer = runAgainst(fake, request);
        CHECK(answer.done);
        json body;
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            body = json::parse(fake.lastBody, nullptr, false);
        }
        CHECK(body.is_object());
        if (!body.is_object()) continue;
        CHECK(wireValue(body, "/model") == request.model);
        CHECK(wireValue(body, "/provider") == expectedProvider);
        for (const char* name : {"prompt", "completion", "request", "image"}) {
            const std::string path = std::string{"/provider/max_price/"} + name;
            const auto value = wireValue(body, path.c_str());
            CHECK(value.is_number());
            CHECK(value == 0);
        }
        CHECK(wireValue(body, "/plugins") == json::array());
        CHECK(!body.contains("cache_control"));
        CHECK(!body.contains("prompt_cache_key"));
        CHECK(wireValue(body, "/reasoning/effort") == "low");
        CHECK(wireValue(body, "/max_output_tokens") == 321);
        CHECK(wireValue(body, "/store") == false);
        CHECK(wireValue(body, "/instructions") == request.baseInstructions);
        CHECK(wireValue(body, "/tools/0/name") == "z_owned");
        CHECK(wireValue(body, "/tools/1/name") == "a_owned");
        if (id.empty()) {
            CHECK(!body.contains("session_id"));
            first = body;
        } else {
            CHECK(wireValue(body, "/session_id") == id);
            body.erase("session_id");
            CHECK(body == first);
        }
    }
    CHECK(fake.requestCount.load() == 2);
}

// 增加免费型号不能改变旧供应商字段；Fable 的 Claude 缓存规则仍按原空/非空身份工作。
void test_particle_free_primary_does_not_change_existing_provider_fields() {
    for (const char* model :
         {"anthropic/claude-fable-5.2", "glm-5.4", "deepseek-flash", "unknown-fixture"}) {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.completed"},
                               {"response", {{"status", "completed"}, {"output", json::array()}}}});
        fake.start();
        MaiModelRequest request;
        request.model = model;
        for (const std::string& id : {std::string{}, std::string{"owned_legacy_session"}}) {
            request.sessionId = id;
            CHECK(runAgainst(fake, request).done);
            std::lock_guard<std::mutex> lock(fake.mutex);
            const json body = json::parse(fake.lastBody, nullptr, false);
            CHECK(body.is_object());
            if (!body.is_object()) continue;
            CHECK(!body.contains("provider"));
            CHECK(!body.contains("plugins"));
            if (std::string(model) == "anthropic/claude-fable-5.2" && !id.empty()) {
                CHECK(wireValue(body, "/session_id") == id);
                CHECK(wireValue(body, "/cache_control") == json({{"type", "ephemeral"}}));
                CHECK(!body.contains("prompt_cache_key"));
            } else if (std::string(model) == "glm-5.4" && !id.empty()) {
                CHECK(wireValue(body, "/prompt_cache_key") == id);
                CHECK(!body.contains("session_id"));
                CHECK(!body.contains("cache_control"));
            } else {
                CHECK(!body.contains("session_id"));
                CHECK(!body.contains("cache_control"));
                CHECK(!body.contains("prompt_cache_key"));
            }
        }
    }
}

// 免费路由的临时限流可按预算重试同一型号；不得切付费型号或读取另一条路由凭据。
void test_particle_free_primary_quota_errors_never_fall_back_to_paid_routes() {
    const std::string freeModel = "particle-ai/particle-1:free";
    for (const int status : {429, 402}) {
        FakeServer free, paid;
        free.status = status;
        free.script =
            json{{"error", {{"code", status}, {"message", "Synthetic free quota rejection."}}}}
                .dump();
        paid.script = sse(json{{"type", "response.completed"},
                               {"response", {{"status", "completed"}, {"output", json::array()}}}});
        free.start();
        paid.start();
        unsigned freeKeyReads = 0, paidKeyReads = 0;
        MaiModelRoute freeRoute;
        freeRoute.modelNames = {freeModel};
        freeRoute.config.baseUrl = free.base();
        freeRoute.config.wire = MaiWireApi::Responses;
        freeRoute.config.maxRetries = 2;
        freeRoute.config.retryInitialDelayMs = 1;
        freeRoute.apiKeyProvider = [&] {
            ++freeKeyReads;
            return std::string{"synthetic-free-route-key"};
        };
        MaiModelRoute paidRoute;
        paidRoute.modelNames = {"anthropic/claude-fable-5.2"};
        paidRoute.config.baseUrl = paid.base();
        paidRoute.config.wire = MaiWireApi::Responses;
        paidRoute.apiKeyProvider = [&] {
            ++paidKeyReads;
            return std::string{"synthetic-paid-route-key"};
        };
        MaiRoutedModelConfig routes;
        routes.routes = {freeRoute, paidRoute};
        auto client = makeMaiRoutedModelClient(std::move(routes));
        MaiModelRequest request;
        request.model = freeModel;
        request.reasoningEffort = "low";
        request.sessionId = "owned_free_failure_session";
        unsigned textEvents = 0, reasoningEvents = 0, toolEvents = 0, stateEvents = 0;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view) { ++textEvents; };
        sink.onReasoning = [&](std::string_view) { ++reasoningEvents; };
        sink.onToolCall = [&](const MaiToolInvocation&) { ++toolEvents; };
        sink.onContinuationState = [&](const MaiModelContinuationState&) { ++stateEvents; };
        const std::atomic<bool> cancel{false};
        const MaiError error = client->stream(request, sink, cancel);
        CHECK(error.code() == (status == 429 ? MaiErrorCode::RateLimited : MaiErrorCode::Protocol));
        CHECK(error.message().rfind("HTTP " + std::to_string(status), 0) == 0);
        CHECK(free.requestCount.load() == (status == 429 ? 3 : 1));
        CHECK(paid.requestCount.load() == 0);
        CHECK(freeKeyReads == 1);
        CHECK(paidKeyReads == 0);
        CHECK(textEvents == 0 && reasoningEvents == 0 && toolEvents == 0 && stateEvents == 0);
        std::lock_guard<std::mutex> lock(free.mutex);
        const auto body = json::parse(free.lastBody, nullptr, false);
        CHECK(wireValue(body, "/model") == freeModel);
        CHECK(free.lastAuthorization == "Bearer synthetic-free-route-key");
    }
}

// Fable 五档是模型能力，不能用旧全局三档列表把 medium/xhigh 静默省略或映射成其它值。
void test_fable_responses_preserves_all_five_reasoning_efforts() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    MaiModelRequest request;
    request.model = "anthropic/claude-fable-5.2";
    for (const char* effort : {"low", "medium", "high", "xhigh", "max"}) {
        request.reasoningEffort = effort;
        CHECK(runAgainst(fake, request).done);
        std::lock_guard<std::mutex> lock(fake.mutex);
        const json body = json::parse(fake.lastBody);
        CHECK(wireValue(body, "/reasoning/effort") == effort);
    }
    request.reasoningEffort.clear();
    CHECK(runAgainst(fake, request).done);
    std::lock_guard<std::mutex> lock(fake.mutex);
    CHECK(!json::parse(fake.lastBody).contains("reasoning"));
}

// 显式不支持的档位必须在任何 HTTP 和流式回调之前拒绝；不能静默省略后使用供应商默认。
void test_responses_rejects_unsupported_model_efforts_before_http() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    for (const char* model : {"glm-5.4", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro",
                              "local:synthetic", "unknown-fixture"}) {
        for (const char* effort : {"medium", "xhigh"}) {
            MaiModelRequest request;
            request.model = model;
            request.reasoningEffort = effort;
            const Collected answer = runAgainst(fake, request);
            CHECK(!answer.done);
            CHECK(answer.code == MaiErrorCode::InvalidInput);
            CHECK(answer.text.empty() && answer.reasoning.empty());
            CHECK(answer.calls.empty() && answer.continuations.empty());
            CHECK(fake.requestCount.load() == 0);
        }
    }
    for (const char* effort : {"minimal", "none", "ultra"}) {
        MaiModelRequest request;
        request.model = "anthropic/claude-fable-5.2";
        request.reasoningEffort = effort;
        const Collected answer = runAgainst(fake, request);
        CHECK(!answer.done);
        CHECK(answer.code == MaiErrorCode::InvalidInput);
        CHECK(answer.calls.empty() && answer.continuations.empty());
        CHECK(fake.requestCount.load() == 0);
    }
}

// 官方型号矩阵统一使用 Responses reasoning.effort，未知型号不擅自添加供应商扩展。
void test_cloud_effort_levels_reach_responses() {
    FakeServer fake;
    fake.script = sse(json{{"type", "response.completed"},
                           {"response", {{"status", "completed"}, {"output", json::array()}}}});
    fake.start();
    for (const char* model : {"deepseek-flash", "deepseek-v4-pro", "glm-5.4", "glm-5.3-flash",
                              "anthropic/claude-fable-5.2"}) {
        for (const char* effort : {"low", "high", "max"}) {
            MaiModelRequest request;
            request.model = model;
            request.reasoningEffort = effort;
            MaiModelMessage user;
            user.role = MaiModelRole::User;
            user.content = "Check effort";
            request.messages.push_back(std::move(user));
            CHECK(runAgainst(fake, request).done);
            json body;
            {
                std::lock_guard<std::mutex> lock(fake.mutex);
                body = json::parse(fake.lastBody);
            }
            CHECK(body["model"] == model);
            CHECK(body["reasoning"]["effort"] == effort);
        }
    }
    MaiModelRequest unknown;
    unknown.model = "unrelated-fixture-model";
    unknown.reasoningEffort = "max";
    CHECK(runAgainst(fake, unknown).done);
    std::lock_guard<std::mutex> lock(fake.mutex);
    const auto body = json::parse(fake.lastBody);
    CHECK(!body.contains("reasoning"));
    CHECK(!body.contains("reasoning_effort"));
    CHECK(!body.contains("thinking"));
}

// 三个供应商均使用 Responses；账号凭据不可交叉，也不因鉴权错误回退到其它供应商。
void test_routed_models_keep_wire_key_and_failure_isolation() {
    FakeServer glm, deepseek, openrouter;
    glm.script = deepseek.script = openrouter.script =
        sse(json{{"type", "response.completed"},
                 {"response", {{"status", "completed"}, {"output", json::array()}}}});
    glm.start();
    deepseek.start();
    openrouter.start();
    int glmKeyReads = 0, deepseekKeyReads = 0, openrouterKeyReads = 0;
    std::string glmKey = "synthetic-glm-key", deepseekKey = "synthetic-deepseek-key";
    MaiModelRoute glmRoute;
    glmRoute.modelNames = {"glm-5.4", "glm-5.3-flash"};
    glmRoute.config.baseUrl = glm.base();
    glmRoute.config.wire = MaiWireApi::Responses;
    glmRoute.apiKeyProvider = [&] {
        ++glmKeyReads;
        return glmKey;
    };
    MaiModelRoute deepseekRoute;
    deepseekRoute.modelNames = {"deepseek-flash", "deepseek-v4-pro"};
    deepseekRoute.config.baseUrl = deepseek.base();
    deepseekRoute.config.wire = MaiWireApi::Responses;
    deepseekRoute.config.maxRetries = 0;
    deepseekRoute.apiKeyProvider = [&] {
        ++deepseekKeyReads;
        return deepseekKey;
    };
    MaiModelRoute openrouterRoute;
    openrouterRoute.modelNames = {"anthropic/claude-fable-5.2"};
    openrouterRoute.config.baseUrl = openrouter.base();
    openrouterRoute.config.wire = MaiWireApi::Responses;
    openrouterRoute.config.maxRetries = 0;
    openrouterRoute.apiKeyProvider = [&] {
        ++openrouterKeyReads;
        return "synthetic-openrouter-key";
    };
    auto client = makeMaiRoutedModelClient({{glmRoute, deepseekRoute, openrouterRoute}});
    CHECK(client->wireApi() == MaiWireApi::Responses);
    CHECK(client->wireApiForModel("glm-5.4") == MaiWireApi::Responses);
    CHECK(client->wireApiForModel("deepseek-v4-pro") == MaiWireApi::Responses);
    CHECK(client->wireApiForModel("anthropic/claude-fable-5.2") == MaiWireApi::Responses);
    CHECK(!client->wireApiForModel("glm-unknown"));
    CHECK(glmKeyReads == 0 && deepseekKeyReads == 0 && openrouterKeyReads == 0);
    std::atomic<bool> cancel{false};
    MaiModelRequest request;
    request.reasoningEffort = "low";
    MaiModelMessage assistant;
    assistant.role = MaiModelRole::Assistant;
    assistant.content = "Tool status";
    assistant.reasoning = "Opaque reasoning\r\n\xE7\x95\x8C";
    assistant.invocations = {{"raw_reasoning_call", "lookup", "{}"}};
    request.messages.push_back(assistant);
    MaiModelMessage output;
    output.role = MaiModelRole::ToolResult;
    output.toolCallId = "raw_reasoning_call";
    output.content = "Result";
    request.messages.push_back(output);
    for (const char* model : {"glm-5.4", "deepseek-v4-pro", "glm-5.3-flash", "deepseek-flash",
                              "anthropic/claude-fable-5.2"}) {
        request.model = model;
        CHECK(!client->stream(request, {}, cancel));
    }
    CHECK(glmKeyReads == 2 && deepseekKeyReads == 2);
    CHECK(glm.requestCount == 2 && deepseek.requestCount == 2);
    CHECK(openrouterKeyReads == 1 && openrouter.requestCount == 1);
    {
        std::lock_guard<std::mutex> lock(glm.mutex);
        CHECK(glm.lastAuthorization == "Bearer synthetic-glm-key");
        const auto body = json::parse(glm.lastBody);
        CHECK(body["reasoning"]["effort"] == "low" && body.contains("input") &&
              !body.contains("messages"));
        CHECK(body["input"][0]["type"] == "reasoning");
        CHECK(body["input"][0]["content"][0]["text"] == assistant.reasoning);
        CHECK(body["input"][1]["content"] == "Tool status");
        CHECK(body["input"][2]["call_id"] == "raw_reasoning_call");
        CHECK(body["input"][3]["call_id"] == "raw_reasoning_call");
    }
    {
        std::lock_guard<std::mutex> lock(openrouter.mutex);
        CHECK(openrouter.lastAuthorization == "Bearer synthetic-openrouter-key");
        const auto body = json::parse(openrouter.lastBody);
        CHECK(wireValue(body, "/reasoning/effort") == "low");
        CHECK(wireValue(body, "/input/0/type") == "message");
        CHECK(wireValue(body, "/input/1/call_id") == "raw_reasoning_call");
        CHECK(wireValue(body, "/input/2/call_id") == "raw_reasoning_call");
        CHECK(wireValue(body, "/store") == false);
    }
    {
        std::lock_guard<std::mutex> lock(deepseek.mutex);
        CHECK(deepseek.lastAuthorization == "Bearer synthetic-deepseek-key");
        const auto body = json::parse(deepseek.lastBody);
        CHECK(body["reasoning"]["effort"] == "low" && body.contains("input") &&
              !body.contains("messages"));
    }
    request.model = "not-in-directory";
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::InvalidInput);
    CHECK(glmKeyReads == 2 && deepseekKeyReads == 2);
    CHECK(openrouterKeyReads == 1 && openrouter.requestCount == 1);
    glmKey = "rotated-glm-key";
    request.model = "glm-5.4";
    CHECK(!client->stream(request, {}, cancel));
    {
        std::lock_guard<std::mutex> lock(glm.mutex);
        CHECK(glm.lastAuthorization == "Bearer rotated-glm-key");
    }
    glm.status = 401;
    CHECK(client->stream(request, {}, cancel).hasError());
    CHECK(deepseekKeyReads == 2 && deepseek.requestCount == 2);
    CHECK(openrouterKeyReads == 1 && openrouter.requestCount == 1);
    glmKey.clear();
    const auto glmRequests = glm.requestCount.load();
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::NotConfigured);
    CHECK(glm.requestCount == glmRequests && deepseek.requestCount == 2);
    auto invalid = makeMaiRoutedModelClient({{glmRoute, glmRoute}});
    CHECK(!invalid->wireApiForModel("glm-5.4"));
    CHECK(invalid->stream(request, {}, cancel).code() == MaiErrorCode::InvalidInput);
    cancel = true;
    CHECK(client->stream(request, {}, cancel).code() == MaiErrorCode::Canceled);
}

void test_responses_needs_terminal_event_and_reports_incomplete() {
    MaiModelRequest request;
    request.model = "deepseek-flash";
    MaiModelMessage user;
    user.role = MaiModelRole::User;
    user.content = "Hello";
    request.messages.push_back(std::move(user));
    {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.output_text.delta"}, {"delta", "partial"}});
        fake.start();
        const Collected result = runAgainst(fake, request);
        CHECK(!result.done);
        CHECK(result.text == "partial");
        CHECK(result.error.find("without a terminal event") != std::string::npos);
    }
    {
        FakeServer fake;
        fake.script = sse(json{{"type", "response.incomplete"},
                               {"response",
                                {{"output", json::array()},
                                 {"incomplete_details", {{"reason", "max_output_tokens"}}}}}});
        fake.start();
        const Collected result = runAgainst(fake, request);
        CHECK(!result.done);
        CHECK(result.error.find("max_output_tokens") != std::string::npos);
    }
    {
        FakeServer fake;
        fake.script = sse(
            json{{"type", "response.failed"},
                 {"response",
                  {{"output", json::array()},
                   {"error", {{"code", "provider_error"}, {"message", "generation failed"}}}}}});
        fake.start();
        const Collected result = runAgainst(fake, request);
        CHECK(!result.done);
        CHECK(result.error.find("generation failed") != std::string::npos);
    }
}

// 通过本地 HTTP 服务观察代理收到的真实请求，目标域名只用于区分重定向来源。
struct ProxyRequest {
    std::string method;
    std::string target;
    std::string host;
    std::string authorization;
    std::string proxyAuthorization;
    std::string body;
};

struct FakeModelProxy {
    httplib::Server server;
    std::thread thread;
    std::mutex mutex;
    std::vector<ProxyRequest> requests;
    std::string redirectLocation;
    int redirectStatus = 0;
    int terminalStatus = 200;
    int delayMs = 0;
    int port = 0;

    void start() {
        auto handle = [this](const httplib::Request& request, httplib::Response& response) {
            std::size_t count;
            {
                std::lock_guard<std::mutex> lock(mutex);
                requests.push_back({request.method, request.target,
                                    request.get_header_value("Host"),
                                    request.get_header_value("Authorization"),
                                    request.get_header_value("Proxy-Authorization"), request.body});
                count = requests.size();
            }
            if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (redirectStatus != 0 && count == 1) {
                response.status = redirectStatus;
                response.set_header("Location", redirectLocation);
                return;
            }
            response.status = terminalStatus;
            response.set_content(kCompleted, "text/event-stream");
        };
        server.Post(".*", handle);
        server.Get(".*", handle);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ~FakeModelProxy() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    std::string address() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    std::vector<ProxyRequest> received() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }
};

// 测试中临时设置环境代理，结束后恢复，避免依赖开发机器的配置或影响后续测试。
class ScopedProxyEnvironment {
public:
    ScopedProxyEnvironment(const char* name, const char* value) : mName(name) {
        const char* previous = std::getenv(name);
        if (previous) {
            mHadValue = true;
            mPrevious = previous;
        }
        set(value);
    }

    ~ScopedProxyEnvironment() {
        if (mHadValue) {
            set(mPrevious.c_str());
        } else {
#if defined(_WIN32)
            _putenv_s(mName.c_str(), "");
#else
            unsetenv(mName.c_str());
#endif
        }
    }

private:
    void set(const char* value) {
#if defined(_WIN32)
        _putenv_s(mName.c_str(), value);
#else
        setenv(mName.c_str(), value, 1);
#endif
    }

    std::string mName;
    std::string mPrevious;
    bool mHadValue = false;
};

void test_responses_proxy_redirects() {
    // NO_PROXY 覆盖了环境变量匹配目标的情况，系统代理选择仍应生效。
    ScopedProxyEnvironment noProxy("no_proxy", "*");
    ScopedProxyEnvironment upperNoProxy("NO_PROXY", "*");
    for (const int status : {302, 307}) {
        FakeModelProxy first;
        FakeModelProxy second;
        first.redirectStatus = status;
        first.redirectLocation = "http://second.test/final";
        first.start();
        second.start();
        std::vector<std::string> resolved;
        MaiModelConfig config;
        config.baseUrl = "http://first.test";
        config.apiKey = "test-key";
        config.wire = MaiWireApi::Responses;
        config.maxRetries = 0;
        config.proxyResolver = [&](const std::string& url, MaiModelProxy& proxy) {
            resolved.push_back(url);
            proxy.url =
                url.find("first.test") != std::string::npos ? first.address() : second.address();
            proxy.username = url.find("first.test") != std::string::npos ? "alpha" : "bravo";
            proxy.password = url.find("first.test") != std::string::npos ? "one" : "two";
            return true;
        };
        MaiModelRequest request;
        request.model = "test-model";
        const std::atomic<bool> cancel{false};
        MaiStreamSink sink;
        const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
        CHECK(!error);
        const auto firstRequests = first.received();
        const auto secondRequests = second.received();
        CHECK(resolved.size() == 2);
        CHECK(firstRequests.size() == 1);
        CHECK(secondRequests.size() == 1);
        if (firstRequests.size() == 1 && secondRequests.size() == 1) {
            CHECK(firstRequests[0].method == "POST");
            CHECK(firstRequests[0].target == "http://first.test/responses");
            CHECK(firstRequests[0].host == "first.test");
            CHECK(firstRequests[0].authorization == "Bearer test-key");
            CHECK(firstRequests[0].proxyAuthorization == "Basic YWxwaGE6b25l");
            CHECK(secondRequests[0].target == "http://second.test/final");
            CHECK(secondRequests[0].host == "second.test");
            CHECK(secondRequests[0].authorization.empty());
            CHECK(secondRequests[0].proxyAuthorization == "Basic YnJhdm86dHdv");
            CHECK(secondRequests[0].method == (status == 302 ? "GET" : "POST"));
            CHECK(status == 302 ? secondRequests[0].body.empty()
                                : secondRequests[0].body == firstRequests[0].body);
        }
    }
}

void test_responses_proxy_same_origin_redirect() {
    ScopedProxyEnvironment noProxy("no_proxy", "*");
    ScopedProxyEnvironment upperNoProxy("NO_PROXY", "*");
    FakeModelProxy proxy;
    proxy.redirectStatus = 307;
    proxy.redirectLocation = "/final";
    proxy.start();
    MaiModelConfig config;
    config.baseUrl = "http://first.test";
    config.apiKey = "test-key";
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    config.proxyResolver = [&](const std::string&, MaiModelProxy& route) {
        route.url = proxy.address();
        return true;
    };
    MaiModelRequest request;
    request.model = "test-model";
    const std::atomic<bool> cancel{false};
    MaiStreamSink sink;
    const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
    CHECK(!error);
    const auto received = proxy.received();
    CHECK(received.size() == 2);
    if (received.size() == 2) {
        CHECK(received[1].target == "http://first.test/final");
        CHECK(received[1].authorization == "Bearer test-key");
        CHECK(received[1].method == "POST");
        CHECK(received[1].body == received[0].body);
    }
}

void test_responses_proxy_redirect_total_timeout() {
    ScopedProxyEnvironment noProxy("no_proxy", "*");
    ScopedProxyEnvironment upperNoProxy("NO_PROXY", "*");
    FakeModelProxy first;
    FakeModelProxy second;
    first.redirectStatus = 307;
    first.redirectLocation = "http://second.test/final";
    first.delayMs = 1300;
    second.delayMs = 1300;
    first.start();
    second.start();
    MaiModelConfig config;
    config.baseUrl = "http://first.test";
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    config.totalTimeoutSeconds = 2;
    config.proxyResolver = [&](const std::string& url, MaiModelProxy& route) {
        route.url =
            url.find("first.test") != std::string::npos ? first.address() : second.address();
        return true;
    };
    MaiModelRequest request;
    request.model = "test-model";
    const std::atomic<bool> cancel{false};
    MaiStreamSink sink;
    const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
    // 每跳均小于两秒，但整条链超过两秒；重置计时会错误地得到成功结果。
    CHECK(error.code() == MaiErrorCode::Network);
    CHECK(error.message().find("Timeout") != std::string::npos);
    CHECK(first.received().size() == 1);
    CHECK(second.received().size() == 1);
}

void test_responses_without_location_retains_body() {
    ScopedProxyEnvironment noProxy("no_proxy", "*");
    ScopedProxyEnvironment upperNoProxy("NO_PROXY", "*");
    FakeModelProxy destination;
    destination.terminalStatus = 300;
    destination.start();
    MaiModelConfig config;
    config.baseUrl = destination.address();
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    CHECK(!makeMaiModelClient(config)->stream(request, sink, cancel));
    config.proxyResolver = [](const std::string&, MaiModelProxy&) { return true; };
    CHECK(!makeMaiModelClient(config)->stream(request, sink, cancel));
    CHECK(destination.received().size() == 2);
}

void test_responses_proxy_resolver_timeout_retries_before_output() {
    FakeModelProxy destination;
    destination.start();
    int resolutions = 0;
    MaiModelConfig config;
    config.baseUrl = destination.address();
    config.wire = MaiWireApi::Responses;
    config.totalTimeoutSeconds = 1;
    config.maxRetries = 1;
    config.retryInitialDelayMs = 0;
    config.proxyResolver = [&](const std::string&, MaiModelProxy&) {
        if (++resolutions == 1) std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        return true;
    };
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
    // 首次选路耗尽期限后仍应走原有重试；只有第二次真的发送模型请求。
    CHECK(!error);
    CHECK(resolutions == 2);
    CHECK(destination.received().size() == 1);
}

#if defined(MAI_MODEL_CLIENT_TLS_TESTS)
void test_responses_proxy_rejects_tls_downgrade() {
    const std::string certificateRoot =
        std::string(MAI_AGENT_SOURCE_DIR) + "/third_party/curl/tests/certs/";
    const std::string certificate = certificateRoot + "Server-localhost-sv.crt";
    const std::string privateKey = certificateRoot + "Server-localhost-sv.key";
    const std::string authority = certificateRoot + "EdelCurlRoot-ca.crt";
    httplib::SSLServer secure(certificate.c_str(), privateKey.c_str());
    CHECK(secure.is_valid());
    if (!secure.is_valid()) return;
    FakeModelProxy plain;
    plain.start();
    std::mutex mutex;
    std::vector<ProxyRequest> secureRequests;
    std::atomic<int> redirectStatus{302};
    secure.Post("/responses", [&](const httplib::Request& request, httplib::Response& response) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            secureRequests.push_back(
                {request.method, request.target, request.get_header_value("Host"),
                 request.get_header_value("Authorization"),
                 request.get_header_value("Proxy-Authorization"), request.body});
        }
        response.status = redirectStatus.load();
        response.set_header("Location", plain.address() + "/final");
    });
    const int securePort = secure.bind_to_any_port("127.0.0.1");
    CHECK(securePort > 0);
    if (securePort <= 0) return;
    std::thread secureThread([&] { secure.listen_after_bind(); });
    for (int i = 0; i < 200 && !secure.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    MaiModelConfig config;
    config.baseUrl = "https://localhost:" + std::to_string(securePort);
    config.caBundlePath = authority;
    config.apiKey = "test-key";
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    std::vector<std::string> resolved;
    config.proxyResolver = [&](const std::string& url, MaiModelProxy&) {
        resolved.push_back(url);
        return true;
    };
    MaiModelRequest request;
    request.model = "test-model";
    MaiStreamSink sink;
    const std::atomic<bool> cancel{false};
    // 302 会切换为 GET，307 会重发正文；两者都不能向明文目标发出请求。
    for (const int status : {302, 307}) {
        redirectStatus.store(status);
        const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
        CHECK(error.code() == MaiErrorCode::Protocol);
        CHECK(error.message().find("downgrade") != std::string::npos);
    }
    secure.stop();
    if (secureThread.joinable()) secureThread.join();
    CHECK(resolved.size() == 2);
    CHECK(plain.received().empty());
    CHECK(secureRequests.size() == 2);
    for (const auto& received : secureRequests) {
        CHECK(received.authorization == "Bearer test-key");
        CHECK(received.method == "POST");
        CHECK(!received.body.empty());
    }
}
#endif

void test_responses_proxy_direct_and_cancel() {
    FakeModelProxy destination;
    destination.start();
    // 本地监听器能证明空路由绕过环境代理，而不是仅验证回调被调用。
    ScopedProxyEnvironment noProxy("no_proxy", "");
    ScopedProxyEnvironment upperNoProxy("NO_PROXY", "");
    ScopedProxyEnvironment httpProxy("http_proxy", "http://127.0.0.1:1");
    MaiModelConfig config;
    config.baseUrl = destination.address();
    config.wire = MaiWireApi::Responses;
    config.maxRetries = 0;
    config.proxyResolver = [](const std::string&, MaiModelProxy& route) {
        route.url.clear();
        return true;
    };
    MaiModelRequest request;
    request.model = "test-model";
    std::atomic<bool> cancel{false};
    MaiStreamSink sink;
    const MaiError error = makeMaiModelClient(config)->stream(request, sink, cancel);
    CHECK(!error);
    CHECK(destination.received().size() == 1);

    config.proxyResolver = [&](const std::string&, MaiModelProxy& route) {
        route.url.clear();
        cancel.store(true);
        return true;
    };
    const MaiError canceled = makeMaiModelClient(config)->stream(request, sink, cancel);
    CHECK(canceled.code() == MaiErrorCode::Canceled);
    CHECK(destination.received().size() == 1);
}

void test_local_mode_never_falls_back_to_a_remote_request() {
    FakeModelProxy destination;
    destination.start();
    MaiModelConfig config;
    config.baseUrl = destination.address();
    config.apiKey = "synthetic-never-forwarded";
    config.wire = MaiWireApi::Local;
    auto client = makeMaiModelClient(config);
    CHECK(client != nullptr);
    CHECK(client->wireApi() == MaiWireApi::Local);
    CHECK(std::string(maiWireApiToString(client->wireApi())) == "local");
    MaiModelRequest request;
    request.model = "local:synthetic";
    bool callbackDelivered = false;
    MaiStreamSink sink;
    sink.onText = [&](std::string_view) { callbackDelivered = true; };
    sink.onToolCall = [&](const MaiToolInvocation&) { callbackDelivered = true; };
    std::atomic<bool> cancel{false};
    CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::NotConfigured);
    cancel.store(true);
    CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::Canceled);
    CHECK(!callbackDelivered);
    CHECK(destination.received().empty());
}

// 两个公开远端入口共享 Responses 默认值；目录描述值不能被当成可发送的协议。
void test_remote_factories_default_to_responses_and_reject_mixed() {
    FakeServer fake;
    fake.script = response_text_delta("default response") + kCompleted;
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    CHECK(config.wire == MaiWireApi::Responses);
    MaiModelRequest request;
    request.model = "test-model";
    std::atomic<bool> cancel{false};
    for (const bool direct : {false, true}) {
        auto client = direct ? makeMaiResponsesClient(config) : makeMaiModelClient(config);
        CHECK(client->wireApi() == MaiWireApi::Responses);
        std::string output;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view text) { output.append(text); };
        CHECK(!client->stream(request, sink, cancel));
        CHECK(output == "default response");
    }
    CHECK(fake.requestCount.load() == 2);
    config.wire = MaiWireApi::Mixed;
    for (const bool direct : {false, true}) {
        auto client = direct ? makeMaiResponsesClient(config) : makeMaiModelClient(config);
        CHECK(client->wireApi() == MaiWireApi::Mixed);
        bool delivered = false;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view) { delivered = true; };
        sink.onToolCall = [&](const MaiToolInvocation&) { delivered = true; };
        cancel.store(false);
        CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::InvalidInput);
        cancel.store(true);
        CHECK(client->stream(request, sink, cancel).code() == MaiErrorCode::Canceled);
        CHECK(!delivered);
    }
    CHECK(fake.requestCount.load() == 2);
}

// 用实际 socket 字节核对 output 上限和 usage 字段，不能把本地估计当作服务端 usage。
void test_context_limits_output_budget_and_actual_usage() {
    std::printf("   context limits, output wire budgets, and actual provider usage\n");
    {
        FakeServer fake;
        fake.chunk = 1;
        fake.script =
            response_text_delta("reply") +
            sse(json{{"type", "response.completed"},
                     {"response", {{"usage", {{"input_tokens", 1234}, {"output_tokens", 57}}}}}});
        fake.start();
        MaiModelConfig config;
        config.baseUrl = fake.base();
        config.wire = MaiWireApi::Responses;
        config.contextLimits = [](const std::string& model) {
            return model == "small-window" ? MaiModelContextLimits{8192, 2048}
                                           : MaiModelContextLimits{32768, 4096};
        };
        auto client = makeMaiModelClient(config);
        CHECK(client->contextLimits("small-window").contextWindowTokens == 8192);
        CHECK(client->contextLimits("large-window").contextWindowTokens == 32768);
        MaiModelRequest request;
        request.model = "small-window";
        request.maxOutputTokens = 256;
        MaiModelMessage user;
        user.content = "Read the preserved history.";
        request.messages.push_back(user);
        std::vector<MaiModelUsage> observed;
        MaiStreamSink sink;
        sink.onUsage = [&](const MaiModelUsage& usage) { observed.push_back(usage); };
        std::atomic<bool> cancel{false};
        CHECK(!client->stream(request, sink, cancel));
        CHECK(observed.size() == 1);
        if (!observed.empty()) {
            CHECK(observed[0].inputTokens == 1234);
            CHECK(observed[0].outputTokens == 57);
        }
        std::string body;
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            body = fake.lastBody;
        }
        const auto sent = json::parse(body);
        CHECK(sent["max_output_tokens"] == 256);
        CHECK(!sent.contains("stream_options"));
        request.maxOutputTokens = 0;
        CHECK(!client->stream(request, sink, cancel));
        {
            std::lock_guard<std::mutex> lock(fake.mutex);
            body = fake.lastBody;
        }
        const auto defaultBody = json::parse(body);
        CHECK(!defaultBody.contains("max_tokens"));
        CHECK(!defaultBody.contains("max_output_tokens"));
    }
    FakeServer absent;
    absent.script = response_text_delta("reply") + kCompleted;
    absent.start();
    MaiModelRequest request;
    request.model = "unknown";
    const auto result = runAgainst(absent, request);
    CHECK(result.done);
    CHECK(result.usage.empty());
    MaiModelConfig unknown;
    unknown.baseUrl = absent.base();
    CHECK(makeMaiModelClient(unknown)->contextLimits("glm-5.4").contextWindowTokens == 0);
}

}  // namespace

// 走真实 HTTP 和 Agent：半截正文后正常 EOF 也必须恢复，失败草稿不能进入重试请求。
void test_agent_recovers_partial_stream_without_replaying_draft(bool stalled,
                                                                bool serverError = false) {
    FakeServer fake;
    const json finalItem{
        {"type", "message"},
        {"id", "msg_recovered"},
        {"role", "assistant"},
        {"content", json::array({json{{"type", "output_text"}, {"text", "RECOVERED_FINAL"}}})}};
    fake.scripts = {sse(json{{"type", "response.output_text.delta"},
                             {"output_index", 0},
                             {"delta", "FAILED_ATTEMPT_PRIVATE_DRAFT"}}),
                    sse(json{{"type", "response.completed"},
                             {"response", {{"output", json::array({finalItem})}}}})};
    if (serverError) {
        // HTTP 200 中的明确 server_error 与 EOF 共用同一恢复所有者。
        fake.scripts[0] += sse(
            json{{"type", "response.failed"},
                 {"response", {{"error", {{"code", "server_error"}, {"message", "try again"}}}}}});
    }
    if (stalled) {
        fake.stallRequests = 1;
        fake.script = fake.scripts.front();
    }
    fake.start();
    MaiModelConfig config;
    config.baseUrl = fake.base();
    config.apiKey = "test-key";
    config.wire = MaiWireApi::Responses;
    config.inactivityTimeoutSeconds = 1;
    config.progressTimeoutSeconds = 2;
    config.maxRetries = 2;
    config.retryInitialDelayMs = 1;
    MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config));
    std::atomic<int> errors{0}, commits{0};
    const auto token = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::SessionError) ++errors;
        if (event.type == MaiEventType::MessageTextCommitted) ++commits;
    });
    const auto session = agent.submit(
        MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(), "", "deepseek-flash", ""});
    CHECK(session);
    if (!session) return;
    CHECK(agent.submit(MaiSendPrompt{session.value(), "Recover this response"}));
    agent.waitIdle();
    CHECK(fake.requestCount.load() == 2);
    CHECK(errors.load() == 0);
    CHECK(commits.load() == 1);
    const auto messages = agent.listMessages(session.value());
    CHECK(messages.size() == 2);
    if (messages.size() == 2) {
        // 本地仍保留失败时已经显示的草稿，成功尝试使用独立 part。
        CHECK(messages.back().text().find("FAILED_ATTEMPT_PRIVATE_DRAFT") != std::string::npos);
        CHECK(messages.back().text().find("RECOVERED_FINAL") != std::string::npos);
        CHECK(messages.back().parts.front().modelDraft);
        CHECK(!messages.back().parts.back().modelDraft);
    }
    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        if (fake.requestBodies.size() >= 2)
            CHECK(fake.requestBodies[1].find("FAILED_ATTEMPT_PRIVATE_DRAFT") == std::string::npos);
    }
    agent.eventBus().unsubscribe(token);
}

// 两层均有重试能力时，真实 HTTP 次数仍严格受同一预算约束；永久拒绝不重试。
void test_agent_transport_retry_budget_and_permanent_rejections() {
    for (const int status : {503, 429, 401, 402, 403}) {
        for (const bool hardQuota : {false, true}) {
            if (hardQuota && status != 429) continue;
            FakeServer fake;
            fake.transientFailures = status == 429 && !hardQuota ? 1 : 100;
            fake.transientStatus = status;
            fake.retryAfter = "0";
            if (hardQuota)
                fake.transientBody =
                    R"({"error":{"code":"insufficient_quota","message":"no credit"}})";
            fake.script = response_text_delta("Recovered after rate limit") + kCompleted;
            fake.start();
            MaiModelConfig config;
            config.baseUrl = fake.base();
            config.wire = MaiWireApi::Responses;
            config.maxRetries = 2;
            config.retryInitialDelayMs = 0;
            MaiAgent agent(makeMaiMemoryStore(), makeMaiModelClient(config));
            std::atomic<int> errors{0}, commits{0};
            const auto token = agent.eventBus().subscribe([&](const MaiEvent& event) {
                if (event.type == MaiEventType::SessionError) ++errors;
                if (event.type == MaiEventType::MessageTextCommitted) ++commits;
            });
            const auto session = agent.submit(MaiCreateSession{
                MaiFileSystem::temporaryDirectory().toUtf8(), "", "deepseek-flash", ""});
            CHECK(session);
            if (!session) continue;
            CHECK(agent.submit(MaiSendPrompt{session.value(), "Retry within budget"}));
            agent.waitIdle();
            const bool succeeds = status == 429 && !hardQuota;
            CHECK(fake.requestCount.load() == (status == 503 ? 3 : succeeds ? 2 : 1));
            CHECK(errors.load() == (succeeds ? 0 : 1));
            CHECK(commits.load() == (succeeds ? 1 : 0));
            agent.eventBus().unsubscribe(token);
        }
    }
}

// 通过服务端观察真实 TCP 来源端口，不能用客户端“命中缓存”计数代替连接复用证明。
void test_routed_clients_reuse_connections_and_retire_changed_credentials() {
    FakeServer fake;
    fake.fixedBody = true;
    fake.script = response_text_delta("pooled") + kCompleted;
    fake.start();
    std::string key = "owned-key-one";
    MaiModelRoute route;
    route.modelNames = {"fixture"};
    route.config.baseUrl = fake.base();
    // TCP 来源端口只能观察直接连接。继承的环境代理可能复用客户端连接，
    // 却为每次转发新建上游连接，使服务端端口无法证明客户端是否复用。
    route.config.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
        proxy = {};
        return true;
    };
    route.config.wire = MaiWireApi::Responses;
    route.config.maxRetries = 0;
    route.apiKeyProvider = [&] { return key; };
    MaiRoutedModelConfig routes;
    routes.routes.push_back(route);
    auto client = makeMaiRoutedModelClient(std::move(routes));
    MaiModelRequest request;
    request.model = "fixture";
    const std::atomic<bool> cancel{false};
    const auto send = [&] {
        std::string text;
        MaiStreamSink sink;
        sink.onText = [&](std::string_view chunk) { text.append(chunk); };
        const auto result = client->stream(request, sink, cancel);
        if (!result) CHECK(text == "pooled");
        return result;
    };
    CHECK(!send());
    CHECK(!send());
    key = "owned-key-two";
    CHECK(!send());
    key.clear();
    CHECK(send().code() == MaiErrorCode::NotConfigured);
    key = "owned-key-two";
    CHECK(!send());
    std::lock_guard<std::mutex> lock(fake.mutex);
    CHECK(fake.clientPorts.size() == 4);
    if (fake.clientPorts.size() == 4) {
        CHECK(fake.clientPorts[0] == fake.clientPorts[1]);
        CHECK(fake.clientPorts[1] != fake.clientPorts[2]);
        CHECK(fake.clientPorts[2] != fake.clientPorts[3]);
    }
    CHECK(fake.lastAuthorization == "Bearer owned-key-two");
}
void test_context_length_failure_is_typed_without_network_retry() {
    FakeServer fake;
    fake.status = 400;
    fake.script = R"({"error":{"code":"context_length_exceeded","message":"owned context limit"}})";
    fake.start();
    MaiModelRequest request;
    request.model = "fixture";
    const auto result = runAgainst(fake, request);
    CHECK(result.code == MaiErrorCode::ContextWindowExceeded);
    CHECK(fake.requestCount.load() == 1);
}

void test_stream_recovery() {
    test_routed_clients_reuse_connections_and_retire_changed_credentials();
    test_context_length_failure_is_typed_without_network_retry();
    test_agent_recovers_partial_stream_without_replaying_draft(false);
    test_agent_recovers_partial_stream_without_replaying_draft(true);
    test_agent_recovers_partial_stream_without_replaying_draft(false, true);
    test_agent_transport_retry_budget_and_permanent_rejections();
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--opus-55") {
        test_opus_55_catalog_and_responses_request();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--image-preview") {
        test_large_historical_image_uses_cached_bounded_preview();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--stream-recovery") {
        test_stream_recovery();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--free-primary") {
        test_particle_free_primary_catalog_uses_the_exact_free_model();
        test_particle_free_primary_wire_keeps_zero_price_guards_without_a_session();
        test_particle_free_primary_does_not_change_existing_provider_fields();
        test_particle_free_primary_quota_errors_never_fall_back_to_paid_routes();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--cache-session") {
        test_responses_cache_session_hints_preserve_the_original_request();
        test_responses_cache_session_rejects_bad_ids_before_http();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--openrouter-402") {
        test_openrouter_402_classifies_in_flight_budget_without_retry();
        test_openrouter_402_limits_do_not_infer_missing_or_foreign_metadata();
        test_openrouter_402_retry_after_only_accepts_bounded_numeric_seconds();
        test_openrouter_402_truncated_reset_response_is_not_retried();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--reasoning-efforts") {
        test_fable_responses_preserves_all_five_reasoning_efforts();
        test_responses_rejects_unsupported_model_efforts_before_http();
        test_cloud_effort_levels_reach_responses();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--continuation-completeness") {
        test_responses_explicit_empty_terminal_does_not_recover_old_tool();
        test_responses_item_snapshot_fallback_rejects_unfinished_visible_item();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--foreign-continuation") {
        test_responses_foreign_continuation_uses_only_ordinary_history();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--cloud-responses") {
        test_responses_continuation_replays_original_output_before_tool_results();
        test_responses_continuation_uses_completed_items_without_terminal_output();
        test_responses_explicit_empty_terminal_does_not_recover_old_tool();
        test_responses_item_snapshot_fallback_rejects_unfinished_visible_item();
        test_responses_never_publishes_continuation_or_tools_from_unsuccessful_output();
        test_responses_cancel_during_state_callback_prevents_tool_delivery();
        test_responses_continuation_checks_source_and_rejects_corruption();
        test_responses_foreign_continuation_uses_only_ordinary_history();
        test_glm_responses_completes_without_done_and_keeps_metadata_only_state();
        test_fable_responses_preserves_all_five_reasoning_efforts();
        test_responses_rejects_unsupported_model_efforts_before_http();
        test_cloud_effort_levels_reach_responses();
        test_routed_models_keep_wire_key_and_failure_isolation();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--responses-proxy") {
        test_responses_proxy_redirects();
        test_responses_proxy_same_origin_redirect();
        test_responses_proxy_redirect_total_timeout();
        test_responses_without_location_retains_body();
        test_responses_proxy_resolver_timeout_retries_before_output();
#if defined(MAI_MODEL_CLIENT_TLS_TESTS)
        test_responses_proxy_rejects_tls_downgrade();
#endif
        test_responses_proxy_direct_and_cancel();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--session-models") {
        test_cloud_effort_levels_reach_responses();
        test_routed_models_keep_wire_key_and_failure_isolation();
        return failures == 0 ? 0 : 1;
    }
#if defined(_WIN32)
    if (std::getenv("MAI_MODEL_LIVE_TLS_TEST") != nullptr) {
        MaiModelConfig config;
        config.baseUrl = "https://open.bigmodel.cn/api/v1";
        config.wire = MaiWireApi::Responses;
        config.maxRetries = 0;
        config.connectTimeoutSeconds = 10;
        config.totalTimeoutSeconds = 15;
        MaiModelRequest request;
        request.model = "glm-5.3-flash";
        MaiModelMessage user;
        user.role = MaiModelRole::User;
        user.content = "unauthenticated TLS probe";
        request.messages.push_back(std::move(user));
        const std::atomic<bool> cancel{false};
        MaiStreamSink sink;
        auto client = makeMaiModelClient(config);
        const MaiError error = client->stream(request, sink, cancel);
        std::printf("live model TLS result: code=%d message=%s\n", int(error.code()),
                    error.message().c_str());
        CHECK(error.code() != MaiErrorCode::Network);
    }
#endif
    test_context_limits_output_budget_and_actual_usage();
    test_particle_free_primary_catalog_uses_the_exact_free_model();
    test_particle_free_primary_wire_keeps_zero_price_guards_without_a_session();
    test_particle_free_primary_does_not_change_existing_provider_fields();
    test_particle_free_primary_quota_errors_never_fall_back_to_paid_routes();
    test_stream_recovery();
    test_text_stream_one_byte_at_a_time();
    test_text_stream_all_at_once();
    test_tool_call_fragments();
    test_function_argument_deltas_extend_progress_without_early_delivery();
    test_two_tool_calls_interleaved();
    test_malformed_lines_are_ignored();
    test_server_error_inside_stream();
    test_reasoning_delta();
    test_http_error_preserves_provider_message();
    test_openrouter_402_classifies_in_flight_budget_without_retry();
    test_openrouter_402_limits_do_not_infer_missing_or_foreign_metadata();
    test_openrouter_402_retry_after_only_accepts_bounded_numeric_seconds();
    test_openrouter_402_truncated_reset_response_is_not_retried();
    test_output_token_limit_is_not_reported_as_success();
    test_final_unterminated_sse_line_is_processed();
    test_transient_http_failure_retries_before_streaming();
    test_responses_item_done_requires_terminal_before_reporting_success();
    test_responses_does_not_release_completed_tools_without_terminal();
    test_heartbeats_do_not_extend_model_progress_deadline();
    test_protocol_liveness_and_progress_are_distinct();
    test_responses_lifecycle_advances_without_synthetic_output();
    test_silent_transport_still_times_out();
    test_real_model_progress_extends_idle_deadline();
    test_completed_ends_stream_without_waiting_for_socket_close();
    test_cancel_interrupts_silent_model_stream();
    test_wire_shape_of_request();
    test_no_tools_field_when_empty();
    test_user_image_is_sent_as_multimodal_content();
    test_view_image_tool_pixels_reach_next_http_request();
    test_opus_55_catalog_and_responses_request();
    test_large_historical_image_uses_cached_bounded_preview();
    test_absolute_desktop_image_can_be_outside_the_workspace();
    test_missing_historical_image_does_not_break_later_turns();
    test_missing_current_image_is_reported();
    test_invalid_utf8_history_is_replaced_before_serialization();
    test_responses_streams_reasoning_text_and_function_calls();
    test_responses_item_done_recovers_missing_text_without_duplicates();
    test_responses_message_items_remain_distinct_in_agent_history();
    test_responses_terminal_snapshots_update_their_original_items();
    test_responses_serializes_history_images_and_flat_tools();
    test_responses_continuation_replays_original_output_before_tool_results();
    test_responses_continuation_uses_completed_items_without_terminal_output();
    test_responses_explicit_empty_terminal_does_not_recover_old_tool();
    test_responses_item_snapshot_fallback_rejects_unfinished_visible_item();
    test_responses_never_publishes_continuation_or_tools_from_unsuccessful_output();
    test_responses_cancel_during_state_callback_prevents_tool_delivery();
    test_responses_continuation_checks_source_and_rejects_corruption();
    test_responses_foreign_continuation_uses_only_ordinary_history();
    test_glm_responses_completes_without_done_and_keeps_metadata_only_state();
    test_fable_responses_preserves_all_five_reasoning_efforts();
    test_responses_cache_session_hints_preserve_the_original_request();
    test_responses_cache_session_rejects_bad_ids_before_http();
    test_responses_rejects_unsupported_model_efforts_before_http();
    test_cloud_effort_levels_reach_responses();
    test_routed_models_keep_wire_key_and_failure_isolation();
    test_responses_needs_terminal_event_and_reports_incomplete();
    {
        test_responses_proxy_redirects();
        test_responses_proxy_same_origin_redirect();
        test_responses_proxy_redirect_total_timeout();
        test_responses_without_location_retains_body();
        test_responses_proxy_resolver_timeout_retries_before_output();
#if defined(MAI_MODEL_CLIENT_TLS_TESTS)
        test_responses_proxy_rejects_tls_downgrade();
#endif
        test_responses_proxy_direct_and_cancel();
    }
    test_local_mode_never_falls_back_to_a_remote_request();
    test_remote_factories_default_to_responses_and_reject_mixed();
    if (failures == 0) std::printf("llm tests passed\n");
    return failures == 0 ? 0 : 1;
}
