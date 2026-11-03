// 文件作用：把 Agent 工具调用转发给移动端宿主，并管理处理器注册、并发调用与释放。
// 调用边界：由 MaiChat 媒体或 Agent 宿主调用；平台资源在停止或失败时须正确释放。
#include "MaiMobileHostTools.h"
#include "MaiChangePasswordTool.h"
#include "MaiPortraitBridge.h"
#include "MaiAccountTools.h"

#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#if defined(MAICHAT_MOBILE_IOS)
#include "MaiPathGuard.h"
#include "MaiScreenFrameArchive.h"
#endif

using Json = nlohmann::json;

class MaiMobileHostDispatcher {
public:
    ~MaiMobileHostDispatcher() {
        clear();
    }

    bool set(void* context, MaiMobileHostToolHandler handler,
             MaiMobileHostToolResponseFree responseFree,
             MaiMobileHostToolContextRelease contextRelease) {
        if (handler == nullptr && context != nullptr) return false;
        if ((handler == nullptr) != (responseFree == nullptr) ||
            (handler == nullptr) != (contextRelease == nullptr))
            return false;
        void* previousContext = nullptr;
        MaiMobileHostToolContextRelease previousRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return false;
            previousContext = mContext;
            previousRelease = mContextRelease;
            mContext = context;
            mHandler = handler;
            mResponseFree = responseFree;
            mContextRelease = contextRelease;
        }
        if (previousRelease != nullptr) previousRelease(previousContext);
        return true;
    }

    // 先在锁内固定宿主回调和上下文，再解锁执行；宿主可能回调 Agent，不能持锁跨越边界。
    // 归还宿主响应后统一解析结果，并把协议错误映射为 Agent 可识别的失败类型。
    MaiToolResult call(const std::string& toolName, const std::string& argumentsJson) {
        void* context = nullptr;
        MaiMobileHostToolHandler handler = nullptr;
        MaiMobileHostToolResponseFree responseFree = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mHandler == nullptr)
                return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                              "the MaiChat host tool handler is not registered");
            ++mActiveCalls;
            context = mContext;
            handler = mHandler;
            responseFree = mResponseFree;
        }

        const char* response = handler(context, toolName.c_str(), argumentsJson.c_str());
        std::string responseJson;
        if (response != nullptr) responseJson = response;
        if (response != nullptr) responseFree(context, response);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            --mActiveCalls;
        }
        if (responseJson.empty())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "the MaiChat host returned an empty response");
        try {
            const Json parsed = Json::parse(responseJson);
            const bool success = parsed.value("ok", parsed.value("success", false));
            if (!success) {
                const std::string code = parsed.value("errorCode", "internal");
                MaiErrorCode errorCode = MaiErrorCode::Internal;
                if (code == "invalid_input") errorCode = MaiErrorCode::InvalidInput;
                else if (code == "busy") errorCode = MaiErrorCode::Busy;
                else if (code == "not_found") errorCode = MaiErrorCode::NotFound;
                else if (code == "not_configured") errorCode = MaiErrorCode::NotConfigured;
                else if (code == "not_supported") errorCode = MaiErrorCode::NotSupported;
                else if (code == "canceled") errorCode = MaiErrorCode::Canceled;
                else if (code == "network") errorCode = MaiErrorCode::Network;
                else if (code == "protocol") errorCode = MaiErrorCode::Protocol;
                return MaiToolResult::failure(
                    errorCode, parsed.value("error", parsed.value(
                        "errorMessage", "the MaiChat host tool failed")));
            }
            if (!parsed.contains("output"))
                return MaiToolResult::failure(MaiErrorCode::Protocol,
                                              "the MaiChat host response has no output");
            return MaiToolResult::success(parsed["output"].is_string()
                                              ? parsed["output"].get<std::string>()
                                              : parsed["output"].dump());
        } catch (const std::exception& error) {
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          std::string("invalid MaiChat host response: ") +
                                              error.what());
        }
    }

    void clear() {
        void* context = nullptr;
        MaiMobileHostToolContextRelease contextRelease = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveCalls != 0) return;
            context = mContext;
            contextRelease = mContextRelease;
            mContext = nullptr;
            mHandler = nullptr;
            mResponseFree = nullptr;
            mContextRelease = nullptr;
        }
        if (contextRelease != nullptr) contextRelease(context);
    }

private:
    std::mutex mMutex;
    void* mContext = nullptr;
    MaiMobileHostToolHandler mHandler = nullptr;
    MaiMobileHostToolResponseFree mResponseFree = nullptr;
    MaiMobileHostToolContextRelease mContextRelease = nullptr;
    int mActiveCalls = 0;
};

namespace {

#if defined(MAICHAT_MOBILE_IOS)
MaiToolResult screenArchiveRequest(const MaiToolContext& context, const Json& request) {
    if (context.isCanceled())
        return MaiToolResult::failure(MaiErrorCode::Canceled, "screen frame request was canceled");
    if (context.root.empty() || context.sessionId.empty())
        return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                      "the screen frame Session workspace is unavailable");
    char* encoded = mai_screen_frame_archive_request(
        context.root.c_str(), context.sessionId.c_str(), request.dump().c_str(), nullptr, 0);
    if (!encoded)
        return MaiToolResult::failure(MaiErrorCode::Internal, "screen frame archive returned no response");
    const Json response = Json::parse(encoded, nullptr, false);
    mai_screen_frame_archive_free(encoded);
    if (!response.is_object() || !response.contains("ok") || !response["ok"].is_boolean())
        return MaiToolResult::failure(MaiErrorCode::Protocol, "invalid screen frame archive response");
    if (!response["ok"].get<bool>()) {
        const std::string code = response.value("errorCode", "internal");
        MaiErrorCode error = MaiErrorCode::Internal;
        if (code == "invalid_input") error = MaiErrorCode::InvalidInput;
        else if (code == "not_found") error = MaiErrorCode::NotFound;
        else if (code == "busy" || code == "limit") error = MaiErrorCode::Busy;
        else if (code == "rate_limited") error = MaiErrorCode::RateLimited;
        return MaiToolResult::failure(error, response.value("error", "screen frame archive failed"));
    }
    if (!response.contains("output") || !response["output"].is_object())
        return MaiToolResult::failure(MaiErrorCode::Protocol, "screen frame archive output is missing");
    if (context.isCanceled())
        return MaiToolResult::failure(MaiErrorCode::Canceled, "screen frame request was canceled");
    return MaiToolResult::success(response["output"].dump());
}

