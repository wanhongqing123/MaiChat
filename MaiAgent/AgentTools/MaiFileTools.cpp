// 文件作用：提供工作区文件的读取、列举和写入工具。
// 直接依赖：MaiApplyPatchTool.h、MaiEditTool.h、MaiFilePath.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiFileTools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <system_error>

#include <json.hpp>

#include "MaiApplyPatchTool.h"
#include "MaiEditTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiFileSearch.h"
#include "MaiIdGenerator.h"
#include "MaiTextFileReader.h"

namespace {

using json = nlohmann::json;

// ── 输出上限 ────────────────────────────────────────────────────
// 工具输出会原样进上下文，所以每一个字节都要花 token、也都要占内存。
// grep 一个大仓库能返回几 MB —— 不设限的话内存、请求体、费用一起炸。这是计划里的二号性能风险。
constexpr std::size_t kMaxOutputBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 256 * 1024;
// 截断时要落在 UTF-8 字符边界上，不然会切出半个汉字，后面 JSON 序列化会失败或者产生乱码。
void truncateUtf8(std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
}

json parseArguments(const std::string& raw) {
    const json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    return parsed.is_object() ? parsed : json::object();
}

std::string stringArgument(const json& arguments, const char* name) {
    const auto value = arguments.find(name);
    return value != arguments.end() && value->is_string() ? value->get<std::string>()
                                                          : std::string{};
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first < 0x80) {
            ++index;
            continue;
        }
        int width = 0;
        if (first >= 0xC2 && first <= 0xDF)
            width = 2;
        else if (first >= 0xE0 && first <= 0xEF)
            width = 3;
        else if (first >= 0xF0 && first <= 0xF4)
            width = 4;
        else
            return false;
        if (index + width > text.size()) return false;
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second > 0x9F) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second > 0x8F))
            return false;
        for (int offset = 2; offset < width; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if (next < 0x80 || next > 0xBF) return false;
        }
        index += static_cast<std::size_t>(width);
    }
    return true;
}

bool looksBinary(const std::string& text) {
    std::size_t controls = 0;
    for (const unsigned char byte : text) {
        if (byte == 0) return true;
        if (byte < 0x09 || (byte > 0x0D && byte < 0x20)) ++controls;
    }
    return !text.empty() && controls > text.size() / 100;
}

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

MaiResult<std::string> decodeUtf16(const std::string& bytes, bool littleEndian) {
    std::size_t offset = 0;
    // 显式编码仍须尊重 BOM；与普通 reader 一样拒绝冲突，不能返回乱码后的“无命中”。
    if (bytes.size() >= 2 &&
        ((littleEndian && bytes.compare(0, 2, "\xFE\xFF", 2) == 0) ||
         (!littleEndian && bytes.compare(0, 2, "\xFF\xFE", 2) == 0)))
        return {MaiErrorCode::InvalidInput, "UTF-16 BOM conflicts with the requested encoding"};
    if (bytes.size() >= 2 && ((littleEndian && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                               static_cast<unsigned char>(bytes[1]) == 0xFE) ||
                              (!littleEndian && static_cast<unsigned char>(bytes[0]) == 0xFE &&
                               static_cast<unsigned char>(bytes[1]) == 0xFF)))
        offset = 2;
    if ((bytes.size() - offset) % 2 != 0)
        return {MaiErrorCode::InvalidInput, "UTF-16 file ends with an incomplete code unit"};
    std::string output;
    output.reserve(bytes.size());
    auto unitAt = [&](std::size_t at) {
        const auto first = static_cast<unsigned char>(bytes[at]);
        const auto second = static_cast<unsigned char>(bytes[at + 1]);
        return static_cast<std::uint32_t>(littleEndian ? first | (second << 8)
                                                       : (first << 8) | second);
    };
    for (; offset < bytes.size(); offset += 2) {
        std::uint32_t codePoint = unitAt(offset);
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (offset + 3 >= bytes.size())
                return {MaiErrorCode::InvalidInput, "UTF-16 file ends with a high surrogate"};
            const std::uint32_t low = unitAt(offset + 2);
            if (low < 0xDC00 || low > 0xDFFF)
                return {MaiErrorCode::InvalidInput, "UTF-16 file has an invalid surrogate pair"};
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            offset += 2;
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            return {MaiErrorCode::InvalidInput, "UTF-16 file has an unmatched low surrogate"};
        }
        appendUtf8(output, codePoint);
    }
    if (looksBinary(output))
        return {MaiErrorCode::InvalidInput, "file contains binary control bytes"};
    return output;
}

std::string normalizedEncoding(const std::string& name) {
    std::string result = name.empty() ? "auto" : name;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    return result;
}

// 按调用方指定的旧编码读取文件字节，再转为模型可处理的文本。
// 转码失败返回错误，不以损坏文本覆盖源文件。
MaiResult<std::string> decodeLegacy(const std::string& bytes, const std::string& encoding,
                                    const MaiToolContext& context) {
    // 整文件检索与流式读取共用同一个平台后端，Apple 编码补充不能只修普通 reader。
    return MaiTextFileReader::decodeLegacyText(bytes, encoding, context.decodeText);
}

MaiResult<std::string> decodeFileText(const std::string& bytes, const std::string& requested,
                                      const MaiToolContext& context) {
    const std::string encoding = normalizedEncoding(requested);
    if (encoding == "utf-16le" || encoding == "utf16le") return decodeUtf16(bytes, true);
    if (encoding == "utf-16be" || encoding == "utf16be") return decodeUtf16(bytes, false);
    if (encoding == "auto" && bytes.size() >= 2) {
        if (static_cast<unsigned char>(bytes[0]) == 0xFF &&
            static_cast<unsigned char>(bytes[1]) == 0xFE)
            return decodeUtf16(bytes, true);
        if (static_cast<unsigned char>(bytes[0]) == 0xFE &&
            static_cast<unsigned char>(bytes[1]) == 0xFF)
            return decodeUtf16(bytes, false);
    }
    if (encoding == "auto" || encoding == "utf-8" || encoding == "utf8") {
        const std::size_t bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                                        static_cast<unsigned char>(bytes[1]) == 0xBB &&
                                        static_cast<unsigned char>(bytes[2]) == 0xBF
                                    ? 3
                                    : 0;
        const std::string utf8 = bytes.substr(bom);
        if (isValidUtf8(utf8) && !looksBinary(utf8)) return utf8;
        if (encoding != "auto")
            return {MaiErrorCode::InvalidInput,
                    "file is not valid UTF-8; retry with an explicit encoding"};
    }
    if (looksBinary(bytes))
        return {MaiErrorCode::InvalidInput,
                "file looks binary or uses UTF-16 without a BOM; specify encoding explicitly"};
    MaiResult<std::string> decoded = decodeLegacy(bytes, encoding, context);
    if (!decoded) return decoded;
    if (!isValidUtf8(decoded.value()) || looksBinary(decoded.value()))
        return {MaiErrorCode::InvalidInput, "decoded file is not valid UTF-8 text"};
    return decoded;
}

// 把 root 之外的路径挡掉，并给模型一句它能据此改正的话。
struct Resolved {
    std::string path;
    MaiToolResult error;
    bool ok = false;
};

