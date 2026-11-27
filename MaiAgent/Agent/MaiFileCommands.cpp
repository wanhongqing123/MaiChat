// 文件作用：把受限文件命令映射到普通原生文件 API；不启动解释器或子进程。
#include "MaiFileCommands.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiAppendFile.h"
#include "MaiFileSearch.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"
#include "MaiTextFileCursorCache.h"
#include "MaiTextFileReader.h"

namespace {

constexpr std::size_t kMaximumBudget = 16 * 1024 * 1024;
constexpr std::size_t kMaximumArguments = 256;
using Json = nlohmann::json;

std::string bindingPart(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

bool parseUnsigned(const std::string& text, std::uint64_t maximum, std::uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9' || static_cast<std::uint64_t>(digit - '0') > maximum ||
            value > (maximum - static_cast<std::uint64_t>(digit - '0')) / 10)
            return false;
        value = value * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    return true;
}

const char* stopName(MaiTextFileStopReason reason) {
    switch (reason) {
        case MaiTextFileStopReason::EndOfFile: return "eof";
        case MaiTextFileStopReason::LineLimit: return "line_limit";
        case MaiTextFileStopReason::OutputLimit: return "output_limit";
        case MaiTextFileStopReason::ScanLimit: return "scan_limit";
        case MaiTextFileStopReason::TimeLimit: return "time_limit";
    }
    return "unknown";
}

// 暂存只服务本次 stdin 搜索，目录身份和边界均固定，退出时不跟随链接。
struct MaiCommandInputFile {
    MaiFilePath directory;
    MaiFilePath file;
    std::string identity;
    std::string fileIdentity;
    void identifyFile() {
        MaiRegularFileInfo info;
        if (MaiFileSystem::regularFileInfo(file, info)) fileIdentity = info.identity;
    }
    ~MaiCommandInputFile() {
        if (directory.isEmpty() || identity.empty()) return;
        MaiDirectoryDeleteOptions options;
        if (!fileIdentity.empty()) {
            MaiRegularFileInfo currentFile;
            if (!MaiFileSystem::regularFileInfo(file, currentFile) ||
                currentFile.identity != fileIdentity)
                return;
            const auto current = MaiFileSystem::stat(directory);
            if (!current || !current.value().isDirectory ||
                MaiFileSystem::resolve(directory) != directory)
                return;
            // 自己创建子文件会改变目录的修改身份；专属文件身份先证明所有权，
            // 再固定此刻的目录身份交给原生递归删除，不能拿旧空目录身份误拒绝。
            options.recursive = true;
            options.expectedIdentity = current.value().identity;
        } else
            options.expectedIdentity = identity;
        options.pathAllowed = [root = directory](const MaiFilePath& path) {
            return path == root || root.isParentOf(path);
        };
        MaiFileSystem::deleteDirectory(directory, options);
    }
};

MaiCommandExecution commandFailure(int status, const std::string& message) {
    MaiCommandExecution result;
    result.exitCode = status;
    result.standardError = message + "\n";
    return result;
}

MaiError budgetError() {
    return {MaiErrorCode::InvalidInput, "File command byte budget exceeded"};
}

bool parseCount(const std::string& text, std::size_t& count) {
    if (text.empty()) return false;
    count = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9' || count > (1000000 - (digit - '0')) / 10) return false;
        count = count * 10 + (digit - '0');
    }
    return true;
}

class MaiFileCommandState {
public:
    explicit MaiFileCommandState(MaiFileCommandOptions options);
    MaiResult<MaiCommandExecution> execute(const MaiCommandInvocation& invocation) const;
    MaiResult<std::string> read(const MaiFilePath& path) const;
    MaiError write(const MaiFilePath& path, const std::string& bytes, bool append) const;
    MaiResult<MaiFilePath> changeDirectory(const MaiFilePath& path) const;

private:
    MaiError validate() const;
    MaiResult<MaiFilePath> resolve(const MaiFilePath& path, const MaiFilePath& cwd) const;
    MaiResult<std::string> readAt(const MaiFilePath& path, const MaiFilePath& cwd) const;
    MaiResult<MaiCommandExecution> output(std::string bytes) const;
    MaiResult<MaiCommandExecution> list(const MaiFilePath& path, bool hidden) const;
    MaiResult<MaiCommandExecution> searchCommand(const MaiCommandInvocation& invocation,
                                                 const MaiFilePath& cwd) const;
    MaiResult<MaiCommandExecution> readText(const MaiCommandInvocation& invocation,
                                            const MaiFilePath& cwd) const;
    MaiResult<std::string> decodeSearchFile(const MaiFilePath& path) const;
    MaiFileSearchCallbacks searchCallbacks(bool decode) const;
    std::string searchScope() const;
    std::size_t resultLimit() const;
    MaiFileCommandOptions mOptions;
    MaiFilePath mRoot;
    MaiFilePath mWorkingDirectory;
    MaiError mConfigurationError;
};

