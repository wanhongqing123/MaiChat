// 文件作用：验证“编排一轮模型调用、工具执行和事件发布”的正确行为和边界条件。
// 验证入口：test_prompt_images_reach_the_current_user_message、test_full_turn、
// test_request_is_assembled_correctly；检查可观察结果而非实现细节。
// agent loop 的测试：从 submit(Prompt) 到事件流吐完的完整一圈。
//
// 用进程内合成模型驱动回调，不访问供应商、凭据或网络；结果完全确定。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiAppStorageTool.h"
#include "MaiCloudModelCatalog.h"
#include "MaiContextCompactor.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiFakeModelClient.h"
#include "MaiTool.h"
#include "MaiToolOutput.h"
#include "MaiToolOutputTool.h"
#include "MaiTurnRunner.h"
#include "MaiSessionTools.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// ── 合成模型：直接驱动客户端回调 ─────────────────────────────
// 默认应答：分三片吐出来，好让上层收到多条 delta。
//
// 不再起假 HTTP 服务端。这个文件测的是"一轮对话的生命周期"——事件顺序、part 归属、落库内容、中断、
// 并发——没有一条和传输层有关。
// 以前每条断言都要绕一圈 JSON -> TCP -> curl -> SSE 解析，想验证
// "第二轮带上了上一轮的回答"还得去 body["messages"][1]["content"] 里翻，
// 而那句话本来就在 MaiModelRequest 这个结构体里放着。
//
// 传输层归 MaiModelClientTests（走真 socket）和 e2e（走真 bridge）管。
const std::vector<std::string> kDefaultChunks{"Hi", ", I am ", "a fake model"};
const char* kDefaultText = "Hi, I am a fake model";

MaiFakeModelClient::Turn defaultTurn(int chunkDelayMilliseconds = 0) {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = kDefaultChunks;
    turn.chunkDelayMilliseconds = chunkDelayMilliseconds;
    return turn;
}

// 只拒绝首次携带续接状态的写入；真实内存后端继续负责读取和其余领域操作。
class ContinuationWriteFailureStore final : public MaiSessionStore {
public:
    int rejectedWrites = 0;
    // Optional owned retention fault: existing continuation tests leave this
    // disabled and retain the default unsupported ToolOutput behavior.
    bool forwardToolOutput = false;
    std::string disappearOutputId;
    mutable std::size_t readableOutputCallsRemaining = 0;
    void putSession(const MaiSession& value) override {
        mInner->putSession(value);
    }
    bool getSession(const std::string& id, MaiSession& out) const override {
        return mInner->getSession(id, out);
    }
    std::vector<MaiSession> listSessions() const override {
        return mInner->listSessions();
    }
    bool removeSession(const std::string& id) override {
        return mInner->removeSession(id);
    }
    bool clearMessages(const std::string& id) override {
        return mInner->clearMessages(id);
    }
    void putMessage(const std::string& id, const MaiMessage& value) override {
        if (mFailed) return;
        for (const auto& part : value.parts) {
            const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body);
            if (reasoning && reasoning->continuation) {
                ++rejectedWrites;
                mFailed = true;
                return;
            }
        }
        mInner->putMessage(id, value);
    }
    std::vector<MaiMessage> listMessages(const std::string& id) const override {
        return mInner->listMessages(id);
    }
    std::vector<MaiMessage> listMessagesPage(const std::string& id, const std::string& before,
                                             std::size_t limit) const override {
        return mInner->listMessagesPage(id, before, limit);
    }
    bool getMessage(const std::string& id, const std::string& message,
                    MaiMessage& out) const override {
        return mInner->getMessage(id, message, out);
    }
    void recoverInterruptedTools(const std::string& error, std::int64_t completed) override {
        mInner->recoverInterruptedTools(error, completed);
    }
    MaiError lastWriteError() const override {
        return mFailed ? MaiError{MaiErrorCode::Internal, "Owned storage write failed."}
                       : mInner->lastWriteError();
    }
    MaiResult<std::string> getToolModelOutput(const MaiToolModelOutputKey& key) const override {
        return forwardToolOutput ? mInner->getToolModelOutput(key)
                                 : MaiSessionStore::getToolModelOutput(key);
    }
    MaiResult<std::string> saveToolModelOutput(const MaiToolModelOutputKey& key,
                                               const std::string& content) override {
        return forwardToolOutput ? mInner->saveToolModelOutput(key, content)
                                 : MaiSessionStore::saveToolModelOutput(key, content);
    }
    MaiError putToolOutput(const MaiToolOutputRecord& output) override {
        return forwardToolOutput ? mInner->putToolOutput(output)
                                 : MaiSessionStore::putToolOutput(output);
    }
    MaiResult<MaiToolOutputPage> readToolOutput(const std::string& session,
                                                const MaiToolOutputQuery& query) const override {
        if (!forwardToolOutput) return MaiSessionStore::readToolOutput(session, query);
        if (!disappearOutputId.empty() && query.outputId == disappearOutputId) {
            if (readableOutputCallsRemaining == 0)
                return {MaiErrorCode::NotFound, "Owned retained capture became unavailable"};
            --readableOutputCallsRemaining;
        }
        return mInner->readToolOutput(session, query);
    }
    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutation) override {
        return mInner->mutateSession(id, mutation);
    }
    bool getContextCheckpoint(const std::string& id, MaiContextCheckpoint& out) const override {
        return mInner->getContextCheckpoint(id, out);
    }
    MaiError advanceContextCheckpoint(const std::string& id, std::uint64_t revision,
                                      const MaiContextCheckpoint& value) override {
        return mInner->advanceContextCheckpoint(id, revision, value);
    }
    MaiError insertSpecialistTask(const MaiSpecialistTask& value) override {
        return mInner->insertSpecialistTask(value);
    }
    bool getSpecialistTask(const std::string& id, const std::string& owner,
                           MaiSpecialistTask& out) const override {
        return mInner->getSpecialistTask(id, owner, out);
    }
    MaiError deleteCompletedSpecialistTask(const std::string& id,
                                           const std::string& owner) override {
        return mInner->deleteCompletedSpecialistTask(id, owner);
    }
    MaiError appendSpecialistText(const std::string& id, const std::string& owner,
                                  const std::string& text, MaiMillis checked) override {
        return mInner->appendSpecialistText(id, owner, text, checked);
    }
    MaiError updateSpecialistProgress(const std::string& id, const std::string& owner,
                                      MaiMillis checked) override {
        return mInner->updateSpecialistProgress(id, owner, checked);
    }
    MaiError recoverStatusCheckFailures() override {
        return mInner->recoverStatusCheckFailures();
    }
    MaiError finishSpecialistTask(const std::string& id, const std::string& owner,
                                  MaiSpecialistTaskStatus status, const std::string& text,
                                  const std::string& path, MaiMillis finished) override {
        return mInner->finishSpecialistTask(id, owner, status, text, path, finished);
    }
    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const override {
        return mInner->listActiveSpecialistTasks(limit);
    }
    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t limit) const override {
        return mInner->listUnnotifiedSpecialistTasks(limit);
    }
    MaiResult<std::string> reserveSpecialistNotification(const std::string& id,
                                                         const std::string& owner,
                                                         const std::string& message,
                                                         MaiMillis attempted) override {
        return mInner->reserveSpecialistNotification(id, owner, message, attempted);
    }
    MaiError markSpecialistNotified(const std::string& id, const std::string& owner,
                                    const std::string& message, MaiMillis notified) override {
        return mInner->markSpecialistNotified(id, owner, message, notified);
    }
    MaiError markSpecialistResultAlreadyDelivered(const std::string& id, const std::string& owner,
                                                  MaiMillis notified) override {
        return mInner->markSpecialistResultAlreadyDelivered(id, owner, notified);
    }

private:
    std::unique_ptr<MaiSessionStore> mInner = makeMaiMemoryStore();
    bool mFailed = false;
};

class GatedStreamModel final : public MaiModelClient {
public:
    std::atomic<bool> emitted{false};
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        for (int index = 0; index < 16; ++index) {
            if (sink.onReasoning) sink.onReasoning(std::string(512, 'r'));
            if (sink.onText) sink.onText(std::string(256, 't'));
        }
        emitted.store(true);
        while (!release.load() && !cancel.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cancel.load() ? MaiError::make(MaiErrorCode::Canceled, "canceled by user")
                             : MaiError::ok();
    }

    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
};

class FinishedVideoTool final : public MaiTool {
public:
    explicit FinishedVideoTool(std::string path) : mPath(std::move(path)) {}

    std::string name() const override {
        return "fake_video";
    }
    std::string description() const override {
        return "Fake completed video";
    }
    std::string parametersSchema() const override {
        return "{}";
    }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        (void)context;
        const auto args = nlohmann::json::parse(arguments, nullptr, false);
        if (!args.is_object() || args.contains("message"))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                R"({"code":"invalid_input","message":"unsupported media parameter: message"})");
        return MaiToolResult::success(
            nlohmann::json{{"status", "SUCCEEDED"}, {"path", mPath}, {"reply", "Video ready"}}
                .dump());
    }

private:
    std::string mPath;
};

class UnavailableStatusTool final : public MaiTool {
public:
    std::atomic<int> calls{0};
    std::string name() const override {
        return "unavailable_status_video";
    }
    std::string description() const override {
        return "Synthetic status failure";
    }
    std::string parametersSchema() const override {
        return "{}";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++calls;
        return MaiToolResult::failure(
            MaiErrorCode::InvalidInput,
            R"({"code":"invalid_input","message":"Status query input is invalid"})");
    }
};

// 把事件流录下来，方便断言顺序和内容。
struct Recorder {
    std::mutex mu;
    std::vector<MaiEvent> events;

    void attach(MaiAgent& a) {
        a.eventBus().subscribe([this](const MaiEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
    }

    std::vector<MaiEvent> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }

    std::size_t count(MaiEventType t) {
        std::lock_guard<std::mutex> lock(mu);
        std::size_t n = 0;
        for (const auto& e : events)
            if (e.type == t) ++n;
        return n;
    }

    // 把某个 part 上的所有 delta 按顺序拼起来——这正是界面做的事，拼出来应该等于完整正文。
    std::string assemble(const std::string& field_owner_part) {
        std::lock_guard<std::mutex> lock(mu);
        std::string out;
        for (const auto& e : events)
            if (e.type == MaiEventType::MessagePartDelta && e.partId == field_owner_part)
                out += e.delta;
        return out;
    }

    std::string firstDeltaPartId() {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& e : events)
            if (e.type == MaiEventType::MessagePartDelta) return e.partId;
        return {};
    }
};

// 假模型的指针挂在 agent 的生命周期上：agent 持有 unique_ptr，这里交回去的只是借来看的观察指针。
struct AgentUnderTest {
    std::unique_ptr<MaiAgent> agent;
    MaiFakeModelClient* model = nullptr;
};

AgentUnderTest makeAgent(MaiFakeModelClient::Turn repeating, MaiAgent::Options options = {}) {
    auto model = std::make_unique<MaiFakeModelClient>();
    model->setRepeatingTurn(std::move(repeating));
    MaiFakeModelClient* observer = model.get();
    if (options.defaultModel.empty()) options.defaultModel = "glm-5.4";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), nullptr, options),
            observer};
}

// 资源归属独立保存，观察结束回调是否缺失、重复或误清替代轮次。
class TurnResourceTool final : public MaiTool {
public:
    explicit TurnResourceTool(std::string toolName) : mName(std::move(toolName)) {}
    std::map<std::string, std::string> owners;
    std::vector<std::pair<std::string, std::string>> finished;
    std::string executedTurn;
    std::vector<std::string> executedTurns;
    std::function<void()> beforeCleanup;
    std::atomic<bool>* cancelAfterExecute = nullptr;
    MaiError cleanupError;
    int cleanupThrows = 0;
    bool available = true;
    int executions = 0;

    std::string name() const override {
        return mName;
    }
    std::string description() const override {
        return "Hold a synthetic turn-owned resource.";
    }
    std::string parametersSchema() const override {
        return "{}";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    bool isAvailableForModel(const std::string&) const override {
        return available;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        ++executions;
        executedTurn = context.turnId;
        executedTurns.push_back(context.turnId);
        owners[context.sessionId] = context.turnId;
        if (cancelAfterExecute) cancelAfterExecute->store(true);
        return MaiToolResult::success("Resource acquired.");
    }
    MaiError onTurnFinished(const std::string& sessionId, const std::string& turnId) override {
        if (beforeCleanup) beforeCleanup();
        finished.emplace_back(sessionId, turnId);
        const auto owner = owners.find(sessionId);
        if (owner != owners.end() && owner->second == turnId) owners.erase(owner);
        if (cleanupThrows == 1) throw std::runtime_error("synthetic cleanup exception");
        if (cleanupThrows == 2) throw 17;
        return cleanupError;
    }

private:
    std::string mName;
};

class TurnCleanupModel final : public MaiModelClient {
public:
    enum class Outcome { Success, Failure, Exception };
    Outcome outcome = Outcome::Success;
    std::function<void()> beforeAnswer;
    int requests = 0;
    int toolRequests = 1;
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (++requests <= toolRequests) {
            sink.onToolCall({"call_turn_resource", "turn_resource", "{}"});
            return {};
        }
        if (beforeAnswer) beforeAnswer();
        if (outcome == Outcome::Failure) return {MaiErrorCode::Network, "synthetic model failure"};
        if (outcome == Outcome::Exception) throw std::runtime_error("synthetic model exception");
        sink.onText("Finished.");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
};

struct TurnCleanupFixture {
    std::unique_ptr<MaiSessionStore> store = makeMaiMemoryStore();
    MaiEventBus bus;
    MaiEventEmitter emitter{bus};
    MaiContextBuilder context;
    MaiToolRegistry tools;
    MaiSession session;
    MaiMessage assistant;
    TurnCleanupModel model;
    std::atomic<bool> cancel{false};
    std::vector<MaiEvent> errors;
    TurnResourceTool* resource = nullptr;
    bool steeringClosed = false;
    std::function<MaiResult<bool>(MaiMessage&, bool, std::vector<std::string>&)> advanceSteering;