class MaiMobileScreenFramesTool final : public MaiTool {
public:
    explicit MaiMobileScreenFramesTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}
    std::string name() const override { return "mobile_screen_frames"; }
    std::string description() const override {
        return "List screenshots saved automatically every 2 seconds during iOS screen sharing. "
               "After sharing stops, list the batch and ask which numbered/timestamped images the "
               "user needs. Listing returns metadata without sending all images to the model. "
               "Open only the requested frame with batch_id and frame_id from that list. "
               "delete_batch clears a stopped batch when the user requests cleanup; images still "
               "referenced by drafts or messages keep their original file. "
               "This tool does not start or restart sharing, and stopped batches remain available.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["list","open","delete_batch"]},)"
               R"("batch_id":{"type":"string","minLength":1,"maxLength":100},)"
               R"("frame_id":{"type":"string","minLength":1,"maxLength":100}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& arguments) const override {
        const Json parsed = Json::parse(arguments, nullptr, false);
        return parsed.is_object() && parsed.value("action", Json{}) == "delete_batch";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object() || !args.contains("action") || !args["action"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "a screen frame action is required");
        for (auto field = args.begin(); field != args.end(); ++field) {
            if (field.key() != "action" && field.key() != "batch_id" && field.key() != "frame_id")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unknown screen frame argument");
            if (!field.value().is_string() || field.value().get_ref<const std::string&>().empty() ||
                field.value().get_ref<const std::string&>().size() > 100)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "screen frame identifiers must be strings");
        }
        const std::string action = args["action"].get<std::string>();
        if ((action != "list" && action != "open" && action != "delete_batch") ||
            (action != "list" && !args.contains("batch_id")) ||
            (action == "open" && !args.contains("frame_id")) ||
            (action != "open" && args.contains("frame_id")))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "open requires batch_id and frame_id; deletion requires batch_id");
        if (action == "open" && context.model == "glm-5.3")
            return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                          "the current model cannot inspect saved screenshots");
        if (action == "delete_batch") {
            const MaiToolResult checked = screenArchiveRequest(context,
                Json{{"action", "list"}, {"batch_id", args["batch_id"]}});
            if (checked.hasError()) return checked;
            const Json listing = Json::parse(checked.output(), nullptr, false);
            if (!listing.is_object() || !listing.contains("batches") ||
                !listing["batches"].is_array())
                return MaiToolResult::failure(MaiErrorCode::Protocol, "screen frame batch is missing");
            if (listing["batches"].empty())
                return MaiToolResult::failure(MaiErrorCode::NotFound, "screen frame batch was already cleared");
            if (listing["batches"][0].value("status", "") == "recording")
                return MaiToolResult::failure(MaiErrorCode::Busy,
                                              "stop screen sharing before deleting its batch");
        }
        args["action"] = action == "open" ? "get" : action == "delete_batch" ? "discard" : "list";
        const MaiToolResult result = screenArchiveRequest(context, args);
        if (result.hasError() || action != "open") return result;
        const Json frame = Json::parse(result.output(), nullptr, false);
        if (!frame.is_object() || !frame.contains("path") || !frame["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::Protocol, "screen frame path is missing");
        // UI and tools retain an independent reference to the same original.
        // Releasing a canceled result must never delete another holder's file.
        const MaiToolResult retained = callMaiMobileHostTool(mDispatcher, "mobile_retain_screen_frame",
            Json{{"workspace_path", context.root}, {"session_id", context.sessionId},
                 {"batch_id", args["batch_id"]}, {"frame_id", args["frame_id"]}}.dump());
        if (retained.hasError()) return retained;
        const Json saved = Json::parse(retained.output(), nullptr, false);
        if (!saved.is_object() || !saved.contains("reference_id") || !saved["reference_id"].is_string() ||
            saved["reference_id"].get_ref<const std::string&>().empty())
            return MaiToolResult::failure(MaiErrorCode::Protocol, "screen frame reference is missing");
        const auto release = [&] {
            const Json request{{"action", "release"}, {"batch_id", args["batch_id"]},
                {"frame_id", args["frame_id"]}, {"reference_id", saved["reference_id"]}};
            char* reply = mai_screen_frame_archive_request(context.root.c_str(), context.sessionId.c_str(),
                request.dump().c_str(), nullptr, 0);
            if (reply) mai_screen_frame_archive_free(reply);
        };
        if (!saved.contains("file_path") || !saved["file_path"].is_string()) {
            release();
            return MaiToolResult::failure(MaiErrorCode::Protocol, "retained screen frame path is missing");
        }
        const std::string path = maiResolvePathWithinRoot(context.root, saved["file_path"].get<std::string>());
        const std::string original = maiResolvePathWithinRoot(context.root, frame["path"].get<std::string>());
        if (path.empty() || path != original) {
            release();
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "screen frame is outside this workspace");
        }
        const MaiFilePath file = MaiFilePath::fromUtf8(path);
        std::uint64_t size = 0;
        std::string header;
        bool clipped = false;
        const MaiError read = MaiFileSystem::readFile(file, header, 8, &clipped);
        if (!MaiFileSystem::fileSize(file, size) || size == 0 || size > 20 * 1024 * 1024 ||
            read.hasError() || header.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) {
            release();
            return MaiToolResult::failure(MaiErrorCode::Protocol, "saved screen frame is not a bounded PNG");
        }
        if (context.isCanceled()) {
            release();
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen frame request was canceled");
        }
        Json output = frame;
        const std::string relative = frame["path"].get<std::string>();
        output["file_path"] = relative;
        return MaiToolResult::successWithImages(output.dump(), {MaiToolImage{relative, "image/png"}});
    }
private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

// The native host binds the camera owner to the account and originating Session.
// The model supplies only opaque batch/frame IDs, never the archive owner or path.
class MaiMobileCameraFramesTool final : public MaiTool {
public:
    explicit MaiMobileCameraFramesTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}
    std::string name() const override { return "mobile_camera_frames"; }
    std::string description() const override {
        return "List images sampled every 2 seconds during iOS camera preview, including capture times. "
               "After closing the camera, ask which image the user wants and open only that image. "
               "delete_batch clears unreferenced images from a stopped batch on request. "
               "Images retained by a draft or message keep the original file.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["list","open","delete_batch"]},)"
               R"("batch_id":{"type":"string","minLength":1,"maxLength":100},)"
               R"("frame_id":{"type":"string","minLength":1,"maxLength":100}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& arguments) const override {
        const Json parsed = Json::parse(arguments, nullptr, false);
        return parsed.is_object() && parsed.value("action", Json{}) == "delete_batch";
    }
    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object() || !args.contains("action") || !args["action"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "a camera frame action is required");
        for (auto field = args.begin(); field != args.end(); ++field) {
            if ((field.key() != "action" && field.key() != "batch_id" && field.key() != "frame_id") ||
                !field.value().is_string() || field.value().get_ref<const std::string&>().empty() ||
                field.value().get_ref<const std::string&>().size() > 100)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid camera frame argument");
        }
        const std::string action = args["action"].get<std::string>();
        if ((action != "list" && action != "open" && action != "delete_batch") ||
            (action != "list" && !args.contains("batch_id")) ||
            (action == "open" && !args.contains("frame_id")) ||
            (action != "open" && args.contains("frame_id")))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid camera frame action or identifiers");
        if (context.root.empty() || context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "camera Session workspace is unavailable");
        if (context.isCanceled()) return MaiToolResult::failure(MaiErrorCode::Canceled, "camera request canceled");
        if (action == "open" && context.model == "glm-5.3")
            return MaiToolResult::failure(MaiErrorCode::NotSupported, "the current model cannot inspect saved images");
        args["workspace_path"] = context.root;
        args["session_id"] = context.sessionId;
        const MaiToolResult result = callMaiMobileHostTool(mDispatcher, "mobile_camera_archive", args.dump());
        if (result.hasError() || action != "open") return result;
        Json frame = Json::parse(result.output(), nullptr, false);
        if (!frame.is_object() || !frame.contains("path") || !frame["path"].is_string() ||
            !frame.contains("reference_id") || !frame["reference_id"].is_string() ||
            !frame.contains("archive_owner") || !frame["archive_owner"].is_string())
            return MaiToolResult::failure(MaiErrorCode::Protocol, "retained camera frame is invalid");
        const std::string owner = frame["archive_owner"].get<std::string>();
        frame.erase("archive_owner");
        const auto release = [&] {
            const Json request{{"action", "release"}, {"batch_id", args["batch_id"]},
                {"frame_id", args["frame_id"]}, {"reference_id", frame["reference_id"]}};
            char* reply = mai_screen_frame_archive_request(context.root.c_str(), owner.c_str(),
                request.dump().c_str(), nullptr, 0);
            if (reply) mai_screen_frame_archive_free(reply);
        };
        const std::string relative = frame["path"].get<std::string>();
        const std::string path = maiResolvePathWithinRoot(context.root, relative);
        const MaiFilePath file = MaiFilePath::fromUtf8(path);
        std::uint64_t size = 0;
        std::string header;
        bool clipped = false;
        if (path.empty() || !MaiFileSystem::fileSize(file, size) || size == 0 || size > 20 * 1024 * 1024 ||
            MaiFileSystem::readFile(file, header, 8, &clipped).hasError() ||
            header.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 || context.isCanceled()) {
            release();
            return MaiToolResult::failure(context.isCanceled() ? MaiErrorCode::Canceled : MaiErrorCode::Protocol,
                                          "camera frame unavailable or canceled");
        }
        return MaiToolResult::successWithImages(frame.dump(), {MaiToolImage{relative, "image/png"}});
    }
private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

