// 文件作用：实现只读搜索、目录快照、可验证分页及无回溯的有界正则匹配。
// 设计参考 Chromium FileEnumerator 的无序枚举契约、Codex file-search 的路径排序与忽略规则，
// 以及 OpenClaw read-page 的“真实续读位置与展示截断分别表达”约定。
#include "MaiFileSearch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "MaiFileSystem.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxSessions = 8;
constexpr std::size_t kMaxCacheBytes = 64u * 1024 * 1024;
constexpr std::size_t kMaxPatternBytes = 1024;
constexpr std::size_t kMaxRegexStates = 2048;

unsigned char fold(unsigned char character, bool caseSensitive) {
    return !caseSensitive && character >= 'A' && character <= 'Z'
               ? static_cast<unsigned char>(character + ('a' - 'A'))
               : character;
}

std::string folded(std::string text, bool caseSensitive) {
    for (char& character : text)
        character = static_cast<char>(fold(static_cast<unsigned char>(character), caseSensitive));
    return text;
}

std::string baseName(const std::string& path) {
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string parentName(const std::string& path) {
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

bool isInside(const MaiFilePath& root, const MaiFilePath& path) {
    return !path.isEmpty() && (root == path || root.isParentOf(path));
}

void addIssue(MaiFileSearchPage& page, const std::string& reason, const std::string& path = {}) {
    page.partial = true;
    for (auto& issue : page.issues) {
        if (issue.reason == reason && issue.path == path) {
            ++issue.count;
            return;
        }
    }
    // 错误清单本身也必须有界；保留前面的定位信息，再按原因合并尾部。
    if (page.issues.size() < 100) {
        page.issues.push_back({reason, path, 1});
    } else {
        for (auto& issue : page.issues) {
            if (issue.reason == reason && issue.path.empty()) {
                ++issue.count;
                return;
            }
        }
        if (page.issues.size() < 120) page.issues.push_back({reason, {}, 1});
    }
}

bool isCanceled(const MaiFileSearchCallbacks& callbacks) {
    return callbacks.isCanceled && callbacks.isCanceled();
}

bool expired(const Clock::time_point& start, const MaiFileSearchBudget& budget) {
    return Clock::now() - start >= std::chrono::milliseconds(budget.maxMilliseconds);
}

// Glob 用逐字符动态规划，避免 * 的递归回溯。状态数受输入长度限制。
// **/ 的零层目录转移单独处理，否则根目录下的文件会被 **/* 漏掉。
bool globMatchOne(const std::string& rawPattern, const std::string& rawPath, bool caseSensitive) {
    const auto pattern = folded(rawPattern, caseSensitive);
    const auto path = folded(rawPath, caseSensitive);
    std::vector<unsigned char> current(path.size() + 1, 0), next(path.size() + 1, 0);
    current[0] = 1;
    for (std::size_t index = 0; index < pattern.size();) {
        std::fill(next.begin(), next.end(), static_cast<unsigned char>(0));
        const char token = pattern[index++];
        if (token == '*') {
            const bool recursive = index < pattern.size() && pattern[index] == '*';
            if (recursive) ++index;
            const bool directoryPrefix =
                recursive && index < pattern.size() && pattern[index] == '/';
            if (directoryPrefix) ++index;
            for (std::size_t at = 0; at <= path.size(); ++at) {
                if (current[at]) next[at] = 1;
                if (at && (recursive || path[at - 1] != '/') && next[at - 1]) next[at] = 1;
            }
            if (directoryPrefix) {
                // **/ 必须停在目录边界；零层目录继承原状态。
                for (std::size_t at = 0; at <= path.size(); ++at)
                    next[at] = current[at] || (at && path[at - 1] == '/' && next[at]);
            }
        } else if (token == '[') {
            std::array<bool, 256> accepted{};
            bool negate =
                index < pattern.size() && (pattern[index] == '!' || pattern[index] == '^');
            if (negate) ++index;
            const auto start = index;
            while (index < pattern.size() && pattern[index] != ']') {
                const unsigned char first = static_cast<unsigned char>(pattern[index++]);
                if (index + 1 < pattern.size() && pattern[index] == '-' &&
                    pattern[index + 1] != ']') {
                    ++index;
                    const unsigned char last = static_cast<unsigned char>(pattern[index++]);
                    for (unsigned value = first; value <= last; ++value) accepted[value] = true;
                } else {
                    accepted[first] = true;
                }
            }
            if (index == pattern.size() || start == index) return false;
            ++index;
            for (std::size_t at = 1; at <= path.size(); ++at)
                next[at] = current[at - 1] && path[at - 1] != '/' &&
                           (accepted[static_cast<unsigned char>(path[at - 1])] != negate);
        } else {
            for (std::size_t at = 1; at <= path.size(); ++at)
                next[at] =
                    current[at - 1] && (token == '?' ? path[at - 1] != '/' : token == path[at - 1]);
        }
        current.swap(next);
    }
    return current[path.size()] != 0;
}

bool expandGlobs(const std::string& pattern, std::vector<std::string>& output) {
    if (output.size() >= 64 || pattern.size() > kMaxPatternBytes * 2) return false;
    bool inClass = false;
    for (std::size_t open = 0; open < pattern.size(); ++open) {
        if (pattern[open] == '[')
            inClass = true;
        else if (pattern[open] == ']')
            inClass = false;
        if (inClass) continue;
        if (pattern[open] == '}') return false;
        if (pattern[open] != '{') continue;
        std::size_t depth = 1, close = open + 1, part = close;
        std::vector<std::string> alternatives;
        for (; close < pattern.size() && depth; ++close) {
            if (pattern[close] == '[')
                inClass = true;
            else if (pattern[close] == ']')
                inClass = false;
            if (inClass) continue;
            if (pattern[close] == '{')
                ++depth;
            else if (pattern[close] == '}') {
                if (!--depth) {
                    if (part == close) return false;
                    alternatives.push_back(pattern.substr(part, close - part));
                }
            } else if (pattern[close] == ',' && depth == 1) {
                if (part == close) return false;
                alternatives.push_back(pattern.substr(part, close - part));
                part = close + 1;
            }
        }
        if (depth || alternatives.empty()) return false;
        for (const auto& alternative : alternatives)
            if (!expandGlobs(pattern.substr(0, open) + alternative + pattern.substr(close), output))
                return false;
        return true;
    }
    output.push_back(pattern);
    return true;
}

bool globMatch(const std::string& pattern, const std::string& path, bool caseSensitive) {
    if (pattern.find('{') == std::string::npos) return globMatchOne(pattern, path, caseSensitive);
    std::vector<std::string> alternatives;
    if (!expandGlobs(pattern, alternatives)) return false;
    for (const auto& alternative : alternatives)
        if (globMatchOne(alternative, path, caseSensitive)) return true;
    return false;
}

bool pathGlob(const std::string& pattern, const std::string& path, bool caseSensitive) {
    return globMatch(pattern, pattern.find('/') == std::string::npos ? baseName(path) : path,
                     caseSensitive);
}

bool defaultExcluded(const std::string& name) {
    static const std::array<const char*, 11> names = {
        ".git", ".hg",   ".svn", "node_modules", "__pycache__", ".venv",
        "venv", "build", "dist", "target",       ".cache"};
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool sameInfo(const MaiRegularFileInfo& first, const MaiRegularFileInfo& second) {
    return first.bytes == second.bytes && first.modified == second.modified &&
           first.identity == second.identity;
}

struct IgnoreRule {
    std::string base;
    std::string pattern;
    bool negated = false;
    bool directory = false;
    bool anchored = false;
};

struct IndexedFile {
    MaiFilePath path;
    std::string relative;
    MaiRegularFileInfo info;
};

struct TreeSnapshot {
    MaiFilePath root;
    bool singleFile = false;
    std::string corpusKey;
    Clock::time_point captured;
    std::vector<IndexedFile> files;
    MaiFileSearchPage observations;
    std::size_t bytes = 0;
};

void boundIssues(MaiFileSearchPage& page, const MaiFileSearchBudget& budget);

bool ignoredBy(const IgnoreRule& rule, const std::string& path, bool directory) {
    std::string relative = path;
    if (!rule.base.empty()) {
        if (path.size() <= rule.base.size() || path.compare(0, rule.base.size(), rule.base) != 0 ||
            path[rule.base.size()] != '/')
            return false;
        relative = path.substr(rule.base.size() + 1);
    }
    if (rule.directory && !directory) return false;
    if (rule.anchored || rule.pattern.find('/') != std::string::npos)
        return globMatchOne(rule.pattern, relative, true);
    return globMatchOne(rule.pattern, baseName(relative), true);
}

TreeSnapshot collectTree(const MaiFilePath& root, const MaiFileSearchFilter& filter,
                         const MaiFileSearchBudget& budget, const MaiFileSearchCallbacks& callbacks,
                         const Clock::time_point& start) {
    TreeSnapshot tree;
    tree.root = root;
    std::map<std::string, std::vector<IgnoreRule>> ignoreFiles;
    const std::string rootText = root.toGenericUtf8();
    auto loadIgnore = [&](const std::string& directory) {
        if (!filter.respectGitIgnore || ignoreFiles.count(directory)) return;
        std::vector<IgnoreRule> rules;
        const auto directoryPath =
            directory.empty() ? root : root.append(MaiFilePath::fromUtf8(directory));
        const auto path = directoryPath.append(MaiFilePath::fromUtf8(".gitignore"));
        MaiRegularFileInfo info;
        if (!MaiFileSystem::regularFileInfo(path, info)) {
            ignoreFiles.emplace(directory, std::move(rules));
            return;
        }
        const auto resolved = MaiFileSystem::resolve(path);
        if (!isInside(root, resolved) ||
            (callbacks.pathAllowed && !callbacks.pathAllowed(resolved))) {
            addIssue(tree.observations, "ignore_file_denied", directory);
            ignoreFiles.emplace(directory, std::move(rules));
            return;
        }
        std::string contents;
        bool truncated = false;
        const auto error = MaiFileSystem::readFile(path, contents, 64u * 1024, &truncated);
        MaiRegularFileInfo after;
        const bool stable = MaiFileSystem::regularFileInfo(path, after) && sameInfo(info, after) &&
                            isInside(root, MaiFileSystem::resolve(path));
        if (error || truncated || !stable) {
            addIssue(tree.observations, "ignore_file_unreadable_or_too_large", directory);
        } else {
            std::istringstream lines(contents);
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                while (!line.empty() && line.back() == ' ' &&
                       (line.size() < 2 || line[line.size() - 2] != '\\'))
                    line.pop_back();
                if (line.empty() || line.front() == '#') continue;
                IgnoreRule rule;
                rule.base = directory;
                if (line.front() == '!') {
                    rule.negated = true;
                    line.erase(0, 1);
                } else if (line.size() > 1 && line.front() == '\\' &&
                           (line[1] == '!' || line[1] == '#')) {
                    line.erase(0, 1);
                }
                if (line.empty()) continue;
                if (line.front() == '/') {
                    rule.anchored = true;
                    line.erase(0, 1);
                }
                if (!line.empty() && line.back() == '/') {
                    rule.directory = true;
                    line.pop_back();
                }
                if (line.size() > kMaxPatternBytes) {
                    addIssue(tree.observations, "ignore_pattern_too_large", directory);
                    continue;
                }
                rule.pattern = std::move(line);
                rules.push_back(std::move(rule));
            }
        }
        ignoreFiles.emplace(directory, std::move(rules));
    };
    loadIgnore({});
    MaiFileSystem::walk(
        root,
        [&](const MaiFileEntry& entry) {
            if (isCanceled(callbacks)) {
                tree.observations.canceled = true;
                addIssue(tree.observations, "canceled");
                return MaiWalkAction::Stop;
            }
            if (tree.observations.visitedEntries >= budget.maxEntries || expired(start, budget)) {
                addIssue(tree.observations, tree.observations.visitedEntries >= budget.maxEntries
                                                ? "entry_budget"
                                                : "time_budget");
                return MaiWalkAction::Stop;
            }
            ++tree.observations.visitedEntries;
            const auto resolved = MaiFileSystem::resolve(entry.path);
            const auto relative = entry.path.toGenericUtf8().substr(
                rootText.size() + (rootText.empty() || rootText.back() != '/' ? 1 : 0));
            if (MaiFileSystem::isSymbolicLink(entry.path) || !isInside(root, resolved) ||
                (callbacks.pathAllowed && !callbacks.pathAllowed(resolved))) {
                addIssue(tree.observations, "path_denied_or_symlink", relative);
                return entry.isDirectory ? MaiWalkAction::SkipDirectory : MaiWalkAction::Continue;
            }
            MaiRegularFileInfo info;
            if (!entry.isDirectory) {
                if (!MaiFileSystem::regularFileInfo(entry.path, info)) {
                    addIssue(tree.observations, "not_regular_or_unreadable", relative);
                    return MaiWalkAction::Continue;
                }
            }
            if ((!filter.includeHidden && !entry.nameUtf8.empty() &&
                 entry.nameUtf8.front() == '.') ||
                (filter.useDefaultExcludes && entry.isDirectory && defaultExcluded(entry.nameUtf8)))
                return entry.isDirectory ? MaiWalkAction::SkipDirectory : MaiWalkAction::Continue;
            std::vector<std::string> ancestors{std::string{}};
            const auto parent = parentName(relative);
            for (std::size_t separator = 0; separator < parent.size(); ++separator)
                if (parent[separator] == '/') ancestors.push_back(parent.substr(0, separator));
            if (!parent.empty()) ancestors.push_back(parent);
            bool ignored = false;
            for (const auto& ancestor : ancestors) {
                loadIgnore(ancestor);
                const auto found = ignoreFiles.find(ancestor);
                if (found == ignoreFiles.end()) continue;
                for (const auto& rule : found->second)
                    if (ignoredBy(rule, relative, entry.isDirectory)) ignored = !rule.negated;
            }
            for (const auto& pattern : filter.excludeGlobs)
                if (pathGlob(pattern, relative, true)) ignored = true;
            if (ignored)
                return entry.isDirectory ? MaiWalkAction::SkipDirectory : MaiWalkAction::Continue;
            if (entry.isDirectory) return MaiWalkAction::Continue;
            if (!filter.includeGlobs.empty() &&
                std::none_of(
                    filter.includeGlobs.begin(), filter.includeGlobs.end(),
                    [&](const std::string& pattern) { return pathGlob(pattern, relative, true); }))
                return MaiWalkAction::Continue;
            const auto bytes = relative.size() +
                               entry.path.value().size() * sizeof(MaiFilePath::CharType) +
                               info.identity.size() + 256;
            if (tree.bytes + bytes > 48u * 1024 * 1024) {
                addIssue(tree.observations, "index_capacity");
                return MaiWalkAction::Stop;
            }
            tree.bytes += bytes;
            tree.files.push_back({entry.path, relative, info});
            return MaiWalkAction::Continue;
        },
        [&](const MaiFilePath& path, const MaiError&) {
            const auto display = path.toGenericUtf8();
            addIssue(
                tree.observations, "enumeration_failed",
                display.size() > rootText.size() ? display.substr(rootText.size()) : std::string{});
        });
    std::sort(tree.files.begin(), tree.files.end(),
              [](const IndexedFile& first, const IndexedFile& second) {
                  return first.relative < second.relative;
              });
    boundIssues(tree.observations, budget);
    tree.captured = Clock::now();
    return tree;
}

TreeSnapshot singleFileTree(const MaiFilePath& path, const MaiRegularFileInfo& info,
                            const MaiFileSearchFilter& filter) {
    TreeSnapshot tree;
    tree.root = path;
    tree.singleFile = true;
    tree.captured = Clock::now();
    const auto relative = path.baseName().toUtf8();
    bool include =
        filter.includeGlobs.empty() ||
        std::any_of(filter.includeGlobs.begin(), filter.includeGlobs.end(),
                    [&](const std::string& pattern) { return pathGlob(pattern, relative, true); });
    for (const auto& pattern : filter.excludeGlobs)
        if (pathGlob(pattern, relative, true)) include = false;
    if (include) tree.files.push_back({path, relative, info});
    tree.bytes = relative.size() + path.value().size() * sizeof(MaiFilePath::CharType) +
                 info.identity.size() + 512;
    return tree;
}

void appendKey(std::string& output, const std::string& value) {
    output += std::to_string(value.size()) + ":" + value;
}

std::string commonKey(const MaiFilePath& root, const MaiFileSearchFilter& filter,
                      const MaiFileSearchBudget& budget) {
    std::string key;
    appendKey(key, root.toGenericUtf8());
    appendKey(key, std::to_string(filter.includeHidden) +
                       std::to_string(filter.useDefaultExcludes) +
                       std::to_string(filter.respectGitIgnore));
    for (const auto& pattern : filter.includeGlobs) appendKey(key, "i" + pattern);
    for (const auto& pattern : filter.excludeGlobs) appendKey(key, "e" + pattern);
    appendKey(key, std::to_string(budget.maxEntries));
    appendKey(key, std::to_string(budget.maxFiles));
    appendKey(key, std::to_string(budget.maxBytes));
    appendKey(key, std::to_string(budget.maxFileBytes));
    appendKey(key, std::to_string(budget.maxOutputBytes));
    appendKey(key, std::to_string(budget.maxRegexSteps));
    appendKey(key, std::to_string(budget.maxMilliseconds));
    return key;
}

bool validOptions(const MaiFileSearchFilter& filter, const MaiFileSearchBudget& budget,
                  std::size_t pageSize, MaiFileSearchPage& page) {
    if (!pageSize || pageSize > 10000 || !budget.maxEntries || budget.maxEntries > 200000 ||
        !budget.maxFiles || !budget.maxBytes || !budget.maxFileBytes ||
        budget.maxFileBytes > 16u * 1024 * 1024 || budget.maxOutputBytes < 1024 ||
        budget.maxOutputBytes > 4u * 1024 * 1024 || !budget.maxMilliseconds ||
        budget.maxMilliseconds > 60000 || !budget.maxRegexSteps) {
        page.error = {MaiErrorCode::InvalidInput, "Invalid search page size or budget"};
        return false;
    }
    if (filter.includeGlobs.size() + filter.excludeGlobs.size() > 100) {
        page.error = {MaiErrorCode::InvalidInput, "Too many include/exclude globs"};
        return false;
    }
    for (const auto* patterns : {&filter.includeGlobs, &filter.excludeGlobs})
        for (const auto& pattern : *patterns) {
            if (pattern.size() > kMaxPatternBytes) {
                page.error = {MaiErrorCode::InvalidInput, "Search glob exceeds 1024 bytes"};
                return false;
            }
            std::vector<std::string> alternatives;
            if (!expandGlobs(pattern, alternatives)) {
                page.error = {MaiErrorCode::InvalidInput,
                              "Glob has empty/unbalanced braces or exceeds 64 expansions"};
                return false;
            }
        }
    return true;
}

bool validUtf8(const std::string& text);

std::u32string unicodeScalars(const std::string& text, bool caseSensitive) {
    std::u32string result;
    if (!validUtf8(text)) return result;
    result.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x80) {
            result.push_back(fold(first, caseSensitive));
            continue;
        }
        const unsigned width = first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
        char32_t scalar = first & (width == 2 ? 0x1F : width == 3 ? 0x0F : 0x07);
        for (unsigned index = 1; index < width; ++index)
            scalar = (scalar << 6) | (static_cast<unsigned char>(text[at++]) & 0x3F);
        result.push_back(scalar);
    }
    return result;
}

