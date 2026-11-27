// 文件作用：在权限边界内执行 shell 命令并收集结果。
// 直接依赖：MaiProcess.h、MaiSessionStore.h；执行一次，保存捕获快照，返回有界预览与恢复事实。
#include "MaiExecCommandTool.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiIdGenerator.h"
#include "MaiProcess.h"
#include "MaiSessionStore.h"
#include "MaiShellEnvironment.h"
#include "MaiShellSessionManager.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiTime.h"
#include "MaiToolOutput.h"

// shell 工具：跑一条命令，把它说的话给模型。
//
// 这是整套工具里最危险的一个，所以它的审批逻辑比别的都细：
// 不是「shell 要不要审批」，而是「**这一条命令**要不要审批」。

namespace {

using json = nlohmann::json;

constexpr std::size_t kPreviewBytes = 1024 * 1024;

// 和 Chromium ReadUnicodeCharacter 一样，只接受 Unicode 标量；非字符仍合法。
// 非法字节按单字节转义展示，原字节只保存在独立快照中，不能悄悄修成另一段文本。
std::size_t utf8ScalarBytes(const std::string& text, std::size_t offset) {
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return 1;
    const std::size_t length = first >= 0xc2 && first <= 0xdf   ? 2
                               : first >= 0xe0 && first <= 0xef ? 3
                               : first >= 0xf0 && first <= 0xf4 ? 4
                                                                : 0;
    if (length == 0 || length > text.size() - offset) return 0;
    unsigned int codepoint = first & (0x7f >> length);
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xc0) != 0x80) return 0;
        codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return 0;
    }
    return length;
}

struct OutputPreview {
    std::string text;
    bool truncated = false;
    bool escapedBytes = false;
    std::size_t omittedBytes = 0;
};

OutputPreview prefixPreview(const std::string& source, std::size_t begin, std::size_t maximumBytes,
                            std::size_t& consumed) {
    OutputPreview preview;
    std::size_t offset = begin;
    while (offset < source.size()) {
        const std::size_t length = utf8ScalarBytes(source, offset);
        const std::size_t renderedBytes = length == 0 ? 4 : length;
        if (renderedBytes > maximumBytes - preview.text.size()) break;
        if (length == 0) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            const unsigned char byte = static_cast<unsigned char>(source[offset++]);
            preview.text += "\\x";
            preview.text += kHex[byte >> 4];
            preview.text += kHex[byte & 0x0f];
            preview.escapedBytes = true;
        } else {
            preview.text.append(source, offset, length);
            offset += length;
        }
    }
    consumed = offset;
    preview.truncated = offset != source.size();
    return preview;
}

OutputPreview previewOf(const std::string& source, std::size_t maximumBytes) {
    std::size_t consumed = 0;
    auto preview = prefixPreview(source, 0, maximumBytes, consumed);
    if (!preview.truncated) return preview;
    // Codex 的截断保留头尾；编译/测试的最终错误通常位于末尾，不能只显示开头。
    // 原始快照仍保持不变，省略提示只属于模型可见预览并计入预览字节预算。
    const std::string marker = maximumBytes >= 40   ? "\n... [middle output omitted] ...\n"
                               : maximumBytes >= 12 ? "\n[...]\n"
                                                    : "...";
    const std::size_t contentBudget = maximumBytes - std::min(maximumBytes, marker.size());
    auto head = prefixPreview(source, 0, contentBudget / 2, consumed);
    const auto tailBudget = contentBudget - head.text.size();
    std::size_t tailStart = source.size(), tailBytes = 0;
    while (tailStart > consumed) {
        auto start = tailStart - 1;
        while (start > consumed && tailStart - start < 4 &&
               (static_cast<unsigned char>(source[start]) & 0xc0) == 0x80)
            --start;
        const auto width = utf8ScalarBytes(source, start);
        const bool scalar = width != 0 && start + width == tailStart;
        if (!scalar) start = tailStart - 1;
        const auto rendered = scalar ? width : std::size_t{4};
        if (rendered > tailBudget - tailBytes) break;
        tailBytes += rendered;
        tailStart = start;
    }
    std::size_t tailEnd = 0;
    auto tail = prefixPreview(source, tailStart, tailBudget, tailEnd);
    preview.text = head.text + marker.substr(0, maximumBytes) + tail.text;
    preview.escapedBytes = head.escapedBytes || tail.escapedBytes;
    preview.omittedBytes = tailStart - consumed;
    return preview;
}