    TurnCleanupFixture() {
        session.id = "ses_turn_cleanup";
        store->putSession(session);
        MaiMessage user;
        user.id = "msg_cleanup_user";
        user.role = MaiRole::User;
        user.parts.push_back({"prt_cleanup_user", MaiTextPart{"Acquire and finish."}, 1});
        store->putMessage(session.id, user);
        assistant.id = "msg_cleanup_original";
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        auto tool = std::make_unique<TurnResourceTool>("turn_resource");
        resource = tool.get();
        tool->beforeCleanup = [this] { CHECK(steeringClosed); };
        tools.add(std::move(tool));
        bus.subscribe([this](const MaiEvent& event) {
            if (event.type == MaiEventType::SessionError) errors.push_back(event);
        });
    }
    bool run(bool modelConfigured = true, const std::string& sessionId = {},
             const std::string& effort = {}) {
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = modelConfigured ? &model : nullptr;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = "glm-5.4";
        dependencies.reasoningEffort = effort;
        dependencies.advanceSteering = advanceSteering;
        dependencies.closeSteering = [this] { steeringClosed = true; };
        MaiTurnRunner runner(dependencies, sessionId.empty() ? session.id : sessionId, assistant);
        runner.run(cancel);
        return runner.succeeded();
    }
};

void test_turn_cleanup_covers_success_failure_and_cancel() {
    for (int outcome = 0; outcome < 4; ++outcome) {
        TurnCleanupFixture fixture;
        if (outcome == 1) fixture.model.outcome = TurnCleanupModel::Outcome::Failure;
        if (outcome == 2) fixture.resource->cancelAfterExecute = &fixture.cancel;
        if (outcome == 3) fixture.model.outcome = TurnCleanupModel::Outcome::Exception;
        const bool succeeded = fixture.run();
        if (outcome == 2) CHECK(!succeeded);
        CHECK(fixture.resource->executions == 1);
        CHECK(fixture.resource->executedTurn == fixture.assistant.id);
        CHECK(fixture.resource->finished.size() == 1);
        if (fixture.resource->finished.size() == 1) {
            CHECK(fixture.resource->finished.front().first == fixture.session.id);
            CHECK(fixture.resource->finished.front().second == fixture.assistant.id);
        }
        CHECK(fixture.resource->owners.empty());
        if (outcome == 1 || outcome == 3) {
            CHECK(!succeeded);
            CHECK(fixture.errors.size() == 1);
        } else {
            CHECK(fixture.errors.empty());
            if (outcome == 0) CHECK(succeeded);
        }
    }
}

void test_turn_cleanup_covers_early_returns() {
    for (int outcome = 0; outcome < 4; ++outcome) {
        TurnCleanupFixture fixture;
        const std::string sessionId = outcome == 1 ? "ses_missing_cleanup" : fixture.session.id;
        fixture.resource->owners[sessionId] = fixture.assistant.id;
        if (outcome == 3) fixture.cancel.store(true);
        const bool succeeded =
            fixture.run(outcome != 0, sessionId, outcome == 2 ? "unknown-effort" : "");
        CHECK(fixture.resource->executions == 0);
        CHECK(fixture.resource->finished.size() == 1);
        if (fixture.resource->finished.size() == 1) {
            CHECK(fixture.resource->finished.front().first == sessionId);
            CHECK(fixture.resource->finished.front().second == fixture.assistant.id);
        }
        CHECK(fixture.resource->owners.empty());
        if (outcome != 3) CHECK(!succeeded);
        CHECK(fixture.errors.size() == (outcome == 3 ? 0 : 1));
    }
}

void test_turn_cleanup_uses_original_identity_after_steering() {
    TurnCleanupFixture fixture;
    fixture.model.toolRequests = 3;
    int steered = 0;
    fixture.advanceSteering = [&](MaiMessage& assistant, bool,
                                  std::vector<std::string>&) -> MaiResult<bool> {
        if (steered == fixture.resource->executions) return false;
        ++steered;
        assistant = {};
        assistant.id = "msg_cleanup_steered_segment_" + std::to_string(steered);
        assistant.role = MaiRole::Assistant;
        fixture.store->putMessage(fixture.session.id, assistant);
        return true;
    };
    CHECK(fixture.run());
    CHECK(steered == 3);
    MaiMessage original;
    CHECK(fixture.store->getMessage(fixture.session.id, fixture.assistant.id, original));
    CHECK(original.turnOutcome.has_value());
    if (original.turnOutcome) {
        CHECK(original.turnOutcome->turnId == fixture.assistant.id);
        CHECK(original.turnOutcome->status == MaiTurnStatus::Completed);
        CHECK(original.turnOutcome->resultMessageId == "msg_cleanup_steered_segment_3");
        MaiMessage result;
        CHECK(fixture.store->getMessage(fixture.session.id, original.turnOutcome->resultMessageId,
                                        result));
        CHECK(result.turnId == fixture.assistant.id);
        CHECK(!result.turnOutcome);
    }
    CHECK(fixture.resource->executedTurns == std::vector<std::string>(3, fixture.assistant.id));
    CHECK(fixture.resource->finished.size() == 1);
    if (fixture.resource->finished.size() == 1)
        CHECK(fixture.resource->finished.front().second == fixture.assistant.id);
    CHECK(fixture.resource->owners.empty());

    TurnCleanupFixture replacement;
    replacement.model.beforeAnswer = [&] {
        replacement.resource->owners[replacement.session.id] = "msg_cleanup_replacement";
    };
    CHECK(replacement.run());
    CHECK(replacement.resource->finished.size() == 1);
    if (replacement.resource->finished.size() == 1)
        CHECK(replacement.resource->finished.front().second == replacement.assistant.id);
    CHECK(replacement.resource->owners[replacement.session.id] == "msg_cleanup_replacement");
}

void test_turn_cleanup_reports_failure_and_continues_other_tools() {
    for (int failure = 0; failure < 3; ++failure) {
        for (int outcome = 0; outcome < 3; ++outcome) {
            TurnCleanupFixture fixture;
            if (failure == 0)
                fixture.resource->cleanupError = {MaiErrorCode::Busy, "synthetic cleanup refusal"};
            else
                fixture.resource->cleanupThrows = failure;
            if (outcome == 1) fixture.resource->cancelAfterExecute = &fixture.cancel;
            if (outcome == 2) fixture.model.outcome = TurnCleanupModel::Outcome::Failure;
            auto follower = std::make_unique<TurnResourceTool>("internal_cleanup_resource");
            auto* observedFollower = follower.get();
            follower->available = false;
            follower->owners[fixture.session.id] = fixture.assistant.id;
            fixture.tools.addInternal(std::move(follower));
            CHECK(!fixture.run());
            CHECK(fixture.resource->finished.size() == 1);
            CHECK(observedFollower->finished.size() == 1);
            CHECK(observedFollower->owners.empty());
            CHECK(fixture.errors.size() == 1);
            if (fixture.errors.size() == 1) {
                CHECK(fixture.errors.front().turnId == fixture.assistant.id);
                CHECK(fixture.errors.front().detail.find("cleanup") != std::string::npos);
                if (outcome == 2)
                    CHECK(fixture.errors.front().detail.find("synthetic model failure") !=
                          std::string::npos);
            }
        }
    }
}

void test_registry_turn_cleanup_preserves_first_error() {
    MaiToolRegistry tools;
    auto first = std::make_unique<TurnResourceTool>("first_cleanup");
    auto* observedFirst = first.get();
    first->cleanupError = {MaiErrorCode::Busy, "first cleanup refusal"};
    tools.add(std::move(first));
    auto second = std::make_unique<TurnResourceTool>("second_cleanup");
    auto* observedSecond = second.get();
    second->cleanupThrows = 1;
    tools.addInternal(std::move(second));
    auto third = std::make_unique<TurnResourceTool>("third_cleanup");
    auto* observedThird = third.get();
    tools.add(std::move(third));
    const auto error = tools.finishTurn("ses_registry_cleanup", "msg_registry_cleanup");
    CHECK(error.code() == MaiErrorCode::Busy);
    CHECK(error.message() == "first cleanup refusal");
    CHECK(observedFirst->finished.size() == 1);
    CHECK(observedSecond->finished.size() == 1);
    CHECK(observedThird->finished.size() == 1);
}

void test_turn_cleanup_contracts() {
    test_turn_cleanup_covers_success_failure_and_cancel();
    test_turn_cleanup_covers_early_returns();
    test_turn_cleanup_uses_original_identity_after_steering();
    test_turn_cleanup_reports_failure_and_continues_other_tools();
    test_registry_turn_cleanup_preserves_first_error();
}

void test_prompt_images_reach_the_current_user_message() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(
        MaiSendPrompt{sessionId, "inspect this image", {{"attachments/photo.png", "image/png"}}});
    agent->waitIdle();

    const MaiModelRequest request = underTest.model->lastRequest();
    CHECK(request.workingDirectory == "/tmp");
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].images.size() == 1);
        if (request.messages[0].images.size() == 1) {
            CHECK(request.messages[0].images[0].path == "attachments/photo.png");
            CHECK(request.messages[0].images[0].mimeType == "image/png");
        }
    }
}

// ── 用例 ────────────────────────────────────────────────────────

void test_full_turn() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    const std::string msg_id = agent->submit(MaiSendPrompt{sessionId, "who are you?"}).value();
    CHECK(!msg_id.empty());
    CHECK(msg_id.rfind("msg_", 0) == 0);

    agent->waitIdle();

    // 1. 事件顺序：至少要有 delta，最后要有 idle
    CHECK(recorder.count(MaiEventType::MessagePartDelta) == 3);  // 三个 chunk
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
    CHECK(recorder.count(MaiEventType::SessionError) == 0);

    // 2. part id 全程稳定 —— 换 id 会让界面重绘甚至闪屏
    const auto all = recorder.snapshot();
    for (const auto& event : all)
        if (event.type == MaiEventType::SessionIdle) CHECK(event.turnId == msg_id);
    std::string partId;
    for (const auto& e : all) {
        if (e.type != MaiEventType::MessagePartDelta) continue;
        if (partId.empty()) partId = e.partId;
        CHECK(e.partId == partId);
        CHECK(e.messageId == msg_id);
        CHECK(e.field == "text");
    }
    CHECK(partId.rfind("prt_", 0) == 0);

    // 3. 界面把 delta 拼起来 == 完整正文
    CHECK(recorder.assemble(partId) == kDefaultText);

    // 4. 落库的内容一致，刷新后不会变样
    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);  // user + assistant
    if (msgs.size() == 2) {
        CHECK(msgs[0].role == MaiRole::User);
        CHECK(msgs[1].role == MaiRole::Assistant);
        CHECK(msgs[1].completed > 0);
        CHECK(msgs[1].parts.size() == 1);
        if (msgs[1].parts.size() == 1) {
            CHECK(msgs[1].parts[0].id == partId);  // 落库的 id 和事件里的一致
            const auto* t = std::get_if<MaiTextPart>(&msgs[1].parts[0].body);
            CHECK(t != nullptr);
            if (t) CHECK(t->text == kDefaultText);
        }
    }

    // 5. 标题自动从第一句话来，不再是"新会话"
    MaiSession s;
    CHECK(agent->getSession(sessionId, s));
    CHECK(s.title == "who are you?");
}

// 注意这里只管"组装出来的请求对不对"。它怎么被序列化成 OpenAI 的线格式（stream=true、
// role 的字符串拼法等）是另一层的事，归 MaiModelClientTests，那边走真 socket，
// 能看到真正发出去的字节。
void test_request_is_assembled_correctly() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", "glm-4.6"}).value();
    agent->submit(MaiSendPrompt{sessionId, "first"});
    agent->waitIdle();

    const MaiModelRequest request = model->lastRequest();
    CHECK(request.model == "glm-4.6");  // 会话上的 model 覆盖默认值
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].content == "first");
    }
}

void test_agent_options_supply_system_prompt() {
    auto model = std::make_unique<MaiFakeModelClient>();
    model->setRepeatingTurn(defaultTurn());
    MaiFakeModelClient* observer = model.get();
    MaiAgent::Options options;
    options.baseInstructions = "Return valid GitHub Flavored Markdown.";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);

    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "show a table"});
    agent.waitIdle();

    const MaiModelRequest request = observer->lastRequest();
    CHECK(request.baseInstructions == options.baseInstructions);
    CHECK(request.messages.size() == 1);
    if (request.messages.size() == 1) {
        CHECK(request.messages[0].role == MaiModelRole::User);
        CHECK(request.messages[0].content == "show a table");
    }
}

void test_multi_turn_history() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "first"});
    agent->waitIdle();
    agent->submit(MaiSendPrompt{sessionId, "second"});
    agent->waitIdle();

    // 第二轮必须带上完整历史，否则模型没有上下文
    const MaiModelRequest request = model->lastRequest();
    CHECK(request.messages.size() == 3);  // user + assistant + user
    if (request.messages.size() == 3) {
        CHECK(request.messages[0].content == "first");
        CHECK(request.messages[1].role == MaiModelRole::Assistant);
        CHECK(request.messages[1].content == kDefaultText);
        CHECK(request.messages[2].content == "second");
    }
    CHECK(agent->listMessages(sessionId).size() == 4);
}

// 会话身份只用于请求路由；工具后继、下一轮及无工具补答都不能重新生成或丢掉它。
void test_runner_session_identity_survives_tools_and_final_answer_recovery() {
    class IdentityTool final : public MaiTool {
    public:
        std::vector<std::string> sessions;
        std::string name() const override {
            return "identity_probe";
        }
        std::string description() const override {
            return "Observe the current owned session without side effects.";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object","properties":{},"additionalProperties":false})";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
            sessions.push_back(context.sessionId);
            return MaiToolResult::success("Observed the owned session.");
        }
    };
    MaiFakeModelClient::Turn call;
    call.invocations = {{"call_identity", "identity_probe", "{}"}};
    MaiFakeModelClient::Turn answered;
    answered.textChunks = {"First owned answer."};
    MaiFakeModelClient::Turn empty;
    MaiFakeModelClient::Turn final;
    final.textChunks = {"Recovered owned answer."};
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{call, answered, empty, final});
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    auto tool = std::make_unique<IdentityTool>();
    auto* observedTool = tool.get();
    tools->add(std::move(tool));
    MaiAgent::Options options;
    options.defaultModel = "anthropic/claude-fable-5.2";
    options.reasoningEffort = "medium";
    options.baseInstructions = "Keep the original instructions and reference order.";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto created = agent.submit(MaiCreateSession{"/tmp", "Owned routing session", ""});
    CHECK(created);
    if (!created) return;
    const auto& sessionId = created.value();
    CHECK(agent.submit(MaiSendPrompt{sessionId, "First owned request."}));
    agent.waitIdle();
    CHECK(agent.submit(MaiSendPrompt{sessionId, "Second owned request."}));
    agent.waitIdle();
    CHECK(observedTool->sessions == std::vector<std::string>{sessionId});
    CHECK(observedModel->requestCount() == 4);
    for (std::size_t index = 0; index < observedModel->requestCount(); ++index) {
        const auto request = observedModel->request(index);
        CHECK(request.sessionId == sessionId);
        CHECK(request.model == options.defaultModel);
        CHECK(request.reasoningEffort == options.reasoningEffort);
        CHECK(request.baseInstructions == options.baseInstructions);
        CHECK(request.maxOutputTokens == 0);
        CHECK(!request.messages.empty());
        if (!request.messages.empty()) {
            CHECK(request.messages.front().role == MaiModelRole::User);
            CHECK(request.messages.front().content == "First owned request.");
        }
        if (index < 3) {
            CHECK(request.tools.size() == 1);
            if (request.tools.size() == 1) {
                CHECK(request.tools.front().name == "identity_probe");
                CHECK(request.tools.front().parametersJson == observedTool->parametersSchema());
            }
        } else {
            CHECK(request.tools.empty());
        }
        // 新身份字段不是提示词；本地估算不能因有无路由 ID 而改变。
        auto withoutIdentity = request;
        withoutIdentity.sessionId.clear();
        const auto withEstimate = maiEstimateModelInputTokens(request);
        const auto withoutEstimate = maiEstimateModelInputTokens(withoutIdentity);
        CHECK(withEstimate.tokens == withoutEstimate.tokens);
        CHECK(withEstimate.exact == withoutEstimate.exact);
    }
    if (observedModel->requestCount() == 4) {
        const auto afterTool = observedModel->request(1);
        bool hasOriginalToolResult = false;
        for (const auto& message : afterTool.messages)
            if (message.role == MaiModelRole::ToolResult && message.toolCallId == "call_identity") {
                CHECK(message.content == "Observed the owned session.");
                hasOriginalToolResult = true;
            }
        CHECK(hasOriginalToolResult);
        bool hasSecondPrompt = false;
        for (const auto& message : observedModel->request(3).messages)
            if (message.role == MaiModelRole::User && message.content == "Second owned request.")
                hasSecondPrompt = true;
        CHECK(hasSecondPrompt);
    }
}

// 根会话与真实子会话共享客户端，但每个请求必须使用其所属的既有会话 ID。
void test_runner_session_identity_keeps_root_and_child_sessions_separate() {
    auto model = std::make_unique<MaiFakeModelClient>();
    model->setRepeatingTurn(defaultTurn());
    auto* observedModel = model.get();
    MaiAgent::Options options;
    options.defaultModel = "deepseek-flash";
    options.reasoningEffort = "high";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);
    const auto rootA = agent.submit(MaiCreateSession{"/tmp", "Owned root A", "glm-5.4", "low"});
    const auto rootB =
        agent.submit(MaiCreateSession{"/tmp", "Owned root B", "deepseek-v4-pro", "max"});
    CHECK(rootA && rootB);
    if (!rootA || !rootB) return;
    CHECK(rootA.value() != rootB.value());
    CHECK(agent.submit(MaiSendPrompt{rootA.value(), "Owned root A request."}));
    CHECK(agent.submit(MaiSendPrompt{rootB.value(), "Owned root B request."}));
    const auto child = agent.spawnSubAgent(rootA.value(), "Owned child", "Owned child request.");
    CHECK(child);
    if (!child) {
        agent.waitIdle();
        return;
    }
    agent.waitIdle();
    CHECK(child.value() != rootA.value() && child.value() != rootB.value());
    MaiSession childSession;
    CHECK(agent.getSession(child.value(), childSession));
    CHECK(childSession.parentId == rootA.value());
    CHECK(agent.submit(MaiSendPrompt{rootA.value(), "Owned root A after child."}));
    agent.waitIdle();
    const std::map<std::string, std::pair<std::string, std::string>> expected{
        {"Owned root A request.", {rootA.value(), "glm-5.4"}},
        {"Owned root B request.", {rootB.value(), "deepseek-v4-pro"}},
        {"Owned child request.", {child.value(), "glm-5.4"}},
        {"Owned root A after child.", {rootA.value(), "glm-5.4"}}};
    std::set<std::string> observedPrompts;
    CHECK(observedModel->requestCount() == expected.size());
    for (std::size_t index = 0; index < observedModel->requestCount(); ++index) {
        const auto request = observedModel->request(index);
        std::string lastOwnedPrompt;
        for (const auto& message : request.messages)
            if (message.role == MaiModelRole::User && expected.count(message.content))
                lastOwnedPrompt = message.content;
        CHECK(!lastOwnedPrompt.empty());
        if (lastOwnedPrompt.empty()) continue;
        observedPrompts.insert(lastOwnedPrompt);
        const auto& identity = expected.at(lastOwnedPrompt);
        CHECK(request.sessionId == identity.first);
        CHECK(request.model == identity.second);
        CHECK(request.reasoningEffort == (identity.second == "deepseek-v4-pro" ? "max" : "low"));
    }
    CHECK(observedPrompts.size() == expected.size());
}

void test_reasoning_goes_to_its_own_part() {
    MaiFakeModelClient::Turn turn = defaultTurn();
    turn.reasoning = "let me think";
    auto underTest = makeAgent(turn);
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "compute this"});
    agent->waitIdle();

    // reasoning 和 text 必须是两个不同的 part，界面才能分开显示
    const auto all = recorder.snapshot();
    std::vector<std::string> part_ids;
    for (const auto& e : all) {
        if (e.type != MaiEventType::MessagePartDelta) continue;
        if (std::find(part_ids.begin(), part_ids.end(), e.partId) == part_ids.end())
            part_ids.push_back(e.partId);
    }
    CHECK(part_ids.size() == 2);

    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) {
        CHECK(msgs[1].parts.size() == 2);
        if (msgs[1].parts.size() == 2) {
            CHECK(std::get_if<MaiReasoningPart>(&msgs[1].parts[0].body) != nullptr);
            CHECK(std::get_if<MaiTextPart>(&msgs[1].parts[1].body) != nullptr);
        }
    }

    // reasoning 不该回灌给模型——它是草稿，会污染下一轮上下文
    agent->submit(MaiSendPrompt{sessionId, "go on"});
    agent->waitIdle();
    bool leaked = false;
    for (const auto& message : model->lastRequest().messages)
        if (message.content.find("let me think") != std::string::npos) leaked = true;
    CHECK(!leaked);
}

MaiModelContinuationState syntheticContinuation(bool withTools, bool finalResponse) {
    MaiModelContinuationState state;
    state.format = "responses.output.v1";
    state.provider = "openrouter";
    state.model = "anthropic/claude-fable-5.2";
    if (finalResponse) {
        state.payload = R"([{"id":"rs_final","type":"reasoning",)"
                        R"("encrypted_content":"synthetic-final-cipher",)"
                        R"("signature":"synthetic-final-signature","summary":[]},)"
                        R"({"id":"msg_final","type":"message","role":"assistant",)"
                        R"("content":[{"type":"output_text","text":"Synthetic final answer."}]}])";
    } else if (withTools) {
        state.payload =
            R"([{"id":"rs_first","type":"reasoning",)"
            R"("encrypted_content":"synthetic-first-cipher",)"
            R"("signature":"synthetic-first-signature",)"
            R"("opaque":{"order":[3,1,2]},"summary":[]},)"
            R"({"id":"fc_first_a","type":"function_call","call_id":"call_continuation_a",)"
            R"("name":"continuation_probe","arguments":"{}"},)"
            R"({"id":"fc_first_b","type":"function_call","call_id":"call_continuation_b",)"
            R"("name":"continuation_probe","arguments":"{}"}])";
    } else {
        state.payload = R"([{"id":"rs_first","type":"reasoning",)"
                        R"("encrypted_content":"synthetic-first-cipher",)"
                        R"("signature":"synthetic-first-signature",)"
                        R"("opaque":{"order":[3,1,2]},"summary":[]}])";
    }
    return state;
}

