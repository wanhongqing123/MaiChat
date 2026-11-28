// 文件作用：实现有界技能发现、不可变目录快照及按需正文读取。
#include "MaiSkillManager.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

#include <json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include "MaiFileSystem.h"
#include "MaiBuiltinSkills.h"
#include "MaiModelClient.h"

namespace {
constexpr std::size_t kMaxScanDepth = 6;
constexpr std::size_t kMaxDirectoriesPerRoot = 2000;
constexpr std::size_t kMaxEntriesPerRoot = 20000;
constexpr std::size_t kMaximumReadBytes = 1024 * 1024;
std::atomic<std::uint64_t> nextOwner{1};

struct MaiDiscoveredSkill {
    MaiSkillMetadata metadata;
    MaiFilePath discoveryPath;
    MaiRegularFileInfo fileInfo;
    // 仅指向本进程静态资源，不通过模型指定的路径或磁盘文件身份查找。
    const MaiBuiltinSkillResource* builtin = nullptr;
};

MaiFilePath child(const MaiFilePath& directory, const std::string& name) {
    return directory.append(MaiFilePath::fromUtf8(name));
}

// Windows 规范化后的大小写别名必须共享身份，避免同一文件重复进入目录。
MaiFilePath::StringType pathKey(const MaiFilePath& path) {
    auto value = path.value();
#if defined(_WIN32)
    for (auto& character : value) {
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        if (character == L'\\') character = L'/';
    }
#endif
    return value;
}

bool sameFile(const MaiRegularFileInfo& left, const MaiRegularFileInfo& right) {
    return left.bytes == right.bytes && left.modified == right.modified &&
           left.identity == right.identity;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string singleLine(const std::string& value) {
    std::string result;
    bool separated = false;
    for (const unsigned char character : value) {
        if (std::isspace(character)) {
            separated = !result.empty();
        } else {
            if (separated) result += ' ';
            result += static_cast<char>(character);
            separated = false;
        }
    }
    return result;
}

bool validUtf8(const std::string& value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first < 0x80) {
            if (first == 0 || (first < 0x20 && first != '\t' && first != '\r' && first != '\n'))
                return false;
            ++index;
            continue;
        }
        const std::size_t width = first >= 0xC2 && first <= 0xDF   ? 2
                                  : first >= 0xE0 && first <= 0xEF ? 3
                                  : first >= 0xF0 && first <= 0xF4 ? 4
                                                                   : 0;
        if (width == 0 || index + width > value.size()) return false;
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if (second < 0x80 || second > 0xBF || (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second >= 0xA0) || (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second >= 0x90))
            return false;
        for (std::size_t offset = 2; offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if (next < 0x80 || next > 0xBF) return false;
        }
        index += width;
    }
    return true;
}

std::size_t codePointCount(const std::string& value) {
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](unsigned char byte) { return (byte & 0xC0) != 0x80; }));
}

// 只在 UTF-8 标量边界缩短描述；目录排序和稳定 id 不随预算改变。
std::string shortenDescription(const std::string& text, std::size_t characters) {
    if (codePointCount(text) <= characters) return text;
    if (characters == 0) return {};
    const auto suffix = characters >= 3 ? std::string("...") : std::string(characters, '.');
    const auto prefix = characters - suffix.size();
    std::size_t index = 0;
    std::size_t count = 0;
    while (index < text.size() && count < prefix) {
        ++index;
        while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80)
            ++index;
        ++count;
    }
    return text.substr(0, index) + suffix;
}

std::string lowerAscii(std::string text) {
    for (auto& character : text)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return text;
}

// 策略文件是可选元数据，不将完整 YAML 的别名/合并等语法误解成允许隐式调用。
MaiResult<bool> readImplicitPolicy(const MaiFilePath& directory) {
    const auto path = child(directory, "agents/openai.yaml");
    auto handle = MaiFileReadHandle::open(path);
    if (!handle) {
        if (handle.error().code() == MaiErrorCode::NotFound) return true;
        return handle.error();
    }
    const auto before = handle.value()->snapshot();
    if (!before) return before.error();
    constexpr std::size_t budget = 64 * 1024;
    if (before.value().bytes > budget)
        return {MaiErrorCode::InvalidInput, "skill policy exceeds 64KiB"};
    std::string text;
    bool eof = false;
    const auto error = handle.value()->readAt(0, budget, text, &eof);
    if (error) return error;
    const auto changed = handle.value()->validate(before.value());
    if (changed) return changed;
    if (text.size() != before.value().bytes || !validUtf8(text))
        return {MaiErrorCode::InvalidInput, "skill policy is incomplete or invalid UTF-8"};
    bool policy = false;
    bool seenPolicy = false;
    bool seenAllow = false;
    bool allowed = true;
    std::size_t policyIndent = 0;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        const auto value = trim(line);
        if (value.empty() || value.front() == '#' || value == "---" || value == "...") continue;
        if (line.find('\t') != std::string::npos)
            return {MaiErrorCode::NotSupported, "tabs are unsupported in skill policy YAML"};
        const auto indent = line.find_first_not_of(' ');
        const auto colon = value.find(':');
        if (indent == 0) {
            policy = false;
            if (colon == std::string::npos)
                return {MaiErrorCode::NotSupported, "expected a skill policy YAML mapping"};
            const auto key = trim(value.substr(0, colon));
            if (key == "<<" || key == "\"policy\"" || key == "'policy'")
                return {MaiErrorCode::NotSupported, "merged or quoted skill policy is unsupported"};
            if (key.empty() ||
                key.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos)
                return {MaiErrorCode::NotSupported, "unsupported skill policy YAML mapping key"};
            if (key != "policy") continue;
            const auto tail = trim(value.substr(colon + 1));
            if (seenPolicy || (!tail.empty() && tail.front() != '#'))
                return {MaiErrorCode::NotSupported, "expected one plain skill policy mapping"};
            seenPolicy = true;
            policy = true;
            continue;
        }
        if (!policy) continue;
        if (policyIndent == 0) policyIndent = indent;
        if (indent != policyIndent || colon == std::string::npos)
            return {MaiErrorCode::NotSupported, "unsupported nested skill policy YAML"};
        const auto key = trim(value.substr(0, colon));
        if (key != "allow_implicit_invocation")
            return {MaiErrorCode::NotSupported, "unsupported skill policy key: " + key};
        auto boolean = trim(value.substr(colon + 1));
        const auto comment = boolean.find(" #");
        if (comment != std::string::npos) boolean = trim(boolean.substr(0, comment));
        if (seenAllow || (boolean != "true" && boolean != "false"))
            return {MaiErrorCode::InvalidInput,
                    "allow_implicit_invocation must be one plain true or false boolean"};
        seenAllow = true;
        allowed = boolean == "true";
    }
    return allowed;
}

