#include "MaiMobileSessionIMBindingTool.h"

#include <json.hpp>

#include <algorithm>
#include <utility>

namespace {

bool identifier(const nlohmann::json& value, bool session) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text.empty() || text.size() > 128 || text.front() == ' ' || text.back() == ' ') return false;
    return std::all_of(text.begin(), text.end(), [session](unsigned char ch) {
        if (session)
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        return ch >= 0x20 && ch != 0x7f;
    });
}

bool parse(const std::string& arguments, bool& mutating) {
    if (arguments.empty() || arguments.size() > 4096) return false;
    const auto value = nlohmann::json::parse(arguments, nullptr, false);
    if (!value.is_object() || !value.contains("action") || !value["action"].is_string()) return false;
    const auto action = value["action"].get<std::string>();
    mutating = action != "list";
    if (!mutating) return value.size() == 1;
    const bool binding = action == "bind";
    return (binding || action == "unbind") && value.size() == (binding ? 3u : 2u) &&
           value.contains("peer_id") && identifier(value["peer_id"], false) &&
           (!binding || (value.contains("session_id") && identifier(value["session_id"], true)));
}

MaiToolResult notStarted(MaiErrorCode code, const std::string& message) {
    MaiToolExecution execution;
    execution.started = false;
    execution.status = code == MaiErrorCode::Canceled ? MaiToolExecutionStatus::Canceled
                                                     : MaiToolExecutionStatus::NotStarted;
    execution.effectStatus = MaiToolEffectStatus::None;
    execution.canceled = code == MaiErrorCode::Canceled;
    execution.nextAction = code == MaiErrorCode::InvalidInput ? MaiToolNextAction::ReviseArguments
                                                             : MaiToolNextAction::None;
    return MaiToolResult::failure(code, message).withExecution(std::move(execution));
}

class BindingTool final : public MaiTool {
public:
    explicit BindingTool(MaiMobileSessionIMBindingHandler handler) : handler_(std::move(handler)) {}
    std::string name() const override { return "maichat_session_im_binding"; }
    std::string description() const override {
        return "List, bind, or unbind this account's accepted friend conversations and existing local Creation "
               "Center Sessions. A binding sends only new Human IM messages to the selected idle Session and "
               "returns its exact response to that friend while this local application is running. One Session "
               "can bind one friend. Bind and unbind follow the normal approval policy. Unbind revokes future "
               "automatic forwarding without stopping the Session; historical messages are never replayed.";
    }
    std::string parametersSchema() const override {
        // Qwen 等供应商从根 properties 发布参数，因此保留 flat object 并在 execute 严格校验动作。
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["list","bind","unbind"]},"peer_id":{"type":"string","minLength":1,"maxLength":128},"session_id":{"type":"string","minLength":1,"maxLength":128}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& arguments) const override {
        bool mutating = false;
        return !parse(arguments, mutating) || mutating;
    }
    bool requiresPerCallApproval(const std::string& arguments) const override { return requiresApproval(arguments); }
    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        bool mutating = false;
        if (!parse(arguments, mutating))
            return notStarted(MaiErrorCode::InvalidInput,
                              "Use exactly action=list, bind with peer_id/session_id, or unbind with peer_id.");
        if (context.isCanceled())
            return notStarted(MaiErrorCode::Canceled, "Session binding operation was canceled before dispatch.");
        if (!handler_)
            return notStarted(MaiErrorCode::NotConfigured, "The local Session binding runtime is unavailable.");
        // handler 直接使用普通实例，不排回宿主串行队列；它负责当前 Agent 身份及变更后取消的真实效果。
        return handler_(arguments, context);
    }
private:
    MaiMobileSessionIMBindingHandler handler_;
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiMobileSessionIMBindingTool(MaiMobileSessionIMBindingHandler handler) {
    return std::make_unique<BindingTool>(std::move(handler));
}
