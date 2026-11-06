// 文件作用：验证所属端在忙碌/收尾/停止时的真实输入消费，以及持久凭据与模型工具入口。
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>
#include "MaiAgent.h"
#include "MaiFileSystem.h"
#include "MaiFakeModelClient.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSessionTools.h"
#include "MaiSqliteStore.h"

static int failures = 0;
#define CHECK(value)                                        \
    do {                                                    \
        if (!(value)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #value); \
            ++failures;                                     \
        }                                                   \
    } while (0)

class InputModel final : public MaiModelClient {
public:
    std::atomic<bool> entered{false}, release{false};
    bool finalBoundary = false;
    std::vector<MaiToolInvocation> initialCalls;
    std::mutex mutex;
    std::vector<MaiModelRequest> requests;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        size_t call;
        {
            std::lock_guard<std::mutex> lock(mutex);
            call = requests.size();
            requests.push_back(request);
        }
        if (finalBoundary && call == 0) {
            if (sink.onToolCall) sink.onToolCall({"fixture_call", "fixture_noop", "{}"});
            return {};
        }
        if (call == 0 && sink.onToolCall)
            for (const auto& invocation : initialCalls) sink.onToolCall(invocation);
        if (call == (finalBoundary ? 1u : 0u)) {
            entered.store(true);
            while (!release.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!cancel.load() && sink.onText) sink.onText("Input received");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    std::vector<std::string> lastUsers() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> users;
        if (!requests.empty())
            for (const auto& item : requests.back().messages)
                if (item.role == MaiModelRole::User) users.push_back(item.content);
        return users;
    }
};

class InputNoop final : public MaiTool {
public:
    std::string name() const override {
        return "fixture_noop";
    }
    std::string description() const override {
        return "Local test operation";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{}})";
    }
    bool requiresApproval(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("done");
    }
};

static bool waitEntered(InputModel& model) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!model.entered.load() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return model.entered.load();
}

static MaiResult<std::string> enqueue(MaiAgent& agent, const std::string& session,
                                      const std::string& request, const std::string& text) {
    return agent.submit(MaiEnqueuePrompt{MaiSendPrompt{session, text}, request});
}

// 第一项工具留在可观察闸门内，后两项只有真实 execute 才计数。
// 因而能区分“回执看起来取消”与“旧工具实际已经执行”的不同结果。
class InputStepTool final : public MaiTool {
public:
    std::atomic<bool> entered{false}, release{false};
    std::array<std::atomic<int>, 3> executions{};
    bool failFirst = false;
    bool requireFirstApproval = false;
    std::string name() const override {
        return "input_step";
    }
    std::string description() const override {
        return "Observe individual tool execution";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"step":{"type":"integer"}}})";
    }
    bool requiresApproval(const std::string& arguments) const override {
        return requireFirstApproval && nlohmann::json::parse(arguments).value("step", -1) == 0;
    }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        const int step = nlohmann::json::parse(arguments).value("step", -1);
        if (step < 0 || step >= 3)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "Invalid fixture step");
        ++executions[step];
        if (step == 0) {
            entered.store(true);
            while (!release.load() && !context.cancel->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (context.cancel->load())
                return MaiToolResult::failure(MaiErrorCode::Canceled, "Fixture interrupted");
            if (failFirst)
                return MaiToolResult::failure(MaiErrorCode::Internal, "First step failed");
        }
        return MaiToolResult::success("Finished step " + std::to_string(step));
    }
};

static std::vector<MaiToolInvocation> inputSteps() {
    return {{"call_A", "input_step", R"({"step":0})"},
            {"call_B", "input_step", R"({"step":1})"},
            {"call_C", "input_step", R"({"step":2})"}};
}