MaiError unsupported(const std::string& detail) {
    return {MaiErrorCode::NotSupported, "unsupported skill frontmatter: " + detail};
}

bool isCommentTail(const std::string& value) {
    const auto tail = trim(value);
    return tail.empty() || tail.front() == '#';
}

// 这里只实现所声明的 YAML 标量子集，不能把复杂语法误当作普通字符串。
MaiResult<std::string> parseScalar(const std::string& input) {
    auto value = trim(input);
    if (value.empty()) return std::string{};
    if (value.front() == '"') {
        bool escaped = false;
        for (std::size_t index = 1; index < value.size(); ++index) {
            if (!escaped && value[index] == '"') {
                if (!isCommentTail(value.substr(index + 1)))
                    return MaiError{MaiErrorCode::InvalidInput, "text after quoted skill scalar"};
                // JSON 的字符串子集复用已有依赖处理 Unicode 转义，不引入 YAML 库。
                const auto parsed =
                    nlohmann::json::parse(value.substr(0, index + 1), nullptr, false);
                if (!parsed.is_string())
                    return unsupported("double-quoted YAML escape outside the JSON string subset");
                return parsed.get<std::string>();
            }
            if (!escaped && value[index] == '\\')
                escaped = true;
            else
                escaped = false;
        }
        return MaiError{MaiErrorCode::InvalidInput, "unterminated quoted skill scalar"};
    }
    if (value.front() == '\'') {
        std::string result;
        for (std::size_t index = 1; index < value.size(); ++index) {
            const char character = value[index];
            if (character == '\'') {
                if (index + 1 < value.size() && value[index + 1] == '\'') {
                    result += '\'';
                    ++index;
                    continue;
                }
                if (!isCommentTail(value.substr(index + 1)))
                    return MaiError{MaiErrorCode::InvalidInput, "text after quoted skill scalar"};
                return result;
            }
            result += character;
        }
        return MaiError{MaiErrorCode::InvalidInput, "unterminated quoted skill scalar"};
    }
    if (std::string("[{}]&*!%@`|").find(value.front()) != std::string::npos || value.front() == '>')
        return unsupported("containers, anchors, tags or scalar indicator");
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '#' &&
            (index == 0 || std::isspace(static_cast<unsigned char>(value[index - 1])))) {
            value.resize(index);
            break;
        }
    }
    value = trim(value);
    if (value == "null" || value == "Null" || value == "NULL" || value == "~") return std::string{};
    // serde_yaml 的 String 字段不把布尔值或数字隐式当作文本。
    if (value == "true" || value == "false" || value == "True" || value == "False" ||
        (!value.empty() &&
         (std::isdigit(static_cast<unsigned char>(value.front())) || value.front() == '+' ||
          value.front() == '-') &&
         value.find_first_not_of("0123456789+-.eE") == std::string::npos))
        return MaiError{MaiErrorCode::InvalidInput, "skill metadata must be a string scalar"};
    return value;
}