Resolved resolveOrFail(const MaiToolContext& context, const std::string& rawPath) {
    if (rawPath.empty())
        return {
            {},
            MaiToolResult::failure(MaiErrorCode::InvalidInput, "missing required parameter: path"),
            false};
    const std::string resolved = context.resolvePath(rawPath);
    if (resolved.empty()) {
        return {
            {},
            MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                   "Path is outside the area accessible to this host: " + rawPath),
            false};
    }
    return {resolved, {}, true};
}

// 自己按段算，不调系统的 PathRelativePathToW：那个 API 在两边不同盘时行为古怪，
// root 内的路径逐段裁成相对路径；宿主允许 root 外访问时保留完整绝对路径。
//
// 一律返回 generic 形式（'/' 分隔）的 UTF-8：模型看到的路径在三个平台上长得一样，
// 它给回来的我们也认。
std::string toRelativePath(const std::string& root, const MaiFilePath& path) {
    const MaiFilePath rootPath = MaiFilePath::fromUtf8(root);
    if (path != rootPath && !rootPath.isParentOf(path)) return path.toGenericUtf8();
    const auto rootParts = rootPath.components();
    const auto pathParts = path.components();
    if (pathParts.size() <= rootParts.size()) return path.toGenericUtf8();

    MaiFilePath relative;
    for (std::size_t i = rootParts.size(); i < pathParts.size(); ++i)
        relative = relative.append(MaiFilePath(pathParts[i]));
    return relative.isEmpty() ? path.toGenericUtf8() : relative.toGenericUtf8();
}

// ── file_read ───────────────────────────────────────────────────
// 文件工具只适配 JSON；普通宿主直接使用 MaiTextFileReader/MaiFileSearch。
// 输出保留可读的路径/行号，footer 单独声明覆盖范围和真实可续用的游标。
MaiToolResult observedRead(MaiToolResult result, bool started = true,
                           const std::string& recovery = {}) {
    MaiToolExecution execution;
    execution.started = started;
    execution.effectStatus = MaiToolEffectStatus::None;
    execution.canceled = result.error().code() == MaiErrorCode::Canceled;
    execution.status = execution.canceled  ? MaiToolExecutionStatus::Canceled
                       : result.hasError() ? MaiToolExecutionStatus::Failed
                                           : MaiToolExecutionStatus::Completed;
    execution.outputTruncated = result.isTruncated();
    if (result.error().code() == MaiErrorCode::InvalidInput)
        execution.nextAction = MaiToolNextAction::ReviseArguments;
    else if (!recovery.empty()) {
        execution.nextAction = MaiToolNextAction::ReadOutput;
        execution.recoveryTool = recovery;
    }
    return result.withExecution(std::move(execution));
}

