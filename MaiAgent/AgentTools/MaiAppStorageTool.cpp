// 文件作用：把共用存储预览服务适配为工具参数、交付保护及逐次审批。
#include "MaiAppStorageTool.h"

#include <json.hpp>

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "MaiAgent.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {
using Json = nlohmann::json;

MaiAppStorageContext storageContext(const MaiToolContext& context) {
    MaiAppStorageContext result{context.sessionId, context.root, {}, context.cancel};
    if (!context.sessionAgent) return result;
    result.protectionComplete = true;
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8(context.root));
    // 普通会话列表不含子 Agent，须补齐其历史收据，不能把“没出现在列表”当成未交付。
    auto sessions = context.sessionAgent->listSessions();
    std::unordered_set<std::string> seen;
    for (const auto& session : sessions) seen.insert(session.id);
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        for (const auto& child : context.sessionAgent->listSubAgents(sessions[index].id)) {
            MaiSession session;
            if (seen.insert(child.sessionId).second &&
                context.sessionAgent->getSession(child.sessionId, session))
                sessions.push_back(std::move(session));
        }
    }
    // 保护实际附件和交付收据，而非把每张图片都当成品；同工作区其他会话也不能误删。
    for (const auto& session : sessions) {
        if (root.isEmpty() ||
            MaiFileSystem::resolve(MaiFilePath::fromUtf8(session.directory)) != root)
            continue;
        for (const auto& message : context.sessionAgent->listMessages(session.id)) {
            for (const auto& part : message.parts) {
                std::string path;
                if (const auto* image = std::get_if<MaiImagePart>(&part.body)) path = image->path;
                if (const auto* video = std::get_if<MaiVideoPart>(&part.body)) path = video->path;
                if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                    if (tool->state != MaiToolState::Completed || tool->tool != "agent_send_media")
                        continue;
                    const auto receipt = Json::parse(tool->output, nullptr, false);
                    if (receipt.is_object() &&
                        receipt.value("delivery", Json{}) == "current_ai_session" &&
                        receipt.value("path", Json{}).is_string())
                        path = receipt["path"].get<std::string>();
                }
                if (!path.empty())
                    result.protectedPaths.push_back(
                        root.append(MaiFilePath::fromUtf8(path)).toUtf8());
            }
        }
    }
    return result;
}

Json previewJson(const MaiAppStoragePreview& preview) {
    static const char* targets[] = {"app", "temporary", "cache", "workspace"};
    Json result{
        {"preview_id", preview.id},
        {"dry_run", true},
        {"target", targets[static_cast<int>(preview.selection.target)]},
        {"scope", preview.selection.scope},
        {"patterns", preview.selection.patterns},
        {"workspace_cleanup_allowed", preview.selection.target == MaiAppStorageTarget::Workspace},
        {"cleanup_min_age_hours", preview.minimumAgeHours},
        {"cleanup_limit", 1000},
        {"cleanup_limited", preview.limited},
        {"cleanup_candidate_count", preview.candidates.size()},
        {"cleanup_candidate_bytes", preview.candidateBytes}};
    bool complete = true;
    for (const auto& summary : preview.summaries) {
        const bool exact = !summary.truncated && summary.scanErrorCount == 0;
        complete = complete && exact;
        std::size_t count = 0;
        for (const auto& candidate : preview.candidates)
            if (candidate.target == summary.target) ++count;
        result[summary.target] =
            Json{{"bytes", summary.bytes},
                 {"files", summary.files},
                 {"scan_truncated", summary.truncated},
                 {"scan_error_count", summary.scanErrorCount},
                 {"totals_exact", exact},
                 {"eligible_files", summary.eligibleFiles},
                 {"eligible_bytes", summary.eligibleBytes},
                 {"excluded_recent_files", summary.excludedRecentFiles},
                 {"excluded_recent_bytes", summary.excludedRecentBytes},
                 {"excluded_protected_files", summary.excludedProtectedFiles},
                 {"excluded_protected_bytes", summary.excludedProtectedBytes},
                 {"excluded_outside_policy_files", summary.excludedPolicyFiles},
                 {"excluded_outside_policy_bytes", summary.excludedPolicyBytes},
                 {"cleanup_candidates", count}};
    }
    // 候选总数/字节始终精确描述已保存执行集合；合格统计只有完整扫描时才是全量。
    result["scan_complete"] = complete;
    result["eligible_totals_exact"] = complete;
    result["cleanup_candidates_complete"] = complete && !preview.limited;
    return result;
}