MaiResult<MaiSkillMetadata> parseMetadata(const std::string& frontmatter, const MaiFilePath& path) {
    if (!validUtf8(frontmatter))
        return {MaiErrorCode::InvalidInput, "skill frontmatter is not valid UTF-8 text"};
    std::vector<std::string> lines;
    std::istringstream stream(frontmatter);
    for (std::string line; std::getline(stream, line);) lines.push_back(line);
    std::map<std::string, std::string> fields;
    for (std::size_t index = 1; index + 1 < lines.size(); ++index) {
        const auto line = trim(lines[index]);
        if (line.empty() || line.front() == '#') continue;
        if (lines[index].front() == ' ' || lines[index].front() == '\t')
            return unsupported("nested mapping or multiline plain scalar");
        const auto separator = line.find(':');
        if (separator == std::string::npos || separator == 0)
            return {MaiErrorCode::InvalidInput, "expected a top-level skill frontmatter key"};
        const auto key = trim(line.substr(0, separator));
        if (key.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
            return unsupported("quoted or non-scalar mapping key");
        if (fields.count(key))
            return {MaiErrorCode::InvalidInput, "duplicate skill frontmatter key: " + key};
        const auto raw = trim(line.substr(separator + 1));
        if (key != "name" && key != "description") {
            // Codex 忽略未知字段。可选 metadata 常含映射和列表；本功能不消费其中任何能力。
            fields.emplace(key, std::string{});
            while (index + 1 < lines.size() - 1) {
                const auto& next = lines[index + 1];
                if (!trim(next).empty() && next.front() != ' ' && next.front() != '\t' &&
                    trim(next).front() != '#')
                    break;
                ++index;
            }
            continue;
        }
        std::string value;
        if (!raw.empty() && (raw.front() == '|' || raw.front() == '>')) {
            const auto indicatorEnd = raw.find_first_of(" \t#");
            const auto indicator = raw.substr(0, indicatorEnd);
            if (indicator != "|" && indicator != "|-" && indicator != "|+" && indicator != ">" &&
                indicator != ">-" && indicator != ">+")
                return unsupported("block scalar indentation indicator");
            if (indicatorEnd != std::string::npos && !isCommentTail(raw.substr(indicatorEnd)))
                return {MaiErrorCode::InvalidInput, "invalid block scalar suffix"};
            std::size_t indentation = 0;
            while (index + 1 < lines.size() - 1) {
                const auto& next = lines[index + 1];
                if (trim(next).empty()) {
                    ++index;
                    value += '\n';
                    continue;
                }
                const auto first = next.find_first_not_of(' ');
                if (first == 0 || first == std::string::npos) break;
                if (indentation == 0) indentation = first;
                if (first < indentation)
                    return {MaiErrorCode::InvalidInput, "inconsistent skill block indentation"};
                if (!value.empty()) value += raw.front() == '>' ? ' ' : '\n';
                value += next.substr(indentation);
                ++index;
            }
        } else {
            const auto parsed = parseScalar(raw);
            if (!parsed) return parsed.error();
            value = parsed.value();
            if (raw.empty() || (raw.front() != '\'' && raw.front() != '"')) {
                while (index + 1 < lines.size() - 1) {
                    const auto& next = lines[index + 1];
                    const auto continuation = trim(next);
                    if (continuation.empty()) {
                        ++index;
                        continue;
                    }
                    if (next.front() != ' ' && next.front() != '\t') break;
                    if (continuation.front() == '#') {
                        ++index;
                        continue;
                    }
                    if (raw.empty() && continuation.find(": ") != std::string::npos)
                        return unsupported("name or description must be a string, not a mapping");
                    const auto parsedLine = parseScalar(continuation);
                    if (!parsedLine) return parsedLine.error();
                    if (!value.empty()) value += ' ';
                    value += parsedLine.value();
                    ++index;
                }
            }
        }
        fields.emplace(key, std::move(value));
    }
    MaiSkillMetadata metadata;
    metadata.path = path;
    metadata.id = path.toGenericUtf8();
    metadata.name = singleLine(fields["name"]);
    if (metadata.name.empty()) metadata.name = path.dirName().baseName().toUtf8();
    metadata.description = singleLine(fields["description"]);
    if (!validUtf8(metadata.name) || !validUtf8(metadata.id) || !validUtf8(metadata.description))
        return {MaiErrorCode::InvalidInput,
                "skill metadata and catalog path must be valid UTF-8 text"};
    if (metadata.name.empty() || codePointCount(metadata.name) > 64)
        return {MaiErrorCode::InvalidInput, "skill name must contain 1..64 characters"};
    if (metadata.description.empty())
        return {MaiErrorCode::InvalidInput, "skill frontmatter is missing description"};
    return metadata;
}

// 每行读到闭合分隔符即停，目录发现不预读正文。预算包括两个分隔符。
MaiResult<std::string> readFrontmatter(MaiFileReadHandle& handle, std::size_t budget) {
    std::string result;
    std::string line;
    std::uint64_t offset = 0;
    bool opened = false;
    while (result.size() < budget) {
        std::string block;
        bool eof = false;
        const auto error =
            handle.readAt(offset, std::min<std::size_t>(256, budget - result.size()), block, &eof);
        if (error) return error;
        offset += block.size();
        for (const char character : block) {
            result += character;
            if (character != '\n') {
                line += character;
                continue;
            }
            if (!opened) {
                if (trim(line) != "---")
                    return {MaiErrorCode::InvalidInput, "skill is missing YAML frontmatter"};
                opened = true;
            } else if (trim(line) == "---") {
                return result;
            }
            line.clear();
        }
        if (eof || block.empty()) {
            if (opened && trim(line) == "---") return result;
            return {MaiErrorCode::InvalidInput, "skill frontmatter has no closing delimiter"};
        }
    }
    return {MaiErrorCode::InvalidInput, "skill frontmatter exceeds maxFrontmatterBytes"};
}

std::string quoted(const std::string& text) {
    std::string result = "\"";
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') result += '\\';
        if (character == '\n')
            result += "\\n";
        else if (character == '\r')
            result += "\\r";
        else if (character == '\t')
            result += "\\t";
        else if (character < 0x20)
            result += '?';
        else
            result += static_cast<char>(character);
    }
    return result + '"';
}

bool mentionCharacter(unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == ':';
}

bool commonEnvironmentName(std::string name) {
    for (auto& character : name)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    static const std::set<std::string> names{"PATH", "HOME",   "USER",           "SHELL",
                                             "PWD",  "TMPDIR", "TEMP",           "TMP",
                                             "LANG", "TERM",   "XDG_CONFIG_HOME"};
    return names.count(name) != 0;
}
}  // namespace

struct MaiSkillCatalog::Data {
    std::uint64_t owner = 0;
    std::size_t maxBodyBytes = 0;
    std::vector<MaiSkillMetadata> skills;
    std::vector<MaiSkillDiscoveryError> errors;
    std::vector<MaiDiscoveredSkill> entries;
};