bool sameContinuation(const MaiModelContinuationState& left,
                      const MaiModelContinuationState& right) {
    return left.format == right.format && left.provider == right.provider &&
           left.model == right.model && left.payload == right.payload;
}

std::vector<MaiReasoningPart> storedContinuationParts(const std::vector<MaiMessage>& history) {
    std::vector<MaiReasoningPart> result;
    for (const auto& message : history)
        for (const auto& part : message.parts)
            if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body))
                if (reasoning->continuation) result.push_back(*reasoning);
    return result;
}

class ContinuationStreamModel final : public MaiModelClient {
public:
    enum class Outcome { MetadataOnly, ToolBatch, Failure, Canceled };
    Outcome outcome = Outcome::ToolBatch;
    std::atomic<bool>* cancelDuringStream = nullptr;
    std::vector<MaiModelRequest> requests;
    int publishedStates = 0;

    MaiWireApi wireApi() const override {
        return MaiWireApi::Responses;
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        requests.push_back(request);
        if (outcome == Outcome::Failure || outcome == Outcome::Canceled) {
            // 失败流只发布不完整显示文本。成功状态 callback 的门禁归 wire 测试，
            // 此处不构造“已承诺成功状态、随后又失败”的自相矛盾客户端。
            if (sink.onReasoning) sink.onReasoning("Synthetic unfinished reasoning.");
            if (outcome == Outcome::Canceled) {
                if (cancelDuringStream) cancelDuringStream->store(true);
                return {MaiErrorCode::Canceled, "Synthetic canceled response."};
            }
            return {MaiErrorCode::Network, "Synthetic interrupted response."};
        }
        CHECK(requests.size() <= 2);
        if (requests.size() > 2)
            return {MaiErrorCode::Protocol, "Unexpected synthetic response iteration."};
        CHECK(static_cast<bool>(sink.onContinuationState));
        const bool finalResponse = requests.size() == 2;
        if (sink.onContinuationState) {
            sink.onContinuationState(
                syntheticContinuation(outcome == Outcome::ToolBatch, finalResponse));
            ++publishedStates;
        }
        // 状态更新的同步订阅者也可触发 Stop。模拟 wire 在交付工具前的取消复检，
        // 此时成功快照已交给 Runner，但后续工具 callback 尚未发生。
        if (cancel.load(std::memory_order_relaxed))
            return {MaiErrorCode::Canceled, "Synthetic cancellation during metadata publication."};
        if (finalResponse) {
            if (sink.onText) sink.onText("Synthetic final answer.");
        } else if (outcome == Outcome::ToolBatch) {
            if (sink.onToolCall) {
                sink.onToolCall({"call_continuation_a", "continuation_probe", "{}"});
                sink.onToolCall({"call_continuation_b", "continuation_probe", "{}"});
            }
        }
        return {};
    }
};

class ContinuationProbeTool final : public MaiTool {
public:
    MaiSessionStore* store = nullptr;
    int executions = 0;
    std::vector<bool> durableBeforeExecution;
    std::atomic<bool>* cancelAfterFirstExecution = nullptr;

    std::string name() const override {
        return "continuation_probe";
    }
    std::string description() const override {
        return "Observe committed synthetic continuation state before execution.";
    }
    std::string parametersSchema() const override {
        return "{}";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        ++executions;
        const auto carriers = storedContinuationParts(store->listMessages(context.sessionId));
        const bool committed =
            carriers.size() == 1 && carriers.front().text.empty() &&
            sameContinuation(*carriers.front().continuation, syntheticContinuation(true, false));
        durableBeforeExecution.push_back(committed);
        CHECK(committed);
        MaiToolExecution execution;
        execution.status = MaiToolExecutionStatus::Completed;
        execution.effectStatus = MaiToolEffectStatus::Confirmed;
        execution.started = true;
        execution.exitCode = 0;
        auto result = MaiToolResult::success("Synthetic tool completed.");
        result.withExecution(execution);
        if (cancelAfterFirstExecution && executions == 1) cancelAfterFirstExecution->store(true);
        return result;
    }
};

struct ContinuationRunnerFixture {
    std::unique_ptr<MaiSessionStore> store;
    MaiEventBus bus;
    MaiEventEmitter emitter{bus};
    MaiContextBuilder context;
    MaiToolRegistry tools;
    MaiSession session;
    MaiMessage assistant;
    ContinuationStreamModel model;
    ContinuationProbeTool* probe = nullptr;
    std::atomic<bool> cancel{false};
    std::set<std::string> updatedContinuationParts;
    std::vector<MaiEvent> errors;
    bool cancelOnContinuationUpdate = false;

    explicit ContinuationRunnerFixture(
        std::unique_ptr<MaiSessionStore> selectedStore = makeMaiMemoryStore())
        : store(std::move(selectedStore)) {
        session.id = "ses_response_continuation";
        session.title = "Synthetic continuation test";
        session.model = "anthropic/claude-fable-5.2";
        store->putSession(session);
        MaiMessage user;
        user.id = "msg_continuation_user";
        user.role = MaiRole::User;
        user.parts.push_back(
            {"prt_continuation_user", MaiTextPart{"Continue the synthetic task."}, 1});
        store->putMessage(session.id, user);
        assistant.id = "msg_continuation_assistant";
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        auto tool = std::make_unique<ContinuationProbeTool>();
        probe = tool.get();
        tool->store = store.get();
        tools.add(std::move(tool));
        model.cancelDuringStream = &cancel;
        bus.subscribe([this](const MaiEvent& event) {
            if (event.type == MaiEventType::SessionError) errors.push_back(event);
            if (event.type != MaiEventType::MessagePartUpdated) return;
            // 回调时从正式 store 精读，不能用 Runner 内存副本证明已持久化。
            for (const auto& message : store->listMessages(session.id))
                for (const auto& part : message.parts)
                    if (part.id == event.partId)
                        if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body)) {
                            if (!reasoning->continuation) continue;
                            updatedContinuationParts.insert(part.id);
                            if (cancelOnContinuationUpdate) cancel.store(true);
                        }
        });
    }

    bool run() {
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = &model;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = session.model;
        MaiTurnRunner runner(dependencies, session.id, assistant);
        runner.run(cancel);
        return runner.succeeded();
    }
};

void checkContinuationCarriersAndReplay(const ContinuationRunnerFixture& fixture, bool withTools) {
    const auto history = fixture.store->listMessages(fixture.session.id);
    const auto carriers = storedContinuationParts(history);
    CHECK(carriers.size() == 2);
    if (carriers.size() == 2) {
        CHECK(carriers[0].text.empty());
        CHECK(carriers[1].text.empty());
        CHECK(sameContinuation(*carriers[0].continuation, syntheticContinuation(withTools, false)));
        CHECK(sameContinuation(*carriers[1].continuation, syntheticContinuation(withTools, true)));
    }
    // 无显示文本也要发布独立 metadata 更新，第二次响应不能覆盖第一次的载体。
    CHECK(fixture.updatedContinuationParts.size() == 2);
    CHECK(fixture.model.publishedStates == 2);
    CHECK(fixture.model.requests.size() == 2);
    if (fixture.model.requests.size() != 2) return;
    const auto& followup = fixture.model.requests[1];
    std::size_t replayed = 0;
    std::vector<std::string> results;
    for (const auto& message : followup.messages) {
        if (message.continuation) {
            ++replayed;
            CHECK(message.role == MaiModelRole::Assistant);
            CHECK(message.content.empty());
            CHECK(sameContinuation(*message.continuation, syntheticContinuation(withTools, false)));
        }
        if (message.role == MaiModelRole::ToolResult) results.push_back(message.toolCallId);
    }
    CHECK(replayed == 1);
    if (withTools)
        CHECK((results == std::vector<std::string>{"call_continuation_a", "call_continuation_b"}));
    else
        CHECK(results.empty());
}

void test_metadata_only_response_state_survives_empty_text_and_followup() {
    ContinuationRunnerFixture fixture;
    fixture.model.outcome = ContinuationStreamModel::Outcome::MetadataOnly;
    CHECK(fixture.run());
    CHECK(fixture.probe->executions == 0);
    CHECK(fixture.errors.empty());
    checkContinuationCarriersAndReplay(fixture, false);
}

void test_response_state_is_durable_before_every_tool_and_not_duplicated() {
    ContinuationRunnerFixture fixture;
    CHECK(fixture.run());
    CHECK(fixture.probe->executions == 2);
    CHECK((fixture.probe->durableBeforeExecution == std::vector<bool>{true, true}));
    CHECK(fixture.errors.empty());
    checkContinuationCarriersAndReplay(fixture, true);
}

void test_failed_or_canceled_response_has_no_replayable_success_state() {
    for (const auto outcome :
         {ContinuationStreamModel::Outcome::Failure, ContinuationStreamModel::Outcome::Canceled}) {
        ContinuationRunnerFixture fixture;
        fixture.model.outcome = outcome;
        CHECK(!fixture.run());
        CHECK(fixture.model.publishedStates == 0);
        CHECK(fixture.model.requests.size() == 1);
        CHECK(fixture.probe->executions == 0);
        CHECK(storedContinuationParts(fixture.store->listMessages(fixture.session.id)).empty());
        CHECK(fixture.updatedContinuationParts.empty());
    }
}

void test_continuation_write_failure_prevents_tool_execution() {
    auto store = std::make_unique<ContinuationWriteFailureStore>();
    auto* observedStore = store.get();
    ContinuationRunnerFixture fixture(std::move(store));
    CHECK(!fixture.run());
    CHECK(observedStore->rejectedWrites == 1);
    CHECK(fixture.model.requests.size() == 1);
    CHECK(fixture.probe->executions == 0);
    CHECK(storedContinuationParts(fixture.store->listMessages(fixture.session.id)).empty());
    CHECK(!fixture.errors.empty());
}

void checkCanceledContinuationHasNoOrphanCalls(const ContinuationRunnerFixture& fixture,
                                               bool firstToolExecuted) {
    const auto history = fixture.store->listMessages(fixture.session.id);
    const auto followup = fixture.context.build(history);
    std::set<std::string> replayedCalls;
    std::set<std::string> projectedCalls;
    std::map<std::string, int> resultCounts;
    std::size_t replayedStates = 0;
    for (const auto& message : followup) {
        if (message.role == MaiModelRole::ToolResult) {
            ++resultCounts[message.toolCallId];
            CHECK(!message.content.empty());
        }
        if (!message.continuation) {
            for (const auto& invocation : message.invocations)
                CHECK(projectedCalls.insert(invocation.id).second);
            continue;
        }
        ++replayedStates;
        // 不能通过删掉 opaque output 中的一项来伪装修复，也不能重建签名载荷。
        CHECK(sameContinuation(*message.continuation, syntheticContinuation(true, false)));
        const auto output = nlohmann::json::parse(message.continuation->payload, nullptr, false);
        CHECK(output.is_array());
        if (!output.is_array()) continue;
        for (const auto& item : output) {
            if (!item.is_object() || item.value("type", std::string{}) != "function_call") continue;
            const auto callId = item.value("call_id", std::string{});
            CHECK(!callId.empty());
            CHECK(replayedCalls.insert(callId).second);
        }
    }
    if (replayedStates == 0) {
        // 尚未执行任何工具时可以整次撤下该 response；已发生的执行不能这样抹去。
        CHECK(!firstToolExecuted);
        CHECK(fixture.probe->executions == 0);
        CHECK(resultCounts.size() == projectedCalls.size());
        for (const auto& callId : projectedCalls) CHECK(resultCounts[callId] == 1);
        return;
    }
    CHECK(replayedStates == 1);
    CHECK((replayedCalls == std::set<std::string>{"call_continuation_a", "call_continuation_b"}));
    CHECK(resultCounts.size() == replayedCalls.size());
    for (const auto& callId : replayedCalls) CHECK(resultCounts[callId] == 1);

    std::map<std::string, MaiToolPart> persisted;
    std::map<std::string, int> persistedCounts;
    for (const auto& message : history)
        for (const auto& part : message.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                persisted[tool->callId] = *tool;
                ++persistedCounts[tool->callId];
            }
    for (const auto& callId : replayedCalls) {
        CHECK(persistedCounts[callId] == 1);
        const auto found = persisted.find(callId);
        CHECK(found != persisted.end());
        if (found == persisted.end()) continue;
        const auto& tool = found->second;
        if (firstToolExecuted && callId == "call_continuation_a") {
            CHECK(tool.state == MaiToolState::Completed);
            CHECK(tool.output == "Synthetic tool completed.");
            CHECK(tool.error.empty());
            CHECK(tool.execution.has_value());
            if (tool.execution) {
                CHECK(tool.execution->status == MaiToolExecutionStatus::Completed);
                CHECK(tool.execution->effectStatus == MaiToolEffectStatus::Confirmed);
                CHECK(tool.execution->started == std::optional<bool>(true));
                CHECK(tool.execution->exitCode == std::optional<int>(0));
                CHECK(!tool.execution->canceled);
            }
        } else {
            CHECK(tool.state == MaiToolState::Canceled);
            CHECK(!tool.output.empty());
            CHECK(!tool.error.empty());
            CHECK(tool.execution.has_value());
            if (tool.execution) {
                CHECK(tool.execution->status == MaiToolExecutionStatus::NotStarted ||
                      tool.execution->status == MaiToolExecutionStatus::Canceled);
                CHECK(tool.execution->effectStatus == MaiToolEffectStatus::None);
                CHECK(tool.execution->started == std::optional<bool>(false));
                CHECK(tool.execution->canceled);
            }
        }
    }
}

void test_cancel_during_metadata_update_does_not_leave_orphan_state_calls() {
    ContinuationRunnerFixture fixture;
    fixture.cancelOnContinuationUpdate = true;
    fixture.run();
    CHECK(fixture.cancel.load());
    CHECK(fixture.model.publishedStates == 1);
    CHECK(fixture.model.requests.size() == 1);
    CHECK(fixture.probe->executions == 0);
    CHECK(!fixture.updatedContinuationParts.empty());
    checkCanceledContinuationHasNoOrphanCalls(fixture, false);
}

void test_stop_after_first_tool_preserves_its_fact_and_closes_remaining_state_calls() {
    ContinuationRunnerFixture fixture;
    fixture.probe->cancelAfterFirstExecution = &fixture.cancel;
    fixture.run();
    CHECK(fixture.cancel.load());
    CHECK(fixture.model.requests.size() == 1);
    CHECK(fixture.probe->executions == 1);
    CHECK((fixture.probe->durableBeforeExecution == std::vector<bool>{true}));
    checkCanceledContinuationHasNoOrphanCalls(fixture, true);
}

void test_response_continuation_cancel_contracts() {
    test_cancel_during_metadata_update_does_not_leave_orphan_state_calls();
    test_stop_after_first_tool_preserves_its_fact_and_closes_remaining_state_calls();
}

void test_response_continuation_contracts() {
    test_metadata_only_response_state_survives_empty_text_and_followup();
    test_response_state_is_durable_before_every_tool_and_not_duplicated();
    test_failed_or_canceled_response_has_no_replayable_success_state();
    test_continuation_write_failure_prevents_tool_execution();
    test_response_continuation_cancel_contracts();
}

void test_interrupt() {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = {"a", "b", "c", "d", "e", "f", "g", "h"};
    turn.chunkDelayMilliseconds = 150;  // 慢慢吐，好让我们插进去
    auto underTest = makeAgent(turn);
    MaiAgent* agent = underTest.agent.get();
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "count"});

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(agent->isBusy(sessionId));
    CHECK(agent->submit(MaiInterrupt{sessionId}).isOk());

    agent->waitIdle();
    CHECK(!agent->isBusy(sessionId));

    // 中断不算错误，而且已经吐出来的内容要保住
    CHECK(recorder.count(MaiEventType::SessionError) == 0);
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
    const std::size_t got = recorder.count(MaiEventType::MessagePartDelta);
    CHECK(got > 0);
    CHECK(got < 8);  // 确实提前停了
    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    if (msgs.size() == 2) CHECK(!msgs[1].parts.empty());  // 半截内容也落库
}

void test_busy_session_rejects_second_turn() {
    auto underTest = makeAgent(defaultTurn(120));
    MaiAgent* agent = underTest.agent.get();

    const std::string sessionId = agent->submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent->submit(MaiSendPrompt{sessionId, "first"}).isOk());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // 不排队而是拒绝：排队会让用户以为消息丢了，界面上看不出区别
    CHECK(!agent->submit(MaiSendPrompt{sessionId, "second"}).isOk());
    agent->waitIdle();
    CHECK(agent->listMessages(sessionId).size() == 2);
}

void test_unknown_session() {
    auto underTest = makeAgent(defaultTurn());
    MaiAgent* agent = underTest.agent.get();
    CHECK(!agent->submit(MaiSendPrompt{"ses_nope", "hi"}).isOk());
    CHECK(!agent->submit(MaiInterrupt{"ses_nope"}).isOk());
}

void test_no_llm_configured() {
    // M1 的空转服务端就是这个配置：没有模型客户端，但不能崩
    MaiAgent agent(makeMaiMemoryStore(), nullptr);
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    CHECK(agent.submit(MaiSendPrompt{sessionId, "hi"}).isOk());
    agent.waitIdle();
    CHECK(recorder.count(MaiEventType::SessionError) == 1);
    CHECK(recorder.count(MaiEventType::SessionIdle) == 1);
}

void test_concurrent_sessions() {
    auto underTest = makeAgent(defaultTurn(40));
    MaiAgent* agent = underTest.agent.get();

    // 多个会话必须能同时跑——一个卡住不能拖累其它的
    std::vector<std::string> sids;
    for (int i = 0; i < 4; ++i)
        sids.push_back(agent->submit(MaiCreateSession{"/tmp", "", ""}).value());
    for (std::size_t index = 0; index < sids.size(); ++index)
        CHECK(agent->submit(MaiSendPrompt{sids[index], "concurrency " + std::to_string(index)})
                  .isOk());
    agent->waitIdle();
    std::set<std::string> requestPrompts;
    for (std::size_t index = 0; index < underTest.model->requestCount(); ++index) {
        const MaiModelRequest request = underTest.model->request(index);
        CHECK(request.messages.size() == 1);
        if (request.messages.size() == 1) requestPrompts.insert(request.messages.front().content);
    }
    CHECK(requestPrompts.size() == sids.size());
    for (const auto& sessionId : sids) {
        const auto msgs = agent->listMessages(sessionId);
        CHECK(msgs.size() == 2);
        if (msgs.size() == 2 && !msgs[1].parts.empty()) {
            const auto* t = std::get_if<MaiTextPart>(&msgs[1].parts[0].body);
            CHECK(t && t->text == kDefaultText);
        }
    }
}

