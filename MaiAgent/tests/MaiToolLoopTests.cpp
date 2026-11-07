// 文件作用：验证“MaiToolLoop”的正确行为和边界条件。
// 验证入口：test_tool_loop_closes、test_balance_error_routes_back_to_main_model、
// test_tool_image_is_persisted_and_fed_back_to_the_model、
// test_screenshot_stop_without_workspace_keeps_file_tools_blocked；检查可观察结果而非实现细节。
// 工具循环测试：模型说要调工具 → 我们执行 → 结果回灌 → 它接着答。
//
// 这是 agent 之所以是 agent 的地方，也是 M3 唯一真正需要证明的东西。
// 假模型按脚本依次返回"要调工具"和"最终回答"，所以整个闭环是确定的。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"
#include "MaiFileTools.h"
#include "MaiFakeModelClient.h"
#include "MaiIdGenerator.h"
#include "MaiScreenshotTool.h"
#include "MaiViewImageTool.h"

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

// 一次模型应答的剧本：要么吐一句话，要么发起一次工具调用。
//
// 不起假 HTTP 服务端。这个文件测的是工具循环本身——调用发出去、执行、结果回灌、
// 再问一遍——中间那层传输是噪音。以前想断言"第二次请求带着调用和结果、而且 toolCallId 对得上"，
// 得去 JSON 里翻`messages[i]["tool_calls"][0]["id"]`；现在直接看`message.invocations[0].id`，
// 编译器帮着查类型。
//
// tool_calls 在线上怎么嵌套（function.name 那一层）是线格式的事，归 MaiModelClientTests 管，
// 那边走真 socket。
MaiFakeModelClient::Turn sayTurn(const std::string& text) {
    MaiFakeModelClient::Turn turn;
    turn.textChunks = {text};
    return turn;
}

MaiFakeModelClient::Turn callTurn(const std::string& tool, const std::string& arguments,
                                  const std::string& callId = "call_1") {
    MaiFakeModelClient::Turn turn;
    turn.invocations.push_back(MaiToolInvocation{callId, tool, arguments});
    return turn;
}

MaiFakeModelClient::Turn reasoningTurn(const std::string& text) {
    MaiFakeModelClient::Turn turn;
    turn.reasoning = text;
    return turn;
}

struct Workspace {
    fs::path root;
    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-loop-" + std::to_string(std::rand()));
        fs::create_directories(root / "src");
        std::ofstream(root / "src" / "hello.txt", std::ios::binary) << "line one\nline two\n";
    }
    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // 注意：不要用 root / "中文"，MSVC 会按 ANSI 代码页解释字面量。
    std::string utf8Root() const {
        return root.u8string();
    }
};

struct AgentUnderTest {
    std::unique_ptr<MaiAgent> agent;
    MaiFakeModelClient* model = nullptr;  // agent 持有，这里只借着看
};

class ImageResultTool final : public MaiTool {
public:
    explicit ImageResultTool(std::string path) : mPath(std::move(path)) {}

    std::string name() const override {
        return "screenshot";
    }
    std::string description() const override {
        return "Capture the current display.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        if (context.model != "glm-5.3") {
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "tool did not receive the active model name");
        }
        return MaiToolResult::successWithImages("Captured the current display.",
                                                {MaiToolImage{mPath, "image/png"}});
    }

private:
    std::string mPath;
};

class TextResultTool final : public MaiTool {
public:
    std::string name() const override {
        return "status";
    }
    std::string description() const override {
        return "Return a status value.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success("ready");
    }
};

class UserImagePreviewTool final : public MaiTool {
public:
    explicit UserImagePreviewTool(std::atomic<int>& calls) : mCalls(calls) {}
    std::string name() const override {
        return "mobile_preview_image";
    }
    std::string description() const override {
        return "Display an image to the user without model vision.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++mCalls;
        return MaiToolResult::success(
            "Displayed the image to the user; image pixels were not analyzed.");
    }

private:
    std::atomic<int>& mCalls;
};

class InternalVideoTool final : public MaiTool {
public:
    explicit InternalVideoTool(std::atomic<int>& calls) : mCalls(calls) {}
    std::string name() const override {
        return "seedance_video";
    }
    std::string description() const override {
        return "Internal task recovery only.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++mCalls;
        return MaiToolResult::success("submitted");
    }

private:
    std::atomic<int>& mCalls;
};

class BalanceErrorTool final : public MaiTool {
public:
    std::string name() const override {
        return "paid_video";
    }
    std::string description() const override {
        return "Fake paid video provider";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::failure(
            MaiErrorCode::Network,
            R"({"code":"provider_error","provider_code":1102,"http_status":429,)"
            R"("message":"Account balance not enough"})");
    }
};

class AlternateVideoTool final : public MaiTool {
public:
    std::string name() const override {
        return "alternate_video";
    }
    std::string description() const override {
        return "Fake alternative video provider";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return MaiToolResult::success(R"({"status":"submitted","task_id":"alternate-task"})");
    }
};

AgentUnderTest makeAgent(std::vector<MaiFakeModelClient::Turn> script,
                         MaiAgent::Options options = {}, bool withTools = true) {
    auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
    MaiFakeModelClient* observer = model.get();

    std::unique_ptr<MaiToolRegistry> tools;
    if (withTools) {
        tools = std::make_unique<MaiToolRegistry>();
        registerMaiBuiltinTools(*tools);
    }
    if (options.defaultModel.empty()) options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), std::move(tools),
                                       options),
            observer};
}