MaiResult<std::string> previewOutput(const MaiAppStoragePreview& preview, bool detail,
                                     std::size_t offset, std::size_t limit) {
    auto result = previewJson(preview);
    if (!detail) return result.dump();
    if (offset > preview.candidates.size())
        return {MaiErrorCode::InvalidInput, "offset exceeds the saved preview candidate count"};
    constexpr std::size_t maximumPageBytes = 65536;
    result["cleanup_candidates"] = Json::array();
    result["detail_offset"] = offset;
    result["detail_limit"] = limit;
    result["detail_total_count"] = preview.candidates.size();
    result["detail_total_bytes"] = preview.candidateBytes;
    std::uint64_t returnedBytes = 0;
    const auto updatePage = [&] {
        const auto count = result["cleanup_candidates"].size();
        const auto next = offset + count;
        result["detail_returned_count"] = count;
        result["detail_returned_bytes"] = returnedBytes;
        result["has_more"] = next < preview.candidates.size();
        result["next_offset"] = next < preview.candidates.size() ? Json(next) : Json(nullptr);
    };
    updatePage();
    for (std::size_t index = offset; index < preview.candidates.size() && index - offset < limit;
         ++index) {
        const auto& candidate = preview.candidates[index];
        result["cleanup_candidates"].push_back(Json{{"path", candidate.path},
                                                    {"relative_path", candidate.relativePath},
                                                    {"target", candidate.target},
                                                    {"bytes", candidate.bytes},
                                                    {"modified_unix_seconds", candidate.modified}});
        returnedBytes += candidate.bytes;
        updatePage();
        // 预算包含元数据与 JSON 转义后的真实字节，不能只按文件项数量推测输出大小。
        if (result.dump().size() > maximumPageBytes) {
            result["cleanup_candidates"].erase(result["cleanup_candidates"].size() - 1);
            returnedBytes -= candidate.bytes;
            updatePage();
            break;
        }
    }
    if ((result["cleanup_candidates"].empty() && offset < preview.candidates.size()) ||
        result.dump().size() > maximumPageBytes)
        return {MaiErrorCode::InvalidInput,
                "storage detail entry exceeds the 65536-byte page budget"};
    return result.dump();
}