std::int64_t fuzzyScore(const std::u32string& query, const std::string& rawPath,
                        bool caseSensitive) {
    const auto path = unicodeScalars(rawPath, caseSensitive);
    if (!rawPath.empty() && path.empty()) return -1;
    const auto separator = path.find_last_of(U'/');
    const auto name = separator == std::u32string::npos ? path : path.substr(separator + 1);
    if (query.empty()) return 0;
    const bool pathQuery = query.find(U'/') != std::u32string::npos;
    const auto& primary = pathQuery ? path : name;
    if (primary == query) return 1000000;
    if (primary.compare(0, query.size(), query) == 0)
        return 800000 - static_cast<std::int64_t>(primary.size());
    const auto substring = primary.find(query);
    if (substring != std::u32string::npos)
        return 600000 - static_cast<std::int64_t>(substring * 100 + primary.size());
    auto subsequence = [&](const std::u32string& value) {
        std::size_t position = 0;
        std::int64_t score = 0;
        for (char32_t character : query) {
            const auto found = value.find(character, position);
            if (found == std::u32string::npos) return std::int64_t{-1};
            score += 100 - static_cast<std::int64_t>(found - position);
            if (!found || value[found - 1] == '/' || value[found - 1] == '_' ||
                value[found - 1] == '-')
                score += 50;
            position = found + 1;
        }
        return score - static_cast<std::int64_t>(value.size());
    };
    const auto primaryScore = subsequence(primary);
    if (primaryScore >= 0) return 400000 + primaryScore;
    if (!pathQuery) {
        const auto pathScore = subsequence(path);
        if (pathScore >= 0) return 100000 + pathScore;
    }
    return -1;
}