class MaiMobileScreenShareLifecycleTool final : public MaiTool {
public:
    MaiMobileScreenShareLifecycleTool(bool start, std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mStart(start), mDispatcher(std::move(dispatcher)) {}
    std::string name() const override {
        return mStart ? "mobile_start_screen_share" : "mobile_stop_screen_share";
    }
    std::string description() const override {
        return mStart
            ? "Start iOS 27+ system-approved screen sharing and save a screenshot every 2 seconds "
              "in this Session. Before opening the picker, tell the user to choose Share Entire Screen "
              "and then tap the gray area outside the system panel to close it without stopping sharing. "
              "The user can visit the pages they need, then stop sharing. Sampling is automatic: do not "
              "repeatedly call capture or restart sharing. After stopping, use mobile_screen_frames to "
              "list the saved images and ask the user which ones they need."
            : "Stop this Session's screen sharing while preserving its screenshots. Return the saved "
              "batch and count, then list its screenshots with mobile_screen_frames and let the user "
              "choose. Stopping must not delete the captured images.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return false; }
    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        const Json args = Json::parse(argumentsJson, nullptr, false);
        if (!args.is_object() || !args.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "screen sharing expects an empty object");
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen sharing was canceled");
        if (context.root.empty() || context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "the Session workspace is unavailable");
        return callMaiMobileHostTool(mDispatcher, name(), Json{
            {"workspace_path", context.root}, {"session_id", context.sessionId}}.dump());
    }
private:
    bool mStart;
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

class MaiMobileCameraCommandTool final : public MaiTool {
public:
    explicit MaiMobileCameraCommandTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}
    std::string name() const override { return "maichat_camera_command"; }
    std::string description() const override {
        return "Open or close the iPhone camera preview. While open, a frame is saved every 2 seconds "
               "in the app sandbox, without adding it to the system photo library. status reports progress. "
        "close may return stopping while saved frames are being finalized. Call status until it reports "
        "captured before using mobile_camera_frames to list timestamps and inspect only the user's selected image. "
        "If status reports unavailable or permission_denied, report the failure.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["open","status","close"]}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return false; }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        Json args = Json::parse(arguments, nullptr, false);
        if (!args.is_object() || args.size() != 1 || !args.contains("action") || !args["action"].is_string() ||
            (args["action"] != "open" && args["action"] != "status" && args["action"] != "close"))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid camera command");
        if (context.isCanceled()) return MaiToolResult::failure(MaiErrorCode::Canceled, "camera command canceled");
        if (context.root.empty() || context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "camera Session workspace is unavailable");
        args["workspace_path"] = context.root;
        args["session_id"] = context.sessionId;
        return callMaiMobileHostTool(mDispatcher, name(), args.dump());
    }
private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};
#endif

class MaiMobileHostTool final : public MaiTool {
public:
    MaiMobileHostTool(std::string name, std::string description, std::string schema,
                      std::shared_ptr<MaiMobileHostDispatcher> dispatcher, bool approval)
        : mName(std::move(name)),
          mDescription(std::move(description)),
          mSchema(std::move(schema)),
          mDispatcher(std::move(dispatcher)),
          mApproval(approval) {}

    std::string name() const override { return mName; }
    std::string description() const override { return mDescription; }
    std::string parametersSchema() const override { return mSchema; }
    bool requiresApproval(const std::string&) const override { return mApproval; }
    bool requiresPerCallApproval(const std::string&) const override { return mApproval; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (mName == "mobile_beautify_image" || mName == "mobile_reshape_body") {
            if (context.isCanceled())
                return MaiToolResult::failure(MaiErrorCode::Canceled, "Portrait edit was canceled");
            const char* operation = mName == "mobile_beautify_image" ? "beautify" : "reshape_body";
            char* invalid = maiPortraitValidateArguments(operation, argumentsJson.c_str(), 0);
            if (invalid) {
                const std::string detail(invalid);
                maiPortraitFree(invalid);
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, detail);
            }
            auto request = Json::parse(argumentsJson);
            if (!context.root.empty()) request["workspace_path"] = context.root;
            if (!context.sessionId.empty()) request["session_id"] = context.sessionId;
            return mDispatcher->call(mName, request.dump());
        }
        return mDispatcher->call(mName, argumentsJson);
    }

private:
    std::string mName;
    std::string mDescription;
    std::string mSchema;
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
    bool mApproval = false;
};

bool validBrowserUrl(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    if (url.empty() || url.size() > 2048 ||
        (url.compare(0, 7, "http://") != 0 &&
         url.compare(0, 8, "https://") != 0) ||
        schemeEnd == std::string::npos)
        return false;
    const std::size_t hostBegin = schemeEnd + 3;
    const std::size_t hostEnd = url.find_first_of("/?#", hostBegin);
    const std::string authority = url.substr(hostBegin, hostEnd - hostBegin);
    return !authority.empty() && authority.find('@') == std::string::npos &&
           authority.find('\\') == std::string::npos &&
           url.find('\0') == std::string::npos;
}

class MaiMobileBrowserTool final : public MaiTool {
public:
    explicit MaiMobileBrowserTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}

    std::string name() const override {
        return "mobile_browser";
    }
    std::string description() const override {
        // 普通网页正文优先使用 curl 的 text_only；浏览器用于脚本渲染、页面交互和截图。
        // 网页属于当前会话，由宿主控制任务入口与展示。操作前读取快照，操作后再次读取验证，
        // 密码由用户手动输入；不能控制 MaiChat 外的浏览器。
        return "Control one browser page owned by the current MaiChat Agent Session. "
               "Prefer curl with text_only=true for ordinary web reading. Use this browser "
               "when JavaScript rendering, page interaction or screenshots are needed. "
               "open starts an in-app page; the host controls its presentation. "
               "observe returns bounded page text and "
               "interactive elements, screenshot returns the current page image, and close "
               "ends the page. To click or type, first observe and use its snapshot_id, "
               "element_id, exact label and URL. Screenshot also needs that observed URL. "
#if defined(MAICHAT_MOBILE_IOS)
               "On iOS, observe waits up to wait_ms (default 5000, maximum 10000) for usable DOM, "
               "then returns ready or loading with partial content. Snapshot and element IDs remain "
               "usable for unchanged controls in the same document, including independent form "
               "inputs; navigation or changed targets need a new observe. Target errors include "
               "a specific reason such as snapshot_expired, element_detached or label_mismatch. "
               "A file control returns file_selection_required; ask the user to open the page "
               "and select the upload through the system picker, then observe again. "
#endif
               "Password fields require manual user input. "
               "Opening a URL, screenshots, clicks and typing follow the Agent approval policy. "
               "A dispatched click or typing action does not prove the intended outcome; "
               "call observe again to verify it. This tool cannot control Safari, Chrome or "
               "other apps outside MaiChat.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("action":{"type":"string","enum":["open","observe","screenshot","click","type","close"]},)"
               R"("url":{"type":"string","maxLength":2048},)"
               R"("expected_url":{"type":"string","maxLength":2048},)"
               R"("snapshot_id":{"type":"string"},"element_id":{"type":"string"},)"
               R"("expected_label":{"type":"string"},)"
               R"("text":{"type":"string","maxLength":4000})"
#if defined(MAICHAT_MOBILE_IOS)
               R"(,"wait_ms":{"type":"integer","minimum":0,"maximum":10000})"
#endif
               R"(},)"
               R"("required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        return requiresPerCallApproval(raw);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string()) return true;
        const std::string action = args["action"].get<std::string>();
        return action != "observe" && action != "close";
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "browser operation was canceled");
        if (context.sessionId.empty() || context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "an Agent Session workspace is required for the browser");
        Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "browser action is required");
        const std::string action = args["action"].get<std::string>();
        const auto hasOnly = [&args](std::initializer_list<const char*> allowed) {
            for (auto field = args.begin(); field != args.end(); ++field) {
                bool known = false;
                for (const char* name : allowed) known |= field.key() == name;
                if (!known) return false;
            }
            return true;
        };
        if (action == "open") {
            if (!hasOnly({"action", "url"}) || !args.value("url", Json{}).is_string() ||
                !validBrowserUrl(args["url"].get<std::string>()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "open needs one HTTP(S) URL without credentials");
        }
#if defined(MAICHAT_MOBILE_IOS)
        else if (action == "observe") {
            if (!hasOnly({"action", "wait_ms"}) ||
                (args.contains("wait_ms") &&
                 (!args["wait_ms"].is_number_integer() || args["wait_ms"].get<int64_t>() < 0 ||
                  args["wait_ms"].get<int64_t>() > 10000)))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "observe wait_ms must be an integer from 0 to 10000");
        }
