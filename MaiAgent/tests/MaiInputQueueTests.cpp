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
#include "MaiRolloutLog.h"
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
    std::atomic<bool> timedOut{false};
    bool holdFollowing = false;
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
        if (call == (finalBoundary ? 1u : 0u) || holdFollowing) {
            entered.store(true);
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release.load() && !cancel.load() && std::chrono::steady_clock::now() < limit)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!release.load() && !cancel.load()) {
                timedOut.store(true);
                return {MaiErrorCode::Internal, "Fixture input barrier timed out"};
            }
        }
        if (!cancel.load() && sink.onText) sink.onText("Input received");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    std::vector<std::string> lastUsers() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> users;
        if (!requests.empty())
            for (const auto& item : requests.back().messages)
                if (item.role == MaiModelRole::User) {
                    const auto value = nlohmann::json::parse(item.content, nullptr, false);
                    // 线格式中的宿主执行观察使用 User 角色，但不是队列接收的人类输入。
                    // 仅在测试投影中排除准确类型；下面两处用例另行核对观察确实保留。
                    if (value.is_object() && value.contains("type") && value["type"].is_string() &&
                        value["type"] == "tool_execution_observation")
                        continue;
                    users.push_back(item.content);
                }
        return users;
    }
    std::vector<nlohmann::json> lastToolObservations() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<nlohmann::json> observations;
        if (!requests.empty())
            for (const auto& item : requests.back().messages) {
                if (item.role != MaiModelRole::User) continue;
                auto value = nlohmann::json::parse(item.content, nullptr, false);
                if (value.is_object() && value.contains("type") && value["type"].is_string() &&
                    value["type"] == "tool_execution_observation")
                    observations.push_back(std::move(value));
            }
        return observations;
    }
};

class InputNoop final : public MaiTool {
public:
    std::atomic<bool> entered{false}, release{true};
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
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        entered.store(true);
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release.load() && !context.cancel->load() &&
               std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (context.cancel->load())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Fixture interrupted");
        if (!release.load())
            return MaiToolResult::failure(MaiErrorCode::Internal, "Fixture tool barrier timed out");
        // 只有实际进入并完成闸门的纯内存操作才报告完成；没有外部进程，不伪造退出码。
        MaiToolExecution execution;
        execution.status = MaiToolExecutionStatus::Completed;
        execution.effectStatus = MaiToolEffectStatus::None;
        execution.started = true;
        return MaiToolResult::success("done").withExecution(std::move(execution));
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
    std::atomic<int> polls{0}, canceled{0};
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
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!release.load() && !context.cancel->load() &&
                   std::chrono::steady_clock::now() < limit) {
                polls.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (context.cancel->load()) {
                canceled.fetch_add(1);
                return MaiToolResult::failure(MaiErrorCode::Canceled, "Fixture interrupted");
            }
            if (!release.load())
                return MaiToolResult::failure(MaiErrorCode::Internal,
                                              "Fixture step barrier timed out");
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

// 真正等待请求级 cancel；超时只负责让失败用例退出，不能替代 steering 的取消证据。
// 每次重采样都停在闸门，避免测试线程尚未送完补充时工作线程已经正常结束。
class SamplingInputModel final : public MaiModelClient {
public:
    std::atomic<int> entered{0}, canceled{0};
    std::atomic<bool> release{false}, timedOut{false};
    bool holdCanceledReturn = false;
    std::atomic<bool> allowCanceledReturn{false};
    std::mutex mutex;
    std::vector<MaiModelRequest> requests;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        int index;
        {
            std::lock_guard<std::mutex> lock(mutex);
            index = static_cast<int>(requests.size());
            requests.push_back(request);
        }
        if (index == 0) {
            const MaiModelTextItem completed{"native_completed", MaiModelTextPhase::Commentary, 0};
            if (sink.onTextItemStarted) sink.onTextItemStarted(completed);
            if (sink.onTextItemDelta) sink.onTextItemDelta(completed, "Completed observation");
            if (sink.onTextItemCompleted) sink.onTextItemCompleted(completed);
            if (sink.onOutputItemCompleted)
                sink.onOutputItemCompleted(
                    {0, "native_completed", "message",
                     R"({"id":"native_completed","type":"message","role":"assistant","status":"completed","content":[{"type":"output_text","text":"Completed observation"}],"fixture_unknown":{"preserve":true}})"});
            const MaiModelTextItem draft{"native_draft", MaiModelTextPhase::FinalAnswer, 1};
            if (sink.onTextItemStarted) sink.onTextItemStarted(draft);
            if (sink.onTextItemDelta) sink.onTextItemDelta(draft, "Unfinished answer");
            if (sink.onReasoning) sink.onReasoning("Unfinished reasoning");
            // 即使自定义客户端过早给出旧调用，取消响应也不能执行它。
            if (sink.onToolCall) sink.onToolCall({"stale_call", "input_step", R"({"step":1})"});
        }
        entered.fetch_add(1);
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cancel.load() && !release.load() && std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancel.load()) {
            canceled.fetch_add(1);
            while (holdCanceledReturn && !allowCanceledReturn.load() &&
                   std::chrono::steady_clock::now() < limit)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return {MaiErrorCode::Canceled, "Fixture request canceled"};
        }
        if (!release.load()) {
            timedOut.store(true);
            return {MaiErrorCode::Internal, "Fixture sampling barrier timed out"};
        }
        if (sink.onText) sink.onText("Steered task completed");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    MaiModelRequest lastRequest() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests.empty() ? MaiModelRequest{} : requests.back();
    }
};