std::string validateSearchArguments(const json& args, std::initializer_list<const char*> allowed) {
    if (!args.is_object()) return "arguments must be a JSON object";
    for (auto it = args.begin(); it != args.end(); ++it) {
        bool known = false;
        for (const char* key : allowed) known = known || it.key() == key;
        if (!known) return "unsupported argument: " + it.key();
        const auto& value = it.value();
        const std::string key = it.key();
        if (key == "limit" || key == "offset" || key == "max_bytes" || key == "context_before" ||
            key == "context_after") {
            const std::uint64_t minimum = key.compare(0, 8, "context_") == 0 ? 0 : 1;
            const std::uint64_t maximum = key == "max_bytes" ? 1048576
                                          : key == "limit"   ? 10000
                                          : key == "offset"  ? 1000000000
                                                             : 10;
            if (!value.is_number_integer() ||
                (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
                value.get<std::uint64_t>() < minimum || value.get<std::uint64_t>() > maximum)
                return key + " is outside its supported integer range";
        } else if (key == "case_sensitive" || key == "include_hidden" || key == "include_ignored" ||
                   key == "refresh" || key == "summary_only" || key == "recursive") {
            if (!value.is_boolean()) return key + " must be a boolean";
        } else if (key == "exclude") {
            if (!value.is_array() || value.size() > 32) return "exclude must be at most 32 globs";
            for (const auto& item : value)
                if (!item.is_string() || item.get_ref<const std::string&>().size() > 4096)
                    return "exclude must contain string globs of at most 4096 bytes";
        } else {
            const std::size_t maximum = key == "path" ? 32768 : key == "cursor" ? 256 : 4096;
            if (!value.is_string() || value.get_ref<const std::string&>().size() > maximum)
                return key + " must be a bounded string";
        }
    }
    return {};
}

std::string fileScope(const MaiToolContext& context, const std::string& encoding) {
    // 不保存回调/上下文指针；会话、访问边界或编码配置变化都不能复用旧查询。
    return json::array({context.sessionId, context.root, context.fileAccessRoot,
                        context.allowOutsideWorkingDirectory, normalizedEncoding(encoding)})
        .dump();
}

MaiFileSearch& sharedFileSearch() {
    static MaiFileSearch service;
    return service;
}

MaiFileSearchCallbacks searchCallbacks(const MaiToolContext& context, const std::string& encoding) {
    MaiFileSearchCallbacks callbacks;
    callbacks.isCanceled = [&context] { return context.isCanceled(); };
    callbacks.pathAllowed = [&context](const MaiFilePath& path) {
        return !context.resolvePath(path.toUtf8()).empty();
    };
    callbacks.decodeText = [&context, encoding](const MaiFilePath&, const std::string& bytes) {
        return decodeFileText(bytes, encoding, context);
    };
    return callbacks;
}

void configureSearchFilter(const json& args, MaiFileSearchFilter& filter) {
    filter.includeHidden = args.value("include_hidden", false);
    const bool ignored = args.value("include_ignored", false);
    filter.respectGitIgnore = !ignored;
    filter.useDefaultExcludes = !ignored;
    if (args.contains("exclude"))
        filter.excludeGlobs = args["exclude"].get<std::vector<std::string>>();
    const auto glob = stringArgument(args, "glob");
    if (!glob.empty()) filter.includeGlobs.push_back(glob);
}

std::string displayedSearchPath(const std::string& root, const MaiFilePath& base, bool directory,
                                const std::string& relative) {
    return toRelativePath(root, directory ? base.append(MaiFilePath::fromUtf8(relative)) : base);
}

MaiToolResult renderSearchPage(const MaiFileSearchPage& page, const MaiToolContext& context,
                               const MaiFilePath& base, const std::string& tool,
                               MaiFileContentSearchMode mode = MaiFileContentSearchMode::Lines,
                               bool summaryOnly = false, bool recursive = true,
                               bool defaultExcludes = true) {
    json metadata = {
        {"complete", !page.error.hasError() && !page.partial && !page.hasMore && !page.canceled},
        {"partial", page.partial},
        {"has_more", page.hasMore},
        {"count", page.files.size() + page.matches.size()},
        {"coverage", "filesystem_snapshot"},
        {"snapshot_id", page.snapshotId},
        {"snapshot_age_ms", page.snapshotAgeMilliseconds},
        {"cache_reused", page.cacheReused},
        {"visited_entries", page.visitedEntries},
        {"searched_files", page.searchedFiles},
        {"read_bytes", page.readBytes},
        {"matched_lines", page.matchedLines}};
    metadata["default_excludes"] = defaultExcludes;
    metadata["excluded_nested_repositories"] = page.excludedNestedRepositories;
    metadata["excluded_nested_repository_paths"] = page.excludedNestedRepositoryPaths;
    metadata["excluded_nested_repository_paths_omitted"] =
        page.excludedNestedRepositories - page.excludedNestedRepositoryPaths.size();
    if (tool == "file_glob") metadata["recursive"] = recursive;
    if (tool == "file_grep") {
        metadata["content_search_started"] = page.contentSearchStarted;
        metadata["not_searched"] = !page.contentSearchStarted;
        metadata["search_phase"] = page.contentSearchStarted
                                       ? (metadata["complete"].get<bool>() ? "complete" : "content")
                                   : metadata["complete"].get<bool>() ? "empty_scope"
                                                                      : "not_searched";
    }
    metadata["count_unit"] =
        tool == "file_glob" && !recursive                                               ? "entries"
        : (tool == "file_glob" && recursive) || mode == MaiFileContentSearchMode::Files ? "files"
        : mode == MaiFileContentSearchMode::Count ? "file_counts"
                                                  : "matching_lines";
    metadata["total_matches_unit"] =
        tool == "file_glob" && !recursive ? "entries"
        : (tool == "file_glob" && recursive) || mode == MaiFileContentSearchMode::Files
            ? "files"
            : "matching_lines";
    metadata["total_matches"] = page.totalMatches ? json(*page.totalMatches) : json(nullptr);
    metadata["total_matches_exact"] = page.totalMatches.has_value();
    metadata["total_matches_lower_bound"] = page.totalMatchesLowerBound;
    metadata["total_matching_files"] =
        page.totalMatchingFiles ? json(*page.totalMatchingFiles) : json(nullptr);
    metadata["total_matching_files_exact"] = page.totalMatchingFiles.has_value();
    metadata["total_matching_files_lower_bound"] = page.totalMatchingFilesLowerBound;
    if (!page.nextCursor.empty()) metadata["next_cursor"] = page.nextCursor;
    json issues = json::array();
    for (const auto& issue : page.issues) {
        if (issues.size() == 8) break;
        std::string path = issue.path;
        truncateUtf8(path, 256);
        issues.push_back({{"reason", issue.reason}, {"path", path}, {"count", issue.count}});
    }
    metadata["issues"] = std::move(issues);
    metadata["issues_omitted"] = page.issues.size() > 8 ? page.issues.size() - 8 : 0;
    std::string output;
    const bool directory = MaiFileSystem::isDirectory(base);
    json entries = json::array();
    for (const auto& file : page.files) {
        const auto path = displayedSearchPath(context.root, base, directory, file.path);
        output += path + (file.isDirectory ? "/\n" : "\n");
        if (!recursive)
            entries.push_back({{"path", path}, {"type", file.isDirectory ? "directory" : "file"}});
    }
    if (tool == "file_glob" && !recursive) metadata["entries"] = std::move(entries);
    std::size_t clipped = 0;
    for (const auto& match : page.matches) {
        const auto path = displayedSearchPath(context.root, base, directory, match.path);
        if (mode == MaiFileContentSearchMode::Files)
            output += path + "\n";
        else if (mode == MaiFileContentSearchMode::Count)
            output += path + ": " + std::to_string(match.count) + "\n";
        else {
            const auto append = [&](std::size_t line, const std::string& text, char separator) {
                // 上下文与本命中成组，只在命中行写完整路径。否则深路径会在 20 行
                // 上下文中反复放大，超过普通搜索已预留的输出预算且游标已经前进。
                output += separator == ':' ? path + ":" : "  ";
                output += std::to_string(line) + separator + " " + text + "\n";
            };
            for (const auto& line : match.before) append(line.line, line.text, '-');
            append(match.line, match.text, ':');
            for (const auto& line : match.after) append(line.line, line.text, '-');
            if (match.truncated) {
                ++clipped;
                output += "[line preview starts at UTF-8 byte " +
                          std::to_string(match.displayOffsetBytes) +
                          "; use file_read for the full line]\n";
            }
        }
    }
    metadata["clipped_line_previews"] = clipped;
    if (output.empty() && tool == "file_grep" && !page.contentSearchStarted && !page.error)
        output = metadata["complete"].get<bool>()
                     ? "No ordinary files are present in this filtered scope."
                     : "File content search has not started; no conclusion about content matches "
                       "is available.";
    if (output.empty() && summaryOnly && !page.error) {
        const auto total = page.totalMatches
                               ? std::to_string(*page.totalMatches)
                               : "at least " + std::to_string(page.totalMatchesLowerBound);
        const auto files = page.totalMatchingFiles
                               ? std::to_string(*page.totalMatchingFiles)
                               : "at least " + std::to_string(page.totalMatchingFilesLowerBound);
        output =
            "Summary: " + total +
            (mode == MaiFileContentSearchMode::Files ? " matching files"
                                                     : " matching lines in " + files + " files");
        if (!page.totalMatches) output += "; totals are not exact yet.";
    }
    if (output.empty())
        output = page.error.hasError() ? "Search failed; results are unavailable."
                 : page.partial || page.canceled
                     ? "Search is partial; no match in the scanned portion."
                 : page.hasMore ? "No match in this page; the search has more pages."
                                : "No content matches in this filesystem snapshot.";
    if (tool == "file_glob" && page.files.empty() && !page.error.hasError() && !page.partial &&
        !page.canceled && !page.hasMore)
        output = "No files match in this filesystem snapshot.";
    if (page.partial)
        output +=
            "\nSearch stopped at a scan limit or skipped files; narrow path/glob or inspect "
            "issues.";
    if (page.hasMore)
        output +=
            "\nContinue with next_cursor and the same arguments. Use refresh=true without a cursor "
            "for a new snapshot.";
    output += "\n\n[file_result]\n" + metadata.dump();
    // 绝不在游标已经推进后裁掉结果。预算在交给普通搜索前为行号、路径前缀及 footer 预留。
    if (output.size() > kMaxOutputBytes)
        return observedRead(MaiToolResult::failure(MaiErrorCode::Internal,
                                                   "file search rendering exceeded its reserved "
                                                   "output budget; narrow path or lower limit"));
    const bool truncated = page.partial || page.hasMore || clipped != 0;
    if (page.error.hasError())
        return observedRead(MaiToolResult::failure(
            page.error.code(), page.error.message() + "\n" + output, truncated));
    if (page.canceled || context.isCanceled())
        return observedRead(MaiToolResult::failure(MaiErrorCode::Canceled,
                                                   "File search canceled.\n" + output, true));
    return observedRead(MaiToolResult::success(std::move(output), truncated), true,
                        page.hasMore   ? tool
                        : clipped != 0 ? "file_read"
                                       : "");
}

std::size_t renderedPageLimit(const MaiToolContext& context, const MaiFilePath& base,
                              std::size_t requested, std::size_t contextLines) {
    const std::size_t prefix = toRelativePath(context.root, base).size() + 80;
    return std::max<std::size_t>(
        1, std::min(requested, (kMaxOutputBytes / 4) / (prefix * (contextLines + 1))));
}

class ReadTool final : public MaiTool {
public:
    // 只读文件查询可跨调用并发；路径与参数校验仍由同一 execute 核心完成。
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }

    std::string name() const override { return "file_read"; }
    std::string description() const override {
        return "Read a text file with line numbers and bounded streaming. Offset is a 1-based "
               "line, "
               "not a prefix byte limit. complete describes the requested line range, separately "
               "from eof and has_more. Use optional next_cursor without offset to continue a long "
               "line or "
               "scan. "
               "UTF-8/BOM UTF-16 are automatic; specify encoding for ambiguous legacy text.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"},"encoding":{"type":"string"},"offset":{"type":"integer","minimum":1},"limit":{"type":"integer","minimum":1,"maximum":10000},"max_bytes":{"type":"integer","minimum":4096,"maximum":1048576},"cursor":{"type":"string"}},"required":["path"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = json::parse(raw, nullptr, false);
        auto invalid = validateSearchArguments(
            args, {"path", "encoding", "offset", "limit", "max_bytes", "cursor"});
        if (!invalid.empty())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput, invalid), false);
        if (context.isCanceled())
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::Canceled, "File read canceled."), false);
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return observedRead(std::move(resolved.error), false);
        const auto path = MaiFilePath::fromUtf8(resolved.path);
        if (MaiFileSystem::isDirectory(path))
            return observedRead(
                MaiToolResult::failure(
                    MaiErrorCode::InvalidInput,
                    "That is a directory, not a file. Use file_glob to list its contents."),
                false);
        MaiTextFileReadOptions options;
        options.offset = args.value("offset", std::uint64_t{1});
        options.encoding = stringArgument(args, "encoding");
        options.isCanceled = [&context] { return context.isCanceled(); };
        // 只借用已有宿主解码器；空回调让普通 reader 选择共用的平台后端。
        options.decodeText = context.decodeText;
        const auto requestedBytes = args.value("max_bytes", kMaxReadBytes);
        if (requestedBytes < 4096)
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                       "max_bytes must be at least 4096"),
                                false);
        options.limit =
            std::min(args.value("limit", std::size_t{500}), (requestedBytes - 2048) / 160);
        options.maxOutputBytes = requestedBytes - 2048 - options.limit * 80;
        const auto binding =
            json::array({fileScope(context, options.encoding), resolved.path}).dump();
        const std::string token = stringArgument(args, "cursor");
        if (!token.empty()) {
            if (options.offset != 1)
                return observedRead(
                    MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                           "cursor resumes its saved position; omit offset"),
                    false);
            std::lock_guard<std::mutex> lock(mMutex);
            prune();
            const auto found = mCursors.find(token);
            if (found == mCursors.end() || found->second.binding != binding)
                return observedRead(
                    MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "file read cursor expired or belongs to another path, encoding or Session"),
                    false);
            found->second.touched = std::chrono::steady_clock::now();
            options.cursor = found->second.cursor;
        }
        const auto result = MaiTextFileReader::read(path, options);
        if (!result) {
            const auto message = result.error().code() == MaiErrorCode::NotFound
                                     ? "file does not exist: " + stringArgument(args, "path") +
                                           "; " + result.error().message()
                                     : result.error().message();
            return observedRead(MaiToolResult::failure(result.error().code(), message));
        }
        const auto& page = result.value();
        const auto requestedLimit = args.value("limit", std::size_t{500});
        const bool partialLine = !page.lines.empty() && !page.lines.back().complete;
        const bool requestedRangeComplete =
            page.eof || (page.stopReason == MaiTextFileStopReason::LineLimit &&
                         options.limit == requestedLimit && !partialLine);
        const bool outputTruncated = !requestedRangeComplete;
        json metadata = {{"complete", requestedRangeComplete},
                         {"requested_range_complete", requestedRangeComplete},
                         {"output_truncated", outputTruncated},
                         {"has_more", !page.eof},
                         {"eof", page.eof},
                         {"encoding", page.encoding},
                         {"count", page.lines.size()},
                         {"coverage", "file_snapshot"},
                         {"open_operations", page.io.openOperations},
                         {"read_operations", page.io.readOperations},
                         {"read_bytes", page.io.bytesRead}};
        // 复用已验证句柄的原始字节快照；虚拟 size-zero 内容不能伪称空文件。
        const bool totalBytesExact = page.snapshot.bytes != 0 || page.io.bytesRead == 0;
        metadata["total_bytes"] = totalBytesExact ? json(page.snapshot.bytes) : json(nullptr);
        metadata["total_bytes_exact"] = totalBytesExact;
        metadata["total_bytes_unit"] = "raw_bytes";
        std::string output;
        for (const auto& line : page.lines)
            output += std::to_string(line.number) + "\t" + line.text + "\n";
        if (!page.lines.empty()) {
            metadata["first_line"] = page.lines.front().number;
            metadata["first_line_utf8_offset"] = page.lines.front().byteOffset;
            metadata["last_line"] = page.lines.back().number;
            metadata["last_line_complete"] = page.lines.back().complete;
        }
        if (page.nextCursor) {
            const auto next = MaiIdGenerator::generate("file_read_");
            {
                std::lock_guard<std::mutex> lock(mMutex);
                prune();
                while (mCursors.size() >= 128) {
                    auto oldest = mCursors.begin();
                    for (auto it = mCursors.begin(); it != mCursors.end(); ++it)
                        if (it->second.touched < oldest->second.touched) oldest = it;
                    mCursors.erase(oldest);
                }
                mCursors.emplace(next, StoredCursor{*page.nextCursor, binding,
                                                    std::chrono::steady_clock::now()});
            }
            metadata["next_cursor"] = next;
            metadata["next_line"] = page.nextCursor->lineNumber;
            metadata["next_line_utf8_offset"] = page.nextCursor->lineUtf8Offset;
        }
        switch (page.stopReason) {
            case MaiTextFileStopReason::EndOfFile: metadata["stop_reason"] = "eof"; break;
            case MaiTextFileStopReason::LineLimit: metadata["stop_reason"] = "line_limit"; break;
            case MaiTextFileStopReason::OutputLimit:
                metadata["stop_reason"] = "output_limit";
                break;
            case MaiTextFileStopReason::ScanLimit: metadata["stop_reason"] = "scan_limit"; break;
            case MaiTextFileStopReason::TimeLimit: metadata["stop_reason"] = "time_limit"; break;
        }
        if (output.empty())
            output =
                page.eof
                    ? "(empty file or no content at the requested line)"
                    : "(scan budget reached before the requested line; continue with next_cursor)";
        if (outputTruncated)
            output +=
                "\n...Output truncated before the requested range completed. Continue with "
                "next_cursor; omit offset.";
        else if (!page.eof)
            output +=
                "\nRequested line range complete. More file content is available; next_cursor is "
                "optional.";
        output += "\n\n[file_result]\n" + metadata.dump();
        if (context.isCanceled())
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::Canceled, "File read canceled."));
        return observedRead(MaiToolResult::success(std::move(output), outputTruncated), true,
                            outputTruncated && page.nextCursor ? "file_read" : "");
    }