MaiToolResult beforeStartFailure(MaiErrorCode code, const std::string& message) {
    MaiToolExecution execution;
    execution.status = MaiToolExecutionStatus::NotStarted;
    execution.effectStatus = MaiToolEffectStatus::None;
    execution.started = false;
    execution.canceled = code == MaiErrorCode::Canceled;
    execution.nextAction =
        execution.canceled ? MaiToolNextAction::None : MaiToolNextAction::ReviseArguments;
    MaiToolResult result = MaiToolResult::failure(code, message);
    result.withExecution(std::move(execution));
    return result;
}

MaiToolResult afterStartFailure(const MaiError& error, const std::string& processId,
                                double wallTimeSeconds) {
    MaiToolExecution execution;
    execution.started = true;
    execution.status = MaiToolExecutionStatus::Unknown;
    execution.effectStatus = MaiToolEffectStatus::Unknown;
    execution.canceled = error.code() == MaiErrorCode::Canceled;
    execution.taskId = processId;
    execution.nextAction = MaiToolNextAction::InspectState;
    execution.recoveryTool = "write_stdin";
    const json result{{"chunk_id", MaiIdGenerator::generate("chunk")},
                      {"wall_time_seconds", wallTimeSeconds},
                      {"output", ""},
                      {"session_id", std::stoi(processId)},
                      {"error", error.message()}};
    return MaiToolResult::failure(error.code(), result.dump()).withExecution(std::move(execution));
}

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string commandOf(const json& arguments) {
    const auto command = arguments.find("cmd");
    return command != arguments.end() && command->is_string() ? command->get<std::string>()
                                                              : std::string{};
}

// 命令行里第一个词，也就是要跑的程序。
//
// 只做最朴素的切分：按空白切，去掉引号。**不求精确**——它只用来生成审批的键
// 和给只读白名单查一下，判错的后果是多问一次用户，不是少问一次。
std::string programOf(const std::string& command) {
    std::size_t begin = command.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    std::size_t end = command.find_first_of(" \t\r\n", begin);
    std::string first = command.substr(begin, end == std::string::npos ? end : end - begin);
    if (first.size() >= 2 && (first.front() == '"' || first.front() == '\'') &&
        first.back() == first.front()) {
        first = first.substr(1, first.size() - 2);
    }
    // 带路径时只留最后一段：`/usr/bin/git` 和 `git` 是一回事。
    const std::size_t slash = first.find_last_of("/\\");
    if (slash != std::string::npos) first = first.substr(slash + 1);
    // Windows 上 `git.exe` 和 `git` 也是一回事。
    if (first.size() > 4) {
        std::string tail = first.substr(first.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".exe" || tail == ".cmd" || tail == ".bat") {
            first = first.substr(0, first.size() - 4);
        }
    }
    return first;
}

// 只读命令白名单：跑这些不用问。
//
// **白名单，不是黑名单。** 黑名单永远列不全——`rm` 挡住了还有 `find -delete`、
// `> file`、`python -c`。名单里只放「无论带什么参数都改不了东西」的程序。
//
// git 是个例外：`git status` 安全，`git push` 不安全，所以 git 整个不在名单里，
// 交给下面的子命令判断。
bool isReadOnlyProgram(const std::string& program) {
    static const char* const kReadOnly[] = {
        "ls",   "dir",   "pwd",   "echo",   "cat",   "head", "tail", "wc",       "stat",
        "grep", "which", "where", "whoami", "uname", "df",   "du",   "printenv", "cmp",
    };
    for (const char* candidate : kReadOnly) {
        if (program == candidate) return true;
    }
    return false;
}

