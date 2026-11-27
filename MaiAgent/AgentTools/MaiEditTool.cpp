// 文件作用：执行 Agent 提出的文本编辑并保留失败信息。
// 直接依赖：MaiFilePath.h、MaiFileSystem.h；本文件在这些接口之间完成调用与结果转换。
#include "MaiEditTool.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

// file_edit：把文件里的一段原文换成另一段。
//
// ── 为什么不能只有 file_write ───────────────────────────────────
//
// file_write 是整份覆写。要改一行，模型得把整个文件重新吐一遍：
//
//   慢     一个 800 行的文件要吐 800 行，而真正变的只有一行
//   贵     那 800 行按输出计费，每改一次付一次
//   危险   吐的过程中任何一处走样都会**悄悄**改掉别的地方——
//          模型不是复制粘贴，它是重新生成，中间漏一行没有任何报错
//
// 所以要有一个「只说改动」的工具。这是 codex 的 apply_patch 和 opencode 的 edit
// 解决的同一个问题。
//
// ── 为什么要求 old_string 唯一 ──────────────────────────────────
//
// 不唯一就没法知道改的是哪一处。默认只替换一处、并且要求全文只出现一次，
// 匹配到多处就失败并告诉模型「多带几行上下文再来」。
// 真要全改的，显式传 replace_all。

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
// 改完回给模型看的那一小段上下文。给全文没意义（它刚给过），一点不给又看不出改对没有。
constexpr std::size_t kContextBytes = 160;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle,
                             const MaiToolContext& context) {
    if (needle.empty()) return 0;
    std::size_t total = 0;
    std::size_t at = haystack.find(needle);
    while (at != std::string::npos) {
        ++total;
        if (total % 1024 == 0 && context.isCanceled()) return 0;
        at = haystack.find(needle, at + needle.size());
    }
    return total;
}

// 与既有 file_read 的 UTF-8 标量约定一致；编辑不猜旧编码或转码，否则会改写未授权字节。
bool isUtf8Text(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first == 0) return false;
        if (first < 0x80) {
            ++index;
            continue;
        }
        const std::size_t width = first >= 0xC2 && first <= 0xDF   ? 2
                                  : first >= 0xE0 && first <= 0xEF ? 3
                                  : first >= 0xF0 && first <= 0xF4 ? 4
                                                                   : 0;
        if (width == 0 || index + width > text.size()) return false;
        const auto second = static_cast<unsigned char>(text[index + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second > 0x9F) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second > 0x8F))
            return false;
        for (std::size_t offset = 2; offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if (next < 0x80 || next > 0xBF) return false;
        }
        index += width;
    }
    return true;
}

