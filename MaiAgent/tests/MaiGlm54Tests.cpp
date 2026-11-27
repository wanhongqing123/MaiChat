// 文件作用：用正式存储及回环 HTTP 验证旗舰替换、视觉输入和历史来源隔离。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <httplib.h>
#include <json.hpp>

#include "MaiAgent.h"
#include "MaiCloudModelCatalog.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiModelContextUsage.h"
#include "MaiMemoryStore.h"
#include "MaiResponsesClient.h"
#include "MaiRoutedModelClient.h"
#include "MaiSessionRecordCodec.h"
#include "MaiSqliteStore.h"
#include "MaiThread.h"
#include "MaiViewImageTool.h"

namespace {
using Json = nlohmann::json;
int failures = 0;
#define CHECK(value)                                       \
    do {                                                   \
        if (!(value)) {                                    \
            std::printf("FAIL %d %s\n", __LINE__, #value); \
            ++failures;                                    \
        }                                                  \
    } while (false)

void test_catalog() {
    const auto flagship = maiCloudModelInfo("glm-5.4");
    CHECK(flagship && flagship->provider == "glm" && flagship->wire == MaiWireApi::Responses);
    CHECK(flagship && flagship->supportsImages && flagship->supportsReasoningEffort);
    CHECK(flagship && flagship->contextLimits.contextWindowTokens == 1048576);
    CHECK(flagship && flagship->contextLimits.maxOutputTokens == 131072);
    const auto limits = maiOfficialModelContextLimits("https://open.bigmodel.cn/api/v1", "glm-5.4");
    CHECK(limits.contextWindowTokens == 1048576 && limits.maxOutputTokens == 131072);
    CHECK(!maiCloudModelInfo("glm-5.3"));
    CHECK(maiCanonicalCloudModel("glm-5.3") == "glm-5.4");
    CHECK(maiCanonicalCloudModel("glm-5.3-flash") == "glm-5.3-flash");
    CHECK(maiCanonicalCloudModel("deepseek-v4-pro") == "deepseek-v4-pro");
    CHECK(maiCanonicalCloudModel("").empty());
    const auto flash = maiCloudModelInfo("glm-5.3-flash");
    CHECK(flash && flash->supportsImages && flash->model == "glm-5.3-flash");
    CHECK(MaiAgent::Options{}.defaultModel == "glm-5.4");
    CHECK(!makeMaiViewImageTool()->isAvailableForModel("deepseek-v4-pro"));
    CHECK(makeMaiViewImageTool()->isAvailableForModel("glm-5.4"));
}

// 归档型号的元数据升级不依赖当前账号启用哪家供应商，不能阻止其他模型及历史读取。
void test_archived_flagship_without_current_glm_route() {
    auto store = makeMaiMemoryStore();
    auto* persisted = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.title = "Archived GLM session";
    session.model = "glm-5.3";
    store->putSession(session);
    std::atomic<int> credentialReads{0};
    MaiModelRoute route;
    route.modelNames = {"deepseek-flash"};
    route.config.baseUrl = "http://127.0.0.1:1/v1";
    route.apiKeyProvider = [&] {
        ++credentialReads;
        return std::string{};
    };
    MaiAgent::Options options;
    options.defaultModel = "deepseek-flash";
    bool opened = false;
    try {
        MaiAgent agent(std::move(store), makeMaiRoutedModelClient({{route}}), nullptr, options);
        opened = true;
        MaiSession restored;
        CHECK(agent.getSession(session.id, restored) && restored.model == "glm-5.4");
        CHECK(persisted->getSession(session.id, restored) && restored.model == "glm-5.4");
        // 正常用户选择仍须存在可用路由，不能为元数据恢复放宽这个契约。
        CHECK(!agent.submit(MaiUpdateSession{session.id, "", "glm-5.4"}));
    } catch (const std::runtime_error&) {
    }
    CHECK(opened);
    CHECK(credentialReads == 0);
}

void test_migrated_session_and_image_wire() {
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-glm54-" + MaiIdGenerator::newSessionId()));
    CHECK(!MaiFileSystem::createDirectories(root));
    const auto index = root.append(MaiFilePath::fromUtf8("agent.db"));
    const auto image = root.append(MaiFilePath::fromUtf8("image.png"));
    CHECK(!MaiFileSystem::writeFile(image, std::string("\x89PNG\r\n\x1a\n", 8)));
    const std::string id = MaiIdGenerator::newSessionId();
    const std::string flashId = MaiIdGenerator::newSessionId();
    MaiMessage historical;
    historical.id = MaiIdGenerator::newMessageId();
    historical.role = MaiRole::Assistant;
    historical.created = 1;
    MaiMessagePart part;
    part.id = MaiIdGenerator::newPartId();
    part.created = 1;
    part.body = MaiReasoningPart{
        "Historical thought",
        MaiModelContinuationState{
            "responses.output.v1", "glm", "glm-5.3",
            R"([{"type":"reasoning","id":"old_reasoning","encrypted_content":"synthetic-old-state"}])"}};
    historical.parts.push_back(part);
    const auto original = MaiSessionRecordCodec::encodeMessage(historical);
    CHECK(original);
    {
        auto opened = makeMaiSqliteStore(index.toUtf8());
        CHECK(opened);
        if (!opened) return;
        MaiSession session;
        session.id = id;
        session.directory = root.toUtf8();
        session.title = "Legacy flagship session";
        session.model = "glm-5.3";
        session.reasoningEffort = "low";
        session.created = 1;
        session.updated = 1;
        opened.value()->putSession(session);
        session.id = flashId;
        session.model = "glm-5.3-flash";
        opened.value()->putSession(session);
        opened.value()->putMessage(id, historical);
        MaiModelContextUsage usage;
        usage.model = "glm-5.3";
        usage.provider = "old-provider";
        usage.windowTokens = 1048576;
        usage.windowSource = MaiContextWindowSource::Configuration;
        usage.samplingValid = true;
        usage.thread.total = {42, MaiUsageValueSource::Provider};
        const auto encoded = MaiSessionRecordCodec::encodeContextUsage(usage);
        CHECK(encoded);
        if (encoded)
            CHECK(!opened.value()->appendSessionRecord(
                id, "token_usage_record",
                Json{{"context_usage", Json::parse(encoded.value())}}.dump()));
        CHECK(!opened.value()->lastWriteError());
    }

    httplib::Server server;
    Json sent;
    std::mutex mutex;
    server.Post("/v1/responses", [&](const httplib::Request& request, httplib::Response& response) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent = Json::parse(request.body);
        }
        response.set_content(
            "data: "
            "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[]}"
            "}\n\n",
            "text/event-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) return;
    std::thread worker([&] {
        MaiThread::setCurrentName("mai-glm54-wire");
        server.listen_after_bind();
    });
    for (int i = 0; i < 200 && !server.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(server.is_running());
    {
        auto opened = makeMaiSqliteStore(index.toUtf8());
        CHECK(opened);
        if (opened) {
            auto* store = opened.value().get();
            MaiModelConfig config;
            config.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
            config.maxRetries = 0;
            config.totalTimeoutSeconds = 5;
            MaiAgent agent(std::move(opened.value()), makeMaiModelClient(config), nullptr);
            MaiSession session;
            CHECK(agent.getSession(id, session));
            CHECK(session.model == "glm-5.4" && session.reasoningEffort == "low");
            CHECK(agent.getSession(flashId, session) && session.model == "glm-5.3-flash");
            MaiMessage restored;
            CHECK(agent.getMessage(id, historical.id, restored));
            const auto encoded = MaiSessionRecordCodec::encodeMessage(restored);
            CHECK(encoded && original && encoded.value() == original.value());
            const auto usage = maiReadModelContextUsage(*store, id, "glm-5.4", "old-provider");
            CHECK(usage && !usage.value().samplingValid && usage.value().thread.total.tokens == 42);
            CHECK(agent.submit(
                MaiSendPrompt{id, "Inspect this image", {{image.toUtf8(), "image/png"}}}));
            agent.waitIdle();
            std::lock_guard<std::mutex> lock(mutex);
            CHECK(sent.is_object() && sent.value("model", "") == "glm-5.4");
            CHECK(sent.dump().find("input_image") != std::string::npos);
            CHECK(sent.dump().find("data:image/png;base64,") != std::string::npos);
            CHECK(sent.dump().find("synthetic-old-state") == std::string::npos);
            CHECK(sent.dump().find("old_reasoning") == std::string::npos);
            CHECK(agent.getMessage(id, historical.id, restored));
            const auto after = MaiSessionRecordCodec::encodeMessage(restored);
            CHECK(after && original && after.value() == original.value());
            MaiToolContext context;
            context.root = root.toUtf8();
            context.model = "glm-5.4";
            const auto viewed = makeMaiViewImageTool()->execute(R"({"path":"image.png"})", context);
            CHECK(!viewed.hasError() && viewed.images().size() == 1);
        }
    }
    server.stop();
    worker.join();
    const auto temporary = MaiFileSystem::resolve(MaiFileSystem::temporaryDirectory());
    const auto resolved = MaiFileSystem::resolve(root);
    if (!failures && temporary.isParentOf(resolved)) MaiFileSystem::removeRecursively(resolved);
}
}  // namespace

int main() {
    test_catalog();
    test_archived_flagship_without_current_glm_route();
    test_migrated_session_and_image_wire();
    std::printf("GLM 5.4 checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