// git 的只读子命令。`git status` 每一轮都要用，每次弹框会把用户训练成
// 条件反射点「允许」，那时候真正危险的那次也会被一起放过。
bool isReadOnlyGitCommand(const std::string& command) {
    static const char* const kReadOnlySubcommands[] = {
        "status", "describe", "rev-parse", "ls-files", "shortlog",
    };
    const std::size_t programEnd = command.find_first_of(" \t");
    if (programEnd == std::string::npos) return false;
    std::size_t begin = command.find_first_not_of(" \t", programEnd);
    if (begin == std::string::npos) return false;
    const std::size_t end = command.find_first_of(" \t", begin);
    const std::string sub = command.substr(begin, end == std::string::npos ? end : end - begin);
    for (const char* candidate : kReadOnlySubcommands) {
        if (sub == candidate) return true;
    }
    return false;
}

// 一条命令里有没有 shell 的控制字符。
//
// 有的话就不能只看第一个词了：`ls; rm -rf .` 的第一个词是 ls，但它真正干的是 rm。
// 这种一律要审批，不管第一个词多无害。
bool hasShellControlCharacters(const std::string& command) {
    // CR/C0 控制字符、Windows 变量展开和 PowerShell 表达式均可能改变真实执行语义。
    return command.find_first_of(";&|><`$%!^(){}@") != std::string::npos ||
           std::any_of(command.begin(), command.end(),
                       [](unsigned char c) { return (c < 32 && c != '\t') || c == 127; });
}

// 核对自动批准的程序写法，不能把任意路径或脚本剥成可信 basename。
bool hasTrustedProgramSpelling(const std::string& command) {
    const auto begin = command.find_first_not_of(" \t");
    if (begin == std::string::npos) return false;
    const auto end = command.find_first_of(" \t", begin);
    auto token = command.substr(begin, end == std::string::npos ? end : end - begin);
    if (token.size() >= 2 && (token.front() == '\'' || token.front() == '"') &&
        token.back() == token.front())
        token = token.substr(1, token.size() - 2);
    const auto program = programOf(command);
    if (token.find_first_of("/\\") != std::string::npos) {
#if defined(_WIN32)
        // Windows 路径、批处理和 UNC 不参与自动批准，仍可走普通逐次审批。
        return false;
#else
        return token == "/bin/" + program || token == "/usr/bin/" + program;
#endif
    }
    if (token.size() >= 4) {
        auto extension = token.substr(token.size() - 4);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".cmd" || extension == ".bat") return false;
    }
    return token == program || token == program + ".exe";
}

bool shellCommandRequiresApproval(const std::string& command) {
    if (command.empty() || hasShellControlCharacters(command) ||
        !hasTrustedProgramSpelling(command))
        return true;
    const auto program = programOf(command);
    return program == "git" ? !isReadOnlyGitCommand(command) : !isReadOnlyProgram(program);
}

// 模型的会话 ID 是 i32 数字；内部使用同一数字的十进制文本，拒绝旧字符串别名。
bool validSessionId(const json& value) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() > 0 &&
               value.get<std::uint64_t>() <=
                   static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return value.get<std::int64_t>() > 0 &&
           value.get<std::int64_t>() <= std::numeric_limits<int>::max();
}

std::string validateSessionArguments(const json& arguments, bool input) {
    const std::vector<std::string> allowed =
        input
            ? std::vector<std::string>{"session_id", "chars", "yield_time_ms", "max_output_tokens"}
            : std::vector<std::string>{
                  "cmd", "workdir", "yield_time_ms", "max_output_tokens", "shell", "login", "tty"};
    for (auto item = arguments.begin(); item != arguments.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
            return "unsupported execution option: " + item.key();
        if (item.key() == "session_id") {
            if (!validSessionId(item.value())) return "session_id must be a positive i32 integer";
        } else if (item.key() == "login" || item.key() == "tty") {
            if (!item.value().is_boolean()) return item.key() + " must be a boolean";
        } else if (item.key() == "yield_time_ms" || item.key() == "max_output_tokens") {
            if (!item.value().is_number_integer() ||
                (!item.value().is_number_unsigned() && item.value().get<std::int64_t>() < 0))
                return item.key() + " must be a nonnegative integer";
        } else if (!item.value().is_string())
            return item.key() + " must be a string";
        else if (item.key() != "chars" &&
                 item.value().get_ref<const std::string&>().find('\0') != std::string::npos)
            return item.key() + " contains NUL";
    }
    if (input && (!arguments.contains("session_id") ||
                  arguments.value("chars", std::string{}).size() > 64 * 1024))
        return "session_id is required; stdin is at most 64 KiB";
    return {};
}