bool validUtf8(const std::string& text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto first = static_cast<unsigned char>(text[at]);
        if (first < 0x80) {
            ++at;
            continue;
        }
        const std::size_t width = first >= 0xC2 && first <= 0xDF   ? 2
                                  : first >= 0xE0 && first <= 0xEF ? 3
                                  : first >= 0xF0 && first <= 0xF4 ? 4
                                                                   : 0;
        if (!width || at + width > text.size()) return false;
        const auto second = static_cast<unsigned char>(text[at + 1]);
        if ((second & 0xC0) != 0x80 || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second >= 0xA0) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second >= 0x90))
            return false;
        for (std::size_t tail = 2; tail < width; ++tail)
            if ((static_cast<unsigned char>(text[at + tail]) & 0xC0) != 0x80) return false;
        at += width;
    }
    return true;
}

bool binaryText(const std::string& text) {
    std::size_t controls = 0;
    for (unsigned char character : text) {
        if (!character) return true;
        if (character < 9 || (character > 13 && character < 32)) ++controls;
    }
    return !text.empty() && controls > text.size() / 100;
}

MaiFileSearchLine displayLine(const std::string& text, std::size_t start, std::size_t length,
                              std::size_t line, std::size_t focus, std::size_t maximum) {
    MaiFileSearchLine output;
    output.line = line;
    std::size_t offset = length > maximum && focus > maximum / 3 ? focus - maximum / 3 : 0;
    if (offset + maximum > length) offset = length > maximum ? length - maximum : 0;
    while (offset && (static_cast<unsigned char>(text[start + offset]) & 0xC0) == 0x80) --offset;
    std::size_t end = std::min(length, offset + maximum);
    while (end < length && end > offset &&
           (static_cast<unsigned char>(text[start + end]) & 0xC0) == 0x80)
        --end;
    output.text = text.substr(start + offset, end - offset);
    output.displayOffsetBytes = offset;
    output.truncated = offset || end < length;
    return output;
}

// Thompson NFA：每个字节最多处理每个状态一次，(a+)+$ 等表达式不会指数回溯。
// 工作计数同时限制“巨大状态数 × 巨大行”；超限会明确跳过该行，而非断言无命中。
class SafeRegex {
public:
    bool compile(const std::string& pattern, bool caseSensitive);
    bool search(const std::string& text, std::size_t begin, std::size_t length,
                std::uint64_t& steps, std::uint64_t maximum, std::size_t& matchStart,
                const std::function<bool()>& stop, bool& interrupted) const;
    const std::string& error() const;

private:
    enum class Kind { Bytes, Any, Split, Begin, End, Boundary, NotBoundary, Match };
    struct State {
        Kind kind = Kind::Split;
        std::array<bool, 256> bytes{};
        int next = -1;
        int alternate = -1;
    };
    struct Fragment {
        int start = -1;
        std::vector<std::pair<int, bool>> exits;
    };
    int add(State state);
    void patch(const std::vector<std::pair<int, bool>>& exits, int destination);
    Fragment expression();
    Fragment sequence();
    Fragment repeated();
    Fragment atom();
    bool escaped(std::array<bool, 256>& bytes, bool inClass);
    std::vector<State> mStates;
    std::string mPattern;
    std::string mError;
    std::size_t mPosition = 0;
    int mStart = -1;
    bool mCaseSensitive = true;
};

int SafeRegex::add(State state) {
    if (mStates.size() >= kMaxRegexStates) {
        mError = "Regex exceeds 2048 states";
        return -1;
    }
    mStates.push_back(std::move(state));
    return static_cast<int>(mStates.size() - 1);
}

void SafeRegex::patch(const std::vector<std::pair<int, bool>>& exits, int destination) {
    for (const auto& exit : exits)
        if (exit.first >= 0)
            (exit.second ? mStates[exit.first].alternate : mStates[exit.first].next) = destination;
}

SafeRegex::Fragment SafeRegex::expression() {
    auto left = sequence();
    while (mPosition < mPattern.size() && mPattern[mPosition] == '|' && mError.empty()) {
        ++mPosition;
        auto right = sequence();
        State split;
        split.next = left.start;
        split.alternate = right.start;
        left.start = add(std::move(split));
        left.exits.insert(left.exits.end(), right.exits.begin(), right.exits.end());
    }
    return left;
}

SafeRegex::Fragment SafeRegex::sequence() {
    Fragment output;
    while (mPosition < mPattern.size() && mPattern[mPosition] != ')' &&
           mPattern[mPosition] != '|' && mError.empty()) {
        auto item = repeated();
        if (output.start < 0)
            output = std::move(item);
        else {
            patch(output.exits, item.start);
            output.exits = std::move(item.exits);
        }
    }
    if (output.start < 0) {
        output.start = add(State{});
        output.exits.push_back({output.start, false});
    }
    return output;
}