MaiFileCommandState::MaiFileCommandState(MaiFileCommandOptions options)
    : mOptions(std::move(options)) {
    if (!mOptions.accessRoot.isAbsolute() || !mOptions.workingDirectory.isAbsolute() ||
        mOptions.maxOutputBytes == 0 || mOptions.maxOutputBytes > kMaximumBudget) {
        mConfigurationError = {MaiErrorCode::InvalidInput,
                               "File command directories and byte budget are invalid"};
        return;
    }
    mRoot = MaiFileSystem::resolve(mOptions.accessRoot);
    mWorkingDirectory = MaiFileSystem::resolve(mOptions.workingDirectory);
    if (mRoot.isEmpty() || mWorkingDirectory.isEmpty() || !MaiFileSystem::isDirectory(mRoot) ||
        !MaiFileSystem::isDirectory(mWorkingDirectory) ||
        maiResolvePathWithinRoot(mRoot.toUtf8(), mWorkingDirectory.toUtf8()).empty())
        mConfigurationError = {MaiErrorCode::InvalidInput,
                               "File command working directory is outside its container"};
    if (mOptions.scopeKey.size() > 3000 || mOptions.maxResultBytes == 0 ||
        mOptions.maxResultBytes > kMaximumBudget)
        mConfigurationError = {MaiErrorCode::InvalidInput,
                               "Invalid command scope or result budget"};
    if (!mOptions.search) mOptions.search = std::make_shared<MaiFileSearch>();
    if (!mOptions.textCursors) mOptions.textCursors = std::make_shared<MaiTextFileCursorCache>();
}

std::size_t MaiFileCommandState::resultLimit() const {
    return std::min(mOptions.maxOutputBytes, mOptions.maxResultBytes);
}

std::string MaiFileCommandState::searchScope() const {
    return bindingPart(mRoot.toGenericUtf8()) + bindingPart(mOptions.scopeKey);
}

MaiResult<std::string> MaiFileCommandState::decodeSearchFile(const MaiFilePath& path) const {
    MaiTextFileReadOptions options;
    options.limit = 10000;
    options.maxOutputBytes = std::min<std::size_t>(mOptions.maxOutputBytes, 1024 * 1024);
    options.isCanceled = mOptions.isCanceled;
    options.decodeText = mOptions.decodeText;
    std::string result;
    do {
        const auto page = MaiTextFileReader::read(path, options);
        if (!page) return page.error();
        for (const auto& line : page.value().lines) {
            const auto added = line.text.size() + (line.complete ? 1u : 0u);
            if (added > mOptions.maxOutputBytes - result.size()) return budgetError();
            result += line.text;
            if (line.complete) result += '\n';
        }
        if (page.value().eof) break;
        if (!page.value().nextCursor)
            return {MaiErrorCode::NotSupported, "Decoded search source has no safe continuation"};
        options.cursor = page.value().nextCursor;
    } while (true);
    return result;
}

MaiFileSearchCallbacks MaiFileCommandState::searchCallbacks(bool decode) const {
    MaiFileSearchCallbacks callbacks;
    callbacks.isCanceled = mOptions.isCanceled;
    callbacks.pathAllowed = [this](const MaiFilePath& path) {
        return resolve(path, mWorkingDirectory).isOk();
    };
    if (decode)
        callbacks.decodeText = [this](const MaiFilePath& path, const std::string&) {
            return decodeSearchFile(path);
        };
    return callbacks;
}

MaiError MaiFileCommandState::validate() const {
    if (mConfigurationError) return mConfigurationError;
    if (mOptions.isCanceled && mOptions.isCanceled())
        return {MaiErrorCode::Canceled, "File command canceled"};
    return {};
}

MaiResult<MaiFilePath> MaiFileCommandState::resolve(const MaiFilePath& path,
                                                    const MaiFilePath& cwd) const {
    if (const auto error = validate()) return error;
    if (path.isEmpty() || path.toUtf8().find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "A nonempty file path without NUL is required"};
    const auto absolute = path.isAbsolute() ? path : cwd.append(path);
    const auto allowed = maiResolvePathWithinRoot(mRoot.toUtf8(), absolute.toUtf8());
    if (allowed.empty())
        return {MaiErrorCode::InvalidInput, "File path is outside the command container"};
    // Windows 对不存在的末段可能只做词法 resolve，因此还须逐层检查真实存在的祖先。
    // 任一级链接均拒绝，既保护新建目标，也避免把 rm 链接误变为删除其目标。
    auto ancestor = absolute;
    bool reachedRoot = false;
    for (std::size_t depth = 0; depth < 256; ++depth) {
        if (const auto error = validate()) return error;
        if (MaiFileSystem::isSymbolicLink(ancestor))
            return {MaiErrorCode::InvalidInput, "File command symbolic links are unsupported"};
        if (MaiFileSystem::resolve(ancestor) == mRoot) {
            reachedRoot = true;
            break;
        }
        const auto parent = ancestor.dirName();
        if (parent.isEmpty() || parent == ancestor) break;
        ancestor = parent;
    }
    if (!reachedRoot)
        return {MaiErrorCode::InvalidInput,
                "File path ancestry does not reach the command container"};
    return MaiFilePath::fromUtf8(allowed);
}

