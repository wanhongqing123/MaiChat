// 文件作用：用同步屏障验证并发准入、顺序落库、取消排空与调用身份边界。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <json.hpp>
#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"
#include "MaiExecCommandTool.h"
#include "MaiToolCallValidation.h"
#include "MaiModelHttpFraming.h"

static std::atomic<int> failures{0};
static bool serialControl = false;
#define CHECK(value)                                                    \
    do {                                                                \
        if (!(value)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #value); \
            ++failures;                                                 \
        }                                                               \
    } while (0)
namespace {
class Model final : public MaiModelClient {
public:
    std::function<MaiError(const MaiModelRequest&, const MaiStreamSink&, int)> respond;
    int requests = 0;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        return respond(request, sink, ++requests);
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
};
struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0, peak = 0, started = 0, finished = 0, exclusive = 0;
    bool overlapped = true, exclusiveOverlap = false;
    bool waitForCancel = false;
};
class Read final : public MaiTool {
public:
    explicit Read(Probe& probe) : mProbe(probe) {}
    std::string name() const override {
        return "owned_read";
    }
    std::string description() const override {
        return "Read owned test data.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        const int value = nlohmann::json::parse(arguments).value("value", 0);
        std::unique_lock<std::mutex> lock(mProbe.mutex);
        ++mProbe.started;
        ++mProbe.active;
        mProbe.peak = std::max(mProbe.peak, mProbe.active);
        mProbe.cv.notify_all();
        if (mProbe.waitForCancel) {
            while (!context.isCanceled()) mProbe.cv.wait_for(lock, std::chrono::milliseconds(10));
        } else {
            const int target = value < 2 ? 2 : 4;
            if (!mProbe.cv.wait_for(lock, std::chrono::seconds(2),
                                    [&] { return mProbe.started >= target; }))
                mProbe.overlapped = false;
        }
        --mProbe.active;
        ++mProbe.finished;
        mProbe.cv.notify_all();
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Canceled owned read.");
        return MaiToolResult::success(std::to_string(value));
    }

private:
    Probe& mProbe;
};
class Exclusive final : public MaiTool {
public:
    explicit Exclusive(Probe& probe) : mProbe(probe) {}
    std::string name() const override {
        return "owned_exclusive";
    }
    std::string description() const override {
        return "Record an exclusive owned operation.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        std::lock_guard<std::mutex> lock(mProbe.mutex);
        ++mProbe.exclusive;
        mProbe.exclusiveOverlap |= mProbe.active != 0;
        return MaiToolResult::success("exclusive");
    }

private:
    Probe& mProbe;
};
std::string create(MaiAgent& agent) {
    auto session = agent.submit(
        MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(), "", "glm-5.4", ""});
    CHECK(session);
    return session ? session.value() : std::string{};
}
void test_bounded_parallelism_preserves_exclusive_barriers_and_result_order() {
    Probe probe;
    auto model = std::make_unique<Model>();
    model->respond = [](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        if (attempt == 1) {
            sink.onToolCall({"read_0", "owned_read", R"({"value":0})"});
            sink.onToolCall({"read_1", "owned_read", R"({"value":1})"});
            sink.onToolCall({"exclusive", "owned_exclusive", "{}"});
            sink.onToolCall({"read_3", "owned_read", R"({"value":3})"});
            sink.onToolCall({"read_4", "owned_read", R"({"value":4})"});
        } else {
            std::vector<std::string> results;
            for (const auto& item : request.messages)
                if (item.role == MaiModelRole::ToolResult) results.push_back(item.content);
            CHECK(results == (std::vector<std::string>{"0", "1", "exclusive", "3", "4"}));
            if (attempt == 2)
                return MaiError{MaiErrorCode::Network, "owned post-tool stream interruption"}
                    .withRetryAdvice({1, 0, -1, false});
            sink.onText("Finished owned operations.");
        }
        return MaiError{};
    };
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<Read>(probe));
    tools->add(std::make_unique<Exclusive>(probe));
    MaiAgent::Options options;
    options.maxParallelTools = serialControl ? 1 : 2;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    std::vector<MaiEvent> phases;
    std::atomic<int> errors{0};
    auto token = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::TurnPhaseChanged) phases.push_back(event);
        if (event.type == MaiEventType::SessionError) ++errors;
    });
    const auto session = create(agent);
    const auto turn = agent.submit(
        MaiSendPrompt{session, "Run owned independent reads and one exclusive operation."});
    CHECK(turn);
    agent.waitIdle();
    CHECK(probe.overlapped && probe.peak == 2);
    CHECK(!probe.exclusiveOverlap && probe.exclusive == 1 && probe.finished == 4 &&
          probe.active == 0);
    CHECK(errors.load() == 0);
    CHECK(!phases.empty() && phases.front().detail == "preparing_context" &&
          phases.back().detail == "finalizing");
    for (const auto& event : phases)
        CHECK(event.turnId == turn.value() && event.sessionId == session);
    agent.eventBus().unsubscribe(token);
}
void test_cancel_drains_readers_before_turn_cleanup() {
    Probe probe;
    probe.waitForCancel = true;
    auto model = std::make_unique<Model>();
    model->respond = [](const MaiModelRequest&, const MaiStreamSink& sink, int) {
        sink.onToolCall({"a", "owned_read", "{}"});
        sink.onToolCall({"b", "owned_read", "{}"});
        sink.onToolCall({"write", "owned_exclusive", "{}"});
        return MaiError{};
    };
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<Read>(probe));
    tools->add(std::make_unique<Exclusive>(probe));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = create(agent);
    CHECK(agent.submit(MaiSendPrompt{session, "Wait until canceled."}));
    {
        std::unique_lock<std::mutex> lock(probe.mutex);
        CHECK(probe.cv.wait_for(lock, std::chrono::seconds(3), [&] { return probe.started == 2; }));
    }
    CHECK(agent.submit(MaiInterrupt{session}));
    agent.waitIdle();
    CHECK(probe.finished == 2 && probe.active == 0 && probe.exclusive == 0);
}
void test_invalid_call_identity_cannot_start_any_tool() {
    for (const bool empty : {false, true}) {
        Probe probe;
        auto model = std::make_unique<Model>();
        model->respond = [=](const MaiModelRequest&, const MaiStreamSink& sink, int) {
            sink.onToolCall({empty ? "" : "same", "owned_exclusive", "{}"});
            sink.onToolCall({"same", "owned_exclusive", "{}"});
            return MaiError{};
        };
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(std::make_unique<Exclusive>(probe));
        MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
        std::atomic<int> errors{0};
        auto token = agent.eventBus().subscribe([&](const MaiEvent& e) {
            if (e.type == MaiEventType::SessionError) ++errors;
        });
        CHECK(agent.submit(
            MaiSendPrompt{create(agent), "Reject malformed identity before any effect."}));
        agent.waitIdle();
        CHECK(probe.exclusive == 0 && errors.load() == 1);
        agent.eventBus().unsubscribe(token);
    }
    CHECK(MaiToolCallValidation::validateArguments({"id", "tool", "[]"}).code() ==
          MaiErrorCode::InvalidInput);
    CHECK(!MaiToolCallValidation::validateArguments({"id", "tool", "{}"}));
}
void test_parallel_exceptions_become_tool_results_and_protected_context_does_not_loop() {
    class Throw final : public MaiTool {
    public:
        std::atomic<int> calls{0};
        std::string name() const override {
            return "owned_throw";
        }
        std::string description() const override {
            return "Throw an owned failure.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        MaiToolExecutionMode executionMode() const override {
            return MaiToolExecutionMode::ParallelReadOnly;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            ++calls;
            throw std::runtime_error("owned worker failure");
        }
    };
    auto tool = std::make_unique<Throw>();
    auto* observed = tool.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    auto model = std::make_unique<Model>();
    model->respond = [](const MaiModelRequest& request, const MaiStreamSink& sink, int attempt) {
        if (attempt == 1) {
            sink.onToolCall({"throw_1", "owned_throw", "{}"});
            sink.onToolCall({"throw_2", "owned_throw", "{}"});
        } else {
            int results = 0;
            for (const auto& item : request.messages)
                if (item.role == MaiModelRole::ToolResult) {
                    CHECK(item.content.find("owned worker failure") != std::string::npos);
                    ++results;
                }
            CHECK(results == 2);
            sink.onText("Handled owned tool failures.");
        }
        return MaiError{};
    };
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    CHECK(agent.submit(MaiSendPrompt{create(agent), "Handle parallel failures."}));
    agent.waitIdle();
    CHECK(observed->calls.load() == 2);

    auto reject = std::make_unique<Model>();
    auto* calls = reject.get();
    reject->respond = [](const MaiModelRequest&, const MaiStreamSink&, int) {
        return MaiError{MaiErrorCode::ContextWindowExceeded, "owned uncompressible context"};
    };
    MaiAgent protectedAgent(makeMaiMemoryStore(), std::move(reject));
    CHECK(protectedAgent.submit(
        MaiSendPrompt{create(protectedAgent), "Keep this current input exactly."}));
    protectedAgent.waitIdle();
    CHECK(calls->requests == 1);
}

