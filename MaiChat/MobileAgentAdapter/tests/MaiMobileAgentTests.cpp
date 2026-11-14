// 文件作用：移动端 C++ Agent 适配层；MaiMobileAgentTests 负责行为与回归验证，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：freeHostToolResponse、releaseHostToolContext。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h、MaiIdGenerator.h、MaiMobileAgent.h；实现与这些接口协作。
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMobileAgent.h"
#include "MaiMobileHostTools.h"
#include "MaiImageFilter.h"
#include "MaiSqliteStore.h"
#include "MaiToolOutput.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <httplib.h>
#include <json.hpp>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <thread>
#include <utility>
#if defined(__APPLE__)
#include "MaiScreenFrameArchive.h"
#include "MaiQrCode.h"
#endif
#if !defined(_WIN32)
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
using Json = nlohmann::json;
#if defined(__APPLE__)
static const char* kPhotoAlbumTool = "mobile_photos_add_to_album";
#elif defined(__ANDROID__)
static const char* kPhotoAlbumTool = "mobile_photos_copy_to_album";
#else
static const char* kPhotoAlbumTool = nullptr;
#endif
static const char* kCameraTool = "mobile_take_photo";
static const char* kCameraArguments = "{}";
#define CHECK(x)                                                      \
    do {                                                              \
        if (!(x)) {                                                   \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",     \
                         __FILE__, __LINE__, #x);                    \
            throw std::runtime_error(#x);                            \
        }                                                             \
    } while (0)
static Json call(void* agent, const Json& value) {
    char* response = maiMobileAgentRequest(agent, value.dump().c_str());
    CHECK(response);
    auto parsed = Json::parse(response);
    maiMobileAgentFree(response);
    return parsed;
}
static Json callSessionIMControl(const Json& value) {
    char* response = maiMobileSessionIMControl(value.dump().c_str());
    CHECK(response);
    const auto parsed = Json::parse(response);
    maiMobileAgentFree(response);
    return parsed;
}
static Json callIMHistorySnapshot(const Json& value) {
    char* response = maiMobileIMHistorySnapshot(value.dump().c_str());
    CHECK(response);
    const auto parsed = Json::parse(response);
    maiMobileAgentFree(response);
    return parsed;
}
struct HostToolProbe {
    std::atomic<int> calls{0};
    std::atomic<bool> released{false};
    std::string lastTool;
    std::string lastArguments;
};
static const char* hostToolHandler(void* context, const char* tool, const char* arguments) {
    auto* probe = static_cast<HostToolProbe*>(context);
    probe->lastTool = tool ? tool : "";
    probe->lastArguments = arguments ? arguments : "";
    ++probe->calls;
    const std::string response =
        Json{{"ok", true},
             {"output", probe->lastTool == "maichat_send_text" ? "sent"
                        : probe->lastTool == "mobile_decode_text" ? "\u4f60\u597d" : "file_read"}}
            .dump();
    char* owned = static_cast<char*>(std::malloc(response.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, response.c_str(), response.size() + 1);
    return owned;
}
static void freeHostToolResponse(void*, const char* response) {
    std::free(const_cast<char*>(response));
}
static void releaseHostToolContext(void* context) {
    static_cast<HostToolProbe*>(context)->released = true;
}

// 数据库查询不执行媒体导出；取消前不能进入宿主，取消后不能把旧账号结果当成功交回模型。
// 宿主内部查询的实时中断由平台 reader deadline 负责，这里只验证共用转发边界。
static void messageHistoryHostContract() {
    struct Probe {
        int calls = 0;
        bool cancelInHost = false;
        std::atomic<bool> canceled{false};
    } probe;
    const auto handler = [](void* raw, const char*, const char*) -> const char* {
        auto& state = *static_cast<Probe*>(raw);
        ++state.calls;
        if (state.cancelInHost) state.canceled = true;
        const std::string response = R"({"ok":true,"output":{"count":0,"has_more":false,"coverage":"local_persisted"}})";
        char* owned = static_cast<char*>(std::malloc(response.size() + 1));
        CHECK(owned != nullptr);
        std::memcpy(owned, response.c_str(), response.size() + 1);
        return owned;
    };
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, handler, freeHostToolResponse,
                                      [](void*) {}));
    MaiToolRegistry registry;
    registerMaiChatHostTools(registry, dispatcher);
    MaiToolContext context;
    context.cancel = &probe.canceled;
    for (const char* name : {"maichat_search_messages", "maichat_get_messages"}) {
        MaiTool* tool = registry.findForModel(name);
        CHECK(tool != nullptr);
        const Json schema = Json::parse(tool->parametersSchema());
        CHECK(schema.value("additionalProperties", true) == false);
        for (const char* field : {"peer_id", "limit", "kind", "direction", "since_ms",
                                  "until_ms", "cursor"})
            CHECK(schema.at("properties").contains(field));
        CHECK(schema["properties"]["limit"]["maximum"] == 200);
        const bool search = std::strcmp(name, "maichat_search_messages") == 0;
        CHECK(schema["properties"].contains("query") == search);
        const std::string arguments = search ? R"({"query":"owned"})" : R"({"peer_id":"owned-peer"})";
        probe.canceled = true;
        probe.cancelInHost = false;
        const int before = probe.calls;
        auto result = tool->execute(arguments, context);
        CHECK(result.error().code() == MaiErrorCode::Canceled);
        CHECK(probe.calls == before);
        CHECK(result.execution().has_value());
        CHECK(result.execution()->effectStatus == MaiToolEffectStatus::None);
        CHECK(result.execution()->started == false);

        probe.canceled = false;
        probe.cancelInHost = true;
        result = tool->execute(arguments, context);
        CHECK(probe.calls == before + 1);
        CHECK(result.error().code() == MaiErrorCode::Canceled);
        CHECK(result.execution().has_value());
        CHECK(result.execution()->canceled);
        CHECK(result.execution()->started == true);
        CHECK(result.execution()->effectStatus == MaiToolEffectStatus::None);

        probe.canceled = false;
        probe.cancelInHost = false;
        result = tool->execute(arguments, context);
        CHECK(!result.hasError());
        CHECK(Json::parse(result.output())["coverage"] == "local_persisted");
        CHECK(result.execution().has_value());
        CHECK(result.execution()->status == MaiToolExecutionStatus::Completed);
        CHECK(result.execution()->effectStatus == MaiToolEffectStatus::None);
    }
    clearMaiMobileHostToolHandler(dispatcher);
    std::puts("PASS: persisted message schemas and pre/post host cancellation");
}

// 相册 ID 不能冒充已知工作区文件；只测普通校验和分发，不访问系统相册。
static void libraryExportDomainContract() {
    const auto root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mobile_export_domain_")));
    const auto workspace = root.append(MaiFilePath::fromUtf8("Workspace"));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const auto source = workspace.append(MaiFilePath::fromUtf8("portrait-source.jpg"));
    const auto outside = root.append(MaiFilePath::fromUtf8("outside.jpg"));
    CHECK(!MaiFileSystem::writeFile(source, "owned workspace fixture"));
    CHECK(!MaiFileSystem::writeFile(outside, "owned outside fixture"));
    CHECK(!MaiFileSystem::createDirectories(
        workspace.append(MaiFilePath::fromUtf8("directory.jpg"))));
    HostToolProbe probe;
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, hostToolHandler, freeHostToolResponse,
                                      releaseHostToolContext));
    MaiToolRegistry registry;
    registerMobilePhotoTools(registry, dispatcher);
    MaiToolContext context;
    context.root = workspace.toUtf8();
    context.sessionId = "owned-library-domain-test";
    for (const char* name : {"mobile_export_photo_original", "mobile_export_media_original"}) {
        auto* tool = registry.find(name);
        CHECK(tool);
        const int before = probe.calls.load();
        const auto invalid = tool->execute(R"({"id":"portrait-source.jpg"})", context);
        CHECK(invalid.hasError() && invalid.error().code() == MaiErrorCode::InvalidInput);
        CHECK(probe.calls.load() == before);
        CHECK(invalid.error().message().find("same exact path") != std::string::npos);
        CHECK(invalid.error().message().find("permission") != std::string::npos);
        for (const char* id :
             {"OPAQUE-ASSET-UUID/L0/001", "absent.jpg", "directory.jpg", "../outside.jpg"}) {
            const auto raw = Json{{"id", id}, {"component", "photo"}}.dump();
            const int calls = probe.calls.load();
            CHECK(!tool->execute(raw, context).hasError());
            CHECK(probe.calls.load() == calls + 1);
            CHECK(probe.lastTool == name && probe.lastArguments == raw);
        }
    }
    std::string bytes;
    CHECK(!MaiFileSystem::readFile(source, bytes) && bytes == "owned workspace fixture");
    CHECK(!MaiFileSystem::readFile(outside, bytes) && bytes == "owned outside fixture");
    clearMaiMobileHostToolHandler(dispatcher);
    CHECK(probe.released.load());
    MaiFileSystem::removeRecursively(root);
    std::puts("PASS: library IDs reject existing workspace files before host dispatch");
}

struct BrowserToolProbe {
    int calls = 0;
    bool released = false;
    bool rejectObservedURL = false;
    Json arguments;
};

static const char* browserToolHandler(void* context, const char* tool, const char* arguments) {
    auto& probe = *static_cast<BrowserToolProbe*>(context);
    CHECK(tool && std::strcmp(tool, "mobile_browser") == 0);
    probe.arguments = Json::parse(arguments);
    ++probe.calls;
    Json response;
    if (probe.rejectObservedURL) {
        response = {{"ok", false},
                    {"errorCode", "invalid_input"},
                    {"error", "the observed URL no longer matches the current browser page"}};
    } else {
        if (probe.arguments.at("action") == "screenshot") {
            CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(probe.arguments.at("output_path").get<std::string>()),
                                            std::string("\x89PNG\r\n\x1a\n", 8))
                       .hasError());
        }
        response = {{"ok", true}, {"output", {{"action", probe.arguments.at("action")}}}};
    }
    const std::string bytes = response.dump();
    auto* owned = static_cast<char*>(std::malloc(bytes.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, bytes.c_str(), bytes.size() + 1);
    return owned;
}

static void releaseBrowserToolContext(void* context) {
    static_cast<BrowserToolProbe*>(context)->released = true;
}