MaiResult<std::string> MaiFileCommandState::readAt(const MaiFilePath& path,
                                                   const MaiFilePath& cwd) const {
    const auto allowed = resolve(path, cwd);
    if (!allowed) return allowed.error();
    MaiRegularFileInfo info;
    if (!MaiFileSystem::regularFileInfo(allowed.value(), info))
        return {MaiErrorCode::NotFound, "File command input is not an accessible ordinary file"};
    if (info.bytes > mOptions.maxOutputBytes) return budgetError();
    const auto opened = MaiFileReadHandle::open(allowed.value());
    if (!opened) return opened.error();
    std::string result;
    bool eof = false;
    while (!eof) {
        if (const auto error = validate()) return error;
        std::string block;
        const auto error = opened.value()->readAt(
            result.size(),
            std::min<std::size_t>(64 * 1024, mOptions.maxOutputBytes - result.size() + 1), block,
            &eof);
        if (error) return error;
        if (block.size() > mOptions.maxOutputBytes - result.size()) return budgetError();
        result += block;
        if (block.empty()) break;
    }
    if (const auto error = opened.value()->validate(info)) return error;
    return result;
}

MaiResult<std::string> MaiFileCommandState::read(const MaiFilePath& path) const {
    return readAt(path, mWorkingDirectory);
}

MaiError MaiFileCommandState::write(const MaiFilePath& path, const std::string& bytes,
                                    bool append) const {
    if (const auto error = validate()) return error;
    if (bytes.size() > mOptions.maxOutputBytes) return budgetError();
    const auto allowed = resolve(path, mWorkingDirectory);
    if (!allowed) return allowed.error();
    if (append) {
        // 在打开追加句柄前检查既有体积，超预算不得先创建或改动目标。
        if (MaiFileSystem::exists(allowed.value())) {
            MaiRegularFileInfo info;
            if (!MaiFileSystem::regularFileInfo(allowed.value(), info))
                return {MaiErrorCode::InvalidInput, "Append target must be an ordinary file"};
            if (info.bytes > mOptions.maxOutputBytes - bytes.size()) return budgetError();
        }
        const auto opened = MaiAppendFile::open(allowed.value());
        if (!opened) return opened.error();
        if (const auto error = opened.value()->lockWriter()) return error;
        const auto size = opened.value()->size();
        if (!size) return size.error();
        if (size.value() > mOptions.maxOutputBytes - bytes.size()) return budgetError();
        if (const auto error = validate()) return error;
        const auto written = opened.value()->append(bytes);
        return written ? MaiError{} : written.error();
    }
    if (const auto error = validate()) return error;
    return MaiFileSystem::writeFile(allowed.value(), bytes);
}

MaiResult<MaiFilePath> MaiFileCommandState::changeDirectory(const MaiFilePath& path) const {
    const auto allowed = resolve(path, mWorkingDirectory);
    if (!allowed) return allowed.error();
    if (!MaiFileSystem::isDirectory(allowed.value()))
        return {MaiErrorCode::NotFound, "Command working directory does not exist"};
    return allowed.value();
}

MaiResult<MaiCommandExecution> MaiFileCommandState::output(std::string bytes) const {
    if (const auto error = validate()) return error;
    if (bytes.size() > mOptions.maxOutputBytes) return budgetError();
    MaiCommandExecution result;
    result.standardOutput = std::move(bytes);
    return result;
}

MaiResult<MaiCommandExecution> MaiFileCommandState::list(const MaiFilePath& path,
                                                         bool hidden) const {
    if (!MaiFileSystem::isDirectory(path)) {
        MaiRegularFileInfo info;
        if (!MaiFileSystem::regularFileInfo(path, info))
            return commandFailure(1, "ls: input does not exist");
        return output(path.baseName().toUtf8() + "\n");
    }
    MaiFileSearch search;
    MaiFileNameSearchOptions options;
    options.root = path;
    options.recursive = false;
    options.query = "*";
    options.filter.includeHidden = hidden;
    options.filter.useDefaultExcludes = false;
    options.filter.respectGitIgnore = false;
    options.budget.maxOutputBytes = mOptions.maxOutputBytes;
    MaiFileSearchCallbacks callbacks;
    callbacks.isCanceled = mOptions.isCanceled;
    callbacks.pathAllowed = [this](const MaiFilePath& candidate) {
        return !maiResolvePathWithinRoot(mRoot.toUtf8(), candidate.toUtf8()).empty();
    };
    std::vector<std::string> entries;
    std::size_t bytes = 0;
    do {
        if (const auto error = validate()) return error;
        const auto page = search.findFiles(options, callbacks);
        if (page.error) return page.error;
        if (page.canceled) return {MaiErrorCode::Canceled, "Directory listing canceled"};
        if (page.partial) return budgetError();
        for (const auto& entry : page.files) {
            std::string name = entry.path + (entry.isDirectory ? "/\n" : "\n");
            if (name.size() > mOptions.maxOutputBytes - bytes) return budgetError();
            bytes += name.size();
            entries.push_back(std::move(name));
        }
        options.cursor = page.hasMore ? page.nextCursor : std::string{};
    } while (!options.cursor.empty());
    std::sort(entries.begin(), entries.end());
    std::string result;
    result.reserve(bytes);
    for (const auto& entry : entries) result += entry;
    return output(std::move(result));
}