#endif
        else if (action == "observe" || action == "close") {
            if (!hasOnly({"action"}))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "this browser action accepts no other parameters");
        } else if (action == "screenshot") {
            if (!hasOnly({"action", "expected_url"}) || !args.value("expected_url", Json{}).is_string() ||
                args["expected_url"].get<std::string>().empty() ||
                args["expected_url"].get<std::string>().size() > 2048)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "screenshot needs the exact observed URL");
        } else if (action == "click" || action == "type") {
            if (!hasOnly(action == "click"
                             ? std::initializer_list<const char*>{"action", "snapshot_id", "element_id",
                                                                  "expected_label", "expected_url"}
                             : std::initializer_list<const char*>{"action", "snapshot_id", "element_id",
                                                                  "expected_label", "expected_url", "text"}) ||
                !args.value("snapshot_id", Json{}).is_string() || !args.value("element_id", Json{}).is_string() ||
                !args.value("expected_label", Json{}).is_string() || !args.value("expected_url", Json{}).is_string() ||
                (action == "type" && !args.value("text", Json{}).is_string()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "use a current browser snapshot and element label");
            for (const char* field : {"snapshot_id", "element_id", "expected_label"})
                if (args[field].get<std::string>().empty() || args[field].get<std::string>().size() > 200)
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput, "browser target is empty or too long");
            if (args["expected_url"].get<std::string>().empty() ||
                args["expected_url"].get<std::string>().size() > 2048)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "browser action needs the exact observed URL");
            if (action == "type" && args["text"].get<std::string>().size() > 4000)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "browser text is too long");
        } else {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "unknown browser action");
        }

        args["session_id"] = context.sessionId;
        if (action == "open") {
            // 宿主用真实调用身份将网页入口插回消息流，不能由模型指定或按 URL 猜位置。
            args["message_id"] = context.messageId;
            args["part_id"] = context.partId;
        }
        if (action != "screenshot") return mDispatcher->call(name(), args.dump());
        if (context.model == "glm-5.3")
            return MaiToolResult::failure(MaiErrorCode::NotSupported,
                                          "the current model cannot inspect browser screenshots");
        MaiFilePath directory = MaiFilePath::fromUtf8(context.root);
#if defined(__ANDROID__)
        directory = directory.append(MaiFilePath::fromUtf8("no_backup"));
#endif
        directory = directory.append(MaiFilePath::fromUtf8("browser"));
        const MaiError created = MaiFileSystem::createDirectories(directory);
        if (created.hasError()) return MaiToolResult::failure(created.code(), created.message());
        const MaiFilePath path = directory.append(MaiFilePath::fromUtf8(
            MaiIdGenerator::generate("browser_") + ".png"));
        args["output_path"] = path.toUtf8();
        const MaiToolResult result = mDispatcher->call(name(), args.dump());
        if (result.hasError()) {
            MaiFileSystem::removeFile(path);
            return result;
        }
        std::uint64_t bytes = 0;
        std::string header;
        bool clipped = false;
        const MaiError read = MaiFileSystem::readFile(path, header, 8, &clipped);
        if (!MaiFileSystem::fileSize(path, bytes) || bytes == 0 ||
            bytes > 20 * 1024 * 1024 || read.hasError() ||
            header.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) {
            MaiFileSystem::removeFile(path);
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "browser screenshot was not saved as a bounded PNG");
        }
        return MaiToolResult::successWithImages(result.output(),
                                                {MaiToolImage{path.toUtf8(), "image/png"}});
    }

private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

class MaiMobileScreenCaptureTool final : public MaiTool {
public:
    explicit MaiMobileScreenCaptureTool(std::shared_ptr<MaiMobileHostDispatcher> dispatcher)
        : mDispatcher(std::move(dispatcher)) {}

    std::string name() const override { return "mobile_capture_screen"; }
    std::string description() const override {
#if defined(MAICHAT_MOBILE_IOS)
        return "Capture MaiChat's current visible window with target=current_app; it does not "
               "include other apps or require starting system screen sharing. Use delay_ms "
               "(0 to 30000) only when the user needs time to switch to the desired MaiChat page. "
               "For another app, omit target or use last_external to read the recent whole-device "
               "frame from a screen-sharing session the user started and visited that app in. "
               "Returning to MaiChat does not overwrite that external frame. If no frame is ready, "
               "let the user visit the target app; do not repeatedly restart sharing. "
               "Screen sharing already saves frames every 2 seconds; after stopping, use "
               "mobile_screen_frames to list and open the saved images instead of this live-frame tool.";
#else
        return "Read the most recent whole-device screen frame from a screen-sharing session "
               "that the user started with mobile_start_screen_share. This can include other "
               "apps. If sharing is not active or there is no recent frame, ask the user to "
               "start sharing and visit the target app first.";
#endif
    }
    std::string parametersSchema() const override {
#if defined(MAICHAT_MOBILE_IOS)
        return R"({"type":"object","properties":{"target":{"type":"string","enum":["current_app","last_external"]},)"
               R"("delay_ms":{"type":"integer","minimum":0,"maximum":30000}},"additionalProperties":false})";
#else
        return R"({"type":"object","properties":{},"additionalProperties":false})";
#endif
    }
    bool requiresApproval(const std::string&) const override { return false; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const Json arguments = Json::parse(argumentsJson, nullptr, false);
#if defined(MAICHAT_MOBILE_IOS)
        if (!arguments.is_object())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "mobile_capture_screen expects an object");
        for (auto field = arguments.begin(); field != arguments.end(); ++field) {
            if (field.key() == "target") {
                if (!field.value().is_string() ||
                    (field.value() != "current_app" && field.value() != "last_external"))
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "target must be current_app or last_external");
            } else if (field.key() == "delay_ms") {
                if (!field.value().is_number_integer() || field.value() < 0 || field.value() > 30000)
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "delay_ms must be an integer between 0 and 30000");
            } else {
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unknown screen capture argument");
            }
        }
#else
        if (!arguments.is_object() || !arguments.empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "mobile_capture_screen expects an empty object");
#endif
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        if (context.root.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "the Agent workspace is not configured");
#if defined(MAICHAT_MOBILE_IOS)
        // The host callback is synchronous and cannot observe this turn's
        // cancellation flag. Wait here so stopping a delayed capture never
        // reaches UIKit, while ordinary Swift callers retain their async delay.
        const auto captureAt = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(arguments.value("delay_ms", 0));
        while (std::chrono::steady_clock::now() < captureAt) {
            if (context.isCanceled())
                return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
#endif
        MaiFilePath directory = MaiFilePath::fromUtf8(context.root);
#if defined(__ANDROID__)
        // Android's host accepts only files excluded from OS backup. The Agent
        // root is the app data directory, while getNoBackupFilesDir() is its child.
        directory = directory.append(MaiFilePath::fromUtf8("no_backup"));
#endif
        directory = directory.append(MaiFilePath::fromUtf8("screenshots"));
        const MaiError directoryError = MaiFileSystem::createDirectories(directory);
        if (directoryError.hasError())
            return MaiToolResult::failure(directoryError.code(), directoryError.message());
        const MaiFilePath path = directory.append(MaiFilePath::fromUtf8(
            MaiIdGenerator::generate("mobile_screen_") + ".png"));
        Json payload = arguments;
#if defined(MAICHAT_MOBILE_IOS)
        payload["delay_ms"] = 0;
        payload["session_id"] = context.sessionId;
#endif
        payload["output_path"] = path.toUtf8();
        const MaiToolResult response = callMaiMobileHostTool(mDispatcher, name(), payload.dump());
        if (context.isCanceled()) {
            MaiFileSystem::removeFile(path);
            return MaiToolResult::failure(MaiErrorCode::Canceled, "screen capture was canceled");
        }
        if (response.hasError()) return response;
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "screen capture did not write a PNG file");
        return MaiToolResult::successWithImages(response.output(),
                                                {MaiToolImage{path.toUtf8(), "image/png"}});
    }