AgentUnderTest makeAgentWithImageTool(std::vector<MaiFakeModelClient::Turn> script,
                                      const std::string& imagePath) {
    auto model = std::make_unique<MaiFakeModelClient>(std::move(script));
    MaiFakeModelClient* observer = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<ImageResultTool>(imagePath));
    tools->add(std::make_unique<TextResultTool>());
    MaiAgent::Options options;
    options.defaultModel = "glm-5.3";
    return {std::make_unique<MaiAgent>(makeMaiMemoryStore(), std::move(model), std::move(tools),
                                       options),
            observer};
}

struct Recorder {
    std::mutex mu;
    std::vector<MaiEvent> events;
    void attach(MaiAgent& a) {
        a.eventBus().subscribe([this](const MaiEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
    }
    std::vector<MaiEvent> all() {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }
};

// ── 用例 ────────────────────────────────────────────────────────

void test_tool_loop_closes() {
    Workspace workspace;
    // 第一次：要调 read。第二次：拿到内容后给出回答。
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"src/hello.txt"})", "call_read"),
                                sayTurn("The file has two lines.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(*agent);

    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent->waitIdle();

    // 1. 模型被请求了两次——工具循环确实转了一圈
    CHECK(model->requestCount() == 2);

    // 2. 第一次请求里带了工具清单
    const MaiModelRequest first = model->request(0);
    // 按名字查而不是比个数：加一个工具不该让这条用例红。
    CHECK(!first.tools.empty());
    bool hasRead = false;
    for (const auto& tool : first.tools)
        if (tool.name == "file_read") hasRead = true;
    CHECK(hasRead);

    // 3. 第二次请求里必须带着调用和结果，而且 toolCallId 对得上——
    //    对不上的话模型认不出这是哪次调用的结果，下一轮会重复调。
    const MaiModelRequest second = model->request(1);
    bool foundAssistantCall = false;
    bool foundToolResult = false;
    std::string callId;
    for (const auto& message : second.messages) {
        if (message.role == MaiModelRole::Assistant && !message.invocations.empty()) {
            foundAssistantCall = true;
            callId = message.invocations[0].id;
            CHECK(message.invocations[0].name == "file_read");
        }
        if (message.role == MaiModelRole::ToolResult) {
            foundToolResult = true;
            CHECK(message.toolCallId == callId);
            // 真的把文件内容回灌了
            CHECK(message.content.find("line one") != std::string::npos);
        }
    }
    CHECK(foundAssistantCall);
    CHECK(foundToolResult);

    // 4. 落库里有一个完成状态的 tool part
    const auto msgs = agent->listMessages(sessionId);
    CHECK(msgs.size() == 2);
    const MaiToolPart* toolPart = nullptr;
    const MaiTextPart* final_text = nullptr;
    if (msgs.size() == 2) {
        for (const auto& p : msgs[1].parts) {
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
            if (const auto* t = std::get_if<MaiTextPart>(&p.body)) final_text = t;
        }
    }
    CHECK(toolPart != nullptr);
    if (toolPart) {
        CHECK(toolPart->tool == "file_read");
        CHECK(toolPart->state == MaiToolState::Completed);
        CHECK(toolPart->output.find("line one") != std::string::npos);
    }
    // 5. 最终回答也在
    CHECK(final_text != nullptr);
    if (final_text) CHECK(final_text->text == "The file has two lines.");

    // 6. 界面能看到工具卡的状态变化
    std::size_t partUpdates = 0;
    for (const auto& e : recorder.all())
        if (e.type == MaiEventType::MessagePartUpdated) ++partUpdates;
    CHECK(partUpdates >= 2);  // 至少 running 和 completed 各一次
}

void test_screenshot_stop_without_workspace_keeps_file_tools_blocked() {
    // 使用真实截图工具和文件工具经过完整 Agent dispatch，不能只证明直接 execute 绕过了工作区检查。
    // 空目录会话中的 capture/read 必须在原生采集或文件访问之前拒绝；整个用例不采屏、不输入。
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("screenshot", R"({"action":"stop"})", "stop_without_workspace"),
        callTurn("screenshot", R"({"action":"capture"})", "capture_without_workspace"),
        callTurn("file_read", R"({"path":"blocked.txt"})", "read_without_workspace"),
        sayTurn("Cleanup completed; workspace access remains unavailable.")});
    MaiFakeModelClient* observed = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(makeMaiScreenshotTool());
    registerMaiFileTools(*tools);
    MaiAgent::Options options;
    options.defaultModel = "deepseek-flash";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const auto created = agent.submit(MaiCreateSession{"", "", "deepseek-flash"});
    CHECK(created);
    if (!created) return;
    CHECK(agent.submit(
        MaiSendPrompt{created.value(), "Stop capture without changing workspace access."}));
    agent.waitIdle();

    CHECK(observed->requestCount() == 4);
    CHECK(agent.listPendingPermissions().empty());
    bool stopFedBack = false;
    for (std::size_t index = 0; index < observed->requestCount(); ++index) {
        const MaiModelRequest request = observed->request(index);
        CHECK(request.model == "deepseek-flash");
        CHECK(request.workingDirectory.empty());
        for (const MaiModelMessage& message : request.messages) {
            CHECK(message.images.empty());
            if (message.role == MaiModelRole::ToolResult &&
                message.toolCallId == "stop_without_workspace") {
                const auto output = nlohmann::json::parse(message.content, nullptr, false);
                CHECK(output == nlohmann::json({{"action", "stop"}, {"state", "stopped"}}));
                stopFedBack = true;
            }
        }
    }
    CHECK(stopFedBack);

    const auto history = agent.listMessages(created.value());
    CHECK(history.size() == 2);
    if (history.size() != 2) return;
    CHECK(history.back().completed != 0);
    CHECK(history.back().text() == "Cleanup completed; workspace access remains unavailable.");
    bool stopped = false;
    int workspaceRejections = 0;
    for (const MaiMessagePart& part : history.back().parts) {
        CHECK(!std::holds_alternative<MaiImagePart>(part.body));
        const auto* tool = std::get_if<MaiToolPart>(&part.body);
        if (!tool) continue;
        if (tool->callId == "stop_without_workspace") {
            CHECK(tool->tool == "screenshot");
            CHECK(tool->state == MaiToolState::Completed);
            CHECK(tool->error.empty());
            const auto output = nlohmann::json::parse(tool->output, nullptr, false);
            CHECK(output == nlohmann::json({{"action", "stop"}, {"state", "stopped"}}));
            stopped = true;
        } else if (tool->callId == "capture_without_workspace" ||
                   tool->callId == "read_without_workspace") {
            CHECK(tool->state == MaiToolState::Error);
            CHECK(tool->error.find("working directory") != std::string::npos);
            CHECK(tool->output == tool->error);
            ++workspaceRejections;
        }
    }
    CHECK(stopped);
    CHECK(workspaceRejections == 2);
}