struct MaiSkillManager::State {
    struct CacheEntry {
        MaiRegularFileInfo fileInfo;
        MaiSkillMetadata metadata;
        std::size_t frontmatterBudget = 0;
        MaiError metadataError;
    };
    std::uint64_t owner = nextOwner.fetch_add(1);
    std::mutex mutex;
    std::map<MaiFilePath::StringType, CacheEntry> cache;
};

const std::vector<MaiSkillMetadata>& MaiSkillCatalog::skills() const {
    static const std::vector<MaiSkillMetadata> empty;
    return mData ? mData->skills : empty;
}

const std::vector<MaiSkillDiscoveryError>& MaiSkillCatalog::errors() const {
    static const std::vector<MaiSkillDiscoveryError> empty;
    return mData ? mData->errors : empty;
}

MaiSkillManager::MaiSkillManager() : mState(std::make_unique<State>()) {}
MaiSkillManager::~MaiSkillManager() = default;

MaiResult<MaiSkillMetadata> MaiSkillManager::validateMetadata(const std::string& contents,
                                                              const MaiFilePath& skillPath,
                                                              std::size_t maxFrontmatterBytes,
                                                              std::size_t maxBodyBytes) {
    if (maxFrontmatterBytes < 8 || maxFrontmatterBytes > 1024 * 1024 || maxBodyBytes == 0 ||
        maxBodyBytes > 4 * 1024 * 1024 || contents.size() > maxBodyBytes)
        return {MaiErrorCode::InvalidInput, "skill contents exceed configured body budget"};
    if (!validUtf8(contents))
        return {MaiErrorCode::InvalidInput, "SKILL.md is not valid UTF-8 text"};
    std::istringstream stream(contents);
    std::size_t end = 0;
    bool opened = false;
    for (std::string line; std::getline(stream, line);) {
        end += line.size();
        if (end < contents.size()) ++end;
        if (end > maxFrontmatterBytes)
            return {MaiErrorCode::InvalidInput, "skill frontmatter exceeds maxFrontmatterBytes"};
        if (!opened) {
            if (trim(line) != "---")
                return {MaiErrorCode::InvalidInput, "skill is missing YAML frontmatter"};
            opened = true;
        } else if (trim(line) == "---") {
            return parseMetadata(contents.substr(0, end), skillPath);
        }
    }
    return {MaiErrorCode::InvalidInput, "skill frontmatter has no closing delimiter"};
}

MaiResult<MaiFilePath> MaiSkillManager::defaultHomeDirectory() {
#if defined(_WIN32)
    const auto required = ::GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (required != 0) {
        std::wstring buffer(required, L'\0');
        const auto copied = ::GetEnvironmentVariableW(L"USERPROFILE", buffer.data(), required);
        if (copied != 0 && copied < required) {
            buffer.resize(copied);
            MaiFilePath path(std::move(buffer));
            if (path.isAbsolute()) return path;
        }
    }
#else
    const char* home = std::getenv("HOME");
    if (home && *home) {
        MaiFilePath path{std::string(home)};
        if (path.isAbsolute()) return path;
    }
    std::vector<char> buffer(64 * 1024);
    struct passwd account{};
    struct passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &account, buffer.data(), buffer.size(), &result) == 0 && result &&
        account.pw_dir && *account.pw_dir) {
        MaiFilePath path{std::string(account.pw_dir)};
        if (path.isAbsolute()) return path;
    }
#endif
    return {MaiErrorCode::NotConfigured,
            "Cannot determine the native user home for skill discovery."};
}

