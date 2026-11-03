// 文件作用：通过真实移动 C ABI 验证运行中补充输入的模型读取与工具批次边界。
// 所有账号资料、模型与工具均为本进程合成数据；网络只使用环回假模型端点。
#include "MaiMobileAgent.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
    std::atomic<bool> destroyed{false};
};

const char* hostTool(void* context, const char* name, const char*) {
    auto& host = *static_cast<HeldHost*>(context);
    if (std::strcmp(name, "mobile_ark_service_config") == 0) {
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
        const std::string session = call(handle, {{"op", "create"}}).at("id");
        const Json started =
            call(handle, {{"op", "send"}, {"session", session}, {"text", "Improve the current result"}});
        check(started.at("ok"), "send failed");
        const std::string turn = started.at("id");
        {
            std::unique_lock<std::mutex> lock(host.mutex);
            check(host.wake.wait_for(lock, std::chrono::seconds(5), [&] { return host.entered; }),
                  "tool did not reach the synthetic host");
        }
        const auto snapshot = [&] { return call(handle, {{"op", "snapshot"}, {"session", session}, {"force", true}}); };
        check(snapshot().at("turnId") == turn, "snapshot changed the stable turn identity");
        check(call(handle, {{"op", "remote_snapshot"}, {"session", session}}).at("turnId") == turn,
              "remote snapshot lost the target turn identity");
        check(!call(handle, {{"op", "send"}, {"session", session}, {"text", "Busy send"}}).at("ok"),
              "ordinary send no longer rejects busy work");
        const Json steer{{"op", "steer"},
                         {"session", session},
                         {"turnId", turn},
                         {"requestId", "synthetic-steering-one"},
                         {"text", "Show the current result first"}};
        const Json accepted = call(handle, steer);
        check(accepted.at("ok"), "steering was not accepted");
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
        check(call(handle, steer).at("id") == accepted.at("id"), "completed-turn retry lost its receipt");
        const std::string acceptedUserId = accepted.at("id").get<std::string>();
        const Json receiptPage = call(
            handle, {{"op", "messages_page"}, {"session", session}, {"before", acceptedUserId + "~"}, {"limit", 1}});
        check(receiptPage.at("ok") && receiptPage.at("messages").size() == 1 &&
                  receiptPage.at("messages")[0].at("id") == acceptedUserId &&
                  receiptPage.at("messages")[0].at("role") == "user",
              "exact receipt cursor lost the accepted user behind newer assistant messages");
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
            check(
                messages[last - 1].at("role") == "tool" && messages[last - 1].at("tool_call_id") == "synthetic-profile",
                "user input split a tool call/result pair");
            check(messages[last - 2].at("role") == "assistant" &&
                      messages[last - 2].at("tool_calls")[0].at("id") == "synthetic-profile",
                  "tool invocation was not preserved");
        }
        check(host.calls.load() == 1, "steering restarted the completed host operation");
        std::puts(
            "PASS: real C ABI steering, persisted user, dedupe, stable turn, complete tool results, next model input");
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
