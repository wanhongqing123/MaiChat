// 文件作用：实现 Codex 补丁语法、文本替换及逐文件落盘，共用普通和模型入口。
#include "MaiApplyPatchTool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiCloudModelCatalog.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"

namespace {

using json = nlohmann::json;

// OpenAI Codex rust-v0.172.0 apply_patch.lark（Apache-2.0）。原生文本输入与解析器
// 使用同一补丁语法；兼容供应商仍走下方 patch 字符串参数，不增加另一套执行核心。
constexpr const char* kApplyPatchGrammar = R"lark(start: begin_patch hunk+ end_patch
begin_patch: "*** Begin Patch" LF
end_patch: "*** End Patch" LF?

hunk: add_hunk | delete_hunk | update_hunk
add_hunk: "*** Add File: " filename LF add_line+
delete_hunk: "*** Delete File: " filename LF
update_hunk: "*** Update File: " filename LF change_move? change?

filename: /(.+)/
add_line: "+" /(.*)/ LF -> line

change_move: "*** Move to: " filename LF
change: (change_context | change_line)+ eof_line?
change_context: ("@@" | "@@ " /(.+)/) LF
change_line: ("+" | "-" | " ") /(.*)/ LF
eof_line: "*** End of File" LF

%import common.LF
)lark";

// 对齐 codex 3d86e43b 的 parser/streaming_parser、file_update、text_file 和 seek_sequence。
// 这里保留宿主授权边界；不引入远端环境路由，也不声称多文件写入具备事务性。
struct MaiPatchChunk {
    std::optional<std::string> changeContext;
    std::vector<std::string> oldLines;
    std::vector<std::string> newLines;
    std::vector<std::pair<std::size_t, std::size_t>> contextIndices;
    bool atEnd = false;
};

struct MaiPatchOperation {
    enum class Kind { Add, Delete, Update };
    Kind kind = Kind::Update;
    std::string path;
    std::optional<std::string> movePath;
    std::string contents;
    std::vector<MaiPatchChunk> chunks;
};

struct MaiParsedPatch {
    MaiError error;
    std::vector<MaiPatchOperation> operations;
    std::optional<std::string> environmentId;
};

bool startsWith(const std::string& text, const char* prefix) {
    return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// 逐码点读取并校验 UTF-8，避免以当前 C locale 判断 Unicode 空白或接受无效文本。
// 同时用于匹配 Codex 的 Rust str::trim 和 read_to_string 的 UTF-8 约束。
bool nextCodePoint(const std::string& text, std::size_t& at, std::uint32_t& value) {
    if (at >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[at++]);
    if (first < 0x80) {
        value = first;
        return true;
    }
    const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                           : first >= 0xe0 && first <= 0xef ? 2
                           : first >= 0xf0 && first <= 0xf4 ? 3
                                                            : 0;
    if (count == 0 || at + count > text.size()) return false;
    value = first & ((1u << (6 - count)) - 1);
    for (unsigned index = 0; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(text[at++]);
        if ((byte & 0xc0) != 0x80) return false;
        value = (value << 6) | (byte & 0x3f);
    }
    const std::uint32_t minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
    return value >= minimum && value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
}

bool isUtf8(const std::string& text) {
    std::size_t at = 0;
    std::uint32_t value = 0;
    while (at < text.size()) {
        if (!nextCodePoint(text, at, value)) return false;
    }
    return true;
}

bool isWhitespace(std::uint32_t value) {
    return (value >= 9 && value <= 13) || value == 0x20 || value == 0x85 || value == 0xa0 ||
           value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
           value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}

std::string trimWhitespace(const std::string& text, bool trimStart = true) {
    std::size_t begin = text.size();
    std::size_t end = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t previous = at;
        std::uint32_t value = 0;
        if (!nextCodePoint(text, at, value)) return text;
        if (!isWhitespace(value)) {
            if (begin == text.size()) begin = previous;
            end = at;
        }
    }
    if (end == 0) return {};
    return text.substr(trimStart ? begin : 0, end - (trimStart ? begin : 0));
}

