#include "MaiAccountTools.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"

namespace {

using Json = nlohmann::json;

bool boundedText(const Json& value, size_t maximum, bool emptyAllowed = true) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() <= maximum && (emptyAllowed || !text.empty()) &&
           std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

bool stableAvatarUrl(const Json& value) {
    if (!boundedText(value, 4096, false)) return false;
    const auto& url = value.get_ref<const std::string&>();
    if (url.compare(0, 8, "https://") != 0 || url.find_first_of("?#\\") != std::string::npos) return false;
    const auto path = url.find('/', 8);
    if (path == std::string::npos || path == 8 || url.substr(8, path - 8).find('@') != std::string::npos) return false;
    return url.find(' ') == std::string::npos;
}

MaiToolResult protocolFailure() {
    return MaiToolResult::failure(MaiErrorCode::Protocol, "invalid account profile response");
}

// 资料只做明确字段映射，不转发整个鉴权 JSON；未知字段（包括凭据）即使宿主误传也丢弃。
MaiToolResult safeAccountResult(const MaiToolResult& result) {
    if (result.hasError())
        return MaiToolResult::failure(result.error().code(), "current account profile is unavailable");
    const Json input = Json::parse(result.output(), nullptr, false);
    if (!input.is_object() || !input.contains("logged_in") || !input["logged_in"].is_boolean())
        return protocolFailure();
    if (!input["logged_in"].get<bool>()) return MaiToolResult::success(Json{{"logged_in", false}}.dump());
    if (!input.contains("account_id") || !boundedText(input["account_id"], 128, false) ||
        !input.contains("im_user_id") || !boundedText(input["im_user_id"], 128, false))
        return protocolFailure();
    Json output{{"logged_in", true},   {"account_id", input["account_id"]}, {"im_user_id", input["im_user_id"]},
                {"nickname", nullptr}, {"phone_masked", nullptr},           {"avatar_url", nullptr}};
    if (input.contains("nickname") && !input["nickname"].is_null()) {
        if (!boundedText(input["nickname"], 120)) return protocolFailure();
        if (!input["nickname"].get_ref<const std::string&>().empty()) output["nickname"] = input["nickname"];
    }
    if (input.contains("phone_masked") && !input["phone_masked"].is_null()) {
        if (!boundedText(input["phone_masked"], 64)) return protocolFailure();
        const auto& phone = input["phone_masked"].get_ref<const std::string&>();
        // 旧宿主若误传完整手机号也不向模型泄漏；缺遮罩时以未知处理。
        if (phone.find('*') != std::string::npos) output["phone_masked"] = phone;
    }
    if (input.contains("avatar_url") && !input["avatar_url"].is_null()) {
        if (!input["avatar_url"].is_string()) return protocolFailure();
        // 历史 SDK 可能缓存临时签名地址；查询不能把其中的签名回灌给模型。
        if (stableAvatarUrl(input["avatar_url"])) output["avatar_url"] = input["avatar_url"];
    }
    return MaiToolResult::success(output.dump());
}

// C/Swift/JNI 的同步回调不自动继承 Agent 的取消状态；用仅限宿主的通知取消请求。
// 不复制或保存 context.cancel，其生命周期由 execute 中等待并回收该线程保证。
class AvatarCancelWatch final {
public:
    AvatarCancelWatch(const MaiToolContext& context, const MaiAccountToolCancelHandler& cancel, const std::string& id) {
        if (!context.cancel || !cancel) return;
        mThread = std::thread([this, &context, cancel, id] {
            MaiThread::setCurrentName("mai-avatar-stop");
            std::unique_lock<std::mutex> lock(mMutex);
            while (!mStopped && !context.isCanceled()) mChanged.wait_for(lock, std::chrono::milliseconds(20));
            if (mStopped) return;
            lock.unlock();
            try {
                cancel(id);
            } catch (...) { /* 宿主超时仍由原请求处理，不让取消线程终止进程。 */
            }
        });
    }
    ~AvatarCancelWatch() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopped = true;
        }
        mChanged.notify_all();
        if (mThread.joinable()) mThread.join();
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mStopped = false;
    std::thread mThread;
};

class MaiAccountTool final : public MaiTool {
public:
    MaiAccountTool(bool writesAvatar, MaiAccountToolHandler handler, MaiAccountToolCancelHandler cancel = {})
        : mWritesAvatar(writesAvatar), mHandler(std::move(handler)), mCancel(std::move(cancel)) {}

