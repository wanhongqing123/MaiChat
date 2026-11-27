// 文件作用：真实回环 SSE 经正式联合后端验证已完成 item、重开及删索引重建。
// 只操作独立临时账号根目录；不访问用户数据库、供应商或运行中的应用。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiContextBuilder.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiResponsesClient.h"
#include "MaiModelContextUsage.h"
#include "MaiSqliteStore.h"
#include "MaiThread.h"
#include "MaiTime.h"
#include "MaiTurnRunner.h"

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

struct OwnedRoot {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-journal-runtime-" + MaiIdGenerator::newSessionId()));
    MaiFilePath index = root.append(MaiFilePath::fromUtf8("agent.db"));
    OwnedRoot() {
        CHECK(!MaiFileSystem::createDirectories(root));
    }
    ~OwnedRoot() {
        const auto temporary = MaiFileSystem::resolve(MaiFileSystem::temporaryDirectory());
        const auto resolved = MaiFileSystem::resolve(root);
        if (!failures && !resolved.isEmpty() && temporary.isParentOf(resolved))
            MaiFileSystem::removeRecursively(resolved);
        else
            std::printf("Owned runtime fixture: %s\n", root.toUtf8().c_str());
    }
    bool owns(const MaiFilePath& path) const {
        return MaiFileSystem::resolve(root).isParentOf(MaiFileSystem::resolve(path));
    }
};

struct Loopback {
    httplib::Server server;
    std::thread worker;
    std::atomic<int> requests{0};
    std::string script;
    int port = 0;
    Loopback() {
        server.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            response.set_content(script, "text/event-stream");
        });
        port = server.bind_to_any_port("127.0.0.1");
        CHECK(port > 0);
        if (port <= 0) return;
        worker = std::thread([&] {
            MaiThread::setCurrentName("mai-journal-sse");
            server.listen_after_bind();
        });
        for (int i = 0; i < 200 && !server.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(server.is_running());
    }
    ~Loopback() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::unique_ptr<MaiModelClient> model() const {
        MaiModelConfig config;
        config.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        config.maxRetries = 0;
        config.contextLimits = [](const std::string&) {
            return MaiModelContextLimits{20000, 0, 0};
        };
        return makeMaiResponsesClient(config);
    }
};

std::string event(const Json& value) {
    return "data: " + value.dump() + "\n\n";
}

MaiMessage textMessage(MaiRole role, const std::string& text) {
    MaiMessage message;
    message.id = MaiIdGenerator::newMessageId();
    message.role = role;
    message.created = MaiTime::getCurrentTime();
    if (!text.empty()) {
        MaiMessagePart part;
        part.id = MaiIdGenerator::newPartId();
        part.created = message.created;
        part.body = MaiTextPart{text};
        message.parts.push_back(std::move(part));
    }
    return message;
}

bool run(MaiSessionStore& store, MaiModelClient& model, const std::string& sessionId,
         const MaiMessage& assistant) {
    MaiEventBus bus;
    MaiEventEmitter emitter(bus);
    MaiContextBuilder context;
    MaiToolRegistry tools;
    MaiTurnRunner::Dependencies dependencies;
    dependencies.store = &store;
    dependencies.model = &model;
    dependencies.emitter = &emitter;
    dependencies.context = &context;
    dependencies.tools = &tools;
    std::atomic<bool> cancel{false};
    MaiTurnRunner runner(dependencies, sessionId, assistant);
    runner.run(cancel);
    return runner.succeeded();
}

std::vector<MaiFilePath> rolloutFiles(const OwnedRoot& fixture) {
    std::vector<MaiFilePath> files;
    MaiFileSystem::walk(
        fixture.root.append(MaiFilePath::fromUtf8("sessions")),
        [&](const MaiFileEntry& entry) {
            if (!entry.isDirectory && entry.nameUtf8.compare(0, 8, "rollout-") == 0)
                files.push_back(entry.path);
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath&, const MaiError&) { CHECK(false); });
    return files;
}