void test_balance_error_routes_back_to_main_model() {
    Workspace workspace;
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("paid_video", R"({"message":"create a cat video"})"),
        callTurn("alternate_video", R"({"message":"create a cat video"})"),
        sayTurn("The video task is underway.")});
    MaiFakeModelClient* observer = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<BalanceErrorTool>());
    tools->add(std::make_unique<AlternateVideoTool>());
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "create a video"});
    agent.waitIdle();

    CHECK(observer->requestCount() == 3);
    bool sawRecovery = false;
    for (const MaiModelMessage& message : observer->request(1).messages) {
        if (message.role == MaiModelRole::ToolResult &&
            message.content.find("\"provider_code\":1102") != std::string::npos &&
            message.content.find("Check other configured specialist tools") != std::string::npos &&
            message.content.find("ask for a top-up") != std::string::npos)
            sawRecovery = true;
    }
    CHECK(sawRecovery);
    bool sawAlternative = false;
    for (const MaiModelMessage& message : observer->request(2).messages) {
        if (message.role == MaiModelRole::ToolResult &&
            message.content.find("alternate-task") != std::string::npos)
            sawAlternative = true;
    }
    CHECK(sawAlternative);
}

void test_tool_image_is_persisted_and_fed_back_to_the_model() {
    Workspace workspace;
    const fs::path screenshot = workspace.root / "screenshot.png";
    std::ofstream(screenshot, std::ios::binary) << "png";
    auto underTest = makeAgentWithImageTool(
        {callTurn("screenshot", R"({})", "call_screenshot"), sayTurn("I can see it.")},
        screenshot.u8string());
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();

    agent.submit(MaiSendPrompt{sessionId, "inspect my screen"});
    agent.waitIdle();

    CHECK(model->requestCount() == 2);
    const MaiModelRequest second = model->request(1);
    bool foundToolResult = false;
    bool foundScreenshot = false;
    std::size_t toolResultIndex = 0;
    std::size_t screenshotIndex = 0;
    for (std::size_t i = 0; i < second.messages.size(); ++i) {
        const MaiModelMessage& message = second.messages[i];
        if (message.role == MaiModelRole::ToolResult && message.toolCallId == "call_screenshot") {
            foundToolResult = true;
            toolResultIndex = i;
        }
        if (message.role == MaiModelRole::User && message.images.size() == 1 &&
            message.images.front().path == screenshot.u8string()) {
            foundScreenshot = true;
            screenshotIndex = i;
            CHECK(message.images.front().mimeType == "image/png");
            CHECK(message.content.find("Image returned") != std::string::npos);
        }
    }
    CHECK(foundToolResult);
    CHECK(foundScreenshot);
    CHECK(toolResultIndex < screenshotIndex);

    bool persistedScreenshot = false;
    for (const MaiMessage& message : agent.listMessages(sessionId)) {
        for (const MaiMessagePart& part : message.parts) {
            const auto* image = std::get_if<MaiImagePart>(&part.body);
            if (image && image->path == screenshot.u8string() && image->mimeType == "image/png") {
                persistedScreenshot = true;
            }
        }
    }
    CHECK(persistedScreenshot);
}

void test_tool_image_keeps_parallel_tool_calls_in_one_assistant_batch() {
    Workspace workspace;
    const fs::path screenshot = workspace.root / "batch-screenshot.png";
    std::ofstream(screenshot, std::ios::binary) << "png";
    MaiFakeModelClient::Turn calls;
    calls.invocations = {MaiToolInvocation{"call_screenshot", "screenshot", R"({})"},
                         MaiToolInvocation{"call_status", "status", R"({})"}};
    auto underTest =
        makeAgentWithImageTool({calls, sayTurn("Both results arrived.")}, screenshot.u8string());
    MaiAgent& agent = *underTest.agent;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();

    agent.submit(MaiSendPrompt{sessionId, "inspect and report status"});
    agent.waitIdle();

    const MaiModelRequest second = underTest.model->request(1);
    std::size_t assistantBatches = 0;
    std::size_t toolResults = 0;
    std::size_t imageObservations = 0;
    std::size_t batchIndex = 0;
    std::size_t lastToolResultIndex = 0;
    std::size_t imageIndex = 0;
    for (std::size_t i = 0; i < second.messages.size(); ++i) {
        const MaiModelMessage& message = second.messages[i];
        if (message.role == MaiModelRole::Assistant && !message.invocations.empty()) {
            ++assistantBatches;
            batchIndex = i;
            CHECK(message.invocations.size() == 2);
        } else if (message.role == MaiModelRole::ToolResult) {
            ++toolResults;
            lastToolResultIndex = i;
        } else if (!message.images.empty()) {
            ++imageObservations;
            imageIndex = i;
        }
    }
    CHECK(assistantBatches == 1);
    CHECK(toolResults == 2);
    CHECK(imageObservations == 1);
    CHECK(batchIndex < lastToolResultIndex);
    CHECK(lastToolResultIndex < imageIndex);
}