void test_tool_completion_is_stored_before_update_event() {
    MaiFakeModelClient::Turn first;
    first.invocations.push_back(MaiToolInvocation{"call_1", "missing_tool", "{}"});
    MaiFakeModelClient::Turn second;
    second.textChunks = {"done"};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, second});
    MaiAgent agent(makeMaiMemoryStore(), std::move(model));
    const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
    std::vector<MaiToolState> observed;
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.sessionId != sessionId || event.type != MaiEventType::MessagePartUpdated) return;
        for (const MaiMessage& message : agent.listMessages(sessionId)) {
            for (const MaiMessagePart& part : message.parts) {
                if (part.id != event.partId) continue;
                if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                    observed.push_back(tool->state);
            }
        }
    });

    CHECK(agent.submit(MaiSendPrompt{sessionId, "try a missing tool"}).isOk());
    agent.waitIdle();
    CHECK(observed.size() == 2);
    if (observed.size() == 2) {
        CHECK(observed[0] == MaiToolState::Running);
        CHECK(observed[1] == MaiToolState::Error);
    }
}

void test_streamed_text_is_checkpointed_before_network_finishes() {
    const auto runCase = [](std::unique_ptr<MaiSessionStore> store) {
        auto model = std::make_unique<GatedStreamModel>();
        GatedStreamModel* gated = model.get();
        MaiAgent agent(std::move(store), std::move(model));
        const std::string sessionId = agent.submit(MaiCreateSession{"/tmp", "", ""}).value();
        CHECK(agent.submit(MaiSendPrompt{sessionId, "long answer"}).isOk());
        for (int i = 0; i < 200 && !gated->emitted.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(gated->emitted.load());

        const auto sizes = [&] {
            std::size_t reasoningBytes = 0;
            std::size_t textBytes = 0;
            for (const MaiMessage& message : agent.listMessages(sessionId)) {
                if (message.role != MaiRole::Assistant) continue;
                for (const MaiMessagePart& part : message.parts) {
                    if (const auto* reasoning = std::get_if<MaiReasoningPart>(&part.body))
                        reasoningBytes += reasoning->text.size();
                    if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                        textBytes += text->text.size();
                }
            }
            return std::pair<std::size_t, std::size_t>{reasoningBytes, textBytes};
        };
        const auto during = sizes();
        CHECK(during.first >= 7 * 1024);
        CHECK(during.second >= 3 * 1024);
        gated->release.store(true);
        agent.waitIdle();
        const auto final = sizes();
        CHECK(final.first == 8 * 1024);
        CHECK(final.second == 4 * 1024);
    };

    runCase(makeMaiMemoryStore());
    auto opened = makeMaiSqliteStore(":memory:");
    CHECK(opened.isOk());
    if (opened.isOk()) runCase(std::move(opened.value()));
}

// 工具故意停在两个可观察阶段，验证前面的正文在工具尚未完成时已经显示和保存。
class GatedProgressTool final : public MaiTool {
public:
    std::atomic<int> entered{0};
    std::atomic<int> released{0};
    std::string name() const override {
        return "progress_probe";
    }
    std::string description() const override {
        return "Read-only progress test";
    }
    std::string parametersSchema() const override {
        return "{}";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        const int stage = ++entered;
        while (released.load() < stage && !context.cancel->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return MaiToolResult::success("observed " + std::to_string(stage));
    }
};

std::vector<std::string> progressSequence(const std::vector<MaiMessage>& messages) {
    std::vector<std::string> sequence;
    for (const auto& message : messages) {
        if (message.role != MaiRole::Assistant) continue;
        for (const auto& part : message.parts) {
            if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                sequence.push_back(text->text);
            else if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                sequence.push_back(tool->callId);
        }
    }
    return sequence;
}

void test_intermediate_text_survives_tool_waits_and_sqlite_reopen() {
    const auto runCase = [](std::unique_ptr<MaiSessionStore> store) {
        MaiFakeModelClient::Turn first;
        first.textChunks = {"A"};
        first.invocations.push_back({"X", "progress_probe", "{}"});
        MaiFakeModelClient::Turn second;
        second.textChunks = {"B"};
        second.invocations.push_back({"Y", "progress_probe", "{}"});
        MaiFakeModelClient::Turn final;
        final.textChunks = {"C"};
        auto tools = std::make_unique<MaiToolRegistry>();
        auto gate = std::make_unique<GatedProgressTool>();
        auto* observed = gate.get();
        tools->add(std::move(gate));
        auto model = std::make_unique<MaiFakeModelClient>(
            std::vector<MaiFakeModelClient::Turn>{first, second, final});
        MaiAgent agent(std::move(store), std::move(model), std::move(tools));
        Recorder recorder;
        recorder.attach(agent);
        const auto root = MaiFileSystem::temporaryDirectory().toUtf8();
        const std::string session = agent.submit(MaiCreateSession{root, "", ""}).value();
        agent.submit(MaiSendPrompt{session, "observe two steps"});
        for (int stage = 1; stage <= 2; ++stage) {
            for (int wait = 0; wait < 400 && observed->entered.load() < stage; ++wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            CHECK(observed->entered.load() == stage);
            const auto expected = stage == 1 ? std::vector<std::string>{"A", "X"}
                                             : std::vector<std::string>{"A", "X", "B", "Y"};
            CHECK(progressSequence(agent.listMessages(session)) == expected);
            std::string visible;
            for (const auto& event : recorder.snapshot())
                if (event.type == MaiEventType::MessagePartDelta) visible += event.delta;
            CHECK(visible == (stage == 1 ? "A" : "AB"));
            observed->released.store(stage);
        }
        agent.waitIdle();
        CHECK(progressSequence(agent.listMessages(session)) ==
              (std::vector<std::string>{"A", "X", "B", "Y", "C"}));
        return session;
    };
    runCase(makeMaiMemoryStore());
    const auto database = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-progress-" + MaiIdGenerator::newSessionId() + ".db"));
    auto opened = makeMaiSqliteStore(database.toUtf8());
    CHECK(opened.isOk());
    if (!opened) return;
    const std::string session = runCase(std::move(opened.value()));
    auto reopened = makeMaiSqliteStore(database.toUtf8());
    CHECK(reopened.isOk());
    if (reopened) {
        CHECK(progressSequence(reopened.value()->listMessages(session)) ==
              (std::vector<std::string>{"A", "X", "B", "Y", "C"}));
        reopened.value().reset();
    }
    MaiFileSystem::removeFile(database);
}

// 验证子任务只有文本回复、没有媒体文件时也能回到父 Agent。
// 回灌内容必须来自真实子任务结果，不能凭空生成文件路径。
void test_specialist_text_reply_reaches_parent_without_a_file() {
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observer = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = "/tmp";
    session.created = MaiTime::getCurrentTime();
    session.updated = session.created;
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "text_specialist";
    task.intent = "Summarize the result";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    CHECK(!store->appendSpecialistText(task.id, session.id, "child answer", task.created));
    CHECK(!store->finishSpecialistTask(task.id, session.id, MaiSpecialistTaskStatus::Succeeded, "",
                                       "", task.created + 1));
    const std::string messageId = MaiIdGenerator::newMessageId();
    CHECK(store->reserveSpecialistNotification(task.id, session.id, messageId, 1));
    MaiMessage interrupted;
    interrupted.id = messageId;
    interrupted.role = MaiRole::Assistant;
    interrupted.created = task.created + 2;
    interrupted.completed = task.created + 3;
    MaiMessagePart partial;
    partial.id = MaiIdGenerator::newPartId();
    partial.body = MaiTextPart{"interrupted half-answer"};
    partial.created = interrupted.created;
    interrupted.parts.push_back(std::move(partial));
    store->putMessage(session.id, interrupted);
    MaiFakeModelClient::Turn first;
    first.invocations.push_back(MaiToolInvocation{"check_1", "missing_tool", "{}"});
    MaiFakeModelClient::Turn answer;
    answer.textChunks = {"Parent reviewed the child answer."};
    auto model =
        std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{first, answer});
    MaiFakeModelClient* modelObserver = model.get();
    MaiAgent agent(std::move(store), std::move(model));
    MaiSpecialistTask loaded;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (observer->getSpecialistTask(task.id, session.id, loaded) && loaded.notifiedAt != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(loaded.notifiedAt != 0);
    CHECK(loaded.notificationMessageId == messageId);
    agent.waitIdle();
    CHECK(modelObserver->requestCount() == 2);
    const auto request = modelObserver->request(0);
    CHECK(request.sessionId == session.id);
    CHECK(request.messages.size() == 2);
    if (request.messages.size() == 2) {
        CHECK(request.messages[0].role == MaiModelRole::System);
        CHECK(request.messages[1].content.find("child answer") != std::string::npos);
        CHECK(request.messages[1].content.find("Summarize the result") != std::string::npos);
    }
    const auto afterTool = modelObserver->request(1);
    CHECK(afterTool.sessionId == session.id);
    CHECK(afterTool.messages.size() == 5);
    if (afterTool.messages.size() == 5) {
        CHECK(afterTool.messages[1].content.find("child answer") != std::string::npos);
        CHECK(afterTool.messages[2].role == MaiModelRole::Assistant);
        CHECK(afterTool.messages[3].role == MaiModelRole::ToolResult);
        CHECK(afterTool.messages[3].toolCallId == "check_1");
        CHECK(afterTool.messages[4].role == MaiModelRole::User);
        const auto observation = nlohmann::json::parse(afterTool.messages[4].content);
        CHECK(observation["type"] == "tool_execution_observation");
        CHECK(observation["executions"].size() == 1);
        CHECK(observation["executions"][0]["call_id"] == "check_1");
        CHECK(observation["executions"][0]["execution"]["status"] == "not_started");
    }
    const auto messages = agent.listMessages(session.id);
    CHECK(messages.size() == 1);
    if (messages.size() == 1) {
        CHECK(messages[0].id == messageId);
        CHECK(messages[0].text() == "Parent reviewed the child answer.");
    }
}

void test_completed_video_is_forwarded_without_a_user_poll() {
    const MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-specialist-" + MaiIdGenerator::generate("tmp_") + ".mp4"));
    CHECK(!MaiFileSystem::writeFile(path, "fake video bytes"));
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observer = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = MaiFileSystem::temporaryDirectory().toUtf8();
    session.created = MaiTime::getCurrentTime();
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "fake_video";
    task.providerTaskId = "provider-123";
    task.intent = "Create a video";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<FinishedVideoTool>(path.toUtf8()));
    auto model = std::make_unique<MaiFakeModelClient>();
    MaiFakeModelClient::Turn answer;
    answer.textChunks = {"The video is ready."};
    model->setRepeatingTurn(answer);
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    MaiSpecialistTask loaded;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (observer->getSpecialistTask(task.id, session.id, loaded) && loaded.notifiedAt != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(loaded.notifiedAt != 0);
    CHECK(loaded.outputPath == path.toUtf8());
    agent.waitIdle();
    const auto messages = agent.listMessages(session.id);
    CHECK(messages.size() == 1);
    if (messages.size() == 1) CHECK(messages[0].text() == "The video is ready.");
    MaiFileSystem::removeFile(path);
}

void test_status_check_error_keeps_accepted_task_active() {
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observer = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = MaiFileSystem::temporaryDirectory().toUtf8();
    session.created = MaiTime::getCurrentTime();
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "unavailable_status_video";
    task.providerTaskId = "provider-already-accepted";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    auto tools = std::make_unique<MaiToolRegistry>();
    auto checker = std::make_unique<UnavailableStatusTool>();
    UnavailableStatusTool* observedChecker = checker.get();
    tools->add(std::move(checker));
    MaiAgent agent(std::move(store), std::make_unique<MaiFakeModelClient>(), std::move(tools));
    MaiSpecialistTask loaded;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (observedChecker->calls.load() > 0 &&
            observer->getSpecialistTask(task.id, session.id, loaded) && loaded.lastCheckedAt != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(observedChecker->calls.load() > 0);
    CHECK(loaded.status == MaiSpecialistTaskStatus::Running);
    CHECK(loaded.providerTaskId == task.providerTaskId);
    CHECK(observer->listUnnotifiedSpecialistTasks(10).empty());
}

void test_old_status_failure_recovers_and_delivers_original_task() {
    const MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-recovered-" + MaiIdGenerator::generate("tmp_") + ".mp4"));
    CHECK(!MaiFileSystem::writeFile(path, "fake recovered video bytes"));
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observer = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = MaiFileSystem::temporaryDirectory().toUtf8();
    session.created = MaiTime::getCurrentTime();
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "fake_video";
    task.providerTaskId = "provider-already-accepted";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    CHECK(!store->finishSpecialistTask(task.id, session.id, MaiSpecialistTaskStatus::Failed,
                                       "Status check failed: unsupported media parameter: message",
                                       {}, session.created + 1));
    const auto oldNotice = store->reserveSpecialistNotification(
        task.id, session.id, "old-status-notice", session.created + 2);
    CHECK(oldNotice.isOk());
    if (oldNotice.isOk())
        CHECK(!store->markSpecialistNotified(task.id, session.id, oldNotice.value(),
                                             session.created + 3));
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<FinishedVideoTool>(path.toUtf8()));
    auto model = std::make_unique<MaiFakeModelClient>();
    MaiFakeModelClient::Turn answer;
    answer.textChunks = {"Recovered the original task."};
    model->setRepeatingTurn(answer);
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    MaiSpecialistTask loaded;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (observer->getSpecialistTask(task.id, session.id, loaded) && loaded.notifiedAt != 0 &&
            loaded.status == MaiSpecialistTaskStatus::Succeeded)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(loaded.status == MaiSpecialistTaskStatus::Succeeded);
    CHECK(loaded.outputPath == path.toUtf8());
    CHECK(loaded.notificationMessageId != "old-status-notice");
    agent.waitIdle();
    MaiFileSystem::removeFile(path);
}

// 验证同一视频任务已交付时不会再次自动发送重复媒体卡片。
// 按任务标识去重，不受应用沙箱路径变化影响。
void test_automatic_video_reply_skips_an_existing_media_delivery() {
    const MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-delivered-" + MaiIdGenerator::generate("tmp_") + ".mp4"));
    CHECK(!MaiFileSystem::writeFile(path, "previously delivered video bytes"));
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observer = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = MaiFileSystem::temporaryDirectory().toUtf8();
    session.created = MaiTime::getCurrentTime();
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "fake_video";
    task.providerTaskId = "3941909b-ed9d-4beb-9030-70e9576ca0ba";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    MaiMessage manual;
    manual.id = MaiIdGenerator::newMessageId();
    manual.role = MaiRole::Assistant;
    manual.created = session.created + 1;
    manual.completed = session.created + 2;
    MaiMessagePart media;
    media.id = MaiIdGenerator::newPartId();
    media.created = manual.created;
    MaiToolPart sent;
    sent.tool = "agent_send_media";
    sent.input = "{\"file_path\":\"wan3-" + task.providerTaskId + ".mp4\"}";
    sent.state = MaiToolState::Completed;
    media.body = std::move(sent);
    manual.parts.push_back(std::move(media));
    store->putMessage(session.id, manual);
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<FinishedVideoTool>(path.toUtf8()));
    auto model = std::make_unique<MaiFakeModelClient>();
    MaiFakeModelClient* observedModel = model.get();
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    MaiSpecialistTask loaded;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (observer->getSpecialistTask(task.id, session.id, loaded) && loaded.notifiedAt != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(loaded.status == MaiSpecialistTaskStatus::Succeeded);
    CHECK(loaded.notifiedAt != 0);
    CHECK(loaded.notificationMessageId.empty());
    CHECK(observedModel->requestCount() == 0);
    CHECK(observer->listMessages(session.id).size() == 1);
    MaiFileSystem::removeFile(path);
}

// 通过普通压缩入口整理工具日志，再让真实 Agent/TurnRunner 调用原史工具找回旧 URL。
// 压力属于工具输出，不能用长用户正文来触发有损摘要；用户和助手正文始终保留。
// 不调用供应商，也不把“URL 仍在 DB”当成已完成回查。
void test_compacted_turn_reads_original_link_without_user_resend() {
    std::printf("   compacted turn retrieves original source URL through session_history\n");
    const std::string originalUrl =
        "https://original.example.net/source/archive-17.zip?revision=17";
    class RecallModel final : public MaiModelClient {
    public:
        std::string expectedUrl;
        std::string originalMessageId;
        int summaryCalls = 0;
        int mainCalls = 0;
        bool retrievedExactSource = false;
        MaiModelContextLimits contextLimits(const std::string&) const override {
            return {32768, 2048};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            if (request.baseInstructions.find("Summarize earlier conversation") !=
                std::string::npos) {
                ++summaryCalls;
                if (sink.onText)
                    sink.onText(
                        R"({"goals":["Continue from the saved source"],"constraints":["Preserve original files"],"decisions":[],"progress":["Source inspected; work unfinished"],"open_questions":[],"source_notes":["Read original history for exact evidence"]})");
                return {};
            }
            ++mainCalls;
            CHECK(request.maxOutputTokens == 0);
            if (mainCalls == 1) {
                if (sink.onToolCall)
                    sink.onToolCall(
                        {"history_original", "session_history",
                         R"({"search":"original.example.net/source/","max_bytes":4096,"max_messages":2})"});
            } else {
                for (const auto& message : request.messages) {
                    if (message.role != MaiModelRole::ToolResult ||
                        message.toolCallId != "history_original")
                        continue;
                    const auto data = nlohmann::json::parse(message.content, nullptr, false);
                    if (!data.is_object() || !data.contains("excerpts")) continue;
                    for (const auto& item : data["excerpts"]) {
                        if (item.value("message_id", std::string{}) == originalMessageId &&
                            item.value("text", std::string{}).find(expectedUrl) !=
                                std::string::npos)
                            retrievedExactSource = true;
                    }
                }
                if (sink.onText)
                    sink.onText(retrievedExactSource ? "Recovered original source: " + expectedUrl
                                                     : "Original source retrieval failed.");
            }
            return {};
        }
    };
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.title = "Original source continuity";
    session.model = "fixture-small-context";
    store->putSession(session);
    std::string originalId;
    for (int index = 0; index < 5; ++index) {
        MaiMessage user;
        user.id = MaiIdGenerator::newMessageId();
        user.role = MaiRole::User;
        user.created = 1;
        user.completed = 2;
        std::string text = index == 0 ? originalUrl : std::string("Preserve the existing source.");
        user.parts.push_back({MaiIdGenerator::newPartId(), MaiTextPart{std::move(text)}, 1});
        store->putMessage(session.id, user);
        if (index == 0) originalId = user.id;
        MaiMessage assistant;
        assistant.id = MaiIdGenerator::newMessageId();
        assistant.role = MaiRole::Assistant;
        assistant.created = 2;
        assistant.completed = 3;
        assistant.parts.push_back(
            {MaiIdGenerator::newPartId(), MaiTextPart{"The work remains unfinished."}, 2});
        MaiToolPart log;
        log.tool = "owned_recall_log";
        log.callId = "old_recall_" + std::to_string(index);
        log.input = "{}";
        log.output = std::string(8000, 'l');
        log.state = MaiToolState::Completed;
        assistant.parts.push_back({MaiIdGenerator::newPartId(), log, 2});
        store->putMessage(session.id, assistant);
    }
    auto* observedStore = store.get();
    auto model = std::make_unique<RecallModel>();
    model->expectedUrl = originalUrl;
    model->originalMessageId = originalId;
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools);
    MaiModelRequest compactInput;
    compactInput.model = session.model;
    compactInput.tools = tools->specs(session.model);
    MaiContextCompactor::Options policy;
    policy.allowToolProjection = true;
    policy.preserveDialogue = true;
    policy.force = true;
    const std::atomic<bool> cancel{false};
    const auto prepared = MaiContextCompactor(policy).prepare(
        *observedStore, *model, MaiContextBuilder{}, session.id,
        observedStore->listMessages(session.id), compactInput, cancel);
    CHECK(prepared && prepared.value().advancedCheckpoint);
    if (!prepared) return;
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(agent.submit(MaiSendPrompt{session.id, "Continue using my earlier original link."}));
    agent.waitIdle();
    CHECK(observedModel->summaryCalls == 0);
    CHECK(observedModel->mainCalls == 2);
    CHECK(observedModel->retrievedExactSource);
    MaiContextCheckpoint checkpoint;
    CHECK(observedStore->getContextCheckpoint(session.id, checkpoint));
    const auto history = agent.listMessages(session.id);
    CHECK(history.size() == 12);
    CHECK(history.front().id == originalId);
    CHECK(history.front().text().find(originalUrl) != std::string::npos);
    CHECK(history.back().text().find(originalUrl) != std::string::npos);
}

std::string centralProjectionOutputId(const nlohmann::json& value) {
    if (!value.is_object()) return {};
    const auto id = value.find("output_id");
    return id != value.end() && id->is_string() ? id->get<std::string>() : std::string{};
}

void test_central_tool_output_bounds_verbose_inventory_and_keeps_raw_recovery(
    bool smallWindow = false) {
    std::printf("   central output: owned verbose inventory / 971 paths / %s model window\n",
                smallWindow ? "8192-token" : "262144-token");
    struct OwnedStorage {
        MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-output-budget-" + MaiIdGenerator::newSessionId()));
        ~OwnedStorage() {
            MaiDirectoryDeleteOptions cleanup;
            cleanup.recursive = true;
            cleanup.pathAllowed = [this](const MaiFilePath& path) {
                return path == root || root.isParentOf(path);
            };
            CHECK(!MaiFileSystem::deleteDirectory(root, cleanup).error);
        }
    } owned;
    const auto temporary = owned.root.append(MaiFilePath::fromUtf8("temporary"));
    const auto cache = owned.root.append(MaiFilePath::fromUtf8("cache"));
    const auto workspace = owned.root.append(MaiFilePath::fromUtf8("workspace"));
    const auto selected = cache.append(MaiFilePath::fromUtf8("owned"));
    CHECK(!MaiFileSystem::createDirectories(temporary));
    CHECK(!MaiFileSystem::createDirectories(selected));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    CHECK(!MaiFileSystem::writeFile(
        selected.append(MaiFilePath::fromUtf8("owned-970-" + std::string(72, 'a') + ".cache")),
        "owned bytes"));
    struct Observation {
        std::string raw;
        int executions = 0;
        bool adapterTruncated = false;
    } observed;
    // 默认 app_storage 已返回有界摘要；这里显式拥有冗长原文，独立验证中心投影与原文补读。
    class VerboseInventoryTool final : public MaiTool {
    public:
        explicit VerboseInventoryTool(Observation& observation) : mObservation(observation) {}
        std::string name() const override {
            return "owned_verbose_inventory";
        }
        std::string description() const override {
            return "Return all owned synthetic inventory paths without changing files.";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object"})";
        }
        bool requiresApproval(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            auto candidates = nlohmann::json::array();
            for (int index = 0; index < 971; ++index)
                candidates.push_back({{"path", "owned/inventory/" + std::to_string(index) + "/" +
                                                   std::string(240, 'a') + ".cache"},
                                      {"bytes", 11}});
            auto result = MaiToolResult::success(nlohmann::json{
                {"dry_run", true},
                {"cleanup_candidate_count", 971},
                {"preview_id", "owned-inventory-preview"},
                {"partial", true},
                {"skipped_lines", 1},
                {"next_offset", 100},
                {"scan_complete", false},
                {"cleanup_candidates",
                 std::move(candidates)}}.dump());
            mObservation.raw = result.output();
            mObservation.adapterTruncated = result.isTruncated();
            ++mObservation.executions;
            return result;
        }

    private:
        Observation& mObservation;
    };
    class CaptureModel final : public MaiModelClient {
    public:
        std::vector<MaiModelRequest> requests;
        MaiModelContextLimits limits{262144, 8192};
        MaiSessionStore* store = nullptr;
        Observation* source = nullptr;
        std::string sessionId;
        bool projectionRecoverableAtDispatch = false;
        mutable bool emptyModelLookup = false;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiModelContextLimits contextLimits(const std::string& model) const override {
            // The default large window exposes the missing per-tool cap. The
            // small-window case also protects the next request's input capacity.
            if (model.empty()) {
                emptyModelLookup = true;
                return {};
            }
            return limits;
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            requests.push_back(request);
            if (requests.size() == 2) {
                for (const auto& message : request.messages) {
                    if (message.role != MaiModelRole::ToolResult ||
                        message.toolCallId != "owned-storage-preview")
                        continue;
                    const auto wrapper = nlohmann::json::parse(message.content, nullptr, false);
                    if (!wrapper.is_object() || !store || !source) continue;
                    const auto id = centralProjectionOutputId(wrapper);
                    if (id.empty()) continue;
                    const auto page = store->readToolOutput(sessionId, MaiToolOutputQuery{id});
                    projectionRecoverableAtDispatch =
                        page && page.value().totalBytes == source->raw.size() &&
                        page.value().text == source->raw.substr(0, page.value().sourceBytesRead);
                }
            }
            if (requests.size() == 1 && sink.onToolCall)
                sink.onToolCall({"owned-storage-preview", "owned_verbose_inventory", "{}"});
            else if (sink.onText)
                sink.onText("Owned storage preview inspected; nothing was deleted.");
            return {};
        }
    };
    auto store = makeMaiMemoryStore();
    auto* observedStore = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = workspace.toUtf8();
    store->putSession(session);
    auto model = std::make_unique<CaptureModel>();
    if (smallWindow) model->limits = {8192, 1024};
    model->store = observedStore;
    model->source = &observed;
    model->sessionId = session.id;
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<VerboseInventoryTool>(observed));
    tools->add(makeMaiToolOutputReadTool());
    bool completionEventHadFullStoredRaw = false;
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type != MaiEventType::MessagePartUpdated) return;
        MaiMessage message;
        if (!observedStore->getMessage(session.id, event.messageId, message)) return;
        for (const auto& part : message.parts) {
            const auto* tool = std::get_if<MaiToolPart>(&part.body);
            if (part.id != event.partId || !tool || tool->state != MaiToolState::Completed)
                continue;
            completionEventHadFullStoredRaw = tool->output == observed.raw;
        }
    });
    const std::string instruction =
        "Preview the owned cache only; keep all 971 paths recoverable and never delete anything.";
    CHECK(agent.submit(MaiSendPrompt{session.id, instruction}));
    agent.waitIdle();
    CHECK(observed.executions == 1);
    CHECK(!observed.adapterTruncated);
    CHECK(observed.raw.size() > 100000);
    const auto complete = nlohmann::json::parse(observed.raw, nullptr, false);
    CHECK(complete.is_object());
    if (complete.is_object()) {
        CHECK(complete.value("dry_run", false));
        CHECK(complete.value("cleanup_candidate_count", 0) == 971);
        CHECK(complete["cleanup_candidates"].size() == 971);
        CHECK(complete.value("partial", false));
        CHECK(complete.value("skipped_lines", 0) == 1);
        CHECK(complete.value("next_offset", 0) == 100);
        CHECK(!complete.value("scan_complete", true));
    }
    CHECK(observedModel->requests.size() == 2);
    CHECK(!observedModel->emptyModelLookup);
    std::size_t requestBytes = 0;
    std::string outputId;
    if (observedModel->requests.size() == 2) {
        bool preservedInstruction = false;
        for (const auto& message : observedModel->requests[1].messages) {
            if (message.role == MaiModelRole::ToolResult &&
                message.toolCallId == "owned-storage-preview") {
                requestBytes = message.content.size();
                const auto projection = nlohmann::json::parse(message.content, nullptr, false);
                CHECK(projection.is_object());
                outputId = centralProjectionOutputId(projection);
                if (projection.is_object()) {
                    CHECK(projection.value("context_projection_truncated", false));
                    CHECK(projection.value("capture_available", false));
                    CHECK(projection.value("capture_complete", false));
                    CHECK(projection.value("original_bytes", std::size_t{0}) ==
                          observed.raw.size());
                    CHECK(projection.value("retained_bytes", std::size_t{0}) ==
                          observed.raw.size());
                    CHECK(projection.value("recovery_tool", std::string{}) == "tool_output_read");
                    const auto fields =
                        projection.value("structured_fields", nlohmann::json::object());
                    CHECK(fields.value("preview_id", std::string{}) ==
                          complete.value("preview_id", std::string{}));
                    CHECK(fields.value("partial", false));
                    CHECK(fields.value("skipped_lines", 0) == 1);
                    CHECK(fields.value("next_offset", 0) == 100);
                    CHECK(!fields.value("scan_complete", true));
                    const auto recovery =
                        projection.value("recovery_args", nlohmann::json::object());
                    CHECK(centralProjectionOutputId(recovery) == outputId);
                    CHECK(recovery.value("max_bytes", 0) >= 4 &&
                          recovery.value("max_bytes", 0) <= 4096);
                }
            }
            preservedInstruction |=
                message.role == MaiModelRole::User && message.content == instruction;
        }
        CHECK(preservedInstruction);
    }
    std::printf("     verbose inventory raw=%zu bytes; next ToolResult=%zu bytes\n",
                observed.raw.size(), requestBytes);
    CHECK(requestBytes > 0 && requestBytes < observed.raw.size() / 2);
    CHECK(requestBytes <= (smallWindow ? std::size_t{8192} : std::size_t{64 * 1024}));
    MaiToolPart stored;
    for (const auto& message : agent.listMessages(session.id))
        for (const auto& part : message.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body);
                tool && tool->callId == "owned-storage-preview")
                stored = *tool;
    CHECK(stored.state == MaiToolState::Completed);
    CHECK(stored.output == observed.raw);
    CHECK(!stored.execution);
    CHECK(!outputId.empty());
    CHECK(observedModel->projectionRecoverableAtDispatch);
    CHECK(completionEventHadFullStoredRaw);
    if (!outputId.empty()) {
        auto reader = makeMaiToolOutputReadTool();
        MaiToolContext context;
        context.sessionId = session.id;
        context.toolOutputStore = observedStore;
        std::string recovered;
        std::size_t offset = 0;
        do {
            const auto page = reader->execute(
                nlohmann::json{
                    {"output_id", outputId}, {"offset_bytes", offset}, {"max_bytes", 4096}}
                    .dump(),
                context);
            CHECK(!page.hasError());
            if (page.hasError()) break;
            const auto result = nlohmann::json::parse(page.output());
            CHECK(result.value("capture_complete", false));
            recovered += result.value("text", std::string{});
            const auto next = result.value("next_offset_bytes", std::size_t{0});
            CHECK(next == 0 || next > offset);
            offset = next;
        } while (offset != 0);
        CHECK(recovered == observed.raw);
    }
    CHECK(observed.executions == 1);
    CHECK(MaiFileSystem::exists(
        selected.append(MaiFilePath::fromUtf8("owned-970-" + std::string(72, 'a') + ".cache"))));
}