private:
    std::shared_ptr<MaiMobileHostDispatcher> mDispatcher;
};

}  // namespace

std::shared_ptr<MaiMobileHostDispatcher> makeMaiMobileHostDispatcher() {
    return std::make_shared<MaiMobileHostDispatcher>();
}

bool setMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                 void* context, MaiMobileHostToolHandler handler,
                                 MaiMobileHostToolResponseFree responseFree,
                                 MaiMobileHostToolContextRelease contextRelease) {
    return dispatcher->set(context, handler, responseFree, contextRelease);
}

void clearMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    dispatcher->clear();
}

MaiToolResult callMaiMobileHostTool(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                   const std::string& name, const std::string& argumentsJson) {
    return dispatcher->call(name, argumentsJson);
}

void addMaiMobileHostTool(MaiToolRegistry& tools, const char* name, const char* description,
                          const char* schema,
                          const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                          bool approval) {
    tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher, approval));
}

// 集中声明聊天、诊断和媒体宿主工具，保持名称、入参 Schema 与宿主处理器一一对应。
// 具有外部副作用的入口要求审批，注册时不能因同组只读工具而放宽权限。
void registerMaiChatHostTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    tools.add(makeMaiCurrentAccountTool([dispatcher](const std::string& arguments, const MaiToolContext&) {
        return callMaiMobileHostTool(dispatcher, "maichat_get_current_account", arguments);
    }));
    tools.add(makeMaiSetAvatarTool(
        [dispatcher](const std::string& arguments, const MaiToolContext&) {
            return callMaiMobileHostTool(dispatcher, "maichat_set_avatar", arguments);
        },
        [dispatcher](const std::string& requestId) {
            // 仅由执行上下文取消触发，不注册为模型工具，不携带账号凭据。
            callMaiMobileHostTool(dispatcher, "maichat_cancel_avatar", Json{{"request_id", requestId}}.dump());
        }));
    // 密码工具只传递安全表单的无凭据状态；三端使用同一个 factory 拒绝密码参数及结果泄漏。
    tools.add(makeMaiChangePasswordTool([dispatcher](const std::string& arguments,
                                                    const MaiToolContext&) {
        return callMaiMobileHostTool(dispatcher, "maichat_change_password", arguments);
    }));
    const auto add = [&](const char* name, const char* description, const char* schema,
                         bool approval = false) {
        tools.add(std::make_unique<MaiMobileHostTool>(name, description, schema, dispatcher,
                                                      approval));
    };
    // MaiChat 内部联系人与手机系统通讯录是两套数据：以下 maichat_* 只查 IM 会话。
    add("maichat_list_contacts",
        "List MaiChat contacts. Use query to filter by user ID or display name.",
        // query 按用户 ID 或显示名筛选；limit 限本次最多返回 200 人。
        R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}}})");
    add("maichat_list_groups",
        "List joined MaiChat group conversations. Use query to filter by group ID or name.",
        R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}},"additionalProperties":false})");
    add("maichat_create_group",
        "Create a MaiChat text group with 1 to 19 accepted friends. The owner is added "
        "automatically. Approval follows the Agent policy; show the group name and members "
        "when approval is requested.",
        R"({"type":"object","properties":{"name":{"type":"string","minLength":1},)"
            R"("member_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
            R"("maxItems":19,"uniqueItems":true}},"required":["name","member_ids"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_get_group_messages",
        "Read recent text messages from a joined MaiChat group without changing the open chat.",
        R"({"type":"object","properties":{"group_id":{"type":"string"},)"
            R"("limit":{"type":"integer","minimum":1,"maximum":20}},)"
            R"("required":["group_id"],"additionalProperties":false})");
    add("maichat_request_friend",
        "Send a MaiChat friend request by phone number or account ID. "
        "Approval follows the Agent policy. "
        "The recipient must accept.",
        R"({"type":"object","properties":{"account":{"type":"string"}},"required":["account"]})",
        true);
    add("maichat_set_nickname",
        "Set or clear the current MaiChat account nickname. Empty string clears it. "
        "Approval follows the Agent policy.",
        R"({"type":"object","properties":{"nickname":{"type":"string","maxLength":32}},)"
            R"("required":["nickname"]})",
        true);