MaiResult<MaiSkillCatalog> MaiSkillManager::snapshot(const MaiSkillManagerOptions& options) {
    if (options.maxFrontmatterBytes < 8 || options.maxFrontmatterBytes > kMaximumReadBytes ||
        options.maxBodyBytes == 0 || options.maxBodyBytes > 4 * kMaximumReadBytes ||
        options.maxDiscoveredSkills == 0 || options.maxDiscoveredSkills > 20000)
        return {MaiErrorCode::InvalidInput, "invalid skill discovery or read budgets"};
    auto data = std::make_shared<MaiSkillCatalog::Data>();
    data->owner = mState->owner;
    data->maxBodyBytes = options.maxBodyBytes;
    std::vector<std::pair<MaiFilePath, MaiSkillScope>> roots;
    const auto addError = [&](const MaiFilePath& path, const MaiError& error) {
        data->errors.push_back({path, error});
    };
    if (!options.workspaceDirectory.isEmpty()) {
        if (!options.workspaceDirectory.isAbsolute())
            return {MaiErrorCode::InvalidInput, "skill workspace must be an absolute directory"};
        const auto workspace = MaiFileSystem::resolve(options.workspaceDirectory);
        if (workspace.isEmpty() || !MaiFileSystem::isDirectory(workspace))
            return {MaiErrorCode::NotFound, "skill workspace directory does not exist"};
        std::vector<MaiFilePath> ancestors;
        bool foundRepository = false;
        for (auto directory = workspace;;) {
            ancestors.push_back(directory);
            const auto marker = MaiFileSystem::stat(child(directory, ".git"));
            if (marker) {
                foundRepository = true;
                break;
            }
            if (marker.error().code() != MaiErrorCode::NotFound) {
                addError(directory, marker.error());
                // 无法确定近处边界时不能扩大到父仓库。
                foundRepository = true;
                break;
            }
            const auto parent = directory.dirName();
            if (parent == directory || parent.isEmpty()) break;
            directory = parent;
        }
        if (!foundRepository) ancestors = {workspace};
        std::reverse(ancestors.begin(), ancestors.end());
        for (const auto& directory : ancestors)
            roots.emplace_back(child(directory, ".agents/skills"), MaiSkillScope::Workspace);
    }
    if (!options.homeDirectory.isEmpty()) {
        if (!options.homeDirectory.isAbsolute())
            return {MaiErrorCode::InvalidInput, "skill home must be an absolute directory"};
        roots.emplace_back(child(options.homeDirectory, ".agents/skills"), MaiSkillScope::User);
    }
    for (const auto& root : options.additionalRoots) {
        if (!root.isAbsolute())
            return {MaiErrorCode::InvalidInput, "additional skill root must be absolute"};
        roots.emplace_back(root, MaiSkillScope::Additional);
    }
    std::set<MaiFilePath::StringType> seenRoots;
    std::set<MaiFilePath::StringType> seenFiles;
    std::size_t candidates = 0;
    for (const auto& root : roots) {
        const auto canonicalRoot = MaiFileSystem::resolve(root.first);
        if (canonicalRoot.isEmpty()) {
            addError(root.first, {MaiErrorCode::InvalidInput, "cannot resolve skill root"});
            continue;
        }
        const auto rootInfo = MaiFileSystem::stat(canonicalRoot);
        if (!rootInfo) {
            if (rootInfo.error().code() != MaiErrorCode::NotFound)
                addError(root.first, rootInfo.error());
            continue;
        }
        if (!rootInfo.value().isDirectory) {
            addError(root.first, {MaiErrorCode::InvalidInput, "skill root is not a directory"});
            continue;
        }
        if (!seenRoots.insert(pathKey(canonicalRoot)).second) continue;
        std::vector<std::pair<MaiFilePath, std::size_t>> pending{{canonicalRoot, 0}};
        std::set<MaiFilePath::StringType> seenDirectories;
        std::size_t entries = 0;
        bool limited = false;
        while (!pending.empty() && !limited) {
            const auto directory = pending.back();
            pending.pop_back();
            const auto canonical = MaiFileSystem::resolve(directory.first);
            if (canonical.isEmpty() || !seenDirectories.insert(pathKey(canonical)).second) continue;
            if (seenDirectories.size() > kMaxDirectoriesPerRoot) {
                limited = true;
                break;
            }
            std::vector<MaiFileEntry> children;
            MaiFileSystem::walk(
                directory.first,
                [&](const MaiFileEntry& entry) {
                    if (++entries > kMaxEntriesPerRoot) {
                        limited = true;
                        return MaiWalkAction::Stop;
                    }
                    children.push_back(entry);
                    return MaiWalkAction::SkipDirectory;
                },
                addError);
            std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
                return left.path.value() < right.path.value();
            });
            for (const auto& entry : children) {
                // walk 本身不跟随链接；这里只显式跟随目录，并用真实路径集合防止环。
                if (entry.isDirectory || MaiFileSystem::isSymbolicLink(entry.path)) {
                    if (MaiFileSystem::isDirectory(entry.path)) {
                        if (!entry.nameUtf8.empty() && entry.nameUtf8.front() == '.') continue;
                        if (directory.second < kMaxScanDepth)
                            pending.emplace_back(entry.path, directory.second + 1);
                        else
                            addError(entry.path, {MaiErrorCode::InvalidInput,
                                                  "skill scan reached maximum directory depth"});
                        continue;
                    }
                }
                if (entry.nameUtf8 != "SKILL.md") continue;
                const auto file = MaiFileSystem::resolve(entry.path);
                if (file.isEmpty() || !seenFiles.insert(pathKey(file)).second) continue;
                if (++candidates > options.maxDiscoveredSkills) {
                    limited = true;
                    break;
                }
                auto opened = MaiFileReadHandle::open(file);
                if (!opened) {
                    addError(entry.path, opened.error());
                    continue;
                }
                auto info = opened.value()->snapshot();
                if (!info) {
                    addError(entry.path, info.error());
                    continue;
                }
                State::CacheEntry cached;
                bool reused = false;
                {
                    std::lock_guard<std::mutex> lock(mState->mutex);
                    const auto found = mState->cache.find(pathKey(file));
                    if (found != mState->cache.end() &&
                        sameFile(found->second.fileInfo, info.value()) &&
                        found->second.frontmatterBudget == options.maxFrontmatterBytes) {
                        cached = found->second;
                        reused = true;
                    }
                }
                if (!reused) {
                    cached.fileInfo = info.value();
                    cached.frontmatterBudget = options.maxFrontmatterBytes;
                    const auto frontmatter =
                        readFrontmatter(*opened.value(), options.maxFrontmatterBytes);
                    if (!frontmatter)
                        cached.metadataError = frontmatter.error();
                    else {
                        const auto metadata = parseMetadata(frontmatter.value(), file);
                        if (!metadata)
                            cached.metadataError = metadata.error();
                        else
                            cached.metadata = metadata.value();
                    }
                }
                const auto validated = opened.value()->validate(info.value());
                if (validated) {
                    addError(entry.path, validated);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(mState->mutex);
                    if (mState->cache.size() >= 512) mState->cache.clear();
                    mState->cache[pathKey(file)] = cached;
                }
                if (cached.metadataError) {
                    addError(entry.path, cached.metadataError);
                    continue;
                }
                cached.metadata.scope = root.second;
                // 策略独立于 SKILL.md 的缓存身份，每次发现都重新验证，避免仅修改策略时失效。
                const auto policy = readImplicitPolicy(file.dirName());
                cached.metadata.allowImplicitInvocation = policy && policy.value();
                if (!policy) addError(child(file.dirName(), "agents/openai.yaml"), policy.error());
                data->entries.push_back({cached.metadata, entry.path, info.value()});
            }
        }
        if (limited)
            addError(root.first, {MaiErrorCode::InvalidInput,
                                  "skill scan reached its directory, entry or skill count budget"});
    }
    if (options.includeBuiltinSkills) {
        for (const auto& resource : maiBuiltinSkillResources()) {
            const std::string name(resource.name);
            const auto path = MaiFilePath::fromUtf8("builtin-skills/" + name + "/SKILL.md");
            if (data->entries.size() >= options.maxDiscoveredSkills) {
                addError(path, {MaiErrorCode::InvalidInput,
                                "builtin skills reached the skill count budget"});
                break;
            }
            // 只解析前置元数据；完整编译正文与磁盘技能一样只在 read 时返回。
            auto delimiter = resource.contents.find("\n---\n", 4);
            std::size_t delimiterBytes = 5;
            if (delimiter == std::string_view::npos) {
                delimiter = resource.contents.find("\n---\r\n", 4);
                delimiterBytes = 6;
            }
            if (delimiter == std::string_view::npos ||
                delimiter + delimiterBytes > options.maxFrontmatterBytes) {
                addError(path, {MaiErrorCode::InvalidInput,
                                "builtin skill frontmatter exceeds configured budget"});
                continue;
            }
            const auto parsed = parseMetadata(
                std::string(resource.contents.substr(0, delimiter + delimiterBytes)), path);
            if (!parsed || parsed.value().name != resource.name ||
                parsed.value().description != resource.description) {
                addError(path, {MaiErrorCode::InvalidInput, "invalid builtin skill metadata"});
                continue;
            }
            auto metadata = parsed.value();
            metadata.id = "builtin:" + name;
            metadata.scope = MaiSkillScope::Builtin;
            data->entries.push_back({std::move(metadata), path, {}, &resource});
        }
    }
    std::sort(data->entries.begin(), data->entries.end(), [](const auto& left, const auto& right) {
        if (left.metadata.scope != right.metadata.scope)
            return left.metadata.scope < right.metadata.scope;
        if (left.metadata.name != right.metadata.name)
            return left.metadata.name < right.metadata.name;
        return left.metadata.path.value() < right.metadata.path.value();
    });
    for (const auto& entry : data->entries) data->skills.push_back(entry.metadata);
    MaiSkillCatalog catalog;
    catalog.mData = std::move(data);
    return catalog;
}