int boundedInteger(const json& arguments, const char* key, int fallback, int minimum, int maximum) {
    const auto value = arguments.value(key, json(fallback));
    const auto requested = value.is_number_unsigned()
                               ? value.get<std::uint64_t>()
                               : static_cast<std::uint64_t>(value.get<std::int64_t>());
    // 无符号大值不能先转换为 int64；缺省值也必须经过同一上下界约束。
    return static_cast<int>(std::clamp(requested, static_cast<std::uint64_t>(minimum),
                                       static_cast<std::uint64_t>(maximum)));
}
std::size_t previewLimit(const json& arguments) {
    return static_cast<std::size_t>(boundedInteger(arguments, "max_output_tokens", 10000, 0,
                                                   static_cast<int>(kPreviewBytes / 4))) *
           4;
}

// 同一同步适配器保存每次冻结的原始字节；后台 worker 不持有输出存储或调用上下文。
MaiToolResult renderSession(const MaiShellSessionSnapshot& snapshot, std::size_t previewBytes,
                            double wallTimeSeconds, const MaiToolContext& context) {
    MaiToolExecution execution;
    execution.started = snapshot.started;
    execution.taskId = snapshot.sessionId;
    execution.effectStatus = MaiToolEffectStatus::Unknown;
    execution.timedOut = snapshot.timedOut;
    execution.canceled = snapshot.canceled;
    execution.status = !snapshot.completed ? MaiToolExecutionStatus::Running
                       : snapshot.canceled ? MaiToolExecutionStatus::Canceled
                       : snapshot.timedOut || snapshot.terminated || snapshot.error
                           ? MaiToolExecutionStatus::Unknown
                           : MaiToolExecutionStatus::Completed;
    if (execution.status == MaiToolExecutionStatus::Completed)
        execution.exitCode = snapshot.exitCode;

    // 模型仅收到本次消费的数据；完整字节另存，不把固定状态说明反复拼进正文。
    auto source = snapshot.completed && !snapshot.captureTruncated
                      ? maiProcessOutputText(snapshot.output, false)
                      : snapshot.output;
    if (!snapshot.previewTail.empty()) {
        const auto prefixEnd = snapshot.totalOutputBytes - snapshot.previewTail.size();
        if (prefixEnd > snapshot.output.size())
            source += "\n... [capture gap; only retained stream tail follows] ...\n";
        source += snapshot.previewTail;
    }
    const auto preview = previewOf(source, previewBytes);
    const bool lost = snapshot.captureTruncated || snapshot.discardedBytes != 0;
    const bool interrupted =
        snapshot.timedOut || snapshot.canceled || snapshot.terminated || snapshot.error;
    execution.outputTruncated = preview.truncated || lost || interrupted || !snapshot.completed;
    bool saved = false;
    if (context.toolOutputStore != nullptr) {
        MaiToolOutputRecord record;
        record.id = MaiIdGenerator::generate("out");
        record.sessionId = context.sessionId;
        record.messageId = context.messageId;
        record.partId = context.partId;
        record.callId = context.callId;
        record.text = snapshot.output;
        record.truncated = lost || interrupted || !snapshot.completed;
        record.created = MaiTime::getCurrentTime();
        saved = !context.toolOutputStore->putToolOutput(record);
        if (saved) execution.outputId = record.id;
    }
    json result{{"chunk_id", MaiIdGenerator::generate("chunk")},
                {"wall_time_seconds", wallTimeSeconds},
                {"output", preview.text}};
    // 结束但仍有未消费尾部时会话继续可读；排空后才不再返回 session_id。
    if (!snapshot.completed || snapshot.hasMoreOutput) {
        result["session_id"] = std::stoi(snapshot.sessionId);
        execution.nextAction = MaiToolNextAction::QueryStatus;
        execution.recoveryTool = "write_stdin";
    } else if (snapshot.exitCode) {
        result["exit_code"] = *snapshot.exitCode;
    }
    if (preview.truncated) result["original_token_count"] = (source.size() + 3) / 4;
    // MaiChat 的真实捕获与补读扩展只在需要时出现，不复制固定的长说明。
    if (lost) result["capture_truncated"] = true;
    if (snapshot.discardedBytes) result["discarded_bytes"] = snapshot.discardedBytes;
    if (snapshot.canceled) result["canceled"] = true;
    if (snapshot.timedOut) result["timed_out"] = true;
    if (snapshot.terminated) result["terminated"] = true;
    if (snapshot.backgroundRunning) result["background_running"] = true;
    if (snapshot.error) result["error"] = snapshot.error.message();
    if (preview.escapedBytes) result["invalid_source_bytes_escaped"] = true;
    if (preview.truncated || lost || interrupted) {
        if (saved)
            result["output_id"] = execution.outputId;
        else
            result["output_unavailable"] = true;
        if (snapshot.completed && !snapshot.hasMoreOutput && (interrupted || !saved)) {
            execution.nextAction = MaiToolNextAction::InspectState;
        } else if (snapshot.completed && !snapshot.hasMoreOutput) {
            execution.nextAction = MaiToolNextAction::ReadOutput;
            execution.recoveryTool = "tool_output_read";
        }
    }
    MaiToolResult output =
        snapshot.canceled ? MaiToolResult::failure(MaiErrorCode::Canceled, result.dump(),
                                                   execution.outputTruncated)
        : snapshot.error  ? MaiToolResult::failure(snapshot.error.code(), result.dump(),
                                                   execution.outputTruncated)
                          : MaiToolResult::success(result.dump(), execution.outputTruncated);
    return output.withExecution(std::move(execution));
}