void checkRecovered(MaiSessionStore& store, const std::string& sessionId,
                    const std::string& assistantId, const std::string& unfinished) {
    const auto history = store.listMessages(sessionId);
    CHECK(!store.lastReadError());
    CHECK(!store.lastProjectionError());
    bool completed = false;
    for (const auto& message : history)
        for (const auto& part : message.parts) {
            if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
                CHECK(text->text.find(unfinished) == std::string::npos);
                if (message.id == assistantId && text->text == "completed-A-runtime-fixture-58b") {
                    CHECK(!part.modelDraft);
                    completed = true;
                }
            }
            CHECK(!std::holds_alternative<MaiToolPart>(part.body));
        }
    CHECK(completed);
    const auto replay = MaiContextBuilder{}.build(history);
    std::string replayText;
    for (const auto& message : replay) replayText += message.content;
    CHECK(replayText.find("completed-A-runtime-fixture-58b") != std::string::npos);
    CHECK(replayText.find(unfinished) == std::string::npos);
}

void checkMeasuredUsage(Loopback& loopback) {
    OwnedRoot fixture;
    auto opened = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(opened);
    if (!opened) return;
    auto model = loopback.model();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.model = "gpt-6.1-sol";
    session.created = MaiTime::getCurrentTime();
    session.updated = session.created;
    opened.value()->putSession(session);
    opened.value()->putMessage(session.id, textMessage(MaiRole::User, "Owned usage fixture"));
    auto assistant = textMessage(MaiRole::Assistant, {});
    opened.value()->putMessage(session.id, assistant);
    const auto provider = model->usageProviderForModel(session.model);
    CHECK(!provider.empty() && provider.find("127.0.0.1") == std::string::npos);
    const auto response = [&](std::size_t output, std::size_t total, const char* id) {
        return event(
            {{"type", "response.completed"},
             {"response",
              Json{{"id", id},
                   {"status", "completed"},
                   {"output",
                    Json::array({Json{
                        {"id", std::string(id) + "-text"},
                        {"type", "message"},
                        {"role", "assistant"},
                        {"status", "completed"},
                        {"content", Json::array({Json{{"type", "output_text"},
                                                      {"text", "Owned measured answer"}}})}}})},
                   {"usage", Json{{"input_tokens", 15000},
                                  {"output_tokens", output},
                                  {"total_tokens", total},
                                  {"input_tokens_details", Json{{"cached_tokens", 14000}}}}}}}});
    };
    loopback.script = response(1000, 16000, "fixture-measured-response");
    CHECK(run(*opened.value(), *model, session.id, assistant));
    const auto live =
        maiReadModelContextUsage(*opened.value(), session.id, session.model, provider);
    CHECK(live && live.value().samplingValid);
    if (live) {
        CHECK(live.value().latestSampling.total.tokens == 16000);
        CHECK(live.value().latestSampling.cachedInput.tokens == 14000);
        CHECK(maiContextRemainingPercent(live.value()) == 50);
    }
    const auto metadata = opened.value()->readTurnMetadata(session.id, assistant.id);
    CHECK(metadata && metadata.value().turnOutcome && metadata.value().turnOutcome->context);
    if (metadata && metadata.value().turnOutcome && metadata.value().turnOutcome->context)
        CHECK(metadata.value().turnOutcome->context->usage.latestSampling.total.tokens == 16000);
    // 第二次主采样明确输出零，仍必须发布和保存 input/total 的新测量。
    assistant = textMessage(MaiRole::Assistant, {});
    opened.value()->putMessage(session.id, assistant);
    loopback.script = response(0, 15000, "fixture-zero-output-response");
    CHECK(run(*opened.value(), *model, session.id, assistant));
    opened.value().reset();
    auto reopened = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(reopened);
    if (!reopened) return;
    const auto cold =
        maiReadModelContextUsage(*reopened.value(), session.id, session.model, provider);
    CHECK(cold && cold.value().samplingValid);
    if (cold) {
        CHECK(cold.value().latestSampling.output.tokens == 0);
        CHECK(cold.value().latestSampling.output.source == MaiUsageValueSource::Provider);
        CHECK(cold.value().latestSampling.total.tokens == 15000);
        CHECK(cold.value().turn.total.tokens == 15000);
        CHECK(cold.value().thread.total.tokens == 31000);
        CHECK(cold.value().responseId == "fixture-zero-output-response");
        CHECK(maiContextRemainingPercent(cold.value()) == 63);
    }
    const auto wrongProvider =
        maiReadModelContextUsage(*reopened.value(), session.id, session.model, "different");
    CHECK(wrongProvider && !wrongProvider.value().samplingValid);
    reopened.value().reset();
    // 夹具独占的派生索引可删后重建；采样和消费必须仍来自原 JSONL。
    for (const auto* suffix : {"", "-wal", "-shm"}) {
        const auto path = MaiFilePath::fromUtf8(fixture.index.toUtf8() + suffix);
        CHECK(fixture.owns(path));
        if (fixture.owns(path) && MaiFileSystem::exists(path))
            CHECK(!MaiFileSystem::removeFile(path));
    }
    auto rebuilt = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(rebuilt);
    if (!rebuilt) return;
    const auto reconstructed =
        maiReadModelContextUsage(*rebuilt.value(), session.id, session.model, provider);
    CHECK(reconstructed && reconstructed.value().samplingValid &&
          reconstructed.value().thread.total.tokens == 31000);
    CHECK(rebuilt.value()->clearMessages(session.id));
    const auto cleared =
        maiReadModelContextUsage(*rebuilt.value(), session.id, session.model, provider);
    CHECK(cleared && !cleared.value().samplingValid);
}
}  // namespace