std::string normalizeContext(const std::string& text) {
    const std::string trimmed = trimWhitespace(text);
    std::string normalized;
    std::size_t at = 0;
    while (at < trimmed.size()) {
        const std::size_t previous = at;
        std::uint32_t value = 0;
        if (!nextCodePoint(trimmed, at, value)) return trimmed;
        if ((value >= 0x2010 && value <= 0x2015) || value == 0x2212)
            normalized += '-';
        else if (value >= 0x2018 && value <= 0x201b)
            normalized += '\'';
        else if (value >= 0x201c && value <= 0x201f)
            normalized += '"';
        else if (value == 0xa0 || (value >= 0x2002 && value <= 0x200a) || value == 0x202f ||
                 value == 0x205f || value == 0x3000)
            normalized += ' ';
        else
            normalized.append(trimmed, previous, at - previous);
    }
    return normalized;
}

// 补丁按 Rust str::lines 处理 LF/CRLF；裸 CR 只在源文件中作为换行。
std::vector<std::string> patchLines(const std::string& patch) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < patch.size()) {
        const std::size_t end = patch.find('\n', at);
        std::string line = patch.substr(at, end == std::string::npos ? end : end - at);
        if (end != std::string::npos && !line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return lines;
}

MaiError invalidPatch(std::string message) {
    return {MaiErrorCode::InvalidInput, "invalid patch: " + std::move(message)};
}

MaiParsedPatch parsePatch(const std::string& patch) {
    MaiParsedPatch parsed;
    if (!isUtf8(patch)) {
        parsed.error = invalidPatch("patch text is not valid UTF-8");
        return parsed;
    }
    auto lines = patchLines(trimWhitespace(patch));
    // Codex 兼容 local_shell 曾把 heredoc 作为单个实参发送的三种 EOF 包装。
    if (lines.size() >= 4 &&
        (lines.front() == "<<EOF" || lines.front() == "<<'EOF'" || lines.front() == "<<\"EOF\"") &&
        lines.back().size() >= 3 && lines.back().compare(lines.back().size() - 3, 3, "EOF") == 0) {
        lines.erase(lines.begin());
        lines.pop_back();
    }
    if (lines.empty() || trimWhitespace(lines.front()) != "*** Begin Patch") {
        parsed.error = invalidPatch("The first line of the patch must be '*** Begin Patch'");
        return parsed;
    }
    if (trimWhitespace(lines.back()) != "*** End Patch") {
        parsed.error = invalidPatch("The last line of the patch must be '*** End Patch'");
        return parsed;
    }

    const auto fail = [&](std::size_t index, const std::string& message) {
        parsed.error = {MaiErrorCode::InvalidInput,
                        "invalid hunk at line " + std::to_string(index + 1) + ", " + message};
    };
    const auto emptyUpdate = [&]() {
        if (parsed.operations.empty()) return false;
        const auto& operation = parsed.operations.back();
        return operation.kind == MaiPatchOperation::Kind::Update &&
               (operation.chunks.empty() || (operation.chunks.back().oldLines.empty() &&
                                             operation.chunks.back().newLines.empty()));
    };
    bool ended = false;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto& line = lines[index];
        const bool updating = !parsed.operations.empty() &&
                              parsed.operations.back().kind == MaiPatchOperation::Kind::Update;
        // Update 中行首空格是上下文标记，不能 trim 后误认成另一个文件头。
        const std::string marker = trimWhitespace(line, !updating || index + 1 == lines.size());
        if (ended) {
            if (!trimWhitespace(line).empty()) {
                parsed.error = invalidPatch("The last line of the patch must be '*** End Patch'");
                return parsed;
            }
            continue;
        }
        if (marker == "*** End Patch") {
            if (emptyUpdate()) {
                fail(index, "Update file hunk does not contain any lines");
                return parsed;
            }
            ended = true;
            continue;
        }
        std::optional<MaiPatchOperation::Kind> kind;
        const char* prefix = nullptr;
        if (startsWith(marker, "*** Add File: ")) {
            kind = MaiPatchOperation::Kind::Add;
            prefix = "*** Add File: ";
        } else if (startsWith(marker, "*** Delete File: ")) {
            kind = MaiPatchOperation::Kind::Delete;
            prefix = "*** Delete File: ";
        } else if (startsWith(marker, "*** Update File: ")) {
            kind = MaiPatchOperation::Kind::Update;
            prefix = "*** Update File: ";
        }
        if (kind) {
            if (emptyUpdate()) {
                fail(index, "Update file hunk does not contain any lines");
                return parsed;
            }
            MaiPatchOperation operation;
            operation.kind = *kind;
            operation.path = marker.substr(std::char_traits<char>::length(prefix));
            parsed.operations.push_back(std::move(operation));
            continue;
        }
        if (parsed.operations.empty()) {
            if (startsWith(marker, "*** Environment ID: ")) {
                const std::string environment = trimWhitespace(marker.substr(20));
                if (parsed.environmentId || environment.empty()) {
                    parsed.error =
                        invalidPatch("apply_patch environment_id must be nonempty and unique");
                    return parsed;
                }
                parsed.environmentId = environment;
                continue;
            }
            fail(index, "expected an Add File, Delete File, or Update File header");
            return parsed;
        }
        auto& operation = parsed.operations.back();
        if (operation.kind == MaiPatchOperation::Kind::Add) {
            if (line.empty() || line[0] != '+') {
                fail(index, "lines in an Add File hunk must start with '+'");
                return parsed;
            }
            operation.contents += line.substr(1) + '\n';
            continue;
        }
        if (operation.kind == MaiPatchOperation::Kind::Delete) {
            fail(index, "Delete File does not take any lines");
            return parsed;
        }
        if (!operation.chunks.empty() && operation.chunks.back().atEnd) {
            if (marker.empty()) continue;
            if (marker != "@@" && !startsWith(marker, "@@ ")) {
                fail(index, "expected an @@ context marker after End of File");
                return parsed;
            }
        }
        if (operation.chunks.empty() && !operation.movePath &&
            startsWith(marker, "*** Move to: ")) {
            operation.movePath = marker.substr(13);
            continue;
        }
        if (marker == "@@" || startsWith(marker, "@@ ")) {
            if (emptyUpdate() && !operation.chunks.empty()) {
                fail(index, "Update hunk does not contain any lines");
                return parsed;
            }
            MaiPatchChunk chunk;
            if (startsWith(marker, "@@ ")) chunk.changeContext = marker.substr(3);
            operation.chunks.push_back(std::move(chunk));
            continue;
        }
        if (marker == "*** End of File") {
            if (emptyUpdate() && !operation.chunks.empty()) {
                fail(index, "Update hunk does not contain any lines");
                return parsed;
            }
            if (!operation.chunks.empty()) operation.chunks.back().atEnd = true;
            continue;
        }
        if (!line.empty() && line[0] != ' ' && line[0] != '+' && line[0] != '-') {
            fail(index, "every update line must start with a space, '+' or '-': " + line);
            return parsed;
        }
        if (operation.chunks.empty()) operation.chunks.emplace_back();
        auto& chunk = operation.chunks.back();
        const char type = line.empty() ? ' ' : line[0];
        const std::string body = line.empty() ? std::string{} : line.substr(1);
        if (type == ' ') {
            chunk.contextIndices.emplace_back(chunk.oldLines.size(), chunk.newLines.size());
            chunk.oldLines.push_back(body);
            chunk.newLines.push_back(body);
        } else if (type == '-') {
            chunk.oldLines.push_back(body);
        } else {
            chunk.newLines.push_back(body);
        }
    }
    if (!ended) parsed.error = invalidPatch("The last line of the patch must be '*** End Patch'");
    return parsed;
}