private:
    struct StoredCursor {
        MaiTextFileCursor cursor;
        std::string binding;
        std::chrono::steady_clock::time_point touched;
    };
    void prune() {
        const auto oldest = std::chrono::steady_clock::now() - std::chrono::seconds(120);
        for (auto it = mCursors.begin(); it != mCursors.end();)
            if (it->second.touched < oldest)
                it = mCursors.erase(it);
            else
                ++it;
    }
    std::mutex mMutex;
    std::unordered_map<std::string, StoredCursor> mCursors;
};

// ── explicit file and directory mutations ─────────────────────
class CreateFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_create";
    }
    std::string description() const override {
        // 只创建不存在的空文件；路径已有文件时不覆盖。
        return "Create a new empty file without overwriting an existing file. Missing parent "
               "directories are created. Use file_write when the file needs content.";
    }
    std::string parametersSchema() const override {
        // 创建空文件只需 path；路径可相对工作目录，但执行时不允许覆盖已有文件。
        return R"({"type":"object","properties":{"path":{"type":"string",)"
               R"("description":"Absolute path or path relative to the working directory"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_create requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        if (MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "path already exists; nothing was overwritten");
        const MaiFilePath parent = path.dirName();
        if (!parent.isEmpty() && parent != path) {
            const MaiError error = MaiFileSystem::createDirectories(parent);
            if (error.hasError())
                return MaiToolResult::failure(
                    error.code(), "could not create parent directory: " + error.message());
        }
        const MaiError error = MaiFileSystem::createEmptyFile(path);
        if (error.hasError()) {
            if (MaiFileSystem::exists(path))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "path already exists; nothing was overwritten");
            return MaiToolResult::failure(error.code(), error.message());
        }
        return MaiToolResult::success("Created file " + toRelativePath(context.root, path));
    }
};

class CreateDirectoryTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_create_directory";
    }
    std::string description() const override {
        // 递归补齐父目录；若目标路径已被普通文件占用则返回错误。
        return "Create a directory and any missing parents. An existing directory is accepted; "
               "a file at that path is an error.";
    }
    std::string parametersSchema() const override {
        // 创建目录只需 path；已存在的目录可复用，普通文件占位则报错。
        return R"({"type":"object","properties":{"path":{"type":"string",)"
               R"("description":"Absolute path or path relative to the working directory"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"directory:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "file_create_directory requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        if (MaiFileSystem::exists(path) && !MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "a file already exists at the requested directory path");
        const bool existed = MaiFileSystem::isDirectory(path);
        const MaiError error = MaiFileSystem::createDirectories(path);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());
        if (!MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "directory creation returned without a directory");
        return MaiToolResult::success(
            std::string(existed ? "Directory already exists " : "Created directory ") +
            toRelativePath(context.root, path));
    }
};

class DeleteFileTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_delete";
    }
    std::string description() const override {
        // 只删除指定的单个文件，不删目录也不递归。
        return "Delete one existing file. Directories are never deleted; this is not recursive.";
    }
    std::string parametersSchema() const override {
        // 删除指定 path 的单个文件；没有递归目录参数。
        return R"({"type":"object","properties":{"path":{"type":"string",)"
               R"("description":"Absolute path or path relative to the working directory"}},)"
               R"("required":["path"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const std::string path =
            context.resolvePath(stringArgument(parseArguments(argumentsJson), "path"));
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        if (args.size() != 1 || !args.contains("path") || !args["path"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_delete requires one string parameter named path");
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        MaiFilePath requested = MaiFilePath::fromUtf8(stringArgument(args, "path"));
        if (!requested.isAbsolute())
            requested = MaiFilePath::fromUtf8(context.root).append(requested);
        if (MaiFileSystem::isSymbolicLink(requested))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_delete refuses symbolic links; use exec_command with "
                                          "the selected Agent policy to remove the link itself");
        if (!MaiFileSystem::exists(path))
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + stringArgument(args, "path"));
        if (MaiFileSystem::isDirectory(path))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_delete cannot delete a directory");
        const MaiError error = MaiFileSystem::removeFile(path);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());
        return MaiToolResult::success("Deleted file " + toRelativePath(context.root, path));
    }
};

// 拒绝用户原始路径中的链接段，避免规范化把末段链接藏成普通目标。
bool hasLinkComponent(const MaiToolContext& context, const std::string& raw) {
    auto path = MaiFilePath::fromUtf8(raw);
    if (!path.isAbsolute()) path = MaiFilePath::fromUtf8(context.root).append(path);
    for (;;) {
        if (MaiFileSystem::isSymbolicLink(path)) return true;
        const auto parent = path.dirName();
        if (parent.isEmpty() || parent == path) return false;
        path = parent;
    }
}

struct MaiFileMutationSnapshot {
    std::string source;
    std::string destination;
    std::string sourceIdentity;
    std::string parentIdentity;
    std::string sessionId;
    std::string turnId;
};

// 审批期间保存待执行调用的元数据；按真实调用与轮次隔离，不保存宿主指针。
class MaiFileMutationSnapshots {
public:
    void remember(const std::string& key, MaiFileMutationSnapshot snapshot,
                  const MaiToolContext& context) const;
    void finishTurn(const std::string& sessionId, const std::string& turnId) const;
    MaiFileMutationSnapshot take(const std::string& key) const;

private:
    mutable std::mutex mMutex;
    mutable std::unordered_map<std::string, MaiFileMutationSnapshot> mSnapshots;
};
void MaiFileMutationSnapshots::remember(const std::string& key, MaiFileMutationSnapshot snapshot,
                                        const MaiToolContext& context) const {
    std::lock_guard<std::mutex> lock(mMutex);
    // 保留所有等待执行的审批，不能容量淘汰后按当前身份继续。正常执行即消费，
    // 被拒或被取消的调用由真实轮次的 onTurnFinished 清理，不留下跨轮次缓存。
    snapshot.sessionId = context.sessionId;
    snapshot.turnId = context.turnId;
    mSnapshots[key] = std::move(snapshot);
}
MaiFileMutationSnapshot MaiFileMutationSnapshots::take(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mMutex);
    const auto found = mSnapshots.find(key);
    if (found == mSnapshots.end()) return {};
    auto snapshot = found->second;
    mSnapshots.erase(found);
    return snapshot;
}
void MaiFileMutationSnapshots::finishTurn(const std::string& sessionId,
                                          const std::string& turnId) const {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto it = mSnapshots.begin(); it != mSnapshots.end();) {
        if (it->second.sessionId == sessionId && it->second.turnId == turnId)
            it = mSnapshots.erase(it);
        else
            ++it;
    }
}
std::string mutationSnapshotKey(const MaiToolContext& context, const std::string& arguments) {
    return json::array({context.sessionId, context.root, context.fileAccessRoot,
                        context.allowOutsideWorkingDirectory, context.turnId, context.messageId,
                        context.partId, context.callId, arguments})
        .dump();
}