// 只展示首处改动附近至多 480 字节，不能把模型给的大段 new_string 原样放回上下文。
// 输入已经验证为 UTF-8，起止向内收缩到标量边界；省略号明确表示显示的只是片段。
std::string excerptAround(const std::string& text, std::size_t at, std::size_t length) {
    std::size_t begin = at > kContextBytes ? at - kContextBytes : 0;
    std::size_t end = std::min(text.size(), at + std::min(length, kContextBytes) + kContextBytes);
    const auto continuation = [&](std::size_t index) {
        return (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80;
    };
    while (begin < end && continuation(begin)) ++begin;
    while (end < text.size() && end > begin && continuation(end)) --end;
    std::string piece = text.substr(begin, end - begin);
    if (begin > 0) piece.insert(0, "...");
    if (end < text.size()) piece += "...";
    return piece;
}

class EditTool final : public MaiTool {
public:
    std::string name() const override {
        return "file_edit";
    }

    std::string description() const override {
        // 精确替换旧文本的一段；匹配不唯一时应先核对上下文，避免改错位置。
        return "Replace an exact piece of text in a file. Prefer this over file_write when you are "
               "changing part of an existing file: it only sends the difference, so it is faster "
               "and cannot silently rewrite the parts you did not mean to touch. old_string must "
               "match the file exactly, including indentation, and must appear only once unless "
               "replace_all is set. Files and replacements must be UTF-8 text without NUL bytes; "
               "other encodings are rejected without conversion. Both the original and edited file "
               "must fit within 8 MiB. Success includes only a bounded excerpt.";
    }

    std::string parametersSchema() const override {
        // path 指目标文件；old_string 必须是文件中的精确原文，new_string 是替换内容。
        // 执行层还会校验匹配次数，避免把一个含糊片段改到错误位置。
        return R"({"type":"object","properties":{)"
               R"("path":{"type":"string","description":"Absolute path or path relative to the wor)"
               R"(king directory"},)"
               R"("old_string":{"type":"string","description":"The exact text to replace,)"
               R"( copied from the file"},)"
               R"("new_string":{"type":"string","description":"What to put there instead"},)"
               R"("replace_all":{"type":"boolean","description":"Replace every occurrence instead )"
               R"(of requiring exactly one, default false"}},)"
               R"("required":["path","old_string","new_string"],"additionalProperties":false})";
    }

    // 改文件，一律要问。危险程度不随参数变，所以不用看参数。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    std::vector<std::string> approvalKeys(const std::string& argumentsJson,
                                          const MaiToolContext& context) const override {
        const json args = parseArguments(argumentsJson);
        if (!args.contains("path") || !args["path"].is_string())
            return MaiTool::approvalKeys(argumentsJson, context);
        const auto& requested = args["path"].get_ref<const std::string&>();
        if (requested.find('\0') != std::string::npos)
            return MaiTool::approvalKeys(argumentsJson, context);
        const std::string path = context.resolvePath(requested);
        return path.empty() ? MaiTool::approvalKeys(argumentsJson, context)
                            : std::vector<std::string>{"file:" + path};
    }

    // 解析模型提供的参数并编辑已授权的本地文件。
    // 成功和失败都转成明确的工具结果；中间状态不能冒充最终产物。
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "file edit canceled before reading");
        const json args = parseArguments(raw);
        for (auto field = args.begin(); field != args.end(); ++field)
            if (field.key() != "path" && field.key() != "old_string" &&
                field.key() != "new_string" && field.key() != "replace_all")
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "unexpected file_edit parameter: " + field.key());
        if (args.contains("replace_all") && !args["replace_all"].is_boolean())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "replace_all must be a boolean");
        const std::string requested = args.contains("path") && args["path"].is_string()
                                          ? args["path"].get<std::string>()
                                          : std::string{};
        if (requested.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: path");
        }
        if (requested.find('\0') != std::string::npos)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "path must not contain NUL");
        if (context.root.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so file tools are unavailable");
        }
        const std::string resolved = context.resolvePath(requested);
        if (resolved.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "path is outside the area accessible to this host: " + requested);
        }
        if (!args.contains("old_string") || !args["old_string"].is_string() ||
            !args.contains("new_string") || !args["new_string"].is_string()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string and new_string are both required and must be strings");
        }

        const auto& oldText = args["old_string"].get_ref<const std::string&>();
        const auto& newText = args["new_string"].get_ref<const std::string&>();
        const bool replaceAll = args.value("replace_all", false);

        if (oldText.size() > kMaxFileBytes || newText.size() > kMaxFileBytes ||
            !isUtf8Text(oldText) || !isUtf8Text(newText))
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string and new_string must be NUL-free UTF-8 text of at most 8 MiB each");

        if (oldText.empty()) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string is empty. Use file_write to replace the file contents.");
        }
        if (oldText == newText) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "old_string and new_string are identical, so there is "
                                          "nothing to change");
        }

        const MaiFilePath path = MaiFilePath::fromUtf8(resolved);
        if (!MaiFileSystem::exists(path)) {
            return MaiToolResult::failure(MaiErrorCode::NotFound,
                                          "file does not exist: " + requested);
        }
        if (MaiFileSystem::isDirectory(path)) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "that is a directory, not a file: " + requested);
        }

        std::string contents;
        bool readTruncated = false;
        const MaiError readError =
            MaiFileSystem::readFile(path, contents, kMaxFileBytes, &readTruncated);
        if (readError.hasError()) {
            return MaiToolResult::failure(readError.code(), readError.message());
        }
        if (readTruncated) {
            // 截断过的内容写回去会**把文件尾巴砍掉**。宁可不改。
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "this file is too large to edit safely; it would be truncated");
        }
        if (!isUtf8Text(contents))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "file_edit requires NUL-free UTF-8 text; binary and "
                                          "other encodings are not converted");

        const std::size_t matches = countOccurrences(contents, oldText, context);
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "file edit canceled before replacement");
        if (matches == 0) {
            // 这句话要写成模型能据此改正的样子。最常见的原因是缩进抄错，或者
            // 它凭记忆写而不是从 read 的结果里抄。
            return MaiToolResult::failure(
                MaiErrorCode::NotFound,
                "old_string was not found in " + requested +
                    ". Copy it exactly from the file, including indentation and line breaks. "
                    "Read the file again if you are not sure.");
        }
        if (matches > 1 && !replaceAll) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "old_string appears " + std::to_string(matches) + " times in " + requested +
                    ". Include a few more surrounding lines so it matches exactly one place, "
                    "or set replace_all to change all of them.");
        }

        const std::size_t changed = replaceAll ? matches : 1;
        std::size_t updatedBytes = contents.size();
        if (newText.size() > oldText.size()) {
            const auto growth = newText.size() - oldText.size();
            // 先除再乘：同时阻止 size_t 溢出和 replace_all 产生的巨大中间分配。
            if (changed > (kMaxFileBytes - contents.size()) / growth)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "edited file would exceed the 8 MiB byte limit");
            updatedBytes += changed * growth;
        } else {
            updatedBytes -= changed * (oldText.size() - newText.size());
        }
        const std::size_t firstAt = contents.find(oldText);
        std::string updated;
        if (replaceAll) {
            updated.reserve(updatedBytes);
            std::size_t cursor = 0;
            std::size_t replaced = 0;
            while (true) {
                if (replaced++ % 1024 == 0 && context.isCanceled())
                    return MaiToolResult::failure(MaiErrorCode::Canceled,
                                                  "file edit canceled during replacement");
                const std::size_t at = contents.find(oldText, cursor);
                if (at == std::string::npos) {
                    updated.append(contents, cursor, std::string::npos);
                    break;
                }
                updated.append(contents, cursor, at - cursor);
                updated += newText;
                cursor = at + oldText.size();
            }
        } else {
            updated = contents;
            updated.replace(firstAt, oldText.size(), newText);
        }

        // 取消是协作式的；在唯一写入动作前再核对，之前的读取和计算均不修改原文件。
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled,
                                          "file edit canceled before writing");
        const MaiError writeError = MaiFileSystem::writeFile(path, updated);
        if (writeError.hasError()) {
            return MaiToolResult::failure(writeError.code(), writeError.message());
        }

        std::string report = "Replaced " + std::to_string(changed) +
                             (changed == 1 ? " occurrence in " : " occurrences in ") + requested +
                             ".\n\n" + excerptAround(updated, firstAt, newText.size());
        return MaiToolResult::success(std::move(report));
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiEditTool() {
    return std::make_unique<EditTool>();
}