// 只清理独占创建的测试目录；声明早于 Agent，保证日志句柄先销毁再清理文件。
class InputJournalDirectory final {
public:
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("sampling_input_")));
    bool created = !MaiFileSystem::createNewDirectory(root);
    ~InputJournalDirectory() {
        if (created) MaiFileSystem::removeRecursively(root);
    }
};

static void newInputCancelsOnlySampling(bool steer) {
    std::printf("Sampling cancellation: steer=%d\n", steer);
    auto model = std::make_unique<SamplingInputModel>();
    auto* observed = model.get();
    auto tool = std::make_unique<InputStepTool>();
    auto* probe = tool.get();
    probe->release.store(true);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    InputJournalDirectory directory;
    CHECK(directory.created);
    if (!directory.created) return;
    auto opened =
        makeMaiSqliteStore(directory.root.append(MaiFilePath::fromUtf8("agent.db")).toUtf8());
    CHECK(opened);
    if (!opened) return;
    MaiAgent agent(std::move(opened.value()), std::move(model), std::move(tools));
    const auto session = agent.submit(MaiCreateSession{"", "Sampling steering", ""}).value();
    std::atomic<int> started{0}, errors{0}, outcomeEvents{0};
    std::mutex committedMutex;
    std::vector<MaiEvent> committedEvents;
    const auto subscription = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.sessionId != session) return;
        if (event.type == MaiEventType::SessionStatus && event.detail == "running") ++started;
        if (event.type == MaiEventType::SessionError) ++errors;
        if (event.type == MaiEventType::TurnOutcomeUpdated) ++outcomeEvents;
        if (event.type == MaiEventType::MessageTextCommitted) {
            // 同步发布线程禁止日志读取；这里只收集身份，waitIdle 后再检查落库正文。
            std::lock_guard<std::mutex> lock(committedMutex);
            committedEvents.push_back(event);
        }
    });
    CHECK(enqueue(agent, session, "initial", "Original user body\nKeep exact spacing  "));
    CHECK(waitForInputStep([&] { return observed->entered.load() == 1; }));
    const auto turn = agent.activeTurnId(session);
    CHECK(!turn.empty());
    for (int index = 0; index < 2; ++index) {
        const auto text = "Steering body " + std::to_string(index);
        const auto id = "sampling_update_" + std::to_string(index);
        CHECK(steer ? agent.submit(MaiSteerPrompt{session, turn, id, text})
                    : enqueue(agent, session, id, text));
        const bool canceled = waitForInputStep([&] { return observed->canceled.load() > index; });
        CHECK(canceled);
        if (!canceled) {
            // 失败清理才用 Stop；上面的取消断言没有手动释放旧采样。
            agent.submit(MaiInterrupt{session});
            agent.waitIdle();
            agent.eventBus().unsubscribe(subscription);
            return;
        }
        CHECK(waitForInputStep([&] { return observed->entered.load() >= index + 2; }));
        CHECK(agent.activeTurnId(session) == turn);
    }
    observed->release.store(true);
    agent.waitIdle();
    CHECK(!observed->timedOut.load());
    CHECK(observed->canceled.load() == 2);
    CHECK(started.load() == 1);
    CHECK(errors.load() == 0);
    CHECK(outcomeEvents.load() == 1);
    for (const auto& event : committedEvents) {
        MaiMessage message;
        CHECK(agent.getMessage(session, event.messageId, message));
        for (const auto& part : message.parts)
            if (part.id == event.partId)
                if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                    CHECK(text->text != "Unfinished answer");
    }
    const auto outcome = agent.getTurnOutcome(session, turn);
    CHECK(outcome && outcome.value().status == MaiTurnStatus::Completed);
    const auto request = observed->lastRequest();
    std::vector<std::string> users;
    bool completed = false;
    for (const auto& message : request.messages) {
        if (message.role == MaiModelRole::User) users.push_back(message.content);
        if (message.role == MaiModelRole::Assistant && message.content == "Completed observation")
            completed = true;
        CHECK(message.content.find("Unfinished answer") == std::string::npos);
        CHECK(message.reasoning.find("Unfinished reasoning") == std::string::npos);
        if (message.continuation) {
            const auto& payload = message.continuation->payload;
            CHECK(payload.find("native_draft") == std::string::npos);
            CHECK(payload.find("stale_call") == std::string::npos);
        }
    }
    CHECK(completed);
    // 完成的原生 item 是日志事实；取消尾部不能制造整响应 continuation，也不能撤销该事实。
    nlohmann::json retained;
    int nativeItems = 0, ownedRollouts = 0, aborted = 0;
    MaiFileSystem::walk(
        directory.root,
        [&](const MaiFileEntry& entry) {
            if (entry.isDirectory || entry.nameUtf8.compare(0, 8, "rollout-") != 0)
                return MaiWalkAction::Continue;
            auto log = MaiRolloutLog::open(entry.path);
            CHECK(log);
            if (!log) return MaiWalkAction::Continue;
            bool owned = false;
            const auto replayed = log.value()->replay({}, [&](const MaiRolloutRecord& record) {
                const auto payload = nlohmann::json::parse(record.payload, nullptr, false);
                if (record.type == "session_meta") {
                    owned = payload.is_object() && payload.value("session_id", "") == session;
                    if (owned) ++ownedRollouts;
                }
                if (owned && record.type == "response_item" && payload.is_object() &&
                    payload.value("id", "") == "native_completed") {
                    ++nativeItems;
                    retained = payload;
                }
                if (owned && record.type == "event_msg" && payload.is_object() &&
                    payload.value("type", "") == "turn_aborted")
                    ++aborted;
                return MaiError{};
            });
            CHECK(replayed);
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError&) { CHECK(false); });
    CHECK(ownedRollouts == 1);
    CHECK(nativeItems == 1);
    CHECK(aborted == 0);
    if (retained.is_object()) {
        CHECK(retained.value("status", "") == "completed");
        CHECK(
            retained.value("content", nlohmann::json{}) ==
            nlohmann::json::array({{{"type", "output_text"}, {"text", "Completed observation"}}}));
        CHECK(retained.value("fixture_unknown", nlohmann::json{}) ==
              nlohmann::json({{"preserve", true}}));
    }
    CHECK(users == (std::vector<std::string>{"Original user body\nKeep exact spacing  ",
                                             "Steering body 0", "Steering body 1"}));
    for (const auto& executions : probe->executions) CHECK(executions.load() == 0);
    bool draft = false;
    for (const auto& message : agent.listMessages(session)) {
        CHECK(!message.pendingInput);
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                if (text->text == "Unfinished answer") {
                    draft = true;
                    CHECK(part.modelDraft);
                }
    }
    CHECK(draft);
    agent.eventBus().unsubscribe(subscription);
}