int main() {
    OwnedRoot fixture;
    Loopback loopback;
    if (loopback.port <= 0) return 1;
    auto model = loopback.model();
    auto opened = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(opened);
    if (!opened) return 1;
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.model = "gpt-6.1-sol";
    session.created = MaiTime::getCurrentTime();
    session.updated = session.created;
    opened.value()->putSession(session);
    if (const auto error = opened.value()->lastWriteError())
        std::printf("initial session write: %s\n", error.message().c_str());
    opened.value()->putMessage(session.id,
                               textMessage(MaiRole::User, "Owned runtime fixture input"));
    if (const auto error = opened.value()->lastWriteError())
        std::printf("initial user write: %s\n", error.message().c_str());
    auto assistant = textMessage(MaiRole::Assistant, {});
    opened.value()->putMessage(session.id, assistant);
    if (const auto error = opened.value()->lastWriteError())
        std::printf("initial assistant write: %s\n", error.message().c_str());
    CHECK(!opened.value()->lastWriteError());

    const std::string unfinished = "unfinished-B-runtime-fixture-58b";
    const Json first{{"id", "msg_native_A_58b"},
                     {"type", "message"},
                     {"role", "assistant"},
                     {"status", "completed"},
                     {"future_unknown", Json{{"nested", Json::array({1, "opaque", nullptr})}}},
                     {"content", Json::array({Json{{"type", "output_text"},
                                                   {"text", "completed-A-runtime-fixture-58b"}}})}};
    const Json unknown{{"id", "future_native_58b"},
                       {"type", "future_native_item"},
                       {"opaque_unknown", Json{{"signature", "fixture-not-a-real-signature"}}}};
    const Json tool{{"id", "function_native_58b"},
                    {"type", "function_call"},
                    {"call_id", "call_never_execute_58b"},
                    {"name", "fixture_never_execute"},
                    {"arguments", "{}"}};
    loopback.script =
        event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}});
    loopback.script +=
        event({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", first}});
    loopback.script +=
        event({{"type", "response.output_text.delta"}, {"output_index", 1}, {"delta", unfinished}});
    loopback.script +=
        event({{"type", "response.output_item.done"}, {"output_index", 2}, {"item", unknown}});
    loopback.script +=
        event({{"type", "response.output_item.done"}, {"output_index", 3}, {"item", tool}});
    loopback.script +=
        event({{"type", "response.failed"},
               {"response", Json{{"error", Json{{"message", "fixture failed tail"}}}}}});
    CHECK(!run(*opened.value(), *model, session.id, assistant));
    CHECK(loopback.requests.load() == 1);
    CHECK(!opened.value()->lastWriteError());
    opened.value().reset();

    const auto files = rolloutFiles(fixture);
    CHECK(files.size() == 1);
    if (files.empty()) return 1;
    std::string source;
    CHECK(!MaiFileSystem::readFile(files.front(), source));
    CHECK(source.find(unfinished) == std::string::npos);
    int nativeFirst = 0;
    int nativeUnknown = 0;
    int taskStarted = 0;
    int taskFailed = 0;
    std::size_t offset = 0;
    while (offset < source.size()) {
        const auto end = source.find('\n', offset);
        CHECK(end != std::string::npos);
        if (end == std::string::npos) break;
        const auto record = Json::parse(source.substr(offset, end - offset), nullptr, false);
        CHECK(record.is_object());
        if (record.is_object() && record.value("type", "") == "response_item") {
            const auto& payload = record["payload"];
            if (payload.value("id", "") == "msg_native_A_58b") {
                CHECK(payload == first);
                ++nativeFirst;
            }
            if (payload.value("id", "") == "future_native_58b") {
                CHECK(payload == unknown);
                ++nativeUnknown;
            }
        }
        if (record.is_object() && record.value("type", "") == "event_msg") {
            const auto& payload = record["payload"];
            if (payload.value("type", "") == "task_started") {
                CHECK(payload.value("turn_id", "") == assistant.id);
                ++taskStarted;
            }
            if (payload.value("type", "") == "task_complete") {
                CHECK(payload.value("turn_id", "") == assistant.id);
                CHECK(payload.value("status", "") == "failed");
                CHECK(payload["error"]["message"] == "fixture failed tail");
                ++taskFailed;
            }
        }
        offset = end + 1;
    }
    CHECK(nativeFirst == 1 && nativeUnknown == 1);
    CHECK(taskStarted == 1 && taskFailed == 1);
    auto reopened = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(reopened);
    if (!reopened) return 1;
    checkRecovered(*reopened.value(), session.id, assistant.id, unfinished);
    reopened.value().reset();

    // 只移除本夹具明确拥有的索引及其两个 SQLite sidecar；权威 JSONL 保持原字节。
    for (const auto* suffix : {"", "-wal", "-shm"}) {
        const auto index = MaiFilePath::fromUtf8(fixture.index.toUtf8() + suffix);
        CHECK(fixture.owns(index));
        if (fixture.owns(index) && MaiFileSystem::exists(index))
            CHECK(!MaiFileSystem::removeFile(index));
    }
    auto rebuilt = makeMaiSqliteStore(fixture.index.toUtf8());
    CHECK(rebuilt);
    if (!rebuilt) return 1;
    checkRecovered(*rebuilt.value(), session.id, assistant.id, unfinished);
    rebuilt.value().reset();
    std::string afterRebuild;
    CHECK(!MaiFileSystem::readFile(files.front(), afterRebuild));
    CHECK(afterRebuild == source);

    // 独立账号根中的源正文缺失后，即使索引还在也不能发起新的付费模型请求。
    CHECK(fixture.owns(files.front()));
    CHECK(!MaiFileSystem::removeFile(files.front()));
    const auto beforeRequests = loopback.requests.load();
    auto missing = makeMaiSqliteStore(fixture.index.toUtf8());
    if (missing) {
        CHECK(!run(*missing.value(), *model, session.id, textMessage(MaiRole::Assistant, {})));
        // 新 task_started 的源追加也可能先于历史读取被缺失正文拒绝；再直接核对读健康。
        const auto unavailable = missing.value()->listMessages(session.id);
        CHECK(unavailable.empty());
        CHECK(missing.value()->lastReadError());
        missing.value().reset();
    } else
        CHECK(missing.error());
    CHECK(loopback.requests.load() == beforeRequests);
    checkMeasuredUsage(loopback);
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