SafeRegex::Fragment SafeRegex::repeated() {
    const auto firstState = mStates.size();
    auto output = atom();
    if (mPosition >= mPattern.size() || !mError.empty()) return output;
    const char repeat = mPattern[mPosition];
    if (repeat == '{') {
        ++mPosition;
        auto number = [&]() {
            int value = 0;
            const auto start = mPosition;
            while (mPosition < mPattern.size() && mPattern[mPosition] >= '0' &&
                   mPattern[mPosition] <= '9') {
                value = value * 10 + mPattern[mPosition++] - '0';
                if (value > 128) {
                    mError = "Regex repetition exceeds 128";
                    return -1;
                }
            }
            if (start == mPosition) {
                mError = "Regex repetition requires a number";
                return -1;
            }
            return value;
        };
        const int minimum = number();
        int maximum = minimum;
        if (mPosition < mPattern.size() && mPattern[mPosition] == ',') {
            ++mPosition;
            maximum = mPosition < mPattern.size() && mPattern[mPosition] == '}' ? -1 : number();
        }
        if (!mError.empty()) return output;
        if (mPosition == mPattern.size() || mPattern[mPosition++] != '}' ||
            (maximum >= 0 && maximum < minimum)) {
            mError = "Invalid regex repetition";
            return output;
        }
        // 复制未接线的原子片段，再连接必选与可选分支；状态数上限覆盖嵌套展开。
        const auto original = output;
        const auto lastState = mStates.size();
        const auto originalStates = std::vector<State>(mStates.begin() + firstState, mStates.end());
        auto copy = [&]() {
            const auto offset = static_cast<int>(mStates.size() - firstState);
            for (auto state : originalStates) {
                for (int* destination : {&state.next, &state.alternate})
                    if (*destination >= static_cast<int>(firstState) &&
                        *destination < static_cast<int>(lastState))
                        *destination += offset;
                add(std::move(state));
            }
            auto fragment = original;
            fragment.start += offset;
            for (auto& exit : fragment.exits) exit.first += offset;
            return fragment;
        };
        Fragment combined;
        const int copies = maximum < 0 ? minimum + 1 : maximum;
        for (int index = 0; index < copies && mError.empty(); ++index) {
            auto item = index ? copy() : original;
            if (!mError.empty()) return output;
            if (index >= minimum) {
                State split;
                split.next = item.start;
                const int branch = add(std::move(split));
                if (maximum < 0) {
                    patch(item.exits, branch);
                    item.exits = {{branch, true}};
                } else
                    item.exits.push_back({branch, true});
                item.start = branch;
            }
            if (combined.start < 0)
                combined = std::move(item);
            else {
                patch(combined.exits, item.start);
                combined.exits = std::move(item.exits);
            }
        }
        if (combined.start < 0) {
            combined.start = add(State{});
            combined.exits = {{combined.start, false}};
        }
        if (mPosition < mPattern.size() &&
            (mPattern[mPosition] == '*' || mPattern[mPosition] == '+' ||
             mPattern[mPosition] == '?' || mPattern[mPosition] == '{'))
            mError = "Repeated or lazy quantifiers are not supported";
        return combined;
    }
    if (repeat != '*' && repeat != '+' && repeat != '?') return output;
    ++mPosition;
    State split;
    split.next = output.start;
    const int branch = add(std::move(split));
    if (repeat == '?') {
        output.exits.push_back({branch, true});
        output.start = branch;
    } else {
        patch(output.exits, branch);
        if (repeat == '*') output.start = branch;
        output.exits = {{branch, true}};
    }
    if (mPosition < mPattern.size() &&
        (mPattern[mPosition] == '*' || mPattern[mPosition] == '+' || mPattern[mPosition] == '?'))
        mError = "Repeated or lazy quantifiers are not supported";
    return output;
}

bool SafeRegex::escaped(std::array<bool, 256>& bytes, bool inClass) {
    if (mPosition >= mPattern.size()) {
        mError = "Regex ends with a backslash";
        return false;
    }
    const unsigned char character = static_cast<unsigned char>(mPattern[mPosition++]);
    if (character >= '0' && character <= '9') {
        mError = "Regex backreferences are not supported";
        return false;
    }
    const bool invert = character == 'D' || character == 'S' || character == 'W';
    const auto lower = fold(character, false);
    if (lower == 'd' || lower == 's' || lower == 'w') {
        for (unsigned value = 0; value < 256; ++value) {
            const bool digit = value >= '0' && value <= '9';
            const bool word = digit || (value >= 'A' && value <= 'Z') ||
                              (value >= 'a' && value <= 'z') || value == '_';
            const bool whitespace = value == ' ' || (value >= 9 && value <= 13);
            bytes[value] = ((lower == 'd' ? digit : lower == 'w' ? word : whitespace) != invert);
        }
        return true;
    }
    if (character == 'b' && inClass) {
        bytes[8] = true;
        return true;
    }
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')) {
        if (character == 'n')
            bytes['\n'] = true;
        else if (character == 'r')
            bytes['\r'] = true;
        else if (character == 't')
            bytes['\t'] = true;
        else {
            mError = "Unsupported regex escape";
            return false;
        }
    } else
        bytes[fold(character, mCaseSensitive)] = true;
    return true;
}

SafeRegex::Fragment SafeRegex::atom() {
    if (mPosition >= mPattern.size()) return {};
    const char token = mPattern[mPosition++];
    if (token == '(') {
        if (mPosition < mPattern.size() && mPattern[mPosition] == '?') {
            mError = "Regex lookaround and special groups are not supported";
            return {};
        }
        auto group = expression();
        if (mPosition == mPattern.size() || mPattern[mPosition++] != ')')
            mError = "Unclosed regex group";
        return group;
    }
    State state;
    state.kind = Kind::Bytes;
    if (token == '.')
        state.kind = Kind::Any;
    else if (token == '^')
        state.kind = Kind::Begin;
    else if (token == '$')
        state.kind = Kind::End;
    else if (token == '\\') {
        if (mPosition < mPattern.size() &&
            (mPattern[mPosition] == 'b' || mPattern[mPosition] == 'B'))
            state.kind = mPattern[mPosition++] == 'b' ? Kind::Boundary : Kind::NotBoundary;
        else
            escaped(state.bytes, false);
    } else if (token == '[') {
        const bool negate = mPosition < mPattern.size() && mPattern[mPosition] == '^';
        if (negate) ++mPosition;
        bool any = false;
        while (mPosition < mPattern.size() && (mPattern[mPosition] != ']' || !any) &&
               mError.empty()) {
            any = true;
            std::array<bool, 256> item{};
            unsigned char character = static_cast<unsigned char>(mPattern[mPosition++]);
            if (character == '\\')
                escaped(item, true);
            else
                item[fold(character, mCaseSensitive)] = true;
            if (mPosition + 1 < mPattern.size() && mPattern[mPosition] == '-' &&
                mPattern[mPosition + 1] != ']') {
                ++mPosition;
                unsigned char last = static_cast<unsigned char>(mPattern[mPosition++]);
                if (last == '\\') {
                    if (mPosition == mPattern.size()) {
                        mError = "Unclosed regex range";
                        break;
                    }
                    last = static_cast<unsigned char>(mPattern[mPosition++]);
                }
                const auto firstValue = fold(character, mCaseSensitive);
                const auto lastValue = fold(last, mCaseSensitive);
                if (character == '\\' || firstValue > lastValue) {
                    mError = "Unsupported regex character range";
                    break;
                }
                for (unsigned value = firstValue; value <= lastValue; ++value) item[value] = true;
            }
            for (unsigned value = 0; value < 256; ++value)
                state.bytes[value] = state.bytes[value] || item[value];
        }
        if (mPosition == mPattern.size() || mPattern[mPosition++] != ']')
            mError = "Unclosed regex character class";
        if (negate)
            for (auto&& value : state.bytes) value = !value;
    } else if (token == '{' || token == '}' || token == '*' || token == '+' || token == '?') {
        mError = "Unsupported regex quantifier; use *, + or ? on a preceding expression";
    } else
        state.bytes[fold(static_cast<unsigned char>(token), mCaseSensitive)] = true;
    const int index = add(std::move(state));
    return {index, {{index, false}}};
}

bool SafeRegex::compile(const std::string& pattern, bool caseSensitive) {
    mPattern = pattern;
    mCaseSensitive = caseSensitive;
    if (pattern.size() > kMaxPatternBytes) {
        mError = "Regex exceeds 1024 bytes";
        return false;
    }
    auto expressionResult = expression();
    if (mPosition != pattern.size() && mError.empty())
        mError = "Unexpected regex closing parenthesis";
    State end;
    end.kind = Kind::Match;
    const int endIndex = add(std::move(end));
    patch(expressionResult.exits, endIndex);
    mStart = expressionResult.start;
    return mError.empty();
}

const std::string& SafeRegex::error() const {
    return mError;
}