std::size_t findSequence(const std::vector<std::string>& lines,
                         const std::vector<std::string>& pattern, std::size_t start, bool atEnd) {
    if (pattern.empty()) return start;
    if (pattern.size() > lines.size()) return std::string::npos;
    const std::size_t limit = lines.size() - pattern.size();
    const std::size_t first = atEnd ? std::max(start, limit) : start;
    for (int pass = 0; pass < 4; ++pass) {
        for (std::size_t index = first; index <= limit; ++index) {
            bool same = true;
            for (std::size_t offset = 0; offset < pattern.size(); ++offset) {
                const auto& have = lines[index + offset];
                const auto& want = pattern[offset];
                const bool equal = pass == 0 ? have == want
                                   : pass == 1
                                       ? trimWhitespace(have, false) == trimWhitespace(want, false)
                                   : pass == 2 ? trimWhitespace(have) == trimWhitespace(want)
                                               : normalizeContext(have) == normalizeContext(want);
                if (!equal) {
                    same = false;
                    break;
                }
            }
            if (same) return index;
        }
    }
    return std::string::npos;
}

struct MaiPatchSourceLine {
    std::string text;
    std::string ending;
};

struct MaiPatchSource {
    std::vector<MaiPatchSourceLine> lines;
    std::string preferredEnding = "\n";
};