// The fake host records the ordinary flat API and produces only a temporary PNG.
// It never opens a browser; URL freshness errors still come from the host boundary.
static void browserContract() {
    struct Workspace {
        MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-browser-test-" + MaiIdGenerator::newEventId()));
        ~Workspace() {
            MaiFileSystem::removeRecursively(path);
        }
    } workspace;
    CHECK(!MaiFileSystem::createDirectories(workspace.path).hasError());
    BrowserToolProbe probe;
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, browserToolHandler, freeHostToolResponse,
                                      releaseBrowserToolContext));
    MaiToolRegistry registry;
    registerMobileBrowserTools(registry, dispatcher);
    MaiTool* browser = registry.findForModel("mobile_browser");
    CHECK(browser != nullptr);
    const Json schema = Json::parse(browser->parametersSchema());
    CHECK(schema.at("required") == Json::array({"request"}) && schema.at("additionalProperties") == false);
    CHECK(schema.at("properties").size() == 1);
    const Json variants = schema.at("properties").at("request").at("oneOf");
    CHECK(variants.size() == 6);
    for (const Json& variant : variants) {
        CHECK(variant.at("additionalProperties") == false);
        const Json& properties = variant.at("properties");
        const std::string action = properties.at("action").at("enum").at(0);
        if (action == "open") {
            CHECK(variant.at("required") == Json::array({"action", "url"}));
            CHECK(!properties.contains("wait_ms"));
        } else if (action == "screenshot") {
            CHECK(variant.at("required") == Json::array({"action", "expected_url"}));
        } else if (action != "observe") {
            CHECK(!properties.contains("wait_ms"));
        }
    }
    for (const char* action : {"open", "screenshot", "click", "type"}) {
        const std::string request = Json{{"request", {{"action", action}}}}.dump();
        CHECK(browser->requiresApproval(request) && browser->requiresPerCallApproval(request));
    }
    CHECK(!browser->requiresApproval(R"({"request":{"action":"observe"}})"));
    CHECK(!browser->requiresPerCallApproval(R"({"request":{"action":"close"}})"));
    CHECK(browser->requiresPerCallApproval(R"({"action":"observe"})"));
    MaiToolContext context;
    context.root = workspace.path.toUtf8();
    context.sessionId = "ses_browser";
    context.messageId = "msg_browser_open";
    context.partId = "part_browser_open";
    std::atomic<bool> canceled{false};
    context.cancel = &canceled;
    for (
        const char* invalid :
        {R"({"action":"open","url":"https://example.com"})",
         R"({"request":{"action":"open","url":"https://example.com"},"session_id":"forged"})",
         R"({"request":{"action":"open","url":"https://example.com","wait_ms":5000}})",
         R"({"request":{"action":"open","url":"file:///private/test"}})",
         R"({"request":{"action":"open","url":"https://user:password@example.com"}})",
         R"({"request":{"action":"open","url":"https://example.com","message_id":"forged"}})",
         R"({"request":{"action":"screenshot"}})",
         R"({"request":{"action":"click","snapshot_id":"s","element_id":"e","expected_label":"go"}})",
         R"({"request":{"action":"type","snapshot_id":"s","element_id":"e","expected_label":"go","expected_url":"https://example.com"}})",
         R"({"request":{"action":"close","expected_url":"https://example.com"}})",
         R"({"request":{"action":"observe","wait_ms":10001}})"}) {
        const auto rejected = browser->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(probe.calls == 0);
    CHECK(!browser->execute(R"({"request":{"action":"open","url":"https://example.com"}})", context).hasError());
    CHECK(probe.arguments == Json({{"action", "open"},
                                   {"url", "https://example.com"},
                                   {"session_id", context.sessionId},
                                   {"message_id", context.messageId},
                                   {"part_id", context.partId}}));
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    CHECK(!browser->execute(R"({"request":{"action":"observe","wait_ms":5000}})", context).hasError());
    CHECK(probe.arguments == Json({{"action", "observe"}, {"wait_ms", 5000}, {"session_id", context.sessionId}}));
#else
    CHECK(browser->execute(R"({"request":{"action":"observe","wait_ms":5000}})", context).hasError());
    CHECK(!browser->execute(R"({"request":{"action":"observe"}})", context).hasError());
#endif
    const Json target{{"action", "click"},
                      {"snapshot_id", "snapshot-current"},
                      {"element_id", "element-current"},
                      {"expected_label", "Continue"},
                      {"expected_url", "https://example.com/current"}};
    CHECK(!browser->execute(Json{{"request", target}}.dump(), context).hasError());
    CHECK(probe.arguments.at("expected_url") == target.at("expected_url") &&
          probe.arguments.at("session_id") == context.sessionId && !probe.arguments.contains("request"));
    Json typing = target;
    typing["action"] = "type";
    typing["text"] = "";
    CHECK(!browser->execute(Json{{"request", typing}}.dump(), context).hasError());
    CHECK(probe.arguments.at("text") == "");
    typing["text"] = std::string(4001, 'x');
    CHECK(browser->execute(Json{{"request", typing}}.dump(), context).hasError());
    const auto screenshot = browser->execute(
        R"({"request":{"action":"screenshot","expected_url":"https://example.com/current"}})", context);
    CHECK(!screenshot.hasError() && screenshot.images().size() == 1);
    CHECK(probe.arguments.at("expected_url") == target.at("expected_url") &&
          probe.arguments.at("session_id") == context.sessionId && probe.arguments.contains("output_path"));
    CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(screenshot.images().at(0).path)));
    CHECK(!browser->execute(R"({"request":{"action":"close"}})", context).hasError());
    const int validCalls = probe.calls;
    canceled.store(true);
    const auto canceledResult = browser->execute(R"({"request":{"action":"observe"}})", context);
    CHECK(canceledResult.hasError() && canceledResult.error().code() == MaiErrorCode::Canceled);
    canceled.store(false);
    context.sessionId.clear();
    CHECK(browser->execute(R"({"request":{"action":"observe"}})", context).hasError());
    context.sessionId = "ses_browser";
    context.root.clear();
    CHECK(browser->execute(R"({"request":{"action":"observe"}})", context).hasError());
    CHECK(probe.calls == validCalls);
    context.root = workspace.path.toUtf8();
    probe.rejectObservedURL = true;
    const auto staleURL = browser->execute(Json{{"request", target}}.dump(), context);
    CHECK(staleURL.hasError() && staleURL.error().code() == MaiErrorCode::InvalidInput);
    CHECK(staleURL.error().message().find("observed URL") != std::string::npos);
    clearMaiMobileHostToolHandler(dispatcher);
    CHECK(probe.released);
    std::puts("PASS: browser action schema, approval, trusted context, URL guards and flat host dispatch");
}

// Exercise the actual registered tools without invoking camera hardware. Opening
// an archived image must never dispatch a camera preview or a manual capture.
static void cameraRoutingContract() {
    HostToolProbe probe;
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(
        setMaiMobileHostToolHandler(dispatcher, &probe, hostToolHandler, freeHostToolResponse, releaseHostToolContext));
    MaiToolRegistry registry;
    registerMobilePhotoTools(registry, dispatcher);
    MaiTool* photo = registry.findForModel("mobile_take_photo");
    CHECK(photo != nullptr);
    const Json schema = Json::parse(photo->parametersSchema());
    CHECK(schema.at("properties").empty() && schema.at("additionalProperties") == false);
    CHECK(!photo->requiresApproval("{}"));
    MaiToolContext context;
    context.root = "/camera-routing-contract-workspace";
    context.sessionId = "camera-routing-contract-session";
    std::atomic<bool> canceled{false};
    context.cancel = &canceled;
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    MaiTool* frames = registry.findForModel("mobile_camera_frames");
    MaiTool* preview = registry.findForModel("maichat_camera_command");
    CHECK(frames && preview);
    CHECK(frames->description().find("mobile_take_photo") != std::string::npos);
    CHECK(preview->description().find("mobile_take_photo") != std::string::npos);
    const Json frameSchema = Json::parse(frames->parametersSchema());
    CHECK(frameSchema.at("type") == "object" && frameSchema.at("additionalProperties") == false);
    CHECK(frameSchema.at("required") == Json::array({"request"}));
    CHECK(frameSchema.at("properties").size() == 1);
    const Json requestSchema = frameSchema.at("properties").at("request");
    CHECK(requestSchema.at("type") == "object" && requestSchema.at("oneOf").size() == 3);
    for (const auto& variant : requestSchema.at("oneOf")) {
        CHECK(variant.at("type") == "object" && variant.at("additionalProperties") == false);
        const Json actions = variant.at("properties").at("action").at("enum");
        CHECK(actions.size() == 1);
        if (actions.at(0) == "open") {
            CHECK(variant.at("required") == Json::array({"action", "batch_id", "frame_id"}));
        } else {
            CHECK(!variant.at("properties").contains("frame_id"));
            CHECK(variant.at("required") ==
                  (actions.at(0) == "list" ? Json::array({"action"}) : Json::array({"action", "batch_id"})));
        }
    }
    const auto flatRequest = frames->execute(R"({"action":"open"})", context);
    CHECK(flatRequest.hasError() && flatRequest.error().message().find("request") != std::string::npos);
    CHECK(frames->execute(R"({"request":{"action":"list"},"action":"list"})", context).hasError());
    const auto missingFrame = frames->execute(R"({"request":{"action":"open"}})", context);
    CHECK(missingFrame.hasError() && missingFrame.error().code() == MaiErrorCode::InvalidInput);
    CHECK(missingFrame.error().message().find("batch_id") != std::string::npos &&
          missingFrame.error().message().find("frame_id") != std::string::npos &&
          missingFrame.error().message().find("mobile_take_photo") != std::string::npos);
    CHECK(probe.calls.load() == 0);
    const auto unknownAction = frames->execute(R"({"request":{"action":"start"}})", context);
    CHECK(unknownAction.hasError() && unknownAction.error().code() == MaiErrorCode::InvalidInput);
    CHECK(unknownAction.error().message().find("list, open, or delete_batch") != std::string::npos);
    CHECK(frames->execute(R"({"request":{"action":"list","frame_id":"frame-example"}})", context).hasError());
    CHECK(frames->execute(R"({"request":{"action":"delete_batch"}})", context).hasError());
    CHECK(frames->requiresApproval(R"({"request":{"action":"delete_batch","batch_id":"batch-example"}})"));
    CHECK(!frames->requiresApproval(R"({"request":{"action":"list"}})"));
    CHECK(!frames->requiresApproval(R"({"action":"delete_batch","batch_id":"batch-example"})"));
    CHECK(probe.calls.load() == 0);
#endif
    CHECK(!photo->execute("{}", context).hasError());
    CHECK(probe.calls.load() == 1 && probe.lastTool == "mobile_take_photo");
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    CHECK(Json::parse(probe.lastArguments) ==
          Json({{"workspace_path", context.root}, {"session_id", context.sessionId}}));
    CHECK(photo->execute(R"({"action":"open"})", context).hasError());
    CHECK(photo->execute("[]", context).hasError());
    canceled.store(true);
    const auto canceledPhoto = photo->execute("{}", context);
    CHECK(canceledPhoto.hasError() && canceledPhoto.error().code() == MaiErrorCode::Canceled);
    canceled.store(false);
    context.sessionId.clear();
    CHECK(photo->execute("{}", context).hasError());
    context.sessionId = "camera-routing-contract-session";
    context.root.clear();
    CHECK(photo->execute("{}", context).hasError());
    CHECK(probe.calls.load() == 1);
#else
    CHECK(Json::parse(probe.lastArguments).empty());
#endif
    clearMaiMobileHostToolHandler(dispatcher);
    CHECK(probe.released.load());
    std::puts("PASS: registered camera tools keep manual capture separate from archived frames");
}

static std::atomic<int> localSessionChanges{0};
static void localSessionChanged(const char* sessionId) {
    if (sessionId != nullptr && std::strncmp(sessionId, "ses_", 4) == 0)
        ++localSessionChanges;
}
static std::string frame(const Json& delta, const char* finish = nullptr) {
    Json choice = {
        {"delta", delta}, {"index", 0}, {"finish_reason", finish ? Json(finish) : Json(nullptr)}};
    return "data: " + Json{{"choices", Json::array({choice})}}.dump() + "\n\n";
}

struct ScreenCaptureProbe {
    std::atomic<int> calls{0};
    std::atomic<bool> released{false};
    std::atomic<bool>* cancel = nullptr;
    bool cancelOnReturn = false;
    std::string expectedTool = "mobile_capture_screen";
    std::string referenceID;
    Json arguments;
};

static const char* captureHostHandler(void* context, const char* tool, const char* arguments) {
    auto* probe = static_cast<ScreenCaptureProbe*>(context);
    CHECK(tool && probe->expectedTool == tool);
    probe->arguments = Json::parse(arguments);
    ++probe->calls;
    if (probe->expectedTool == "mobile_capture_screen") {
        const std::string output = probe->arguments.at("output_path").get<std::string>();
        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(output),
                                        std::string("\x89PNG\r\n\x1a\n", 8)).hasError());
    }
    Json output{{"captured", true}};
// 与 HostTools 编译开关一致，使 macOS 的 iOS 宿主测试也验证完整捕获协议。
// 保留真实 iOS 目标判定，不改变通用宿主断言。
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    if (probe->expectedTool == "mobile_retain_screen_frame") {
        const std::string root = probe->arguments.at("workspace_path");
        const std::string owner = probe->arguments.at("session_id");
        probe->referenceID = MaiIdGenerator::newEventId();
        const Json request{{"action", "retain"}, {"batch_id", probe->arguments.at("batch_id")},
                           {"frame_id", probe->arguments.at("frame_id")},
                           {"reference_id", probe->referenceID}, {"usage_session_id", owner}};
        char* encoded = mai_screen_frame_archive_request(root.c_str(), owner.c_str(),
            request.dump().c_str(), nullptr, 0);
        CHECK(encoded != nullptr);
        const Json reply = Json::parse(encoded);
        mai_screen_frame_archive_free(encoded);
        CHECK(reply.value("ok", false));
        const auto original = MaiFilePath::fromUtf8(root).append(
            MaiFilePath::fromUtf8(reply.at("output").at("path").get<std::string>()));
        output["file_path"] = original.toUtf8();
        output["reference_id"] = probe->referenceID;
    }
#endif
    if (probe->cancelOnReturn && probe->cancel) probe->cancel->store(true);
    const std::string response = Json{{"ok", true}, {"output", output}}.dump();
    auto* owned = static_cast<char*>(std::malloc(response.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, response.c_str(), response.size() + 1);
    return owned;
}

static void releaseCaptureContext(void* context) {
    static_cast<ScreenCaptureProbe*>(context)->released = true;
}

#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
struct CameraArchiveProbe {
    std::string root;
    std::string session;
    std::string owner;
    std::string reference;
    Json arguments;
    std::atomic<bool>* cancel = nullptr;
    bool cancelOnReturn = false;
    int calls = 0;
};
static void releaseCameraArchiveContext(void*) {}

// The fake enforces one selected Session, while the real archive stores the
// image and token. This checks the C++ tool's native context and ownership contract.
static const char* cameraArchiveHandler(void* context, const char* tool, const char* arguments) {
    auto& probe = *static_cast<CameraArchiveProbe*>(context);
    probe.arguments = Json::parse(arguments);
    ++probe.calls;
    Json response;
    if (probe.arguments.value("workspace_path", "") != probe.root ||
        probe.arguments.value("session_id", "") != probe.session) {
        response = {{"ok", false}, {"errorCode", "invalid_input"}, {"error", "wrong Session or workspace"}};
    } else if (std::string(tool) == "maichat_camera_command") {
        const std::string action = probe.arguments.at("action");
        Json output{{"state", action == "close" ? "stopping" : action == "status" ? "captured" : "opening"}};
        if (action == "close") {
            output["was_open"] = true;
            output["batch_id"] = "batch-example";
            output["next_step"] = "wait for captured";
        } else if (action == "status") {
            output["batch_id"] = "batch-example";
            output["frame_count"] = "2";
        }
        response = {{"ok", true}, {"output", output}};
    } else {
        CHECK(std::string(tool) == "mobile_camera_archive");
        const std::string action = probe.arguments.at("action");
        Json request{{"action", action == "open" ? "retain" : action == "delete_batch" ? "discard" : action}};
        if (probe.arguments.contains("batch_id")) request["batch_id"] = probe.arguments["batch_id"];
        if (action == "open") {
            probe.reference = MaiIdGenerator::newEventId();
            request["frame_id"] = probe.arguments.at("frame_id");
            request["reference_id"] = probe.reference;
            request["usage_session_id"] = probe.session;
        }
        char* encoded = mai_screen_frame_archive_request(probe.root.c_str(), probe.owner.c_str(),
            request.dump().c_str(), nullptr, 0);
        CHECK(encoded != nullptr);
        response = Json::parse(encoded);
        mai_screen_frame_archive_free(encoded);
        if (action == "open" && response.value("ok", false))
            response["output"]["archive_owner"] = probe.owner;
    }
    if (probe.cancelOnReturn && probe.cancel) probe.cancel->store(true);
    const std::string bytes = response.dump();
    auto* owned = static_cast<char*>(std::malloc(bytes.size() + 1));
    CHECK(owned != nullptr);
    std::memcpy(owned, bytes.c_str(), bytes.size() + 1);
    return owned;
}

