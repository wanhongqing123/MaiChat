#include "MaiAgentSendFileTool.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

// POSIX 路径保留原生 bytes；解析链接后的名字未必仍能进入 JSON/跨端 UI。
// 使用现有 JSON 编码器的严格 UTF-8 判定，不能悄悄以替换字符改写真实文件名。
static bool agentFileMetadataIsUtf8(const std::string& value) {
    try {
        (void)nlohmann::json(value).dump();
        return true;
    } catch (const nlohmann::json::type_error&) {
        return false;
    }
}

// MIME 只描述附件，不解析或执行内容；未知扩展名保留为普通二进制文件。
static std::string agentFileMimeType(const std::string& name) {
    const auto dot = name.find_last_of('.');
    std::string extension = dot == std::string::npos ? std::string() : name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static const std::pair<const char*, const char*> types[] = {
        {".pdf", "application/pdf"},
        {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".ppt", "application/vnd.ms-powerpoint"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".zip", "application/zip"},
        {".7z", "application/x-7z-compressed"},
        {".gz", "application/gzip"},
        {".tar", "application/x-tar"},
        {".txt", "text/plain"},
        {".md", "text/markdown"},
        {".csv", "text/csv"},
        {".json", "application/json"},
        {".html", "text/html"},
        {".htm", "text/html"},
        {".xml", "application/xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".webp", "image/webp"},
        {".gif", "image/gif"},
        {".mp4", "video/mp4"},
        {".mov", "video/quicktime"},
        {".mp3", "audio/mpeg"},
        {".wav", "audio/wav"},
    };
    for (const auto& type : types)
        if (extension == type.first) return type.second;
    return "application/octet-stream";
}

MaiResult<MaiAgentFile> maiPrepareAgentFile(const std::string& filePath,
                                            const MaiToolContext& context) {
    if (context.isCanceled()) return {MaiErrorCode::Canceled, "File delivery was canceled"};
    if (context.root.empty())
        return {MaiErrorCode::NotConfigured, "Agent workspace is unavailable"};
    if (filePath.empty() || filePath.find('\0') != std::string::npos ||
        !agentFileMetadataIsUtf8(filePath) || MaiFilePath::fromUtf8(filePath).isAbsolute())
        return {MaiErrorCode::InvalidInput, "file_path must be relative to this Agent workspace"};
    const std::string resolved = maiResolvePathWithinRoot(context.root, filePath);
    if (resolved.empty())
        return {MaiErrorCode::InvalidInput, "file_path leaves this Agent workspace"};
    const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
    MaiRegularFileInfo info;
    if (!MaiFileSystem::regularFileInfo(path, info))
        return {MaiErrorCode::InvalidInput, "File is missing or is not a regular file"};
    constexpr std::uint64_t limit = 512ull * 1024 * 1024;
    if (info.bytes == 0 || info.bytes > limit)
        return {MaiErrorCode::InvalidInput, "File is empty or exceeds the 512 MiB limit"};

    // 授权后按平台原生路径组件生成相对路径；不以大小写敏感的字符串前缀切 Windows 路径。
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8(context.root));
    if (root.isEmpty() || !root.isParentOf(path))
        return {MaiErrorCode::InvalidInput, "file_path leaves this Agent workspace"};
    const auto rootParts = root.components();
    const auto parts = path.components();
    MaiFilePath relative;
    for (std::size_t index = rootParts.size(); index < parts.size(); ++index)
        relative = relative.append(MaiFilePath(parts[index]));
    if (relative.isEmpty() || context.isCanceled())
        return {MaiErrorCode::Canceled, "File delivery was canceled"};
    const std::string name = path.baseName().toUtf8();
    const std::string deliveredPath = relative.toGenericUtf8();
    if (!agentFileMetadataIsUtf8(name) || !agentFileMetadataIsUtf8(deliveredPath))
        return {MaiErrorCode::InvalidInput, "Resolved file path and name must be valid UTF-8"};
    return MaiAgentFile{deliveredPath, name, agentFileMimeType(name), info.bytes};
}

class MaiAgentSendFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "agent_send_file";
    }
    std::string description() const override {
        return "Deliver an existing workspace file as a persistent file card in the CURRENT AI "
               "reply. "
               "Use this for PDF, documents, spreadsheets, archives and other files. Provide a "
               "relative "
               "file_path and optional caption; no file bytes, peer_id or model vision are needed. "
               "This does not upload, execute or modify the file. The file must remain in the "
               "workspace.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"file_path":{"type":"string"},)"
               R"("caption":{"type":"string","maxLength":1000}},"required":["file_path"],)"
               R"("additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto request = nlohmann::json::parse(raw, nullptr, false);
        if (!request.is_object() || !request.contains("file_path") ||
            !request["file_path"].is_string() || request.size() > 2 ||
            (request.contains("caption") && !request["caption"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_path is required; caption is optional");
        for (auto field = request.begin(); field != request.end(); ++field)
            if (field.key() != "file_path" && field.key() != "caption")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "agent_send_file accepts no peer_id or extra fields");
        const std::string caption = request.value("caption", std::string());
        // JSON 已校验 UTF-8；计首字节以遵守 Schema 的字符数，而不是误以字节限制中文说明。
        if (std::count_if(caption.begin(), caption.end(),
                          [](unsigned char value) { return (value & 0xc0) != 0x80; }) > 1000)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "caption exceeds 1000 characters");
        const auto prepared = maiPrepareAgentFile(request["file_path"].get<std::string>(), context);
        if (!prepared)
            return MaiToolResult::failure(prepared.error().code(), prepared.error().message());
        const auto& file = prepared.value();
        return MaiToolResult::success(nlohmann::json{
            {"delivery", "current_ai_session"},
            {"type", "file"},
            {"path", file.relativePath},
            {"mime_type", file.mimeType},
            {"bytes", file.bytes},
            {"caption", caption},
            {"name", file.name}}.dump());
    }
};

std::unique_ptr<MaiTool> makeMaiAgentSendFileTool() {
    return std::make_unique<MaiAgentSendFileTool>();
}