template <typename Predicate>
static bool waitForInputStep(Predicate predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

// 本项目的模型协议要求 assistant 的每个 tool call 都有相邻、同编号的结果。
// 新输入必须排在完整配对之后；未执行的项不能以成功结果或无结果的调用冒充。
static void checkInputStepResults(const MaiModelRequest& request, bool canceled,
                                  bool failedFirst = false) {
    std::size_t start = request.messages.size();
    for (std::size_t i = 0; i < request.messages.size(); ++i)
        if (request.messages[i].role == MaiModelRole::Assistant &&
            request.messages[i].invocations.size() == 3) {
            CHECK(start == request.messages.size());
            start = i;
        }
    CHECK(start + 3 < request.messages.size());
    if (start + 3 >= request.messages.size()) return;
    const auto calls = inputSteps();
    for (std::size_t i = 0; i < calls.size(); ++i) {
        CHECK(request.messages[start].invocations[i].id == calls[i].id);
        const auto& result = request.messages[start + 1 + i];
        CHECK(result.role == MaiModelRole::ToolResult);
        CHECK(result.toolCallId == calls[i].id);
        if (canceled && i > 0) {
            CHECK(result.content.find("not executed") != std::string::npos);
            CHECK(result.content.find("new user input") != std::string::npos);
        } else {
            CHECK(result.content == (failedFirst && i == 0 ? "First step failed"
                                                           : "Finished step " + std::to_string(i)));
        }
    }
}

static void newInputsReplanAfterOneTool(bool failedFirst, bool steer) {
    std::printf("Per-tool input: failedFirst=%d steer=%d\n", failedFirst, steer);
    MaiFakeModelClient::Turn first, next, final;
    first.invocations = inputSteps();
    // 模型结合补充后只重新要求 C；旧的 B、C 不能自行恢复执行。
    next.invocations = {{"replanned_C", "input_step", R"({"step":2})"}};
    final.textChunks = {"Updated plan completed"};
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{first, next, final});
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    probe->failFirst = failedFirst;
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    auto store = makeMaiMemoryStore();
    if (failedFirst) {
        auto opened = makeMaiSqliteStore(":memory:");
        CHECK(opened);
        if (!opened) return;
        store = std::move(opened.value());
    }
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    const auto root = MaiFileSystem::temporaryDirectory().toUtf8();
    const auto session = agent.submit(MaiCreateSession{root, "Per-tool input", ""}).value();
    const auto sent = enqueue(agent, session, "initial", "Run three steps");
    CHECK(sent);
    CHECK(waitForInputStep([&] { return probe->entered.load(); }));
    const auto turn = agent.activeTurnId(session);
    for (int i = 0; i < 2; ++i) {
        const auto id = "update_" + std::to_string(i);
        const auto text = "Changed instruction " + std::to_string(i);
        const auto received = steer ? agent.submit(MaiSteerPrompt{session, turn, id, text})
                                    : enqueue(agent, session, id, text);
        CHECK(received);
    }
    CHECK(observed->requestCount() == 1);
    CHECK(probe->executions[0] == 1);
    CHECK(probe->executions[1] == 0);
    CHECK(probe->executions[2] == 0);
    probe->release.store(true);
    agent.waitIdle();
    CHECK(probe->executions[0] == 1);
    CHECK(probe->executions[1] == 0);
    CHECK(probe->executions[2] == 1);
    CHECK(observed->requestCount() == 3);
    if (observed->requestCount() < 2) return;
    const auto request = observed->request(1);
    checkInputStepResults(request, true, failedFirst);
    CHECK(request.messages.size() >= 2);
    if (request.messages.size() >= 2) {
        const auto end = request.messages.size();
        CHECK(request.messages[end - 2].role == MaiModelRole::User);
        CHECK(request.messages[end - 2].content == "Changed instruction 0");
        CHECK(request.messages[end - 1].role == MaiModelRole::User);
        CHECK(request.messages[end - 1].content == "Changed instruction 1");
    }
    std::vector<MaiToolState> states;
    for (const auto& message : agent.listMessages(session)) {
        CHECK(!message.pendingInput);
        for (const auto& part : message.parts)
            if (const auto* stored = std::get_if<MaiToolPart>(&part.body))
                states.push_back(stored->state);
    }
    CHECK(states == (std::vector<MaiToolState>{
                        failedFirst ? MaiToolState::Error : MaiToolState::Completed,
                        MaiToolState::Canceled, MaiToolState::Canceled, MaiToolState::Completed}));
}