class FileTransferTool final : public MaiTool {
public:
    explicit FileTransferTool(bool move);
    std::string name() const override;
    std::string description() const override;
    std::string parametersSchema() const override;
    bool requiresApproval(const std::string&) const override;
    std::vector<std::string> approvalKeys(const std::string& raw,
                                          const MaiToolContext& context) const override;
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override;
    MaiError onTurnFinished(const std::string& sessionId, const std::string& turnId) override;

private:
    bool mMove;
    MaiFileMutationSnapshots mSnapshots;
};
FileTransferTool::FileTransferTool(bool move) : mMove(move) {}
std::string FileTransferTool::name() const {
    return mMove ? "file_move" : "file_copy";
}
std::string FileTransferTool::description() const {
    // 两个路径独立授权，不覆盖、不建父目录；移动只支持同文件系统，复制先暂存后发布。
    return mMove ? "Move one ordinary file to a new path on the same filesystem, without "
                   "overwriting or creating parents. Cross-filesystem moves are unsupported. "
                   "Requires approval for both paths."
                 : "Copy one ordinary file to a new path without overwriting or creating parents. "
                   "Publishes only after a complete copy. Requires approval for reading source and "
                   "creating destination. Links and directories are refused.";
}
std::string FileTransferTool::parametersSchema() const {
    // source/destination 必填；没有 overwrite 或目录递归隐式选项。
    return R"({"type":"object","properties":{"source":{"type":"string"},"destination":{"type":"string"}},"required":["source","destination"],"additionalProperties":false})";
}
bool FileTransferTool::requiresApproval(const std::string&) const {
    return true;
}
std::vector<std::string> FileTransferTool::approvalKeys(const std::string& raw,
                                                        const MaiToolContext& context) const {
    const auto args = parseArguments(raw);
    const auto source = context.resolvePath(stringArgument(args, "source"));
    const auto destination = context.resolvePath(stringArgument(args, "destination"));
    if (args.size() != 2 || source.empty() || destination.empty())
        return {"invalid:" + name() + ":" + raw};
    const auto info = MaiFileSystem::stat(MaiFilePath::fromUtf8(source));
    const auto parent = MaiFileSystem::stat(MaiFilePath::fromUtf8(destination).dirName());
    mSnapshots.remember(mutationSnapshotKey(context, raw),
                        {source, destination, info ? info.value().identity : "invalid",
                         parent ? parent.value().identity : "invalid"},
                        context);
    return {name() + ":" + (mMove ? "source:" : "read:") + source,
            name() + ":create:" + destination};
}
MaiToolResult FileTransferTool::execute(const std::string& raw, const MaiToolContext& context) {
    const auto args = parseArguments(raw);
    if (args.size() != 2 || stringArgument(args, "source").empty() ||
        stringArgument(args, "destination").empty())
        return MaiToolResult::failure(
            MaiErrorCode::InvalidInput,
            "source and destination must be nonempty strings; no other arguments are supported");
    const auto source = resolveOrFail(context, stringArgument(args, "source"));
    const auto destination = resolveOrFail(context, stringArgument(args, "destination"));
    if (!source.ok) return source.error;
    if (!destination.ok) return destination.error;
    if (hasLinkComponent(context, stringArgument(args, "source")) ||
        hasLinkComponent(context, stringArgument(args, "destination")))
        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                      "file transfer refuses symbolic links and junctions");
    auto approved = mSnapshots.take(mutationSnapshotKey(context, raw));
    const auto parent = MaiFileSystem::stat(MaiFilePath::fromUtf8(destination.path).dirName());
    if (!parent) return MaiToolResult::failure(parent.error().code(), parent.error().message());
    if (!approved.source.empty() &&
        (approved.source != source.path || approved.destination != destination.path ||
         approved.parentIdentity != parent.value().identity))
        return MaiToolResult::failure(
            MaiErrorCode::InvalidInput,
            "transfer paths or destination parent changed after approval");
    const auto canceled = [&context] { return context.isCanceled(); };
    const auto error = mMove ? MaiFileSystem::moveFile(MaiFilePath::fromUtf8(source.path),
                                                       MaiFilePath::fromUtf8(destination.path),
                                                       approved.sourceIdentity, canceled)
                             : MaiFileSystem::copyFile(MaiFilePath::fromUtf8(source.path),
                                                       MaiFilePath::fromUtf8(destination.path),
                                                       approved.sourceIdentity, canceled);
    if (error) return MaiToolResult::failure(error.code(), error.message());
    return MaiToolResult::success(std::string(mMove ? "Moved " : "Copied ") + source.path + " to " +
                                  destination.path);
}

class DeleteDirectoryTool final : public MaiTool {
public:
    std::string name() const override;
    std::string description() const override;
    std::string parametersSchema() const override;
    bool requiresApproval(const std::string&) const override;
    std::vector<std::string> approvalKeys(const std::string& raw,
                                          const MaiToolContext& context) const override;
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override;
    MaiError onTurnFinished(const std::string& sessionId, const std::string& turnId) override;

private:
    MaiFileMutationSnapshots mSnapshots;
};
std::string DeleteDirectoryTool::name() const {
    return "file_delete_directory";
}
std::string DeleteDirectoryTool::description() const {
    // 默认只删空目录；recursive 必须显式授权，拒绝会话根和祖先，失败保留真实部分进度。
    return "Delete an empty directory. Set recursive=true explicitly to delete its ordinary "
           "descendants under a separate approval scope. Refuses filesystem/session roots, their "
           "ancestors, links and junctions. Reports partial removals if canceled or failed; "
           "deletion is not transactional.";
}
std::string DeleteDirectoryTool::parametersSchema() const {
    return R"({"type":"object","properties":{"path":{"type":"string"},"recursive":{"type":"boolean","default":false}},"required":["path"],"additionalProperties":false})";
}
bool DeleteDirectoryTool::requiresApproval(const std::string&) const {
    return true;
}
std::vector<std::string> DeleteDirectoryTool::approvalKeys(const std::string& raw,
                                                           const MaiToolContext& context) const {
    const auto args = parseArguments(raw);
    const auto path = context.resolvePath(stringArgument(args, "path"));
    if (path.empty() || (args.contains("recursive") && !args["recursive"].is_boolean()))
        return {"invalid:file_delete_directory:" + raw};
    const auto info = MaiFileSystem::stat(MaiFilePath::fromUtf8(path));
    mSnapshots.remember(mutationSnapshotKey(context, raw),
                        {path, {}, info ? info.value().identity : "invalid", {}}, context);
    return {std::string("directory-delete:") +
            (args.value("recursive", false) ? "recursive:" : "empty:") + path};
}
MaiToolResult DeleteDirectoryTool::execute(const std::string& raw, const MaiToolContext& context) {
    const auto args = parseArguments(raw);
    if (stringArgument(args, "path").empty() ||
        args.size() != (args.contains("recursive") ? 2u : 1u) ||
        (args.contains("recursive") && !args["recursive"].is_boolean()))
        return MaiToolResult::failure(
            MaiErrorCode::InvalidInput,
            "path must be a nonempty string and recursive must be a boolean");
    const auto resolved = resolveOrFail(context, stringArgument(args, "path"));
    if (!resolved.ok) return resolved.error;
    const auto path = MaiFilePath::fromUtf8(resolved.path);
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8(context.root));
    if (root.isEmpty() || path == root || path.isParentOf(root) || path == path.dirName() ||
        hasLinkComponent(context, stringArgument(args, "path")))
        return MaiToolResult::failure(
            MaiErrorCode::InvalidInput,
            "directory deletion refuses filesystem/session roots, their ancestors and links");
    const auto approved = mSnapshots.take(mutationSnapshotKey(context, raw));
    if (!approved.source.empty() && approved.source != resolved.path)
        return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                      "directory path changed after approval");
    MaiDirectoryDeleteOptions options;
    options.recursive = args.value("recursive", false);
    options.expectedIdentity = approved.sourceIdentity;
    options.isCanceled = [&context] { return context.isCanceled(); };
    options.pathAllowed = [&context, path](const MaiFilePath& candidate) {
        return (candidate == path || path.isParentOf(candidate)) &&
               !context.resolvePath(candidate.toUtf8()).empty();
    };
    const auto result = MaiFileSystem::deleteDirectory(path, options);
    json metadata = {
        {"removed_files", result.removedFiles},
        {"removed_directories", result.removedDirectories},
        {"partial", result.error.hasError() && (result.removedFiles || result.removedDirectories)}};
    const std::string output = "[file_result]\n" + metadata.dump();
    MaiToolExecution execution;
    execution.started = true;
    execution.status = result.error ? (result.error.code() == MaiErrorCode::Canceled
                                           ? MaiToolExecutionStatus::Canceled
                                           : MaiToolExecutionStatus::Failed)
                                    : MaiToolExecutionStatus::Completed;
    execution.effectStatus = result.removedFiles || result.removedDirectories
                                 ? MaiToolEffectStatus::Confirmed
                                 : MaiToolEffectStatus::None;
    execution.canceled = result.error.code() == MaiErrorCode::Canceled;
    return (result.error
                ? MaiToolResult::failure(result.error.code(),
                                         result.error.message() + "\n\n" + output)
                : MaiToolResult::success("Deleted directory " + resolved.path + "\n\n" + output))
        .withExecution(std::move(execution));
}