MaiResult<MaiCommandExecution> MaiFileCommandState::readText(const MaiCommandInvocation& invocation,
                                                             const MaiFilePath& cwd) const {
    const auto& args = invocation.arguments;
    std::string encoding = "auto", cursor, path;
    std::uint64_t offset = 1, limit = 500;
    bool hasOffset = false, hasEncoding = false, hasLimit = false, hasCursor = false;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option == "--encoding" || option == "--offset" || option == "--limit" ||
            option == "--cursor") {
            if (++index >= args.size())
                return commandFailure(2, "read-text option requires a value");
            const auto& value = args[index];
            if (option == "--encoding") {
                if (hasEncoding || value.empty() || value.size() > 64)
                    return commandFailure(2, "Invalid read-text encoding");
                encoding = value;
                hasEncoding = true;
            } else if (option == "--cursor") {
                if (hasCursor || value.empty() || value.size() > 4096)
                    return MaiError{MaiErrorCode::InvalidInput, "Invalid read-text cursor"};
                cursor = value;
                hasCursor = true;
            } else {
                auto& target = option == "--offset" ? offset : limit;
                auto& supplied = option == "--offset" ? hasOffset : hasLimit;
                const auto maximum =
                    option == "--offset" ? std::uint64_t{UINT64_MAX} : std::uint64_t{10000};
                if (supplied || !parseUnsigned(value, maximum, target) || target == 0)
                    return commandFailure(
                        2, "read-text requires a positive offset and a limit from 1 to 10000");
                supplied = true;
            }
        } else if (option.empty() || option[0] == '-' || !path.empty()) {
            return commandFailure(2, "read-text requires one path and documented options");
        } else
            path = option;
    }
    if (path.empty()) return commandFailure(2, "read-text requires a file path");
    if (hasCursor && hasOffset)
        return MaiError{MaiErrorCode::InvalidInput,
                        "read-text --cursor and --offset are mutually exclusive"};
    const auto budget = resultLimit();
    if (budget < 1024) return budgetError();
    const auto allowed = resolve(MaiFilePath::fromUtf8(path), cwd);
    if (!allowed) return allowed.error();
    const auto binding =
        searchScope() + bindingPart(allowed.value().toGenericUtf8()) + bindingPart(encoding);
    MaiTextFileReadOptions options;
    options.encoding = encoding;
    options.offset = offset;
    options.limit = std::min<std::size_t>(static_cast<std::size_t>(limit),
                                          std::max<std::size_t>(1, (budget / 2 - 256) / 128));
    options.maxOutputBytes = std::min<std::size_t>(1024 * 1024, (budget - 512) / 4);
    options.maxScanBytes =
        std::min<std::size_t>(32 * 1024 * 1024, std::max<std::size_t>(4, mOptions.maxOutputBytes));
    options.isCanceled = mOptions.isCanceled;
    options.decodeText = mOptions.decodeText;
    if (hasCursor) {
        const auto saved = mOptions.textCursors->recall(cursor, binding);
        if (!saved) return saved.error();
        options.cursor = saved.value();
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto page = MaiTextFileReader::read(allowed.value(), options);
        if (!page) return page.error();
        Json value{{"type", "text_page"},
                   {"encoding", page.value().encoding},
                   {"eof", page.value().eof},
                   {"partial", !page.value().eof},
                   {"truncated", page.value().nextCursor.has_value()},
                   {"stop_reason", stopName(page.value().stopReason)},
                   {"requested_limit", limit},
                   {"applied_limit", options.limit},
                   {"next_cursor", page.value().nextCursor ? std::string(128, 'x') : std::string{}},
                   {"lines", Json::array()}};
        for (const auto& line : page.value().lines)
            value["lines"].push_back({{"number", line.number},
                                      {"byte_offset", line.byteOffset},
                                      {"text", line.text},
                                      {"complete", line.complete}});
        if (page.value().nextCursor) {
            const auto& next = *page.value().nextCursor;
            value["continuation"] = {{"byte_offset", next.byteOffset},
                                     {"line", next.lineNumber},
                                     {"line_utf8_offset", next.lineUtf8Offset},
                                     {"target_line", next.targetLine}};
        }
        // JSON 引用和每行元数据也占预算。必要时从同一可信游标重读更小的一页，
        // 不能丢弃已推进页中的行再交出一个会跳过内容的游标。
        if (value.dump().size() + 1 > budget) {
            if (options.maxOutputBytes <= 4 && options.limit == 1) return budgetError();
            options.maxOutputBytes = std::max<std::size_t>(4, options.maxOutputBytes / 2);
            options.limit = std::max<std::size_t>(1, options.limit / 2);
            continue;
        }
        if (page.value().nextCursor) {
            const auto token = mOptions.textCursors->remember(*page.value().nextCursor, binding);
            if (!token) return token.error();
            value["next_cursor"] = token.value();
        }
        MaiCommandExecution result;
        result.standardOutput = value.dump() + "\n";
        if (result.standardOutput.size() > budget) return budgetError();
        return result;
    }
    return budgetError();
}

