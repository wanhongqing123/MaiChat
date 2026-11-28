// 文件作用：从公开元数据生成分组短帮助，不触发完整工具声明或执行。
#include "MaiToolHelp.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <utility>

#include <json.hpp>

#include "MaiSessionStore.h"

namespace {
bool isCanceled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

bool isNamed(const std::string& name, std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(),
                       [&](const char* candidate) { return name == candidate; });
}

// 名称表只做组织，不凭前缀推断扩展工具的能力；是否可见始终由真实注册表决定。
const char* groupForName(const std::string& name) {
    if (isNamed(name, {"ffmpeg", "ffprobe", "agent_send_media", "ark_assets", "seedance_video",
                       "seedream_image", "glm_image", "wan_image", "wan_video",
                       "kling_bailian_image", "kling_bailian_video", "minimax_image",
                       "minimax_video", "media_remove_watermark", "video_stretch_lower"}))
        return "media";
    if (isNamed(name,
                {"view_image", "screenshot", "list_windows", "cv_image_quality", "cv_image_compare",
                 "cv_find_contours", "cv_detect_edges", "cv_match_template",
                 "cv_register_translation", "cv_detect_lines", "cv_document_corners",
                 "cv_threshold_mask", "cv_video_matting", "cv_motion_detect", "cv_scene_detect"}))
        return "vision";
    if (isNamed(name, {"curl", "curl_download", "curl_fetch", "curl_request", "curl_upload",
                       "network_ip", "openssl", "crypto_digest", "crypto_certificate_info",
                       "ssh_exec", "web_fetch", "web_search"}))
        return "network-security";
    if (isNamed(name, {"file_read", "file_write", "file_edit", "file_create", "file_delete",
                       "file_create_directory", "file_delete_directory", "file_glob", "file_grep",
                       "file_stat", "file_copy", "file_move", "apply_patch", "agent_send_file",
                       "generate_pdf", "zlib_compress", "zlib_decompress", "app_storage"}))
        return "files";
    if (isNamed(name, {"tool_help",
                       "tool_search",
                       "tool_output_read",
                       "skill_read",
                       "skill_install",
                       "exec_command",
                       "write_stdin",
                       "question",
                       "todowrite",
                       "current_time",
                       "system_resources",
                       "llama",
                       "spawn_agent",
                       "wait_agent",
                       "send_input",
                       "list_agents",
                       "close_agent",
                       "session_status",
                       "session_list",
                       "session_history",
                       "session_create",
                       "session_delete",
                       "session_rename",
                       "session_send_input",
                       "session_set_workspace"}))
        return "agent";
    return "general";
}

// 这里只摘取短用途并折叠控制字符；与现有能力摘要一样在完整 UTF-8 码点处退回。
std::string shortPurpose(const std::string& description, std::size_t budget) {
    std::string purpose;
    for (const unsigned char byte : description) {
        if (byte <= 0x20 || byte == 0x7f) {
            if (!purpose.empty() && purpose.back() != ' ') purpose += ' ';
        } else {
            purpose += static_cast<char>(byte);
        }
        if (purpose.size() > budget + 4) break;
    }
    if (purpose.size() > budget) {
        auto end = budget - 3;
        while (end && (static_cast<unsigned char>(purpose[end]) & 0xc0) == 0x80) --end;
        purpose.resize(end);
        purpose += "...";
    }
    while (!purpose.empty() && purpose.back() == ' ') purpose.pop_back();
    return purpose;
}

std::string renderedName(const std::string& name) {
    const bool quote = std::any_of(name.begin(), name.end(), [](unsigned char byte) {
        return !((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                 (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' || byte == '.');
    });
    return quote ? nlohmann::json(name).dump() : name;
}

class MaiToolHelpTool final : public MaiTool {
public:
    explicit MaiToolHelpTool(const MaiToolRegistry& registry) : mRegistry(registry) {}
    std::string name() const override {
        return "tool_help";
    }
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }
    // 发现只返回分组短帮助；完整参数仍由现有 tool_search 按需声明。
    std::string description() const override {
        return "Show builtin tool groups with arguments:[\"-h\"] or short group help with "
               "arguments:[group,\"-h\"]. Metadata only; load exact schemas with tool_search.";
    }
    // 必填 arguments 最多两个短字符串；未知字段明确拒绝。
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"arguments":{"type":"array","maxItems":2,"items":{"type":"string","maxLength":64}}},"required":["arguments"],"additionalProperties":false})";
    }
    bool requiresWorkingDirectory(const std::string&) const override {
        return false;
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Tool help canceled.");
        const auto invalid = [] {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "Expected only {arguments:[]} or {arguments:[\"-h\"]} or "
                "{arguments:[group,\"-h\"]}. Each argument is at most 64 bytes.");
        };
        if (argumentsJson.size() > 4096) return invalid();
        const auto parsed = nlohmann::json::parse(argumentsJson, nullptr, false);
        if (!parsed.is_object() || parsed.size() != 1 || !parsed.contains("arguments") ||
            !parsed["arguments"].is_array() || parsed["arguments"].size() > 2)
            return invalid();
        MaiToolHelpRequest request;
        for (const auto& argument : parsed["arguments"]) {
            if (!argument.is_string()) return invalid();
            const auto value = argument.get<std::string>();
            if (value.size() > 64) return invalid();
            request.arguments.push_back(value);
        }
        const auto result = maiBuiltinToolHelp(mRegistry, context.model, request, context.cancel);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        return MaiToolResult::success(result.value().text, result.value().truncated);
    }