static void noInputPreservesTheWholeBatch() {
    MaiFakeModelClient::Turn first, final;
    first.invocations = inputSteps();
    final.textChunks = {"Original plan completed"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, final});
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    probe->release.store(true);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent
                             .submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(),
                                                      "No pending input", ""})
                             .value();
    CHECK(enqueue(agent, session, "initial", "Run three steps"));
    agent.waitIdle();
    for (const auto& executions : probe->executions) CHECK(executions == 1);
    CHECK(observed->requestCount() == 2);
    if (observed->requestCount() >= 2) checkInputStepResults(observed->request(1), false);
}

static void inputDuringModelReplySkipsAllUnstartedTools() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    observed->initialCalls = inputSteps();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    probe->release.store(true);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent
                             .submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(),
                                                      "Input during response", ""})
                             .value();
    CHECK(enqueue(agent, session, "initial", "Run three steps"));
    CHECK(waitEntered(*observed));
    CHECK(enqueue(agent, session, "update", "Reconsider every step"));
    observed->release.store(true);
    agent.waitIdle();
    for (const auto& executions : probe->executions) CHECK(executions == 0);
    CHECK(observed->requests.size() == 2);
    CHECK(observed->lastUsers() ==
          (std::vector<std::string>{"Run three steps", "Reconsider every step"}));
    if (observed->requests.size() >= 2) {
        const auto& request = observed->requests[1];
        std::size_t results = 0;
        for (const auto& message : request.messages)
            if (message.role == MaiModelRole::ToolResult) {
                ++results;
                CHECK(message.content.find("not executed") != std::string::npos);
            }
        CHECK(results == 3);
    }
}

static void inputDuringApprovalPreventsTheApprovedOldTool() {
    MaiFakeModelClient::Turn first, final;
    first.invocations = {inputSteps().front()};
    final.textChunks = {"Reconsidered the pending operation"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, final});
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    probe->requireFirstApproval = true;
    probe->release.store(true);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent
                             .submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(),
                                                      "Input during approval", ""})
                             .value();
    CHECK(enqueue(agent, session, "initial", "Run the approved step"));
    CHECK(waitForInputStep([&] { return !agent.listPendingPermissions().empty(); }));
    const auto permissions = agent.listPendingPermissions();
    if (permissions.empty()) {
        agent.submit(MaiInterrupt{session});
        agent.waitIdle();
        return;
    }
    CHECK(enqueue(agent, session, "update", "Change the requested operation"));
    CHECK(
        agent.submit(MaiReplyPermission{permissions.front().id, MaiPermissionDecision::Approved}));
    agent.waitIdle();
    CHECK(probe->executions[0] == 0);
    CHECK(agent.listPendingPermissions().empty());
    CHECK(observed->requestCount() == 2);
    const auto request = observed->lastRequest();
    CHECK(request.messages.back().role == MaiModelRole::User);
    CHECK(request.messages.back().content == "Change the requested operation");
    std::size_t results = 0;
    for (const auto& message : request.messages)
        if (message.role == MaiModelRole::ToolResult) {
            ++results;
            CHECK(message.toolCallId == "call_A");
            CHECK(message.content.find("not executed") != std::string::npos);
        }
    CHECK(results == 1);
}

static void stopDuringToolKeepsNewInputPaused() {
    MaiFakeModelClient::Turn first;
    first.invocations = inputSteps();
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first});
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent
                             .submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(),
                                                      "Stop during tool", ""})
                             .value();
    CHECK(enqueue(agent, session, "initial", "Run three steps"));
    CHECK(waitForInputStep([&] { return probe->entered.load(); }));
    const auto queued = enqueue(agent, session, "update", "Retain this paused input");
    CHECK(queued);
    CHECK(agent.submit(MaiInterrupt{session}));
    agent.waitIdle();
    CHECK(probe->executions[0] == 1);
    CHECK(probe->executions[1] == 0);
    CHECK(probe->executions[2] == 0);
    CHECK(observed->requestCount() == 1);
    MaiMessage stored;
    CHECK(agent.getMessage(session, queued.value(), stored));
    CHECK(stored.pendingInput);
    CHECK(stored.text() == "Retain this paused input");
}