MaiError FileTransferTool::onTurnFinished(const std::string& sessionId, const std::string& turnId) {
    mSnapshots.finishTurn(sessionId, turnId);
    return {};
}
MaiError DeleteDirectoryTool::onTurnFinished(const std::string& sessionId,
                                             const std::string& turnId) {
    mSnapshots.finishTurn(sessionId, turnId);
    return {};
}

class StatTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_stat";
    }
    std::string description() const override {
        // 元数据只查询当前节点；普通文件原始字节数，目录大小不适用，时间为 Unix 秒。
        return "Inspect one ordinary file or directory without reading its contents. Returns type, "
               "raw size_bytes for files (null for directories), and modified_time in Unix "
               "seconds. Does not calculate recursive directory sizes or follow links.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = parseArguments(raw);
        if (args.size() != 1 || stringArgument(args, "path").empty())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                       "path must be one nonempty string"),
                                false);
        if (context.isCanceled())
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::Canceled, "file stat canceled"), false);
        const auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return observedRead(resolved.error, false);
        if (hasLinkComponent(context, stringArgument(args, "path")))
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                       "file_stat refuses symbolic links and junctions"),
                false);
        const auto info = MaiFileSystem::stat(MaiFilePath::fromUtf8(resolved.path));
        if (!info)
            return observedRead(
                MaiToolResult::failure(info.error().code(), info.error().message()));
        const auto& value = info.value();
        json metadata = {{"type", value.isDirectory ? "directory" : "file"},
                         {"size_bytes", value.sizeKnown ? json(value.bytes) : json(nullptr)},
                         {"size_bytes_exact", value.sizeKnown},
                         {"modified_time", value.modified},
                         {"modified_time_unit", "unix_seconds"}};
        return observedRead(
            MaiToolResult::success(resolved.path + "\n\n[file_result]\n" + metadata.dump()));
    }
};

// ── file_write ──────────────────────────────────────────────────
class WriteTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_write";
    }
    std::string description() const override {
        // 写入会替换目标文件的全部内容；局部编辑优先使用 file_edit 或补丁工具。
        return "Write content to a file, replacing whatever was "
               "there. Parent directories are created as needed.";
    }
    std::string parametersSchema() const override {
        // path 是目标文件，content 是写入的全部文本，不是追加片段。
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Absolute path or path relative to the wor)"
               R"(king directory"},)"
               R"("content":{"type":"string","description":"The full content to write"}},)"
               R"("required":["path","content"]})";
    }
    // 会改文件。M4 的闸门就位后这里会真正拦一道。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const json args = parseArguments(argumentsJson);
        if (!args.contains("path") || !args["path"].is_string())
            return MaiTool::approvalKeys(argumentsJson, context);
        const std::string path = context.resolvePath(args["path"].get<std::string>());
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        auto resolved = resolveOrFail(context, stringArgument(args, "path"));
        if (!resolved.ok) return resolved.error;
        if (!args.contains("content") || !args["content"].is_string())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: content");

        const std::string content = args["content"].get<std::string>();

        const MaiFilePath path = MaiFilePath::fromUtf8(resolved.path);
        const MaiFilePath parent = path.dirName();
        if (!parent.isEmpty() && parent != path) {
            const MaiError error = MaiFileSystem::createDirectories(parent);
            if (error.hasError())
                return MaiToolResult::failure(
                    error.code(), "could not create parent directory: " + error.message());
        }
        const MaiError error = MaiFileSystem::writeFile(path, content);
        if (error.hasError()) return MaiToolResult::failure(error.code(), error.message());

        return MaiToolResult::success("Wrote " + toRelativePath(context.root, path) + " (" +
                                      std::to_string(content.size()) + " bytes)");
    }
};

// ── file_glob / file_grep ───────────────────────────────────────
class GlobTool final : public MaiTool {
public:
    // 只读文件查询可跨调用并发；路径与参数校验仍由同一 execute 核心完成。
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }

    std::string name() const override { return "file_glob"; }
    std::string description() const override {
        return "Find files using either pattern (glob *, **, ?, [] and {a,b}) or query (ranked "
               "fuzzy name). "
               "Patterns without a slash match file basenames recursively by default. Set "
               "recursive=false for only immediate ordinary files, without descending. "
               "Dependency/vendor/build directories are excluded by default; include_ignored opts "
               "in. "
               "Path limits the directory. Results use a bounded path snapshot, reused up to 5 "
               "seconds; "
               "refresh starts a new snapshot, next_cursor continues the current one.";
    }
    std::string parametersSchema() const override {
        // 当前 Qwen XML 模板只从 object 根提取参数；根 union 会抹掉 properties。
        // 两个可选字段的互斥/必选规则由 execute 严格执行，不能为表达 XOR 破坏工具线格式。
        return R"({"type":"object","properties":{"pattern":{"type":"string","description":"Glob; provide exactly one of pattern or query"},"query":{"type":"string","description":"Fuzzy name; provide exactly one of pattern or query"},"path":{"type":"string"},"case_sensitive":{"type":"boolean"},"include_hidden":{"type":"boolean"},"include_ignored":{"type":"boolean","description":"Include gitignored and dependency/build directories"},"exclude":{"type":"array","items":{"type":"string"},"maxItems":32},"limit":{"type":"integer","minimum":1,"maximum":1000},"cursor":{"type":"string"},"refresh":{"type":"boolean"},"recursive":{"type":"boolean","default":true,"description":"False lists immediate ordinary files and directories; no subdirectory traversal"}},"required":[],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = json::parse(raw, nullptr, false);
        auto invalid = validateSearchArguments(
            args, {"path", "pattern", "query", "case_sensitive", "include_hidden",
                   "include_ignored", "exclude", "limit", "cursor", "refresh", "recursive"});
        const auto pattern = stringArgument(args, "pattern"), query = stringArgument(args, "query");
        if (invalid.empty() && (pattern.empty() == query.empty()))
            invalid = "provide exactly one nonempty pattern or query";
        if (invalid.empty() && args.value("limit", std::size_t{300}) > 1000)
            invalid = "limit must not exceed 1000";
        if (!invalid.empty())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput, invalid), false);
        auto resolved = resolveOrFail(context, args.value("path", std::string(".")));
        if (!resolved.ok) return observedRead(std::move(resolved.error), false);
        MaiFileNameSearchOptions options;
        options.root = MaiFilePath::fromUtf8(resolved.path);
        if (options.root == options.root.dirName())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                       "Searching an entire filesystem root is too "
                                                       "broad. Set path to a specific directory."),
                                false);
        options.scopeKey = fileScope(context, "");
        options.mode = pattern.empty() ? MaiFileNameSearchMode::Fuzzy : MaiFileNameSearchMode::Glob;
        options.query = pattern.empty() ? query : pattern;
        options.caseSensitive = args.value("case_sensitive", false);
        options.pageSize = renderedPageLimit(
            context, options.root,
            std::min<std::size_t>(1000, args.value("limit", std::size_t{300})), 0);
        options.budget.maxOutputBytes =
            args.value("recursive", true) ? kMaxOutputBytes / 2 : kMaxOutputBytes / 4;
        options.cursor = stringArgument(args, "cursor");
        options.refresh = args.value("refresh", false);
        options.recursive = args.value("recursive", true);
        configureSearchFilter(args, options.filter);
        return renderSearchPage(sharedFileSearch().findFiles(options, searchCallbacks(context, "")),
                                context, options.root, name(), MaiFileContentSearchMode::Lines,
                                false, options.recursive, options.filter.useDefaultExcludes);
    }
};