bool SafeRegex::search(const std::string& text, std::size_t begin, std::size_t length,
                       std::uint64_t& steps, std::uint64_t maximum, std::size_t& matchStart,
                       const std::function<bool()>& stop, bool& interrupted) const {
    const auto absent = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> active(mStates.size(), absent), next(mStates.size(), absent);
    std::vector<std::pair<int, std::size_t>> queue;
    auto close = [&](std::vector<std::size_t>& states, std::size_t position) {
        queue.clear();
        for (std::size_t index = 0; index < states.size(); ++index)
            if (states[index] != absent) queue.push_back({static_cast<int>(index), states[index]});
        while (!queue.empty()) {
            const auto item = queue.back();
            queue.pop_back();
            if (++steps > maximum || ((steps & 4095) == 0 && stop())) {
                interrupted = true;
                return;
            }
            const auto& state = mStates[item.first];
            auto reach = [&](int target) {
                if (target >= 0 && item.second < states[target]) {
                    states[target] = item.second;
                    queue.push_back({target, item.second});
                }
            };
            if (state.kind == Kind::Split) {
                reach(state.next);
                reach(state.alternate);
            } else if (state.kind == Kind::Begin && !position)
                reach(state.next);
            else if (state.kind == Kind::End && position == length)
                reach(state.next);
            else if (state.kind == Kind::Boundary || state.kind == Kind::NotBoundary) {
                auto word = [](unsigned char character) {
                    return (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_';
                };
                const bool previous =
                    position && word(static_cast<unsigned char>(text[begin + position - 1]));
                const bool following =
                    position < length && word(static_cast<unsigned char>(text[begin + position]));
                if ((previous != following) == (state.kind == Kind::Boundary)) reach(state.next);
            }
        }
    };
    for (std::size_t position = 0; position <= length; ++position) {
        // 活跃列表和闭包种子的扫描也属于工作，避免大量不活跃状态绕过预算。
        steps += mStates.size() * 3;
        if (steps > maximum || ((position & 4095) == 0 && stop())) {
            interrupted = true;
            return false;
        }
        active[mStart] = std::min(active[mStart], position);
        close(active, position);
        if (interrupted) return false;
        for (std::size_t index = 0; index < active.size(); ++index)
            if (active[index] != absent && mStates[index].kind == Kind::Match) {
                matchStart = active[index];
                return true;
            }
        if (position == length) break;
        std::fill(next.begin(), next.end(), absent);
        const auto character =
            fold(static_cast<unsigned char>(text[begin + position]), mCaseSensitive);
        for (std::size_t index = 0; index < active.size(); ++index) {
            if (active[index] == absent) continue;
            const auto& state = mStates[index];
            if (state.next >= 0 &&
                (state.kind == Kind::Any || (state.kind == Kind::Bytes && state.bytes[character])))
                next[state.next] = std::min(next[state.next], active[index]);
        }
        active.swap(next);
    }
    return false;
}

struct TextPosition {
    std::size_t start = 0;
    std::size_t length = 0;
};

std::vector<TextPosition> textLines(const std::string& text) {
    std::vector<TextPosition> lines;
    for (std::size_t start = 0; start < text.size();) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string::npos ? text.size() : newline;
        const auto length = end > start && text[end - 1] == '\r' ? end - start - 1 : end - start;
        lines.push_back({start, length});
        start = newline == std::string::npos ? text.size() : newline + 1;
    }
    return lines;
}

std::size_t pageBytes(const MaiFileSearchPage& page) {
    std::size_t bytes = 512;
    for (const auto& file : page.files) bytes += file.path.size() + 48;
    for (const auto& match : page.matches) {
        bytes += match.path.size() + match.text.size() + 160;
        for (const auto* context : {&match.before, &match.after})
            for (const auto& line : *context) bytes += line.text.size() + 64;
    }
    for (const auto& issue : page.issues) bytes += issue.path.size() + issue.reason.size() + 64;
    return bytes;
}

std::size_t metadataReserve(const MaiFileSearchBudget& budget) {
    return 512 + std::min<std::size_t>(4096, budget.maxOutputBytes / 4);
}

void boundIssues(MaiFileSearchPage& page, const MaiFileSearchBudget& budget) {
    const auto maximum = metadataReserve(budget) - 512;
    std::size_t bytes = 0;
    for (const auto& issue : page.issues) bytes += issue.path.size() + issue.reason.size() + 64;
    if (bytes <= maximum) return;
    std::map<std::string, std::size_t> counts;
    for (const auto& issue : page.issues) counts[issue.reason] += issue.count;
    page.issues.clear();
    bytes = 0;
    std::size_t omitted = 0;
    for (const auto& item : counts) {
        const auto needed = item.first.size() + 64;
        if (bytes + needed + 96 > maximum) {
            omitted += item.second;
            continue;
        }
        page.issues.push_back({item.first, {}, item.second});
        bytes += needed;
    }
    if (omitted) page.issues.push_back({"additional_issues", {}, omitted});
}