void test_central_tool_output_preserves_failure_facts_and_handles_unavailable_retention() {
    for (const bool unsupportedRetention : {false, true}) {
        std::printf("   central output: %s\n", unsupportedRetention
                                                   ? "unsupported snapshot storage"
                                                   : "failure facts and original output ID");
        struct Observation {
            std::string raw =
                "owned-error-begin\n" + std::string(160 * 1024, 'b') + "\nowned-error-end";
            std::string originalId;
            std::string originalBytes = "owned-original-source\n" + std::string(32768, 'z');
        } observed;
        class LargeResultTool final : public MaiTool {
        public:
            LargeResultTool(Observation& observation, bool failure)
                : mObservation(observation), mFailure(failure) {}
            std::string name() const override {
                return "owned_large_result";
            }
            std::string description() const override {
                return "Return an owned result without external effects.";
            }
            std::string parametersSchema() const override {
                return R"({"type":"object"})";
            }
            bool requiresWorkingDirectory(const std::string&) const override {
                return false;
            }
            MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
                if (!mFailure) return MaiToolResult::success(mObservation.raw);
                MaiToolOutputRecord original;
                original.id = MaiIdGenerator::generate("out");
                original.sessionId = context.sessionId;
                original.messageId = context.messageId;
                original.partId = context.partId;
                original.callId = context.callId;
                original.text = mObservation.originalBytes;
                original.truncated = true;
                original.created = MaiTime::getCurrentTime();
                CHECK(context.toolOutputStore);
                if (!context.toolOutputStore)
                    return MaiToolResult::failure(MaiErrorCode::Internal,
                                                  "Owned output store missing");
                CHECK(!context.toolOutputStore->putToolOutput(original));
                mObservation.originalId = original.id;
                MaiToolExecution execution;
                execution.started = true;
                execution.status = MaiToolExecutionStatus::Failed;
                execution.effectStatus = MaiToolEffectStatus::Confirmed;
                execution.exitCode = 7;
                execution.outputTruncated = true;
                execution.outputId = original.id;
                execution.taskId = "owned-existing-task";
                execution.nextAction = MaiToolNextAction::InspectState;
                return MaiToolResult::failure(MaiErrorCode::Internal, mObservation.raw)
                    .withExecution(std::move(execution));
            }

        private:
            Observation& mObservation;
            bool mFailure;
        };
        // This existing forwarding store deliberately leaves the optional
        // ToolOutput API unsupported; ordinary message writes still succeed.
        std::unique_ptr<MaiSessionStore> store =
            unsupportedRetention ? std::unique_ptr<MaiSessionStore>(
                                       std::make_unique<ContinuationWriteFailureStore>())
                                 : makeMaiMemoryStore();
        auto* observedStore = store.get();
        MaiSession session;
        session.id = MaiIdGenerator::newSessionId();
        store->putSession(session);
        MaiFakeModelClient::Turn first;
        first.invocations.push_back({"owned-large-call", "owned_large_result", "{}"});
        auto model = std::make_unique<MaiFakeModelClient>(
            std::vector<MaiFakeModelClient::Turn>{first, defaultTurn()});
        auto* observedModel = model.get();
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(std::make_unique<LargeResultTool>(observed, !unsupportedRetention));
        tools->add(makeMaiToolOutputReadTool());
        MaiAgent agent(std::move(store), std::move(model), std::move(tools));
        CHECK(agent.submit(MaiSendPrompt{
            session.id, "Inspect the owned result; never repeat the original action."}));
        agent.waitIdle();
        CHECK(observedModel->requestCount() == 2);
        nlohmann::json projection;
        std::size_t projectedBytes = 0;
        std::string actualContent;
        bool originalHandleStillVisible = false;
        if (observedModel->requestCount() == 2) {
            for (const auto& message : observedModel->request(1).messages) {
                if (!observed.originalId.empty())
                    originalHandleStillVisible |=
                        message.content.find(observed.originalId) != std::string::npos;
                if (message.role == MaiModelRole::ToolResult &&
                    message.toolCallId == "owned-large-call") {
                    projection = nlohmann::json::parse(message.content, nullptr, false);
                    projectedBytes = message.content.size();
                    actualContent = message.content;
                }
            }
        }
        if (unsupportedRetention) {
            CHECK(actualContent == observed.raw);
            CHECK(!projection.is_object());
        } else {
            CHECK(projectedBytes > 0 && projectedBytes < observed.raw.size() / 2);
            CHECK(projectedBytes <= 64 * 1024);
            CHECK(projection.is_object());
        }
        MaiToolPart stored;
        for (const auto& message : agent.listMessages(session.id))
            for (const auto& part : message.parts)
                if (const auto* tool = std::get_if<MaiToolPart>(&part.body);
                    tool && tool->callId == "owned-large-call")
                    stored = *tool;
        CHECK(stored.output == observed.raw);
        if (unsupportedRetention) {
            CHECK(stored.state == MaiToolState::Completed);
            if (projection.is_object()) {
                CHECK(!projection.value("capture_available", true));
                CHECK(!projection.value("capture_complete", true));
                CHECK(centralProjectionOutputId(projection).empty());
            }
        } else {
            CHECK(stored.state == MaiToolState::Error);
            CHECK(stored.error == observed.raw);
            CHECK(stored.execution);
            if (stored.execution) {
                CHECK(stored.execution->status == MaiToolExecutionStatus::Failed);
                CHECK(stored.execution->effectStatus == MaiToolEffectStatus::Confirmed);
                CHECK(stored.execution->started == std::optional<bool>{true});
                CHECK(stored.execution->exitCode == std::optional<int>{7});
                CHECK(stored.execution->outputId == observed.originalId);
                CHECK(stored.execution->taskId == "owned-existing-task");
                CHECK(stored.execution->nextAction == MaiToolNextAction::InspectState);
            }
            CHECK(originalHandleStillVisible);
            const auto original =
                observedStore->readToolOutput(session.id, MaiToolOutputQuery{observed.originalId});
            CHECK(original && original.value().truncated);
            CHECK(original && original.value().totalBytes == observed.originalBytes.size());
            if (projection.is_object()) {
                const auto genericId = centralProjectionOutputId(projection);
                CHECK(!genericId.empty() && genericId != observed.originalId);
                CHECK(projection.value("original_output_id", std::string{}) == observed.originalId);
                const auto generic =
                    observedStore->readToolOutput(session.id, MaiToolOutputQuery{genericId});
                CHECK(generic && generic.value().totalBytes == observed.raw.size());
            }
        }
    }
}