MaiResult<MaiSkillMetadata> MaiSkillManager::find(const MaiSkillCatalog& catalog,
                                                  const std::string& idOrName) const {
    if (!catalog.mData || catalog.mData->owner != mState->owner)
        return {MaiErrorCode::InvalidInput, "skill catalog does not belong to this manager"};
    if (idOrName.empty())
        return {MaiErrorCode::InvalidInput, "skill name or catalog id is required"};
    const MaiDiscoveredSkill* matched = nullptr;
    for (const auto& entry : catalog.mData->entries) {
        if (entry.metadata.id == idOrName || entry.discoveryPath.toGenericUtf8() == idOrName)
            return entry.metadata;
        if (entry.metadata.name == idOrName) {
            if (matched)
                return {MaiErrorCode::InvalidInput,
                        "skill name is ambiguous; use an exact id from the skill catalog"};
            matched = &entry;
        }
    }
    if (matched) return matched->metadata;
    return {MaiErrorCode::NotFound, "skill is not present in this catalog"};
}

MaiResult<MaiSkillContents> MaiSkillManager::read(const MaiSkillCatalog& catalog,
                                                  const std::string& idOrName) {
    return readImpl(catalog, idOrName, false);
}

MaiResult<MaiSkillContents> MaiSkillManager::readCurrent(const MaiSkillCatalog& catalog,
                                                         const std::string& idOrName) {
    return readImpl(catalog, idOrName, true);
}

MaiResult<MaiSkillContents> MaiSkillManager::readImpl(const MaiSkillCatalog& catalog,
                                                      const std::string& idOrName,
                                                      bool currentSource) {
    const auto metadata = find(catalog, idOrName);
    if (!metadata) return metadata.error();
    const auto found =
        std::find_if(catalog.mData->entries.begin(), catalog.mData->entries.end(),
                     [&](const auto& entry) { return entry.metadata.id == metadata.value().id; });
    if (found == catalog.mData->entries.end())
        return {MaiErrorCode::NotFound, "skill no longer belongs to this catalog"};
    const auto& entry = *found;
    if (entry.builtin) {
        if (entry.builtin->contents.size() > catalog.mData->maxBodyBytes)
            return {MaiErrorCode::InvalidInput, "complete SKILL.md exceeds maxBodyBytes"};
        std::string contents(entry.builtin->contents);
        if (!validUtf8(contents))
            return {MaiErrorCode::InvalidInput, "builtin SKILL.md is not valid UTF-8 text"};
        return MaiSkillContents{entry.metadata, std::move(contents)};
    }
    if (pathKey(MaiFileSystem::resolve(entry.discoveryPath)) != pathKey(entry.metadata.path))
        return {MaiErrorCode::InvalidInput, "skill path changed since discovery; refresh catalog"};
    auto opened = MaiFileReadHandle::open(entry.metadata.path);
    if (!opened) return opened.error();
    auto expected = entry.fileInfo;
    if (currentSource) {
        const auto current = opened.value()->snapshot();
        if (!current) return current.error();
        expected = current.value();
    }
    const auto validated = opened.value()->validate(expected);
    if (validated)
        return {MaiErrorCode::InvalidInput, "skill file changed since discovery; refresh catalog"};
    if (expected.bytes > catalog.mData->maxBodyBytes)
        return {MaiErrorCode::InvalidInput, "complete SKILL.md exceeds maxBodyBytes"};
    std::string contents;
    // 只缓存元数据，正文每次按需读取，避免跨会话缓存大量技能指令或保留旧正文。
    {
        bool eof = false;
        while (!eof && contents.size() <= catalog.mData->maxBodyBytes) {
            std::string block;
            const auto remaining = catalog.mData->maxBodyBytes - contents.size() + 1;
            const auto error = opened.value()->readAt(
                contents.size(), std::min<std::size_t>(remaining, kMaximumReadBytes), block, &eof);
            if (error) return error;
            contents += block;
            if (block.empty()) break;
        }
        if (contents.size() > catalog.mData->maxBodyBytes)
            return {MaiErrorCode::InvalidInput, "complete SKILL.md exceeds maxBodyBytes"};
        if (!validUtf8(contents))
            return {MaiErrorCode::InvalidInput, "SKILL.md is not valid UTF-8 text"};
    }
    const auto unchanged = opened.value()->validate(expected);
    if (unchanged) return unchanged;
    if (pathKey(MaiFileSystem::resolve(entry.discoveryPath)) != pathKey(entry.metadata.path))
        return {MaiErrorCode::InvalidInput, "skill path changed while reading; refresh catalog"};
    return MaiSkillContents{entry.metadata, std::move(contents)};
}