void test_reasoning_only_after_tools_retries_for_final_answer() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"src/hello.txt"})", "call_read"),
                                reasoningTurn("I should summarize the tool output."),
                                sayTurn("The file has two lines.")});
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent.waitIdle();

    CHECK(model->requestCount() == 3);
    const MaiModelRequest finalRequest = model->request(2);
    CHECK(!finalRequest.messages.empty());
    if (!finalRequest.messages.empty()) {
        const MaiModelMessage& instruction = finalRequest.messages.back();
        CHECK(instruction.role == MaiModelRole::System);
        CHECK(instruction.content.find("complete final answer") != std::string::npos);
    }

    bool foundFinalText = false;
    const auto messages = agent.listMessages(sessionId);
    if (messages.size() == 2) {
        for (const auto& part : messages[1].parts) {
            const auto* text = std::get_if<MaiTextPart>(&part.body);
            if (text && text->text == "The file has two lines.") foundFinalText = true;
        }
    }
    CHECK(foundFinalText);
}

void test_repeated_missing_final_answer_reports_error() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"src/hello.txt"})"),
                                reasoningTurn("I should summarize this."),
                                reasoningTurn("I still did not provide the answer.")});
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "what is in src/hello.txt"});
    agent.waitIdle();

    CHECK(model->requestCount() == 3);
    bool foundError = false;
    for (const auto& event : recorder.all()) {
        if (event.type == MaiEventType::SessionError &&
            event.detail.find("without a final answer") != std::string::npos) {
            foundError = true;
        }
    }
    CHECK(foundError);
}

void test_tool_error_is_fed_back() {
    Workspace workspace;
    // 模型要读一个不存在的文件，然后（拿到错误后）改口
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"no-such-file.txt"})"),
                                sayTurn("That file does not exist.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read it"});
    agent->waitIdle();

    CHECK(model->requestCount() == 2);
    // 错误必须回灌——模型要知道失败了才能换个做法，而不是干等
    const MaiModelRequest second = model->request(1);
    bool fedError = false;
    for (const auto& message : second.messages)
        if (message.role == MaiModelRole::ToolResult &&
            message.content.find("does not exist") != std::string::npos)
            fedError = true;
    CHECK(fedError);

    const auto msgs = agent->listMessages(sessionId);
    const MaiToolPart* toolPart = nullptr;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
    CHECK(toolPart && toolPart->state == MaiToolState::Error);
}

void test_unknown_tool_does_not_kill_the_turn() {
    Workspace workspace;
    auto underTest =
        makeAgent({callTurn("made-up-tool", R"({})"), sayTurn("Sorry, I used the wrong tool.")});
    MaiAgent* agent = underTest.agent.get();
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "do something"});
    agent->waitIdle();

    // 整轮不能因此失败，而是把"没这个工具"告诉模型让它改
    CHECK(model->requestCount() == 2);
    const auto msgs = agent->listMessages(sessionId);
    bool hasFinalText = false;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiTextPart>(&p.body))
                if (t->text.find("wrong tool") != std::string::npos) hasFinalText = true;
    CHECK(hasFinalText);
}

void test_internal_tool_is_not_visible_or_callable_by_model() {
    Workspace workspace;
    std::atomic<int> calls{0};
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("seedance_video", R"({})"), sayTurn("The tool is unavailable.")});
    MaiFakeModelClient* observer = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->addInternal(std::make_unique<InternalVideoTool>(calls));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools));
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "create video"});
    agent.waitIdle();

    CHECK(observer->request(0).tools.empty());
    CHECK(calls.load(std::memory_order_relaxed) == 0);
    const auto messages = agent.listMessages(sessionId);
    bool rejected = false;
    if (messages.size() == 2)
        for (const MaiMessagePart& part : messages[1].parts)
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body))
                rejected = tool->state == MaiToolState::Error &&
                           tool->error.find("no tool named") != std::string::npos;
    CHECK(rejected);
}

void test_local_primary_blocks_vision_tool_but_keeps_user_display() {
    Workspace workspace;
    CHECK(
        !MaiFileSystem::writeFile(
             MaiFilePath::fromUtf8(workspace.utf8Root()).append(MaiFilePath::fromUtf8("photo.png")),
             std::string("\x89PNG\r\n\x1a\n", 8))
             .hasError());
    int analysisCalls = 0;
    std::atomic<int> displayCalls{0};
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("view_image", R"({"path":"photo.png"})", "call_analysis"),
        callTurn("mobile_preview_image", R"({"path":"photo.png"})", "call_display"),
        sayTurn("The photo is displayed. Image analysis is unavailable.")});
    auto* observer = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(makeMaiViewImageTool([&](const std::string& path, const MaiToolContext&) {
        ++analysisCalls;
        return MaiResult<std::string>(path);
    }));
    tools->add(std::make_unique<UserImagePreviewTool>(displayCalls));
    MaiAgent::Options options;
    options.defaultModel = "glm-5.3-flash";
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), options);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", "local:qa"}).value();
    agent.submit(MaiSendPrompt{sessionId, "Show the photo"});
    agent.waitIdle();
    CHECK(observer->requestCount() == 3);
    CHECK(analysisCalls == 0 && displayCalls.load() == 1);
    for (std::size_t index = 0; index < observer->requestCount(); ++index) {
        const auto request = observer->request(index);
        CHECK(request.model == "local:qa");
        CHECK(request.tools.size() == 1 && request.tools.front().name == "mobile_preview_image");
        for (const auto& message : request.messages) CHECK(message.images.empty());
    }
    bool rejectedAnalysis = false;
    bool displayed = false;
    for (const auto& message : agent.listMessages(sessionId)) {
        for (const auto& part : message.parts) {
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                if (tool->tool == "view_image")
                    rejectedAnalysis =
                        tool->state == MaiToolState::Error &&
                        tool->error.find("unavailable for the current primary model") !=
                            std::string::npos;
                if (tool->tool == "mobile_preview_image")
                    displayed = tool->state == MaiToolState::Completed;
            }
        }
    }
    CHECK(rejectedAnalysis && displayed);
}