static void iterationLimitStillDeliversTheNewInput() {
    MaiFakeModelClient::Turn first, final;
    first.invocations = inputSteps();
    final.textChunks = {"Updated status at the iteration limit"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, final});
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent::Options options;
    options.maxToolIterations = 1;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto session = agent
                             .submit(MaiCreateSession{MaiFileSystem::temporaryDirectory().toUtf8(),
                                                      "Input at limit", ""})
                             .value();
    CHECK(enqueue(agent, session, "initial", "Run three steps"));
    CHECK(waitForInputStep([&] { return probe->entered.load(); }));
    CHECK(enqueue(agent, session, "update", "Reconsider the remaining steps"));
    probe->release.store(true);
    agent.waitIdle();
    CHECK(probe->executions[0] == 1);
    CHECK(probe->executions[1] == 0);
    CHECK(probe->executions[2] == 0);
    CHECK(observed->requestCount() == 2);
    const auto request = observed->lastRequest();
    CHECK(request.tools.empty());
    checkInputStepResults(request, true);
    std::size_t updates = 0;
    for (const auto& message : request.messages)
        if (message.role == MaiModelRole::User &&
            message.content == "Reconsider the remaining steps")
            ++updates;
    CHECK(updates == 1);
}

static void busyInputsArePersistedAndConsumedInOrder() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::make_unique<MaiToolRegistry>());
    const auto session = agent.submit(MaiCreateSession{"", "Input queue", ""}).value();
    CHECK(enqueue(agent, session, "first", "First task"));
    CHECK(waitEntered(*observed));
    std::vector<std::string> expected{"First task"};
    std::string second;
    for (int i = 0; i < 80; ++i) {
        const auto text = "Input " + std::to_string(i);
        const auto sent = enqueue(agent, session, "request_" + std::to_string(i), text);
        CHECK(sent);
        if (i == 0 && sent) second = sent.value();
        expected.push_back(text);
    }
    CHECK(enqueue(agent, session, "request_0", "Input 0").value() == second);
    CHECK(!enqueue(agent, session, "request_0", "Different input"));
    CHECK(observed->lastUsers() == std::vector<std::string>{"First task"});
    CHECK(agent.isBusy(session));
    observed->release.store(true);
    agent.waitIdle();
    CHECK(observed->lastUsers() == expected);
    size_t users = 0;
    for (const auto& message : agent.listMessages(session)) {
        if (message.role == MaiRole::User) ++users;
        CHECK(!message.pendingInput);
    }
    CHECK(users == 81);
    CHECK(enqueue(agent, session, "request_0", "Input 0").value() == second);
}

static void finalBoundaryStartsTheNextQueuedTurn() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    observed->finalBoundary = true;
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<InputNoop>());
    MaiAgent::Options options;
    options.maxToolIterations = 1;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto session = agent.submit(MaiCreateSession{"", "Final boundary", ""}).value();
    CHECK(enqueue(agent, session, "first", "Original task"));
    CHECK(waitEntered(*observed));
    CHECK(enqueue(agent, session, "late", "Input during final response"));
    observed->release.store(true);
    agent.waitIdle();
    CHECK(observed->lastUsers() ==
          (std::vector<std::string>{"Original task", "Input during final response"}));
    CHECK(observed->requests.size() >= 3);
}