#if defined(__ANDROID__)
    // 头像由上面的三端共用 factory 注册，Android 不再覆盖其 path/取消协议。
    // 只打开设置页共用的注销安全表单；密码、验证码、账号与校验回执不经过模型工具。
    // 真正注销必须由用户在本地验证身份并再次确认，宿主工具没有直接删除账号的入口。
    add("maichat_request_account_deletion",
        "Open the current MaiChat account's secure account deletion form. "
        "The user must verify locally and confirm deletion in that form. This tool only opens "
        "the form and accepts no passwords, verification codes, account identifiers or receipt tokens. "
        "Approval follows the Agent policy.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        true);
    // 只读报告与设置页复用同一账号服务；工具不创建另一条 HTTP 路径。
    add("maichat_list_safety_reports",
        "List safety reports for the current MaiChat account using the same service as Settings.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
#endif
    add("maichat_list_conversations",
        "List direct MaiChat conversations with contacts, ordered by latest message and unread "
        "count. Use maichat_list_groups for group conversations.",
        // 会话按最新消息排序；limit 只限制返回条数，未读数来自会话记录。
        R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}}})");
    add("maichat_get_messages",
        "Read recent messages in a MaiChat conversation with a specific contact.",
        // peer_id 必须是指定联系人；limit 限这一次读取的最近消息条数。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"limit":{"type":"integer","minimum":1,)"
            R"("maximum":200}},"required":["peer_id"]})");
    add("maichat_search_messages",
        "Search MaiChat message text across all contacts or within one contact.",
        // query 是必需的检索词；peer_id 可选，传入后只查该联系人。
        R"({"type":"object","properties":{"query":{"type":"string"},"peer_id":{"type":"string"},)"
            R"("limit":{"type":"integer","minimum":1,"maximum":200}},"required":["query"]})");
    add("maichat_get_unread_summary", "Summarize unread MaiChat messages by contact.",
        // 不收参数，按联系人汇总未读消息。
        R"({"type":"object","properties":{}})");
    add("maichat_remote_session_list",
        "List Sessions on other online MaiChat devices, including devices with no Sessions. "
        "Use this to find the destination device slot before creating or deleting a Session.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
    add("maichat_remote_session_create",
        "Create a Session on another online MaiChat device. Omit workspace_path to use that "
        "device's default Workspace; on Desktop an explicit path must already exist there.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("workspace_path":{"type":"string","maxLength":4096}},)"
            R"("required":["device_slot"],"additionalProperties":false})");
    add("maichat_remote_session_delete",
        "Permanently delete a Session and its chat history on another online MaiChat device. "
        "Approval follows the Agent policy.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("session_id":{"type":"string"}},"required":["device_slot","session_id"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_remote_session_set_workspace",
        "Set an existing Session's Workspace on another online Desktop device. The path must "
        "be absolute and already exist on that device. Approval follows the Agent policy.",
        R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
            R"("session_id":{"type":"string"},"workspace_path":{"type":"string","maxLength":4096}},)"
            R"("required":["device_slot","session_id","workspace_path"],)"
            R"("additionalProperties":false})",
        true);
    // 用户明确要求排障时才导出；只返回工作区 TXT 路径，日志正文不回灌给模型，
    // 也不自动发给联系人。AI 会话据此显示可分享的文件卡片。
    add("mobile_export_diagnostics",
        "Export this device's privacy-filtered MaiChat diagnostic log as a new TXT file in "
        "the Agent workspace. Call only when the user asks for logs or troubleshooting. "
        "Return only the file path and size, never the log contents. A shareable file card "
        "appears in the current AI conversation; this tool does not send it to anyone.",
        R"({"type":"object","properties":{},"additionalProperties":false})");
    // 普通模式下发送消息须确认；完全访问按用户选定的权限策略执行。
    add("maichat_send_text",
        "Send text to a MaiChat contact through the host. Approval follows the Agent policy.",
        // peer_id 与 text 均必填；权限闸门按当前 Agent 策略处理发送。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"text":{"type":"string"}},"required":["peer_id",)"
            R"("text"]})",
        true);
    add("maichat_send_group_text",
        "Send text to a joined MaiChat group. Approval follows the Agent policy; show the "
        "group and full text when approval is requested.",
        R"({"type":"object","properties":{"group_id":{"type":"string"},)"
            R"("text":{"type":"string","minLength":1}},"required":["group_id","text"],)"
            R"("additionalProperties":false})",
        true);
    add("maichat_send_media",
        "Send an accessible Agent workspace image or video as a persistent media bubble, "
        "or audio as a file card. The optional caption stays in the same media message. "
        "Approval follows the Agent policy.",
        // file_path 指 Agent 可访问媒体；type 为 image/video/audio；caption 另发文本。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"file_path":{"type":"string"},)"
            R"("type":{"type":"string","enum":["image","video","audio"]},"caption":{"type":"string"}},)"
            R"("required":["peer_id","file_path","type"],"additionalProperties":false})",
        true);
    add("maichat_reply_message",
        "Send a quoted reply to a MaiChat message through the host. "
        "Approval follows the Agent policy.",
        // 回复要同时给联系人、原消息 ID 和回复正文；权限按当前策略核对。
        R"({"type":"object","properties":{"peer_id":{"type":"string"},"message_id":{"type":"string"},)"
            R"("text":{"type":"string"}},"required":["peer_id","message_id","text"]})",
        true);
    add("maichat_broadcast_text",
        "Send the same text to multiple MaiChat contacts through the host. The complete recipient list and text "
        "follow the Agent approval policy.",
        // peer_ids 为本次完整收件人列表，1–200 人；需要审批时展示名单和正文。
        R"({"type":"object","properties":{"peer_ids":{"type":"array","items":{"type":"string"},"minItems":1,)"
        R"("maxItems":200},"text":{"type":"string"}},"required":["peer_ids","text"]})",
        true);
#if defined(__ANDROID__)
    // 电话号码和系统短信权限只在 Android 注册；iOS 接收用户的快捷指令输入。
    // permission 只允许列出的系统能力；Android 比 iOS 多号码和短信两项。
    constexpr const char* permissionSchema =
        R"({"type":"object","properties":{"permission":{"type":"string","enum":["photos","camera","microphone",)"
        R"("location","contacts","phone_call","calendar","notifications",)"
        R"("phone_number","sms"]}},"required":["permission"],)"
        R"("additionalProperties":false})";
#else
    constexpr const char* permissionSchema =
        R"({"type":"object","properties":{"permission":{"type":"string","enum":["photos","camera","microphone",)"
        R"("location","contacts","calendar","notifications"]}},"required":["permission"],)"
        R"("additionalProperties":false})";
#endif
    // 先按任务所需申请单项 OS 权限；有限授权可继续读取被用户选中的媒体或联系人。
    add("mobile_request_permission",
        "Ask the operating system for a MaiChat permission when a task needs it. "
        "Use photos before reading the system gallery. Ask only for the capability needed by "
        "the current task. Returns status, whether the system prompted, whether Settings is "
        "required, and the access level (including approximate or precise location). A limited "
        "photo grant can still expose selected items; write-only calendar access can create "
        "events. Location is foreground-only.",
        permissionSchema);
#if defined(__ANDROID__)
    // Android 号码可能由运营商留空，读取结果不能当作已验证身份。
    add("mobile_get_phone_number",
        "Read the Android device's default subscription phone number after phone_number permission "
        "is granted. The carrier may provide no number, and a returned number is not verified.",
        // 无其他参数；先调用 mobile_request_permission(phone_number)。
        R"({"type":"object","properties":{},"additionalProperties":false})");
#endif
#if defined(__ANDROID__) || defined(MAICHAT_MOBILE_IOS)
    // 短信属于敏感数据：仅在用户明确要求且授权后读取，不为普通任务遍历短信。
    add("mobile_read_sms",
#if defined(__ANDROID__)
        "Read up to 20 recent Android SMS inbox messages only when the user specifically asks, "
        "after sms permission is granted. READ_SMS is restricted by Android and may be unavailable "
        "depending on the installer. Do not use for OTP retrieval without an explicit user request.",
#else
        "Read up to 20 recent messages explicitly supplied by the user's Shortcuts message "
        "automation on this iPhone, only when the user specifically asks. iOS does not allow "
        "MaiChat to read the Messages inbox or its history. The user must enable SMS integration "
        "in MaiChat Settings and configure a Message automation using the MaiChat SMS import "
        "action in Shortcuts. Imported messages expire after 24 hours and are stored only on "
        "this device; requested tool results enter the model task. No sms OS permission "
        "exists on iOS. An empty result only means no imported messages match, not an empty "
        "system inbox. Do not use for OTP retrieval without an explicit user request.",
#endif
        // limit 最多 20 条；after_ms 只取该时间之后的消息。
        // Android 需要系统 sms 授权；iOS 只读取用户快捷指令交入的内容。
        R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":20},)"
        R"("after_ms":{"type":"integer","minimum":0}},"additionalProperties":false})");
#endif
    // 定位工具只在前台读一次当前位置；没有权限先调用 mobile_request_permission。
    add("mobile_get_location",
        "Read one current foreground location after location permission is granted. "
        "Returns latitude, longitude, horizontal accuracy, optional altitude, timestamp, "
        "and source. If permission is missing, call mobile_request_permission first.",
        // 无参数；权限和前台状态由宿主核对，返回经纬度、精度与时间戳。
        R"({"type":"object","properties":{},"additionalProperties":false})");
    // 系统通讯录工具是只读的：先授权，再分页搜索，最后按返回的 id 取单人详情。
    // iOS 有限授权时只能看到用户选中的联系人，不能把空结果解释为通讯录不存在。
    add("mobile_list_contacts",
        "List or search the phone's system contacts visible to MaiChat after contacts permission. "
        "This is different from maichat_list_contacts. Returns names, phone numbers, and email "
        "addresses in pages of at most 50; query matches these fields. A limited iOS grant "
        "exposes only the selected contacts. Call mobile_request_permission first.",
        // query 可匹配姓名、号码、邮箱；offset/limit 分页，单页最多 50 人。
        R"({"type":"object","properties":{"query":{"type":"string"},"offset":{"type":"integer","minimum":0,)"
        R"("maximum":1000},"limit":{"type":"integer","minimum":1,"maximum":50}},"additionalProperties":false})");
    add("mobile_get_contact",
        "Read one accessible system contact by the id returned from mobile_list_contacts. "
        "Requires contacts permission and returns only name, phone numbers, and email addresses.",
        // id 来自 mobile_list_contacts，不接受随意构造的联系人字段组合。
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"],"additionalProperties":false})");
#if defined(__ANDROID__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    // Android 拨号需单独授予 CALL_PHONE；iOS 仍由系统确认，返回值不代表已接通。
    add("mobile_dial_phone",
#if defined(__ANDROID__)
        "Place a regular phone call on Android only when the user explicitly asks. "
        "For a named contact, use the contacts tools to find its number first. "
        "If a contact has multiple phone numbers, ask which one to use. "
        "The first call may request Android's CALL_PHONE permission; later calls reuse the grant "
        "without another MaiChat approval. A call_requested status does not prove connection.",
#else
        "Open iOS Phone confirmation for one number only when the user explicitly asks to call. "
        "The user confirms the call in iOS. Opening the confirmation does not mean the call was placed.",
#endif
        R"({"type":"object","properties":{"phone_number":{"type":"string","minLength":3,)"
            R"("maxLength":32}},"required":["phone_number"],"additionalProperties":false})",
        false);
#endif
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    add("mobile_compose_sms",
        "Open the iOS Messages composer with recipients and body only when the user explicitly "
        "asks to send an SMS. The user reviews and taps Send in the system composer. "
        "A returned composer_opened status never means the message was sent or delivered.",
        R"({"type":"object","properties":{"recipients":{"type":"array","items":{"type":"string"},)"
            R"("minItems":1,"maxItems":10},"body":{"type":"string","minLength":1,)"
            R"("maxLength":2000}},"required":["recipients","body"],"additionalProperties":false})",
        false);
#endif
    // 会话内视频预览和 IM 视频消息走同一个播放器；控制指令只操作当前打开的实例。
    // 创作中心打开视频时播放器固定在上半屏，下半屏继续输入自然语言控制当前视频。
    add("maichat_play_video",
        "Open a video from the Agent workspace in MaiChat's FFplay/Graphics player. "
        "The player occupies the upper part of the screen while chat stays usable below, so "
        "the user can ask for playback changes while it runs. Set start_seconds to begin at a "
        "specific source-media time, instead of opening then immediately seeking. "
        "IM video messages use the same player.",
        // path 是当前 Agent 可访问的现有视频；start_seconds 是可选的原片绝对起播秒数。
        // 直接从指定位置打开比“先打开、立刻再调用 seek”更稳定。
        R"({"type":"object","properties":{"path":{"type":"string"},"start_seconds":{"type":"number","minimum":0}},)"
            R"("required":["path"],"additionalProperties":false})");
    add("maichat_video_command",
        "Control the currently open FFplay video. set_speed accepts 0.25, 0.5, 1, 1.5, or 2 "
        "and changes audio tempo without changing pitch using SoundTouch. It briefly restarts the same "
        "player at the source position without creating a new file. If the user asks to jump to "
        "a specific time such as 8 seconds, call seek_seconds with seconds=8 on the currently "
        "open video; do not reopen it. For 'replay', 'restart', or 'play from the beginning', "
        "use replay, which restarts the current video at 0 seconds with audio. "
        "seek_seconds jumps to an "
        "absolute source-media time; existing play, pause, step, relative/chapter/percent seek, "
        "stream/filter cycling, mute, volume, fullscreen, and close remain available.",
        // 用户说“跳到第 8 秒”就对当前播放实例调用 seek_seconds(seconds=8)，无需重新打开。
        // replay 在当前播放器中从 0 秒重新播放；seek_seconds(0) 也按重播处理。
        // set_speed 带 speed 倍数；seek_seconds 带源片绝对秒数；seek_percent 带百分比。
        // 改速仍在现有播放器中播放同一文件；SoundTouch.setTempo 保持音调，不另存转码文件。
        R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","toggle_pause","replay",)"
            R"("step","seek_forward","seek_backward","seek_minute_forward","seek_minute_backward","next_chapter",)"
            R"("previous_chapter","seek_percent","seek_seconds","set_speed","next_audio","next_video","next_subtitle",)"
            R"("next_program","next_filter","mute","volume_up","volume_down","fullscreen","close"]},)"
            R"("percent":{"type":"number","minimum":0,"maximum":100},"seconds":{"type":"number","minimum":0},)"
            R"("speed":{"type":"number","enum":[0.25,0.5,1,1.5,2]}},"required":["action"],"additionalProperties":false)"
            R"(})");
}