MaiPatchSource parseSource(const std::string& contents) {
    MaiPatchSource source;
    bool hasEnding = false;
    std::size_t start = 0;
    for (std::size_t at = 0; at < contents.size(); ++at) {
        if (contents[at] != '\r' && contents[at] != '\n') continue;
        const std::size_t length =
            contents[at] == '\r' && at + 1 < contents.size() && contents[at + 1] == '\n' ? 2 : 1;
        std::string ending = contents.substr(at, length);
        if (!hasEnding) source.preferredEnding = ending;
        hasEnding = true;
        source.lines.push_back({contents.substr(start, at - start), std::move(ending)});
        at += length - 1;
        start = at + 1;
    }
    if (start < contents.size()) source.lines.push_back({contents.substr(start), {}});
    return source;
}

struct MaiPatchReplacement {
    std::size_t start = 0;
    std::size_t count = 0;
    std::vector<std::string> lines;
};

MaiResult<std::string> updateContents(const std::string& contents,
                                      const MaiPatchOperation& operation) {
    if (!isUtf8(contents))
        return {MaiErrorCode::InvalidInput, "file to update is not valid UTF-8: " + operation.path};
    const auto source = parseSource(contents);
    std::vector<std::string> originalLines;
    for (const auto& line : source.lines) originalLines.push_back(line.text);
    std::vector<MaiPatchReplacement> replacements;
    std::size_t cursor = 0;
    for (const auto& chunk : operation.chunks) {
        if (chunk.changeContext) {
            const auto found = findSequence(originalLines, {*chunk.changeContext}, cursor, false);
            if (found == std::string::npos)
                return {MaiErrorCode::NotFound, "Failed to find context '" + *chunk.changeContext +
                                                    "' in " + operation.path};
            cursor = found + 1;
        }
        // Codex 中没有旧行的块是文件末尾追加，即使它带了 @@ 上下文也如此。
        if (chunk.oldLines.empty()) {
            replacements.push_back({originalLines.size(), 0, chunk.newLines});
            continue;
        }
        auto pattern = chunk.oldLines;
        auto updated = chunk.newLines;
        auto found = findSequence(originalLines, pattern, cursor, chunk.atEnd);
        if (found == std::string::npos && !pattern.empty() && pattern.back().empty()) {
            pattern.pop_back();
            if (!updated.empty() && updated.back().empty()) updated.pop_back();
            found = findSequence(originalLines, pattern, cursor, chunk.atEnd);
        }
        if (found == std::string::npos)
            return {MaiErrorCode::NotFound, "Failed to find expected lines in " + operation.path};
        // 只替换实际增删段，保留模糊匹配的上下文原字节和每行原换行符。
        std::size_t oldStart = 0;
        std::size_t newStart = 0;
        const auto append = [&](std::size_t oldEnd, std::size_t newEnd) {
            if (oldStart != oldEnd || newStart != newEnd)
                replacements.push_back(
                    {found + oldStart, oldEnd - oldStart,
                     std::vector<std::string>(
                         updated.begin() + static_cast<std::ptrdiff_t>(newStart),
                         updated.begin() + static_cast<std::ptrdiff_t>(newEnd))});
        };
        for (const auto& context : chunk.contextIndices) {
            if (context.first >= pattern.size() || context.second >= updated.size()) break;
            append(context.first, context.second);
            oldStart = context.first + 1;
            newStart = context.second + 1;
        }
        append(pattern.size(), updated.size());
        cursor = found + pattern.size();
    }
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const auto& left, const auto& right) { return left.start < right.start; });
    std::string output;
    std::size_t originalIndex = 0;
    const auto appendOriginal = [&](std::size_t index) {
        const auto& line = source.lines[index];
        output += line.text;
        output += line.ending.empty() ? source.preferredEnding : line.ending;
    };
    for (const auto& replacement : replacements) {
        if (replacement.start < originalIndex)
            return {MaiErrorCode::InvalidInput, "overlapping changes in " + operation.path};
        while (originalIndex < replacement.start) appendOriginal(originalIndex++);
        for (const auto& line : replacement.lines) output += line + source.preferredEnding;
        originalIndex += replacement.count;
    }
    while (originalIndex < source.lines.size()) appendOriginal(originalIndex++);
    return output;
}