MaiResult<MaiSkillDiscoveryPage> MaiSkillManager::discover(const MaiSkillCatalog& catalog,
                                                           const std::string& query,
                                                           std::size_t offset,
                                                           std::size_t limit) const {
    if (!catalog.mData || catalog.mData->owner != mState->owner)
        return {MaiErrorCode::InvalidInput, "skill catalog does not belong to this manager"};
    if (query.size() > 4096 || !validUtf8(query) || limit == 0 || limit > 50)
        return {MaiErrorCode::InvalidInput, "invalid skill metadata query or page limit"};
    const auto needle = lowerAscii(singleLine(query));
    MaiSkillDiscoveryPage page;
    std::size_t pageBytes = 256;
    bool full = false;
    for (const auto& metadata : catalog.skills()) {
        if (!needle.empty() && lowerAscii(metadata.name).find(needle) == std::string::npos &&
            lowerAscii(metadata.description).find(needle) == std::string::npos &&
            lowerAscii(metadata.id).find(needle) == std::string::npos)
            continue;
        const auto position = page.total++;
        if (position < offset || full) continue;
        auto result = metadata;
        result.description = shortenDescription(result.description, 1024);
        // JSON 最坏转义按 6 倍计算；页上限包含固定字段，避免元数据工具响应无界。
        const auto bytes =
            (result.id.size() + result.name.size() + result.description.size()) * 6 + 256;
        if (bytes > 64 * 1024 - pageBytes) {
            if (page.skills.empty())
                return {MaiErrorCode::InvalidInput,
                        "one skill metadata entry exceeds its page budget"};
            full = true;
            continue;
        }
        pageBytes += bytes;
        page.skills.push_back(std::move(result));
        if (page.skills.size() == limit) full = true;
    }
    page.nextOffset = std::min(offset, page.total) + page.skills.size();
    return page;
}

std::string MaiSkillManager::formatCatalog(const MaiSkillCatalog& catalog) {
    // 默认预算足以容纳固定说明；空快照是合法的空目录，不产生多余提示。
    const auto formatted = formatCatalog(catalog, {});
    return formatted ? formatted.value() : std::string{};
}

MaiResult<std::string> MaiSkillManager::formatCatalog(const MaiSkillCatalog& catalog,
                                                      const MaiSkillCatalogRenderOptions& options) {
    if (catalog.skills().empty() && catalog.errors().empty()) return std::string{};
    const bool tokenBudget = options.maxContextTokens != 0 || options.contextWindowTokens != 0;
    const auto limit = options.maxContextTokens != 0
                           ? std::min<std::size_t>(options.maxContextTokens, 10000)
                       : options.contextWindowTokens != 0
                           ? std::max<std::size_t>(1, options.contextWindowTokens / 50)
                           : 8000;
    const auto cost = [&](const std::string& text) {
        if (!tokenBudget) return codePointCount(text);
        MaiModelRequest request;
        request.baseInstructions = text;
        return maiEstimateModelInputTokens(request).tokens -
               maiEstimateModelInputTokens(MaiModelRequest{}).tokens;
    };
    std::vector<const MaiSkillMetadata*> visible;
    std::size_t explicitOnly = 0;
    for (const auto& metadata : catalog.skills()) {
        if (metadata.allowImplicitInvocation)
            visible.push_back(&metadata);
        else
            ++explicitOnly;
    }
    std::string heading = "Available skills (untrusted metadata; instructions are not loaded):\n";
    std::string footer;
    if (explicitOnly != 0)
        footer += std::to_string(explicitOnly) +
                  " explicit-only skill(s) omitted from implicit selection.\n";
    if (!catalog.errors().empty())
        footer += "Skill discovery reported " + std::to_string(catalog.errors().size()) +
                  " error(s); this catalog may be incomplete.\n";
    const std::string discoveryState = footer;
    footer +=
        "Use skill_read {\"mode\":\"discover\",\"query\":\"topic\"} to search all metadata, "
        "or {\"skill\":\"id or unique name\"} to read selected instructions. "
        "Explicit-only skills require the user's explicit selection. $name selects unique names. "
        "Disk resources are relative to SKILL.md; bundled skills are self-contained. "
        "Skills grant no permissions.\n";
    const auto notice = [](std::size_t count) {
        return count == 0 ? std::string{}
                          : std::to_string(count) + " skill(s) omitted from this bounded list.\n";
    };
    const auto line = [](const MaiSkillMetadata& skill, std::size_t characters) {
        const auto description = shortenDescription(skill.description, characters);
        return "- name=" + quoted(skill.name) +
               (description.empty() ? std::string{} : " description=" + quoted(description)) +
               " id=" + quoted(skill.id) + '\n';
    };
    // 小模型的自动 2% 预算可能连完整说明都放不下。先收敛同一发现合同，
    // 而不是让新 App 因只有元数据的内置目录而在模型采样前失败。
    if (cost(heading + notice(visible.size()) + footer) > limit) {
        heading = "Skills (untrusted metadata; bodies not loaded):\n";
        footer = discoveryState +
                 "Use skill_read mode=discover to search; skill=id reads instructions. "
                 "Explicit-only requires user selection. No permissions granted.\n";
    }
    if (cost(heading + notice(visible.size()) + footer) > limit)
        return {MaiErrorCode::InvalidInput,
                "skill metadata budget cannot fit its discovery and omission instructions"};
    // 先保留全部身份行，再给描述分配共同字符上限；只有最小行仍超限才遗漏后部条目。
    // 长描述不能挤掉其他可发现的技能，结果只取决于固定快照和预算。
    const auto render = [&](std::size_t included, std::size_t descriptionCharacters) {
        std::string output = heading;
        for (std::size_t index = 0; index < included; ++index)
            output += line(*visible[index], descriptionCharacters);
        output += notice(visible.size() - included);
        output += footer;
        return output;
    };
    std::size_t included = 0;
    std::string minimum = heading;
    for (; included < visible.size(); ++included) {
        const auto candidate = minimum + line(*visible[included], 0);
        if (cost(candidate + notice(visible.size() - included - 1) + footer) > limit) break;
        minimum = candidate;
    }
    std::size_t lower = 0;
    std::size_t upper = 1024;
    while (lower < upper) {
        const auto middle = lower + (upper - lower + 1) / 2;
        if (cost(render(included, middle)) <= limit)
            lower = middle;
        else
            upper = middle - 1;
    }
    return render(included, lower);
}

