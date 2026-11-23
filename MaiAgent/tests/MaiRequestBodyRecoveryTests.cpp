// 文件作用：在本机 HTTP 413 和真实 Agent 检查点之间验证恢复，不请求外部模型。
#include "MaiAgent.h"
#include "MaiContextBuilder.h"
#include "MaiContextCompactor.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiOpenAiClient.h"
#include "MaiRoutedModelClient.h"
#include "MaiRequestBodyFitter.h"
#include "MaiSessionTools.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <httplib.h>
#include <json.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(x)                                        \
    do {                                                \
        if (!(x)) {                                     \
            std::printf("FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                 \
        }                                               \
    } while (0)

class BodyModel final : public MaiModelClient {
public:
    MaiError rejection;
    std::size_t limit = 120000;
    int calls = 0;
    bool recovered = false;
    bool rejectAlways = false;
    std::vector<std::size_t> sizes;
    std::vector<std::string> lastImagePaths;
    MaiResult<std::size_t> requestBodyBytes(const MaiModelRequest& request) override {
        std::size_t bytes = 1000 + request.baseInstructions.size();
        for (const auto& m : request.messages) bytes += m.content.size() + m.images.size() * 10000;
        return bytes;
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        ++calls;
        const auto bytes = requestBodyBytes(request).value();
        sizes.push_back(bytes);
        lastImagePaths.clear();
        for (const auto& message : request.messages)
            for (const auto& image : message.images) lastImagePaths.push_back(image.path);
        if (rejectAlways || bytes > limit) return rejection;
        recovered = true;
        if (sink.onText) sink.onText("Recovered original conversation");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {1000000, 128000};
    }
};

class SummaryBodyModel final : public MaiModelClient {
public:
    MaiError rejection;
    int rejected = 0, completed = 0;
    bool rejectAll = false;
    std::size_t outputLimit = 0;
    MaiResult<std::size_t> requestBodyBytes(const MaiModelRequest& request) override {
        std::size_t bytes = 1000 + request.baseInstructions.size();
        for (const auto& m : request.messages) bytes += m.content.size();
        return bytes;
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (outputLimit == 0) outputLimit = request.maxOutputTokens;
        CHECK(request.maxOutputTokens == outputLimit);
        if (rejectAll || requestBodyBytes(request).value() > 16000) {
            ++rejected;
            return rejection;
        }
        ++completed;
        if (sink.onText)
            sink.onText(
                R"({"goals":["Retain the original task"],"constraints":[],"decisions":[],"progress":["Preserved older conclusions"],"open_questions":[],"source_notes":[]})");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    MaiModelContextLimits contextLimits(const std::string&) const override {
        return {32768, 4096};
    }
};

// 测量口径与 BodyModel 相同：图片各 10000 字节，正文/参数按原字节。
class MeasureModel final : public MaiModelClient {
public:
    bool unmeasurable = false;
    int measurements = 0;
    MaiResult<std::size_t> requestBodyBytes(const MaiModelRequest& request) override {
        ++measurements;
        if (unmeasurable) return std::size_t{0};
        return bytesOf(request);
    }
    static std::size_t bytesOf(const MaiModelRequest& request) {
        std::size_t bytes = 1000 + request.baseInstructions.size();
        for (const auto& m : request.messages) {
            bytes += m.content.size() + m.reasoning.size() + m.images.size() * 10000;
            for (const auto& call : m.invocations) bytes += call.arguments.size();
            if (m.continuation) bytes += m.continuation->payload.size();
        }
        return bytes;
    }
    MaiError stream(const MaiModelRequest&, const MaiStreamSink&,
                    const std::atomic<bool>&) override {
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

static MaiModelMessage textMessage(MaiModelRole role, std::string content) {
    MaiModelMessage message;
    message.role = role;
    message.content = std::move(content);
    return message;
}

static MaiModelMessage observation(const std::string& path) {
    auto message = textMessage(
        MaiModelRole::User,
        nlohmann::json{{"type", "tool_group_image_observation"}, {"workspace_path", path}}.dump());
    message.images.push_back({path, "image/png"});
    return message;
}

// 旧历史图 → 当前轮较早截图 → 文本截断 → 最新截图/当前附件，结构始终保持合法。
static MaiModelRequest fitterFixture() {
    MaiModelRequest request;
    request.model = "fit-model";
    auto old = textMessage(MaiModelRole::User, "Old requirement");
    old.images.push_back({"old.png", "image/png"});
    request.messages.push_back(old);
    auto oldAnswer = textMessage(MaiModelRole::Assistant, std::string(6000, 'a'));
    oldAnswer.continuation = MaiModelContinuationState{"responses.output.v1", "p", "m",
                                                       std::string(8000, 'c')};
    request.messages.push_back(oldAnswer);
    auto current = textMessage(MaiModelRole::User, "Current instruction");
    current.images.push_back({"current.png", "image/png"});
    request.messages.push_back(current);
    auto call = textMessage(MaiModelRole::Assistant, "");
    call.invocations.push_back(
        {"call_1", "read", nlohmann::json{{"data", std::string(5000, 'x')}}.dump()});
    request.messages.push_back(call);
    auto result = textMessage(MaiModelRole::ToolResult, std::string(20000, 'r'));
    result.toolCallId = "call_1";
    result.sourceMessageId = "msg_tool";
    result.sourcePartId = "prt_tool";
    request.messages.push_back(result);
    request.messages.push_back(observation("turn-early.png"));
    auto second = textMessage(MaiModelRole::Assistant, "");
    second.invocations.push_back({"call_2", "screenshot", "{}"});
    request.messages.push_back(second);
    auto secondResult = textMessage(MaiModelRole::ToolResult, "ok");
    secondResult.toolCallId = "call_2";
    request.messages.push_back(secondResult);
    request.messages.push_back(observation("turn-newest.png"));
    return request;
}

static std::vector<std::string> imagePaths(const MaiModelRequest& request) {
    std::vector<std::string> paths;
    for (const auto& m : request.messages)
        for (const auto& image : m.images) paths.push_back(image.path);
    return paths;
}

static void testFitterOrderAndStructure() {
    MeasureModel model;
    const auto original = fitterFixture();
    const auto full = MeasureModel::bytesOf(original);
    std::atomic<bool> cancel{false};

    // 只差一点：只省略最旧的历史图，其它图片与正文原样。
    auto fitted = maiFitRequestBody(model, original, full - 1, cancel);
    CHECK(fitted);
    if (fitted) {
        const auto& value = fitted.value();
        CHECK(value.fits && value.reduced && !value.exhausted);
        CHECK(value.omittedImages == 1);
        CHECK(value.truncatedTexts == 0);
        CHECK(imagePaths(value.request) ==
              (std::vector<std::string>{"current.png", "turn-early.png", "turn-newest.png"}));
        CHECK(value.request.messages[0].content.find("workspace_path=\"old.png\"") !=
              std::string::npos);
        CHECK(value.request.messages[0].content.find("view_image") != std::string::npos);
    }

    // 收紧到需要截断文本：当前附件与最新截图仍保留。
    fitted = maiFitRequestBody(model, original, full - 30000, cancel);
    CHECK(fitted);
    if (fitted) {
        const auto& value = fitted.value();
        CHECK(value.fits);
        CHECK(value.droppedContinuations == 1);
        CHECK(value.truncatedTexts > 0);
        CHECK(imagePaths(value.request) ==
              (std::vector<std::string>{"current.png", "turn-newest.png"}));
        CHECK(value.request.messages[4].content.find("source_message_id=msg_tool") !=
              std::string::npos);
        CHECK(value.request.messages[4].content.find("tool_output_read") != std::string::npos);
        // 当前用户正文在最后一级之前不截断。
        CHECK(value.request.messages[2].content.rfind("Current instruction", 0) == 0);
    }

    // 极小上限：到达最小请求，但结构合法、调用/结果配对与消息数不变。
    fitted = maiFitRequestBody(model, original, 1500, cancel);
    CHECK(fitted);
    if (fitted) {
        const auto& value = fitted.value();
        CHECK(!value.fits && value.exhausted);
        CHECK(imagePaths(value.request).empty());
        CHECK(value.request.messages.size() == original.messages.size());
        for (std::size_t i = 0; i < original.messages.size(); ++i) {
            CHECK(value.request.messages[i].role == original.messages[i].role);
            CHECK(value.request.messages[i].toolCallId == original.messages[i].toolCallId);
            CHECK(value.request.messages[i].invocations.size() ==
                  original.messages[i].invocations.size());
            for (const auto& call : value.request.messages[i].invocations)
                CHECK(nlohmann::json::parse(call.arguments, nullptr, false).is_object());
        }
        CHECK(value.fittedBytes < full);
    }
    // 输入请求不被改写。
    CHECK(MeasureModel::bytesOf(original) == full);
    CHECK(imagePaths(original).size() == 4);

    // 测量次数是对数级，不在内层循环里反复完整编码。
    model.measurements = 0;
    CHECK(maiFitRequestBody(model, original, full - 30000, cancel));
    CHECK(model.measurements < 60);
    std::printf("fitter measurements=%d\n", model.measurements);
}

static void testFitterUnmeasurableAndCancel() {
    MeasureModel model;
    model.unmeasurable = true;
    const auto original = fitterFixture();
    bool measured = true;
    const auto approx = maiMeasureRequestBody(model, original, &measured);
    CHECK(!measured);
    CHECK(approx > MeasureModel::bytesOf(original));  // 近似必须保守
    std::atomic<bool> cancel{false};
    const auto fitted = maiFitRequestBody(model, original, approx / 2, cancel);
    CHECK(fitted);
    if (fitted) {
        CHECK(!fitted.value().measured);
        CHECK(fitted.value().reduced);
        CHECK(fitted.value().fittedBytes <= approx / 2 || fitted.value().exhausted);
    }
    std::atomic<bool> canceled{true};
    model.unmeasurable = false;
    const auto stopped =
        maiFitRequestBody(model, original, MeasureModel::bytesOf(original) / 2, canceled);
    CHECK(!stopped && stopped.error().code() == MaiErrorCode::Canceled);
}

static void testLearnedLimits() {
    maiResetLearnedRequestBodyLimits();
    CHECK(maiLearnedRequestBodyLimit("m") == 0);
    CHECK(maiParseStatedRequestBodyLimit(
              "HTTP 413: The provider rejected the request body (60000000 bytes). Request body "
              "too large. Maximum size is 52428800 bytes") == 52428800);
    CHECK(maiParseStatedRequestBodyLimit(
              "HTTP 413: The provider rejected the request body (60000000 bytes). Too large") ==
          0);
    CHECK(maiParseStatedRequestBodyLimit("limit: 10485760 bytes") == 10485760);
    // 明确上限直接采用并留 2% 余量。
    const auto stated = maiLearnRequestBodyRejection(
        "m", 60000000, "Request body too large. Maximum size is 52428800 bytes", 85);
    CHECK(stated == 52428800 - 52428800 / 50);
    // 无明确上限：按比例收紧，且只会变小。
    CHECK(maiLearnRequestBodyRejection("m", 40000000, {}, 85) == 34000000);
    CHECK(maiLearnRequestBodyRejection("m", 50000000, {}, 85) == 34000000);
    // 声称的上限不小于被拒字节时不可信，回退比例收紧。
    CHECK(maiLearnRequestBodyRejection("n", 1000, "maximum 5000 bytes", 60) == 600);
    // 极小值也严格变小，不会原地打转。
    CHECK(maiLearnRequestBodyRejection("t", 2, {}, 99) == 1);
    maiResetLearnedRequestBodyLimits();
    CHECK(maiLearnedRequestBodyLimit("m") == 0);
}

int main() {
    testFitterOrderAndStructure();
    testFitterUnmeasurableAndCancel();
    testLearnedLimits();
    httplib::Server server;
    std::atomic<std::size_t> transmitted{0};
    server.Post("/chat/completions", [&](const httplib::Request& q, httplib::Response& r) {
        transmitted = q.body.size();
        r.status = 413;
        r.set_content(
            R"({"error":{"code":"request_too_large","message":"Request body too large. Maximum size is 52428800 bytes"}})",
            "application/json");
    });
    server.Post("/responses", [&](const httplib::Request& q, httplib::Response& r) {
        transmitted = q.body.size();
        r.status = 413;
        r.set_content(
            R"({"error":{"code":"request_too_large","message":"Request body too large"}})",
            "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) return 1;
    std::thread worker([&] { server.listen_after_bind(); });
    for (int i = 0; i < 1000 && !server.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MaiError rejection;
    for (const auto wire : {MaiWireApi::ChatCompletions, MaiWireApi::Responses}) {
        MaiModelConfig config;
        config.baseUrl = "http://127.0.0.1:" + std::to_string(port);
        config.apiKey = "owned-local-key";
        config.wire = wire;
        config.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
            proxy = {};
            return true;
        };
        auto client = makeMaiModelClient(config);
        MaiModelRequest request;
        request.model = "owned-test";
        MaiModelMessage input;
        input.role = MaiModelRole::User;
        input.content = "Owned request";
        request.messages.push_back(input);
        std::atomic<bool> cancel{false};
        const auto error = client->stream(request, {}, cancel);
        CHECK(std::string(maiErrorCodeToString(error.code())) == "request_body_too_large");
        CHECK(!error.retryAdvice());
        rejection = error;
        const auto measured = client->requestBodyBytes(request);
        CHECK(measured && measured.value() == transmitted.load());
    }
    {
        MaiRoutedModelConfig config;
        MaiModelRoute route;
        route.modelNames = {"owned-test"};
        route.config.baseUrl = "http://127.0.0.1:" + std::to_string(port);
        route.config.wire = MaiWireApi::ChatCompletions;
        route.config.proxyResolver = [](const std::string&, MaiModelProxy& proxy) {
            proxy = {};
            return true;
        };
        std::atomic<int> credentials{0};
        route.apiKeyProvider = [&] {
            ++credentials;
            return std::string("owned-local-key");
        };
        config.routes.push_back(route);
        auto client = makeMaiRoutedModelClient(std::move(config));
        MaiModelRequest request;
        request.model = "owned-test";
        MaiModelMessage input;
        input.role = MaiModelRole::User;
        input.content = "Owned routed request";
        request.messages.push_back(input);
        std::atomic<bool> cancel{false};
        CHECK(client->stream(request, {}, cancel));
        const auto measured = client->requestBodyBytes(request);
        CHECK(measured && measured.value() == transmitted.load());
        CHECK(credentials.load() == 1);
    }
    server.stop();
    worker.join();
    auto store = makeMaiMemoryStore();
    auto* rawStore = store.get();
    auto model = std::make_unique<BodyModel>();
    auto* observed = model.get();
    observed->rejection = rejection;
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiSessionTools(*tools, false);
    MaiAgent::Options options;
    options.defaultModel = "owned-test";
    // 已学到的上限是进程内全局状态；各场景之间清空，互不影响。
    maiResetLearnedRequestBodyLimits();
    MaiAgent agent(std::move(store), std::move(model), std::move(tools), options);
    const auto session = agent.submit(MaiCreateSession{"", "Size recovery", ""}).value();
    for (int i = 0; i < 20; ++i) {
        MaiMessage user;
        user.id = MaiIdGenerator::newMessageId();
        user.role = MaiRole::User;
        user.created = user.completed = 100 + i * 2;
        user.parts.push_back({"prt_text_" + std::to_string(i),
                              MaiTextPart{"Preserve user requirement " + std::to_string(i)},
                              user.created});
        user.parts.push_back(
            {"prt_image_" + std::to_string(i),
             MaiImagePart{"owned-image-" + std::to_string(i) + ".png", "image/png"}, user.created});
        rawStore->putMessage(session, user);
        MaiMessage assistant;
        assistant.id = MaiIdGenerator::newMessageId();
        assistant.role = MaiRole::Assistant;
        assistant.created = assistant.completed = 101 + i * 2;
        assistant.parts.push_back({"prt_answer_" + std::to_string(i),
                                   MaiTextPart{"Preserve confirmed result " + std::to_string(i)},
                                   assistant.created});
        rawStore->putMessage(session, assistant);
    }
    CHECK(agent.submit(MaiSendPrompt{session, "Continue the same task"}));
    agent.waitIdle();
    CHECK(observed->recovered);
    CHECK(observed->calls >= 2 && observed->calls <= 5);
    for (std::size_t i = 1; i < observed->sizes.size(); ++i)
        CHECK(observed->sizes[i] < observed->sizes[i - 1]);
    const auto history = agent.listMessages(session);
    CHECK(history.size() == 42);
    std::size_t images = 0;
    for (const auto& m : history)
        for (const auto& p : m.parts)
            if (std::get_if<MaiImagePart>(&p.body)) ++images;
    CHECK(images == 20);
    {
        // 同一会话的下一轮先按已学到的上限适配：首次发送就不超过该上限，不再先发原始大请求。
        const auto learned = maiLearnedRequestBodyLimit("owned-test");
        CHECK(learned != 0);
        const auto before = observed->sizes.size();
        observed->recovered = false;
        CHECK(agent.submit(MaiSendPrompt{session, "Next step of the same task"}));
        agent.waitIdle();
        CHECK(observed->recovered);
        CHECK(observed->sizes.size() > before);
        if (observed->sizes.size() > before) CHECK(observed->sizes[before] <= learned);
        CHECK(observed->sizes.size() - before <= 3);
        bool failedTurn = false;
        for (const auto& m : agent.listMessages(session))
            if (m.role == MaiRole::Assistant && m.turnOutcome && m.turnOutcome->error)
                failedTurn = true;
        CHECK(!failedTurn);
    }
    maiResetLearnedRequestBodyLimits();
    {
        auto protectedStore = makeMaiMemoryStore();
        auto* raw = protectedStore.get();
        auto nearModel = std::make_unique<BodyModel>();
        auto* candidate = nearModel.get();
        candidate->rejection = rejection;
        candidate->limit = 48000;
        auto registry = std::make_unique<MaiToolRegistry>();
        registerMaiSessionTools(*registry, false);
        MaiAgent nearAgent(std::move(protectedStore), std::move(nearModel), std::move(registry),
                           options);
        const auto id =
            nearAgent.submit(MaiCreateSession{"", "Near-limit protected tail", ""}).value();
        MaiMessage old;
        old.id = MaiIdGenerator::newMessageId();
        old.role = MaiRole::User;
        old.created = old.completed = 1;
        old.parts.push_back({"prt_old", MaiImagePart{"old.png", "image/png"}, 1});
        raw->putMessage(id, old);
        MaiMessage done;
        done.id = MaiIdGenerator::newMessageId();
        done.role = MaiRole::Assistant;
        done.created = done.completed = 2;
        done.parts.push_back({"prt_done", MaiTextPart{"Old conclusion"}, 2});
        raw->putMessage(id, done);
        MaiSendPrompt prompt{id, "Keep the current four images"};
        for (int i = 0; i < 4; ++i)
            prompt.images.push_back({"current-" + std::to_string(i) + ".png", "image/png"});
        CHECK(nearAgent.submit(prompt));
        nearAgent.waitIdle();
        CHECK(candidate->recovered);
        CHECK(candidate->calls == 2);
        CHECK(candidate->lastImagePaths ==
              (std::vector<std::string>{"current-0.png", "current-1.png", "current-2.png",
                                        "current-3.png"}));
    }
    {
        maiResetLearnedRequestBodyLimits();
        auto blocked = std::make_unique<BodyModel>();
        auto* stuck = blocked.get();
        stuck->rejection = rejection;
        stuck->rejectAlways = true;
        auto registry = std::make_unique<MaiToolRegistry>();
        registerMaiSessionTools(*registry, false);
        MaiAgent protectedAgent(makeMaiMemoryStore(), std::move(blocked), std::move(registry),
                                options);
        const auto id =
            protectedAgent.submit(MaiCreateSession{"", "Protected current input", ""}).value();
        const std::string original(30000, 'U');
        CHECK(protectedAgent.submit(MaiSendPrompt{id, original}));
        protectedAgent.waitIdle();
        CHECK(!stuck->recovered);
        // 供应商连最小请求也拒绝：持续缩小到下限（并额外整理一次）后才报错，次数有界。
        CHECK(stuck->calls > 1 && stuck->calls <= 26);
        for (std::size_t i = 1; i < stuck->sizes.size(); ++i)
            CHECK(stuck->sizes[i] <= stuck->sizes[i - 1]);
        CHECK(!stuck->sizes.empty() && stuck->sizes.back() < stuck->sizes.front());
        const auto kept = protectedAgent.listMessages(id);
        CHECK(kept.size() == 2);
        bool preserved = false;
        for (const auto& m : kept)
            if (m.role == MaiRole::User && m.text() == original) preserved = true;
        CHECK(preserved);
        // 错误如实说明原始记录完整保留，且不建议换新会话。
        bool honest = false;
        for (const auto& m : kept)
            if (m.role == MaiRole::Assistant && m.turnOutcome) {
                const auto& text = m.turnOutcome->error.message();
                honest = m.turnOutcome->error.code() == MaiErrorCode::RequestBodyTooLarge &&
                         text.find("fresh conversation") == std::string::npos &&
                         text.find("preserved") != std::string::npos;
            }
        CHECK(honest);
        maiResetLearnedRequestBodyLimits();
    }
    {
        auto summaryStore = makeMaiMemoryStore();
        auto* raw = summaryStore.get();
        auto summaryModel = std::make_unique<SummaryBodyModel>();
        auto* summary = summaryModel.get();
        summary->rejection = rejection;
        MaiAgent ownerAgent(std::move(summaryStore), std::move(summaryModel),
                            std::make_unique<MaiToolRegistry>(), options);
        const auto id = ownerAgent.submit(MaiCreateSession{"", "Summary byte limit", ""}).value();
        for (int i = 0; i < 6; ++i) {
            MaiMessage user;
            user.id = MaiIdGenerator::newMessageId();
            user.role = MaiRole::User;
            user.created = user.completed = 10 + i * 2;
            user.parts.push_back({"prt_user_" + std::to_string(i),
                                  MaiTextPart{"Preserve user requirement"}, user.created});
            raw->putMessage(id, user);
            MaiMessage answer;
            answer.id = MaiIdGenerator::newMessageId();
            answer.role = MaiRole::Assistant;
            answer.created = answer.completed = 11 + i * 2;
            answer.parts.push_back({"prt_answer_" + std::to_string(i),
                                    MaiTextPart{std::string(20000, 'A')}, answer.created});
            raw->putMessage(id, answer);
        }
        const auto history = ownerAgent.listMessages(id);
        MaiContextBuilder builder;
        MaiModelRequest input;
        input.model = "owned-test";
        input.messages = builder.build(history);
        MaiContextCompactor::Options policy;
        policy.force = true;
        std::atomic<bool> cancel{false};
        const auto prepared = MaiContextCompactor(policy).prepare(*raw, *summary, builder, id,
                                                                  history, input, cancel);
        CHECK(prepared);
        if (prepared) CHECK(prepared.value().advancedCheckpoint);
        CHECK(summary->rejected > 0 && summary->rejected <= 4);
        CHECK(summary->completed > 0);
        CHECK(ownerAgent.listMessages(id).size() == history.size());
        std::printf("summary recovery rejected=%d completed=%d outputLimit=%zu\n",
                    summary->rejected, summary->completed, summary->outputLimit);
    }
    {
        // 摘要请求无论多小都被 413 拒绝：强制整理也不能让准备失败，改为延后并保留原请求。
        auto summaryStore = makeMaiMemoryStore();
        auto* raw = summaryStore.get();
        auto summaryModel = std::make_unique<SummaryBodyModel>();
        auto* summary = summaryModel.get();
        summary->rejection = rejection;
        summary->rejectAll = true;
        MaiAgent ownerAgent(std::move(summaryStore), std::move(summaryModel),
                            std::make_unique<MaiToolRegistry>(), options);
        const auto id = ownerAgent.submit(MaiCreateSession{"", "Summary always 413", ""}).value();
        for (int i = 0; i < 6; ++i) {
            MaiMessage user;
            user.id = MaiIdGenerator::newMessageId();
            user.role = MaiRole::User;
            user.created = user.completed = 10 + i * 2;
            user.parts.push_back({"prt_user_" + std::to_string(i),
                                  MaiTextPart{"Preserve user requirement"}, user.created});
            raw->putMessage(id, user);
            MaiMessage answer;
            answer.id = MaiIdGenerator::newMessageId();
            answer.role = MaiRole::Assistant;
            answer.created = answer.completed = 11 + i * 2;
            answer.parts.push_back({"prt_answer_" + std::to_string(i),
                                    MaiTextPart{std::string(20000, 'A')}, answer.created});
            raw->putMessage(id, answer);
        }
        const auto history = ownerAgent.listMessages(id);
        MaiContextBuilder builder;
        MaiModelRequest input;
        input.model = "owned-test";
        input.messages = builder.build(history);
        MaiContextCompactor::Options policy;
        policy.force = true;
        std::atomic<bool> cancel{false};
        const auto prepared = MaiContextCompactor(policy).prepare(*raw, *summary, builder, id,
                                                                  history, input, cancel);
        CHECK(prepared);
        if (prepared) {
            CHECK(prepared.value().deferredCompaction);
            CHECK(!prepared.value().advancedCheckpoint);
            CHECK(!prepared.value().request.messages.empty());
        }
        CHECK(summary->completed == 0);
        CHECK(summary->rejected > 0 && summary->rejected <= 10);
        CHECK(ownerAgent.listMessages(id).size() == history.size());
        if (!prepared)
            std::printf("summary always-413 error=%s: %s\n",
                        maiErrorCodeToString(prepared.error().code()),
                        prepared.error().message().c_str());
        std::printf("summary always-413 rejected=%d deferred=%d\n", summary->rejected,
                    prepared ? int(prepared.value().deferredCompaction) : -1);
    }
    std::printf("body recovery calls=%d recovered=%d imagesPreserved=%zu failures=%d\n",
                observed->calls, observed->recovered, images, failures);
    return failures ? 1 : 0;
}
