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
#include "MaiSqliteStore.h"
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
    std::atomic<bool> windowEntered{false};
    std::atomic<bool> windowEmitTail{false};
    std::atomic<bool> windowTailSent{false};
    std::atomic<bool> windowReleased{false};
    std::atomic<bool> windowQuestionReplanned{false};
    server.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
        const Json body = Json::parse(request.body);
        std::size_t index;
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            index = requests.size();
            requests.push_back(body);
        }
        std::string lastUser;
        for (const auto& message : body.at("messages"))
            if (message.value("role", "") == "user" && message.value("content", Json{}).is_string())
                lastUser = message.at("content");
        if (lastUser == "Native old-window stream") {
            response.set_chunked_content_provider("text/event-stream", [&](std::size_t, httplib::DataSink& sink) {
                const auto writeDelta = [&](const Json& delta, const Json& finish) {
                    const auto bytes =
                        "data: " +
                        Json{{"choices", Json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}}
                            .dump() +
                        "\n\n";
                    return sink.write(bytes.data(), bytes.size());
                };
                if (!writeDelta({{"content", std::string(1100, 'P')}}, nullptr)) return false;
                windowEntered = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!windowReleased && sink.is_writable() && std::chrono::steady_clock::now() < deadline) {
                    if (windowEmitTail.exchange(false)) {
                        if (!writeDelta({{"content", " Native live tail outside latest twenty"}}, nullptr))
                            return false;
                        windowTailSent = true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (!windowReleased) return false;
                const Json tool{
                    {"index", 0},
                    {"id", "native-outside-page-question"},
                    {"type", "function"},
                    {"function",
                     {{"name", "question"}, {"arguments", R"({"question":"Keep the native old result?"})"}}}};
                if (!writeDelta({{"tool_calls", Json::array({tool})}}, "tool_calls")) return false;
                sink.write("data: [DONE]\n\n", 14);
                sink.done();
                return true;
            });
        } else if (lastUser == "Native accepted supplement 20" && !windowQuestionReplanned.exchange(true)) {
            // 新输入已被真实下一次模型请求读取，重新提出的问题不再属于被取消的旧计划。
            const Json tool{
                {"index", 0},
                {"id", "native-replanned-question"},
                {"type", "function"},
                {"function",
                 {{"name", "question"}, {"arguments", R"({"question":"Keep the reviewed native result?"})"}}}};
            response.set_content(frame({{"tool_calls", Json::array({tool})}}, "tool_calls"), "text/event-stream");
        } else if (index == 0) {
            const Json tool{{"index", 0},
                            {"id", "synthetic-profile"},
                            {"type", "function"},
                            {"function", {{"name", "maichat_get_current_account"}, {"arguments", "{}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({tool})}}, "tool_calls"), "text/event-stream");
        } else if (index == 2 || index == 4 || index == 6) {
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
        const Json question = pending.at("questions").front();
        const std::string questionId = question.at("id");
        check(question.value("sessionId", "") == session && !question.value("messageId", "").empty() &&
                  !question.value("partId", "").empty() && question.value("asked", 0LL) > 0,
              "pending question lost its transcript ownership metadata");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questions").size() == 1,
              "remote snapshot did not expose the pending question");
        const Json crossedAnswer =
            call(handle, {{"op", "answer"}, {"session", otherSession}, {"id", questionId}, {"text", "Wrong owner"}});
        check(!crossedAnswer.at("ok") && crossedAnswer.value("errorCode", "") == "not_found",
              "answer crossed conversation ownership");
        const Json mediaAnswer = call(handle, {{"op", "answer"},
                                               {"session", session},
                                               {"id", questionId},
                                               {"text", "Keep this result"},
                                               {"images", Json::array({"unused"})}});
        check(!mediaAnswer.at("ok") && mediaAnswer.value("errorCode", "") == "invalid_input",
              "question answer silently discarded attached media");
        const int changesBeforeAnswer = mobileSessionChanges.load();
        const Json answerReceipt = call(handle, {{"op", "answer"},
                                                 {"session", session},
                                                 {"id", questionId},
                                                 {"text", "Keep this result"},
                                                 {"requestId", "answer-capture"}});
        check(answerReceipt.at("ok") && answerReceipt.value("question_id", "") == questionId &&
                  !answerReceipt.contains("user_message_id"),
              "question receipt did not identify the accepted question independently of normal prompts");
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

        // 子任务的问答是原工具片段的只读投影，不复制进根会话，也不带出子任务其它正文。
        const std::string childSession = MaiIdGenerator::newSessionId();
        {
            auto saved = makeMaiSqliteStore(workspace.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8());
            check(bool(saved), "child fixture store did not open");
            MaiSession child;
            child.id = childSession;
            child.parentId = session;
            child.depth = 1;
            child.directory = workspace.toUtf8();
            child.model = "test-model";
            child.created = MaiTime::getCurrentTime();
            saved.value()->putSession(child);
        }
        check(call(handle, {{"op", "send"}, {"session", childSession}, {"text", "Private child task"}}).at("ok"),
              "child question turn did not start");
        Json childPending;
        for (int attempt = 0; attempt < 500; ++attempt) {
            childPending = snapshot();
            if (!childPending.at("questions").empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(childPending.at("questions").size() == 1 &&
                  childPending.at("questions").front().at("sessionId") == childSession,
              "root snapshot omitted its child's pending question");
        const Json childQuestion = childPending.at("questions").front();
        const auto inspectProjection = [&](const Json& state, bool answered) {
            check(state.at("questionMessages").size() == 1, "child question was duplicated or omitted");
            const auto& projected = state.at("questionMessages").front();
            check(projected.at("id") == childQuestion.at("messageId") && projected.at("sessionId") == childSession &&
                      projected.at("questionProjection") == true,
                  "child projection invented a message or root owner identity");
            check(projected.at("parts").size() == 1 && projected.at("parts").front().at("tool") == "question",
                  "child projection leaked non-question parts");
            if (answered)
                check(projected.at("parts").front().at("output") == "Keep child result",
                      "completed child answer could not be replayed from its tool result");
            for (const auto& message : state.at("messages"))
                check(message.at("id") != childQuestion.at("messageId"), "child projection polluted root pagination");
        };
        inspectProjection(childPending, false);
        const Json rootRemote = call(handle, {{"op", "remote_snapshot"}, {"session", session}});
        check(rootRemote.at("rootSessionId") == session, "root remote snapshot changed its room owner");
        inspectProjection(rootRemote, false);
        const Json changedChild = call(handle, {{"op", "remote_snapshot"}, {"session", childSession}});
        check(changedChild.at("rootSessionId") == session,
              "child change notification could not resolve the root synchronization room");
        const Json otherProjection = call(handle, {{"op", "snapshot"}, {"session", otherSession}, {"force", true}});
        check(otherProjection.at("questions").empty() && otherProjection.at("questionMessages").empty(),
              "question projection crossed unrelated roots");
        const Json childAnswer = call(
            handle,
            {{"op", "answer"}, {"session", session}, {"id", childQuestion.at("id")}, {"text", "Keep child result"}});
        check(childAnswer.at("ok") && childAnswer.at("question_id") == childQuestion.at("id"),
              "root could not answer its child's pending question");
        Json completedChild;
        for (int attempt = 0; attempt < 500; ++attempt) {
            completedChild = snapshot();
            if (completedChild.at("questions").empty() && !completedChild.at("questionMessages").empty() &&
                completedChild.at("questionMessages").front().at("parts").front().at("state") == "completed")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        inspectProjection(completedChild, true);
        inspectProjection(call(handle, {{"op", "remote_snapshot"}, {"session", session}}), true);
        std::puts("PASS: scoped normal question receipts and child transcript projection without root history copies");

        // 根会话可能空闲而子任务仍在收尾；缓存探针必须等子任务自己的终态，
        // 不能把随后正常的 SessionIdle revision 误当作一次多余读取。
        bool childIdle = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            childIdle = !call(handle, {{"op", "remote_snapshot"}, {"session", childSession}}).at("busy").get<bool>();
            if (childIdle) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(childIdle, "child question task did not finish before the cache probe");
        inspectProjection(call(handle, {{"op", "remote_snapshot"}, {"session", session}}), true);

        // 存储探针不发 Core 事件：同一远端 revision 必须复用小问答窗口，
        // 本机 force snapshot 也不能消费或使远端缓存失效；下一条结构事件才重新读取。
        {
            auto saved = makeMaiSqliteStore(workspace.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8());
            check(bool(saved), "remote question cache fixture store did not open");
            MaiMessage extra;
            extra.id = MaiIdGenerator::newMessageId();
            extra.role = MaiRole::Assistant;
            extra.created = MaiTime::getCurrentTime();
            extra.completed = extra.created;
            MaiToolPart tool;
            tool.tool = "question";
            tool.input = R"({"question":"Cached history probe?"})";
            tool.output = "Probe result";
            tool.state = MaiToolState::Completed;
            extra.parts.push_back({MaiIdGenerator::newPartId(), tool, extra.created});
            saved.value()->putMessage(childSession, extra);
        }
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questionMessages").size() == 1,
              "remote snapshot reread child history during an unchanged structure revision");
        check(snapshot().at("questionMessages").size() == 2, "forced local snapshot missed the storage probe");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questionMessages").size() == 1,
              "local force snapshot invalidated the independent remote question cache");
        check(call(handle, {{"op", "rename"}, {"session", session}, {"title", "Question cache refresh"}}).at("ok"),
              "question cache structure event fixture failed");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("questionMessages").size() == 2,
              "structure event did not refresh remote child question history");
        std::puts("PASS: independent remote question cache, local refresh isolation, structure-event invalidation");

        // 64 个历史 owner 窗口之外仍在等待的原消息必须点取，不能为显示它再扫描
        // 该旧 owner 的其它历史。较晚写入的旧 owner 记录用于区分 owner 窗口和仅消息截断。
        check(
            call(handle, {{"op", "send"}, {"session", childSession}, {"text", "Ask another child question"}}).at("ok"),
            "old child question turn did not start");
        Json oldPending;
        for (int attempt = 0; attempt < 500; ++attempt) {
            oldPending = snapshot();
            if (!oldPending.at("questions").empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(oldPending.at("questions").size() == 1, "old child did not wait before the history window fixture");
        const Json oldQuestion = oldPending.at("questions").front();
        const std::string excludedMessage = MaiIdGenerator::newMessageId();
        {
            auto saved = makeMaiSqliteStore(workspace.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8());
            check(bool(saved), "owner window fixture store did not open");
            const auto created = MaiTime::getCurrentTime();
            for (int index = 0; index < 65; ++index) {
                MaiSession recent;
                recent.id = MaiIdGenerator::newSessionId();
                recent.parentId = session;
                recent.depth = 1;
                recent.directory = workspace.toUtf8();
                recent.created = created + index;
                recent.updated = recent.created;
                saved.value()->putSession(recent);
                MaiMessage message;
                message.id = MaiIdGenerator::newMessageId();
                message.role = MaiRole::Assistant;
                message.created = recent.created;
                message.completed = recent.created;
                MaiToolPart tool;
                tool.tool = "question";
                tool.input = R"({"question":"Historical window question?"})";
                tool.output = "Historical window answer";
                tool.state = MaiToolState::Completed;
                message.parts.push_back({MaiIdGenerator::newPartId(), tool, message.created});
                saved.value()->putMessage(recent.id, message);
            }
            MaiMessage excluded;
            excluded.id = excludedMessage;
            excluded.role = MaiRole::Assistant;
            excluded.created = created + 100;
            excluded.completed = excluded.created;
            MaiToolPart tool;
            tool.tool = "question";
            tool.input = R"({"question":"Outside owner history?"})";
            tool.output = "Outside owner history answer";
            tool.state = MaiToolState::Completed;
            excluded.parts.push_back({MaiIdGenerator::newPartId(), tool, excluded.created});
            saved.value()->putMessage(childSession, excluded);
        }
        const Json windowed = snapshot();
        bool keptPending = false;
        bool leakedOldHistory = false;
        check(windowed.at("questionMessages").size() <= 64, "child question message window exceeded its limit");
        for (const auto& message : windowed.at("questionMessages")) {
            keptPending |= message.at("id") == oldQuestion.at("messageId");
            leakedOldHistory |= message.at("id") == excludedMessage;
        }
        check(keptPending, "old pending question was lost beyond the historical owner window");
        check(!leakedOldHistory, "historical owner window still scanned the old owner's other messages");
        check(call(handle, {{"op", "answer"},
                            {"session", session},
                            {"id", oldQuestion.at("id")},
                            {"text", "Complete old pending question"}})
                  .at("ok"),
              "old pending question was not answerable outside the history window");
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (!call(handle, {{"op", "remote_snapshot"}, {"session", childSession}}).at("busy").get<bool>()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::puts("PASS: 64 historical-owner query window and direct retrieval of older pending questions");

        // 真实 SSE 未关闭时接受 21 条补充，原 assistant 与 question 均被根最新二十条挤出。
        // remote-only 事件点取不能被普通 runtime 查询或本机 snapshot 消费。
        const std::string windowSession = call(handle, {{"op", "create"}}).at("id");
        call(handle, {{"op", "remote_snapshot"}, {"session", windowSession}, {"includeMessageUpdates", true}});
        const Json windowStarted =
            call(handle, {{"op", "send"}, {"session", windowSession}, {"text", "Native old-window stream"}});
        check(windowStarted.at("ok"), "native old-window stream did not start");
        const std::string windowTurn = windowStarted.at("turnId");
        for (int attempt = 0; attempt < 500 && !windowEntered; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(windowEntered, "native old-window SSE did not enter");
        for (int index = 0; index < 21; ++index)
            check(call(handle, {{"op", "steer"},
                                {"session", windowSession},
                                {"turnId", windowTurn},
                                {"requestId", "native-window-" + std::to_string(index)},
                                {"text", "Native accepted supplement " + std::to_string(index)}})
                      .at("ok"),
                  "native old-window supplement was rejected");
        windowEmitTail = true;
        Json oldTailUpdate;
        bool sawNativeTail = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            oldTailUpdate =
                call(handle, {{"op", "remote_snapshot"}, {"session", windowSession}, {"includeMessageUpdates", true}});
            for (const auto& message : oldTailUpdate.at("messages"))
                check(message.at("id") != windowTurn,
                      "native fixture did not move the original assistant outside latest twenty");
            for (const auto& message : oldTailUpdate.value("messageUpdates", Json::array()))
                if (message.at("id") == windowTurn)
                    for (const auto& part : message.at("parts"))
                        sawNativeTail |= part.value("text", "").find("Native live tail") != std::string::npos;
            if (sawNativeTail) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(sawNativeTail, "remote producer omitted live tail of assistant outside latest twenty");
        const auto ordinaryRuntime = call(handle, {{"op", "remote_snapshot"}, {"session", windowSession}});
        check(ordinaryRuntime.value("messageUpdates", Json::array()).empty(),
              "ordinary runtime read consumed producer updates");
        windowReleased = true;
        Json windowPending;
        for (int attempt = 0; attempt < 500; ++attempt) {
            windowPending = call(handle, {{"op", "snapshot"},
                                          {"session", windowSession},
                                          {"force", true},
                                          {"messageLimit", 8},
                                          {"includeMessageUpdates", true}});
            if (!windowPending.at("questions").empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(windowPending.at("questions").size() == 1 && windowPending.at("turnId") == windowTurn,
              "reviewed question lost its original stable task identity");
        const std::string questionMessageID = windowPending.at("questions").front().at("messageId");
        check(!questionMessageID.empty() && questionMessageID != windowTurn,
              "reviewed question did not belong to its real continuation assistant");
        const Json staleAssistant = call(handle, {{"op", "message"}, {"session", windowSession}, {"id", windowTurn}});
        bool staleCanceled = false;
        for (const auto& part : staleAssistant.at("messages").front().at("parts"))
            staleCanceled |= part.value("kind", "") == "tool" && part.value("tool", "") == "question" &&
                             part.value("state", "") == "canceled" &&
                             part.value("error", "").find("new user input arrived") != std::string::npos;
        check(staleCanceled, "the pre-steering question plan was executed without reviewing new inputs");
        // 只有已经实际 Running 的问题才在新补充到达后继续等待。推到最新页外后，
        // 仍必须点取同一条 question owner，而非把问题错误归到本轮最初 assistant。
        for (int index = 0; index < 21; ++index)
            check(call(handle, {{"op", "steer"},
                                {"session", windowSession},
                                {"turnId", windowTurn},
                                {"requestId", "native-question-window-" + std::to_string(index)},
                                {"text", "Native question supplement " + std::to_string(index)}})
                      .at("ok"),
                  "running question supplement was rejected");
        windowPending = call(handle, {{"op", "snapshot"},
                                      {"session", windowSession},
                                      {"force", true},
                                      {"messageLimit", 8},
                                      {"includeMessageUpdates", true}});
        check(windowPending.at("questions").size() == 1 &&
                  windowPending.at("questions").front().at("messageId") == questionMessageID,
              "question did not remain attached to its actual assistant outside the latest page");
        for (const auto& message : windowPending.at("messages"))
            check(message.at("id") != questionMessageID,
                  "running question fixture did not move its owner outside the latest page");
        check(call(handle, {{"op", "answer"},
                            {"session", windowSession},
                            {"id", windowPending.at("questions").front().at("id")},
                            {"text", "Kept native old result"}})
                  .at("ok"),
              "native old-window question answer failed");
        bool sawNativeFinal = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            call(handle, {{"op", "snapshot"},
                          {"session", windowSession},
                          {"force", true},
                          {"messageLimit", 8},
                          {"includeMessageUpdates", true}});
            const auto remote =
                call(handle, {{"op", "remote_snapshot"}, {"session", windowSession}, {"includeMessageUpdates", true}});
            check(remote.value("messageUpdates", Json::array()).size() <= 20,
                  "remote owner update batch exceeded twenty");
            for (const auto& message : remote.value("messageUpdates", Json::array()))
                if (message.at("id") == questionMessageID && message.value("completed", 0LL) > 0)
                    for (const auto& part : message.at("parts"))
                        sawNativeFinal |= part.value("output", "") == "Kept native old result";
            if (sawNativeFinal) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(sawNativeFinal,
              "remote producer lost the old assistant's final question answer after local update consumption");
        bool windowIdle = false;
        for (int attempt = 0; attempt < 500; ++attempt) {
            windowIdle = !call(handle, {{"op", "remote_snapshot"}, {"session", windowSession}}).at("busy").get<bool>();
            if (windowIdle) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(windowIdle, "native old-window task did not finish before the local configuration gate");
        std::puts(
            "PASS: remote-only old-owner live tail, question completion, opt-in isolation, terminal point updates");

        // 配置检查不启动推理；真实 GGUF 生成与工具闭环由本地主模型专测/正常 App 另验。
        // 这里独立证明参数不回绕、损坏文件不覆盖配置，失败不偷偷降级到 HTTP。
        const int assetProbesBeforeLocal = host.assetProbes.load();
        std::size_t requestsBeforeLocal;
        {
            std::lock_guard<std::mutex> lock(requestsMutex);
            requestsBeforeLocal = requests.size();
        }
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
            check(requests.size() == requestsBeforeLocal,
                  "local configuration unexpectedly sent a remote model request");
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
    windowReleased = true;
    maiMobileAgentDestroy(handle);
    server.stop();
    listener.join();
    MaiFileSystem::removeRecursively(workspace);
    if (!host.destroyed.load()) return 1;
    return status;
}