class ExecCommandTool final : public MaiTool {
public:
    explicit ExecCommandTool(std::shared_ptr<MaiShellSessionManager> manager)
        : mManager(std::move(manager)) {}
    std::string name() const override {
        return "exec_command";
    }

    std::string description() const override {
        // cmd 是解释器命令；workdir 仍过会话路径策略，shell/login 不改变批准边界。
        // tty=false 关闭标准输入，true 才允许交互；yield 仅限制本次等待，不限制进程寿命。
        // 输出按 token 近似预算保留头尾；原文补读不重跑命令，退出码不代表整条管线均成功。
        return "Run a command, returning output or a numeric session_id for continued interaction. "
               "cmd uses the selected shell syntax; workdir defaults to the authorized session "
               "cwd. "
#if defined(_WIN32)
               "Windows selects the first available pwsh, Windows PowerShell, then cmd by default. "
               "Use the selected interpreter's syntax; Bash commands are not PowerShell commands. "
               "External executables need PATH discovery or an explicit path. If rg is "
               "unavailable in PowerShell, use Get-ChildItem and Select-String; use -LiteralPath "
               "for paths containing spaces or wildcard characters. "
#endif
               "shell selects the interpreter; login defaults to true. tty defaults to false "
               "(closed stdin and pipes); true allocates a native PTY for interactive input. "
               "yield_time_ms limits this wait, not the command lifetime; default 10000, "
#if defined(_WIN32)
               "effective range 10000..30000 on Windows. "
#else
               "effective range 250..30000. "
#endif
               "Continue with write_stdin; do not restart a running command to obtain its output. "
               "max_output_tokens defaults to 10000 and is capped by policy; omitted output "
               "retains the beginning and end. An output_id permits tool_output_read without "
               "re-execution. Approval and workspace policies apply; exit_code belongs to the "
               "shell process and does not establish every pipeline stage succeeded. "
               "On Windows, a completed command may report background_running=true: "
               "its background services remain owned by this Agent session and count toward "
               "process limits. "
               "Use the service's own close command when finished; cancellation and owner cleanup "
               "still "
               "terminate owned processes. Do not reopen an existing service solely because its "
               "launcher exited.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"cmd":{"type":"string"},)"
               R"("workdir":{"type":"string"},"shell":{"type":"string"},)"
               R"("login":{"type":"boolean","default":true},"tty":{"type":"boolean","default":false},)"
               R"("yield_time_ms":{"type":"integer","minimum":0,"default":10000},)"
               R"("max_output_tokens":{"type":"integer","minimum":0,"default":10000}},)"
               R"("required":["cmd"],"additionalProperties":false})";
    }