void test_central_tool_output_preserves_legacy_history_until_checkpoint() {
    std::printf(
        "   central output: legacy published history stays byte-identical until a checkpoint\n");
    const std::string raw =
        "owned-legacy-head\n" + std::string(180 * 1024, 'l') + "\nowned-legacy-tail";
    auto store = makeMaiMemoryStore();
    auto* observedStore = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    store->putSession(session);
    MaiMessage user;
    user.id = MaiIdGenerator::newMessageId();
    user.role = MaiRole::User;
    user.parts.push_back(
        {MaiIdGenerator::newPartId(), MaiTextPart{"Inspect the owned legacy source."}, 1});
    store->putMessage(session.id, user);
    MaiMessage previous;
    previous.id = MaiIdGenerator::newMessageId();
    previous.role = MaiRole::Assistant;
    previous.created = previous.completed = 2;
    MaiToolPart original;
    original.tool = "owned_legacy_tool";
    original.callId = "owned-legacy-call";
    original.input = "{}";
    original.output = raw;
    original.state = MaiToolState::Completed;
    previous.parts.push_back({MaiIdGenerator::newPartId(), original, 2});
    store->putMessage(session.id, previous);
    class LegacyModel final : public MaiModelClient {
    public:
        std::vector<MaiModelRequest> requests;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiModelContextLimits contextLimits(const std::string&) const override {
            return {262144, 8192};
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            requests.push_back(request);
            if (sink.onText) sink.onText("The owned legacy observation remains available.");
            return {};
        }
    };
    auto model = std::make_unique<LegacyModel>();
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(makeMaiToolOutputReadTool());
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(agent.submit(
        MaiSendPrompt{session.id, "Continue using the saved result without rerunning its tool."}));
    agent.waitIdle();
    CHECK(observedModel->requests.size() == 1);
    bool foundOriginal = false;
    if (observedModel->requests.size() == 1)
        for (const auto& message : observedModel->requests.front().messages)
            if (message.role == MaiModelRole::ToolResult && message.toolCallId == original.callId) {
                foundOriginal = message.content == raw;
                CHECK(message.content.size() > 64 * 1024);
            }
    CHECK(foundOriginal);
    MaiContextCheckpoint checkpoint;
    CHECK(!observedStore->getContextCheckpoint(session.id, checkpoint));
    MaiMessage persisted;
    CHECK(observedStore->getMessage(session.id, previous.id, persisted));
    CHECK(persisted.parts.size() == 1);
    if (persisted.parts.size() == 1)
        CHECK(std::get<MaiToolPart>(persisted.parts.front().body).output == raw);
}

void test_central_tool_output_applies_aggregate_budget_and_keeps_each_call_owner() {
    std::printf("   central output: six same-body calls share an aggregate request budget\n");
    class BatchTool final : public MaiTool {
    public:
        std::string name() const override {
            return "owned_batch_output";
        }
        std::string description() const override {
            return "Return owned synthetic observations.";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object"})";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            return MaiToolResult::success("owned-batch-head\n" + std::string(96 * 1024, 'q') +
                                          "\nowned-batch-tail");
        }
    };
    class BatchModel final : public MaiModelClient {
    public:
        std::vector<MaiModelRequest> requests;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiModelContextLimits contextLimits(const std::string&) const override {
            return {1048576, 8192};
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            requests.push_back(request);
            if (requests.size() == 1 && sink.onToolCall) {
                for (int index = 0; index < 6; ++index)
                    sink.onToolCall(
                        {"owned-batch-call-" + std::to_string(index), "owned_batch_output", "{}"});
            } else if (sink.onText)
                sink.onText("All six owned observations remain distinct.");
            return {};
        }
    };
    auto store = makeMaiMemoryStore();
    auto* observedStore = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    store->putSession(session);
    auto model = std::make_unique<BatchModel>();
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<BatchTool>());
    tools->add(makeMaiToolOutputReadTool());
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(agent.submit(
        MaiSendPrompt{session.id, "Inspect the six owned observations without rerunning them."}));
    agent.waitIdle();
    CHECK(observedModel->requests.size() == 2);
    std::size_t bytes = 0, results = 0;
    std::set<std::string> ids;
    if (observedModel->requests.size() == 2)
        for (const auto& message : observedModel->requests.back().messages) {
            if (message.role != MaiModelRole::ToolResult) continue;
            ++results;
            bytes += message.content.size();
            CHECK(message.content.size() <= 64 * 1024);
            const auto wrapper = nlohmann::json::parse(message.content, nullptr, false);
            CHECK(wrapper.is_object());
            const auto id = centralProjectionOutputId(wrapper);
            CHECK(!id.empty());
            ids.insert(id);
            const auto page = observedStore->readToolOutput(session.id, MaiToolOutputQuery{id});
            CHECK(page && page.value().callId == message.toolCallId);
            CHECK(page && page.value().totalBytes > 96 * 1024);
        }
    std::printf("     aggregate ToolResult=%zu bytes across %zu calls\n", bytes, results);
    CHECK(results == 6 && ids.size() == 6);
    CHECK(bytes <= 256 * 1024);
}

void test_central_tool_output_enforces_aggregate_when_many_envelopes_do_not_fit() {
    for (const bool unknownWindow : {false, true}) {
        for (const bool compactRows : {false, true}) {
            const std::size_t count = compactRows ? 400 : 1000;
            const std::string raw(compactRows ? 70000 : 300, 'h');
            std::printf(
                "   central boundary: %zu historical results, %zu raw bytes each, %s window\n",
                count, raw.size(), unknownWindow ? "unknown" : "1048576-token");
            auto store = makeMaiMemoryStore();
            auto* observedStore = store.get();
            MaiSession session;
            session.id = MaiIdGenerator::newSessionId();
            store->putSession(session);
            MaiMessage user;
            user.id = MaiIdGenerator::newMessageId();
            user.role = MaiRole::User;
            user.parts.push_back(
                {MaiIdGenerator::newPartId(), MaiTextPart{"Inspect owned history."}, 1});
            store->putMessage(session.id, user);
            MaiMessage source;
            source.id = MaiIdGenerator::newMessageId();
            source.role = MaiRole::Assistant;
            source.created = source.completed = 2;
            for (std::size_t index = 0; index < count; ++index) {
                MaiToolPart tool;
                tool.tool = "owned_history";
                tool.callId = "owned-history-" + std::to_string(index);
                tool.input = "{}";
                tool.output = raw;
                tool.state = MaiToolState::Completed;
                tool.modelOutputVersion = 1;  // Owned fixture has never been exposed to a model.
                source.parts.push_back({MaiIdGenerator::newPartId(), std::move(tool), 2});
            }
            store->putMessage(session.id, source);
            class Model final : public MaiModelClient {
            public:
                bool unknownWindow = false;
                std::vector<MaiModelRequest> requests;
                MaiWireApi wireApi() const override {
                    return MaiWireApi::Responses;
                }
                MaiModelContextLimits contextLimits(const std::string&) const override {
                    return unknownWindow ? MaiModelContextLimits{}
                                         : MaiModelContextLimits{1048576, 8192};
                }
                MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                                const std::atomic<bool>&) override {
                    requests.push_back(request);
                    if (sink.onText) sink.onText("Owned history inspected.");
                    return {};
                }
            };
            auto model = std::make_unique<Model>();
            model->unknownWindow = unknownWindow;
            auto* observedModel = model.get();
            auto tools = std::make_unique<MaiToolRegistry>();
            tools->add(makeMaiToolOutputReadTool());
            MaiAgent agent(std::move(store), std::move(model), std::move(tools));
            CHECK(agent.submit(
                MaiSendPrompt{session.id, "Continue without changing the owned source."}));
            agent.waitIdle();
            if (!compactRows && observedModel->requests.empty()) {
                // 未开始模型输出的失败轮次可能是隐藏draft；通过普通终态API读取，
                // 不把“可见聊天行没有失败气泡”等同于没有保存失败事实。
                const auto outcome = agent.getTurnOutcome(session.id);
                const bool explicitCapacityFailure =
                    outcome && outcome.value().status == MaiTurnStatus::Failed &&
                    outcome.value().error.hasError() &&
                    (outcome.value().error.message().find("budget") != std::string::npos ||
                     outcome.value().error.message().find("capacity") != std::string::npos);
                if (!explicitCapacityFailure) {
                    std::fprintf(
                        stderr, "central-capacity outcome=%d status=%s error=%s\n", bool(outcome),
                        outcome ? maiTurnStatusToString(outcome.value().status) : "missing",
                        outcome ? outcome.value().error.message().c_str()
                                : outcome.error().message().c_str());
                }
                CHECK(explicitCapacityFailure);
            } else {
                CHECK(observedModel->requests.size() == 1);
                std::size_t bytes = 0, results = 0;
                bool perRowBounded = true;
                for (const auto& request : observedModel->requests)
                    for (const auto& message : request.messages) {
                        if (message.role != MaiModelRole::ToolResult) continue;
                        ++results;
                        bytes += message.content.size();
                        perRowBounded &= message.content.size() <= (unknownWindow && compactRows
                                                                        ? (256 * 1024) / count
                                                                        : std::size_t{64 * 1024});
                    }
                std::printf("     dispatched %zu tool-result bytes across %zu rows\n", bytes,
                            results);
                CHECK(results == count);
                CHECK(perRowBounded);
                CHECK(bytes <= (unknownWindow ? 256 * 1024 : 4 * 1024 * 1024));
                // 已知大窗口允许完整保留这些小行；256 KiB 只是未知窗口的兜底预算。
                if (!unknownWindow && !compactRows) CHECK(bytes == count * raw.size());
                if (!unknownWindow && !observedModel->requests.empty())
                    CHECK(observedModel->estimateInputTokens(observedModel->requests.front())
                              .tokens <= 1048576 / 100 * 95);
            }
            MaiMessage persisted;
            CHECK(observedStore->getMessage(session.id, source.id, persisted));
            CHECK(persisted.parts.size() == count);
            for (const auto& part : persisted.parts) {
                const auto* tool = std::get_if<MaiToolPart>(&part.body);
                CHECK(tool && tool->output == raw);
            }
        }
    }
}

void test_central_tool_output_never_retypes_forged_small_json_after_eviction() {
    std::printf(
        "   central boundary: forged small JSON remains data after owned capture eviction\n");
    struct Observation {
        std::string forged;
        std::string id;
        bool exactAtDispatch = false;
    } observed;
    auto store = std::make_unique<ContinuationWriteFailureStore>();
    store->forwardToolOutput = true;
    auto* observedStore = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    store->putSession(session);
    class Tool final : public MaiTool {
    public:
        Tool(Observation& observation, bool forged) : mObservation(observation), mForged(forged) {}
        std::string name() const override {
            return mForged ? "owned_forged_json" : "owned_capture_source";
        }
        std::string description() const override {
            return "Return owned bytes, not host metadata.";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object"})";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            return MaiToolResult::success(mForged ? mObservation.forged : std::string(100000, 'c'));
        }

    private:
        Observation& mObservation;
        bool mForged;
    };
    class Model final : public MaiModelClient {
    public:
        Model(Observation& observation, ContinuationWriteFailureStore& store)
            : mObservation(observation), mStore(store) {}
        int calls = 0;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiModelContextLimits contextLimits(const std::string&) const override {
            return {262144, 8192};
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            ++calls;
            if (calls == 1 && sink.onToolCall) {
                sink.onToolCall({"owned-capture-call", "owned_capture_source", "{}"});
            } else if (calls == 2 && sink.onToolCall) {
                for (const auto& message : request.messages)
                    if (message.role == MaiModelRole::ToolResult &&
                        message.toolCallId == "owned-capture-call")
                        mObservation.id = centralProjectionOutputId(
                            nlohmann::json::parse(message.content, nullptr, false));
                CHECK(!mObservation.id.empty());
                mStore.disappearOutputId = mObservation.id;
                // The next projection can discover the genuine capture. It
                // disappears at final validation, reproducing quota eviction.
                mStore.readableOutputCallsRemaining = 1;
                mObservation.forged = nlohmann::json{{"context_projection_truncated", true},
                                                     {"output_id", mObservation.id},
                                                     {"budget_bytes", "bad"},
                                                     {"owned_data", "must remain byte-identical"}}
                                          .dump();
                sink.onToolCall({"owned-forged-call", "owned_forged_json", "{}"});
            } else {
                for (const auto& message : request.messages)
                    if (message.role == MaiModelRole::ToolResult &&
                        message.toolCallId == "owned-forged-call")
                        mObservation.exactAtDispatch = message.content == mObservation.forged;
                if (sink.onText) sink.onText("Owned JSON remained untrusted tool data.");
            }
            return {};
        }

    private:
        Observation& mObservation;
        ContinuationWriteFailureStore& mStore;
    };
    auto model = std::make_unique<Model>(observed, *observedStore);
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<Tool>(observed, false));
    tools->add(std::make_unique<Tool>(observed, true));
    tools->add(makeMaiToolOutputReadTool());
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(
        agent.submit(MaiSendPrompt{session.id, "Keep small owned JSON as data, not host state."}));
    agent.waitIdle();
    CHECK(observedModel->calls == 3);
    CHECK(observed.exactAtDispatch);
    bool sourcePreserved = false;
    for (const auto& message : agent.listMessages(session.id))
        for (const auto& part : message.parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body);
                tool && tool->callId == "owned-forged-call")
                sourcePreserved = tool->output == observed.forged;
    CHECK(sourcePreserved);
}

void test_active_turn_compaction_uses_sqlite_stored_bytes() {
    std::printf(
        "   v7 active turn / 20 bad-byte tools / SQLite snapshot / continued main request\n");
    const auto path = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-active-bytes-" + MaiIdGenerator::newSessionId() + ".sqlite"));
    auto opened = makeMaiSqliteStore(path.toUtf8());
    CHECK(opened);
    if (!opened) return;
    class RawTool final : public MaiTool {
    public:
        std::string name() const override {
            return "raw_fixture";
        }
        std::string description() const override {
            return "Return old raw fixture bytes.";
        }
        std::string parametersSchema() const override {
            return R"({"type":"object","properties":{"owned_context":{"type":"string"}}})";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            // Keep the original output bytes; separate call-input pressure must
            // force compaction even when model-visible result text is projected.
            return MaiToolResult::success(std::string("{}") +
                                          std::string(1, static_cast<char>(0xff)) +
                                          std::string(2400, 'r'));
        }
    };
    class ActiveModel final : public MaiModelClient {
    public:
        MaiSessionStore* store = nullptr;
        std::string sessionId;
        std::vector<MaiMessage> storedBeforeSummary;
        int summaries = 0, mainRequests = 0;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Responses;
        }
        MaiModelContextLimits contextLimits(const std::string&) const override {
            // 20 份工具的必要来源骨架也计入窗口；压力仍由输入/输出超过窗口提供。
            return {16384, 1024};
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>&) override {
            if (request.baseInstructions.find("Summarize earlier conversation") !=
                std::string::npos) {
                ++summaries;
                if (storedBeforeSummary.empty())
                    storedBeforeSummary = store->listMessages(sessionId);
                if (sink.onText)
                    sink.onText(
                        R"({"goals":["Continue this turn"],"constraints":[],"decisions":[],"progress":["Read saved tool results"],"open_questions":[],"source_notes":[]})");
            } else if (++mainRequests == 1) {
                for (int index = 0; index < 20; ++index)
                    if (sink.onToolCall)
                        sink.onToolCall(
                            {"raw_call_" + std::to_string(index), "raw_fixture",
                             nlohmann::json{{"owned_context", std::string(1024, 'i')}}.dump()});
            } else {
                if (storedBeforeSummary.empty())
                    storedBeforeSummary = store->listMessages(sessionId);
                if (sink.onText) sink.onText("The active tool loop continued after compaction.");
            }
            return {};
        }
    };
    auto store = std::move(opened.value());
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.title = "Active raw byte fixture";
    store->putSession(session);
    auto* observedStore = store.get();
    auto model = std::make_unique<ActiveModel>();
    model->store = observedStore;
    model->sessionId = session.id;
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools, false);
    tools->add(std::make_unique<RawTool>());
    {
        MaiAgent agent(std::move(store), std::move(model), std::move(tools));
        CHECK(agent.submit(
            MaiSendPrompt{session.id, "Preserve this instruction and complete all tool work."}));
        agent.waitIdle();
        CHECK(observedModel->summaries == 0);
        CHECK(observedModel->mainRequests == 2);
        MaiContextCheckpoint checkpoint;
        CHECK(observedStore->getContextCheckpoint(session.id, checkpoint));
        const auto after = agent.listMessages(session.id);
        CHECK(after.back().text().find("continued after compaction") != std::string::npos);
        CHECK(maiContextCheckpointMatches(checkpoint, session.id, after));
        if (!observedModel->storedBeforeSummary.empty() && !after.empty()) {
            const auto& before = observedModel->storedBeforeSummary.back();
            const auto& final = after.back();
            CHECK(before.parts.size() == 20);
            for (std::size_t index = 0; index < before.parts.size(); ++index) {
                const auto* oldTool = std::get_if<MaiToolPart>(&before.parts[index].body);
                const auto* newTool = std::get_if<MaiToolPart>(&final.parts[index].body);
                CHECK(oldTool && newTool);
                if (oldTool && newTool) {
                    CHECK(oldTool->output == newTool->output);
                    // 权威会话保留原始字节；模型文本编码与展示清洗由请求边界负责。
                    CHECK(oldTool->output.find(static_cast<char>(0xff)) != std::string::npos);
                    CHECK(oldTool->output.find("\xef\xbf\xbd") == std::string::npos);
                }
            }
            CHECK(maiContextPartialSourceFingerprint(checkpoint,
                                                     observedModel->storedBeforeSummary) ==
                  checkpoint.partialSourceFingerprint);
        }
    }
    MaiFileSystem::removeFile(path);
    MaiFileSystem::removeFile(MaiFilePath::fromUtf8(path.toUtf8() + "-wal"));
    MaiFileSystem::removeFile(MaiFilePath::fromUtf8(path.toUtf8() + "-shm"));
}