void registerMobileScreenCaptureTools(
    MaiToolRegistry& tools, const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
#if defined(MAICHAT_MOBILE_IOS)
    tools.add(std::make_unique<MaiMobileScreenShareLifecycleTool>(true, dispatcher));
    tools.add(std::make_unique<MaiMobileScreenShareLifecycleTool>(false, dispatcher));
    tools.add(std::make_unique<MaiMobileScreenFramesTool>(dispatcher));
#else
    addMaiMobileHostTool(
        tools, "mobile_start_screen_share",
        "Ask the user to start a system-approved whole-device screen-sharing session. "
        "On Android choose Share entire screen in the system picker. "
        "The user can then switch to another app and return before requesting a frame.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false);
    addMaiMobileHostTool(
        tools, "mobile_stop_screen_share",
        "Stop the active whole-device screen-sharing session and discard its cached frame.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false);
#endif
    tools.add(std::make_unique<MaiMobileScreenCaptureTool>(dispatcher));
}

void registerMobileBrowserTools(
    MaiToolRegistry& tools, const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    tools.add(std::make_unique<MaiMobileBrowserTool>(dispatcher));
}

// 移动端相册和播放器工具复用同一个宿主调度器；具体权限与媒体生命周期由手机宿主处理。
// Schema 限定操作和范围，模型不能凭工具参数请求任意原生调用。
void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
#if defined(MAICHAT_MOBILE_IOS)
    tools.add(std::make_unique<MaiMobileCameraFramesTool>(dispatcher));
    tools.add(std::make_unique<MaiMobileCameraCommandTool>(dispatcher));
#else
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_take_photo",
        "Open the device camera preview and wait for the user to take a photo. Save a new JPEG "
        "in the current Agent workspace. Cancel creates no file and never sends a message.",
        R"({"type":"object","properties":{},"additionalProperties":false})",
        dispatcher, false));