bool acquireForRequest(std::unique_lock<std::mutex>& lock, const Clock::time_point& start,
                       const MaiFileSearchBudget& budget, const MaiFileSearchCallbacks& callbacks,
                       MaiFileSearchPage& page) {
    for (;;) {
        if (isCanceled(callbacks)) {
            page.canceled = true;
            addIssue(page, "canceled");
            return false;
        }
        if (expired(start, budget)) {
            addIssue(page, "time_budget_waiting_for_search");
            return false;
        }
        if (lock.try_lock()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

struct MaiFileSearch::Impl {
    struct Session {
        std::string id;
        std::string key;
        bool content = false;
        std::shared_ptr<TreeSnapshot> tree;
        Clock::time_point touched;
        std::vector<MaiFileNameMatch> names;
        std::size_t namePosition = 0;
        std::size_t filePosition = 0;
        std::size_t linePosition = 0;
        std::size_t fileMatches = 0;
        std::string currentText;
        std::vector<TextPosition> currentLines;
        std::map<std::size_t, MaiFileSearchPage> pages;
        std::size_t nextPage = 0;
        std::size_t bytes = 0;
        std::vector<MaiFileSearchIssue> skippedIssues;
    };
    std::mutex mutex;
    std::map<std::string, Session> sessions;
    std::atomic<std::uint64_t> counter{0};

    bool prune(const std::string& protectedId = {});
    std::string newId();
    Session* resume(const std::string& cursor, const std::string& key, bool content,
                    std::size_t& pageNumber, MaiFileSearchPage& page);
    bool validatePage(const Session& session, const MaiFileSearchCallbacks& callbacks,
                      const Clock::time_point& start, const MaiFileSearchBudget& budget,
                      MaiFileSearchPage& page);
    void save(Session& session, std::size_t pageNumber, MaiFileSearchPage& page);
    std::shared_ptr<TreeSnapshot> cachedTree(const std::string& corpusKey);
};

std::shared_ptr<TreeSnapshot> MaiFileSearch::Impl::cachedTree(const std::string& corpusKey) {
    // 与上游长期文件搜索会话一样复用已采集的路径集合；没有文件系统监视器。
    // 仅新查询受短 TTL 限制，已发出的分页游标继续引用冻结集合。
    std::shared_ptr<TreeSnapshot> newest;
    for (const auto& item : sessions) {
        const auto& previous = item.second.tree;
        if (previous->corpusKey == corpusKey &&
            Clock::now() - previous->captured < std::chrono::seconds(5) &&
            (!newest || previous->captured > newest->captured))
            newest = previous;
    }
    return newest;
}

bool MaiFileSearch::Impl::prune(const std::string& protectedId) {
    const auto now = Clock::now();
    for (auto iterator = sessions.begin(); iterator != sessions.end();) {
        if (iterator->first != protectedId &&
            now - iterator->second.touched > std::chrono::seconds(120))
            iterator = sessions.erase(iterator);
        else
            ++iterator;
    }
    for (;;) {
        std::size_t bytes = 0;
        std::set<const TreeSnapshot*> trees;
        for (const auto& item : sessions) {
            if (trees.insert(item.second.tree.get()).second)
                bytes += item.second.tree->bytes + item.second.tree->corpusKey.capacity() + 32768;
            // 为容器节点、短字符串及错误元数据额外预留容量，避免只计正文而低估缓存。
            bytes += item.second.bytes + item.second.key.capacity() + item.second.id.capacity() +
                     32768 + item.second.currentText.capacity() +
                     item.second.currentLines.capacity() * sizeof(TextPosition);
        }
        if (sessions.size() <= kMaxSessions && bytes <= kMaxCacheBytes) return true;
        auto oldest = sessions.end();
        for (auto iterator = sessions.begin(); iterator != sessions.end(); ++iterator)
            if (iterator->first != protectedId &&
                (oldest == sessions.end() || iterator->second.touched < oldest->second.touched))
                oldest = iterator;
        if (oldest == sessions.end()) return false;
        sessions.erase(oldest);
    }
}

std::string MaiFileSearch::Impl::newId() {
    std::ostringstream output;
    output << std::hex << Clock::now().time_since_epoch().count() << '-' << ++counter;
    return output.str();
}

MaiFileSearch::Impl::Session* MaiFileSearch::Impl::resume(const std::string& cursor,
                                                          const std::string& key, bool content,
                                                          std::size_t& pageNumber,
                                                          MaiFileSearchPage& page) {
    const auto separator = cursor.find(':');
    const auto found = sessions.find(cursor.substr(0, separator));
    if (separator == std::string::npos || found == sessions.end()) {
        page.error = {MaiErrorCode::InvalidInput,
                      "Search cursor is invalid, expired or evicted; start a new search"};
        return nullptr;
    }
    if (found->second.key != key || found->second.content != content) {
        page.error = {MaiErrorCode::InvalidInput,
                      "Search cursor belongs to a different root or query"};
        return nullptr;
    }
    pageNumber = 0;
    if (separator + 1 == cursor.size()) {
        page.error = {MaiErrorCode::InvalidInput, "Invalid search cursor page"};
        return nullptr;
    }
    for (std::size_t at = separator + 1; at < cursor.size(); ++at) {
        if (cursor[at] < '0' || cursor[at] > '9' || pageNumber > 1000000) {
            page.error = {MaiErrorCode::InvalidInput, "Invalid search cursor page"};
            return nullptr;
        }
        pageNumber = pageNumber * 10 + static_cast<std::size_t>(cursor[at] - '0');
    }
    if (pageNumber > found->second.nextPage) {
        page.error = {MaiErrorCode::InvalidInput,
                      "Search cursor does not identify a retained continuation"};
        return nullptr;
    }
    if (pageNumber == found->second.nextPage && !found->second.pages.empty() &&
        !found->second.pages.rbegin()->second.hasMore) {
        page.error = {MaiErrorCode::InvalidInput, "Search cursor is past the retained results"};
        return nullptr;
    }
    found->second.touched = Clock::now();
    return &found->second;
}

bool MaiFileSearch::Impl::validatePage(const Session& session,
                                       const MaiFileSearchCallbacks& callbacks,
                                       const Clock::time_point& start,
                                       const MaiFileSearchBudget& budget, MaiFileSearchPage& page) {
    std::set<std::string> paths;
    for (const auto& match : page.files) paths.insert(match.path);
    for (const auto& match : page.matches) paths.insert(match.path);
    for (const auto& path : paths) {
        if (isCanceled(callbacks) || expired(start, budget)) {
            page.canceled = isCanceled(callbacks);
            addIssue(page, page.canceled ? "canceled" : "time_budget_validating_page");
            page.files.clear();
            page.matches.clear();
            page.nextCursor.clear();
            page.hasMore = false;
            return false;
        }
        const auto found =
            std::lower_bound(session.tree->files.begin(), session.tree->files.end(), path,
                             [](const IndexedFile& file, const std::string& relative) {
                                 return file.relative < relative;
                             });
        MaiRegularFileInfo info;
        if (found == session.tree->files.end() || found->relative != path) return false;
        const auto resolved = MaiFileSystem::resolve(found->path);
        if (!isInside(session.tree->root, resolved) || MaiFileSystem::isSymbolicLink(found->path) ||
            (callbacks.pathAllowed && !callbacks.pathAllowed(resolved)) ||
            !MaiFileSystem::regularFileInfo(found->path, info) || !sameInfo(info, found->info)) {
            page.error = {MaiErrorCode::InvalidInput,
                          "Search result source changed or access was denied; refresh the search"};
            page.files.clear();
            page.matches.clear();
            page.nextCursor.clear();
            page.hasMore = false;
            return false;
        }
    }
    return true;
}

void MaiFileSearch::Impl::save(Session& session, std::size_t pageNumber, MaiFileSearchPage& page) {
    page.snapshotId = session.id;
    page.snapshotAgeMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - session.tree->captured)
            .count());
    if (page.hasMore) page.nextCursor = session.id + ":" + std::to_string(pageNumber + 1);
    session.nextPage = pageNumber + 1;
    session.bytes += pageBytes(page);
    session.pages.emplace(pageNumber, page);
    session.touched = Clock::now();
    const auto id = session.id;
    if (!prune(id)) {
        addIssue(page, "cache_capacity");
        page.nextCursor.clear();
        page.hasMore = false;
        sessions.erase(id);
    }
}

MaiFileSearch::MaiFileSearch() : mImpl(new Impl) {}
MaiFileSearch::~MaiFileSearch() = default;

MaiFileSearchPage MaiFileSearch::findFiles(const MaiFileNameSearchOptions& options,
                                           const MaiFileSearchCallbacks& callbacks) {
    const auto start = Clock::now();
    MaiFileSearchPage page;
    if (!validOptions(options.filter, options.budget, options.pageSize, page)) return page;
    if (options.scopeKey.size() > 4096 || (options.refresh && !options.cursor.empty())) {
        page.error = {MaiErrorCode::InvalidInput, "Invalid scopeKey or refresh with cursor"};
        return page;
    }
    std::unique_lock<std::mutex> lock(mImpl->mutex, std::defer_lock);
    if (!acquireForRequest(lock, start, options.budget, callbacks, page)) return page;
    mImpl->prune();
    if (options.query.size() > kMaxPatternBytes) {
        page.error = {MaiErrorCode::InvalidInput, "Filename query exceeds 1024 bytes"};
        return page;
    }
    std::vector<std::string> queryAlternatives;
    if ((options.mode == MaiFileNameSearchMode::Glob &&
         !expandGlobs(options.query, queryAlternatives)) ||
        (options.mode == MaiFileNameSearchMode::Fuzzy && !validUtf8(options.query))) {
        page.error = {MaiErrorCode::InvalidInput,
                      "Invalid Unicode fuzzy query or glob braces/alternatives"};
        return page;
    }
    const auto root = MaiFileSystem::resolve(options.root);
    if (root.isEmpty() || !MaiFileSystem::isDirectory(root) ||
        (callbacks.pathAllowed && !callbacks.pathAllowed(root))) {
        page.error = {MaiErrorCode::InvalidInput, "Search root is missing, inaccessible or denied"};
        return page;
    }
    auto corpusKey = commonKey(root, options.filter, options.budget);
    appendKey(corpusKey, options.scopeKey);
    auto key = corpusKey;
    appendKey(key, options.query);
    appendKey(key, std::to_string(static_cast<int>(options.mode)) + ":" +
                       std::to_string(options.caseSensitive) + ":" +
                       std::to_string(options.pageSize));
    Impl::Session* session = nullptr;
    std::size_t pageNumber = 0;
    bool reused = false;
    if (!options.cursor.empty()) {
        session = mImpl->resume(options.cursor, key, false, pageNumber, page);
        if (!session) return page;
    }
    auto tree = session ? session->tree : options.refresh ? nullptr : mImpl->cachedTree(corpusKey);
    reused = static_cast<bool>(tree);
    if (!tree) {
        tree = std::make_shared<TreeSnapshot>(
            collectTree(root, options.filter, options.budget, callbacks, start));
        tree->corpusKey = corpusKey;
    }
    if (tree->observations.canceled) return tree->observations;
    if (session) {
        const auto cached = session->pages.find(pageNumber);
        if (cached != session->pages.end()) {
            page = cached->second;
            page.cacheReused = true;
            page.visitedEntries = 0;
            page.searchedFiles = 0;
            page.readBytes = 0;
            page.snapshotAgeMilliseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - tree->captured)
                    .count());
            mImpl->validatePage(*session, callbacks, start, options.budget, page);
            return page;
        }
    } else {
        const auto scalarQuery = unicodeScalars(options.query, options.caseSensitive);
        Impl::Session fresh;
        fresh.id = mImpl->newId();
        fresh.key = key;
        fresh.tree = tree;
        fresh.touched = Clock::now();
        for (const auto& file : tree->files) {
            if (isCanceled(callbacks) || expired(start, options.budget)) {
                page.canceled = isCanceled(callbacks);
                addIssue(page, page.canceled ? "canceled" : "time_budget_ranking");
                return page;
            }
            const auto score = options.mode == MaiFileNameSearchMode::Fuzzy
                                   ? fuzzyScore(scalarQuery, file.relative, options.caseSensitive)
                               : pathGlob(options.query, file.relative, options.caseSensitive) ? 0
                                                                                               : -1;
            if (score >= 0) fresh.names.push_back({file.relative, score});
        }
        std::sort(fresh.names.begin(), fresh.names.end(),
                  [](const MaiFileNameMatch& first, const MaiFileNameMatch& second) {
                      if (first.score != second.score) return first.score > second.score;
                      const auto firstName = baseName(first.path),
                                 secondName = baseName(second.path);
                      if (firstName != secondName) return firstName < secondName;
                      return first.path < second.path;
                  });
        for (const auto& name : fresh.names) fresh.bytes += name.path.capacity() + 96;
        const auto inserted = mImpl->sessions.emplace(fresh.id, std::move(fresh));
        session = &inserted.first->second;
    }
    page = session->tree->observations;
    if (reused) page.visitedEntries = 0;
    const auto oldPosition = session->namePosition;
    std::size_t outputBytes = metadataReserve(options.budget);
    while (session->namePosition < session->names.size() && page.files.size() < options.pageSize) {
        const auto& match = session->names[session->namePosition];
        if (outputBytes + match.path.size() + 48 > options.budget.maxOutputBytes) {
            if (page.files.empty()) {
                page.error = {
                    MaiErrorCode::InvalidInput,
                    "Search path or metadata exceeds output budget; increase maxOutputBytes"};
                mImpl->prune();
                return page;
            }
            break;
        }
        outputBytes += match.path.size() + 48;
        page.files.push_back(match);
        ++session->namePosition;
    }
    page.hasMore = session->namePosition < session->names.size();
    page.cacheReused = !options.cursor.empty() || reused;
    boundIssues(page, options.budget);
    if (!mImpl->validatePage(*session, callbacks, start, options.budget, page)) {
        session->namePosition = oldPosition;
        mImpl->prune();
        return page;
    }
    mImpl->save(*session, pageNumber, page);
    return page;
}

