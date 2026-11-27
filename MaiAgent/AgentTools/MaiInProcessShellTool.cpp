// 文件作用：校验移动脚本参数，复用普通解析器和原生文件命令，不启动外部进程。
#include "MaiInProcessShellTool.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <json.hpp>

#include "MaiCommandScript.h"
#include "MaiFileCommands.h"
#include "MaiFileSearch.h"
#include "MaiTextFileCursorCache.h"

namespace {
using Json = nlohmann::json;

// 最终显示按 UTF-8 首字节边界裁剪；原始管道和文件内容由普通核心持有，不裁剪后写盘。
std::string clipped(const std::string& text, std::size_t limit) {
    auto end = std::min(text.size(), limit);
    while (end && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
    return text.substr(0, end);
}

class MaiInProcessShellTool final : public MaiTool {
public:
    std::string name() const override {
        return "exec_command";
    }
    std::string description() const override {
        return "Run a bounded in-process file command script on mobile. No external shell, child "
               "process, PTY or resumable process is started. Commands: echo [-n], pwd, cd, cat, "
               "ls [-a], mkdir [-p], cp, mv, rm (one file), touch (ensure a file exists), "
               "head/tail [-n N], find PATH -name GLOB, stat, rmdir (empty directory), "
               "read-text [--encoding E] [--offset N] [--limit N] [--cursor TOKEN] PATH, "
               "grep [-F] [-n] [-i] [-r] [-l|-c] PATTERN [PATH]. grep defaults to regex; -F is "
               "literal. Quote glob and regex patterns to keep shell metacharacters literal. "
               "find/grep accept --limit and --cursor for bounded pages. Each command "
               "accepts only its documented bounded subset, "
               "not all Unix flags. cp/mv do not overwrite. Supports quotes, variables, newline/; "
               "sequencing, &&, ||, |, <, > and >>. Variables/cd live only for this call. "
               "No glob expansion, command substitution, background execution, loops or here-doc. "
               "Use find/grep for search and read-text for paged/encoded text. Copy the returned "
               "next_cursor to continue, preserving query, path and encoding; a cursor is not a "
               "file permission. Use apply_patch for exact text edits. Standalone file_* tools "
               "are not exposed. curl/ffmpeg are separate tools, not commands in this interpreter. "
               "Check exit_code and output_truncated. No session_id is returned; write_stdin is "
               "not available. Files stay inside the host App container.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"cmd":{"type":"string","maxLength":65536},"workdir":{"type":"string","maxLength":4096},"max_output_tokens":{"type":"integer","minimum":256,"maximum":65536}},"required":["cmd"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto invalid = [](const std::string& text) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, text);
        };
        const auto args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.contains("cmd") || !args["cmd"].is_string() ||
            (args.contains("workdir") && !args["workdir"].is_string()))
            return invalid("Expected cmd and optional workdir strings.");
        for (const auto& item : args.items())
            if (item.key() != "cmd" && item.key() != "workdir" && item.key() != "max_output_tokens")
                return invalid("Unsupported in-process script parameter: " + item.key());
        std::uint64_t tokens = 4096;
        if (args.contains("max_output_tokens")) {
            const auto& count = args["max_output_tokens"];
            if (!count.is_number_integer() ||
                (!count.is_number_unsigned() && count.get<std::int64_t>() < 0))
                return invalid("max_output_tokens must be an integer in 256..65536.");
            tokens = count.get<std::uint64_t>();
            if (tokens < 256 || tokens > 65536)
                return invalid("max_output_tokens must be in 256..65536.");
        }
        if (context.root.empty()) return invalid("This session has no working directory.");
        const auto command = args["cmd"].get<std::string>();
        const auto directory = args.value("workdir", context.root);
        if (command.empty() || command.size() > 65536 || command.find('\0') != std::string::npos ||
            directory.empty() || directory.size() > 4096 ||
            directory.find('\0') != std::string::npos)
            return invalid("Script/workdir is empty, contains NUL or exceeds its byte limit.");
        const auto resolved = context.resolvePath(directory);
        if (resolved.empty()) return invalid("workdir is outside the host file boundary.");
        MaiFileCommandOptions files;
        files.workingDirectory = MaiFilePath::fromUtf8(resolved);
        files.accessRoot = MaiFilePath::fromUtf8(
            context.fileAccessRoot.empty() ? context.root : context.fileAccessRoot);
        files.isCanceled = [&context] { return context.isCanceled(); };
        // 服务只缓存有界索引和可信游标；会话与容器由宿主绑定，不接受模型声明的权限。
        files.search = mSearch;
        files.textCursors = mTextCursors;
        files.scopeKey = Json::array({context.sessionId, files.accessRoot.toUtf8()}).dump();
        files.decodeText = context.decodeText;
        files.maxResultBytes = static_cast<std::size_t>(tokens) * 4;
        MaiCommandScriptOptions options;
        options.workingDirectory = files.workingDirectory;
        options.isCanceled = files.isCanceled;
        const auto result = maiRunCommandScript(command, options, makeMaiFileCommandHost(files));
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        const auto& execution = result.value();
        const auto limit = static_cast<std::size_t>(tokens) * 4;
        const auto output = clipped(execution.standardOutput, limit);
        const auto error = clipped(execution.standardError, limit - output.size());
        const bool truncated = execution.outputTruncated ||
                               output.size() != execution.standardOutput.size() ||
                               error.size() != execution.standardError.size();
        // JSON 边界显式声明替换策略；文件/管道原始字节不经过这层显示转换。
        const auto receipt = Json{
            {"exit_code", execution.exitCode},
            {"output", output},
            {"stderr", error},
            {"output_truncated", truncated},
            {"output_encoding", "utf8_with_replacement_for_invalid_bytes"},
            {"execution_mode", "in_process"},
            {"initial_working_directory",
             resolved}}.dump(-1, ' ', false, Json::error_handler_t::replace);
        return execution.exitCode == 0
                   ? MaiToolResult::success(receipt, truncated)
                   : MaiToolResult::failure(MaiErrorCode::InvalidInput, receipt, truncated);
    }

private:
    std::shared_ptr<MaiFileSearch> mSearch = std::make_shared<MaiFileSearch>();
    std::shared_ptr<MaiTextFileCursorCache> mTextCursors =
        std::make_shared<MaiTextFileCursorCache>();
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiInProcessShellTool() {
    return std::make_unique<MaiInProcessShellTool>();
}