#endif
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_play_local_music",
        "Play an MP3, M4A, or WAV with MaiChat's FFplay music player. Provide a relative "
        "workspace path, or omit it so the user chooses the system music library or a file. "
        "Protected Apple Music songs use Apple's system player in the same MaiChat UI. "
        "Never scan the device music library without user selection.",
        R"({"type":"object","properties":{"path":{"type":"string"}},)"
            R"("additionalProperties":false})",
        dispatcher, false));
    // 相册工作流：先授权并列出素材；预览用有界 JPEG，任何编辑/转码都先导出原文件。
    // 处理工具创建新文件；只有用户需要加入相册时再调用保存工具，原素材始终保留。
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_photos",
        "List photos, Live Photos, and videos visible in the system library after OS authorization. "
        "Each item includes mediaType; use offset and limit to page. A limited grant exposes "
        "only the user's selected media.",
        // offset/limit 分页；album_id 可将范围限制在一个相册。
        R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0},"limit":{"type":"integer",)"
            R"("minimum":1,"maximum":100},"album_id":{"type":"string"}}})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_list_albums",
        "List system photo albums available under the current OS photo-library permission.",
        // 无参数，结果仍受系统相册授权范围限制。
        R"({"type":"object","properties":{}})", dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_read_photo",
        // 预览图最多 2048 像素，可能丢掉宽色域信息，不能用它充当编辑源图。
        "Copy one bounded JPEG preview by ID into the Agent working directory (up to 2048px). "
        "This loses original resolution and wide-gamut metadata; never use it as an editing "
        "source. Use mobile_export_photo_original before image processing. Use "
        "mobile_preview_image to show it to the user, or view_image with a vision-capable model. "
        "Only images authorized by the OS photo-library permission can be read.",
        // id 是相册项目 ID；返回的是有界预览副本，不能用作后续编辑输入。
        R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_photo_original",
        // 编辑或给视频模型提交首帧时导出原件；Live Photo 可明确选择静图或配对视频。
        "Copy the original photo or video bytes by ID from the authorized system library into "
        "the Agent working directory, preserving the source format and color metadata. Use this "
        "for any image edit or media conversion. The library item stays unchanged. For an iOS Live Photo, "
        "component=video exports its paired video; otherwise its still photo is exported.",
        // id 是相册项目 ID；Live Photo 的 component=video 取配对视频，省略取静图。
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"])"
            R"(}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_export_media_original",
        "Export the original photo or video file from the authorized system library into the "
        "Agent workspace, preserving its media format. Use this for videos before ffprobe, "
        "ffmpeg, or playback. A Live Photo exports its still image unless component=video.",
        // 与原图导出共用素材 ID/Live Photo component 约定；视频分析应选原始视频。
        R"({"type":"object","properties":{"id":{"type":"string"},"component":{"type":"string","enum":["photo","video"])"
            R"(}},"required":["id"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_image",
        // 保存动作创建新相册项目，不能覆盖输入文件；时间按发起保存命令的时刻写入。
        "Save an image from the Agent working directory as a NEW photo in the system library. "
        "The source file and all existing library photos remain unchanged. Pass a relative path "
        "inside the Agent working directory.",
        // path 是工作区已存在的图片；保存时新建相册项，不覆盖原文件。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_save_video",
        // 视频保存同样只新增相册项目，使用生成工具返回的确切路径。
        "Save a video file from the Agent workspace as a NEW video in the system photo library. "
        "The source file remains unchanged. Accepts MP4, MOV, and M4V up to 2 GB. "
        "Pass the exact workspace path returned by the video tool.",
        // path 要用视频工具返回的准确工作区路径；只接受 MP4/MOV/M4V。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_transform_image",
        // 一次调用只做一种端侧处理；成品交付聊天，用户明确要求后才打开全屏预览。
        "Create a NEW image in the Agent working directory using an on-device edit; the source "
        "is unchanged. Apply one operation per call: crop (top-left x,y,width,height), rotate "
        "(degrees 90/180/270 clockwise), resize (width,height), flip_horizontal, flip_vertical, "
        "grayscale, sharpen (amount 0..2), adjust (brightness -1..1, contrast 0..2, "
        "saturation 0..2), or beautify "
        "(strength 0..1, local face-aware skin processing using the same portrait engine as "
        "mobile_beautify_image). Deliver the returned path with agent_send_media "
        "(file_path=path, type=image) in the current AI conversation. Do not open a full-screen "
        "preview automatically; the user can tap the chat image to inspect it. Use "
        "mobile_preview_image only when explicitly requested, and mobile_save_image to add "
        "the result to the system gallery if requested.",
        // operation 每次只做一项裁剪、旋转、缩放、翻转、灰度、锐化、调色或轻度美化。
        // 其余坐标/强度字段只对所选操作有效，执行层逐项校验范围。
        R"({"type":"object","properties":{"path":{"type":"string"},"operation":{"type":"string","enum":["crop",)"
            R"("rotate","resize","flip_horizontal","flip_vertical","grayscale","sharpen","adjust","beautify"]},)"
            R"("x":{"type":"integer"},"y":{"type":"integer"},"width":{"type":"integer"},"height":{"type":"integer"},)"
            R"("degrees":{"type":"integer"},"brightness":{"type":"number"},"contrast":{"type":"number"},)"
            R"("saturation":{"type":"number"},"strength":{"type":"number"},"amount":{"type":"number"}},)"
            R"("required":["path","operation"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_beautify_image",
        "Create a NEW locally retouched portrait using dense face landmarks and a skin mask. "
        "Smoothing, brightening, whitening, warmth, slimming and eye enlargement are independent. "
        "whitening (default 0) lifts skin lightness and gently reduces skin chroma without an intentional "
        "cool color shift; it affects the selected face skin, excluding eyes, brows and lips, not the entire image. "
        "brightening is RGB gain, not whitening. For whitening-only use explicit smoothing=0, "
        "brightening=0 and warmth=0; strength only controls smoothing and brightening. "
        "Use original_path from prior results when refining; prior generated paths are also "
        "resolved to their original. Explicit parameters override the validated personal model. "
        "Unspecified shape changes default to zero. For multiple faces set subject_index "
        "from left to right starting at 0. Return result_id for user feedback and deliver the "
        "returned path with agent_send_media (file_path=path, type=image) in this AI conversation. "
        "Do not open a full-screen preview automatically; the user can tap the chat image. "
        "Use mobile_preview_image only when explicitly requested. The iOS host automatically processes a bounded face ROI "
        "and composites it at the original dimensions; returned mask is diagnostic, not another blend alpha. "
        "Use skin ROI L*/C* and reported mask coverage, not full-image threshold fractions, to assess changes. "
        "Unknown fields including action/delegate are rejected. Never infer user preference from your own judgment.",
        // 所有处理复用普通 C++ 核心；单独的几何参数默认关闭，strength 保留为旧调用映射。
        R"({"type":"object","properties":{"path":{"type":"string"},"strength":{"type":"number","minimum":0,"maximum":1},)"
        R"("smoothing":{"type":"number","minimum":0,"maximum":1},"brightening":{"type":"number","minimum":0,"maximum":1},)"
        R"("whitening":{"type":"number","minimum":0,"maximum":1,"default":0,"description":"Independent skin lightness lift and gentle chroma reduction; not a warmth adjustment."},)"
        R"("warmth":{"type":"number","minimum":-1,"maximum":1},"slim_face":{"type":"number","minimum":0,"maximum":1},)"
        R"("enlarge_eyes":{"type":"number","minimum":0,"maximum":1},"subject_index":{"type":"integer","minimum":0},)"
        R"("use_personal_model":{"type":"boolean"}},"required":["path"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_reshape_body",
        "Create a NEW locally reshaped photo using body pose and person segmentation. "
        "Supports mild waist slimming and leg proportion extension on clear upright standing "
        "photos. Occluded joints, uncertain poses or no room below the feet return an error. "
        "slim_waist and leg_length are bounded normalized strengths, not literal percentages. "
        "For multiple people specify subject_index from left to right starting at 0. "
        "Keep the original and deliver the new path with agent_send_media (file_path=path, "
        "type=image) in this AI conversation. Do not open a full-screen preview automatically; "
        "the user can tap the chat image. Use mobile_preview_image only when explicitly "
        "requested, and use result_id for explicit user feedback.",
        R"({"type":"object","properties":{"path":{"type":"string"},"slim_waist":{"type":"number","minimum":0,"maximum":1},)"
        R"("leg_length":{"type":"number","minimum":0,"maximum":1},"subject_index":{"type":"integer","minimum":0},)"
        R"("use_personal_model":{"type":"boolean"}},"required":["path"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_portrait_feedback",
        "Record the user's explicit preference between two portrait results from the SAME "
        "original photo. Use returned result_id values. Include ONLY the attributes the user "
        "actually evaluated; unmentioned attributes remain unknown. This stores feedback, "
        "not model training. Use model_continue_training afterward when requested.",
        R"({"type":"object","properties":{"preferred_result_id":{"type":"string"},"rejected_result_id":{"type":"string"},)"
        R"("attributes":{"type":"array","minItems":1,"maxItems":8,"uniqueItems":true,"items":{"type":"string",)"
        R"("enum":["smoothing","brightening","warmth","slim_face","enlarge_eyes","slim_waist","leg_length","whitening"]}}},)"
        R"("required":["preferred_result_id","rejected_result_id","attributes"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "model_continue_training",
        "Train or inspect the CURRENT user's local portrait preference model using saved "
        "explicit result-pair feedback. This updates small ranking weights; it does not train "
        "MediaPipe base networks or implement album semantic search. train saves a checkpoint. "
        "Fewer than three independent original photos produce candidate_only, not an activated "
        "model. Activation requires held-out-photo validation improvement. status/versions "
        "show active and saved versions. rollback with version=0 restores the baseline. "
        "Report the actual returned activation and validation status to the user.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["train","status","versions","rollback"]},)"
        R"("version":{"type":"integer","minimum":0}},"required":["action"],"additionalProperties":false})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_detect_faces",
        "Find faces and facial landmarks in an image from the Agent working directory using "
        "on-device vision. Return top-left pixel bounds and landmark positions for local image "
        "editing; this does not identify who a person is or modify the source image.",
        // path 为现有工作区图；只返回面框与关键点坐标，不返回人物身份。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_segment_person",
        "Create a grayscale person-versus-background mask from an image in the Agent working "
        "directory using on-device vision. White means person and black means background. "
        "Return a NEW mask PNG path and dimensions; do not modify the source image.",
        // path 为输入图；返回独立灰度遮罩，白色为人物、黑色为背景。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    tools.add(std::make_unique<MaiMobileHostTool>(
        "mobile_preview_image",
        "Show an image from the Agent working directory to the user in a full-screen mobile "
        "preview. This does not require a vision-capable model and does not save or modify it. "
        "Use only when the user explicitly asks to open a full-screen preview. Deliver image "
        "editing results in the chat with agent_send_media; do not call this tool automatically "
        "after reading or editing an image.",
        // path 为用户明确要求全屏查看的图片；普通成品交付使用聊天卡片，不自动打开本工具。
        R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        dispatcher, false));
    registerPlatformPhotoAlbumTools(tools, dispatcher);
}