void test_withdrawn_parallel_tool_does_not_start_after_placeholder_event() {
    class Withdrawn final : public MaiTool {
    public:
        std::atomic<bool> available{true};
        std::atomic<int> executed{0};
        std::string name() const override {
            return "owned_withdrawn";
        }
        std::string description() const override {
            return "Owned removable read capability.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        bool isAvailableForModel(const std::string&) const override {
            return available.load();
        }
        MaiToolExecutionMode executionMode() const override {
            return MaiToolExecutionMode::ParallelReadOnly;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            ++executed;
            return MaiToolResult::success("unexpected execution");
        }
    };
    auto tool = std::make_unique<Withdrawn>();
    auto* observed = tool.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    auto model = std::make_unique<Model>();
    model->respond = [](const MaiModelRequest&, const MaiStreamSink& sink, int attempt) {
        if (attempt == 1) {
            sink.onToolCall({"withdraw_1", "owned_withdrawn", "{}"});
            sink.onToolCall({"withdraw_2", "owned_withdrawn", "{}"});
        } else
            sink.onText("Capability withdrawal handled.");
        return MaiError{};
    };
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    auto token = agent.eventBus().subscribe([&](const MaiEvent& e) {
        if (e.type == MaiEventType::MessagePartUpdated) observed->available.store(false);
    });
    CHECK(agent.submit(MaiSendPrompt{create(agent), "Handle withdrawn tools."}));
    agent.waitIdle();
    CHECK(observed->executed.load() == 0);
    agent.eventBus().unsubscribe(token);
}