void test_path_escape_through_model() {
    Workspace workspace;
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"../../../etc/passwd"})"),
                                sayTurn("I cannot read that path.")});
    MaiAgent* agent = underTest.agent.get();
    const std::string sessionId =
        agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent->submit(MaiSendPrompt{sessionId, "read the system password file"});
    agent->waitIdle();

    const auto msgs = agent->listMessages(sessionId);
    const MaiToolPart* toolPart = nullptr;
    if (msgs.size() == 2)
        for (const auto& p : msgs[1].parts)
            if (const auto* t = std::get_if<MaiToolPart>(&p.body)) toolPart = t;
    CHECK(toolPart != nullptr);
    if (toolPart) {
        CHECK(toolPart->state == MaiToolState::Error);
        CHECK(toolPart->output.find("outside the area accessible") != std::string::npos);
    }
}

void test_iteration_cap() {
    Workspace workspace;
    // 模型一直要调工具，永不收手——真实中会发生（它会绕圈）
    std::vector<MaiFakeModelClient::Turn> script;
    for (int i = 0; i < 40; ++i)
        script.push_back(callTurn("file_read", R"({"path":"src/hello.txt"})"));

    MaiAgent::Options options;
    options.maxToolIterations = 3;  // 调小便于测试
    auto underTest = makeAgent(std::move(script), options);
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;

    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "loop forever"});
    agent.waitIdle();

    // 到上限就停，不能无限烧钱
    CHECK(model->requestCount() == 4);
    CHECK(model->request(3).tools.empty());
    // 而且要明确告诉用户停在哪儿了，不是悄悄结束让人以为跑完了
    bool toldUser = false;
    for (const auto& e : recorder.all())
        if (e.type == MaiEventType::SessionError &&
            e.detail.find("tool-call limit") != std::string::npos)
            toldUser = true;
    CHECK(toldUser);
}

void test_iteration_cap_provides_a_final_status() {
    Workspace workspace;
    MaiAgent::Options options;
    options.maxToolIterations = 3;
    auto underTest = makeAgent({callTurn("file_read", R"({"path":"src/hello.txt"})"),
                                callTurn("file_read", R"({"path":"src/hello.txt"})"),
                                callTurn("file_read", R"({"path":"src/hello.txt"})"),
                                sayTurn("I read the file, but the task needs more work.")},
                               options);
    Recorder recorder;
    recorder.attach(*underTest.agent);
    const std::string sessionId =
        underTest.agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    underTest.agent->submit(MaiSendPrompt{sessionId, "inspect the file"});
    underTest.agent->waitIdle();

    CHECK(underTest.model->requestCount() == 4);
    CHECK(underTest.model->request(3).tools.empty());
    CHECK(underTest.model->request(3).messages.back().content.find("what remains unfinished") !=
          std::string::npos);
    bool hasStatus = false;
    for (const auto& message : underTest.agent->listMessages(sessionId))
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                hasStatus |= text->text.find("needs more work") != std::string::npos;
    CHECK(hasStatus);
    for (const auto& event : recorder.all()) CHECK(event.type != MaiEventType::SessionError);
}

void test_distinct_tool_calls_continue_past_twelve_rounds() {
    Workspace workspace;
    std::vector<MaiFakeModelClient::Turn> script;
    for (int i = 0; i < 15; ++i) {
        const std::string path = "src/step" + std::to_string(i) + ".txt";
        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(workspace.utf8Root() + "/" + path),
                                        "step " + std::to_string(i)));
        script.push_back(callTurn("file_read", "{\"path\":\"" + path + "\"}"));
    }
    script.push_back(sayTurn("All fifteen files have been inspected."));
    auto underTest = makeAgent(std::move(script));
    Recorder recorder;
    recorder.attach(*underTest.agent);
    const std::string sessionId =
        underTest.agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    underTest.agent->submit(MaiSendPrompt{sessionId, "inspect the files"});
    underTest.agent->waitIdle();

    CHECK(underTest.model->requestCount() == 16);
    bool hasAnswer = false;
    for (const auto& message : underTest.agent->listMessages(sessionId))
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<MaiTextPart>(&part.body))
                hasAnswer |= text->text.find("fifteen files") != std::string::npos;
    CHECK(hasAnswer);
    for (const auto& event : recorder.all()) CHECK(event.type != MaiEventType::SessionError);
}

void test_repeated_tool_results_stop_before_hard_limit() {
    Workspace workspace;
    std::vector<MaiFakeModelClient::Turn> script;
    for (int i = 0; i < 40; ++i)
        script.push_back(callTurn("file_read", R"({"path":"src/hello.txt"})"));
    auto underTest = makeAgent(std::move(script));
    Recorder recorder;
    recorder.attach(*underTest.agent);
    const std::string sessionId =
        underTest.agent->submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    underTest.agent->submit(MaiSendPrompt{sessionId, "loop forever"});
    underTest.agent->waitIdle();

    CHECK(underTest.model->requestCount() == 4);
    CHECK(underTest.model->request(3).tools.empty());
    CHECK(underTest.model->request(3).messages.back().content.find("identical results") !=
          std::string::npos);
}