MaiResult<MaiCommandExecution> MaiFileCommandState::searchCommand(
    const MaiCommandInvocation& invocation, const MaiFilePath& cwd) const {
    const auto& args = invocation.arguments;
    const bool find = args[0] == "find";
    std::string path, pattern, cursor;
    bool literal = false, numbered = false, insensitive = false, recursive = false;
    bool names = false, counts = false, patternSet = false, limitSet = false, cursorSet = false;
    std::uint64_t requestedLimit = 300;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option == "--limit" || option == "--cursor" || (find && option == "-name")) {
            if (++index >= args.size()) return commandFailure(2, "Search option requires a value");
            const auto& value = args[index];
            if (option == "--limit") {
                if (limitSet || !parseUnsigned(value, 10000, requestedLimit) || requestedLimit == 0)
                    return commandFailure(2, "Search --limit must be from 1 to 10000");
                limitSet = true;
            } else if (option == "--cursor") {
                if (cursorSet || value.empty() || value.size() > 4096)
                    return MaiError{MaiErrorCode::InvalidInput, "Invalid search cursor"};
                cursor = value;
                cursorSet = true;
            } else {
                if (patternSet || value.empty())
                    return commandFailure(2, "find requires one nonempty -name glob");
                pattern = value;
                patternSet = true;
            }
        } else if (!find && (option == "-F" || option == "-n" || option == "-i" || option == "-r" ||
                             option == "-l" || option == "-c")) {
            literal = literal || option == "-F";
            numbered = numbered || option == "-n";
            insensitive = insensitive || option == "-i";
            recursive = recursive || option == "-r";
            names = names || option == "-l";
            counts = counts || option == "-c";
        } else if (option.size() > 1 && option[0] == '-') {
            return commandFailure(2, "Unsupported find/grep option");
        } else if (!find && !patternSet) {
            pattern = option;
            patternSet = true;
        } else if (path.empty())
            path = option;
        else
            return commandFailure(2, "Search accepts only one input path");
    }
    if (!patternSet || (find && path.empty()) || (names && counts) || pattern.size() > 65536)
        return commandFailure(2, "Invalid search pattern, path or mutually exclusive output modes");
    const bool standardInput = !find && (path == "-" || (path.empty() && !recursive));
    if (standardInput && cursorSet)
        return MaiError{MaiErrorCode::InvalidInput,
                        "stdin search cursors cannot outlive their temporary source"};
    const auto budget = resultLimit();
    if (budget < 1024) return budgetError();
    MaiCommandInputFile temporary;
    MaiFilePath source;
    if (standardInput) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        temporary.directory = mRoot.append(MaiFilePath::fromUtf8(
            ".mai-command-stdin-" + std::to_string(tick) + "-" + std::to_string(++sequence)));
        if (const auto error = MaiFileSystem::createNewDirectory(temporary.directory)) return error;
        const auto info = MaiFileSystem::stat(temporary.directory);
        if (!info) return info.error();
        temporary.identity = info.value().identity;
        temporary.file = temporary.directory.append(MaiFilePath::fromUtf8("input.txt"));
        if (const auto error = MaiFileSystem::createEmptyFile(temporary.file)) return error;
        temporary.identifyFile();
        if (const auto error = validate()) return error;
        const auto written = MaiFileSystem::writeFile(temporary.file, invocation.standardInput);
        temporary.identifyFile();
        if (written) return written;
        source = temporary.file;
    } else {
        const auto allowed = resolve(path.empty() ? cwd : MaiFilePath::fromUtf8(path), cwd);
        if (!allowed) return allowed.error();
        source = allowed.value();
        if (!find && MaiFileSystem::isDirectory(source) && !recursive)
            return commandFailure(2, "grep directory input requires -r");
    }
    MaiFileSearchBudget work;
    work.maxOutputBytes =
        std::min<std::size_t>(4 * 1024 * 1024, std::max<std::size_t>(1024, budget / 4));
    work.maxFileBytes = std::min<std::size_t>(16 * 1024 * 1024, mOptions.maxOutputBytes);
    work.maxBytes = std::max<std::size_t>(work.maxFileBytes, mOptions.maxOutputBytes * 4);
    const auto pageLimit = std::min<std::size_t>(
        static_cast<std::size_t>(requestedLimit),
        std::max<std::size_t>(1, (budget - 512) / (512 + source.toGenericUtf8().size() * 4)));
    MaiFileSearchFilter filter;
    filter.includeHidden = true;
    filter.useDefaultExcludes = false;
    filter.respectGitIgnore = false;
    MaiFileNameSearchOptions fileOptions;
    fileOptions.root = source;
    fileOptions.scopeKey = searchScope();
    fileOptions.query = pattern;
    fileOptions.filter = filter;
    fileOptions.budget = work;
    fileOptions.pageSize = pageLimit;
    fileOptions.cursor = cursor;
    MaiFileContentSearchOptions textOptions;
    textOptions.root = source;
    textOptions.scopeKey = searchScope();
    textOptions.pattern = pattern.empty() ? "^" : pattern;
    textOptions.isRegex = !literal || pattern.empty();
    textOptions.caseSensitive = !insensitive;
    textOptions.mode = names    ? MaiFileContentSearchMode::Files
                       : counts ? MaiFileContentSearchMode::Count
                                : MaiFileContentSearchMode::Lines;
    textOptions.filter = filter;
    textOptions.budget = work;
    textOptions.pageSize = pageLimit;
    textOptions.maxLineDisplayBytes =
        std::max<std::size_t>(16, std::min<std::size_t>(4096, budget / 8));
    textOptions.cursor = cursor;
    const auto callbacks = searchCallbacks(!standardInput);
    auto service = standardInput ? std::make_shared<MaiFileSearch>() : mOptions.search;
    MaiCommandExecution result;
    bool any = false, incomplete = false, hasMore = false, renderedTruncated = false;
    std::size_t issueCount = 0;
    Json issues = Json::array();
    Json displayWindows = Json::array();
    const auto metadataItems = std::min<std::size_t>(4, std::max<std::size_t>(1, budget / 2048));
    std::string next;
    std::size_t inputLine = 1, inputBegin = 0;
    do {
        if (const auto error = validate()) return error;
        const auto page = find ? service->findFiles(fileOptions, callbacks)
                               : service->searchText(textOptions, callbacks);
        if (page.error) return page.error;
        if (page.canceled) return MaiError{MaiErrorCode::Canceled, "Search canceled"};
        incomplete = incomplete || page.partial;
        hasMore = page.hasMore;
        next = page.nextCursor;
        issueCount += page.issues.size();
        for (const auto& issue : page.issues) {
            if (issues.size() < metadataItems)
                issues.push_back({{"reason", issue.reason}, {"count", issue.count}});
        }
        std::vector<std::string> records;
        if (find) {
            for (const auto& file : page.files)
                records.push_back(source.append(MaiFilePath::fromUtf8(file.path)).toGenericUtf8() +
                                  "\n");
        } else
            for (const auto& match : page.matches) {
                any = true;
                const std::string displayPath = standardInput ? "(stdin)" : match.path;
                std::string record;
                if (names)
                    record = displayPath + "\n";
                else if (counts)
                    record =
                        (recursive ? displayPath + ":" : "") + std::to_string(match.count) + "\n";
                else {
                    if (recursive) record = displayPath + ":";
                    if (numbered) record += std::to_string(match.line) + ":";
                    if (standardInput) {
                        while (inputLine < match.line &&
                               inputBegin < invocation.standardInput.size()) {
                            const auto newline = invocation.standardInput.find('\n', inputBegin);
                            inputBegin = newline == std::string::npos
                                             ? invocation.standardInput.size()
                                             : newline + 1;
                            ++inputLine;
                        }
                        const auto newline = invocation.standardInput.find('\n', inputBegin);
                        const auto end = newline == std::string::npos
                                             ? invocation.standardInput.size()
                                             : newline + 1;
                        record.append(invocation.standardInput, inputBegin, end - inputBegin);
                    } else
                        record += match.text + "\n";
                    if (!standardInput && match.truncated) {
                        renderedTruncated = true;
                        if (displayWindows.size() < metadataItems)
                            displayWindows.push_back(
                                {{"line", match.line}, {"utf8_offset", match.displayOffsetBytes}});
                    }
                }
                records.push_back(std::move(record));
            }
        for (const auto& record : records) {
            if (record.size() > budget / 2 - std::min(result.standardOutput.size(), budget / 2)) {
                if (!standardInput) return budgetError();
                incomplete = true;
                renderedTruncated = true;
                hasMore = true;
                break;
            }
            result.standardOutput += record;
        }
        if (!standardInput || renderedTruncated || !page.hasMore) break;
        textOptions.cursor = page.nextCursor;
    } while (true);
    Json status{{"type", "search_status"},
                {"partial", incomplete || renderedTruncated},
                {"has_more", hasMore},
                {"truncated", renderedTruncated},
                {"next_cursor", standardInput ? "" : next},
                {"scope", "process_local"},
                {"stdin", standardInput},
                {"issue_count", issueCount},
                {"issues", issues},
                {"display_windows", displayWindows}};
    result.standardError = status.dump() + "\n";
    if (result.standardOutput.size() > budget ||
        result.standardError.size() > budget - result.standardOutput.size())
        return budgetError();
    result.outputTruncated = incomplete || renderedTruncated || hasMore;
    if (!find && !any) result.exitCode = incomplete || hasMore ? 2 : 1;
    return result;
}

