// 文件作用：真实 C ABI + loopback 验证会话模型/推理等级、双供应商、原子失败和重启。
// 合成账号/模型响应与独立临时库，不读取用户数据库、真实凭据或付费供应商。
#include "MaiMobileAgent.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <json.hpp>
#include <sqlite3.h>

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
    char* raw = maiMobileAgentRequest(handle, request.dump().c_str());
    check(raw != nullptr, "C ABI returned no response");
    const auto result = Json::parse(raw, nullptr, false);
    maiMobileAgentFree(raw);
    check(result.is_object(), "C ABI returned malformed JSON");
    return result;
}
struct Handle {
    void* value = maiMobileAgentCreate();
    ~Handle() {
        maiMobileAgentDestroy(value);
    }
};
struct Keys {
    bool glmAvailable = true;
};
const char* keyHandler(void* context, const char* name, const char*) {
    auto& keys = *static_cast<Keys*>(context);
    Json result;
    if (std::strcmp(name, "glm_api_key") == 0)
        result = {{"ok", true}, {"output", Json{{"key", keys.glmAvailable ? "synthetic-glm-key" : ""}}.dump()}};
    else if (std::strcmp(name, "deepseek_api_key") == 0)
        result = {{"ok", true}, {"output", Json{{"key", "synthetic-deepseek-key"}}.dump()}};
    else
        result = {{"ok", false}, {"error", "Synthetic optional host capability is unavailable."}};
    const auto encoded = result.dump();
    auto* copied = static_cast<char*>(std::malloc(encoded.size() + 1));
    if (copied) std::memcpy(copied, encoded.c_str(), encoded.size() + 1);
    return copied;
}
void freeResponse(void*, const char* response) {
    std::free(const_cast<char*>(response));
}
void releaseKeys(void*) {}

struct Provider {
    httplib::Server server;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Json> requests;
    bool holdNext = false, entered = false, released = false;
    std::string url;
    explicit Provider(bool glm) {
        server.new_task_queue = [] { return new httplib::ThreadPool(2); };
        server.Post(glm ? "/chat/completions" : "/responses", [this, glm](const auto& request, auto& response) {
            const auto body = Json::parse(request.body, nullptr, false);
            std::unique_lock<std::mutex> lock(mutex);
            requests.push_back(body);
            const bool hold = holdNext;
            holdNext = false;
            if (hold) {
                entered = true;
                wake.notify_all();
                if (!wake.wait_for(lock, std::chrono::seconds(15), [&] { return released; })) {
                    response.status = 500;
                    response.set_content("{}", "application/json");
                    return;
                }
            }
            lock.unlock();
            if (request.get_header_value("Authorization") !=
                (glm ? "Bearer synthetic-glm-key" : "Bearer synthetic-deepseek-key")) {
                response.status = 401;
                response.set_content("{}", "application/json");
                return;
            }
            response.set_content(
                glm ? "data: {\"choices\":[{\"delta\":{\"content\":\"Owned GLM "
                      "reply\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"
                    : "data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"delta\":\"Owned DeepSeek "
                      "reply\"}\n\ndata: "
                      "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[]}}\n\n",
                "text/event-stream");
        });
        const int port = server.bind_to_any_port("127.0.0.1");
        check(port > 0, "Could not bind owned provider");
        url = "http://127.0.0.1:" + std::to_string(port);
        worker = std::thread([this] {
            MaiThread::setCurrentName("mai-session-provider");
            server.listen_after_bind();
        });
        for (int index = 0; index < 200 && !server.is_running(); ++index)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(server.is_running(), "Owned provider did not start");
    }
    ~Provider() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
            wake.notify_all();
        }
        server.stop();
        if (worker.joinable()) worker.join();
    }
};