    std::string name() const override {
        return mWritesAvatar ? "maichat_set_avatar" : "maichat_get_current_account";
    }
    std::string description() const override {
        // 查询当前登录账号的本机资料快照；离线不等于退出，未知资料返回 null。
        // 换头像只接受当前 Agent 可访问的图片，上传后等 SDK 确认，不把入队当完成。
        return mWritesAvatar
                   ? "Change the currently signed-in MaiChat account avatar using an accessible local image path. "
                     "The host prepares a square JPEG, uploads it directly to storage and waits for IM profile "
                     "confirmation. The original image is unchanged. This changes the account profile and follows "
                     "the Agent approval policy. Use only when the user requests an avatar change; return the stable "
                     "avatar_url only after updated=true."
                   : "Read the current signed-in MaiChat account's local profile without changing it. Returns "
                     "logged_in, public account_id, im_user_id, nickname, masked phone and stable avatar_url; "
                     "unknown fields are null. Network disconnection does not mean the account is signed out. "
                     "This is a cached profile snapshot, refreshed by normal account/IM synchronization. "
                     "It never returns passwords, tokens, signing credentials or login session IDs.";
    }
    std::string parametersSchema() const override {
        return mWritesAvatar
                   ? R"({"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":4096}},"required":["path"],"additionalProperties":false})"
                   : R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return mWritesAvatar;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return mWritesAvatar;
    }

    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        const Json input = argumentsJson.size() <= 32 * 1024 ? Json::parse(argumentsJson, nullptr, false) : Json{};
        if (!input.is_object() || (mWritesAvatar ? input.size() != 1 || !input.contains("path") : !input.empty()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "only the documented account tool arguments are allowed");
        if (context.isCanceled()) return MaiToolResult::failure(MaiErrorCode::Canceled, "account request was canceled");
        if (!mHandler)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "account profile host is unavailable");
        Json request = Json::object();
        if (mWritesAvatar) {
            if (!boundedText(input["path"], 4096, false))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "path must be a local image path");
            const auto path = context.resolvePath(input["path"].get<std::string>());
            if (path.empty())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "image path is outside the accessible area");
            std::uint64_t size = 0;
            const auto file = MaiFilePath::fromUtf8(path);
            if (MaiFileSystem::isDirectory(file) || !MaiFileSystem::fileSize(file, size) || size == 0 ||
                size > 20 * 1024 * 1024)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "image must be a readable file within 20 MiB");
            request["path"] = path;
        }
        // 绑定信息由执行上下文注入，模型不能自行声明其他账号或 Session 的文件域。
        if (!context.root.empty()) request["workspace_path"] = context.root;
        if (!context.sessionId.empty()) request["session_id"] = context.sessionId;
        const std::string requestId = mWritesAvatar ? MaiIdGenerator::generate("avatar") : std::string{};
        if (mWritesAvatar) request["request_id"] = requestId;
        AvatarCancelWatch watch(context, mWritesAvatar ? mCancel : MaiAccountToolCancelHandler{}, requestId);
        const auto result = mHandler(request.dump(), context);
        if (!mWritesAvatar) return safeAccountResult(result);
        if (result.hasError()) return MaiToolResult::failure(result.error().code(), "avatar update was not confirmed");
        const Json output = Json::parse(result.output(), nullptr, false);
        if (!output.is_object() || !output.contains("updated") || output["updated"] != true ||
            !output.contains("avatar_url") || !stableAvatarUrl(output["avatar_url"]))
            return protocolFailure();
        return MaiToolResult::success(Json{{"updated", true}, {"avatar_url", output["avatar_url"]}}.dump());
    }

private:
    bool mWritesAvatar;
    MaiAccountToolHandler mHandler;
    MaiAccountToolCancelHandler mCancel;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiCurrentAccountTool(MaiAccountToolHandler handler) {
    return std::make_unique<MaiAccountTool>(false, std::move(handler));
}

std::unique_ptr<MaiTool> makeMaiSetAvatarTool(MaiAccountToolHandler handler, MaiAccountToolCancelHandler cancel) {
    return std::make_unique<MaiAccountTool>(true, std::move(handler), std::move(cancel));
}