void test_no_tools_means_no_tool_field() {
    Workspace workspace;
    // 不给工具注册表 = 纯对话模式
    auto underTest = makeAgent({sayTurn("just chatting")}, {}, /*withTools=*/false);
    MaiAgent& agent = *underTest.agent;
    MaiFakeModelClient* model = underTest.model;
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "chat"});
    agent.waitIdle();

    // 请求里不该带工具清单——模型看不到工具就不会尝试调用。
    // 这个清单空着时线上会不会真的省掉 tools 字段，是序列化那一层的事，归 MaiModelClientTests 管。
    CHECK(model->request(0).tools.empty());
}

void test_model_exception_becomes_a_session_error() {
    class ThrowingModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest&, const MaiStreamSink&,
                        const std::atomic<bool>&) override {
            throw std::runtime_error("model exploded");
        }

        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }
    };

    Workspace workspace;
    auto model = std::make_unique<ThrowingModel>();
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr);
    Recorder recorder;
    recorder.attach(agent);
    const std::string sessionId =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    agent.submit(MaiSendPrompt{sessionId, "trigger the model"});
    agent.waitIdle();

    bool reported = false;
    for (const MaiEvent& event : recorder.all()) {
        if (event.type == MaiEventType::SessionError &&
            event.detail.find("model exploded") != std::string::npos) {
            reported = true;
        }
    }
    CHECK(reported);
}

// 条件变量卡住真实工具边界，证明补充输入不会用 cancel 打断已开始的工具。
class SteeringHeldTool final : public MaiTool {
public:
    std::string name() const override {
        return "held_work";
    }
    std::string description() const override {
        return "Wait for the local test gate.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        entered.notify_all();
        const bool released = wake.wait_for(lock, std::chrono::seconds(5), [&] { return ready; });
        canceled = context.cancel && context.cancel->load(std::memory_order_relaxed);
        return released ? MaiToolResult::success("current result")
                        : MaiToolResult::failure(MaiErrorCode::Internal, "test gate timed out");
    }
    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        return entered.wait_for(lock, std::chrono::seconds(5), [&] { return calls > 0; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        wake.notify_all();
    }
    std::atomic<int> calls{0};
    std::atomic<bool> canceled{false};

private:
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable wake;
    bool ready = false;
};

void test_steer_waits_for_all_tool_results_and_reaches_the_next_model_request() {
    Workspace workspace;
    MaiFakeModelClient::Turn batch;
    batch.invocations = {{"held_1", "held_work", "{}"}, {"status_2", "status", "{}"}};
    auto nextTurn = sayTurn("A new turn");
    nextTurn.chunkDelayMilliseconds = 100;
    auto model = std::make_unique<MaiFakeModelClient>(
        std::vector<MaiFakeModelClient::Turn>{batch, callTurn("status", "{}", "after_steer"),
                                              sayTurn("Here is the current result."), nextTurn});
    MaiFakeModelClient* observed = model.get();
    auto registry = std::make_unique<MaiToolRegistry>();
    auto held = std::make_unique<SteeringHeldTool>();
    SteeringHeldTool* gate = held.get();
    registry->add(std::move(held));
    registry->add(std::make_unique<TextResultTool>());
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(registry));
    const std::string session =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    const auto started = agent.submit(MaiSendPrompt{session, "Improve this image"});
    CHECK(started.isOk());
    CHECK(gate->waitEntered());
    CHECK(agent.activeTurnId(session) == started.value());
    CHECK(agent.submit(MaiSendPrompt{session, "ordinary send remains busy"}).error().code() ==
          MaiErrorCode::Busy);