void test_shell_spelling_and_http_framing_fail_closed() {
    auto shell = makeMaiExecCommandTool();
    for (const auto& command : std::vector<std::string>{
             "/tmp/owned/cat file", "./git status", "cat.cmd file", "echo hello\rdel owned",
             "echo %OWNED%", "echo !OWNED!", std::string("echo ok\0hidden", 14)}) {
        CHECK(maiExecCommandRequiresApproval(command));
        CHECK(shell->requiresApproval(nlohmann::json{{"cmd", command}}.dump()));
    }
    CHECK(!maiExecCommandRequiresApproval("echo owned"));
    MaiModelHttpFraming framing;
    framing.onHeader("HTTP/1.1 200 OK\r\n");
    framing.onHeader("Content-Length: 4\r\n");
    framing.received(3);
    CHECK(!framing.isComplete());
    framing.received(1);
    CHECK(framing.isComplete());
    framing.onHeader("Content-Encoding: gzip\r\n");
    CHECK(!framing.isComplete());
    framing.onHeader("HTTP/1.1 200 OK\r\n");
    framing.onHeader("Content-Length: 4\r\n");
    framing.onHeader("Transfer-Encoding: chunked\r\n");
    framing.received(4);
    CHECK(!framing.isComplete());
}
}  // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--serial-control") {
        serialControl = true;
        test_bounded_parallelism_preserves_exclusive_barriers_and_result_order();
        return failures.load() == 0 ? 0 : 1;
    }
    test_bounded_parallelism_preserves_exclusive_barriers_and_result_order();
    test_cancel_drains_readers_before_turn_cleanup();
    test_invalid_call_identity_cannot_start_any_tool();
    test_parallel_exceptions_become_tool_results_and_protected_context_does_not_loop();
    test_withdrawn_parallel_tool_does_not_start_after_placeholder_event();
    test_shell_spelling_and_http_framing_fail_closed();
    std::printf("runtime tests: %d failure(s)\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}