MaiResult<MaiCommandExecution> MaiFileCommandState::execute(
    const MaiCommandInvocation& invocation) const {
    if (const auto error = validate()) return error;
    const auto& args = invocation.arguments;
    if (args.empty() || args.size() > kMaximumArguments)
        return commandFailure(2, "A bounded command argument list is required");
    std::size_t inputBytes = invocation.standardInput.size();
    if (inputBytes > mOptions.maxOutputBytes) return budgetError();
    for (const auto& argument : args) {
        if (argument.size() > mOptions.maxOutputBytes - inputBytes) return budgetError();
        inputBytes += argument.size();
    }
    const auto cwd = resolve(
        invocation.workingDirectory.isEmpty() ? mWorkingDirectory : invocation.workingDirectory,
        mWorkingDirectory);
    if (!cwd) return cwd.error();
    if (!MaiFileSystem::isDirectory(cwd.value()))
        return commandFailure(1, "Working directory does not exist");
    const std::string& command = args[0];
    if (command == "find" || command == "grep") return searchCommand(invocation, cwd.value());
    if (command == "read-text") return readText(invocation, cwd.value());
    if (command == "stat" || command == "rmdir") {
        if (args.size() != 2 || args[1].empty() || args[1][0] == '-')
            return commandFailure(2, "stat/rmdir requires exactly one path and no options");
        const auto target = resolve(MaiFilePath::fromUtf8(args[1]), cwd.value());
        if (!target) return target.error();
        const auto info = MaiFileSystem::stat(target.value());
        if (!info) return commandFailure(1, info.error().message());
        if (command == "stat") {
            return output(Json{{"path", target.value().toGenericUtf8()},
                               {"directory", info.value().isDirectory},
                               {"size_known", info.value().sizeKnown},
                               {"bytes", info.value().bytes},
                               {"modified", info.value().modified}}
                              .dump() +
                          "\n");
        }
        if (target.value() == mRoot || target.value() == mWorkingDirectory ||
            target.value().isParentOf(mWorkingDirectory) || target.value() == cwd.value() ||
            target.value().isParentOf(cwd.value()))
            return MaiError{MaiErrorCode::InvalidInput,
                            "rmdir refuses the command container and working directories"};
        if (!info.value().isDirectory) return commandFailure(1, "rmdir requires a directory");
        MaiDirectoryDeleteOptions options;
        options.expectedIdentity = info.value().identity;
        options.isCanceled = mOptions.isCanceled;
        options.pathAllowed = [this](const MaiFilePath& path) {
            return resolve(path, mWorkingDirectory).isOk();
        };
        const auto removed = MaiFileSystem::deleteDirectory(target.value(), options);
        if (removed.error.code() == MaiErrorCode::Canceled) return removed.error;
        return removed.error ? commandFailure(1, removed.error.message()) : MaiCommandExecution{};
    }
    if (command == "echo") {
        const bool noNewline = args.size() > 1 && args[1] == "-n";
        std::string text;
        for (std::size_t index = noNewline ? 2 : 1; index < args.size(); ++index) {
            if (index > (noNewline ? 2u : 1u)) text += ' ';
            text += args[index];
        }
        if (!noNewline) text += '\n';
        return output(std::move(text));
    }
    if (command == "pwd") {
        if (args.size() != 1) return commandFailure(2, "pwd accepts no options");
        return output(cwd.value().toGenericUtf8() + "\n");
    }
    const auto readInput = [&](std::size_t index) -> MaiResult<std::string> {
        if (index == args.size() || args[index] == "-") return invocation.standardInput;
        return readAt(MaiFilePath::fromUtf8(args[index]), cwd.value());
    };
    const auto readFailure = [](const MaiError& error) -> MaiResult<MaiCommandExecution> {
        if (error.code() == MaiErrorCode::NotFound) return commandFailure(1, error.message());
        return error;
    };
    if (command == "cat" || command == "head" || command == "tail") {
        std::size_t index = 1;
        std::size_t count = 10;
        if (command == "head" || command == "tail") {
            if (index < args.size() && args[index] == "-n") {
                if (index + 1 >= args.size() || !parseCount(args[index + 1], count))
                    return commandFailure(2, "head/tail -n requires an integer from 0 to 1000000");
                index += 2;
            }
        }
        if (args.size() > index + 1 ||
            (index < args.size() && args[index].size() > 1 && args[index][0] == '-'))
            return commandFailure(
                2, "The command accepts only its documented options and one input file");
        const auto input = readInput(index);
        if (!input) return readFailure(input.error());
        if (command == "cat") return output(input.value());
        // 行切片保留每个原始换行，包括 CRLF 和没有末尾换行的最后一行。
        // 只保存行计数，不为每个换行分配索引；固定字符串搜索只扫描当前行，避免平方扫描。
        const auto& bytes = input.value();
        const auto totalLines =
            static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), '\n')) +
            (!bytes.empty() && bytes.back() != '\n' ? 1 : 0);
        std::string result;
        for (std::size_t line = 0, begin = 0; begin < bytes.size(); ++line) {
            if (const auto error = validate()) return error;
            if (command == "head" && line >= count) break;
            const auto newline = bytes.find('\n', begin);
            const auto end = newline == std::string::npos ? bytes.size() : newline + 1;
            const auto currentBegin = begin;
            const auto length = end - begin;
            begin = end;
            if (command == "tail" && line < totalLines - std::min(count, totalLines)) continue;
            if (length > mOptions.maxOutputBytes - std::min(result.size(), mOptions.maxOutputBytes))
                return budgetError();
            result.append(bytes, currentBegin, length);
        }
        return output(std::move(result));
    }
    if (command == "cp" || command == "mv") {
        if (args.size() != 3 || args[1].empty() || args[2].empty() || args[1][0] == '-' ||
            args[2][0] == '-')
            return commandFailure(2,
                                  "cp/mv requires exactly two ordinary file paths and no options");
        const auto source = resolve(MaiFilePath::fromUtf8(args[1]), cwd.value());
        const auto target = resolve(MaiFilePath::fromUtf8(args[2]), cwd.value());
        if (!source) return source.error();
        if (!target) return target.error();
        const auto error =
            command == "cp"
                ? MaiFileSystem::copyFile(source.value(), target.value(), {}, mOptions.isCanceled)
                : MaiFileSystem::moveFile(source.value(), target.value(), {}, mOptions.isCanceled);
        if (error.code() == MaiErrorCode::Canceled) return error;
        return error ? commandFailure(1, error.message()) : MaiCommandExecution{};
    }
    if (command == "mkdir" || command == "ls" || command == "rm" || command == "touch") {
        std::size_t index = 1;
        bool flag = false;
        if (index < args.size() && args[index] == (command == "mkdir" ? "-p"
                                                   : command == "ls"  ? "-a"
                                                                      : "")) {
            flag = true;
            ++index;
        }
        if (args.size() > index + 1 || (command != "ls" && args.size() != index + 1) ||
            (index < args.size() && (args[index].empty() || args[index][0] == '-')))
            return commandFailure(
                2, "The file command accepts only one path and its documented option");
        const auto target = resolve(
            index == args.size() ? cwd.value() : MaiFilePath::fromUtf8(args[index]), cwd.value());
        if (!target) return target.error();
        if (command == "ls") return list(target.value(), flag);
        MaiError error;
        if (command == "mkdir") {
            if (!flag && (MaiFileSystem::exists(target.value()) ||
                          !MaiFileSystem::isDirectory(target.value().dirName())))
                return commandFailure(1, "mkdir target exists or its parent directory is missing");
            error = MaiFileSystem::createDirectories(target.value());
        } else if (command == "rm") {
            error = MaiFileSystem::removeFile(target.value());
        } else {
            if (MaiFileSystem::exists(target.value())) {
                MaiRegularFileInfo info;
                return MaiFileSystem::regularFileInfo(target.value(), info)
                           ? MaiCommandExecution{}
                           : commandFailure(1, "touch target is not an ordinary file");
            }
            error = MaiFileSystem::createEmptyFile(target.value());
        }
        return error ? commandFailure(1, error.message()) : MaiCommandExecution{};
    }
    return commandFailure(127, "Unsupported in-process command: " + command);
}
}  // namespace

MaiCommandScriptHost makeMaiFileCommandHost(MaiFileCommandOptions options) {
    const auto outputLimit = options.maxOutputBytes;
    const auto state = std::make_shared<const MaiFileCommandState>(std::move(options));
    MaiCommandScriptHost host;
    host.execute = [state, outputLimit](
                       const MaiCommandInvocation& invocation) -> MaiResult<MaiCommandExecution> {
        auto result = state->execute(invocation);
        if (!result) return result.error();
        // 普通失败诊断同样计入输出预算；先比较再相减，避免合计字节数溢出。
        if (result.value().standardOutput.size() > outputLimit ||
            result.value().standardError.size() >
                outputLimit - result.value().standardOutput.size())
            return budgetError();
        return result;
    };
    host.read = [state](const MaiFilePath& path) { return state->read(path); };
    host.write = [state](const MaiFilePath& path, const std::string& bytes, bool append) {
        return state->write(path, bytes, append);
    };
    host.changeDirectory = [state](const MaiFilePath& path) {
        return state->changeDirectory(path);
    };
    return host;
}