    bool requiresApproval(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        return !validateSessionArguments(args, false).empty() || args.value("tty", false) ||
               maiExecCommandRequiresApproval(commandOf(args));
    }

    // 「这个会话以后都允许」只记到**完整命令**这一级。
    //
    // 只记工具名或程序名会让 `rm file-a` 的批准错误地放行 `rm file-b`。
    std::string approvalKey(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string command = commandOf(args);
        const std::string program = programOf(command);
        if (args.value("tty", json(false)) != json(false))
            return "exec_command:tty:" + args.value("tty", json(false)).dump() + ":" + command;
        // 带控制字符时，真正执行的内容不由第一个词决定；保留独立前缀方便审计。
        if (program.empty() || hasShellControlCharacters(command))
            return "exec_command:<compound>:" + command;
        // Shell 语法可写入或删除任意路径；不像内置文件工具那样能可靠识别目标文件。
        // 因此一次命令获批不能扩大为整个程序永久获批。
        return "exec_command:command:" + command;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const auto valid = validateSessionArguments(args, false);
        if (!valid.empty()) return beforeStartFailure(MaiErrorCode::InvalidInput, valid);
        if (commandOf(args).empty())
            return beforeStartFailure(MaiErrorCode::InvalidInput, "the cmd parameter is required");
        if (context.root.empty())
            return beforeStartFailure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so shell commands are unavailable");
        const auto directory = context.resolvePath(args.value("workdir", std::string(".")));
        if (directory.empty() || !MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(directory)))
            return beforeStartFailure(
                MaiErrorCode::InvalidInput,
                "workdir is outside the authorized workspace or not a directory");
        MaiShellSessionOptions options;
        options.command = commandOf(args);
        options.workdir = directory;
        options.shell = args.value("shell", std::string{});
        options.login = args.value("login", true);
        options.tty = args.value("tty", false);
        options.timeoutMs = 0;
        const auto canceled = [&context] { return context.isCanceled(); };
        const auto began = std::chrono::steady_clock::now();
        const auto started = mManager->start(context.sessionId, options, canceled);
        if (!started) return beforeStartFailure(started.error().code(), started.error().message());
        const auto snapshot = mManager->interact(context.sessionId, started.value(), {},
                                                 boundedInteger(args, "yield_time_ms", 10000,
#if defined(_WIN32)
                                                                10000,
#else
                                                                250,
#endif
                                                                30000),
                                                 false, canceled, true);
        if (!snapshot)
            return afterStartFailure(
                snapshot.error(), started.value(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
        return renderSession(
            snapshot.value(), previewLimit(args),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(),
            context);
    }
    std::vector<std::string> approvalKeys(const std::string& raw,
                                          const MaiToolContext& context) const override {
        const auto args = parseArguments(raw);
        const auto cwd = args.value("workdir", json("."));
        const auto directory =
            cwd.is_string() ? context.resolvePath(cwd.get<std::string>()) : std::string{};
        const auto requested = args.value("shell", json(""));
        const auto resolved = requested.is_string()
                                  ? maiResolveShellEnvironment(requested.get<std::string>())
                                  : MaiResult<MaiShellEnvironment>(
                                        MaiError{MaiErrorCode::InvalidInput, "invalid shell"});
        const auto login = args.value("login", json(true));
        // 同一命令换解释器、登录或终端模式会改变进程语义；批准不能跨模式复用。
        // 新工具使用自己的批准身份，不把旧 shell 的授权直接继承为新工具授权。
        auto scope = json::array(
            {approvalKey(raw), directory, resolved ? resolved.value().path : "unresolved", login});
        if (args.value("tty", json(false)) != json(false))
            scope.push_back(json{{"tty", args.value("tty", json(false))}});
        return {"exec_command:scope:" + scope.dump()};
    }
    MaiError onSessionClosed(const std::string& owner) override {
        mManager->stopOwner(owner);
        return {};
    }
    MaiError onAgentShutdown() override {
        mManager->stopAll();
        return {};
    }

private:
    std::shared_ptr<MaiShellSessionManager> mManager;
};

class WriteStdinTool final : public MaiTool {
public:
    explicit WriteStdinTool(std::shared_ptr<MaiShellSessionManager> manager)
        : mManager(std::move(manager)) {}
    std::string name() const override {
        return "write_stdin";
    }
    std::string description() const override {
        // 仅续接当前 Agent 所拥有的数字进程会话。空 chars 轮询，普通输入要求启动时已选 PTY。
        // Ctrl-C 中断原进程；非空输入仍走审批。预算只覆盖新输出，不把旧块重复塞回上下文。
        return "Write characters to or poll an existing exec_command session owned by this Agent "
               "session, returning only newly consumed output. session_id is the numeric ID "
               "returned by exec_command. Empty chars polls without writing; normal input "
               "requires tty=true at launch. chars=\"\\u0003\" interrupts the process. "
               "Nonempty writes default to 250ms (250..30000); empty polls use 5000..300000ms. "
               "Nonempty input follows the approval policy; empty polls require no approval. "
               "max_output_tokens defaults to 10000 and is capped by policy. Use output_id with "
               "tool_output_read for retained output; never relaunch solely to recover output.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"session_id":{"type":"integer","minimum":1,"maximum":2147483647},)"
               R"("chars":{"type":"string"},"yield_time_ms":{"type":"integer","minimum":0},)"
               R"("max_output_tokens":{"type":"integer","minimum":0,"default":10000}},)"
               R"("required":["session_id"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        const auto args = parseArguments(raw);
        return !validateSessionArguments(args, true).empty() ||
               !args.value("chars", std::string{}).empty();
    }
    std::string approvalKey(const std::string& raw) const override {
        const auto args = parseArguments(raw);
        return "write_stdin:" + args.dump();
    }
    std::vector<std::string> approvalKeys(const std::string& raw,
                                          const MaiToolContext& context) const override {
        return {"write_stdin:owner:" + context.sessionId + ":" + approvalKey(raw)};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = parseArguments(raw);
        const auto invalid = validateSessionArguments(args, true);
        if (!invalid.empty()) return beforeStartFailure(MaiErrorCode::InvalidInput, invalid);
        const auto chars = args.value("chars", std::string{});
        const auto sessionId = std::to_string(args["session_id"].get<int>());
        const auto began = std::chrono::steady_clock::now();
        const auto snapshot = mManager->interact(
            context.sessionId, sessionId, chars,
            boundedInteger(args, "yield_time_ms", 250, chars.empty() ? 5000 : 250,
                           chars.empty() ? 300000 : 30000),
            false, [&context] { return context.isCanceled(); }, true);
        if (!snapshot) {
            if (snapshot.error().code() == MaiErrorCode::NotFound)
                return beforeStartFailure(snapshot.error().code(), snapshot.error().message());
            return afterStartFailure(
                snapshot.error(), sessionId,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
        }
        return renderSession(
            snapshot.value(), previewLimit(args),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count(),
            context);
    }
    MaiError onSessionClosed(const std::string& owner) override {
        mManager->stopOwner(owner);
        return {};
    }
    MaiError onAgentShutdown() override {
        mManager->stopAll();
        return {};
    }

private:
    std::shared_ptr<MaiShellSessionManager> mManager;
};

}  // namespace

bool maiExecCommandRequiresApproval(const std::string& command) {
    return shellCommandRequiresApproval(command);
}

std::unique_ptr<MaiTool> makeMaiExecCommandTool() {
    return std::make_unique<ExecCommandTool>(std::make_shared<MaiShellSessionManager>());
}
std::unique_ptr<MaiTool> makeMaiExecCommandTool(std::shared_ptr<MaiShellSessionManager> manager) {
    return std::make_unique<ExecCommandTool>(manager ? std::move(manager)
                                                     : std::make_shared<MaiShellSessionManager>());
}
std::unique_ptr<MaiTool> makeMaiWriteStdinTool(std::shared_ptr<MaiShellSessionManager> manager) {
    return std::make_unique<WriteStdinTool>(manager ? std::move(manager)
                                                    : std::make_shared<MaiShellSessionManager>());
}
void registerMaiExecTools(MaiToolRegistry& registry) {
    if (!maiIsProcessExecutionSupported()) return;
    auto manager = std::make_shared<MaiShellSessionManager>();
    registry.add(makeMaiExecCommandTool(manager));
    registry.add(makeMaiWriteStdinTool(std::move(manager)));
}
