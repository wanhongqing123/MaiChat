// 文件作用：序列化可信工具执行事实，并保留供应商错误的独立恢复建议。
#include "MaiToolExecution.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>

#include <json.hpp>

namespace {
using Json = nlohmann::json;

const char* executionStatusName(MaiToolExecutionStatus status) {
    switch (status) {
        case MaiToolExecutionStatus::NotStarted: return "not_started";
        case MaiToolExecutionStatus::Running: return "running";
        case MaiToolExecutionStatus::Completed: return "completed";
        case MaiToolExecutionStatus::Failed: return "failed";
        case MaiToolExecutionStatus::Canceled: return "canceled";
        case MaiToolExecutionStatus::Unknown: return "unknown";
    }
    return "unknown";
}

const char* effectStatusName(MaiToolEffectStatus status) {
    switch (status) {
        case MaiToolEffectStatus::None: return "none";
        case MaiToolEffectStatus::Unknown: return "unknown";
        case MaiToolEffectStatus::Confirmed: return "confirmed";
    }
    return "unknown";
}

const char* nextActionName(MaiToolNextAction action) {
    switch (action) {
        case MaiToolNextAction::None: return "none";
        case MaiToolNextAction::ReadOutput: return "read_output";
        case MaiToolNextAction::InspectState: return "inspect_state";
        case MaiToolNextAction::QueryStatus: return "query_status";
        case MaiToolNextAction::ReviseArguments: return "revise_arguments";
        case MaiToolNextAction::AskUser: return "ask_user";
    }
    return "inspect_state";
}

template <typename Enum, typename Name>
bool readEnum(const Json& object, const char* field, Enum& out, std::initializer_list<Enum> values,
              Name name) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_string()) return false;
    for (const auto value : values) {
        if (*found == name(value)) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace

std::string maiSerializeToolExecution(const MaiToolExecution& execution) {
    Json result{{"status", executionStatusName(execution.status)},
                {"effect_status", effectStatusName(execution.effectStatus)},
                {"next_action", nextActionName(execution.nextAction)},
                {"timed_out", execution.timedOut},
                {"canceled", execution.canceled},
                {"output_truncated", execution.outputTruncated}};
    if (execution.started) result["started"] = *execution.started;
    if (execution.exitCode) result["exit_code"] = *execution.exitCode;
    if (!execution.outputId.empty()) result["output_id"] = execution.outputId;
    if (!execution.taskId.empty()) result["task_id"] = execution.taskId;
    if (!execution.recoveryTool.empty()) result["recovery_tool"] = execution.recoveryTool;
    return result.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool maiParseToolExecution(const std::string& source, MaiToolExecution& out) {
    const auto object = Json::parse(source, nullptr, false);
    if (!object.is_object()) return false;
    MaiToolExecution parsed;
    if (!readEnum(object, "status", parsed.status,
                  {MaiToolExecutionStatus::NotStarted, MaiToolExecutionStatus::Running,
                   MaiToolExecutionStatus::Completed, MaiToolExecutionStatus::Failed,
                   MaiToolExecutionStatus::Canceled, MaiToolExecutionStatus::Unknown},
                  executionStatusName) ||
        !readEnum(object, "effect_status", parsed.effectStatus,
                  {MaiToolEffectStatus::None, MaiToolEffectStatus::Unknown,
                   MaiToolEffectStatus::Confirmed},
                  effectStatusName) ||
        !readEnum(object, "next_action", parsed.nextAction,
                  {MaiToolNextAction::None, MaiToolNextAction::ReadOutput,
                   MaiToolNextAction::InspectState, MaiToolNextAction::QueryStatus,
                   MaiToolNextAction::ReviseArguments, MaiToolNextAction::AskUser},
                  nextActionName))
        return false;
    for (const auto& field : {std::make_pair("timed_out", &parsed.timedOut),
                              std::make_pair("canceled", &parsed.canceled),
                              std::make_pair("output_truncated", &parsed.outputTruncated)}) {
        const auto found = object.find(field.first);
        if (found == object.end()) continue;
        if (!found->is_boolean()) return false;
        *field.second = found->get<bool>();
    }
    for (const auto& field :
         {std::make_pair("output_id", &parsed.outputId), std::make_pair("task_id", &parsed.taskId),
          std::make_pair("recovery_tool", &parsed.recoveryTool)}) {
        const auto found = object.find(field.first);
        if (found == object.end()) continue;
        if (!found->is_string()) return false;
        *field.second = found->get<std::string>();
    }
    if (const auto found = object.find("started"); found != object.end()) {
        if (!found->is_boolean()) return false;
        parsed.started = found->get<bool>();
    }
    if (const auto found = object.find("exit_code"); found != object.end()) {
        if (!found->is_number_integer()) return false;
        if (found->is_number_unsigned()) {
            if (found->get<std::uint64_t>() >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                return false;
        } else {
            const auto code = found->get<std::int64_t>();
            if (code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max())
                return false;
        }
        parsed.exitCode = found->get<int>();
    }
    out = std::move(parsed);
    return true;
}

namespace {

// 只把明确的欠费信号归入余额不足；普通网络错误和限流不能按欠费处理。
bool isBalanceFailure(const nlohmann::json& error) {
    if (!error.is_object()) return false;
    const auto providerCode = error.value("provider_code", nlohmann::json{});
    if (providerCode.is_number_integer() && (providerCode == 1008 || providerCode == 1102))
        return true;
    std::string detail;
    if (providerCode.is_string()) detail = providerCode.get<std::string>() + " ";
    for (const char* field : {"message", "error", "reply"}) {
        if (error.contains(field) && error[field].is_string())
            detail += error[field].get<std::string>() + " ";
    }
    std::transform(detail.begin(), detail.end(), detail.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return detail.find("insufficient_balance") != std::string::npos ||
           detail.find("insufficient balance") != std::string::npos ||
           detail.find("balance insufficient") != std::string::npos ||
           detail.find("balance not enough") != std::string::npos ||
           detail.find("account balance") != std::string::npos ||
           detail.find("arrears") != std::string::npos;
}

// 一个供应商欠费不代表整个用户任务结束。保留素材，寻找可用模型；视频换模型前
// 仍需用户选择，已提交任务的查询失败则绝不能重复付费提交。
constexpr const char* kBalanceRecovery =
    "This provider cannot accept more work because its balance is insufficient. Do not stop "
    "the user's task, ask for a top-up, or retry this provider. Keep the original request and "
    "media references. Check other configured specialist tools and continue with a suitable "
    "provider, using the normal paid-call approval for any new submission. For video generation, "
    "confirm a different model choice with the user before its paid call. Report an obstacle "
    "to the user only after practical alternatives have been exhausted. If this error came "
    "from checking an already-submitted task, its outcome is unverified; do not claim that "
    "generation failed or silently submit a duplicate paid task.";

// 把子任务失败状态转换成可回灌给主 Agent 的准确说明。
// 只使用真实错误与已确认的产物，不能把查询异常写成生成失败。
std::string specialistFailureGuidance(const nlohmann::json& specialist) {
    // 子工具失败先把原始结果回灌主模型，再按可观察到的信号附加下一步建议。
    // 下方每个分支只是建议，不把笼统审核标签伪装成已查明的根因。
    if (isBalanceFailure(specialist)) return std::string(" ") + kBalanceRecovery;
    std::string detail;
    for (const char* field : {"message", "error", "reply", "provider_message"}) {
        if (!specialist.contains(field)) continue;
        const nlohmann::json& value = specialist[field];
        if (value.is_string()) detail += value.get<std::string>() + " ";
        if (value.is_object() && value.value("message", nlohmann::json{}).is_string())
            detail += value["message"].get<std::string>() + " ";
    }
    if (specialist.contains("provider_code")) {
        const nlohmann::json& code = specialist["provider_code"];
        if (code.is_string()) detail += code.get<std::string>();
        if (code.is_number_integer()) detail += std::to_string(code.get<std::int64_t>());
    }
    std::transform(detail.begin(), detail.end(), detail.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    if (specialist.value("specialist", std::string{}) == "seedance_video" &&
        (detail.find("real person") != std::string::npos ||
         detail.find("human face") != std::string::npos ||
         detail.find("portrait") != std::string::npos)) {
        // Seedance 报可能有真人时，保留原素材并要求真人授权组；不能降级到 AIGC 组，
        // 也不能通过改脸绕过审核。重试仍须使用 Active 资产和新的付费确认。
        return " The Seedance attempt was rejected for a possible real person. Keep every "
               "original image and video unchanged. Do not register a natural person in an "
               "AIGC group. Use an existing LivenessFace group for that person, or complete "
               "ark_assets begin_real_validation and get_real_validation first. Prepare each "
               "required local medium through OSS and Ark Assets, wait until every asset is "
               "Active, then retry the same Seedance model using only asset:// IDs and new "
               "paid-call approval. Do not resubmit local paths or silently drop an input. "
               "Do not flip, repaint, or rewrite a face to evade review. If registration is "
               "Processing or Failed, report that actual state instead of changing models.";
    }
    if (detail.find("text sensitive") != std::string::npos ||
        detail.find("prompt sensitive") != std::string::npos) {
        // 文本敏感标签只说明上游怎样归类，不证明参考图已经过审；先核对原请求文本。
        return " This attempt failed. The provider labeled the text input as sensitive. Inspect "
               "the exact submitted "
               "instruction for unnecessary or ambiguous wording, then propose a concise "
               "rewrite that preserves the legitimate action. That label alone does not prove "
               "the reference image was accepted; compare inputs one at a time if needed. "
               "Show the next plan and obtain new per-call approval before a paid retry.";
    }
    if (detail.find("real person") != std::string::npos ||
        detail.find("human face") != std::string::npos ||
        detail.find("portrait") != std::string::npos) {
        // 其他模型报告真人时，区分“明确要求授权”和“模糊审核结果”。允许处理的图像
        // 先轻度处理，仍失败才考虑较强处理，并把处理损失如实写入后续提示词。
        return " This attempt failed. The provider reported a possible real person in the "
               "reference image. This signal alone does not prove an explicit authorization "
               "requirement. Inspect the raw provider wording, selected tool limitation, and "
               "exact submitted image. If the provider explicitly forbids recognizable "
               "real-person references without an authorized asset and the user's goal needs "
               "the same identity, do not hide the face in an edited image and reconstruct it "
               "with text. Briefly offer the authorized-asset path or a visibly fictional "
               "subject; ask before proposing another model. Otherwise, for a permitted "
               "derivative, do not switch providers yet. Preserve the original, then first "
               "use local FFmpeg/OpenCV image tools for a light, legitimate transformation. "
               "Preview the "
               "derivative and retry the same provider with new per-call approval. If it is still "
               "rejected, consider a stronger FFmpeg oil-paint-style derivative as a last "
               "image-processing "
               "step. Record details lost at each step and describe permitted details accurately "
               "in the next prompt so the result remains close to the original goal. Consider "
               "another provider only after these bounded same-provider attempts.";
    }
    if (detail.find("image sensitive") != std::string::npos ||
        detail.find("reference image") != std::string::npos) {
        // 明确指向参考图时，主模型应保留原图、预览衍生图，再用同一模型重试。
        return " This attempt failed. The provider identified the reference image. Inspect its "
               "actual content and role, preserve the original, then use FFmpeg/OpenCV image tools "
               "for the "
               "smallest permitted transformation that still serves the user's goal. Preview "
               "the derivative and retry this provider with new per-call approval. Only if that "
               "fails, consider a stronger FFmpeg oil-paint-style derivative as the last "
               "image-processing "
               "step. Record changed or lost traits and describe desired permitted details in "
               "the next prompt. Consider a different provider only after these bounded "
               "same-provider attempts.";
    }
    if (detail.find("sensitive") != std::string::npos ||
        detail.find("moderation") != std::string::npos ||
        detail.find("1026") != std::string::npos) {
        // 审核原因笼统时不能仅凭错误码判定是文字还是图片；逐项核对实际提交输入。
        return " This attempt failed. The provider gave an ambiguous input-moderation result. Do "
               "not assign it to text or image without evidence. Inspect the exact submitted "
               "prompt and source image. If evidence points to the photo, preserve the original "
               "and "
               "first use FFmpeg/OpenCV image tools for a light, permitted user-consistent "
               "transformation; "
               "preview the derivative and retry the same provider with new per-call approval. "
               "If image evidence still points to the image, a stronger FFmpeg oil-paint-style "
               "derivative is "
               "the last image-processing step. Record changed or lost details and describe "
               "desired permitted details in the next prompt. Change text separately if its "
               "wording is implicated. Consider another provider only after bounded, "
               "justified same-provider attempts have failed; do not launch paid diagnostic "
               "generations without approval or conceal explicitly prohibited material.";
    }
    if (detail.find("output delivery failed") != std::string::npos) {
        // 生成已完成但下载失败：恢复已有任务产物，不再发起一次收费生成。
        return " The provider finished, but delivery of its output failed. First recover the "
               "existing result using the saved task ID and output location; do not submit "
               "another paid generation just to download the same result.";
    }
    if (detail.find("status check failed") != std::string::npos) {
        // 查询失败无法推断生成失败；保留任务 ID 后继续安全查询。
        return " The status check failed, so the accepted provider task's result is unknown. "
               "Keep its task ID, inspect the original provider error, and try a safe status "
               "recovery before considering a new generation. Never claim that generation "
               "failed or silently create a duplicate paid task.";
    }
    if (detail.find("rate limit") != std::string::npos ||
        detail.find("temporarily unavailable") != std::string::npos ||
        detail.find("timeout") != std::string::npos) {
        // 暂时性故障可有界重试，但不能把同一收费请求重复提交。
        return " This appears temporary. Use a bounded retry or an available equivalent "
               "specialist while preserving the original task. Do not repeatedly submit the "
               "same paid generation.";
    }
    if (detail.find("invalid input") != std::string::npos ||
        detail.find("unsupported") != std::string::npos ||
        detail.find("invalid_input") != std::string::npos) {
        // 字段类错误由主模型结合工具规则修正，避免让用户承担参数排错。
        return " Check the exact rejected field and repair it locally while preserving the "
               "user's creative intent. Then continue the task using the normal paid-call "
               "approval; do not ask the user to diagnose an implementation detail.";
    }
    // 未识别错误也要把失败交给主模型继续判断，不能吞掉子工具结果。
    return " The specialist did not provide a classified recovery path. Inspect its exact "
           "error and execution stage, preserve the original task and media references, "
           "and try a practical repair or a capable alternate specialist before reporting "
           "an unresolved result to the user. Do not make speculative paid retry calls.";
}

}  // namespace

std::string maiToolFailureRecoveryGuidance(const std::string& tool, const std::string& error,
                                           bool executionUncertain) {
    auto detail = nlohmann::json::parse(error, nullptr, false);
    if (!detail.is_object()) return {};
    const auto code = detail.value("code", nlohmann::json{});
    const bool providerFailure = code == "provider_error" || code == "task_failed";
    // 后台专用任务回复已有 status=failed 包装，不能在移动 helper 时丢掉原恢复提示。
    if (!providerFailure && detail.value("status", nlohmann::json{}) != "failed") return {};
    detail["specialist"] = tool;
    // 执行结果未知时，旧的暂时故障/审核文案不能同时暗示可重新提交。
    // 明确余额拒绝仍保留已有选模型提示；参数修正可以本地完成，提交前仍须核对现状。
    if (executionUncertain && !isBalanceFailure(detail)) {
        return " Inspect current execution and task state before considering another "
               "submission. Preserve the original request, media, and any accepted task ID. "
               "Correct a known rejected argument locally when possible, but do not replay "
               "an uncertain operation or submit a replacement just to recover its result.";
    }
    return specialistFailureGuidance(detail);
}