static void cameraArchiveContract() {
    struct Workspace {
        MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-camera-test-" + MaiIdGenerator::newEventId()));
        ~Workspace() { MaiFileSystem::removeRecursively(path); }
    } workspace;
    CHECK(!MaiFileSystem::createDirectories(workspace.path).hasError());
    CameraArchiveProbe probe;
    probe.root = workspace.path.toUtf8();
    probe.session = "camera-session-A";
    probe.owner = "camera:account-A-session-A";
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, cameraArchiveHandler,
                                     freeHostToolResponse, releaseCameraArchiveContext));
    MaiToolRegistry registry;
    registerMobilePhotoTools(registry, dispatcher);
    MaiTool* command = registry.findForModel("maichat_camera_command");
    MaiTool* frames = registry.findForModel("mobile_camera_frames");
    CHECK(command && frames);
    std::atomic<bool> cancel{false};
    probe.cancel = &cancel;
    MaiToolContext context;
    context.root = probe.root; context.sessionId = probe.session; context.cancel = &cancel;
    CHECK(!command->execute(R"({"action":"open"})", context).hasError());
    CHECK(probe.arguments == Json({{"action", "open"}, {"workspace_path", probe.root},
                                   {"session_id", probe.session}}));
    // A close only initiates finalization. The tool must preserve the host's
    // stopping state and expose captured through a subsequent status call.
    const auto closing = command->execute(R"({"action":"close"})", context);
    CHECK(!closing.hasError());
    const Json closingStatus = Json::parse(closing.output());
    CHECK(closingStatus.at("state") == "stopping" && closingStatus.at("was_open") == true &&
          closingStatus.at("batch_id") == "batch-example" && closingStatus.contains("next_step"));
    const auto finished = command->execute(R"({"action":"status"})", context);
    CHECK(!finished.hasError());
    const Json finishedStatus = Json::parse(finished.output());
    CHECK(finishedStatus.at("state") == "captured" && finishedStatus.at("frame_count") == "2");
    CHECK(command->description().find("stopping") != std::string::npos &&
          command->description().find("captured") != std::string::npos);
    const int calls = probe.calls;
    CHECK(command->execute(R"({"action":"open","session_id":"camera-session-B"})", context).hasError());
    CHECK(probe.calls == calls);
    context.sessionId = "camera-session-B";
    CHECK(command->execute(R"({"action":"close"})", context).hasError());
    CHECK(probe.arguments.at("session_id") == "camera-session-B");
    context.sessionId = probe.session;
    context.root = probe.root + "/another-workspace";
    CHECK(command->execute(R"({"action":"status"})", context).hasError());
    CHECK(probe.arguments.at("workspace_path") == context.root);
    context.root = probe.root;
    CHECK(frames->execute(R"({"request":{"action":"list","session_id":"camera-session-B"}})", context).hasError());

    const auto archive = [&](const std::string& owner, const Json& request, const std::string& png = std::string()) {
        char* encoded = mai_screen_frame_archive_request(probe.root.c_str(), owner.c_str(), request.dump().c_str(),
            png.empty() ? nullptr : png.data(), png.size());
        CHECK(encoded != nullptr);
        const Json reply = Json::parse(encoded);
        mai_screen_frame_archive_free(encoded);
        return reply;
    };
    const Json begun = archive(probe.owner, {{"action", "begin"}});
    CHECK(begun.value("ok", false));
    const std::string batch = begun.at("output").at("batch").at("id");
    const auto png = maiEncodeQrCodePng("camera archive original");
    CHECK(png.isOk());
    const auto dimension = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value = (value << 8) | static_cast<unsigned char>(png.value()[offset + index]);
        return value;
    };
    const auto nowMS = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const Json appended = archive(probe.owner, {{"action", "append"}, {"batch_id", batch},
        {"width", dimension(16)}, {"height", dimension(20)}, {"captured_at_ms", nowMS}, {"elapsed_ms", 2000}}, png.value());
    CHECK(appended.value("ok", false));
    const Json original = appended.at("output").at("frame");
    const std::string id = original.at("id");
    CHECK(archive(probe.owner, {{"action", "finish"}, {"batch_id", batch}, {"reason", "stopped"}}).value("ok", false));
    // Screen and another account/session owner cannot enumerate or retrieve this batch.
    for (const std::string& other : {probe.session, std::string("camera:account-B-session-A"),
                                    std::string("camera:account-A-session-B")}) {
        CHECK(archive(other, {{"action", "list"}}).at("output").at("batches").empty());
        CHECK(!archive(other, {{"action", "get"}, {"batch_id", batch}, {"frame_id", id}}).value("ok", true));
    }
    const auto listed = frames->execute(Json{{"request", {{"action", "list"}, {"batch_id", batch}}}}.dump(), context);
    CHECK(!listed.hasError() && Json::parse(listed.output()).at("frames").size() == 1);
    CHECK(
        probe.arguments ==
        Json({{"action", "list"}, {"batch_id", batch}, {"workspace_path", probe.root}, {"session_id", probe.session}}));
    context.sessionId = "camera-session-B";
    CHECK(
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context)
            .hasError());
    context.sessionId = probe.session;
    const auto opened =
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context);
    CHECK(!opened.hasError() && opened.images().size() == 1);
    CHECK(opened.images()[0].path == original.at("path").get<std::string>());
    const auto file = workspace.path.append(MaiFilePath::fromUtf8(opened.images()[0].path));
    CHECK(!Json::parse(opened.output()).contains("archive_owner"));
    probe.cancelOnReturn = true;
    const auto canceled =
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context);
    CHECK(canceled.hasError() && canceled.error().code() == MaiErrorCode::Canceled);
    cancel.store(false); probe.cancelOnReturn = false;
    CHECK(archive(probe.owner, {{"action", "release"}, {"batch_id", batch}, {"frame_id", id},
        {"reference_id", probe.reference}}).at("output").at("released") == false);
    const Json discard{{"request", {{"action", "delete_batch"}, {"batch_id", batch}}}};
    CHECK(frames->requiresApproval(discard.dump()));
    CHECK(!frames->execute(discard.dump(), context).hasError());
    std::string saved;
    CHECK(!MaiFileSystem::readFile(file, saved).hasError() && saved == png.value());
    CHECK(archive(probe.session, {{"action", "release_session"}}).value("ok", false));
    CHECK(!MaiFileSystem::exists(file));
    clearMaiMobileHostToolHandler(dispatcher);
    std::puts("PASS: camera tool context, archive isolation, original reference and cancellation");
}
#endif

static void captureContract() {
    struct Workspace {
        MaiFilePath path = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-capture-test-" + MaiIdGenerator::newEventId()));
        ~Workspace() { MaiFileSystem::removeRecursively(path); }
    } workspace;
    CHECK(!MaiFileSystem::createDirectories(workspace.path).hasError());
    ScreenCaptureProbe probe;
    auto dispatcher = makeMaiMobileHostDispatcher();
    CHECK(setMaiMobileHostToolHandler(dispatcher, &probe, captureHostHandler,
                                     freeHostToolResponse, releaseCaptureContext));
    MaiToolRegistry registry;
    registerMobileScreenCaptureTools(registry, dispatcher);
    MaiTool* capture = registry.findForModel("mobile_capture_screen");
    CHECK(capture != nullptr);
    const Json schema = Json::parse(capture->parametersSchema());
    CHECK(schema["additionalProperties"] == false);
    std::atomic<bool> cancel{false};
    probe.cancel = &cancel;
    MaiToolContext context;
    context.root = workspace.path.toUtf8();
    context.cancel = &cancel;
    context.sessionId = "capture-contract-owner";

#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    CHECK(schema["properties"]["target"]["enum"] ==
          Json::array({"current_app", "last_external"}));
    CHECK(schema["properties"]["delay_ms"]["type"] == "integer");
    CHECK(schema["properties"]["delay_ms"]["minimum"] == 0);
    CHECK(schema["properties"]["delay_ms"]["maximum"] == 30000);

    // Cancel the actual C++ tool while it is waiting, before any host callback.
    std::optional<MaiToolResult> pending;
    std::exception_ptr workerError;
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        entered.store(true);
        try {
            pending.emplace(capture->execute(
                R"({"target":"current_app","delay_ms":5000})", context));
        } catch (...) { workerError = std::current_exception(); }
        finished.store(true);
    });
    while (!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool waited = !finished.load();
    const auto canceledAt = std::chrono::steady_clock::now();
    cancel.store(true);
    worker.join();
    if (workerError) std::rethrow_exception(workerError);
    CHECK(waited && pending.has_value() && pending->hasError());
    CHECK(pending->error().code() == MaiErrorCode::Canceled);
    CHECK(pending->images().empty());
    CHECK(std::chrono::steady_clock::now() - canceledAt < std::chrono::seconds(2));
    CHECK(probe.calls.load() == 0);
    int files = 0;
    MaiFileSystem::walk(workspace.path, [&](const MaiFileEntry& entry) {
        if (!entry.isDirectory) ++files;
        return MaiWalkAction::Continue;
    });
    CHECK(files == 0);
    cancel.store(false);

    const auto started = std::chrono::steady_clock::now();
    auto delayed = capture->execute(R"({"target":"current_app","delay_ms":75})", context);
    CHECK(!delayed.hasError() && delayed.images().size() == 1);
    CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(50));
    CHECK(probe.calls.load() == 1);
    CHECK(probe.arguments["target"] == "current_app" && probe.arguments["delay_ms"] == 0);
    CHECK(probe.arguments.size() == 4);
    CHECK(probe.arguments["session_id"] == context.sessionId);
    CHECK(delayed.images()[0].path == probe.arguments["output_path"]);
    CHECK(!MaiFileSystem::removeFile(MaiFilePath::fromUtf8(delayed.images()[0].path)).hasError());
    auto external = capture->execute(R"({"target":"last_external"})", context);
    CHECK(!external.hasError());
    CHECK(probe.arguments["target"] == "last_external" && probe.arguments["delay_ms"] == 0);
    CHECK(!MaiFileSystem::removeFile(MaiFilePath::fromUtf8(external.images()[0].path)).hasError());
    const int validCalls = probe.calls.load();
    for (const char* invalid : {R"({"target":"entire_device"})", R"({"target":false})",
                               R"({"delay_ms":-1})", R"({"delay_ms":30001})",
                               R"({"delay_ms":0.5})", R"({"delay_ms":true})",
                               R"({"delay_ms":"25"})", R"({"output_path":"outside.png"})"}) {
        auto rejected = capture->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(probe.calls.load() == validCalls);
#else
    CHECK(schema["properties"].empty());
    for (const char* invalid : {R"({"target":"current_app"})", R"({"target":"last_external"})",
                               R"({"delay_ms":0})", R"({"delay_ms":75})"}) {
        auto rejected = capture->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    CHECK(probe.calls.load() == 0);
#endif

    auto normal = capture->execute("{}", context);
    CHECK(!normal.hasError() && normal.images().size() == 1);
    CHECK(!probe.arguments.contains("target"));
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    CHECK(probe.arguments["delay_ms"] == 0);
#else
    CHECK(!probe.arguments.contains("delay_ms"));
    CHECK(probe.arguments.size() == 1);
#endif
    const auto output = MaiFilePath::fromUtf8(normal.images()[0].path);
    CHECK(normal.images()[0].mimeType == "image/png");
#if defined(__ANDROID__)
    CHECK(output.dirName() == workspace.path.append(MaiFilePath::fromUtf8("no_backup/screenshots")));
#else
    CHECK(output.dirName() == workspace.path.append(MaiFilePath::fromUtf8("screenshots")));
#endif
    CHECK(MaiFileSystem::exists(output));
    CHECK(!MaiFileSystem::removeFile(output).hasError());

    // A late cancellation must remove only this call's newly written PNG.
    const auto retained = output.dirName().append(MaiFilePath::fromUtf8("mobile_screen_retained.png"));
    CHECK(!MaiFileSystem::writeFile(retained, "retained").hasError());
    probe.cancelOnReturn = true;
    const int callsBeforeCancel = probe.calls.load();
    auto canceled = capture->execute("{}", context);
    CHECK(canceled.hasError() && canceled.error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.images().empty());
    CHECK(probe.calls.load() == callsBeforeCancel + 1);
    CHECK(!MaiFileSystem::exists(MaiFilePath::fromUtf8(
        probe.arguments["output_path"].get<std::string>())));
    std::string retainedBytes;
    CHECK(!MaiFileSystem::readFile(retained, retainedBytes).hasError() && retainedBytes == "retained");
    auto alreadyCanceled = capture->execute("{}", context);
    CHECK(alreadyCanceled.hasError() && alreadyCanceled.error().code() == MaiErrorCode::Canceled);
    CHECK(probe.calls.load() == callsBeforeCancel + 1);
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    cancel.store(false);
    probe.cancelOnReturn = false;
    context.sessionId = "capture-contract-owner";
    for (const char* name : {"mobile_start_screen_share", "mobile_stop_screen_share"}) {
        auto* lifecycle = registry.findForModel(name);
        CHECK(lifecycle != nullptr);
        probe.expectedTool = name;
        auto bad = lifecycle->execute(R"({"sample_interval_ms":1})", context);
        CHECK(bad.hasError() && bad.error().code() == MaiErrorCode::InvalidInput);
        auto accepted = lifecycle->execute("{}", context);
        CHECK(!accepted.hasError());
        CHECK(probe.arguments == Json({{"workspace_path", context.root},
                                       {"session_id", context.sessionId}}));
    }
    const auto archive = [&](const Json& request, const std::string& png = std::string()) {
        char* encoded = mai_screen_frame_archive_request(context.root.c_str(),
            context.sessionId.c_str(), request.dump().c_str(), png.empty() ? nullptr : png.data(), png.size());
        CHECK(encoded != nullptr);
        const Json reply = Json::parse(encoded);
        mai_screen_frame_archive_free(encoded);
        CHECK(reply.value("ok", false));
        return reply.at("output");
    };
    const Json begun = archive({{"action", "begin"}});
    const std::string batch = begun.at("batch").at("id").get<std::string>();
    const auto png = maiEncodeQrCodePng("saved screen frame contract");
    CHECK(png.isOk());
    const auto readDimension = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
            value = (value << 8) | static_cast<unsigned char>(png.value()[offset + index]);
        return value;
    };
    const auto nowMS = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    archive({{"action", "append"}, {"batch_id", batch}, {"width", readDimension(16)},
        {"height", readDimension(20)}, {"captured_at_ms", nowMS}, {"elapsed_ms", 2000}}, png.value());
    archive({{"action", "finish"}, {"batch_id", batch}, {"reason", "user_stopped"}});
    auto* frames = registry.findForModel("mobile_screen_frames");
    CHECK(frames != nullptr);
    const Json frameSchema = Json::parse(frames->parametersSchema());
    CHECK(frameSchema.at("required") == Json::array({"request"}) && frameSchema.at("additionalProperties") == false);
    CHECK(frameSchema.at("properties").at("request").at("oneOf").size() == 3);
    const int callsBeforeInvalidFrames = probe.calls.load();
    for (const char* invalid :
         {R"({"action":"open"})", R"({"request":{"action":"open"}})", R"({"request":{"action":"delete_batch"}})",
          R"({"request":{"action":"list","frame_id":"f"}})", R"({"request":{"action":"list","session_id":"forged"}})",
          R"({"request":{"action":"list"},"batch_id":"forged"})"}) {
        const auto rejected = frames->execute(invalid, context);
        CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::InvalidInput);
    }
    context.sessionId.clear();
    const auto missingOwner = frames->execute(R"({"request":{"action":"list"}})", context);
    CHECK(missingOwner.hasError() && missingOwner.error().code() == MaiErrorCode::NotConfigured);
    context.sessionId = "capture-contract-owner";
    CHECK(probe.calls.load() == callsBeforeInvalidFrames);
    CHECK(!frames->requiresApproval(R"({"request":{"action":"list"}})"));
    const auto listed = frames->execute(Json{{"request", {{"action", "list"}, {"batch_id", batch}}}}.dump(), context);
    CHECK(!listed.hasError() && listed.images().empty());
    const Json listing = Json::parse(listed.output());
    CHECK(listing.at("frames").size() == 1);
    CHECK(listing.at("batches").at(0).at("status") == "stopped");
    const std::string id = listing.at("frames").at(0).at("id").get<std::string>();
    probe.expectedTool = "mobile_retain_screen_frame";
    const auto opened =
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context);
    CHECK(!opened.hasError() && opened.images().size() == 1);
    CHECK(probe.arguments == Json({{"workspace_path", context.root},
                                   {"session_id", context.sessionId},
                                   {"batch_id", batch},
                                   {"frame_id", id}}));
    const auto openedPath = workspace.path.append(MaiFilePath::fromUtf8(opened.images()[0].path));
    CHECK(MaiFileSystem::exists(openedPath));
    CHECK(opened.images()[0].path == listing.at("frames").at(0).at("path").get<std::string>());
    probe.cancelOnReturn = true;
    auto canceledSelection =
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context);
    CHECK(canceledSelection.hasError() && canceledSelection.error().code() == MaiErrorCode::Canceled);
    cancel.store(false);
    probe.cancelOnReturn = false;
    CHECK(archive({{"action", "release"}, {"batch_id", batch}, {"frame_id", id},
                   {"reference_id", probe.referenceID}}).at("released") == false);
    context.sessionId = "another-session";
    auto rejected =
        frames->execute(Json{{"request", {{"action", "open"}, {"batch_id", batch}, {"frame_id", id}}}}.dump(), context);
    CHECK(rejected.hasError() && rejected.error().code() == MaiErrorCode::NotFound);
    context.sessionId = "capture-contract-owner";
    const Json discard{{"request", {{"action", "delete_batch"}, {"batch_id", batch}}}};
    CHECK(frames->requiresApproval(discard.dump()));
    CHECK(!frames->execute(discard.dump(), context).hasError());
    CHECK(MaiFileSystem::exists(retained));
    std::string selectedPNG;
    CHECK(!MaiFileSystem::readFile(openedPath, selectedPNG).hasError());
    CHECK(selectedPNG == png.value());
    archive({{"action", "release_session"}});
    CHECK(!MaiFileSystem::exists(openedPath));
    CHECK(MaiFileSystem::exists(retained));