void waitIdle(void* handle, const std::string& id) {
    for (int index = 0; index < 500; ++index) {
        const auto snapshot = call(handle, {{"op", "snapshot"}, {"session", id}, {"messageLimit", 0}, {"force", true}});
        if (snapshot.value("ok", false)) {
            for (const auto& session : snapshot["sessions"])
                if (session["id"] == id && !session["busy"].get<bool>()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Synthetic session did not become idle");
}

Json config(const MaiFilePath& root, const Provider& glm, const Provider& deepseek, const std::string& model) {
    return {{"op", "configure"},
            {"database", root.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8()},
            {"workspace", root.toUtf8()},
            {"appRoot", root.toUtf8()},
            {"baseUrl", model == "glm-5.3" ? glm.url : deepseek.url},
            {"glmBaseUrl", glm.url},
            {"deepseekBaseUrl", deepseek.url},
            {"apiKey", model == "glm-5.3" ? "synthetic-glm-key" : "synthetic-deepseek-key"},
            {"model", model},
            {"wire", model == "glm-5.3" ? "chat_completions" : "responses"},
            {"reasoningEffort", "high"},
            {"policy", "never"},
            {"localModelName", "local:owned-placeholder"}};
}

std::string storedModel(const MaiFilePath& root, const std::string& id) {
    sqlite3* database = nullptr;
    const auto path = root.append(MaiFilePath::fromUtf8("sessions.sqlite")).toUtf8();
    check(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
          "Cannot read owned database");
    sqlite3_stmt* query = nullptr;
    check(sqlite3_prepare_v2(database, "SELECT model FROM sessions WHERE id=?1", -1, &query, nullptr) == SQLITE_OK,
          "Cannot read owned model");
    sqlite3_bind_text(query, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    std::string model;
    if (sqlite3_step(query) == SQLITE_ROW) model = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    sqlite3_finalize(query);
    sqlite3_close(database);
    return model;
}

void run(bool legacyOnly) {
    Provider glm(true), deepseek(false);
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mobile-session-models-")));
    check(!MaiFileSystem::createDirectories(root), "Cannot create owned workspace");
    Keys keys;
    Handle handle;
    check(handle.value != nullptr, "Cannot create native Agent");
    check(maiMobileAgentSetHostToolHandler(handle.value, &keys, keyHandler, freeResponse, releaseKeys) == 1,
          "Cannot bind owned provider keys");
    check(call(handle.value, config(root, glm, deepseek, "glm-5.3"))["ok"], "Cannot initialize GLM fixture");
    const auto a = call(handle.value, {{"op", "create"}}).at("id").get<std::string>();
    check(storedModel(root, a) == "glm-5.3", "Initial GLM session was not persisted");
    check(call(handle.value, config(root, glm, deepseek, "deepseek-flash"))["ok"], "Cannot reinitialize host default");
    const auto b = call(handle.value, {{"op", "create"}}).at("id").get<std::string>();
    check(call(handle.value, {{"op", "send"}, {"session", a}, {"text", "Preserve this existing model"}})["ok"],
          "Cannot send owned GLM prompt");
    waitIdle(handle.value, a);
    check(storedModel(root, a) == "glm-5.3", "send overwrote an existing session model with the global default");
    {
        std::lock_guard<std::mutex> lock(glm.mutex);
        check(!glm.requests.empty() && glm.requests.back()["model"] == "glm-5.3", "GLM prompt went to the wrong route");
    }
    if (legacyOnly) {
        std::printf("Legacy overwrite regression passed\n");
        return;
    }
    check(call(handle.value, {{"op", "send"}, {"session", b}, {"text", "Use the default Flash model"}})["ok"],
          "Cannot send owned DeepSeek Flash prompt");
    waitIdle(handle.value, b);
    {
        std::lock_guard<std::mutex> lock(deepseek.mutex);
        check(deepseek.requests.size() == 1 && deepseek.requests.back()["model"] == "deepseek-flash" &&
                  deepseek.requests.back()["reasoning"]["effort"] == "high",
              "Default Flash route or effort is wrong");
        deepseek.requests.clear();
    }
    const auto select = [&](const std::string& id, const std::string& model, const Json& effort) {
        return call(handle.value,
                    {{"op", "set_model"}, {"session", id}, {"model", model}, {"reasoningEffort", effort}});
    };
    check(select(a, "glm-5.3", "low")["ok"], "Cannot select session A effort");
    check(select(b, "deepseek-v4-pro", "max")["ok"], "Cannot select DeepSeek Pro");
    check(call(handle.value, {{"op", "send"}, {"session", a}, {"text", "Use GLM low effort"}})["ok"],
          "Cannot send GLM low effort");
    waitIdle(handle.value, a);
    {
        std::lock_guard<std::mutex> lock(glm.mutex);
        check(glm.requests.back()["model"] == "glm-5.3" && glm.requests.back()["reasoning_effort"] == "low",
              "GLM low effort was not serialized");
    }

    check(call(handle.value, {{"op", "get_session"}, {"session", a}})["wire"] == "chat_completions",
          "GLM wire is incorrect");
    check(call(handle.value, {{"op", "get_session"}, {"session", b}})["wire"] == "responses",
          "DeepSeek wire is incorrect");
    for (const auto& bad : {select(a, "glm-5.3-flash", "medium"), select(a, "glm-unknown", "max"),
                            select(a, "glm-5.3-flash", Json(nullptr))})
        check(!bad["ok"].get<bool>(), "Invalid model or effort was accepted");
    auto choice = call(handle.value, {{"op", "get_session"}, {"session", a}});
    check(choice["model"] == "glm-5.3" && choice["reasoningEffort"] == "low",
          "Invalid update partially changed session A");
    keys.glmAvailable = false;
    check(!select(b, "glm-5.3-flash", "high")["ok"].get<bool>(), "Missing provider credentials changed the model");
    keys.glmAvailable = true;
    check(storedModel(root, b) == "deepseek-v4-pro", "Missing credentials partially changed session B");
    check(!select(a, "local:owned-placeholder", "high")["ok"].get<bool>(), "Unprepared local slot was selected");
    {
        std::lock_guard<std::mutex> lock(deepseek.mutex);
        deepseek.holdNext = true;
    }
    check(call(handle.value, {{"op", "send"}, {"session", b}, {"text", "Hold this owned request"}})["ok"],
          "Cannot start session B");
    {
        std::unique_lock<std::mutex> lock(deepseek.mutex);
        check(deepseek.wake.wait_for(lock, std::chrono::seconds(10), [&] { return deepseek.entered; }),
              "Session B never reached provider");
    }
    const auto busy = select(b, "glm-5.3", "low");
    check(!busy["ok"].get<bool>() && busy["errorCode"] == "busy", "Busy session changed configuration");
    check(select(a, "glm-5.3-flash", "high")["ok"], "Other idle session was blocked by session B");
    check(call(handle.value, {{"op", "send"}, {"session", a}, {"text", "Use GLM Flash"}})["ok"],
          "Cannot send parallel GLM Flash prompt");
    waitIdle(handle.value, a);
    {
        std::lock_guard<std::mutex> lock(glm.mutex);
        check(glm.requests.back()["model"] == "glm-5.3-flash" && glm.requests.back()["reasoning_effort"] == "high",
              "Parallel GLM config crossed sessions");
    }
    check(call(handle.value, {{"op", "enqueue"},
                              {"session", b},
                              {"requestId", "owned-queued-config"},
                              {"text", "Keep Pro and max"}})["ok"],
          "Cannot enqueue owned Pro prompt");
    {
        std::lock_guard<std::mutex> lock(deepseek.mutex);
        deepseek.released = true;
        deepseek.wake.notify_all();
    }
    waitIdle(handle.value, b);
    {
        std::lock_guard<std::mutex> lock(deepseek.mutex);
        for (const auto& request : deepseek.requests)
            check(request["model"] == "deepseek-v4-pro" && request["reasoning"]["effort"] == "max",
                  "Queued request used a global model or effort");
    }
    const auto rejected =
        call(handle.value, {{"op", "send"},
                            {"session", b},
                            {"text", "New image"},
                            {"images", Json::array({{{"path", "owned-missing.png"}, {"mimeType", "image/png"}}})}});
    check(!rejected["ok"].get<bool>() && rejected["errorCode"] == "not_supported",
          "Text-only Pro accepted an image input");
    check(call(handle.value, config(root, glm, deepseek, "deepseek-flash"))["ok"], "Cannot reopen owned database");
    choice = call(handle.value, {{"op", "get_session"}, {"session", a}});
    check(choice["model"] == "glm-5.3-flash" && choice["reasoningEffort"] == "high",
          "Session A choice did not survive reopen");
    choice = call(handle.value, {{"op", "get_session"}, {"session", b}});
    check(choice["model"] == "deepseek-v4-pro" && choice["reasoningEffort"] == "max",
          "Session B choice did not survive reopen");
    check(call(handle.value, {{"op", "set_model"}, {"session", a}, {"model", "glm-5.3"}})["ok"],
          "Optional effort was rejected");
    check(call(handle.value, {{"op", "get_session"}, {"session", a}})["reasoningEffort"] == "high",
          "Omitted effort overwrote selection");
    check(select(a, "glm-5.3", "")["ok"], "Cannot restore inherited effort");
    choice = call(handle.value, {{"op", "get_session"}, {"session", a}});
    check(choice["reasoningEffort"] == "" && choice["defaultReasoningEffort"] == "high",
          "Inherited effort lost its explicit meaning");
    std::printf(
        "Mobile session models: route, wire, effort, atomic reject, busy isolation, enqueue, image policy and reopen "
        "passed\n");
}
}  // namespace

int main(int argc, char** argv) {
    MaiThread::setCurrentName("mai-mobile-model-tests");
#if defined(_WIN32)
    _putenv_s("NO_PROXY", "*");
#else
    setenv("NO_PROXY", "*", 1);
    setenv("no_proxy", "*", 1);
#endif
    try {
        run(argc == 2 && std::string(argv[1]) == "--legacy-overwrite");
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