    std::atomic<bool> persistedBeforeBroadcast{false};
    auto subscription = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type != MaiEventType::MessageUpdated) return;
        for (const MaiMessage& message : agent.listMessages(session)) {
            if (message.id == event.messageId && message.text() == "Show the current result first")
                persistedBeforeBroadcast = true;
        }
    });
    const MaiSteerPrompt supplement{session, started.value(), "request-one",
                                    "Show the current result first"};
    const auto accepted = agent.submit(supplement);
    CHECK(accepted.isOk());
    CHECK(persistedBeforeBroadcast.load());
    CHECK(agent.submit(supplement).value() == accepted.value());
    MaiSteerPrompt conflicting = supplement;
    conflicting.text = "different content";
    CHECK(agent.submit(conflicting).error().code() == MaiErrorCode::InvalidInput);
    CHECK(observed->requestCount() == 1);
    CHECK(!gate->canceled.load());

    const auto second = agent.submit(
        MaiSteerPrompt{session, started.value(), "request-two", "Keep the original dimensions"});
    CHECK(second.isOk());
    gate->release();
    agent.waitIdle();
    CHECK(!gate->canceled.load());
    CHECK(gate->calls.load() == 1);
    CHECK(observed->requestCount() == 3);
    if (observed->requestCount() < 3) return;

    const auto request = observed->request(1);
    CHECK(request.messages.size() == 6);
    if (request.messages.size() == 6) {
        CHECK(request.messages[1].role == MaiModelRole::Assistant);
        CHECK(request.messages[1].invocations.size() == 2);
        CHECK(request.messages[2].role == MaiModelRole::ToolResult);
        CHECK(request.messages[2].toolCallId == "held_1");
        CHECK(request.messages[3].role == MaiModelRole::ToolResult);
        CHECK(request.messages[3].toolCallId == "status_2");
        CHECK(request.messages[4].role == MaiModelRole::User);
        CHECK(request.messages[4].content == supplement.text);
        CHECK(request.messages[5].content == "Keep the original dimensions");
    }
    const auto last = observed->request(2);
    CHECK(last.messages.size() == 8);
    if (last.messages.size() == 8) {
        CHECK(last.messages[5].role == MaiModelRole::User);
        CHECK(last.messages[6].role == MaiModelRole::Assistant);
        CHECK(last.messages[6].invocations.front().id == "after_steer");
        CHECK(last.messages[7].role == MaiModelRole::ToolResult);
    }
    const auto history = agent.listMessages(session);
    CHECK(history.size() == 5);
    if (history.size() == 5) {
        CHECK(history[1].id == started.value() && history[1].completed > 0);
        CHECK(history[2].id == accepted.value());
        CHECK(history[3].id == second.value());
        CHECK(history[4].role == MaiRole::Assistant && history[4].completed > 0);
        CHECK(history[4].text() == "Here is the current result.");
    }
    CHECK(agent.activeTurnId(session).empty());
    CHECK(agent.submit(supplement).value() == accepted.value());
    CHECK(agent.submit(MaiSteerPrompt{session, started.value(), "idle-request", "Too late"})
              .error()
              .code() == MaiErrorCode::NotFound);
    const auto newTurn = agent.submit(MaiSendPrompt{session, "Start a different task"});
    CHECK(newTurn.isOk());
    CHECK(agent.activeTurnId(session) == newTurn.value());
    CHECK(agent.submit(supplement).value() == accepted.value());
    CHECK(agent.submit(MaiSteerPrompt{session, started.value(), "stale-turn", "Wrong task"})
              .error()
              .code() == MaiErrorCode::InvalidInput);
    agent.waitIdle();
    agent.eventBus().unsubscribe(subscription);
}

void test_steer_rejects_empty_wrong_turn_and_stopped_work_without_restarting() {
    Workspace workspace;
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("held_work", "{}"), sayTurn("must not be requested")});
    MaiFakeModelClient* observed = model.get();
    auto registry = std::make_unique<MaiToolRegistry>();
    auto held = std::make_unique<SteeringHeldTool>();
    SteeringHeldTool* gate = held.get();
    registry->add(std::move(held));
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(registry));
    const std::string session =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    const std::string other = agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    const auto started = agent.submit(MaiSendPrompt{session, "Start"});
    CHECK(gate->waitEntered());
    CHECK(agent.submit(MaiSteerPrompt{session, started.value(), "empty", " \n\t"}).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(agent.submit(MaiSteerPrompt{session, "old-turn", "wrong-turn", "New instructions"})
              .error()
              .code() == MaiErrorCode::InvalidInput);
    CHECK(agent.submit(MaiSteerPrompt{other, started.value(), "other-session", "New instructions"})
              .error()
              .code() == MaiErrorCode::NotFound);
    const MaiSteerPrompt acceptedInput{session, started.value(), "before-stop", "Keep this note"};
    const auto accepted = agent.submit(acceptedInput);
    CHECK(accepted.isOk());
    CHECK(agent.submit(MaiInterrupt{session}).isOk());
    CHECK(agent.submit(MaiSteerPrompt{session, started.value(), "after-stop", "Do not restart"})
              .error()
              .code() == MaiErrorCode::Canceled);
    CHECK(agent.submit(acceptedInput).value() == accepted.value());
    gate->release();
    agent.waitIdle();
    CHECK(gate->canceled.load());
    CHECK(observed->requestCount() == 1);
    CHECK(agent.listMessages(session).size() == 3);
    CHECK(agent.listMessages(other).empty());
}

// 无工具的流即将结束时也必须 drain-or-close，不能成功收下输入后直接退出。
void test_steer_during_final_text_is_consumed_before_turn_completion() {
    class FinalTextGate final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>& cancel) override {
            {
                std::lock_guard<std::mutex> lock(mutex);
                requests.push_back(request);
            }
            sink.onText(requests.size() == 1 ? "Original answer" : "Adjusted answer");
            if (requests.size() == 1) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                wake.notify_all();
                if (!wake.wait_for(lock, std::chrono::seconds(5), [&] { return ready; }))
                    return MaiError::make(MaiErrorCode::Internal, "test gate timed out");
            }
            return cancel.load() ? MaiError::make(MaiErrorCode::Canceled, "canceled") : MaiError{};
        }
        MaiWireApi wireApi() const override {
            return MaiWireApi::ChatCompletions;
        }
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false;
        bool ready = false;
        std::vector<MaiModelRequest> requests;
    };
    Workspace workspace;
    // 1 次预算额外覆盖：已经接受的补充也不能在最后一次普通请求后被吞掉。
    auto model = std::make_unique<FinalTextGate>();
    FinalTextGate* gate = model.get();
    MaiAgent::Options options;
    options.maxToolIterations = 1;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), nullptr, options);
    const std::string session =
        agent.submit(MaiCreateSession{workspace.utf8Root(), "", ""}).value();
    const auto started = agent.submit(MaiSendPrompt{session, "Start"});
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        CHECK(gate->wake.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }));
    }
    CHECK(agent.submit(MaiSteerPrompt{session, started.value(), "stream-steer", "Adjust it now"})
              .isOk());
    std::atomic<bool> closedAtIdle{false};
    auto subscription = agent.eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::SessionIdle)
            closedAtIdle =
                !agent.submit(MaiSteerPrompt{session, started.value(), "at-idle", "Late"});
    });
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->ready = true;
        gate->wake.notify_all();
    }
    agent.waitIdle();
    CHECK(closedAtIdle.load());
    CHECK(gate->requests.size() == 2);
    if (gate->requests.size() == 2) {
        CHECK(gate->requests[1].messages.size() == 4);
        CHECK(gate->requests[1].messages[2].role == MaiModelRole::User);
        CHECK(gate->requests[1].messages[2].content == "Adjust it now");
        CHECK(gate->requests[1].tools.empty());
    }
    CHECK(agent.listMessages(session).back().text() == "Adjusted answer");
    agent.eventBus().unsubscribe(subscription);
}