static void stopSamplingKeepsNewInputPaused(bool queueInput) {
    auto model = std::make_unique<SamplingInputModel>();
    auto* observed = model.get();
    // 暂停取消后的返回点，确保 Stop 和“消费新输入”的先后完全由用例控制。
    observed->holdCanceledReturn = true;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::make_unique<MaiToolRegistry>());
    const auto session = agent.submit(MaiCreateSession{"", "Stop sampling", ""}).value();
    CHECK(enqueue(agent, session, "initial", "Initial sampling input"));
    CHECK(waitForInputStep([&] { return observed->entered.load() == 1; }));
    const auto turn = agent.activeTurnId(session);
    std::string queued;
    if (queueInput) {
        const auto received = enqueue(agent, session, "paused", "Keep this pending input");
        CHECK(received);
        if (received) queued = received.value();
        CHECK(waitForInputStep([&] { return observed->canceled.load() == 1; }));
    }
    CHECK(agent.submit(MaiInterrupt{session}));
    CHECK(waitForInputStep([&] { return observed->canceled.load() == 1; }));
    observed->allowCanceledReturn.store(true);
    agent.waitIdle();
    CHECK(observed->entered.load() == 1);
    CHECK(!observed->timedOut.load());
    CHECK(!agent.isBusy(session));
    if (!queued.empty()) {
        MaiMessage stored;
        CHECK(agent.getMessage(session, queued, stored));
        CHECK(stored.pendingInput);
        CHECK(stored.text() == "Keep this pending input");
    }
    const auto outcome = agent.getTurnOutcome(session, turn);
    CHECK(outcome && outcome.value().status == MaiTurnStatus::Interrupted);
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
    // 工具已重新轮询自己的取消位，证明新输入只抢占采样，没有中断所属工具。
    const auto polls = probe->polls.load();
    CHECK(waitForInputStep([&] { return probe->polls.load() > polls || probe->canceled.load(); }));
    CHECK(probe->canceled.load() == 0);
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
    const auto observations = observed->lastToolObservations();
    // 该响应没有完成 item 或成功终态；过早交付的调用不应成为已接受的旧计划。
    CHECK(observations.empty());
    if (observed->requests.size() >= 2) {
        const auto& request = observed->requests[1];
        for (const auto& message : request.messages) {
            CHECK(message.role != MaiModelRole::ToolResult);
            CHECK(message.invocations.empty());
        }
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
    observed->finalBoundary = true;
    auto tool = std::make_unique<InputNoop>();
    auto* probe = tool.get();
    probe->release.store(false);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent.submit(MaiCreateSession{"", "Input queue", ""}).value();
    // 队列路径必须在模型执行前发开始事件，宿主才能显示等待/停止。
    // 回调重入只读 API 同时确保通知在 Runtime 锁外发布。
    std::atomic<int> started{0};
    const auto subscription = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.sessionId != session || event.type != MaiEventType::SessionStatus ||
            event.detail != "running")
            return;
        CHECK(!event.turnId.empty());
        CHECK(agent.isBusy(session));
        CHECK(agent.activeTurnId(session) == event.turnId);
        if (started.fetch_add(1) == 0) CHECK(!observed->entered.load());
    });
    CHECK(enqueue(agent, session, "first", "First task"));
    // 大批输入在真实所属工具内排队，模型抢占不会让批量测试提前结束或分成新轮次。
    CHECK(waitForInputStep([&] { return probe->entered.load(); }));
    CHECK(started.load() == 1);
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
    probe->release.store(true);
    agent.waitIdle();
    CHECK(observed->lastUsers() == expected);
    size_t users = 0;
    for (const auto& message : agent.listMessages(session)) {
        if (message.role == MaiRole::User) ++users;
        CHECK(!message.pendingInput);
    }
    CHECK(users == 81);
    CHECK(enqueue(agent, session, "request_0", "Input 0").value() == second);
    CHECK(started.load() == 1);
    agent.eventBus().unsubscribe(subscription);
}

