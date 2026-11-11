// 文件作用：验证“编排一轮模型调用、工具执行和事件发布”的正确行为和边界条件。
// 验证入口：test_prompt_images_reach_the_current_user_message、test_full_turn、
// test_request_is_assembled_correctly；检查可观察结果而非实现细节。
// agent loop 的测试：从 submit(Prompt) 到事件流吐完的完整一圈。
//
// 用假的 Chat Completions 服务端当模型，所以不需要 API key、不花钱、结果完全确定。
// 真实 GLM 说的是同一个协议，切过去只改 baseUrl。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiCloudModelCatalog.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiFakeModelClient.h"
#include "MaiTool.h"
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

// ── 假模型：一个真的 Chat Completions 端点 ─────────────────────
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
        return MaiWireApi::ChatCompletions;
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
    if (options.defaultModel.empty()) options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), nullptr, options),
            observer};
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
    CHECK(request.messages.size() == 2);
    if (request.messages.size() == 2) {
        CHECK(request.messages[0].role == MaiModelRole::System);
        CHECK(request.messages[1].content.find("child answer") != std::string::npos);
        CHECK(request.messages[1].content.find("Summarize the result") != std::string::npos);
    }
    const auto afterTool = modelObserver->request(1);
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

// 通过真实 Agent/TurnRunner 自动触发摘要，再让假模型调用当前会话原史工具找回旧 URL。
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
            return {8192, 2048};
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
        std::string text =
            std::string(4000, 'a') + "\n" +
            (index == 0 ? originalUrl : std::string("Preserve the existing source.")) + "\n" +
            std::string(4000, 'b');
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
        store->putMessage(session.id, assistant);
    }
    auto* observedStore = store.get();
    auto model = std::make_unique<RecallModel>();
    model->expectedUrl = originalUrl;
    model->originalMessageId = originalId;
    auto* observedModel = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools);
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(agent.submit(MaiSendPrompt{session.id, "Continue using my earlier original link."}));
    agent.waitIdle();
    CHECK(observedModel->summaryCalls > 0);
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
            return R"({"type":"object"})";
        }
        bool requiresWorkingDirectory(const std::string&) const override {
            return false;
        }
        MaiToolResult execute(const std::string& input, const MaiToolContext&) override {
            return MaiToolResult::success(input + std::string(1, static_cast<char>(0xff)) +
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
            return {8192, 1024};
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
                        sink.onToolCall({"raw_call_" + std::to_string(index), "raw_fixture", "{}"});
            } else if (sink.onText)
                sink.onText("The active tool loop continued after compaction.");
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
    tools->add(std::make_unique<RawTool>());
    {
        MaiAgent agent(std::move(store), std::move(model), std::move(tools));
        CHECK(agent.submit(
            MaiSendPrompt{session.id, "Preserve this instruction and complete all tool work."}));
        agent.waitIdle();
        CHECK(observedModel->summaries > 0);
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
                    CHECK(oldTool->output.find(static_cast<char>(0xff)) == std::string::npos);
                    CHECK(oldTool->output.find("\xef\xbf\xbd") != std::string::npos);
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
                return R"({"type":"object","properties":{"index":{"type":"integer"},"phase":{"type":"integer"}}})";
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
                return {32768, 4096};
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
                CHECK(request.maxOutputTokens == 0);
                if (mainRequests > 1) {
                    bool keptUser = false;
                    int observations = 0;
                    for (const auto& message : request.messages) {
                        if (message.role == MaiModelRole::User && message.content == instruction)
                            keptUser = true;
                        for (const auto& image : message.images) {
                            ++observations;
                            CHECK(image.path.find("phase-" + std::to_string(mainRequests - 2) +
                                                  "-") != std::string::npos);
                            std::string actual;
                            CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(image.path),
                                                           actual, 1024));
                            CHECK(actual == pixels);
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
                    for (int index = 0; index < 20; ++index)
                        if (sink.onToolCall)
                            sink.onToolCall(
                                {"phase_" + std::to_string(mainRequests - 1) + "_" +
                                     std::to_string(index),
                                 index == 19 ? "screenshot_fixture" : "read_fixture",
                                 nlohmann::json{{"index", index}, {"phase", mainRequests - 1}}
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
            CHECK(observedModel->mainRequests == 3 && observedModel->summaryRequests > 0);
            std::printf("     images=%d main_requests=%d summary_requests=%d\n", multiImage ? 2 : 1,
                        observedModel->mainRequests, observedModel->summaryRequests);
            MaiContextCheckpoint checkpoint;
            CHECK(observedStore->getContextCheckpoint(session.id, checkpoint));
            CHECK(checkpoint.revision == observedModel->firstCheckpoint.revision + 1);
            const auto after = agent.listMessages(session.id);
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
            const int glmCycle = request.model == "glm-5.3" ? ++glmRequests : 0;
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
    const auto a = agent.submit(MaiCreateSession{"/tmp/work", "A", "glm-5.3", "low"}).value();
    const auto b =
        agent.submit(MaiCreateSession{"/tmp/work", "B", "deepseek-v4-pro", "max"}).value();
    CHECK(!agent.submit(MaiCreateSession{"/tmp/work", "Bad", "glm-unknown", "low"}));
    CHECK(!agent.submit(MaiCreateSession{"/tmp/work", "Bad", "glm-5.3", "medium"}));
    CHECK(agent.submit(MaiUpdateSession{b, "Must not change", "glm-5.3", "", "", "medium"})
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
    CHECK(observedTool->actualModel == "glm-5.3");
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
            CHECK((request.model == "glm-5.3" && request.reasoningEffort == "low") ||
                  (request.model == "deepseek-v4-pro" && request.reasoningEffort == "high"));
    }
    CHECK(agent.submit(MaiUpdateSession{a, "", "", "", "/tmp/new-work"}));
    MaiSession after;
    CHECK(agent.getSession(a, after));
    CHECK(after.model == "glm-5.3" && after.reasoningEffort == "low" && after.title == "A");
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
        dependencies.defaultModel = "glm-5.3";
        MaiTurnRunner runner(dependencies, session.id, assistant);
        std::atomic<bool> cancel{false};
        runner.run(cancel);
        CHECK(runner.succeeded());
        CHECK(observed->executions == 1);
        const auto history = store->listMessages(session.id);
        CHECK(history.back().parts.size() == 3);
        if (history.back().parts.size() != 3) continue;
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
        dependencies.defaultModel = "glm-5.3";
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
        dependencies.defaultModel = "glm-5.3";
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
    test_session_model_effort_are_atomic_and_turn_scoped();
    test_unfenced_image_groups_follow_execute_tools_and_continue();
    if (argc == 2 && std::string(argv[1]) == "--active-turn-bytes") {
        test_active_turn_compaction_uses_sqlite_stored_bytes();
        return failures == 0 ? 0 : 1;
    }
    test_active_turn_compaction_uses_sqlite_stored_bytes();
    test_compacted_turn_reads_original_link_without_user_resend();
    test_full_turn();
    test_prompt_images_reach_the_current_user_message();
    test_request_is_assembled_correctly();
    test_agent_options_supply_system_prompt();
    test_multi_turn_history();
    test_reasoning_goes_to_its_own_part();
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
