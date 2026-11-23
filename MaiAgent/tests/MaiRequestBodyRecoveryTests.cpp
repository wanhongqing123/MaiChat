// 文件作用：在本机 HTTP 413 和真实 Agent 检查点之间验证恢复，不请求外部模型。
#include "MaiAgent.h"
#include "MaiContextBuilder.h"
#include "MaiContextCompactor.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiOpenAiClient.h"
#include "MaiRoutedModelClient.h"
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
        if (requestBodyBytes(request).value() > 16000) {
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

int main() {
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
        CHECK(stuck->calls == 1);
        const auto kept = protectedAgent.listMessages(id);
        CHECK(kept.size() == 2);
        bool preserved = false;
        for (const auto& m : kept)
            if (m.role == MaiRole::User && m.text() == original) preserved = true;
        CHECK(preserved);
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
    std::printf("body recovery calls=%d recovered=%d imagesPreserved=%zu failures=%d\n",
                observed->calls, observed->recovered, images, failures);
    return failures ? 1 : 0;
}