static void inputDuringFinalSamplingStaysInTheSameTurn() {
    auto model = std::make_unique<InputModel>();
    auto* observed = model.get();
    observed->finalBoundary = true;
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<InputNoop>());
    MaiAgent::Options options;
    options.maxToolIterations = 1;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto session = agent.submit(MaiCreateSession{"", "Final boundary", ""}).value();
    // 队列路径必须在模型执行前发开始事件，宿主才能显示等待/停止。
    // 回调重入只读 API 同时确保通知在 Runtime 锁外发布。
    std::atomic<int> started{0};
    const auto subscription = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.sessionId != session || event.type != MaiEventType::SessionStatus ||
            event.detail != "running")
            return;
        CHECK(!event.turnId.empty());
        CHECK(agent.isBusy(session));
        CHECK(agent.activeTurnId(session) == event.turnId);
        if (started.fetch_add(1) == 0) CHECK(!observed->entered.load());
    });
    CHECK(enqueue(agent, session, "first", "Original task"));
    CHECK(waitEntered(*observed));
    CHECK(started.load() == 1);
    CHECK(enqueue(agent, session, "late", "Input during final response"));
    observed->release.store(true);
    agent.waitIdle();
    CHECK(observed->lastUsers() ==
          (std::vector<std::string>{"Original task", "Input during final response"}));
    const auto observations = observed->lastToolObservations();
    CHECK(observations.size() == 1);
    if (observations.size() == 1) {
        CHECK(observations[0].at("type") == "tool_execution_observation");
        CHECK(observations[0].at("executions").size() == 1);
        if (observations[0].at("executions").size() == 1) {
            const auto& execution = observations[0].at("executions")[0];
            CHECK(execution.at("call_id") == "fixture_call");
            CHECK(execution.at("tool") == "fixture_noop");
            CHECK(execution.at("execution").at("canceled") == false);
            CHECK(execution.at("execution").at("status") == "completed");
            CHECK(execution.at("execution").at("started") == true);
        }
    }
    CHECK(observed->requests.size() >= 3);
    CHECK(started.load() == 1);
    agent.eventBus().unsubscribe(subscription);
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
        observed->finalBoundary = true;
        auto tool = std::make_unique<InputNoop>();
        auto* probe = tool.get();
        probe->release.store(false);
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(std::move(tool));
        auto store = makeMaiSqliteStore(database.toUtf8());
        CHECK(store);
        MaiAgent agent(std::move(store.value()), std::move(model), std::move(tools));
        session = agent.submit(MaiCreateSession{"", "Persistent queue", ""}).value();
        CHECK(enqueue(agent, session, "first", "First task"));
        CHECK(waitForInputStep([&] { return probe->entered.load(); }));
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
    observed->holdFollowing = true;
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
    observed->finalBoundary = true;
    auto tool = std::make_unique<InputNoop>();
    auto* probe = tool.get();
    probe->release.store(false);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::move(tool));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto session = agent.submit(MaiCreateSession{"", "Clear queue", ""}).value();
    CHECK(enqueue(agent, session, "first", "First task"));
    CHECK(waitForInputStep([&] { return probe->entered.load(); }));
    CHECK(enqueue(agent, session, "queued", "Remove this queued input"));
    CHECK(agent.submit(MaiInterrupt{session}));
    agent.waitIdle();
    CHECK(agent.submit(MaiClearMessages{session}));
    observed->release.store(true);
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
    newInputCancelsOnlySampling(false);
    newInputCancelsOnlySampling(true);
    stopSamplingKeepsNewInputPaused(false);
    stopSamplingKeepsNewInputPaused(true);
    newInputsReplanAfterOneTool(false, false);
    newInputsReplanAfterOneTool(true, false);
    newInputsReplanAfterOneTool(false, true);
    noInputPreservesTheWholeBatch();
    inputDuringModelReplySkipsAllUnstartedTools();
    inputDuringApprovalPreventsTheApprovedOldTool();
    stopDuringToolKeepsNewInputPaused();
    iterationLimitStillDeliversTheNewInput();
    busyInputsArePersistedAndConsumedInOrder();
    inputDuringFinalSamplingStaysInTheSameTurn();
    stopRetainsTheQueueAndSqliteReceiptSurvivesRestart();
    modelToolUsesTheSameOrdinaryQueue();
    clearingHistoryDoesNotRestorePausedInputs();
    std::printf("MaiInputQueueTests: %d failures\n", failures);
    return failures ? 1 : 0;
}