class AppStorageTool final : public MaiTool {
public:
    explicit AppStorageTool(MaiAppStoragePaths paths) : mStorage(std::move(paths)) {}
    std::string name() const override {
        return "app_storage";
    }
    std::string description() const override {
        // 默认只读预览；工作区仅选明确的可再生中间目录，不以扩展名推断成品。
        // 默认只返回统计；显式明细按快照 ID 分页，条数和序列化字节均有界。
        // 最近文件和默认权重受保护；零年龄必须有界。执行只消费已确认预览，不补入新文件。
        return "Inspect App storage and preview cleanup. action=scan without target reports App "
               "temporary/cache/workspace sizes and selects no workspace files. Cleanup defaults "
               "to "
               "dry_run=true and returns summary counts/bytes plus age/protection exclusions, "
               "without file paths. detail=true returns a bounded candidate page; offset defaults "
               "to 0 and limit to 50 (clamped to 1..100), with a 65536-byte JSON page budget. "
               "Continue using preview_id, detail=true and next_offset; pages reuse that exact "
               "snapshot without rescanning or extending its expiry. has_more=false ends paging. "
               "Candidate totals describe the saved execution set; eligible_totals_exact=false "
               "means scan totals are observed lower bounds, and cleanup_candidates_complete=false "
               "means the saved set may not cover all eligible files. target is temporary, cache, "
               "or workspace. Workspace requires explicit "
               "scope or bounded root-relative patterns limited to regenerable frames_<id>, "
               "wmframes, "
               "screenshots, search, browser, or .maiagent/model-previews. Never wipe the whole "
               "workspace; gallery-* and recorded delivered media/attachments are protected. "
               "Patterns "
               "support * within one component only. min_age_hours defaults to 24; 0 requires a "
               "bounded "
               "scope and a preview first. Model files are protected unless explicitly scoped; "
               "verify "
               "ownership and inactivity before selecting QA weights. To execute send only "
               "action=cleanup, dry_run=false and the confirmed preview_id. Only unchanged regular "
               "files from that single-use ten-minute session/root-bound preview are removed; new "
               "files are never added. Deletion follows the Agent approval policy.";
    }
    std::string parametersSchema() const override {
        // 保留 action 入口；detail 默认关闭，offset/limit 只用于显式明细。
        // preview_id 明细读取不能混入选择参数，真实执行仍只接受原先三个字段；拒绝未知字段。
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["scan","cleanup"]},"target":{"type":"string","enum":["temporary","cache","workspace"]},"scope":{"type":"string","minLength":1,"maxLength":1024},"patterns":{"type":"array","minItems":1,"maxItems":20,"items":{"type":"string","minLength":1,"maxLength":1024}},"min_age_hours":{"type":"integer","minimum":0,"maximum":8760,"default":24},"dry_run":{"type":"boolean","default":true},"preview_id":{"type":"string","minLength":1},"detail":{"type":"boolean","default":false},"offset":{"type":"integer","minimum":0,"default":0},"limit":{"type":"integer","minimum":1,"maximum":100,"default":50}},"required":["action"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        const auto args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string() ||
            (args.contains("dry_run") && !args["dry_run"].is_boolean()))
            return true;
        const auto action = args["action"].get<std::string>();
        return (action != "scan" && action != "cleanup") || !args.value("dry_run", true);
    }
    bool requiresPerCallApproval(const std::string& raw) const override {
        return requiresApproval(raw);
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("action", Json{}).is_string())
            return invalid("action is required");
        for (auto item = args.begin(); item != args.end(); ++item)
            if (item.key() != "action" && item.key() != "target" && item.key() != "scope" &&
                item.key() != "patterns" && item.key() != "min_age_hours" &&
                item.key() != "dry_run" && item.key() != "preview_id" && item.key() != "detail" &&
                item.key() != "offset" && item.key() != "limit")
                return invalid("unknown storage field");
        const auto action = args["action"].get<std::string>();
        if (action != "scan" && action != "cleanup") return invalid("unknown storage action");
        if (args.contains("dry_run") && !args["dry_run"].is_boolean())
            return invalid("dry_run must be boolean");
        if (!args.value("dry_run", true)) {
            if (action != "cleanup" || args.size() != 3 ||
                !args.value("preview_id", Json{}).is_string())
                return invalid("execute with only action=cleanup, dry_run=false and preview_id");
            const auto result =
                mStorage.cleanup(args["preview_id"].get<std::string>(), storageContext(context));
            if (!result) return failure(result.error());
            return MaiToolResult::success(Json{
                {"dry_run", false},
                {"removed_files", result.value().removedFiles},
                {"reclaimed_bytes", result.value().reclaimedBytes},
                {"skipped_files",
                 result.value().skippedFiles}}.dump());
        }
        if (args.contains("detail") && !args["detail"].is_boolean())
            return invalid("detail must be boolean");
        const bool detail = args.value("detail", false);
        if (!detail && (args.contains("offset") || args.contains("limit")))
            return invalid("offset and limit require detail=true");
        std::size_t offset = 0, limit = 50;
        if (args.contains("offset")) {
            if (!args["offset"].is_number_integer() || args["offset"] < 0 || args["offset"] > 1000)
                return invalid("offset must be a nonnegative saved candidate index");
            offset = args["offset"].get<std::size_t>();
        }
        if (args.contains("limit")) {
            if (!args["limit"].is_number_integer()) return invalid("limit must be an integer");
            if (args["limit"].is_number_unsigned())
                limit = static_cast<std::size_t>(std::clamp(args["limit"].get<std::uint64_t>(),
                                                            std::uint64_t{1}, std::uint64_t{100}));
            else
                limit = static_cast<std::size_t>(std::clamp(args["limit"].get<std::int64_t>(),
                                                            std::int64_t{1}, std::int64_t{100}));
        }
        if (args.contains("preview_id")) {
            if (!detail || !args["preview_id"].is_string())
                return invalid("preview_id reads require detail=true");
            for (const auto* key : {"target", "scope", "patterns", "min_age_hours"})
                if (args.contains(key))
                    return invalid("saved preview pages cannot change the selection");
            const auto saved = mStorage.readPreview(args["preview_id"].get<std::string>(),
                                                    storageContext(context));
            if (!saved) return failure(saved.error());
            const auto output = previewOutput(saved.value(), true, offset, limit);
            if (!output) return failure(output.error());
            return MaiToolResult::success(output.value());
        }
        MaiAppStorageSelection selection;
        if (args.contains("target")) {
            if (!args["target"].is_string()) return invalid("target must be a storage category");
            const auto target = args["target"].get<std::string>();
            if (target == "temporary")
                selection.target = MaiAppStorageTarget::Temporary;
            else if (target == "cache")
                selection.target = MaiAppStorageTarget::Cache;
            else if (target == "workspace")
                selection.target = MaiAppStorageTarget::Workspace;
            else
                return invalid("target must be temporary, cache, or workspace");
        } else if (action == "cleanup")
            return invalid("cleanup preview requires an explicit target");
        if (args.contains("scope")) {
            if (!args["scope"].is_string() || args["scope"].get<std::string>().empty())
                return invalid("scope must be a nonempty relative path");
            selection.scope = args["scope"].get<std::string>();
        }
        if (args.contains("patterns")) {
            if (!args["patterns"].is_array() || args["patterns"].empty())
                return invalid("patterns must be a nonempty array of relative patterns");
            for (const auto& pattern : args["patterns"]) {
                if (!pattern.is_string()) return invalid("patterns must contain strings");
                selection.patterns.push_back(pattern.get<std::string>());
            }
        }
        if (args.contains("min_age_hours")) {
            if (!args["min_age_hours"].is_number_integer() || args["min_age_hours"] < 0 ||
                args["min_age_hours"] > 8760)
                return invalid("min_age_hours must be an integer from 0 to 8760");
            selection.minimumAgeHours = args["min_age_hours"].get<int>();
        }
        if (selection.target == MaiAppStorageTarget::Workspace && !context.sessionAgent)
            return invalid(
                "workspace preview requires the session host to protect delivered media");
        const auto result = mStorage.preview(selection, storageContext(context));
        if (!result) return failure(result.error());
        const auto output = previewOutput(result.value(), detail, offset, limit);
        if (!output) return failure(output.error());
        return MaiToolResult::success(output.value());
    }

private:
    static MaiToolResult invalid(const std::string& message) {
        return MaiToolResult::failure(MaiErrorCode::InvalidInput, message);
    }
    static MaiToolResult failure(const MaiError& error) {
        return MaiToolResult::failure(error.code(), error.message());
    }
    MaiAppStorage mStorage;
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiAppStorageTool(MaiAppStoragePaths paths) {
    return std::make_unique<AppStorageTool>(std::move(paths));
}