private:
    const MaiToolRegistry& mRegistry;
};
}  // namespace

MaiResult<MaiToolHelpDocument> maiBuiltinToolHelp(const MaiToolRegistry& registry,
                                                  const std::string& model,
                                                  const MaiToolHelpRequest& request,
                                                  const std::atomic<bool>* cancel) {
    const auto canceled = [] {
        return MaiResult<MaiToolHelpDocument>{MaiErrorCode::Canceled, "Tool help canceled."};
    };
    if (isCanceled(cancel)) return canceled();
    if (request.byteBudget < 512 || request.byteBudget > 8192)
        return {MaiErrorCode::InvalidInput, "Expected a help byte budget in 512..8192."};
    if (request.arguments.size() > 2 ||
        std::any_of(request.arguments.begin(), request.arguments.end(),
                    [](const auto& argument) { return argument.size() > 64; }) ||
        (request.arguments.size() == 1 && request.arguments[0] != "-h") ||
        (request.arguments.size() == 2 &&
         (request.arguments[0].empty() || request.arguments[1] != "-h")))
        return {MaiErrorCode::InvalidInput,
                "Expected [], [\"-h\"] or [group,\"-h\"], with arguments <=64 bytes."};

    // 不能以 specs/specialists 替代这个入口：前者会生成 Schema，后者可能读取凭据。
    auto metadata = registry.metadata(model);
    if (isCanceled(cancel)) return canceled();
    bool hasSearch = false;
    bool hasSkills = false;
    std::map<std::string, std::vector<MaiToolMetadata>> groups;
    for (auto& item : metadata) {
        if (isCanceled(cancel)) return canceled();
        if (item.name.empty() || item.name.size() > 256 ||
            maiValidateSessionContextContent(item.name))
            return {MaiErrorCode::Internal, "Invalid public tool name in help metadata."};
        hasSearch = hasSearch || item.name == "tool_search";
        hasSkills = hasSkills || item.name == "skill_read";
        const std::string group = groupForName(item.name);
        groups[group].push_back(std::move(item));
    }
    const bool listingGroups = request.arguments.size() < 2;
    const std::string group = listingGroups ? std::string{} : request.arguments[0];
    if (!listingGroups && groups.find(group) == groups.end())
        return {MaiErrorCode::InvalidInput,
                "Unknown or unavailable tool group. Use arguments:[\"-h\"] to list groups."};

    MaiToolHelpDocument document;
    document.text = listingGroups ? "Builtin tool groups for the current model:\n"
                                  : "Builtin tools in group " + group + " (short purposes only):\n";
    document.text +=
        "Read a group with arguments:[group,\"-h\"]; these flags belong to tool_help.\n";
    if (hasSearch)
        document.text +=
            "Use tool_search select (up to 5 exact names) to load schemas before calls.\n";
    if (hasSkills) document.text += "For complex workflows, use skill_read mode=discover.\n";
    document.text += "No permissions, credentials or backend readiness are implied.\n";
    const std::string continuation =
        hasSearch ? "Additional entries omitted; browse tool_search with query/offset/limit.\n"
                  : "Additional entries omitted by the help byte budget.\n";
    if (document.text.size() + continuation.size() > request.byteBudget)
        return {MaiErrorCode::Internal, "Cannot fit the tool help discovery instructions."};
    const auto append = [&](const std::string& line, bool hasMore) {
        if (document.text.size() + line.size() + (hasMore ? continuation.size() : 0) >
            request.byteBudget) {
            document.text += continuation;
            document.truncated = true;
            return false;
        }
        document.text += line;
        return true;
    };
    if (listingGroups) {
        std::size_t index = 0;
        for (const auto& entry : groups) {
            if (isCanceled(cancel)) return canceled();
            if (!append(
                    "- " + entry.first + " (" + std::to_string(entry.second.size()) + " tools)\n",
                    ++index < groups.size()))
                break;
        }
        if (groups.empty()) document.text += "No public tool groups are available.\n";
    } else {
        auto& tools = groups.at(group);
        std::sort(tools.begin(), tools.end(),
                  [](const auto& left, const auto& right) { return left.name < right.name; });
        const std::size_t purposeBytes = request.byteBudget < 1024 ? 64 : 128;
        for (std::size_t index = 0; index < tools.size(); ++index) {
            if (isCanceled(cancel)) return canceled();
            const auto purpose = shortPurpose(tools[index].description, purposeBytes);
            if (maiValidateSessionContextContent(purpose))
                return {MaiErrorCode::Internal, "Invalid UTF-8 tool purpose in help metadata."};
            const auto line = "- " + renderedName(tools[index].name) + ": " + purpose + '\n';
            if (!append(line, index + 1 < tools.size())) break;
        }
    }
    if (isCanceled(cancel)) return canceled();
    return document;
}

std::unique_ptr<MaiTool> makeMaiToolHelpTool(const MaiToolRegistry& registry) {
    return std::make_unique<MaiToolHelpTool>(registry);
}