void test_unfenced_image_groups_follow_execute_tools_and_continue() {
    std::printf(
        "   v7c actual executeTools: 20 contiguous results / group-only original pixels / next "
        "large batch\n");
    constexpr char png[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
        "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        "\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\x30\x6d\x3b\xfc\x1f"
        "\x00\x04\xf0\x02\x7e\xf1\xe8\x4f\x8a\x00\x00\x00\x00\x49\x45\x4e\x44"
        "\xae\x42\x60\x82";
    const std::string pixels(png, sizeof(png) - 1);
    for (bool multiImage : {false, true}) {
        const auto folder = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-unfenced-execute-" + MaiIdGenerator::newSessionId()));
        CHECK(!MaiFileSystem::createDirectories(folder));
        auto opened =
            makeMaiSqliteStore(folder.append(MaiFilePath::fromUtf8("state.sqlite")).toUtf8());
        CHECK(opened);
        if (!opened) continue;
        class BatchTool final : public MaiTool {
        public:
            std::string toolName;
            std::string folder;
            std::string pixels;
            bool multiImage = false;
            std::string name() const override {
                return toolName;
            }
            std::string description() const override {
                return "Read deterministic fixture results or return fixture pixels.";
            }
            std::string parametersSchema() const override {
                return R"({"type":"object","properties":{"index":{"type":"integer"},"phase":{"type":"integer"},"owned_context":{"type":"string"}}})";
            }
            bool requiresWorkingDirectory(const std::string&) const override {
                return false;
            }
            MaiToolResult execute(const std::string& raw, const MaiToolContext&) override {
                const auto input = nlohmann::json::parse(raw);
                const int index = input["index"], phase = input["phase"];
                const bool screenshot = toolName == "screenshot_fixture";
                const std::string output =
                    screenshot ? "Fixture image result." : std::string(12000, 'r');
                if (!screenshot && !(multiImage && index == 5))
                    return MaiToolResult::success(output);
                const auto path = MaiFilePath::fromUtf8(folder).append(MaiFilePath::fromUtf8(
                    "phase-" + std::to_string(phase) + "-tool-" + std::to_string(index) + ".png"));
                const auto error = MaiFileSystem::writeFile(path, pixels);
                if (error) return MaiToolResult::failure(error.code(), error.message());
                return MaiToolResult::successWithImages(output, {{path.toUtf8(), "image/png"}});
            }
        };
        class BatchModel final : public MaiModelClient {
        public:
            MaiSessionStore* store = nullptr;
            std::string sessionId, instruction, pixels;
            int mainRequests = 0, summaryRequests = 0;
            int imageCount = 1;
            std::vector<MaiMessage> initialSource;
            std::set<std::string> initialToolParts;
            MaiContextCheckpoint firstCheckpoint;
            MaiWireApi wireApi() const override {
                return MaiWireApi::Responses;
            }
            MaiModelContextLimits contextLimits(const std::string&) const override {
                // 两组40份工具骨架和读取声明都必须容纳；下面的调用参数继续
                // 制造超过此窗口的真实压力，而不扩大任何用户/助手正文。
                return {65536, 4096};
            }
            MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                            const std::atomic<bool>&) override {
                if (request.baseInstructions.find("Summarize earlier conversation") !=
                    std::string::npos) {
                    ++summaryRequests;
                    if (initialSource.empty()) {
                        initialSource = store->listMessages(sessionId);
                        CHECK(initialSource.back().parts.size() ==
                              static_cast<std::size_t>(20 + imageCount));
                        for (std::size_t index = 0; index < 20; ++index) {
                            CHECK(std::holds_alternative<MaiToolPart>(
                                initialSource.back().parts[index].body));
                            initialToolParts.insert(initialSource.back().parts[index].id);
                        }
                    }
                    MaiContextCheckpoint prior;
                    if (store->getContextCheckpoint(sessionId, prior)) {
                        const auto data = nlohmann::json::parse(request.messages[0].content);
                        for (const auto& fragment : data["source_fragments"])
                            CHECK(initialToolParts.count(fragment["part_id"].get<std::string>()) ==
                                  0);
                    }
                    if (sink.onText)
                        sink.onText(
                            R"({"goals":["Keep the original request"],"constraints":[],"decisions":[],"progress":["The completed group returned stored results and image observations"],"open_questions":[],"source_notes":[]})");
                    return {};
                }
                ++mainRequests;
                if (mainRequests == 2 && initialSource.empty()) {
                    initialSource = store->listMessages(sessionId);
                    for (const auto& part : initialSource.back().parts)
                        if (std::holds_alternative<MaiToolPart>(part.body))
                            initialToolParts.insert(part.id);
                }
                CHECK(request.maxOutputTokens == 0);
                if (mainRequests > 1) {
                    // Projection can make tool text fit without compaction. A
                    // missing checkpoint must fail this fixture, not dereference
                    // an empty source snapshot and conceal the failed premise.
                    CHECK(!initialSource.empty());
                    if (initialSource.empty())
                        return MaiError::make(MaiErrorCode::Internal,
                                              "Expected an archived owned image-group source");
                    bool keptUser = false;
                    int observations = 0;
                    for (const auto& message : request.messages) {
                        if (message.role == MaiModelRole::User && message.content == instruction)
                            keptUser = true;
                        for (const auto& image : message.images) {
                            ++observations;
                            std::string actual;
                            CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(image.path),
                                                           actual, 1024));
                            CHECK(actual == pixels);
                            // 未为达到目标而省略的旧工具图片仍可保留；只对本轮最新
                            // 观察要求原来的完整组来源，不把旧图强行归给最新调用组。
                            if (image.path.find("phase-" + std::to_string(mainRequests - 2) +
                                                "-") == std::string::npos) {
                                --observations;
                                continue;
                            }
                            const auto data = nlohmann::json::parse(message.content);
                            CHECK(data["ownership"] == "tool_group_only");
                            CHECK(data["source_message_id"] == initialSource.back().id);
                            CHECK(data["tool_count"] == 20);
                            CHECK(data["tools"].size() == 20);
                            CHECK(data["archived_tool_count"] == 20);
                            for (const auto& tool : data["tools"]) CHECK(tool["archived"] == true);
                            CHECK(message.content.find("preceding tool") == std::string::npos);
                        }
                    }
                    CHECK(keptUser && observations == imageCount);
                }
                if (mainRequests == 2)
                    CHECK(store->getContextCheckpoint(sessionId, firstCheckpoint));
                if (mainRequests <= 2) {
                    // Call-input protocol data remains outside result projection;
                    // it keeps the original image-group compaction boundary under
                    // genuine pressure without enlarging user text or tool output.
                    for (int index = 0; index < 20; ++index)
                        if (sink.onToolCall)
                            sink.onToolCall(
                                {"phase_" + std::to_string(mainRequests - 1) + "_" +
                                     std::to_string(index),
                                 index == 19 ? "screenshot_fixture" : "read_fixture",
                                 nlohmann::json{{"index", index},
                                                {"phase", mainRequests - 1},
                                                {"owned_context", std::string(12000, 'i')}}
                                     .dump()});
                } else if (sink.onText)
                    sink.onText("Completed both image groups after context compaction.");
                return {};
            }
        };
        auto store = std::move(opened.value());
        MaiSession session;
        session.id = MaiIdGenerator::newSessionId();
        session.title = "Unfenced batch fixture";
        session.directory = folder.toUtf8();
        store->putSession(session);
        auto* observedStore = store.get();
        auto model = std::make_unique<BatchModel>();
        model->store = observedStore;
        model->sessionId = session.id;
        model->instruction =
            "Keep my full original instruction and continue both large tool groups.";
        model->pixels = pixels;
        model->imageCount = multiImage ? 2 : 1;
        auto* observedModel = model.get();
        auto tools = std::make_unique<MaiToolRegistry>();
        registerMaiSessionTools(*tools, false);
        for (const auto& name : {"read_fixture", "screenshot_fixture"}) {
            auto tool = std::make_unique<BatchTool>();
            tool->toolName = name;
            tool->folder = folder.toUtf8();
            tool->pixels = pixels;
            tool->multiImage = multiImage;
            tools->add(std::move(tool));
        }
        {
            MaiAgent agent(std::move(store), std::move(model), std::move(tools));
            CHECK(agent.submit(MaiSendPrompt{session.id, observedModel->instruction}));
            agent.waitIdle();
            CHECK(observedModel->mainRequests == 3 && observedModel->summaryRequests == 0);
            std::printf("     images=%d main_requests=%d summary_requests=%d\n", multiImage ? 2 : 1,
                        observedModel->mainRequests, observedModel->summaryRequests);
            MaiContextCheckpoint checkpoint;
            CHECK(observedStore->getContextCheckpoint(session.id, checkpoint));
            CHECK(checkpoint.revision == observedModel->firstCheckpoint.revision + 1);
            const auto after = agent.listMessages(session.id);
            if (observedModel->mainRequests != 3 && after.back().turnOutcome)
                std::printf("     image-group failure=%s\n",
                            after.back().turnOutcome->error.message().c_str());
            CHECK(after.back().text().find("Completed both image groups") != std::string::npos);
            CHECK(maiContextCheckpointMatches(checkpoint, session.id, after));
            if (!observedModel->initialSource.empty())
                for (std::size_t index = 0; index < 20; ++index) {
                    const auto& original = std::get<MaiToolPart>(
                        observedModel->initialSource.back().parts[index].body);
                    const auto& persisted = std::get<MaiToolPart>(after.back().parts[index].body);
                    CHECK(original.callId == persisted.callId &&
                          original.input == persisted.input && original.output == persisted.output);
                }
        }
        MaiFileSystem::removeRecursively(folder);
    }
}

void test_runner_efforts_follow_the_actual_model_and_preserve_invalid_errors() {
    const auto runCase = [](const std::string& selectedModel, const std::string& savedEffort,
                            const std::string& defaultModel, const std::string& inheritedEffort,
                            bool useProvider, bool valid, const std::string& expectedEffort) {
        auto store = makeMaiMemoryStore();
        MaiSession session;
        session.id = MaiIdGenerator::newSessionId();
        session.model = selectedModel;
        session.reasoningEffort = savedEffort;
        store->putSession(session);
        MaiMessage user;
        user.id = MaiIdGenerator::newMessageId();
        user.role = MaiRole::User;
        user.parts.push_back({MaiIdGenerator::newPartId(), MaiTextPart{"Synthetic effort."}, 1});
        store->putMessage(session.id, user);
        MaiMessage assistant;
        assistant.id = MaiIdGenerator::newMessageId();
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        MaiFakeModelClient model;
        model.setRepeatingTurn(defaultTurn());
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiToolRegistry tools;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = &model;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = defaultModel;
        dependencies.reasoningEffort = inheritedEffort;
        if (useProvider)
            dependencies.reasoningEffortProvider = [inheritedEffort] { return inheritedEffort; };
        std::atomic<bool> cancel{false};
        MaiTurnRunner runner(dependencies, session.id, assistant);
        runner.run(cancel);
        CHECK(runner.succeeded() == valid);
        CHECK(model.requestCount() == (valid ? 1U : 0U));
        if (valid && model.requestCount() == 1) {
            const auto request = model.lastRequest();
            CHECK(request.model == (selectedModel.empty() ? defaultModel : selectedModel));
            CHECK(request.reasoningEffort == expectedEffort);
        }
    };
    const std::string fable = "anthropic/claude-fable-5.2";
    for (const auto& effort : {"low", "medium", "high", "xhigh", "max"}) {
        runCase(fable, effort, "deepseek-flash", "unknown-default", false, true, effort);
        runCase("", "", fable, effort, false, true, effort);
        runCase(fable, "", "deepseek-flash", effort, true, true, effort);
    }
    for (const auto& model : {"glm-5.4", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro"}) {
        for (const auto& effort : {"low", "high", "max"})
            runCase(model, effort, fable, "xhigh", false, true, effort);
        for (const auto& effort : {"medium", "xhigh"}) {
            // 兼容已保存值及宿主继承值；只按当前请求实际型号归一，不回写全局偏好。
            runCase(model, effort, fable, "low", false, true, "high");
            runCase(model, "", fable, effort, false, true, "high");
            runCase("", "", model, effort, true, true, "high");
        }
    }
    for (const auto& invalid : {"unknown-effort", "none", "MEDIUM"}) {
        runCase(fable, invalid, fable, "low", false, false, "");
        runCase("deepseek-flash", "", fable, invalid, true, false, "");
    }
    runCase(fable, "", fable, "", false, true, "");
}

// 两会话共用客户端时仍把选择写入各自会话；活动轮次/工具上下文使用固定配置。
void test_session_model_effort_are_atomic_and_turn_scoped() {
    struct Probe final : MaiModelClient {
        std::mutex mutex;
        std::condition_variable wake;
        std::vector<MaiModelRequest> requests;
        bool released = false;
        int glmRequests = 0;
        MaiWireApi wireApi() const override {
            return MaiWireApi::Mixed;
        }
        std::optional<MaiWireApi> wireApiForModel(const std::string& model) const override {
            const auto info = maiCloudModelInfo(model);
            return info ? std::optional<MaiWireApi>(info->wire) : std::nullopt;
        }
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>& cancel) override {
            std::unique_lock<std::mutex> lock(mutex);
            requests.push_back(request);
            const bool first = requests.size() == 1;
            const int glmCycle = request.model == "glm-5.4" ? ++glmRequests : 0;
            wake.notify_all();
            if (first && !wake.wait_for(lock, std::chrono::seconds(10),
                                        [&] { return released || cancel.load(); }))
                return {MaiErrorCode::Internal, "Synthetic model gate timed out."};
            lock.unlock();
            if (cancel.load()) return {MaiErrorCode::Canceled, "Canceled."};
            if (glmCycle > 0 && glmCycle <= 2) {
                if (sink.onReasoning)
                    sink.onReasoning("Opaque GLM reasoning " + std::to_string(glmCycle) + "\r\n");
                if (sink.onToolCall)
                    sink.onToolCall(
                        {"session_effort_call_" + std::to_string(glmCycle), "effort_probe", "{}"});
            } else if (sink.onText)
                sink.onText("Completed.");
            return {};
        }
    };
    struct Tool final : MaiTool {
        std::string actualModel;
        std::string name() const override {
            return "effort_probe";
        }
        std::string description() const override {
            return "Synthetic current-turn model probe.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
            actualModel = context.model;
            return MaiToolResult::success("Observed current-turn model.");
        }
    };
    auto model = std::make_unique<Probe>();
    auto* probe = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    auto tool = std::make_unique<Tool>();
    auto* observedTool = tool.get();
    tools->add(std::move(tool));
    std::atomic<int> defaultEffort{1};
    MaiAgent::Options options;
    options.defaultModel = "deepseek-flash";
    options.reasoningEffortProvider = [&] { return defaultEffort.load() == 1 ? "high" : "max"; };
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto a = agent.submit(MaiCreateSession{"/tmp/work", "A", "glm-5.4", "low"}).value();
    const auto b =
        agent.submit(MaiCreateSession{"/tmp/work", "B", "deepseek-v4-pro", "max"}).value();
    CHECK(!agent.submit(MaiCreateSession{"/tmp/work", "Bad", "glm-unknown", "low"}));
    CHECK(!agent.submit(MaiCreateSession{"/tmp/work", "Bad", "glm-5.4", "medium"}));
    CHECK(agent.submit(MaiUpdateSession{b, "Must not change", "glm-5.4", "", "", "medium"})
              .error()
              .code() == MaiErrorCode::InvalidInput);
    CHECK(agent.submit(MaiUpdateSession{b, "Must not change", "glm-unknown", "", "", "low"})
              .error()
              .code() == MaiErrorCode::InvalidInput);
    MaiSession before;
    CHECK(agent.getSession(b, before));
    CHECK(before.title == "B" && before.model == "deepseek-v4-pro" &&
          before.reasoningEffort == "max");
    CHECK(agent.submit(MaiSendPrompt{a, "Check session A"}));
    {
        std::unique_lock<std::mutex> lock(probe->mutex);
        CHECK(probe->wake.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return !probe->requests.empty(); }));
    }
    CHECK(agent.submit(MaiUpdateSession{a, "Must not change", "deepseek-flash", "", "", "max"})
              .error()
              .code() == MaiErrorCode::Busy);
    CHECK(agent.submit(MaiUpdateSession{b, "", "deepseek-v4-pro", "", "", "high"}));
    defaultEffort = 2;
    CHECK(agent.submit(MaiSendPrompt{b, "Check independent session B"}));
    {
        std::unique_lock<std::mutex> lock(probe->mutex);
        CHECK(probe->wake.wait_for(lock, std::chrono::seconds(10),
                                   [&] { return probe->requests.size() >= 2; }));
        probe->released = true;
        probe->wake.notify_all();
    }
    agent.waitIdle();
    CHECK(observedTool->actualModel == "glm-5.4");
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        CHECK(probe->requests.size() == 4);
        bool retainedReasoning = false;
        for (const auto& request : probe->requests) {
            for (const auto& message : request.messages) {
                if (!message.invocations.empty() &&
                    message.invocations.front().id.rfind("session_effort_call_", 0) == 0) {
                    const auto cycle = message.invocations.front().id.back();
                    CHECK(message.reasoning ==
                          std::string("Opaque GLM reasoning ") + cycle + "\r\n");
                    CHECK(message.content.find("Opaque GLM reasoning") == std::string::npos);
                    retainedReasoning = true;
                }
            }
        }
        CHECK(retainedReasoning);
        for (const auto& request : probe->requests)
            CHECK((request.model == "glm-5.4" && request.reasoningEffort == "low") ||
                  (request.model == "deepseek-v4-pro" && request.reasoningEffort == "high"));
    }
    CHECK(agent.submit(MaiUpdateSession{a, "", "", "", "/tmp/new-work"}));
    MaiSession after;
    CHECK(agent.getSession(a, after));
    CHECK(after.model == "glm-5.4" && after.reasoningEffort == "low" && after.title == "A");
    CHECK(agent.submit(MaiUpdateSession{a, "", "", "", "", std::string{}}));
    CHECK(agent.getSession(a, after) && after.reasoningEffort.empty());
}