std::string resolvePath(const MaiPatchOptions& options, const std::string& candidate) {
    if (candidate.empty() || options.root.empty()) return {};
    return options.resolvePath ? options.resolvePath(candidate)
                               : maiResolvePathWithinRoot(options.root, candidate);
}

std::string appliedReport(const std::vector<MaiPatchChange>& changes) {
    std::string report;
    for (const auto& change : changes) {
        report += change.status;
        report += ' ';
        report += change.path;
        report += " (" + std::to_string(change.bytesWritten) + " bytes written)\n";
    }
    return report;
}

json parseArguments(const std::string& arguments) {
    auto parsed = json::parse(arguments, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

class MaiApplyPatchTool final : public MaiTool {
public:
    MaiToolSpec specForModel(const std::string& model) const override {
        MaiToolSpec spec{name(), description(), parametersSchema(), {}};
        const auto capability = maiCloudModelInfo(model);
        if (capability && capability->provider == "openai") {
            spec.description =
                "The `apply_patch` tool can be used to edit files. This is a FREEFORM tool, "
                "so do not wrap the patch in JSON.";
            spec.inputGrammar = kApplyPatchGrammar;
        }
        return spec;
    }
    std::string name() const override {
        return "apply_patch";
    }

    std::string description() const override {
        // 整份先解析语法，再顺序执行文件块；失败可能已经写入此前文件，不提供整体回滚。
        // @@ 文本缩小定位范围，纯新增块追加到文件尾。移动先写目标再删除源文件。
        return "Apply a Codex-format text patch. File hunks run in order; a later failure can "
               "leave "
               "earlier changes applied. Inspect the reported paths and current files before "
               "retrying. "
               "Add File replaces existing content; Update File supports @@ or @@ context, "
               "*** End of File, and *** Move to:. A chunk containing only additions appends at "
               "EOF. "
               "Format:\n*** Begin Patch\n*** Update File: path\n@@\n unchanged context\n"
               "-removed line\n+added line\n*** Add File: new-path\n+content\n"
               "*** Delete File: old-path\n*** End Patch\n"
               "Prefix context with a space and copy it from the file.";
    }

    std::string parametersSchema() const override {
        // patch 必须含完整 Begin/End 标记；解析器校验全部语法后才开始修改。
        return R"({"type":"object","properties":{"patch":{"type":"string","description":"The whole patch, from *** Begin Patch to *** End Patch"}},"required":["patch"]})";
    }

    bool requiresApproval(const std::string&) const override {
        return true;
    }

    std::vector<std::string> approvalKeys(const std::string& arguments,
                                          const MaiToolContext& context) const override {
        const auto args = parseArguments(arguments);
        if (!args.contains("patch") || !args["patch"].is_string())
            return MaiTool::approvalKeys(arguments, context);
        const auto parsed = parsePatch(args["patch"].get<std::string>());
        if (parsed.error || parsed.environmentId) return MaiTool::approvalKeys(arguments, context);
        std::set<std::string> paths;
        for (const auto& operation : parsed.operations) {
            const auto source = context.resolvePath(operation.path);
            if (source.empty()) return MaiTool::approvalKeys(arguments, context);
            paths.insert("file:" + source);
            if (operation.movePath) {
                const auto destination = context.resolvePath(*operation.movePath);
                if (destination.empty()) return MaiTool::approvalKeys(arguments, context);
                paths.insert("file:" + destination);
            }
        }
        return paths.empty() ? MaiTool::approvalKeys(arguments, context)
                             : std::vector<std::string>(paths.begin(), paths.end());
    }

    MaiToolResult execute(const std::string& arguments, const MaiToolContext& context) override {
        const auto args = parseArguments(arguments);
        if (!args.contains("patch") || !args["patch"].is_string() ||
            args["patch"].get<std::string>().empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: patch");
        MaiPatchOptions options;
        options.root = context.root;
        options.resolvePath = [&context](const auto& path) { return context.resolvePath(path); };
        const auto result = maiApplyPatch(args["patch"].get<std::string>(), options);
        if (result.error) {
            std::string message = result.error.message();
            if (!result.appliedChanges.empty())
                message += "\nChanges already applied (no rollback):\n" +
                           appliedReport(result.appliedChanges);
            else if (!result.failedWriteMayHaveChangedFile)
                message += "\nNo completed file changes were recorded.";
            if (result.failedWriteMayHaveChangedFile)
                message += "\nThe failed write may have changed " + result.failurePath +
                           "; its written-byte count is unknown.";
            message +=
                "\nInspect current files before retrying; do not blindly resubmit the whole patch.";
            return MaiToolResult::failure(result.error.code(), std::move(message));
        }
        return MaiToolResult::success("Applied the patch:\n" +
                                      appliedReport(result.appliedChanges));
    }
};

}  // namespace

MaiPatchResult maiApplyPatch(const std::string& patch, const MaiPatchOptions& options) {
    MaiPatchResult result;
    if (options.root.empty()) {
        result.error = {MaiErrorCode::InvalidInput,
                        "this session has no working directory, so file tools are unavailable"};
        return result;
    }
    const auto parsed = parsePatch(patch);
    if (parsed.error) {
        result.error = parsed.error;
        return result;
    }
    if (parsed.environmentId) {
        result.error = {MaiErrorCode::NotSupported,
                        "apply_patch environment_id is not supported by this in-process host"};
        return result;
    }
    if (parsed.operations.empty()) {
        result.error = {MaiErrorCode::InvalidInput, "No files were modified."};
        return result;
    }
    for (const auto& operation : parsed.operations) {
        result.failurePath = operation.path;
        const auto resolved = resolvePath(options, operation.path);
        if (resolved.empty()) {
            result.error = {MaiErrorCode::InvalidInput,
                            "path is outside the area accessible to this host: " + operation.path};
            return result;
        }
        const auto path = MaiFilePath::fromUtf8(resolved);
        if (operation.kind == MaiPatchOperation::Kind::Delete) {
            result.error = MaiFileSystem::removeFile(path);
            if (result.error) return result;
            result.appliedChanges.push_back({operation.path, resolved, 'D', 0});
            continue;
        }
        std::string contents = operation.contents;
        if (operation.kind == MaiPatchOperation::Kind::Update) {
            result.error = MaiFileSystem::readFile(path, contents);
            if (result.error) return result;
            const auto updated = updateContents(contents, operation);
            if (!updated) {
                result.error = updated.error();
                return result;
            }
            contents = updated.value();
        }
        const std::string reported = operation.movePath ? *operation.movePath : operation.path;
        // 在写目标前完成该目标的授权检查；整个调用不保存跨次缓存或读取旧计划。
        const auto destination = operation.movePath ? resolvePath(options, reported) : resolved;
        result.failurePath = reported;
        if (destination.empty()) {
            result.error = {
                MaiErrorCode::InvalidInput,
                "Move to path is outside the area accessible to this host: " + reported};
            return result;
        }
        const auto target = MaiFilePath::fromUtf8(destination);
        if (operation.kind == MaiPatchOperation::Kind::Add || operation.movePath) {
            result.error = MaiFileSystem::createDirectories(target.dirName());
            if (result.error) return result;
        }
        // 与 Codex write_file 一致，此处不是全文件原子替换。失败时不能伪称目标未改变。
        result.error = MaiFileSystem::writeFile(target, contents);
        if (result.error) {
            result.failedWriteMayHaveChangedFile = true;
            return result;
        }
        result.appliedChanges.push_back(
            {reported, destination,
             operation.kind == MaiPatchOperation::Kind::Add || operation.movePath ? 'A' : 'M',
             static_cast<std::uint64_t>(contents.size())});
        if (operation.movePath) {
            result.failurePath = operation.path;
            result.error = MaiFileSystem::removeFile(path);
            if (result.error) return result;
            result.appliedChanges.push_back({operation.path, resolved, 'D', 0});
        }
    }
    result.failurePath.clear();
    return result;
}

std::unique_ptr<MaiTool> makeMaiApplyPatchTool() {
    return std::make_unique<MaiApplyPatchTool>();
}