class GrepTool final : public MaiTool {
public:
    // 只读文件查询可跨调用并发；路径与参数校验仍由同一 execute 核心完成。
    MaiToolExecutionMode executionMode() const override {
        return MaiToolExecutionMode::ParallelReadOnly;
    }

    std::string name() const override { return "file_grep"; }
    std::string description() const override {
        return "Search full text lines before clipping previews. Prefer mode=literal for exact "
               "text; "
               "default regex is a bounded nonbacktracking subset (no lookarounds/backreferences). "
               "Path may be one file. Pages disclose skipped/partial coverage and next_cursor; "
               "read clipped lines with file_read. summary_only scans for totals without detail "
               "rows; "
               "count is page rows, total_matches is null until exact. Default scope excludes "
               "dependency/vendor/build directories and nested repositories; include_ignored opts "
               "in. not_searched distinguishes enumeration-only results from cached content "
               "results. Scan budgets: 5 seconds, "
               "50000 entries, 32 MiB total, 2 MiB per file.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"},"mode":{"type":"string","enum":["literal","regex"]},"encoding":{"type":"string"},"glob":{"type":"string"},"exclude":{"type":"array","items":{"type":"string"},"maxItems":32},"case_sensitive":{"type":"boolean"},"output_mode":{"type":"string","enum":["lines","files","count"]},"context_before":{"type":"integer","minimum":0,"maximum":10},"context_after":{"type":"integer","minimum":0,"maximum":10},"include_hidden":{"type":"boolean"},"include_ignored":{"type":"boolean"},"limit":{"type":"integer","minimum":1,"maximum":1000},"cursor":{"type":"string"},"refresh":{"type":"boolean"},"summary_only":{"type":"boolean","description":"Scan within budgets for totals without returning detail rows"}},"required":["pattern"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = json::parse(raw, nullptr, false);
        auto invalid = validateSearchArguments(
            args, {"path", "pattern", "mode", "encoding", "glob", "exclude", "case_sensitive",
                   "output_mode", "context_before", "context_after", "include_hidden",
                   "include_ignored", "limit", "cursor", "refresh", "summary_only"});
        if (invalid.empty() && args.value("limit", std::size_t{200}) > 1000)
            invalid = "limit must not exceed 1000";
        if (!invalid.empty())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput, invalid), false);
        const auto pattern = stringArgument(args, "pattern");
        const auto matchMode = args.value("mode", std::string("regex"));
        const auto outputMode = args.value("output_mode", std::string("lines"));
        if (pattern.empty())
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                       "missing required parameter: pattern"),
                                false);
        if (matchMode != "literal" && matchMode != "regex")
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::InvalidInput, "mode must be literal or regex"),
                false);
        if (outputMode != "lines" && outputMode != "files" && outputMode != "count")
            return observedRead(MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                       "output_mode must be lines, files or count"),
                                false);
        auto resolved = resolveOrFail(context, args.value("path", std::string(".")));
        if (!resolved.ok) return observedRead(std::move(resolved.error), false);
        MaiFileContentSearchOptions options;
        options.root = MaiFilePath::fromUtf8(resolved.path);
        if (options.root == options.root.dirName())
            return observedRead(
                MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                       "Searching an entire filesystem root is too broad. Set path "
                                       "to a file or specific directory."),
                false);
        const auto encoding = stringArgument(args, "encoding");
        options.scopeKey = fileScope(context, encoding);
        options.pattern = pattern;
        options.summaryOnly = args.value("summary_only", false);
        options.isRegex = matchMode == "regex";
        options.caseSensitive = args.value("case_sensitive", true);
        options.mode = outputMode == "files"   ? MaiFileContentSearchMode::Files
                       : outputMode == "count" ? MaiFileContentSearchMode::Count
                                               : MaiFileContentSearchMode::Lines;
        options.contextBefore = args.value("context_before", std::size_t{0});
        options.contextAfter = args.value("context_after", std::size_t{0});
        options.pageSize =
            renderedPageLimit(context, options.root,
                              std::min<std::size_t>(1000, args.value("limit", std::size_t{200})),
                              options.contextBefore + options.contextAfter);
        options.budget.maxOutputBytes = kMaxOutputBytes / 2;
        options.cursor = stringArgument(args, "cursor");
        options.refresh = args.value("refresh", false);
        configureSearchFilter(args, options.filter);
        return renderSearchPage(
            sharedFileSearch().searchText(options, searchCallbacks(context, encoding)), context,
            options.root, name(), options.mode, options.summaryOnly, true,
            options.filter.useDefaultExcludes);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiReadTool() {
    return std::make_unique<ReadTool>();
}
std::unique_ptr<MaiTool> makeMaiWriteTool() {
    return std::make_unique<WriteTool>();
}
std::unique_ptr<MaiTool> makeMaiCreateFileTool() {
    return std::make_unique<CreateFileTool>();
}
std::unique_ptr<MaiTool> makeMaiCreateDirectoryTool() {
    return std::make_unique<CreateDirectoryTool>();
}
std::unique_ptr<MaiTool> makeMaiDeleteFileTool() {
    return std::make_unique<DeleteFileTool>();
}
std::unique_ptr<MaiTool> makeMaiGlobTool() {
    return std::make_unique<GlobTool>();
}
std::unique_ptr<MaiTool> makeMaiGrepTool() {
    return std::make_unique<GrepTool>();
}

std::unique_ptr<MaiTool> makeMaiMoveFileTool() {
    return std::make_unique<FileTransferTool>(true);
}
std::unique_ptr<MaiTool> makeMaiCopyFileTool() {
    return std::make_unique<FileTransferTool>(false);
}
std::unique_ptr<MaiTool> makeMaiDeleteDirectoryTool() {
    return std::make_unique<DeleteDirectoryTool>();
}
std::unique_ptr<MaiTool> makeMaiStatTool() {
    return std::make_unique<StatTool>();
}

void registerMaiFileTools(MaiToolRegistry& registry) {
    registry.add(makeMaiReadTool());
    registry.add(makeMaiMoveFileTool());
    registry.add(makeMaiCopyFileTool());
    registry.add(makeMaiDeleteDirectoryTool());
    registry.add(makeMaiStatTool());
    registry.add(makeMaiCreateFileTool());
    registry.add(makeMaiCreateDirectoryTool());
    registry.add(makeMaiDeleteFileTool());
    registry.add(makeMaiWriteTool());
    registry.add(makeMaiEditTool());
    registry.add(makeMaiApplyPatchTool());
    registry.add(makeMaiGlobTool());
    registry.add(makeMaiGrepTool());
}