#else
    CHECK(registry.findForModel("mobile_screen_frames") == nullptr);
#endif
    clearMaiMobileHostToolHandler(dispatcher);
    CHECK(probe.released.load());
    std::puts("PASS: screen capture schema, dispatch, delay cancellation and owned PNG cleanup");
}
// Details are read from owned, persisted tool records. No model, command, host
// callback or external file is involved; closing before writes also exercises
// the same SQLite recovery path as a normal application restart.
static void toolDetailContract(const std::string& only = "") {
    struct Fixture {
        MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-tool-detail-" + MaiIdGenerator::newEventId()));
        void* agent = nullptr;
        ~Fixture() {
            maiMobileAgentDestroy(agent);
            MaiFileSystem::removeRecursively(root);
        }
    } fixture;
    CHECK(!MaiFileSystem::createDirectories(fixture.root));
    const std::string database = fixture.root.toUtf8() + "/sessions.db";
    const Json config{{"op", "configure"},
                      {"model", "test"},
                      {"apiKey", "unused-owned-fixture"},
                      {"baseUrl", "http://127.0.0.1:9"},
                      {"workspace", fixture.root.toUtf8()},
                      {"database", database}};
    const auto reopen = [&] {
        fixture.agent = maiMobileAgentCreate();
        CHECK(fixture.agent != nullptr);
        CHECK(call(fixture.agent, config).at("ok") == true);
    };
    const auto close = [&] {
        maiMobileAgentDestroy(fixture.agent);
        fixture.agent = nullptr;
    };
    reopen();
    const std::string owner = call(fixture.agent, {{"op", "create"}}).at("id");
    const std::string other = call(fixture.agent, {{"op", "create"}}).at("id");
    close();
    MaiToolPart tool;
    tool.tool = "owned_recorded_tool";
    tool.callId = only == "call_id" ? std::string(50 * 1024, 'c') : "call_owned_detail";
    tool.input = R"({"query":"owned recorded arguments","literal":"\\n"})";
    // A scalar straddles the default byte page. It must be delivered intact on
    // the next page, rather than introducing a replacement into valid text.
    tool.output = std::string(4095, 'a') + "\xe4\xb8\xad\xf0\x9f\x98\x84" + std::string(5000, 'z');
    tool.error = "owned recorded failure\nsecond line";
    tool.state = MaiToolState::Error;
    MaiMessage message;
    message.id = MaiIdGenerator::newMessageId();
    message.role = MaiRole::Assistant;
    message.created = MaiTime::getCurrentTime();
    message.completed = message.created;
    const std::string partId = MaiIdGenerator::newPartId();
    const std::string textPartId = MaiIdGenerator::newPartId();
    message.parts.push_back({partId, tool, message.created});
    message.parts.push_back({textPartId, MaiTextPart{"owned sibling text"}, message.created});
    {
        auto store = makeMaiSqliteStore(database);
        CHECK(store);
        store.value()->putMessage(owner, message);
        CHECK(!store.value()->lastWriteError());
    }
    reopen();
    const Json base{{"op", "tool_detail"},     {"session", owner}, {"requestId", "owned-detail-read"},
                    {"messageId", message.id}, {"partId", partId}, {"field", "output"}};
    if (only == "binding") {
        // A request below the control-message budget must not become a larger
        // failure just because an invalid JSON binding value is echoed back.
        Json oversized{{"op", "tool_detail"},
                       {"session", "s"},
                       {"requestId", "r"},
                       {"messageId", "m"},
                       {"partId", "p"},
                       {"field", "input"},
                       {"offset", std::string(65400, 'a')}};
        CHECK(oversized.dump().size() < 64 * 1024);
        const auto invalidOffset = call(fixture.agent, oversized);
        std::fprintf(stderr, "OBSERVE: malformed offset request=%zu response=%zu bytes\n", oversized.dump().size(),
                     invalidOffset.dump().size());
        CHECK(invalidOffset.dump().size() < 64 * 1024);
        CHECK(invalidOffset.at("ok") == false && invalidOffset.at("errorCode") == "invalid_input");
        CHECK(invalidOffset.at("offset") == 0 && invalidOffset.at("requestId") == "r");
        for (const char* key : {"requestId", "session", "messageId", "partId", "field", "offset"}) {
            for (const auto& value : {Json(std::string(65400, 'a')), Json{{"nested", std::string(65400, 'a')}},
                                      Json::array({"invalid", "binding"})}) {
                Json request = base;
                request[key] = value;
                const auto response = call(fixture.agent, request);
                CHECK(response.dump().size() < 64 * 1024);
                CHECK(response.at("ok") == false && response.at("errorCode") == "invalid_input");
                const std::string responseKey = std::strcmp(key, "session") == 0 ? "sessionId" : key;
                CHECK(response.at(responseKey) == (std::strcmp(key, "offset") == 0 ? Json(0) : Json("")));
                CHECK(!response.contains("text") && !response.contains("rawBase64"));
                if (std::strcmp(key, "messageId") != 0) CHECK(response.at("messageId") == message.id);
                if (std::strcmp(key, "partId") != 0) CHECK(response.at("partId") == partId);
            }
        }
        std::puts("PASS: malformed tool-detail bindings remain bounded and retain valid identities");
        return;
    }
    if (only == "call_id") {
        for (const char* op : {"snapshot", "remote_snapshot", "messages_page", "message"}) {
            const auto response =
                call(fixture.agent, {{"op", op}, {"session", owner}, {"force", true}, {"id", message.id}});
            CHECK(response.at("ok") == true && response.at("messages").size() == 1);
            const auto& projectedMessage = response.at("messages").at(0);
            const auto& projected = projectedMessage.at("parts").at(0);
            std::fprintf(stderr, "OBSERVE: %s projected message=%zu bytes callId_present=%d\n", op,
                         projectedMessage.dump().size(), projected.contains("callId") ? 1 : 0);
            CHECK(!projected.contains("callId"));
            CHECK(projectedMessage.dump().size() < 48 * 1024);
            CHECK(projected.at("detailsAvailable") == true);
            const auto original = call(fixture.agent, base);
            CHECK(original.at("ok") == true && original.at("callId") == tool.callId);
            CHECK(original.at("detailRevision") == projected.at("detailRevision"));
        }
        std::puts("PASS: oversized optional call IDs are omitted without truncating the source revision");
        return;
    }
    const auto bound = [&](const Json& response, const Json& request) {
        CHECK(response.at("requestId") == request.at("requestId"));
        CHECK(response.at("sessionId") == request.at("session"));
        CHECK(response.at("messageId") == request.at("messageId"));
        CHECK(response.at("partId") == request.at("partId"));
        const std::string field = request.at("field");
        CHECK(response.at("field") == (field == "input" || field == "output" || field == "error" ? field : ""));
        const auto requestedOffset = request.value("offset", Json(0));
        const auto expectedOffset = requestedOffset.is_number_integer() && (requestedOffset.is_number_unsigned() ||
                                                                            requestedOffset.get<std::int64_t>() >= 0)
                                        ? requestedOffset
                                        : Json(0);
        CHECK(response.at("offset") == expectedOffset);
    };
    const auto rejected = [&](const Json& request, const char* code) {
        const Json response = call(fixture.agent, request);
        bound(response, request);
        CHECK(response.at("ok") == false);
        CHECK(response.at("errorCode") == code);
        CHECK(!response.contains("text") && !response.contains("rawBase64"));
    };
    Json first = call(fixture.agent, base);
    CHECK(first.at("ok") == true);
    bound(first, base);
    CHECK(first.at("callId") == tool.callId && first.at("tool") == tool.tool);
    CHECK(first.at("state") == "error");
    CHECK(first.at("offset") == 0 && first.at("totalBytes") == tool.output.size());
    CHECK(first.at("sourceBytesRead") == 4095 && first.at("nextOffset") == 4095);
    CHECK(first.at("text") == std::string(4095, 'a'));
    CHECK(first.at("textIsValidUtf8") == true && first.at("hasMore") == true);
    CHECK(!first.contains("rawBase64"));
    const std::string revision = first.at("detailRevision");
    CHECK(!revision.empty());
    std::string assembled = first.at("text");
    Json page = first;
    while (page.at("hasMore") == true) {
        Json request = base;
        request["offset"] = page.at("nextOffset");
        request["detailRevision"] = revision;
        page = call(fixture.agent, request);
        bound(page, request);
        CHECK(page.at("ok") == true && page.at("detailRevision") == revision);
        CHECK(page.at("totalBytes") == tool.output.size());
        CHECK(page.at("textIsValidUtf8") == true && !page.contains("rawBase64"));
        CHECK(page.at("sourceBytesRead").get<std::size_t>() == page.at("text").get<std::string>().size());
        CHECK(page.at("nextOffset").get<std::size_t>() ==
              (page.at("hasMore").get<bool>()
                   ? request.at("offset").get<std::size_t>() + page.at("sourceBytesRead").get<std::size_t>()
                   : 0));
        CHECK(page.at("sourceBytesRead").get<std::size_t>() > 0);
        assembled += page.at("text").get<std::string>();
    }
    CHECK(assembled == tool.output);
    CHECK(page.at("nextOffset") == 0);
    for (const char* field : {"input", "error"}) {
        Json request = base;
        request["field"] = field;
        request["detailRevision"] = revision;
        const Json response = call(fixture.agent, request);
        bound(response, request);
        CHECK(response.at("ok") == true && response.at("detailRevision") == revision);
        CHECK(response.at("text") == (std::strcmp(field, "input") == 0 ? tool.input : tool.error));
        CHECK(response.at("hasMore") == false);
    }
    for (const char* op : {"snapshot", "remote_snapshot", "messages_page", "message"}) {
        Json request{{"op", op}, {"session", owner}, {"force", true}, {"id", message.id}};
        const Json response = call(fixture.agent, request);
        CHECK(response.at("ok") == true && response.at("messages").size() == 1);
        const Json& projected = response.at("messages").at(0).at("parts").at(0);
        CHECK(projected.at("callId") == tool.callId);
        CHECK(projected.at("detailRevision") == revision && projected.at("detailsAvailable") == true);
        CHECK(projected.at("input") == tool.input && projected.at("output") == tool.output &&
              projected.at("error") == tool.error);
    }
    for (const auto& change :
         {Json{{"session", other}}, Json{{"session", "ses_absent"}}, Json{{"messageId", "msg_absent"}},
          Json{{"partId", "prt_absent"}}, Json{{"partId", textPartId}}}) {
        Json request = base;
        request.update(change);
        rejected(request, "not_found");
    }
    for (const auto& change : {Json{{"field", "path"}}, Json{{"field", "metadata"}}, Json{{"offset", -1}},
                               Json{{"offset", 1.5}}, Json{{"offset", "0"}}, Json{{"offset", tool.output.size() + 1}},
                               Json{{"limit", 0}}, Json{{"limit", -1}}, Json{{"limit", 4097}}, Json{{"limit", "4096"}},
                               Json{{"limit", 1.5}}, Json{{"offset", 4096}}, Json{{"offset", 4095}, {"limit", 1}}}) {
        Json request = base;
        request.update(change);
        request["detailRevision"] = revision;
        rejected(request, "invalid_input");
    }
    Json unboundContinuation = base;
    unboundContinuation["offset"] = 4095;
    rejected(unboundContinuation, "invalid_input");
    Json eof = base;
    eof["offset"] = tool.output.size();
    eof["detailRevision"] = revision;
    const Json emptyPage = call(fixture.agent, eof);
    bound(emptyPage, eof);
    CHECK(emptyPage.at("ok") == true && emptyPage.at("text") == "");
    CHECK(emptyPage.at("sourceBytesRead") == 0 && emptyPage.at("hasMore") == false);
    close();
    // Same-length changes invalidate the whole-part revision; a size-only token
    // would incorrectly let a client append pages from two different outputs.
    std::get<MaiToolPart>(message.parts.front().body).output.front() = 'b';
    const std::string invalidPartId = MaiIdGenerator::newPartId();
    MaiToolPart invalid = tool;
    invalid.callId = "call_owned_invalid_utf8";
    invalid.output = "owned legacy fixture placeholder";
    message.parts.push_back({invalidPartId, invalid, message.created});
    {
        auto store = makeMaiSqliteStore(database);
        CHECK(store);
        store.value()->putMessage(owner, message);
        CHECK(!store.value()->lastWriteError());
    }
    // Ordinary writes correctly normalize UTF-8. A legacy-reader test therefore
    // creates its own row normally, then injects exact historical bytes through
    // a bound BLOB->TEXT update. No production store behavior is bypassed outside
    // this disposable, closed fixture database.
    const std::string legacyBytes("\xff\x80x", 3);
    {
        sqlite3* rawDatabase = nullptr;
        const int opened = sqlite3_open_v2(database.c_str(), &rawDatabase, SQLITE_OPEN_READWRITE, nullptr);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> legacy(rawDatabase, sqlite3_close);
        CHECK(opened == SQLITE_OK);
        sqlite3_stmt* rawUpdate = nullptr;
        const int prepared =
            sqlite3_prepare_v2(legacy.get(),
                               "UPDATE parts SET output=CAST(?1 AS TEXT) WHERE id=?2 AND message_id=?3 "
                               "AND message_id IN (SELECT id FROM messages WHERE session_id=?4)",
                               -1, &rawUpdate, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> update(rawUpdate, sqlite3_finalize);
        CHECK(prepared == SQLITE_OK);
        CHECK(sqlite3_bind_blob(update.get(), 1, legacyBytes.data(), static_cast<int>(legacyBytes.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(update.get(), 2, invalidPartId.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(update.get(), 3, message.id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(update.get(), 4, owner.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_step(update.get()) == SQLITE_DONE && sqlite3_changes(legacy.get()) == 1);
        update.reset();
        sqlite3_stmt* rawRead = nullptr;
        const int readPrepared = sqlite3_prepare_v2(
            legacy.get(), "SELECT CAST(output AS BLOB),typeof(output) FROM parts WHERE id=?1 AND message_id=?2", -1,
            &rawRead, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> read(rawRead, sqlite3_finalize);
        CHECK(readPrepared == SQLITE_OK);
        CHECK(sqlite3_bind_text(read.get(), 1, invalidPartId.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(read.get(), 2, message.id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_step(read.get()) == SQLITE_ROW);
        CHECK(sqlite3_column_bytes(read.get(), 0) == 3);
        CHECK(std::memcmp(sqlite3_column_blob(read.get(), 0), legacyBytes.data(), legacyBytes.size()) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(read.get(), 1)), "text") == 0);
        std::fprintf(stderr, "OBSERVE: owned legacy tool output stored_bytes=%d\n",
                     sqlite3_column_bytes(read.get(), 0));
    }
    reopen();
    Json stale = base;
    stale["offset"] = 4095;
    stale["detailRevision"] = revision;
    rejected(stale, "busy");
    const Json changed = call(fixture.agent, base);
    CHECK(changed.at("ok") == true && changed.at("detailRevision") != revision);
    Json invalidRequest = base;
    invalidRequest["partId"] = invalidPartId;
    const Json invalidPage = call(fixture.agent, invalidRequest);
    bound(invalidPage, invalidRequest);
    CHECK(invalidPage.at("ok") == true && invalidPage.at("textIsValidUtf8") == false);
    CHECK(invalidPage.at("sourceBytesRead") == 3 && invalidPage.at("nextOffset") == 0);
    CHECK(invalidPage.at("totalBytes") == 3 && invalidPage.at("hasMore") == false);
    CHECK(invalidPage.at("rawBase64") == "/4B4");
    CHECK(invalidPage.at("text") == "\xef\xbf\xbd\xef\xbf\xbdx");
    std::puts("PASS: stored tool detail C ABI identity, pagination, revision, UTF-8 and ownership");
}

// 作用：按测试场景构造移动 Agent、调用公开接口并核对结果。
// Exercise the ordinary Mobile configure/model/Runner path with an owned DB.
// The retained record is historical data, never a command to execute again.
static void toolOutputRegistrationContract() {
    struct Fixture {
        MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-output-" + MaiIdGenerator::newEventId()));
        httplib::Server server;
        std::thread listener;
        void* agent = nullptr;
        ~Fixture() {
            maiMobileAgentDestroy(agent);
            server.stop();
            if (listener.joinable()) listener.join();
            MaiFileSystem::removeRecursively(root);
        }
    };
    const std::string outputId = "owned_mobile_retained_output";
    const std::string expectedText = "OWNED MOBILE OUTPUT\n\xe4\xb8\xad\xf0\x9f\x98\x84";
    std::string session, messageId, partId;
    std::atomic<int> requests{0};
    std::atomic<bool> advertised{false}, readBack{false}, invalidRequest{false};
    // Destroy the Agent and join HTTP callbacks before their captured state.
    Fixture fixture;
    CHECK(!MaiFileSystem::createDirectories(fixture.root));
    const std::string database = fixture.root.toUtf8() + "/sessions.db";
    fixture.server.Post("/chat/completions", [&](const httplib::Request& request,
                                                 httplib::Response& response) {
        try {
            const Json body = Json::parse(request.body);
            for (const auto& tool : body.at("tools")) {
                if (tool.at("function").at("name") == "tool_output_read") advertised = true;
            }
            const int index = requests.fetch_add(1);
            if (index == 0) {
                response.set_content(
                    frame({{"tool_calls", Json::array({
                        {{"index", 0}, {"id", "owned_mobile_read_call"}, {"type", "function"},
                         {"function", {{"name", "tool_output_read"},
                                       {"arguments", Json{{"output_id", outputId}}.dump()}}}}
                    })}}, "tool_calls") + "data: [DONE]\n\n", "text/event-stream");
                return;
            }
            for (const auto& message : body.at("messages")) {
                if (message.value("role", "") != "tool" ||
                    message.value("tool_call_id", "") != "owned_mobile_read_call") continue;
                const Json result = Json::parse(message.at("content").get<std::string>(), nullptr, false);
                if (result.is_object() && result.value("output_id", "") == outputId &&
                    result.value("message_id", "") == messageId &&
                    result.value("part_id", "") == partId &&
                    result.value("call_id", "") == "owned_original_call" &&
                    result.value("text", "") == expectedText &&
                    result.value("next_offset_bytes", 1) == 0) readBack = true;
            }
            response.set_content(frame({{"content", "owned read complete"}}, "stop") +
                                     "data: [DONE]\n\n", "text/event-stream");
        } catch (...) {
            invalidRequest = true;
            response.status = 500;
            response.set_content("owned fixture request shape mismatch", "text/plain");
        }
    });
    const int port = fixture.server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    const Json config{{"op", "configure"}, {"model", "test"}, {"apiKey", "test-key"},
                      {"wire", "chat_completions"}, {"policy", "never"},
                      {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
                      {"workspace", fixture.root.toUtf8()}, {"database", database}};
    const auto open = [&] {
        fixture.agent = maiMobileAgentCreate();
        CHECK(fixture.agent != nullptr);
        CHECK(call(fixture.agent, config).at("ok") == true);
    };
    open();
    session = call(fixture.agent, {{"op", "create"}}).at("id");
    maiMobileAgentDestroy(fixture.agent);
    fixture.agent = nullptr;
    {
        auto store = makeMaiSqliteStore(database);
        CHECK(store);
        MaiToolPart original;
        original.tool = "owned_recorded_tool";
        original.callId = "owned_original_call";
        original.input = "{}";
        original.output = "owned retained-output preview";
        original.state = MaiToolState::Completed;
        MaiMessage message;
        message.id = messageId = MaiIdGenerator::newMessageId();
        message.role = MaiRole::Assistant;
        message.created = message.completed = MaiTime::getCurrentTime();
        partId = MaiIdGenerator::newPartId();
        message.parts.push_back({partId, original, message.created});
        store.value()->putMessage(session, message);
        CHECK(!store.value()->lastWriteError());
        MaiToolOutputRecord output;
        output.id = outputId;
        output.sessionId = session;
        output.messageId = messageId;
        output.partId = partId;
        output.callId = original.callId;
        output.text = expectedText;
        output.created = message.created;
        CHECK(!store.value()->putToolOutput(output));
    }
    open();
    // All identifiers are fixed before the listener thread starts.
    fixture.listener = std::thread([&] { fixture.server.listen_after_bind(); });
    fixture.server.wait_until_ready();
    CHECK(fixture.server.is_running());
    CHECK(call(fixture.agent, {{"op", "send"}, {"session", session},
                               {"text", "Read the owned saved output without rerunning its tool."}}).at("ok") == true);
    bool finished = false;
    for (int i = 0; i < 160; ++i) {
        const auto snapshot = call(fixture.agent,
            {{"op", "snapshot"}, {"session", session}, {"force", true}});
        if (!snapshot.at("busy").get<bool>()) { finished = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    CHECK(finished);
    CHECK(!invalidRequest.load());
    CHECK(requests.load() >= 1);
    CHECK(advertised.load());
    CHECK(readBack.load());
    CHECK(requests.load() == 2);
    std::puts("PASS: Mobile configure advertises tool_output_read; Runner reads same-session saved output");
}

// 按真实调用顺序检查输入和可观察结果，失败时保留定位所需的上下文。
// 真正经过 C ABI 的无模型调用合同；只用自有内存 SQLite 和临时工作目录。
// 入站仅入队，立即 takeover 后再 pump，不能连到 dummy 模型地址或发真实 IM。
static void sessionIMCabiContract() {
    const auto workspace = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mobile_session_im_cabi_")));
    CHECK(!MaiFileSystem::createDirectories(workspace));
    const std::string historyPath = workspace.append(MaiFilePath::fromUtf8("owned-im.db")).toUtf8();
    sqlite3* history = nullptr;
    CHECK(sqlite3_open(historyPath.c_str(), &history) == SQLITE_OK);
    CHECK(sqlite3_exec(history,
        "PRAGMA journal_mode=WAL; CREATE TABLE messages(id TEXT PRIMARY KEY,from_user TEXT,to_user TEXT,"
        "peer TEXT,direction INTEGER,status INTEGER,text TEXT,created_at INTEGER,"
        "caption_above INTEGER DEFAULT 0,has_image INTEGER DEFAULT 0,image_path TEXT DEFAULT '',"
        "image_w INTEGER DEFAULT 0,image_h INTEGER DEFAULT 0,image_bytes INTEGER DEFAULT 0,"
        "has_voice INTEGER DEFAULT 0,voice_path TEXT DEFAULT '',voice_seconds INTEGER DEFAULT 0,"
        "has_file INTEGER DEFAULT 0,file_path TEXT DEFAULT '',file_name TEXT DEFAULT '',"
        "file_mime TEXT DEFAULT '',file_bytes INTEGER DEFAULT 0,has_video INTEGER DEFAULT 0,"
        "video_path TEXT DEFAULT '',video_name TEXT DEFAULT '',video_cover TEXT DEFAULT '',"
        "video_seconds INTEGER DEFAULT 0,video_bytes INTEGER DEFAULT 0,"
        "quote_msg_id TEXT DEFAULT '',quote_sender TEXT DEFAULT '',quote_digest TEXT DEFAULT '',quote_kind TEXT DEFAULT '');",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(history);
    void* mobile = maiMobileAgentCreate();
    CHECK(mobile != nullptr);
    const auto cleanup = [&] {
        maiMobileAgentDestroy(mobile);
        MaiFileSystem::removeRecursively(workspace);
    };
    try {
        CHECK(call(mobile, {{"op", "configure"}, {"database", ":memory:"}, {"workspace", workspace.toUtf8()},
                            {"baseUrl", "http://127.0.0.1:1"}, {"apiKey", "owned-unused"}, {"model", "test"}})["ok"] == true);
        const auto activated = call(mobile, {{"op", "im_session_binding"}, {"action", "activate"},
            {"sdk_app_id", "100"}, {"owner_id", "owned-account"}, {"auth_generation", "1"},
            {"friends", Json::array({"owned-peer"})},
            {"history_source", {{"profile", "desktop"}, {"path", historyPath}, {"provenance", "owner_only_legacy"}}}});
        CHECK(activated["ok"] == true && activated["has_bindings"] == false);
        const auto scope = activated.at("scope");
        const auto bridge = activated.at("bridge_id");
        const auto request = [&](Json values) {
            values["op"] = "im_session_binding";
            values["scope"] = scope;
            values["bridge_id"] = bridge;
            return call(mobile, values);
        };
        const std::string session = call(mobile, {{"op", "create"}}).at("id");
        const auto bound = request({{"action", "bind"}, {"peer_id", "owned-peer"}, {"session_id", session},
                                    {"expected_workspace_key", MaiFileSystem::resolve(workspace).toUtf8()}});
        CHECK(bound["ok"] == true && bound["has_bindings"] == true);
        CHECK(bound["bindings"].size() == 1);
        const auto epoch = bound["bindings"][0].at("epoch");
        CHECK(bound["history_tasks"].size() == 1);
        const auto task = bound["history_tasks"][0];
        const auto snapshot = callIMHistorySnapshot({{"action", "open"}, {"task", task}});
        CHECK(snapshot["ok"] == true);
        const auto snapshotId = snapshot.at("snapshot_id");
        CHECK(request({{"action", "history_opened"}, {"task", task}, {"snapshot_id", snapshotId}})["ok"] == true);
        const auto page = callIMHistorySnapshot({{"action", "read"}, {"snapshot_id", snapshotId}});
        CHECK(page["ok"] == true && page["records"].empty() && page["has_more"] == false);
        CHECK(request({{"action", "history_chunk"}, {"task", task}, {"page", page}})["page_accepted"] == true);
        CHECK(callIMHistorySnapshot({{"action", "close"}, {"snapshot_id", snapshotId}})["ok"] == true);
        CHECK(request({{"action", "list"}})["bindings"][0]["history_state"] == "ready");
        Json input{{"action", "admit"}, {"bridge_id", bridge}, {"scope", scope}, {"peer_id", "owned-peer"},
            {"message_id", "owned-live-message"}, {"sender_id", "owned-peer"}, {"recipient_id", "owned-account"},
            {"expected_binding_epoch", epoch}, {"expected_session_id", session}, {"origin", "human"},
            {"live", true}, {"group", false}, {"control_frame", false}, {"has_content", true}};
        const auto admitted = callSessionIMControl(input);
        CHECK(admitted["ok"] == true && admitted["accepted"] == true);
        CHECK(sqlite3_open(historyPath.c_str(), &history) == SQLITE_OK);
        CHECK(sqlite3_exec(history,
            "INSERT INTO messages(id,from_user,to_user,peer,direction,status,text,created_at) "
            "VALUES('owned-live-message','owned-peer','owned-account','owned-peer',0,1,"
            "'owned input, never submitted',1);", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(history);
        CHECK(callSessionIMControl({{"action", "persisted_input"}, {"bridge_id", bridge}, {"scope", scope},
            {"input_id", admitted.at("input_id")}, {"inserted", true}, {"source_row_id", "owned-live-message"}})["ok"] == true);
        input["action"] = "receive";
        input["input_id"] = admitted.at("input_id");
        input["text"] = "owned input, never submitted";
        input.erase("has_content");
        CHECK(request(input)["accepted"] == true);
        CHECK(request({{"action", "list"}})["pending_count"] == 1);
        const auto revoked = callSessionIMControl({{"action", "takeover"}, {"bridge_id", bridge},
            {"scope", scope}, {"session_id", session}});
        CHECK(revoked["ok"] == true);
        const auto pumped = request({{"action", "pump"}});
        CHECK(pumped["ok"] == true && pumped["has_bindings"] == false && pumped["pending_count"] == 0);
        CHECK(pumped["deliveries"].empty());
        CHECK(call(mobile, {{"op", "messages_page"}, {"session", session}, {"limit", 1}})["messages"].empty());
        CHECK(call(mobile, {{"op", "suggest_replies"}, {"messages", Json::array({{{"text", "unused"}}})}})["errorCode"] == "invalid_input");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    std::puts("PASS: Session IM C ABI scope, live admission and immediate no-handle takeover without model or SDK calls");
}

// 消费正常 configure/send 发出的真实模型请求，只连接自有环回 SSE 端点。
// 暂停视频不能连带移除图片或可灵，也不能继续在系统提示里推荐万相付费重试。
static void wanVideoPausedRequestContract() {
    struct Fixture {
        MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-wan-paused-" + MaiIdGenerator::newEventId()));
        httplib::Server server;
        std::thread listener;
        void* agent = nullptr;
        ~Fixture() {
            maiMobileAgentDestroy(agent);
            server.stop();
            if (listener.joinable()) listener.join();
            MaiFileSystem::removeRecursively(root);
        }
    };
    std::atomic<int> requests{0};
    std::atomic<bool> valid{false};
    // Destroy the Agent/listener before the callback state they capture.
    Fixture fixture;
    fixture.server.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
        try {
            const Json body = Json::parse(request.body);
            bool wanVideo = false, wanImage = false, klingVideo = false, klingImage = false;
            for (const auto& tool : body.at("tools")) {
                const auto name = tool.at("function").at("name").get<std::string>();
                wanVideo |= name == "wan_video";
                wanImage |= name == "wan_image";
                klingVideo |= name == "kling_bailian_video";
                klingImage |= name == "kling_bailian_image";
            }
            const auto& system = body.at("messages").front();
            const std::string prompt = system.at("content").get<std::string>();
            valid = system.at("role") == "system" && !wanVideo && wanImage && klingVideo && klingImage &&
                    prompt.find("Wan video generation (wan_video) is temporarily disabled to control costs.") !=
                        std::string::npos &&
                    prompt.find("Do not submit or revise Wan video tasks") != std::string::npos &&
                    prompt.find("silently choose a replacement model") != std::string::npos;
            ++requests;
            response.set_content(frame({{"content", "owned catalog verified"}}, "stop") + "data: [DONE]\n\n",
                                 "text/event-stream");
        } catch (...) {
            response.status = 500;
            response.set_content("owned request shape mismatch", "text/plain");
        }
    });
    const int port = fixture.server.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    CHECK(!MaiFileSystem::createDirectories(fixture.root));
    fixture.agent = maiMobileAgentCreate();
    CHECK(fixture.agent);
    CHECK(call(fixture.agent, {{"op", "configure"},
                               {"model", "owned-model"},
                               {"apiKey", "test-key"},
                               {"wire", "chat_completions"},
                               {"policy", "never"},
                               {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
                               {"workspace", fixture.root.toUtf8()},
                               {"database", fixture.root.toUtf8() + "/sessions.db"}})
              .at("ok") == true);
    const std::string session = call(fixture.agent, {{"op", "create"}}).at("id");
    fixture.listener = std::thread([&] { fixture.server.listen_after_bind(); });
    fixture.server.wait_until_ready();
    CHECK(fixture.server.is_running());
    CHECK(call(fixture.agent, {{"op", "send"}, {"session", session}, {"text", "Check the owned tool catalog."}})
              .at("ok") == true);
    bool finished = false;
    for (int index = 0; index < 160; ++index) {
        const auto snapshot = call(fixture.agent, {{"op", "snapshot"}, {"session", session}, {"force", true}});
        if (!snapshot.at("busy").get<bool>()) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    CHECK(finished && requests.load() == 1 && valid.load());
    std::puts("PASS: normal Mobile model request hides Wan video, retains Wan image/Kling and explains cost pause");
}

int main(int argc, char** argv) {
    const bool wanPausedOnly = argc == 2 && std::strcmp(argv[1], "--wan-video-paused-only") == 0;
    if (argc == 1 || wanPausedOnly) {
        try {
            wanVideoPausedRequestContract();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: Wan video paused request contract: %s\n", error.what());
            return 1;
        }
        if (wanPausedOnly) return 0;
    }
    const bool sessionIMOnly = argc == 2 && std::strcmp(argv[1], "--session-im-binding-only") == 0;
    if (argc == 1 || sessionIMOnly) {
        try { sessionIMCabiContract(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: Session IM C ABI contract: %s\n", error.what());
            return 1;
        }
        if (sessionIMOnly) return 0;
    }
    const bool historyOnly = argc == 2 && std::strcmp(argv[1], "--message-history-host-only") == 0;
    if (argc == 1 || historyOnly) {
        try { messageHistoryHostContract(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: Mobile message history host: %s\n", error.what());
            return 1;
        }
        if (historyOnly) return 0;
    }
    const bool outputOnly = argc == 2 && std::strcmp(argv[1], "--tool-output-registration-only") == 0;
    if (argc == 1 || outputOnly) {
        try { toolOutputRegistrationContract(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: Mobile output registration: %s\n", error.what());
            return 1;
        }
        if (outputOnly) return 0;
    }
    const bool captureOnly = argc == 2 && std::strcmp(argv[1], "--capture-contract-only") == 0;
    const bool cameraOnly = argc == 2 && std::strcmp(argv[1], "--camera-routing-contract-only") == 0;
    const bool detailOnly = argc == 2 && std::strcmp(argv[1], "--tool-detail-contract-only") == 0;
    const bool bindingOnly = argc == 2 && std::strcmp(argv[1], "--tool-detail-binding-contract-only") == 0;
    const bool callIdOnly = argc == 2 && std::strcmp(argv[1], "--tool-detail-call-id-contract-only") == 0;
    if (argc != 1 && !captureOnly && !cameraOnly && !detailOnly && !bindingOnly && !callIdOnly) {
        std::fputs(
            "usage: MaiMobileAgentTests "
            "[--capture-contract-only|--camera-routing-contract-only|--tool-detail-contract-only|"
            "--tool-detail-binding-contract-only|--tool-detail-call-id-contract-only|"
            "--tool-output-registration-only|--message-history-host-only]\n",
            stderr);
        return 2;
    }
    if (argc == 1 || detailOnly || bindingOnly || callIdOnly) {
        try {
            if (argc == 1 || detailOnly) toolDetailContract();
            if (!callIdOnly) toolDetailContract("binding");
            if (!bindingOnly) toolDetailContract("call_id");
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: tool detail contract: %s\n", error.what());
            return 1;
        }
        if (detailOnly || bindingOnly || callIdOnly) return 0;
    }
    try {
        libraryExportDomainContract();
        browserContract();
        cameraRoutingContract();
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        cameraArchiveContract();
#endif
        if (cameraOnly) return 0;
        captureContract();
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: capture contract: %s\n", error.what());
        return 1;
    }
    if (captureOnly) return 0;
    MaiToolRegistry hostRegistry;
    registerMaiChatHostTools(hostRegistry, makeMaiMobileHostDispatcher());
    MaiTool* remoteList = hostRegistry.find("maichat_remote_session_list");
    MaiTool* remoteCreate = hostRegistry.find("maichat_remote_session_create");
    MaiTool* remoteDelete = hostRegistry.find("maichat_remote_session_delete");
    MaiTool* remoteWorkspace = hostRegistry.find("maichat_remote_session_set_workspace");
    CHECK(remoteList != nullptr && remoteCreate != nullptr && remoteDelete != nullptr &&
          remoteWorkspace != nullptr);
    CHECK(!remoteList->requiresApproval("{}"));
    CHECK(!remoteCreate->requiresApproval("{}"));
    CHECK(remoteDelete->requiresApproval("{}"));
    CHECK(remoteDelete->requiresPerCallApproval("{}"));
    CHECK(remoteWorkspace->requiresApproval("{}"));
    CHECK(remoteWorkspace->requiresPerCallApproval("{}"));
#if !defined(_WIN32)
    const unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    auto flipped = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"flip_horizontal"})");
    CHECK(flipped.error == nullptr);
    CHECK(flipped.rgba != nullptr && flipped.width == 2 && flipped.height == 1);
    CHECK(flipped.rgba[0] == 0 && flipped.rgba[1] == 255);
    CHECK(flipped.rgba[4] == 255 && flipped.rgba[5] == 0);
    maiImageFilterFree(flipped.rgba);
    auto cropped = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"crop","x":1,"y":0,"width":1,"height":1})");
    CHECK(cropped.error == nullptr && cropped.width == 1 && cropped.height == 1);
    CHECK(cropped.rgba[0] == 0 && cropped.rgba[1] == 255);
    maiImageFilterFree(cropped.rgba);
    auto rotated = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"rotate","degrees":90})");
    CHECK(rotated.error == nullptr && rotated.width == 1 && rotated.height == 2);
    maiImageFilterFree(rotated.rgba);
    auto resized = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"resize","width":4,"height":2})");
    CHECK(resized.error == nullptr && resized.width == 4 && resized.height == 2);
    maiImageFilterFree(resized.rgba);
    auto grayscale = maiImageFilterRgba(pixels, 2, 1, 8,
                                        R"({"operation":"grayscale"})");
    CHECK(grayscale.error == nullptr && grayscale.width == 2 && grayscale.height == 1);
    CHECK(std::abs(int(grayscale.rgba[0]) - int(grayscale.rgba[1])) <= 2);
    maiImageFilterFree(grayscale.rgba);
    auto adjusted = maiImageFilterRgba(pixels, 2, 1, 8,
                                       R"({"operation":"adjust","brightness":0.1,"contrast":1.1,"saturation":1.2})");
    CHECK(adjusted.error == nullptr && adjusted.width == 2 && adjusted.height == 1);
    maiImageFilterFree(adjusted.rgba);
    auto sharpened = maiImageFilterRgba(pixels, 2, 1, 8,
                                        R"({"operation":"sharpen","amount":1})");
    CHECK(sharpened.error == nullptr && sharpened.width == 2 && sharpened.height == 1);
    maiImageFilterFree(sharpened.rgba);
    auto beautified = maiImageFilterRgba(pixels, 2, 1, 8,
                                         R"({"operation":"beautify","strength":0.5})");
    CHECK(beautified.error == nullptr && beautified.width == 2 && beautified.height == 1);
    maiImageFilterFree(beautified.rgba);
    auto invalid = maiImageFilterRgba(pixels, 2, 1, 8,
                                      R"({"operation":"crop","x":2,"y":0,"width":1,"height":1})");
    CHECK(invalid.rgba == nullptr && invalid.error != nullptr);
    maiImageFilterFree(invalid.error);
#endif

    httplib::Server server;
    std::atomic<bool> sawSteeringInput{false};
    // 本地假模型端点核对 Agent 发出的请求体，并返回可控的流式事件。
    // 测试只连环回地址，不会向真实模型供应商提交任务。
    server.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
        const Json body = Json::parse(request.body);
        CHECK(request.get_header_value("Authorization") == "Bearer test-key");
        CHECK(body["messages"].front()["role"] == "system");
        CHECK(body["messages"].front()["content"].get<std::string>().find(
                  "GitHub-Flavored Markdown") != std::string::npos);
        const std::string instructions = body["messages"].front()["content"].get<std::string>();
        CHECK(instructions.find("Seedance") == std::string::npos);
        CHECK(instructions.find("Seedream") == std::string::npos);
        CHECK(instructions.find("ark_assets") == std::string::npos);
        // 移动端不能向模型宣称可以执行桌面 shell。
        bool hasCreateFile = false, hasCreateDirectory = false, hasDeleteFile = false, hasViewImage = false,
             hasMaiChatContacts = false, hasMaiChatRequestFriend = false, hasMaiChatNickname = false,
             hasMaiChatSend = false, hasMaiChatBroadcast = false, hasGeneratePdf = false, hasMobilePhotos = false,
             hasMobileAlbums = false, hasMobilePhotoRead = false, hasMobilePhotoOriginal = false,
             hasMobileMediaOriginal = false, hasMobilePhotoSave = false, hasMobileTransform = false,
             hasMobileBeautify = false, hasFfprobe = false, hasMobilePreview = false, hasFaceDetection = false,
             hasPersonSegmentation = false, hasMobilePhotoAlbumWrite = false, hasPermissionRequest = false,
             hasSceneDetect = false, hasMotionDetect = false, hasDownloadFile = false, hasLocation = false,
             hasMaiChatMedia = false, hasVideoMatting = false, hasAgentMedia = false, hasSeedance = false,
             hasSeedream = false, hasArkAssets = false, hasCamera = false, hasLocalMusic = false, hasSessionList = false,
             hasSessionCreate = false, hasSessionDelete = false, hasLocalSessionWorkspace = false,
             hasRemoteSessionList = false, hasRemoteSessionCreate = false, hasRemoteSessionDelete = false,
             hasRemoteSessionWorkspace = false, hasMobileBrowser = false,
             hasScreenShareStart = false, hasScreenShareStop = false,
             hasScreenCapture = false;
        for (const auto& tool : body["tools"]) {
            CHECK(tool["function"]["name"] != "shell");
            if (tool["function"]["name"] == "file_create") hasCreateFile = true;
            if (tool["function"]["name"] == "file_create_directory") hasCreateDirectory = true;
            if (tool["function"]["name"] == "file_delete") hasDeleteFile = true;
            if (tool["function"]["name"] == "session_list") hasSessionList = true;
            if (tool["function"]["name"] == "session_create") hasSessionCreate = true;
            if (tool["function"]["name"] == "session_delete") hasSessionDelete = true;
            if (tool["function"]["name"] == "session_set_workspace") hasLocalSessionWorkspace = true;
            if (tool["function"]["name"] == "maichat_remote_session_list") hasRemoteSessionList = true;
            if (tool["function"]["name"] == "maichat_remote_session_create") hasRemoteSessionCreate = true;
            if (tool["function"]["name"] == "maichat_remote_session_delete") {
                hasRemoteSessionDelete = true;
                const auto& schema = tool["function"]["parameters"];
                CHECK(schema["required"] == Json::array({"device_slot", "session_id"}));
            }
            if (tool["function"]["name"] == "maichat_remote_session_set_workspace") {
                hasRemoteSessionWorkspace = true;
                const auto& schema = tool["function"]["parameters"];
                CHECK(schema["required"] ==
                      Json::array({"device_slot", "session_id", "workspace_path"}));
            }
            if (tool["function"]["name"] == "mobile_browser") hasMobileBrowser = true;
            if (tool["function"]["name"] == "mobile_start_screen_share") hasScreenShareStart = true;
            if (tool["function"]["name"] == "mobile_stop_screen_share") hasScreenShareStop = true;
            if (tool["function"]["name"] == "mobile_capture_screen") hasScreenCapture = true;
            if (tool["function"]["name"] == "view_image") hasViewImage = true;
            if (tool["function"]["name"] == "generate_pdf") hasGeneratePdf = true;
            if (tool["function"]["name"] == "maichat_list_contacts") hasMaiChatContacts = true;
            if (tool["function"]["name"] == "maichat_request_friend") hasMaiChatRequestFriend = true;
            if (tool["function"]["name"] == "maichat_set_nickname") hasMaiChatNickname = true;
            if (tool["function"]["name"] == "maichat_send_text") hasMaiChatSend = true;
            if (tool["function"]["name"] == "maichat_send_media") hasMaiChatMedia = true;
            if (tool["function"]["name"] == "agent_send_media") hasAgentMedia = true;
            if (tool["function"]["name"] == "seedance_video") hasSeedance = true;
            if (tool["function"]["name"] == "seedream_image") hasSeedream = true;
            if (tool["function"]["name"] == "ark_assets") hasArkAssets = true;
            if (tool["function"]["name"] == "curl") hasDownloadFile = true;
            if (tool["function"]["name"] == "maichat_broadcast_text")
                hasMaiChatBroadcast = true;
            if (tool["function"]["name"] == "mobile_list_photos") hasMobilePhotos = true;
            if (tool["function"]["name"] == "mobile_take_photo") hasCamera = true;
            if (tool["function"]["name"] == "mobile_play_local_music") hasLocalMusic = true;
            if (tool["function"]["name"] == "mobile_list_albums") hasMobileAlbums = true;
            if (tool["function"]["name"] == "mobile_read_photo") hasMobilePhotoRead = true;
            if (tool["function"]["name"] == "mobile_export_photo_original")
                hasMobilePhotoOriginal = true;
            if (tool["function"]["name"] == "mobile_export_media_original")
                hasMobileMediaOriginal = true;
            if (tool["function"]["name"] == "mobile_save_image") hasMobilePhotoSave = true;
            if (tool["function"]["name"] == "mobile_transform_image")
                hasMobileTransform = true;
            if (tool["function"]["name"] == "mobile_beautify_image")
                hasMobileBeautify = true;
            if (tool["function"]["name"] == "ffprobe") hasFfprobe = true;
            if (tool["function"]["name"] == "mobile_detect_faces") hasFaceDetection = true;
            if (tool["function"]["name"] == "mobile_segment_person") hasPersonSegmentation = true;
            if (tool["function"]["name"] == "mobile_preview_image") hasMobilePreview = true;
            if (tool["function"]["name"] == "mobile_request_permission")
                hasPermissionRequest = true;
            if (tool["function"]["name"] == "mobile_get_location") hasLocation = true;
            if (tool["function"]["name"] == "cv_scene_detect") hasSceneDetect = true;
            if (tool["function"]["name"] == "cv_motion_detect") hasMotionDetect = true;
            if (tool["function"]["name"] == "cv_video_matting") hasVideoMatting = true;
            if (kPhotoAlbumTool != nullptr && tool["function"]["name"] == kPhotoAlbumTool)
                hasMobilePhotoAlbumWrite = true;
        }
        CHECK(hasCreateFile);
        CHECK(hasCreateDirectory);
        CHECK(hasDeleteFile);
        CHECK(hasSessionList && hasSessionCreate && hasSessionDelete);
        CHECK(!hasLocalSessionWorkspace);
        CHECK(hasRemoteSessionList && hasRemoteSessionCreate && hasRemoteSessionDelete);
        CHECK(hasRemoteSessionWorkspace);
        CHECK(hasMobileBrowser);
#if defined(__ANDROID__) || defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        CHECK(hasScreenShareStart && hasScreenShareStop && hasScreenCapture);
#else
        CHECK(!hasScreenShareStart && !hasScreenShareStop && !hasScreenCapture);
#endif
        CHECK(hasViewImage);
        CHECK(hasGeneratePdf);
        CHECK(hasMaiChatContacts);
        CHECK(hasMaiChatRequestFriend);
        CHECK(hasMaiChatNickname);
        CHECK(hasMaiChatSend);
        CHECK(hasMaiChatMedia);
        CHECK(hasAgentMedia);
        CHECK(hasCamera);
        CHECK(hasLocalMusic);
        CHECK(!hasSeedance);
        CHECK(!hasSeedream);
        CHECK(!hasArkAssets);
        CHECK(hasDownloadFile);
        CHECK(hasMaiChatBroadcast);
        CHECK(hasMobilePhotos);
        CHECK(hasMobileAlbums);
        CHECK(hasMobilePhotoRead);
        CHECK(hasMobilePhotoOriginal);
        CHECK(hasMobileMediaOriginal);
        CHECK(hasMobilePhotoSave);
        CHECK(hasMobileTransform);
        CHECK(hasMobileBeautify);
        CHECK(hasFfprobe);
        CHECK(hasFaceDetection);
        CHECK(hasPersonSegmentation);
        CHECK(hasMobilePreview);
        CHECK(hasPermissionRequest);
        CHECK(hasLocation);
        CHECK(hasSceneDetect);
        CHECK(hasMotionDetect);
        CHECK(hasVideoMatting);
        if (kPhotoAlbumTool != nullptr) CHECK(hasMobilePhotoAlbumWrite);
        const auto& last = body["messages"].back();
        std::string input;
        if (last["content"].is_string()) {
            input = last["content"].get<std::string>();
        } else if (last["content"].is_array()) {
            for (const auto& part : last["content"]) {
                if (part.value("type", "") == "text") input = part.value("text", "");
                if (part.value("type", "") == "image_url")
                    CHECK(part["image_url"].value("url", "") == "data:image/png;base64,iVBORw==");
            }
        }
        if (input == "error") {
            response.status = 429;
            response.set_content(R"({"error":{"message":"quota exhausted"}})", "application/json");
            return;
        }
        if (input == "steer-adjustment") sawSteeringInput = true;
        if (input == "file_write") {
            Json invocation = {
                {"index", 0},
                {"id", "call_write"},
                {"type", "function"},
                {"function",
                 {{"name", "file_write"},
                  {"arguments", "{\"path\":\"result.txt\",\"content\":\"saved\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "read-app-sibling") {
            Json invocation = {
                {"index", 0}, {"id", "call_read_sibling"}, {"type", "function"},
                {"function", {{"name", "file_read"},
                              {"arguments", "{\"path\":\"../shared.txt\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "read-legacy") {
            Json invocation = {
                {"index", 0}, {"id", "call_read_legacy"}, {"type", "function"},
                {"function", {{"name", "file_read"},
                              {"arguments", "{\"path\":\"legacy.txt\",\"encoding\":\"gb18030\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "host-send") {
            Json invocation = {
                {"index", 0},
                {"id", "call_host_send"},
                {"type", "function"},
                {"function",
                 {{"name", "maichat_send_text"},
                  {"arguments", "{\"peer_id\":\"alice\",\"text\":\"hello\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "camera-open") {
            Json invocation = {
                {"index", 0}, {"id", "call_camera_open"}, {"type", "function"},
                {"function", {{"name", kCameraTool}, {"arguments", kCameraArguments}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "music-open") {
            Json invocation = {
                {"index", 0}, {"id", "call_music_open"}, {"type", "function"},
                {"function", {{"name", "mobile_play_local_music"}, {"arguments", "{}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n",
                                 "text/event-stream");
            return;
        }
        if (input == "photo-add") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_add"}, {"type", "function"},
                 {"function", {{"name", kPhotoAlbumTool == nullptr ? "unused" : kPhotoAlbumTool},
                              {"arguments", "{\"album_name\":\"Trip\",\"photo_ids\":[\"one\"]}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-save") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_save"}, {"type", "function"},
                {"function", {{"name", "mobile_save_image"},
                              {"arguments", "{\"path\":\"created.png\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-transform") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_transform"}, {"type", "function"},
                {"function", {{"name", "mobile_transform_image"},
                              {"arguments", "{\"path\":\"photo.jpg\",\"operation\":\"beautify\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        if (input == "photo-preview") {
            Json invocation = {
                {"index", 0}, {"id", "call_photo_preview"}, {"type", "function"},
                {"function", {{"name", "mobile_preview_image"},
                              {"arguments", "{\"path\":\"created.png\"}"}}}};
            response.set_content(frame({{"tool_calls", Json::array({invocation})}}, "tool_calls") +
                                     "data: [DONE]\n\n", "text/event-stream");
            return;
        }
        const bool slow = input == "stop";
        response.set_chunked_content_provider(
            "text/event-stream", [slow, step = 0](size_t, httplib::DataSink& sink) mutable {
                std::string text;
                if (step == 0)
                    text = frame({{"reasoning_content", "draft"}});
                else if (step == 1)
                    text = frame({{"content", "hello "}});
                else {
                    text = frame({{"content", "world"}}, "stop") + "data: [DONE]\n\n";
                }
                if (!sink.write(text.data(), text.size())) return false;
                if (step++ >= 2) {
                    sink.done();
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(slow ? 1600 : 160));
                return true;
            });
    });
    int port = server.bind_to_any_port("127.0.0.1");
    std::thread listener([&] {
#if defined(__APPLE__)
        pthread_setname_np("MaiMobileHTTP");
#elif !defined(_WIN32)
        pthread_setname_np(pthread_self(), "MaiMobileHTTP");
#endif
        server.listen_after_bind();
    });
    const MaiFilePath testRoot = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-mobile-test-" + MaiIdGenerator::newEventId()));
    CHECK(!MaiFileSystem::createDirectories(testRoot).hasError());
    const std::string directory = testRoot.toUtf8();
    void* agent = maiMobileAgentCreate();
    int fakeOrtApi = 0;
    HostToolProbe hostProbe;
    int status = 0;
    try {
        CHECK(maiMobileAgentSetChangeHandler(agent, localSessionChanged) == 1);
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, nullptr, nullptr, nullptr) == 0);
        CHECK(maiMobileAgentSetHostToolHandler(agent, &hostProbe, hostToolHandler,
                                               freeHostToolResponse,
                                               releaseHostToolContext) == 1);
        CHECK(maiMobileAgentSetOrtApiBase(agent, &fakeOrtApi) == 1);
        const std::string modelPath = directory + "/rvm.onnx";
        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(modelPath), "model"));
        Json config = {
            {"op", "configure"},      {"baseUrl", "http://127.0.0.1:" + std::to_string(port)},
            {"apiKey", "test-key"},   {"model", "test"},
            {"workspace", directory}, {"database", directory + "/sessions.db"},
            {"rvmModelPath", modelPath}};
#if defined(MAICHAT_MOBILE_IOS) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        config["supportsScreenCapture"] = true;
#endif
        CHECK(call(agent, config)["ok"] == true);
        // 旧独立回复生成链已删除：不得绕开 Session/普通审批另发模型请求。
        const Json removedSuggestion = call(
            agent,
            {{"op", "suggest_replies"},
             {"messages", Json::array({{{"speaker", "friend"}, {"text", "今天能完成吗？"}}})}});
        CHECK(removedSuggestion["ok"] == false);
        CHECK(removedSuggestion["errorCode"] == "invalid_input");
        const int initialHostCalls = hostProbe.calls.load();
        std::string session = call(agent, {{"op", "create"}}).at("id");
        CHECK(localSessionChanges.load() > 0);
        const auto remotePage = call(agent, {{"op", "remote_snapshot"}, {"session", session},
                                             {"limit", 2}});
        CHECK(remotePage["ok"] == true && remotePage["messages"].empty());
        CHECK(call(agent, {{"op", "snapshot"}, {"session", session}})["changed"] == true);
        auto snapshot = [&] {
            return call(agent, {{"op", "snapshot"}, {"session", session}, {"force", true}});
        };
        auto wait = [&](const auto& condition) {
            Json last;
            for (int i = 0; i < 160; ++i) {
                auto s = snapshot();
                if (condition(s)) return s;
                last = std::move(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            throw std::runtime_error("timed out waiting for native state: " +
                                     last.value("error", std::string{}));
        };
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "host-send"}})["ok"] ==
              true);
        const auto hostPermission =
            wait([](const Json& s) { return !s["permissions"].empty(); });
        CHECK(hostProbe.calls == initialHostCalls);
        CHECK(hostPermission["permissions"][0]["tool"] == "maichat_send_text");
        CHECK(hostPermission["permissions"][0]["allowForSession"] == false);
        CHECK(call(agent,
                   {{"op", "permission"},
                    {"id", hostPermission["permissions"][0]["id"]},
                    {"decision", "approved"}})["ok"] == true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(hostProbe.calls == initialHostCalls + 1);
        CHECK(hostProbe.lastTool == "maichat_send_text");
        CHECK(Json::parse(hostProbe.lastArguments)["peer_id"] == "alice");
        if (kPhotoAlbumTool != nullptr) {
            CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-add"}})["ok"] == true);
            const auto photoDone = wait([](const Json& s) { return s["busy"] == false; });
            CHECK(photoDone["permissions"].empty());
            CHECK(hostProbe.calls == initialHostCalls + 2);
            CHECK(hostProbe.lastTool == kPhotoAlbumTool);
        }
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-save"}})["ok"] == true);
        const auto photoSaved = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoSaved["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 2 : 3));
        CHECK(hostProbe.lastTool == "mobile_save_image");
        CHECK(Json::parse(hostProbe.lastArguments)["path"] == "created.png");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-transform"}})["ok"] == true);
        const auto photoTransformed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoTransformed["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 3 : 4));
        CHECK(hostProbe.lastTool == "mobile_transform_image");
        CHECK(Json::parse(hostProbe.lastArguments)["operation"] == "beautify");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "photo-preview"}})["ok"] == true);
        const auto photoPreviewed = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(photoPreviewed["permissions"].empty());
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 4 : 5));
        CHECK(hostProbe.lastTool == "mobile_preview_image");
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "camera-open"}})["ok"] == true);
        const auto cameraDone = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(cameraDone["permissions"].empty());
        CHECK(hostProbe.lastTool == kCameraTool);
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 5 : 6));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "music-open"}})["ok"] == true);
        const auto musicDone = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(musicDone["permissions"].empty());
        CHECK(hostProbe.lastTool == "mobile_play_local_music");
        CHECK(hostProbe.calls == initialHostCalls + (kPhotoAlbumTool == nullptr ? 6 : 7));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "hello"}})["ok"] == true);
        CHECK(call(agent, config)["ok"] == false);  // 工作中不能销毁并换配置。
        auto partial = wait([](const Json& s) {
            return s["busy"] == true && s.dump().find("hello ") != std::string::npos;
        });
        CHECK(partial.dump().find("reasoning") != std::string::npos);
        auto done = wait([](const Json& s) { return s["busy"] == false; });
        const auto latestPage = call(agent, {{"op", "snapshot"}, {"session", session},
                                             {"force", true}, {"messageLimit", 1}});
        CHECK(latestPage["messages"].size() == 1);
        CHECK(latestPage["messages"][0]["id"] == done["messages"].back()["id"]);
        const auto latestId = latestPage["messages"][0]["id"].get<std::string>();
        const auto olderPage = call(agent, {{"op", "messages_page"}, {"session", session},
                                            {"before", latestId}, {"limit", 2}});
        CHECK(olderPage["messages"].size() == 2);
        CHECK(olderPage["messages"][0]["id"] < latestId);
        CHECK(olderPage["messages"][1]["id"] < olderPage["messages"][0]["id"]);
        const auto sessionOnly = call(agent, {{"op", "snapshot"}, {"session", session},
                                              {"force", true}, {"messageLimit", 0}});
        CHECK(sessionOnly["messages"].empty());
        CHECK(!sessionOnly["sessions"].empty());
        for (const auto& part : done["messages"].back()["parts"]) {
            if (part["kind"] == "text")
                CHECK(part["text"] == "hello world");
            else if (part["kind"] == "reasoning")
                CHECK(part["text"] == "draft");
            else
                CHECK(false);
        }
        CHECK(done.dump().find("test-key") == std::string::npos);
        CHECK(
            !MaiFileSystem::writeFile(MaiFilePath::fromUtf8(std::string(directory) + "/photo.png"),
                                      std::string("\x89PNG", 4)));
        Json imageRequest = {{"op", "send"}, {"session", session}, {"text", "image"}};
        imageRequest["images"] =
            Json::array({Json{{"path", "photo.png"}, {"mimeType", "image/png"}}});
        CHECK(call(agent, imageRequest)["ok"] == true);
        auto imageDone = wait([](const Json& s) { return s["busy"] == false; });
        bool foundImage = false;
        for (const auto& message : imageDone["messages"])
            for (const auto& part : message["parts"])
                if (part["kind"] == "image" && part["path"] == "photo.png" &&
                    part["mimeType"] == "image/png")
                    foundImage = true;
        CHECK(foundImage);

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(directory + "/clip.mp4"),
                                        "video fixture"));
        Json videoRequest = { {"op", "send"}, {"session", session},
                              {"text", "Please inspect this video"},
                              {"videos", Json::array({{{"path", "clip.mp4"},
                                                       {"mimeType", "video/mp4"}}})} };
        CHECK(call(agent, videoRequest)["ok"] == true);
        const auto videoDone = wait([](const Json& s) { return s["busy"] == false; });
        bool foundVideo = false;
        for (const auto& message : videoDone["messages"])
            for (const auto& part : message["parts"])
                if (part["kind"] == "video" && part["path"] == "clip.mp4" &&
                    part["mimeType"] == "video/mp4")
                    foundVideo = true;
        CHECK(foundVideo);

        // iOS 升级或恢复后，持久化数据库会跟着数据迁移，但应用容器的绝对路径
        // 可能变化。把同一张相对路径图片放进新工作区，再确认下一轮会先迁移
        // session.directory，历史图片不会继续从旧容器读取。
        const std::string movedWorkspace = std::string(directory) + "/moved-workspace";
        CHECK(!MaiFileSystem::createDirectories(MaiFilePath::fromUtf8(movedWorkspace)));
        CHECK(!MaiFileSystem::writeFile(
            MaiFilePath::fromUtf8(movedWorkspace + "/photo.png"), std::string("\x89PNG", 4)));
        CHECK(!MaiFileSystem::removeFile(
            MaiFilePath::fromUtf8(std::string(directory) + "/photo.png")));
        config["workspace"] = movedWorkspace;
        config["appRoot"] = directory;
        CHECK(call(agent, config)["ok"] == true);
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "relocated"}})["ok"] ==
              true);
        const auto relocated = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(relocated.value("error", "").empty());

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(directory + "/shared.txt"),
                                        "app sibling content"));
        CHECK(call(agent, { {"op", "send"}, {"session", session},
                            {"text", "read-app-sibling"} })["ok"] == true);
        const auto sibling = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(sibling.dump().find("app sibling content") != std::string::npos);

        CHECK(!MaiFileSystem::writeFile(MaiFilePath::fromUtf8(movedWorkspace + "/legacy.txt"),
                                        std::string("\xC4\xE3\xBA\xC3", 4)));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "read-legacy"}})["ok"] == true);
        const auto decoded = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(decoded.dump().find("\u4f60\u597d") != std::string::npos);
        CHECK(hostProbe.lastTool == "mobile_decode_text");
        CHECK(Json::parse(hostProbe.lastArguments).value("encoding", "") == "gb18030");
        CHECK(Json::parse(hostProbe.lastArguments).value("base64", "") == "xOO6ww==");

        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "file_write"}})["ok"] == true);
        auto pending = wait([](const Json& s) { return !s["permissions"].empty(); });
        std::string permission = pending["permissions"][0]["id"];
        CHECK(!MaiFileSystem::exists(MaiFilePath::fromUtf8(movedWorkspace + "/result.txt")));
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "unknown"}})["ok"] ==
              false);
        CHECK(snapshot()["permissions"].size() == 1);
        CHECK(call(agent,
                   {{"op", "permission"}, {"id", permission}, {"decision", "approved"}})["ok"] ==
              true);
        wait([](const Json& s) { return s["busy"] == false; });
        CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(movedWorkspace + "/result.txt")));
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "stop"}})["ok"] == true);
        wait([](const Json& s) {
            return s["busy"] == true && !s["messages"].back()["parts"].empty();
        });
        CHECK(call(agent, {{"op", "stop"}, {"session", session}})["ok"] == true);
        wait([](const Json& s) { return s["busy"] == false; });

        // 真正经过 C ABI、SQLite 和本地假 HTTP 模型，不能只断言按钮或请求被接收。
        const auto steerStarted = call(agent, {{"op", "send"}, {"session", session}, {"text", "stop"}});
        CHECK(steerStarted["ok"] == true);
        const std::string targetTurn = steerStarted.at("id");
        const auto steeringBusy =
            wait([](const Json& s) { return s["busy"] == true && !s["messages"].back()["parts"].empty(); });
        CHECK(steeringBusy.at("turnId") == targetTurn);
        CHECK(call(agent, {{"op", "remote_snapshot"}, {"session", session}}).at("turnId") == targetTurn);
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "busy-send"}})["ok"] == false);
        const Json steerRequest{{"op", "steer"},
                                {"session", session},
                                {"turnId", targetTurn},
                                {"requestId", "cabi-steer-one"},
                                {"text", "steer-adjustment"}};
        const auto acceptedSteering = call(agent, steerRequest);
        CHECK(acceptedSteering.at("ok") == true);
        CHECK(call(agent, steerRequest).at("id") == acceptedSteering.at("id"));
        Json changedSteering = steerRequest;
        changedSteering["text"] = "different payload";
        CHECK(call(agent, changedSteering).at("ok") == false);
        auto steered = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(sawSteeringInput.load());
        CHECK(steered.at("turnId") == "");
        CHECK(call(agent, steerRequest).at("id") == acceptedSteering.at("id"));
        Json idleSteering = steerRequest;
        idleSteering["requestId"] = "cabi-steer-idle";
        CHECK(call(agent, idleSteering).at("ok") == false);
        int acceptedUsers = 0;
        for (const auto& message : steered.at("messages"))
            if (message.at("id") == acceptedSteering.at("id")) ++acceptedUsers;
        CHECK(acceptedUsers == 1);
        CHECK(call(agent, {{"op", "send"}, {"session", session}, {"text", "error"}})["ok"] == true);
        auto failure = wait([](const Json& s) { return s["busy"] == false; });
        CHECK(failure["error"].get<std::string>().find("quota exhausted") != std::string::npos);
        auto messageCount = failure["messages"].size();
        maiMobileAgentDestroy(agent);
        agent = nullptr;
        // 模拟进程被系统终止后残留的未完成记录；不能把它显示成仍在运行。
        {
            auto saved = makeMaiSqliteStore(std::string(directory) + "/sessions.db");
            CHECK(saved);
            MaiMessage orphan;
            orphan.id = MaiIdGenerator::newMessageId();
            orphan.role = MaiRole::Assistant;
            orphan.created = MaiTime::getCurrentTime();
            saved.value()->putMessage(session, orphan);
        }
        agent = maiMobileAgentCreate();
        CHECK(maiMobileAgentSetChangeHandler(agent, localSessionChanged) == 1);
        CHECK(call(agent, config)["ok"] == true);
        CHECK(snapshot()["messages"].size() == messageCount + 1);
        CHECK(snapshot()["messages"].back()["completed"] == 0);
        CHECK(snapshot()["messages"].back()["active"] == false);
        CHECK(call(agent, {{"op", "clear"}, {"session", session}})["ok"] == true);
        CHECK(snapshot()["messages"].empty());
        CHECK(call(agent, {{"op", "delete"}, {"session", session}})["ok"] == true);
        CHECK(snapshot()["sessions"].empty());
        std::puts(
            "PASS: streaming, part kinds, permissions, interruption, "
            "provider errors, persistence, clear/delete");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        status = 1;
    }
    maiMobileAgentDestroy(agent);
    CHECK(hostProbe.released);
    server.stop();
    listener.join();
    MaiFileSystem::removeRecursively(testRoot);
    return status;
}
