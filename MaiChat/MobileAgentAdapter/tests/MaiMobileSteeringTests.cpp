// 文件作用：通过真实移动 C ABI 验证运行中补充输入的模型读取与工具批次边界。
// 所有账号资料、模型与工具均为本进程合成数据；网络只使用环回假模型端点。
#include "MaiMobileAgent.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"

namespace {
using Json = nlohmann::json;

std::atomic<int> mobileSessionChanges{0};

void localSessionChanged(const char* sessionId) {
    if (sessionId != nullptr && sessionId[0] != '\0') ++mobileSessionChanges;
}

void check(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

Json call(void* handle, const Json& request) {
    char* encoded = maiMobileAgentRequest(handle, request.dump().c_str());
    check(encoded != nullptr, "C ABI returned no response");
    const auto response = Json::parse(encoded, nullptr, false);
    maiMobileAgentFree(encoded);
    check(response.is_object(), "C ABI returned invalid JSON");
    return response;
}

struct HeldHost {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool released = false;
    std::atomic<int> calls{0};
    std::atomic<int> assetProbes{0};
    std::atomic<bool> destroyed{false};
};

const char* hostTool(void* context, const char* name, const char*) {
    auto& host = *static_cast<HeldHost*>(context);
    if (std::strcmp(name, "mobile_ark_service_config") == 0) {
        ++host.assetProbes;
        // configure 只探测资产能力；合成宿主明确禁用它，不下发任何真实凭据。
        constexpr const char response[] = R"({"ok":false,"error":"Synthetic assets unavailable"})";
        auto* copied = static_cast<char*>(std::malloc(sizeof(response)));
        if (copied) std::memcpy(copied, response, sizeof(response));
        return copied;
    }
    check(std::strcmp(name, "maichat_get_current_account") == 0, "unexpected synthetic host tool");
    std::unique_lock<std::mutex> lock(host.mutex);
    ++host.calls;
    host.entered = true;
    host.wake.notify_all();
    const bool released = host.wake.wait_for(lock, std::chrono::seconds(10), [&] { return host.released; });
    const std::string response = released ? Json{{"ok", true}, {"output", "Synthetic current result"}}.dump()
                                          : Json{{"ok", false}, {"error", "Synthetic test gate timed out"}}.dump();
    auto* copied = static_cast<char*>(std::malloc(response.size() + 1));
    if (copied) std::memcpy(copied, response.c_str(), response.size() + 1);
    return copied;
}

void freeHost(void*, const char* response) {
    std::free(const_cast<char*>(response));
}
void releaseHost(void* context) {
    static_cast<HeldHost*>(context)->destroyed = true;
}

std::string frame(const Json& delta, const char* finish) {
    return "data: " +
           Json{{"choices", Json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}}.dump() +
           "\n\ndata: [DONE]\n\n";
}
}  // namespace

int main() {
    HeldHost host;
    httplib::Server server;
    std::mutex requestsMutex;
    std::vector<Json> requests;
    server.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
        const Json body = Json::parse(request.body);
        std::size_t index;
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            index = requests.size();
            requests.push_back(body);
        }
        if (index == 0) {
            const Json tool{{"index", 0},
                            {"id", "synthetic-profile"},
                            {"type", "function"},
                            {"function", {{"name", "maichat_get_current_account"}, {"arguments", "{}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({tool})}}, "tool_calls"), "text/event-stream");
        } else if (index == 2) {
            const Json tool{
                {"index", 0},
                {"id", "synthetic-question"},
                {"type", "function"},
                {"function", {{"name", "question"}, {"arguments", R"({"question":"Which result should be kept?"})"}}}};
            response.set_content(frame({{"tool_calls", Json::array({tool})}}, "tool_calls"), "text/event-stream");
        } else {
            response.set_content(frame({{"content", "Current result ready."}}, "stop"), "text/event-stream");
        }
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) return 1;
    std::thread listener([&] {
        MaiThread::setCurrentName("mai-steer-http");
        server.listen_after_bind();
    });
    const auto workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-mobile-steering-" + MaiIdGenerator::newEventId()));
    void* handle = maiMobileAgentCreate();
    int status = 0;
    try {
        check(handle != nullptr, "create failed");
        check(!MaiFileSystem::createDirectories(workspace).hasError(), "workspace creation failed");
        check(maiMobileAgentSetChangeHandler(handle, localSessionChanged) == 1,
              "change notification registration failed");
        check(maiMobileAgentSetHostToolHandler(handle, &host, hostTool, freeHost, releaseHost) == 1,
              "host registration failed");
        const Json configured =
            call(handle, {{"op", "configure"},
                          {"database", workspace.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8()},
                          {"workspace", workspace.toUtf8()},
                          {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
                          {"apiKey", "synthetic-test-key"},
                          {"model", "test-model"},
                          {"policy", "never"}});
        if (!configured.at("ok").get<bool>())
            throw std::runtime_error("configure failed: " + configured.value("error", std::string{}));
        const Json cloudProbe = call(
            handle,
            {{"op", "local_model_probe"}, {"messages", Json::array({{{"role", "user"}, {"content", "Local only"}}})}});
        check(!cloudProbe.at("ok") && cloudProbe.value("errorCode", "") == "not_configured",
              "device probe could use a remote model instead of failing closed");
        const std::string session = call(handle, {{"op", "create"}}).at("id");
        const Json started =
            call(handle, {{"op", "send"}, {"session", session}, {"text", "Improve the current result"}});
        check(started.at("ok"), "send failed");
        const std::string turn = started.at("id");
        check(started.value("turnId", "") == turn, "send receipt did not identify its original assistant turn");
        const std::string initialUser = started.value("user_message_id", "");
        check(!initialUser.empty() && initialUser != turn, "send receipt confused assistant and user identities");
        const Json initialRows =
            call(handle, {{"op", "messages_page"}, {"session", session}, {"before", turn}, {"limit", 1}});
        check(initialRows.at("messages").size() == 1 && initialRows.at("messages").front().at("role") == "user" &&
                  initialRows.at("messages").front().at("id") == initialUser,
              "send receipt user identity was not the actual persisted predecessor");
        const Json exactUser = call(handle, {{"op", "message"}, {"session", session}, {"id", initialUser}});
        check(exactUser.at("ok") && exactUser.at("messages").size() == 1 &&
                  exactUser.at("messages").front().at("id") == initialUser &&
                  exactUser.at("messages").front().at("role") == "user",
              "exact receipt query did not return the persisted user");
        const std::string otherSession = call(handle, {{"op", "create"}}).at("id");
        const Json wrongOwner = call(handle, {{"op", "message"}, {"session", otherSession}, {"id", initialUser}});
        check(!wrongOwner.at("ok") && wrongOwner.value("errorCode", "") == "not_found",
              "exact receipt query crossed conversation ownership");
        const Json absentUser = call(handle, {{"op", "message"}, {"session", session}, {"id", "msg_absent_fixture"}});
        check(!absentUser.at("ok") && absentUser.value("errorCode", "") == "not_found",
              "exact receipt query returned a neighboring row for a missing id");
        {
            std::unique_lock<std::mutex> lock(host.mutex);
            check(host.wake.wait_for(lock, std::chrono::seconds(5), [&] { return host.entered; }),
                  "tool did not reach the synthetic host");
        }
        const auto snapshot = [&] { return call(handle, {{"op", "snapshot"}, {"session", session}, {"force", true}}); };
        check(snapshot().at("turnId") == turn, "snapshot changed the stable turn identity");
        // 带轮次的 Stop 是点击时的取消目标，旧消息不能取消已经替换的新任务。
        const auto wrongStop = call(handle, {{"op", "stop"}, {"session", session}, {"turnId", "old-turn"}});
        check(!wrongStop.at("ok") && wrongStop.value("errorCode", "") == "not_found",
              "wrong-turn stop was not rejected before interrupting the active task");
        check(snapshot().at("turnId") == turn, "wrong-turn stop interrupted the active task");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("turnId") == turn,
              "remote snapshot lost the target turn identity");
        check(!call(handle, {{"op", "send"}, {"session", session}, {"text", "Busy send"}}).at("ok"),
              "ordinary send no longer rejects busy work");
        const auto boundedSnapshot = [&] {
            return call(handle, {{"op", "snapshot"},
                                 {"session", session},
                                 {"force", true},
                                 {"messageLimit", 8},
                                 {"includeMessageUpdates", true}});
        };
        check(boundedSnapshot().at("messages").size() <= 8, "initial mobile snapshot read unbounded history");
        // 把仍在运行的 assistant 推出最新八条；后续完成状态必须通过精确更新通道送回。
        for (int index = 0; index < 10; ++index)
            check(call(handle, {{"op", "steer"},
                                {"session", session},
                                {"turnId", turn},
                                {"requestId", "synthetic-page-" + std::to_string(index)},
                                {"text", "Synthetic queued supplement " + std::to_string(index)}})
                      .at("ok"),
                  "synthetic pagination supplement was rejected");
        const Json busyPage = boundedSnapshot();
        check(busyPage.at("messages").size() == 8, "latest mobile page was not bounded to eight messages");
        for (const auto& message : busyPage.at("messages"))
            check(message.at("id") != turn, "fixture failed to move the active assistant outside the latest window");
        const Json steer{{"op", "steer"},
                         {"session", session},
                         {"turnId", turn},
                         {"requestId", "synthetic-steering-one"},
                         {"text", "Show the current result first"}};
        const Json accepted = call(handle, steer);
        check(accepted.at("ok"), "steering was not accepted");
        check(accepted.value("user_message_id", "") == accepted.at("id").get<std::string>(),
              "steer receipt did not identify the accepted user message");
        check(accepted.value("turnId", "") == turn, "steer receipt changed its original task identity");
        int persistedUsers = 0;
        const Json acceptedSnapshot = snapshot();
        for (const auto& message : acceptedSnapshot.at("messages"))
            if (message.at("id") == accepted.at("id")) ++persistedUsers;
        check(persistedUsers == 1, "accepted user was not present in the persisted snapshot");
        check(call(handle, steer).at("id") == accepted.at("id"), "steering retry duplicated the user message");
        Json conflict = steer;
        conflict["text"] = "Different input";
        check(!call(handle, conflict).at("ok"), "request identity accepted conflicting content");
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            check(requests.size() == 1, "steering sent a model request before the running tool completed");
        }
        {
            std::lock_guard<std::mutex> lock(host.mutex);
            check(!host.released, "steering canceled the held host operation");
            host.released = true;
            host.wake.notify_all();
        }
        Json completed;
        for (int attempt = 0; attempt < 500; ++attempt) {
            completed = snapshot();
            if (!completed.at("busy").get<bool>()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(!completed.at("busy").get<bool>(), "steered turn did not finish");
        check(completed.at("turnId") == "", "idle snapshot retained an active turn");
        const Json finishedPage = boundedSnapshot();
        check(finishedPage.at("messageUpdates").size() <= 20, "mobile update metadata exceeded one old page");
        bool oldAssistantCompleted = false;
        for (const auto& message : finishedPage.at("messageUpdates"))
            if (message.at("id") == turn && message.at("completed").get<std::int64_t>() != 0)
                oldAssistantCompleted = true;
        check(oldAssistantCompleted, "active assistant completion was lost outside the latest mobile window");
        std::set<std::string> loaded;
        std::string before;
        for (const auto& message : finishedPage.at("messages")) {
            loaded.insert(message.at("id").get<std::string>());
            before = message.at("id").get<std::string>();
        }
        while (!before.empty()) {
            const Json page =
                call(handle, {{"op", "messages_page"}, {"session", session}, {"before", before}, {"limit", 20}});
            check(page.at("ok") && page.at("messages").size() <= 20, "old mobile page exceeded its query limit");
            if (page.at("messages").empty()) break;
            for (const auto& message : page.at("messages")) {
                const std::string id = message.at("id").get<std::string>();
                check(id < before && loaded.insert(id).second, "old page cursor repeated or skipped its boundary");
            }
            before = page.at("messages").back().at("id").get<std::string>();
        }
        check(loaded.size() == completed.at("messages").size(), "native mobile pages did not reach the earliest row");
        const Json completedRetry = call(handle, steer);
        check(completedRetry.at("id") == accepted.at("id"), "completed-turn retry lost its receipt");
        check(completedRetry.value("turnId", "") == turn &&
                  completedRetry.value("user_message_id", "") == accepted.at("id").get<std::string>(),
              "idle steering retry lost the original task/user receipt");
        const std::string acceptedUserId = accepted.at("id").get<std::string>();
        const Json receiptPage = call(handle, {{"op", "message"}, {"session", session}, {"id", acceptedUserId}});
        check(receiptPage.at("ok") && receiptPage.at("messages").size() == 1 &&
                  receiptPage.at("messages")[0].at("id") == acceptedUserId &&
                  receiptPage.at("messages")[0].at("role") == "user",
              "exact receipt lookup lost the accepted user behind newer assistant messages");
        Json late = steer;
        late["requestId"] = "synthetic-idle-input";
        const Json idleRejection = call(handle, late);
        check(!idleRejection.at("ok") && idleRejection.value("errorCode", std::string{}) == "not_found",
              "idle steering did not preserve the typed not_found rejection");
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            check(requests.size() == 2, "accepted steering did not reach exactly one follow-up model request");
            const auto& messages = requests.back().at("messages");
            check(messages.size() >= 5, "follow-up model history was incomplete");
            const std::size_t last = messages.size() - 1;
            check(
                messages[last].at("role") == "user" && messages[last].at("content") == "Show the current result first",
                "model did not read the accepted user input");
            std::size_t invocationIndex = messages.size(), resultIndex = messages.size();
            std::vector<std::string> observedInputs;
            std::vector<std::string> expectedInputs;
            for (int index = 0; index < 10; ++index)
                expectedInputs.push_back("Synthetic queued supplement " + std::to_string(index));
            expectedInputs.push_back("Show the current result first");
            std::printf("Synthetic follow-up roles:");
            for (std::size_t index = 0; index < messages.size(); ++index) {
                const Json& message = messages[index];
                const std::string role = message.value("role", "");
                std::printf(" %zu:%s", index, role.c_str());
                if (role == "assistant" && message.contains("tool_calls"))
                    for (const auto& tool : message.at("tool_calls"))
                        if (tool.value("id", "") == "synthetic-profile") {
                            check(invocationIndex == messages.size(), "tool invocation was duplicated");
                            invocationIndex = index;
                        }
                if (role == "tool" && message.value("tool_call_id", "") == "synthetic-profile") {
                    check(resultIndex == messages.size(), "tool result was duplicated");
                    resultIndex = index;
                }
                if (role == "user" && message.value("content", Json{}).is_string()) {
                    const std::string text = message.at("content");
                    if (text.rfind("Synthetic queued supplement ", 0) == 0 || text == "Show the current result first") {
                        check(resultIndex < index, "user input appeared before the complete tool call/result pair");
                        observedInputs.push_back(text);
                    }
                }
            }
            std::puts("");
            check(invocationIndex < messages.size() && resultIndex == invocationIndex + 1,
                  "user input split a tool call/result pair");
            check(observedInputs == expectedInputs, "accepted supplemental inputs were reordered or lost");
        }
        check(host.calls.load() == 1, "steering restarted the completed host operation");
        std::puts(
            "PASS: real C ABI steering, persisted user, dedupe, stable turn, complete tool results, next model input");

        // 真实 question 闸门经 C ABI 结算后，移动宿主必须收到通知并读到权威空列表。
        // 通知不暴露事件类型，专属 QuestionAnswered 事件由核心测试单独验证。
        check(call(handle, {{"op", "send"}, {"session", session}, {"text", "Ask which result to keep"}}).at("ok"),
              "question turn did not start");
        Json pending;
        for (int attempt = 0; attempt < 500; ++attempt) {
            pending = snapshot();
            if (!pending.at("questions").empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(pending.at("questions").size() == 1, "synthetic question did not become pending");
        const std::string questionId = pending.at("questions").front().at("id");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questions").size() == 1,
              "remote snapshot did not expose the pending question");
        const int changesBeforeAnswer = mobileSessionChanges.load();
        check(call(handle, {{"op", "answer"}, {"session", session}, {"id", questionId}, {"text", "Keep this result"}})
                  .at("ok"),
              "pending question answer was not accepted");
        bool clearedByNotification = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            const Json update = call(handle, {{"op", "snapshot"}, {"session", session}});
            if (update.value("changed", false) && update.at("questions").empty() &&
                mobileSessionChanges.load() > changesBeforeAnswer) {
                clearedByNotification = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(clearedByNotification, "settled question did not notify the mobile consumer with cleared pending state");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questions").empty(),
              "remote snapshot retained the consumed question");
        const Json staleAnswer =
            call(handle, {{"op", "answer"}, {"session", session}, {"id", questionId}, {"text", "Repeated answer"}});
        check(!staleAnswer.at("ok") && staleAnswer.value("errorCode", "") == "not_found",
              "consumed question did not preserve its typed stale-answer rejection");
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (!snapshot().at("busy").get<bool>()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(!snapshot().at("busy").get<bool>(), "answered question turn did not finish");
        std::puts("PASS: mobile question answer notification, authoritative pending clear, stale-answer rejection");

        // 配置检查不启动推理；真实 GGUF 生成与工具闭环由本地主模型专测/正常 App 另验。
        // 这里独立证明参数不回绕、损坏文件不覆盖配置，失败不偷偷降级到 HTTP。
        const int assetProbesBeforeLocal = host.assetProbes.load();
        const auto localFile = workspace.append(MaiFilePath::fromUtf8("local-config-probe.gguf"));
        check(!MaiFileSystem::writeFile(localFile, std::string("GGUF") + std::string(24, '\0')),
              "local configuration probe creation failed");
        Json localConfig{{"op", "configure"},
                         {"database", workspace.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8()},
                         {"workspace", workspace.toUtf8()},
                         {"modelProvider", "llama"},
                         {"wire", "local"},
                         {"model", "local:configuration-probe"},
                         {"localModelPath", localFile.toUtf8()},
                         {"localContextSize", 32768},
                         {"localMaximumTokens", 0},
                         {"localThreads", 2},
                         {"policy", "never"}};
        auto invalid = localConfig;
        invalid["localContextSize"] = std::uint64_t(-1);
        const auto overflow = call(handle, invalid);
        check(!overflow.at("ok") && overflow.value("errorCode", "") == "invalid_input",
              "local context integer overflow was accepted");
        invalid = localConfig;
        invalid["localModelPath"] = workspace.append(MaiFilePath::fromUtf8("missing.gguf")).toUtf8();
        const auto missing = call(handle, invalid);
        check(!missing.at("ok") && missing.value("errorCode", "") == "not_found",
              "missing local model was not explicitly rejected");
        invalid = localConfig;
        invalid["localModelPath"] = MaiFileSystem::temporaryDirectory().toUtf8();
        const auto outside = call(handle, invalid);
        check(!outside.at("ok") && outside.value("errorCode", "") == "invalid_input",
              "local model escaped its application root");
        const auto corrupt = call(handle, localConfig);
        check(!corrupt.at("ok") && corrupt.value("errorCode", "") == "invalid_input",
              "corrupt local GGUF replaced a valid configured Agent");
        const auto retained = snapshot();
        check(retained.at("configured") && retained.at("model") == "test-model",
              "failed local configuration changed the original primary model");

        // 可选的公开真实权重探针。显式复制到本测试 root 内，不放宽正式路径边界。
        // 不设置该环境变量时只跑拒绝门禁，不宣称已验收真实模型配置。
        if (const char* modelPath = std::getenv("MAI_LOCAL_PRIMARY_GGUF")) {
            const auto validFile = workspace.append(MaiFilePath::fromUtf8("real-primary-model.gguf"));
            std::error_code copyError;
            std::filesystem::copy_file(std::filesystem::u8path(modelPath), std::filesystem::u8path(validFile.toUtf8()),
                                       copyError);
            check(!copyError, "public test GGUF could not be copied into the isolated workspace");
            localConfig["localModelPath"] = validFile.toUtf8();
            check(call(handle, localConfig).at("ok"), "valid local configuration required a cloud API key");
            const std::string localSession = call(handle, {{"op", "create"}}).at("id");
            check(call(handle, {{"op", "snapshot"}, {"session", localSession}, {"force", true}}).at("configured"),
                  "local configured state still depended on an API key");
            if (std::getenv("MAI_LOCAL_PRIMARY_RUN_PROBE")) {
                const Json input{
                    {"op", "local_model_probe"},
                    {"timeout_ms", 120000},
                    {"messages", Json::array({{{"role", "user"},
                                               {"content", "Do not use tools. Reply with only: Hello from Qwen."}}})}};
                const auto probe = call(handle, input);
                check(
                    probe.at("ok") && probe.at("tool_call_count") == 0 && !probe.at("text").get<std::string>().empty(),
                    "real local non-executing client probe failed");
                check(probe.at("wire") == "local", "device probe changed model execution mode");
                const auto history = call(handle, {{"op", "messages_page"}, {"session", localSession}});
                check(history.at("messages").empty(), "non-executing probe wrote session history");
                auto invalidProbe = input;
                invalidProbe["timeout_ms"] = std::uint64_t(-1);
                check(!call(handle, invalidProbe).at("ok"), "probe deadline overflow was accepted");
                invalidProbe = input;
                invalidProbe["messages"][0]["role"] = "system";
                check(!call(handle, invalidProbe).at("ok"), "probe allowed overriding model instructions");
            }
        }
        check(host.assetProbes.load() == assetProbesBeforeLocal,
              "local configuration unexpectedly probed cloud asset credentials");
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            check(requests.size() == 4, "local configuration unexpectedly sent a remote model request");
        }
        std::puts("PASS: local C ABI configuration, no API key, limits, file boundary, no remote fallback");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        status = 1;
    }
    {
        std::lock_guard<std::mutex> lock(host.mutex);
        host.released = true;
        host.wake.notify_all();
    }
    maiMobileAgentDestroy(handle);
    server.stop();
    listener.join();
    MaiFileSystem::removeRecursively(workspace);
    if (!host.destroyed.load()) return 1;
    return status;
}