void test_specialist_reply_keeps_existing_history_and_new_steering_in_order() {
    Workspace workspace;
    auto store = makeMaiMemoryStore();
    MaiSessionStore* observedStore = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = workspace.utf8Root();
    session.created = MaiTime::getCurrentTime();
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = "synthetic_specialist";
    task.intent = "Review the completed result";
    task.created = session.created;
    CHECK(!store->insertSpecialistTask(task));
    CHECK(!store->finishSpecialistTask(task.id, session.id, MaiSpecialistTaskStatus::Succeeded,
                                       "Synthetic specialist result", "", task.created + 1));
    MaiMessage earlierNotice;
    earlierNotice.id = MaiIdGenerator::newMessageId();
    earlierNotice.role = MaiRole::Assistant;
    earlierNotice.created = session.created;
    earlierNotice.completed = session.created;
    CHECK(store->reserveSpecialistNotification(task.id, session.id, earlierNotice.id, 1));
    store->putMessage(session.id, earlierNotice);
    MaiMessage laterUser;
    laterUser.id = MaiIdGenerator::newMessageId();
    laterUser.role = MaiRole::User;
    laterUser.created = session.created;
    laterUser.completed = session.created;
    MaiMessagePart instruction;
    instruction.id = MaiIdGenerator::newPartId();
    instruction.created = session.created;
    instruction.body = MaiTextPart{"Existing later user history"};
    laterUser.parts.push_back(std::move(instruction));
    store->putMessage(session.id, laterUser);
    auto model = std::make_unique<MaiFakeModelClient>(std::vector<MaiFakeModelClient::Turn>{
        callTurn("held_work", "{}", "specialist-tool"), sayTurn("Delivered the current result.")});
    MaiFakeModelClient* observed = model.get();
    auto tools = std::make_unique<MaiToolRegistry>();
    auto held = std::make_unique<SteeringHeldTool>();
    SteeringHeldTool* gate = held.get();
    tools->add(std::move(held));
    MaiAgent agent(std::move(store), std::move(model), std::move(tools));
    CHECK(gate->waitEntered());
    CHECK(agent.activeTurnId(session.id) == earlierNotice.id);
    CHECK(agent.submit(
        MaiSteerPrompt{session.id, earlierNotice.id, "specialist-steer", "Present it now"}));
    gate->release();
    agent.waitIdle();
    CHECK(!gate->canceled.load());
    CHECK(observed->requestCount() == 2);
    if (observed->requestCount() == 2) {
        const auto first = observed->request(0);
        CHECK(first.messages.size() == 3);
        if (first.messages.size() == 3) {
            CHECK(first.messages[0].content == "Existing later user history");
            CHECK(first.messages[2].content.find("Synthetic specialist result") !=
                  std::string::npos);
        }
        const auto next = observed->request(1);
        CHECK(next.messages.size() == 6);
        if (next.messages.size() == 6) {
            CHECK(next.messages[0].content == "Existing later user history");
            CHECK(next.messages[2].content.find("Synthetic specialist result") !=
                  std::string::npos);
            CHECK(next.messages[3].role == MaiModelRole::Assistant);
            CHECK(next.messages[4].toolCallId == "specialist-tool");
            CHECK(next.messages[5].content == "Present it now");
        }
    }
    MaiSpecialistTask delivered;
    CHECK(observedStore->getSpecialistTask(task.id, session.id, delivered));
    CHECK(delivered.notifiedAt > 0);
    CHECK(delivered.notificationMessageId == earlierNotice.id);
    CHECK(observedStore->listUnnotifiedSpecialistTasks(10).empty());
}

}  // namespace

int main() {
    test_tool_loop_closes();
    test_screenshot_stop_without_workspace_keeps_file_tools_blocked();
    test_balance_error_routes_back_to_main_model();
    test_tool_image_is_persisted_and_fed_back_to_the_model();
    test_tool_image_keeps_parallel_tool_calls_in_one_assistant_batch();
    test_reasoning_only_after_tools_retries_for_final_answer();
    test_repeated_missing_final_answer_reports_error();
    test_tool_error_is_fed_back();
    test_unknown_tool_does_not_kill_the_turn();
    test_internal_tool_is_not_visible_or_callable_by_model();
    test_local_primary_blocks_vision_tool_but_keeps_user_display();
    test_path_escape_through_model();
    test_iteration_cap();
    test_iteration_cap_provides_a_final_status();
    test_distinct_tool_calls_continue_past_twelve_rounds();
    test_repeated_tool_results_stop_before_hard_limit();
    test_no_tools_means_no_tool_field();
    test_model_exception_becomes_a_session_error();
    test_steer_waits_for_all_tool_results_and_reaches_the_next_model_request();
    test_steer_rejects_empty_wrong_turn_and_stopped_work_without_restarting();
    test_steer_during_final_text_is_consumed_before_turn_completion();
    test_specialist_reply_keeps_existing_history_and_new_steering_in_order();
    if (failures == 0) std::printf("tool loop tests passed\n");
    return failures == 0 ? 0 : 1;
}