static void stopRetainsTheQueueAndSqliteReceiptSurvivesRestart() {
    const auto directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("input_queue_")));
    CHECK(!MaiFileSystem::createDirectories(directory));
    const auto database = directory.append(MaiFilePath::fromUtf8("agent.db"));
    std::string session, messageId;
    {
        auto model = std::make_unique<InputModel>();
        auto* observed = model.get();
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store);
        MaiAgent agent(std::move(store.value()), std::move(model),
                       std::make_unique<MaiToolRegistry>());
        session = agent.submit(MaiCreateSession{"", "Persistent queue", ""}).value();
        CHECK(enqueue(agent, session, "first", "First task"));
        CHECK(waitEntered(*observed));
        const auto waiting = enqueue(agent, session, "pending", "Keep this input");
        CHECK(waiting);
        messageId = waiting.value();
        CHECK(agent.submit(MaiInterrupt{session}));
        agent.waitIdle();
        bool pending = false;
        for (const auto& message : agent.listMessages(session))
            if (message.id == messageId) pending = message.pendingInput;
        CHECK(pending);
    }
    {
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store);
        MaiMessage stored;
        CHECK(store.value()->getInputMessage(session, "pending", stored));
        CHECK(stored.id == messageId && stored.pendingInput);
        auto model = std::make_unique<InputModel>();
        auto* observed = model.get();
        observed->release.store(true);
        MaiAgent agent(std::move(store.value()), std::move(model),
                       std::make_unique<MaiToolRegistry>());
        CHECK(enqueue(agent, session, "pending", "Keep this input").value() == messageId);
        agent.waitIdle();
        CHECK(observed->lastUsers() == (std::vector<std::string>{"First task", "Keep this input"}));
        CHECK(!enqueue(agent, session, "pending", "Changed after restart"));
        CHECK(agent.listMessages(session).size() == 4);
    }
    MaiFileSystem::removeFile(database);
}

static void modelToolUsesTheSameOrdinaryQueue() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::make_unique<MaiToolRegistry>());
    const auto source = agent.submit(MaiCreateSession{"", "Source", ""}).value();
    const auto target = agent.submit(MaiCreateSession{"", "Target", ""}).value();
    CHECK(enqueue(agent, target, "first", "First task"));
    CHECK(waitEntered(*observed));
    MaiToolRegistry tools;
    registerMaiSessionTools(tools);
    auto* tool = tools.find("session_send_input");
    CHECK(tool != nullptr);
    MaiToolContext context;
    context.sessionId = source;
    context.sessionAgent = &agent;
    context.partId = "prt_stable_input";
    const auto args =
        nlohmann::json{{"session_id", target}, {"text", "Input from another conversation"}}.dump();
    const auto sent = tool->execute(args, context);
    CHECK(!sent.hasError());
    CHECK(tool->execute(args, context).output() == sent.output());
    CHECK(
        tool->execute(nlohmann::json{{"session_id", source}, {"text", "Self loop"}}.dump(), context)
            .hasError());
    observed->release.store(true);
    agent.waitIdle();
    CHECK(observed->lastUsers() ==
          (std::vector<std::string>{"First task", "Input from another conversation"}));
}

static void clearingHistoryDoesNotRestorePausedInputs() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::make_unique<MaiToolRegistry>());
    const auto session = agent.submit(MaiCreateSession{"", "Clear queue", ""}).value();
    CHECK(enqueue(agent, session, "first", "First task"));
    CHECK(waitEntered(*observed));
    CHECK(enqueue(agent, session, "queued", "Remove this queued input"));
    CHECK(agent.submit(MaiInterrupt{session}));
    agent.waitIdle();
    CHECK(agent.submit(MaiClearMessages{session}));
    CHECK(enqueue(agent, session, "fresh", "Only the fresh input"));
    agent.waitIdle();
    size_t users = 0;
    for (const auto& message : agent.listMessages(session)) {
        if (message.role == MaiRole::User) {
            ++users;
            CHECK(message.text() == "Only the fresh input");
        }
    }
    CHECK(users == 1);
}

int main() {
    newInputsReplanAfterOneTool(false, false);
    newInputsReplanAfterOneTool(true, false);
    newInputsReplanAfterOneTool(false, true);
    noInputPreservesTheWholeBatch();
    inputDuringModelReplySkipsAllUnstartedTools();
    inputDuringApprovalPreventsTheApprovedOldTool();
    stopDuringToolKeepsNewInputPaused();
    iterationLimitStillDeliversTheNewInput();
    busyInputsArePersistedAndConsumedInOrder();
    finalBoundaryStartsTheNextQueuedTurn();
    stopRetainsTheQueueAndSqliteReceiptSurvivesRestart();
    modelToolUsesTheSameOrdinaryQueue();
    clearingHistoryDoesNotRestorePausedInputs();
    std::printf("MaiInputQueueTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