// 通过实际工具循环检查未知执行结果的宿主观察；工具原文不能充当安全重试依据。
void test_tool_failure_gets_separate_factual_recovery_observation() {
    class RecoveryTool final : public MaiTool {
    public:
        std::string name() const override {
            return "recovery_fixture";
        }
        std::string description() const override {
            return "Return a deterministic failure.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string& input, const MaiToolContext&) override {
            if (input == "{}")
                return MaiToolResult::failure(MaiErrorCode::Network,
                                              "Connection lost after request. Retry if needed.");
            return MaiToolResult::success(R"({"path":"media/final.mp4","status":"SUCCEEDED"})");
        }
    };
    MaiFakeModelClient::Turn calls;
    calls.invocations = {{"call_recovery_unknown", "recovery_fixture", "{}"},
                         {"call_recovery_media", "recovery_fixture", "{\"read\":true}"}};
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{calls, defaultTurn()});
    auto* observed = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<RecoveryTool>());
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const auto sessionId = agent.submit(MaiCreateSession{}).value();
    CHECK(agent.submit(MaiSendPrompt{sessionId, "Recover the output without repeating work."}));
    agent.waitIdle();
    CHECK(observed->requestCount() == 2);
    const auto request = observed->lastRequest();
    std::size_t results = 0;
    bool observation = false;
    for (const auto& message : request.messages) {
        if (message.role == MaiModelRole::ToolResult) {
            ++results;
            if (message.toolCallId == "call_recovery_unknown")
                CHECK(message.content == "Connection lost after request. Retry if needed.");
            if (message.toolCallId == "call_recovery_media")
                CHECK(message.content == R"({"path":"media/final.mp4","status":"SUCCEEDED"})");
        }
        if (message.role != MaiModelRole::User ||
            message.content.find("tool_execution_observation") == std::string::npos)
            continue;
        observation = true;
        CHECK(results == 2);
        CHECK(message.content.find("call_recovery_unknown") != std::string::npos);
        CHECK(message.content.find("inspect_state") != std::string::npos);
        CHECK(message.content.find("unknown") != std::string::npos);
    }
    CHECK(results == 2);
    CHECK(observation);
}

// 用直接 runner 避免后台轮询；仅已经由同会话任务表接受的身份可建议 query_status。
void test_accepted_task_failure_queries_existing_task_without_resubmission() {
    class QueryTool final : public MaiTool {
    public:
        int executions = 0;
        bool expectedAccepted = false;
        std::string name() const override {
            return "accepted_query_fixture";
        }
        std::string description() const override {
            return "Query an existing fixture task.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string& input, const MaiToolContext& context) override {
            ++executions;
            CHECK(nlohmann::json::parse(input)["action"] == "continue");
            CHECK(context.callId == "call_existing_query");
            CHECK(!context.turnId.empty());
            CHECK(context.turnId == "msg_existing_query_answer");
            CHECK(context.toolOutputStore != nullptr);
            const auto runningHistory = context.toolOutputStore->listMessages(context.sessionId);
            const auto& running = std::get<MaiToolPart>(runningHistory.back().parts.back().body);
            CHECK(running.execution &&
                  running.execution->status == MaiToolExecutionStatus::Running);
            if (running.execution) {
                CHECK(running.execution->taskId == (expectedAccepted ? "provider_accepted" : ""));
                CHECK(running.execution->nextAction == (expectedAccepted
                                                            ? MaiToolNextAction::QueryStatus
                                                            : MaiToolNextAction::InspectState));
            }
            return MaiToolResult::failure(
                MaiErrorCode::Network,
                R"({"code":"provider_error","message":"Status connection lost"})");
        }
    };
    for (bool accepted : {false, true}) {
        auto store = makeMaiMemoryStore();
        MaiSession session;
        session.id = "ses_existing_query";
        store->putSession(session);
        if (accepted) {
            MaiSpecialistTask task;
            task.id = "spt_existing";
            task.ownerSessionId = session.id;
            task.specialistName = "accepted_query_fixture";
            task.providerTaskId = "provider_accepted";
            task.status = MaiSpecialistTaskStatus::Running;
            CHECK(!store->insertSpecialistTask(task));
        }
        MaiMessage user;
        user.id = "msg_existing_query_user";
        user.parts.push_back({"prt_existing_query_user", MaiTextPart{"Inspect existing work."}, 1});
        store->putMessage(session.id, user);
        MaiMessage assistant;
        assistant.id = "msg_existing_query_answer";
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        MaiFakeModelClient::Turn calls;
        calls.invocations = {{"call_missing_precheck", "missing_fixture", "{}"},
                             {"call_existing_query", "accepted_query_fixture",
                              R"({"action":"continue","conversation_id":"spt_existing"})"}};
        MaiFakeModelClient model({calls, defaultTurn()});
        MaiToolRegistry tools;
        auto query = std::make_unique<QueryTool>();
        query->expectedAccepted = accepted;
        auto* observed = query.get();
        tools.add(std::move(query));
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = &model;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = "glm-5.4";
        MaiTurnRunner runner(dependencies, session.id, assistant);
        std::atomic<bool> cancel{false};
        runner.run(cancel);
        CHECK(runner.succeeded());
        CHECK(observed->executions == 1);
        const auto history = store->listMessages(session.id);
        const auto boundaries = std::count_if(
            history.back().parts.begin(), history.back().parts.end(), maiIsResponseBoundaryPart);
        CHECK(boundaries == 1 && history.back().parts.size() == 3 + boundaries);
        if (history.back().parts.size() < 2) continue;
        const auto& precheck = std::get<MaiToolPart>(history.back().parts[0].body);
        const auto& result = std::get<MaiToolPart>(history.back().parts[1].body);
        CHECK(precheck.execution && result.execution);
        if (!precheck.execution || !result.execution) continue;
        CHECK(precheck.execution->status == MaiToolExecutionStatus::NotStarted);
        CHECK(precheck.execution->effectStatus == MaiToolEffectStatus::None);
        CHECK(precheck.execution->started == false);
        CHECK(result.error == R"({"code":"provider_error","message":"Status connection lost"})");
        CHECK(result.output == result.error);
        CHECK(result.execution->status == MaiToolExecutionStatus::Unknown);
        CHECK(!result.execution->started);
        CHECK(result.execution->taskId == (accepted ? "provider_accepted" : ""));
        CHECK(result.execution->nextAction ==
              (accepted ? MaiToolNextAction::QueryStatus : MaiToolNextAction::InspectState));
        const auto request = model.lastRequest();
        CHECK(request.messages.back().role == MaiModelRole::User);
        const auto observation = nlohmann::json::parse(request.messages.back().content);
        CHECK(observation["executions"].size() == 2);
        if (accepted) {
            const auto guidance = observation["executions"][1].value("guidance", std::string{});
            CHECK(guidance.find("do not submit a replacement") != std::string::npos);
            CHECK(!observation["executions"][1].contains("recovery_guidance"));
        }
    }
}

// 已持久化终态不能因一次查询回执失败退回“继续查询”，否则会反复轮询已结束任务。
void test_terminal_accepted_task_failure_keeps_trusted_terminal_state() {
    class TerminalQueryTool final : public MaiTool {
    public:
        std::string error;
        std::string name() const override {
            return "terminal_query_fixture";
        }
        std::string description() const override {
            return "Read a terminal fixture task.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            return MaiToolResult::failure(MaiErrorCode::Network, error);
        }
    };
    for (const auto taskStatus :
         {MaiSpecialistTaskStatus::Failed, MaiSpecialistTaskStatus::Canceled,
          MaiSpecialistTaskStatus::Succeeded, MaiSpecialistTaskStatus::Running}) {
        auto store = makeMaiMemoryStore();
        MaiSession session;
        session.id = "ses_terminal_query";
        store->putSession(session);
        MaiSpecialistTask task;
        task.id = "spt_terminal";
        task.ownerSessionId = session.id;
        task.specialistName = "terminal_query_fixture";
        task.providerTaskId = "provider_terminal";
        task.status = taskStatus;
        CHECK(!store->insertSpecialistTask(task));
        MaiMessage user;
        user.id = "msg_terminal_query_user";
        user.parts.push_back(
            {"prt_terminal_query_user", MaiTextPart{"Review the failed task."}, 1});
        store->putMessage(session.id, user);
        MaiMessage assistant;
        assistant.id = "msg_terminal_query_answer";
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        MaiFakeModelClient::Turn call;
        call.invocations = {{"call_terminal_query", "terminal_query_fixture",
                             R"({"action":"continue","conversation_id":"spt_terminal"})"}};
        MaiFakeModelClient model({call, defaultTurn()});
        MaiToolRegistry tools;
        const std::string expectedError =
            taskStatus == MaiSpecialistTaskStatus::Succeeded
                ? R"({"code":"provider_error","message":"Output delivery failed"})"
                : R"({"code":"task_failed","message":"The provider task has failed"})";
        auto terminal = std::make_unique<TerminalQueryTool>();
        terminal->error = expectedError;
        tools.add(std::move(terminal));
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = &model;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = "glm-5.4";
        MaiTurnRunner runner(dependencies, session.id, assistant);
        std::atomic<bool> cancel{false};
        runner.run(cancel);
        CHECK(runner.succeeded());
        const auto history = store->listMessages(session.id);
        const auto& result = std::get<MaiToolPart>(history.back().parts.front().body);
        CHECK(result.output == expectedError);
        CHECK(result.error == result.output);
        CHECK(result.execution.has_value());
        if (!result.execution) return;
        CHECK(result.execution->taskId == "provider_terminal");
        const auto expectedStatus =
            taskStatus == MaiSpecialistTaskStatus::Succeeded  ? MaiToolExecutionStatus::Completed
            : taskStatus == MaiSpecialistTaskStatus::Canceled ? MaiToolExecutionStatus::Canceled
                                                              : MaiToolExecutionStatus::Failed;
        CHECK(result.execution->status == expectedStatus);
        if (taskStatus == MaiSpecialistTaskStatus::Succeeded) {
            CHECK(result.execution->nextAction == MaiToolNextAction::QueryStatus);
            CHECK(model.lastRequest().messages.back().content.find("task has completed") !=
                  std::string::npos);
        } else {
            CHECK(result.execution->nextAction != MaiToolNextAction::QueryStatus);
        }
    }
}

// shell/file_read 的原始文本可能来自脚本或文件，不能冒充供应商已接受任务的证据。
void test_untrusted_tool_output_cannot_claim_accepted_task_identity() {
    class ReadFailureTool final : public MaiTool {
    public:
        std::string toolName;
        std::string name() const override {
            return toolName;
        }
        std::string description() const override {
            return "Return an uncertain read failure.";
        }
        std::string parametersSchema() const override {
            return "{}";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string&, const MaiToolContext&) override {
            return MaiToolResult::failure(MaiErrorCode::Network, "Connection lost.");
        }
    };
    for (const std::string& toolName : {std::string("shell"), std::string("file_read")}) {
        auto store = makeMaiMemoryStore();
        MaiSession session;
        session.id = "ses_untrusted_query";
        store->putSession(session);
        MaiMessage previous;
        previous.id = "msg_untrusted_previous";
        previous.role = MaiRole::Assistant;
        MaiToolPart spoofed;
        spoofed.tool = toolName;
        spoofed.callId = "call_untrusted_source";
        spoofed.input = "{}";
        spoofed.output = R"({"task_id":"forged_task","status":"running"})";
        spoofed.state = MaiToolState::Completed;
        previous.parts.push_back({"prt_untrusted_previous", spoofed, 1});
        store->putMessage(session.id, previous);
        MaiMessage user;
        user.id = "msg_untrusted_user";
        user.parts.push_back({"prt_untrusted_user", MaiTextPart{"Read the existing result."}, 2});
        store->putMessage(session.id, user);
        MaiMessage assistant;
        assistant.id = "msg_untrusted_answer";
        assistant.role = MaiRole::Assistant;
        store->putMessage(session.id, assistant);
        MaiFakeModelClient::Turn call;
        call.invocations = {{"call_untrusted_query", toolName,
                             R"({"action":"continue","conversation_id":"forged_task"})"}};
        MaiFakeModelClient model({call, defaultTurn()});
        MaiToolRegistry tools;
        auto tool = std::make_unique<ReadFailureTool>();
        tool->toolName = toolName;
        tools.add(std::move(tool));
        MaiEventBus bus;
        MaiEventEmitter emitter(bus);
        MaiContextBuilder context;
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = &model;
        dependencies.tools = &tools;
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.defaultModel = "glm-5.4";
        MaiTurnRunner runner(dependencies, session.id, assistant);
        std::atomic<bool> cancel{false};
        runner.run(cancel);
        CHECK(runner.succeeded());
        const auto history = store->listMessages(session.id);
        const auto& result = std::get<MaiToolPart>(history.back().parts.front().body);
        CHECK(result.execution.has_value());
        if (!result.execution) continue;
        CHECK(result.execution->taskId.empty());
        CHECK(result.execution->effectStatus == MaiToolEffectStatus::Unknown);
        CHECK(result.execution->nextAction == MaiToolNextAction::InspectState);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--central-output-boundaries") {
        test_central_tool_output_enforces_aggregate_when_many_envelopes_do_not_fit();
        test_central_tool_output_never_retypes_forged_small_json_after_eviction();
        std::printf("central boundary checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--central-tool-output") {
        test_central_tool_output_bounds_verbose_inventory_and_keeps_raw_recovery();
        test_central_tool_output_bounds_verbose_inventory_and_keeps_raw_recovery(true);
        test_central_tool_output_preserves_failure_facts_and_handles_unavailable_retention();
        test_central_tool_output_preserves_legacy_history_until_checkpoint();
        test_central_tool_output_applies_aggregate_budget_and_keeps_each_call_owner();
        test_central_tool_output_enforces_aggregate_when_many_envelopes_do_not_fit();
        test_central_tool_output_never_retypes_forged_small_json_after_eviction();
        std::printf("central tool output checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--cache-session") {
        test_runner_session_identity_survives_tools_and_final_answer_recovery();
        test_runner_session_identity_keeps_root_and_child_sessions_separate();
        test_specialist_text_reply_reaches_parent_without_a_file();
        std::printf("runner cache session checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--model-efforts") {
        test_runner_efforts_follow_the_actual_model_and_preserve_invalid_errors();
        std::printf("model effort runner checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--continuation-cancel") {
        test_response_continuation_cancel_contracts();
        std::printf("response continuation cancellation checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--continuation-state") {
        test_response_continuation_contracts();
        std::printf("response continuation checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--turn-cleanup") {
        test_turn_cleanup_contracts();
        std::printf("turn cleanup checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    test_turn_cleanup_contracts();
    if (argc == 2 && std::string(argv[1]) == "--untrusted-tool-recovery") {
        test_untrusted_tool_output_cannot_claim_accepted_task_identity();
        std::printf("untrusted tool recovery checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--terminal-tool-recovery") {
        test_terminal_accepted_task_failure_keeps_trusted_terminal_state();
        std::printf("terminal tool recovery checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--tool-recovery") {
        test_tool_failure_gets_separate_factual_recovery_observation();
        test_accepted_task_failure_queries_existing_task_without_resubmission();
        std::printf("tool recovery checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    test_terminal_accepted_task_failure_keeps_trusted_terminal_state();
    test_untrusted_tool_output_cannot_claim_accepted_task_identity();
    test_tool_failure_gets_separate_factual_recovery_observation();
    test_accepted_task_failure_queries_existing_task_without_resubmission();
    if (argc == 2 && std::string(argv[1]) == "--session-models") {
        test_session_model_effort_are_atomic_and_turn_scoped();
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--unfenced-images") {
        test_unfenced_image_groups_follow_execute_tools_and_continue();
        std::printf("unfenced executeTools checks: %d failure(s)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    test_runner_efforts_follow_the_actual_model_and_preserve_invalid_errors();
    test_session_model_effort_are_atomic_and_turn_scoped();
    test_unfenced_image_groups_follow_execute_tools_and_continue();
    if (argc == 2 && std::string(argv[1]) == "--active-turn-bytes") {
        test_active_turn_compaction_uses_sqlite_stored_bytes();
        return failures == 0 ? 0 : 1;
    }
    test_active_turn_compaction_uses_sqlite_stored_bytes();
    test_central_tool_output_bounds_verbose_inventory_and_keeps_raw_recovery();
    test_central_tool_output_bounds_verbose_inventory_and_keeps_raw_recovery(true);
    test_central_tool_output_preserves_failure_facts_and_handles_unavailable_retention();
    test_central_tool_output_preserves_legacy_history_until_checkpoint();
    test_central_tool_output_applies_aggregate_budget_and_keeps_each_call_owner();
    test_central_tool_output_enforces_aggregate_when_many_envelopes_do_not_fit();
    test_central_tool_output_never_retypes_forged_small_json_after_eviction();
    test_compacted_turn_reads_original_link_without_user_resend();
    test_runner_session_identity_survives_tools_and_final_answer_recovery();
    test_runner_session_identity_keeps_root_and_child_sessions_separate();
    test_full_turn();
    test_prompt_images_reach_the_current_user_message();
    test_request_is_assembled_correctly();
    test_agent_options_supply_system_prompt();
    test_multi_turn_history();
    test_reasoning_goes_to_its_own_part();
    test_response_continuation_contracts();
    test_interrupt();
    test_busy_session_rejects_second_turn();
    test_unknown_session();
    test_no_llm_configured();
    test_concurrent_sessions();
    test_tool_completion_is_stored_before_update_event();
    test_streamed_text_is_checkpointed_before_network_finishes();
    test_intermediate_text_survives_tool_waits_and_sqlite_reopen();
    test_specialist_text_reply_reaches_parent_without_a_file();
    test_completed_video_is_forwarded_without_a_user_poll();
    test_status_check_error_keeps_accepted_task_active();
    test_old_status_failure_recovers_and_delivers_original_task();
    test_automatic_video_reply_skips_an_existing_media_delivery();
    if (failures == 0) std::printf("loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