MaiFileSearchPage MaiFileSearch::searchText(const MaiFileContentSearchOptions& options,
                                            const MaiFileSearchCallbacks& callbacks) {
    const auto start = Clock::now();
    MaiFileSearchPage page;
    if (!validOptions(options.filter, options.budget, options.pageSize, page)) return page;
    if (options.scopeKey.size() > 4096 || (options.refresh && !options.cursor.empty())) {
        page.error = {MaiErrorCode::InvalidInput, "Invalid scopeKey or refresh with cursor"};
        return page;
    }
    std::unique_lock<std::mutex> lock(mImpl->mutex, std::defer_lock);
    if (!acquireForRequest(lock, start, options.budget, callbacks, page)) return page;
    mImpl->prune();
    if (options.pattern.empty() || options.pattern.size() > 65536 || options.contextBefore > 20 ||
        options.contextAfter > 20 || options.maxLineDisplayBytes < 16 ||
        options.maxLineDisplayBytes > 65536) {
        page.error = {MaiErrorCode::InvalidInput,
                      "Invalid content pattern, context or display limit"};
        return page;
    }
    SafeRegex regex;
    if (options.isRegex && !regex.compile(options.pattern, options.caseSensitive)) {
        page.error = {MaiErrorCode::InvalidInput, regex.error()};
        return page;
    }
    const auto root = MaiFileSystem::resolve(options.root);
    if (root.isEmpty() || (callbacks.pathAllowed && !callbacks.pathAllowed(root))) {
        page.error = {MaiErrorCode::InvalidInput, "Search root is missing, inaccessible or denied"};
        return page;
    }
    const bool singleFile = !MaiFileSystem::isDirectory(root);
    MaiRegularFileInfo singleInfo;
    if (singleFile) {
        if (MaiFileSystem::isSymbolicLink(options.root)) {
            page.error = {MaiErrorCode::InvalidInput,
                          "Search target must be an ordinary file, not a symbolic link"};
            return page;
        }
        if (!MaiFileSystem::regularFileInfo(root, singleInfo)) {
            std::string ignored;
            const auto readError =
                MaiFileSystem::readFile(root, ignored, options.budget.maxFileBytes);
            page.error = readError ? readError
                                   : MaiError(MaiErrorCode::InvalidInput,
                                              "Search target is not a supported ordinary file");
            return page;
        }
        if (singleInfo.bytes > options.budget.maxFileBytes) {
            page.error = {MaiErrorCode::InvalidInput,
                          options.budget.maxFileBytes == 2u * 1024 * 1024
                              ? "File exceeds the 2 MiB search limit"
                              : "File exceeds maxFileBytes search limit"};
            return page;
        }
    }
    auto corpusKey = commonKey(root, options.filter, options.budget);
    appendKey(corpusKey, options.scopeKey);
    auto key = corpusKey;
    appendKey(key, options.pattern);
    appendKey(key, std::to_string(options.isRegex) + ":" + std::to_string(options.caseSensitive) +
                       ":" + std::to_string(static_cast<int>(options.mode)) + ":" +
                       std::to_string(options.pageSize) + ":" +
                       std::to_string(options.contextBefore) + ":" +
                       std::to_string(options.contextAfter) + ":" +
                       std::to_string(options.maxLineDisplayBytes));
    Impl::Session* session = nullptr;
    std::size_t pageNumber = 0;
    bool reused = false;
    if (!options.cursor.empty()) {
        session = mImpl->resume(options.cursor, key, true, pageNumber, page);
        if (!session) return page;
    }
    auto tree = session ? session->tree : options.refresh ? nullptr : mImpl->cachedTree(corpusKey);
    reused = static_cast<bool>(tree);
    if (!tree) {
        tree = std::make_shared<TreeSnapshot>(
            singleFile ? singleFileTree(root, singleInfo, options.filter)
                       : collectTree(root, options.filter, options.budget, callbacks, start));
        tree->corpusKey = corpusKey;
    }
    if (tree->observations.canceled) return tree->observations;
    if (session) {
        const auto cached = session->pages.find(pageNumber);
        if (cached != session->pages.end()) {
            page = cached->second;
            page.cacheReused = true;
            page.visitedEntries = 0;
            page.searchedFiles = 0;
            page.readBytes = 0;
            page.snapshotAgeMilliseconds = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - tree->captured)
                    .count());
            mImpl->validatePage(*session, callbacks, start, options.budget, page);
            return page;
        }
    } else {
        Impl::Session fresh;
        fresh.id = mImpl->newId();
        fresh.key = key;
        fresh.content = true;
        fresh.tree = tree;
        fresh.touched = Clock::now();
        const auto inserted = mImpl->sessions.emplace(fresh.id, std::move(fresh));
        session = &inserted.first->second;
    }
    // 取消时恢复原进度，重放旧游标不会丢掉本次已读取而未交付的行。
    const auto oldFile = session->filePosition, oldLine = session->linePosition,
               oldMatches = session->fileMatches;
    const auto oldText = session->currentText;
    const auto oldLines = session->currentLines;
    page = session->tree->observations;
    if (reused) page.visitedEntries = 0;
    for (const auto& issue : session->skippedIssues) {
        const auto existing = std::find_if(
            page.issues.begin(), page.issues.end(), [&](const MaiFileSearchIssue& item) {
                return item.reason == issue.reason && item.path == issue.path;
            });
        if (existing == page.issues.end()) {
            page.issues.push_back(issue);
            page.partial = true;
        }
    }
    page.cacheReused = !options.cursor.empty() || reused;
    std::uint64_t regexSteps = 0;
    std::size_t outputBytes = metadataReserve(options.budget);
    const auto needle = folded(options.pattern, options.caseSensitive);
    auto finishFile = [&] {
        ++session->filePosition;
        session->linePosition = 0;
        session->fileMatches = 0;
        session->currentText.clear();
        session->currentLines.clear();
    };
    bool stopPage = false;
    while (session->filePosition < session->tree->files.size() && !stopPage) {
        if (isCanceled(callbacks)) {
            page.canceled = true;
            addIssue(page, "canceled");
            break;
        }
        if (expired(start, options.budget)) {
            addIssue(page, "time_budget");
            break;
        }
        const auto& file = session->tree->files[session->filePosition];
        const auto resolved = MaiFileSystem::resolve(file.path);
        MaiRegularFileInfo before;
        if (!isInside(root, resolved) ||
            (callbacks.pathAllowed && !callbacks.pathAllowed(resolved)) ||
            !MaiFileSystem::regularFileInfo(file.path, before) || !sameInfo(before, file.info)) {
            page.error = {MaiErrorCode::InvalidInput,
                          "Search source changed or access was denied; refresh the search"};
            page.matches.clear();
            break;
        }
        if (session->currentText.empty() && session->currentLines.empty()) {
            if (page.searchedFiles >= options.budget.maxFiles) {
                addIssue(page, "file_budget");
                break;
            }
            if (before.bytes > options.budget.maxFileBytes) {
                addIssue(page, "file_too_large", file.relative);
                finishFile();
                continue;
            }
            if (before.bytes >
                options.budget.maxBytes - std::min(page.readBytes, options.budget.maxBytes)) {
                if (!page.readBytes) {
                    addIssue(page, "file_exceeds_read_budget", file.relative);
                    finishFile();
                    continue;
                }
                addIssue(page, "read_budget");
                break;
            }
            std::string bytes;
            bool truncated = false;
            const auto error =
                MaiFileSystem::readFile(file.path, bytes, options.budget.maxFileBytes, &truncated);
            page.readBytes += bytes.size();
            ++page.searchedFiles;
            MaiRegularFileInfo after;
            if (error || truncated || !MaiFileSystem::regularFileInfo(file.path, after) ||
                !sameInfo(before, after) || !isInside(root, MaiFileSystem::resolve(file.path))) {
                if (session->tree->singleFile) {
                    page.error = error ? error
                                       : MaiError(MaiErrorCode::InvalidInput,
                                                  "Search file changed or exceeded the read limit");
                    break;
                }
                addIssue(page, "read_failed_or_source_changed", file.relative);
                finishFile();
                continue;
            }
            if (callbacks.decodeText) {
                auto decoded = callbacks.decodeText(file.path, bytes);
                if (!decoded) {
                    if (session->tree->singleFile) {
                        page.error = decoded.error();
                        break;
                    }
                    addIssue(page, "decode_failed", file.relative);
                    finishFile();
                    continue;
                }
                session->currentText = std::move(decoded.value());
            } else {
                if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
                if (!validUtf8(bytes)) {
                    if (session->tree->singleFile) {
                        page.error = {MaiErrorCode::InvalidInput, "Search file is not valid UTF-8"};
                        break;
                    }
                    addIssue(page, "encoding_not_utf8", file.relative);
                    finishFile();
                    continue;
                }
                session->currentText = std::move(bytes);
            }
            if (session->currentText.size() > 64u * 1024 * 1024 ||
                binaryText(session->currentText)) {
                if (session->tree->singleFile) {
                    page.error = {MaiErrorCode::InvalidInput,
                                  "Search file is binary or decoded text exceeds the limit"};
                    break;
                }
                addIssue(page, "binary_or_decoded_file_too_large", file.relative);
                finishFile();
                continue;
            }
            if (!validUtf8(session->currentText)) {
                if (session->tree->singleFile) {
                    page.error = {MaiErrorCode::InvalidInput,
                                  "Decoded search file is not valid UTF-8"};
                    break;
                }
                addIssue(page, "decoded_text_not_utf8", file.relative);
                finishFile();
                continue;
            }
            MaiRegularFileInfo afterDecode;
            if (!MaiFileSystem::regularFileInfo(file.path, afterDecode) ||
                !sameInfo(before, afterDecode) ||
                !isInside(root, MaiFileSystem::resolve(file.path))) {
                addIssue(page, "source_changed_during_decode", file.relative);
                finishFile();
                continue;
            }
            session->currentLines = textLines(session->currentText);
        }
        while (session->linePosition < session->currentLines.size()) {
            if (isCanceled(callbacks)) {
                page.canceled = true;
                addIssue(page, "canceled");
                stopPage = true;
                break;
            }
            if (expired(start, options.budget)) {
                addIssue(page, "time_budget");
                stopPage = true;
                break;
            }
            const auto position = session->currentLines[session->linePosition];
            std::size_t focus = 0;
            bool interrupted = false;
            bool matched = false;
            if (options.isRegex) {
                matched = regex.search(
                    session->currentText, position.start, position.length, regexSteps,
                    options.budget.maxRegexSteps, focus,
                    [&] { return isCanceled(callbacks) || expired(start, options.budget); },
                    interrupted);
            } else if (options.caseSensitive) {
                const auto line =
                    std::string_view(session->currentText.data() + position.start, position.length);
                focus = line.find(needle);
                matched = focus != std::string_view::npos;
            } else {
                const auto line =
                    folded(session->currentText.substr(position.start, position.length), false);
                focus = line.find(needle);
                matched = focus != std::string::npos;
            }
            if (interrupted) {
                if (isCanceled(callbacks)) {
                    page.canceled = true;
                    addIssue(page, "canceled");
                    stopPage = true;
                    break;
                }
                if (expired(start, options.budget)) {
                    addIssue(page, "time_budget");
                    stopPage = true;
                    break;
                }
                addIssue(page, "regex_work_budget_line_skipped", file.relative);
                ++session->linePosition;
                stopPage = true;
                break;
            }
            if (!matched) {
                ++session->linePosition;
                continue;
            }
            if (options.mode == MaiFileContentSearchMode::Count) {
                ++session->fileMatches;
                ++page.matchedLines;
                ++session->linePosition;
                continue;
            }
            MaiFileContentMatch match;
            match.path = file.relative;
            match.line = session->linePosition + 1;
            match.count = 1;
            if (options.mode == MaiFileContentSearchMode::Lines) {
                const auto displayed =
                    displayLine(session->currentText, position.start, position.length, match.line,
                                focus, options.maxLineDisplayBytes);
                match.text = displayed.text;
                match.displayOffsetBytes = displayed.displayOffsetBytes;
                match.truncated = displayed.truncated;
                const auto first = session->linePosition > options.contextBefore
                                       ? session->linePosition - options.contextBefore
                                       : 0;
                for (auto index = first; index < session->linePosition; ++index) {
                    const auto context = session->currentLines[index];
                    match.before.push_back(displayLine(session->currentText, context.start,
                                                       context.length, index + 1, 0,
                                                       options.maxLineDisplayBytes));
                }
                const auto last = std::min(session->currentLines.size(),
                                           session->linePosition + options.contextAfter + 1);
                for (auto index = session->linePosition + 1; index < last; ++index) {
                    const auto context = session->currentLines[index];
                    match.after.push_back(displayLine(session->currentText, context.start,
                                                      context.length, index + 1, 0,
                                                      options.maxLineDisplayBytes));
                }
            }
            MaiFileSearchPage estimate;
            estimate.matches.push_back(match);
            const auto bytes = pageBytes(estimate) - 512;
            if (!page.matches.empty() && (page.matches.size() >= options.pageSize ||
                                          outputBytes + bytes > options.budget.maxOutputBytes)) {
                stopPage = true;
                break;
            }
            if (bytes + metadataReserve(options.budget) > options.budget.maxOutputBytes) {
                addIssue(page, "context_or_display_exceeds_output_budget", file.relative);
                // 首条结果缩减展示，而不丢掉命中；完整文本仍可通过普通 reader 读取。
                match.before.clear();
                match.after.clear();
                const auto displayBudget =
                    options.budget.maxOutputBytes - metadataReserve(options.budget) -
                    std::min(file.relative.size() + 160,
                             options.budget.maxOutputBytes - metadataReserve(options.budget));
                if (!displayBudget && options.mode == MaiFileContentSearchMode::Lines) {
                    page.error = {MaiErrorCode::InvalidInput,
                                  "Search path exceeds output budget; increase maxOutputBytes"};
                    stopPage = true;
                    break;
                }
                if (match.text.size() > displayBudget) {
                    const auto smaller =
                        displayLine(session->currentText, position.start, position.length,
                                    match.line, focus, displayBudget);
                    match.text = smaller.text;
                    match.displayOffsetBytes = smaller.displayOffsetBytes;
                    match.truncated = smaller.truncated;
                }
            }
            outputBytes += bytes;
            page.matches.push_back(std::move(match));
            ++page.matchedLines;
            ++session->linePosition;
            if (options.mode == MaiFileContentSearchMode::Files) {
                session->linePosition = session->currentLines.size();
                break;
            }
            if (page.matches.size() >= options.pageSize) {
                stopPage = true;
                break;
            }
        }
        if (session->linePosition == session->currentLines.size() &&
            options.mode != MaiFileContentSearchMode::Count) {
            // 页面恰好在 EOF 达到数量上限时，仍交付文件结束位置，避免制造空的末页游标。
            finishFile();
            if (page.matches.size() >= options.pageSize) stopPage = true;
            continue;
        }
        if (session->linePosition == session->currentLines.size() && !stopPage) {
            if (options.mode == MaiFileContentSearchMode::Count && session->fileMatches) {
                const auto bytes = file.relative.size() + 160;
                if (page.matches.size() >= options.pageSize ||
                    (!page.matches.empty() &&
                     outputBytes + bytes > options.budget.maxOutputBytes)) {
                    stopPage = true;
                    break;
                }
                if (outputBytes + bytes > options.budget.maxOutputBytes) {
                    page.error = {
                        MaiErrorCode::InvalidInput,
                        "Search path or metadata exceeds output budget; increase maxOutputBytes"};
                    stopPage = true;
                    break;
                }
                MaiFileContentMatch match;
                match.path = file.relative;
                match.count = session->fileMatches;
                page.matches.push_back(std::move(match));
                outputBytes += bytes;
            }
            finishFile();
            if (page.matches.size() >= options.pageSize) stopPage = true;
        }
    }
    if (!page.canceled && !page.error &&
        !mImpl->validatePage(*session, callbacks, start, options.budget, page)) {
        session->filePosition = oldFile;
        session->linePosition = oldLine;
        session->fileMatches = oldMatches;
        session->currentText = oldText;
        session->currentLines = oldLines;
        mImpl->prune();
        return page;
    }
    if (page.canceled || page.error) {
        session->filePosition = oldFile;
        session->linePosition = oldLine;
        session->fileMatches = oldMatches;
        session->currentText = oldText;
        session->currentLines = oldLines;
        mImpl->prune();
        return page;
    }
    page.hasMore = session->filePosition < session->tree->files.size();
    // 当前文件最多受 maxFileBytes 限制；分页缓存及目录索引也计入淘汰预算。
    boundIssues(page, options.budget);
    session->skippedIssues.clear();
    for (const auto& issue : page.issues)
        if (issue.reason != "time_budget" && issue.reason != "read_budget" &&
            issue.reason != "file_budget" && issue.reason != "canceled")
            session->skippedIssues.push_back(issue);
    mImpl->save(*session, pageNumber, page);
    return page;
}

void MaiFileSearch::invalidate(const MaiFilePath& root) {
    const auto resolved = MaiFileSystem::resolve(root);
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    for (auto iterator = mImpl->sessions.begin(); iterator != mImpl->sessions.end();) {
        if (iterator->second.tree->root == resolved)
            iterator = mImpl->sessions.erase(iterator);
        else
            ++iterator;
    }
}

void MaiFileSearch::clear() {
    std::lock_guard<std::mutex> lock(mImpl->mutex);
    mImpl->sessions.clear();
}