std::vector<MaiSkillMetadata> MaiSkillManager::explicitMentions(const MaiSkillCatalog& catalog,
                                                                const std::string& text) {
    std::set<std::string> mentions;
    std::set<std::string> paths;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '$') continue;
        const auto start = ++index;
        while (index < text.size() && mentionCharacter(static_cast<unsigned char>(text[index])))
            ++index;
        const auto name = text.substr(start, index - start);
        if (!name.empty() && !commonEnvironmentName(name)) {
            bool linked = false;
            if (start >= 2 && text[start - 2] == '[' && index < text.size() && text[index] == ']') {
                auto position = index + 1;
                while (position < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[position])))
                    ++position;
                if (position < text.size() && text[position] == '(') {
                    const auto closing = text.find(')', position + 1);
                    if (closing != std::string::npos) {
                        auto path = trim(text.substr(position + 1, closing - position - 1));
                        if (!path.empty()) {
                            if (path.compare(0, 8, "skill://") == 0) path.erase(0, 8);
                            paths.insert(std::move(path));
                            linked = true;
                            index = closing + 1;
                        }
                    }
                }
            }
            if (!linked) mentions.insert(name);
        }
        if (index > 0) --index;
    }
    std::map<std::string, std::size_t> counts;
    for (const auto& skill : catalog.skills()) ++counts[skill.name];
    std::vector<MaiSkillMetadata> result;
    for (const auto& skill : catalog.skills()) {
        bool exactPath = paths.count(skill.id) != 0;
        if (!exactPath && catalog.mData) {
            const auto entry =
                std::find_if(catalog.mData->entries.begin(), catalog.mData->entries.end(),
                             [&](const auto& item) { return item.metadata.id == skill.id; });
            exactPath = entry != catalog.mData->entries.end() &&
                        paths.count(entry->discoveryPath.toGenericUtf8()) != 0;
        }
        if (exactPath || (counts[skill.name] == 1 && mentions.count(skill.name)))
            result.push_back(skill);
    }
    return result;
}

MaiResult<std::string> MaiSkillManager::explicitContext(const MaiSkillCatalog& catalog,
                                                        const std::string& text,
                                                        std::size_t maxBytes) {
    if (maxBytes == 0 || maxBytes > 4 * kMaximumReadBytes)
        return {MaiErrorCode::InvalidInput, "Invalid selected skill context byte budget."};
    const auto selected = explicitMentions(catalog, text);
    if (selected.empty()) return std::string{};
    nlohmann::json context{{"type", "selected_skill_context"},
                           {"trust", "untrusted_user_context"},
                           {"permission", "does_not_grant_permissions"},
                           {"skills", nlohmann::json::array()}};
    std::size_t bytes = context.dump().size();
    for (const auto& metadata : selected) {
        const auto contents = readCurrent(catalog, metadata.id);
        if (!contents) return contents.error();
        nlohmann::json entry{{"name", metadata.name},
                             {"id", metadata.id},
                             {"resource_directory", metadata.scope == MaiSkillScope::Builtin
                                                        ? std::string{}
                                                        : metadata.path.dirName().toGenericUtf8()},
                             {"contents", contents.value().contents}};
        // 字符串按 JSON 引用，正文中的标签/分隔符不会伪装成宿主创建的新上下文边界。
        const auto added = entry.dump().size() + (context["skills"].empty() ? 0 : 1);
        if (bytes > maxBytes || added > maxBytes - bytes)
            return {MaiErrorCode::InvalidInput, "Selected skill context exceeds its byte budget."};
        bytes += added;
        context["skills"].push_back(std::move(entry));
    }
    return context.dump();
}
