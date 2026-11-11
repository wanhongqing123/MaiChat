// 文件作用：在权限边界内执行 shell 命令并收集结果。
// 直接依赖：MaiProcess.h、MaiSessionStore.h；执行一次，保存捕获快照，返回有界预览与恢复事实。
#include "MaiShellTool.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>

#include "MaiIdGenerator.h"
#include "MaiProcess.h"
#include "MaiSessionStore.h"
#include "MaiTime.h"
#include "MaiToolOutput.h"

// shell 工具：跑一条命令，把它说的话给模型。
//
// 这是整套工具里最危险的一个，所以它的审批逻辑比别的都细：
// 不是「shell 要不要审批」，而是「**这一条命令**要不要审批」。

namespace {

using json = nlohmann::json;

constexpr int kDefaultTimeoutMs = 120000;
constexpr int kMaxTimeoutMs = 600000;
constexpr std::size_t kPreviewBytes = 48 * 1024;

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
};

OutputPreview previewOf(const std::string& source) {
    OutputPreview preview;
    std::size_t offset = 0;
    while (offset < source.size()) {
        const std::size_t length = utf8ScalarBytes(source, offset);
        const std::size_t renderedBytes = length == 0 ? 4 : length;
        if (renderedBytes > kPreviewBytes - preview.text.size()) break;
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
    preview.truncated = offset != source.size();
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

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string commandOf(const json& arguments) {
    const auto command = arguments.find("command");
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
    return command.find_first_of(";&|><`$\n") != std::string::npos;
}

class ShellTool final : public MaiTool {
public:
    std::string name() const override {
        return "shell";
    }

    std::string description() const override {
        // 只有内置工具不能完成时才回退到 shell；工作目录、审批、超时和输出长度受控。
        return "Fallback for tasks built-in tools cannot perform. Run a shell command in the "
               "working directory and return combined stdout and stderr. Prefer file_create, "
               "file_create_directory, file_delete, file_read, file_write, file_edit, and "
               "file_patch for file "
               "operations. Commands that only read run immediately; anything else asks first.";
    }

    std::string parametersSchema() const override {
        // command 是完整命令行；timeout_ms 为有界等待，执行前还要过 shell 审批。
        return R"({"type":"object","properties":{)"
               R"("command":{"type":"string","description":"The command line to run"},)"
               R"("timeout_ms":{"type":"integer","description":"Give up after this many millisecon)"
               R"(ds, default 120000, max 600000"}},)"
               R"("required":["command"]})";
    }

    // 只有能确定是只读的才放行。看不懂、解析不了、带控制字符——一律问。
    // 这一层的兜底方向永远是「不放行」。
    bool requiresApproval(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string command = commandOf(args);
        if (command.empty()) return true;
        if (hasShellControlCharacters(command)) return true;

        const std::string program = programOf(command);
        if (program.empty()) return true;
        if (program == "git") return !isReadOnlyGitCommand(command);
        return !isReadOnlyProgram(program);
    }

    // 「这个会话以后都允许」只记到**完整命令**这一级。
    //
    // 记成 "shell" 或程序名的话，给 `rm file-a` 点一次头，`rm file-b` 也会被放行。
    std::string approvalKey(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string command = commandOf(args);
        const std::string program = programOf(command);
        // 带控制字符时，真正执行的内容不由第一个词决定；保留独立前缀方便审计。
        if (program.empty() || hasShellControlCharacters(command))
            return "shell:<compound>:" + command;
        // Shell 语法可写入或删除任意路径；不像内置文件工具那样能可靠识别目标文件。
        // 因此一次命令获批不能扩大为整个程序永久获批。
        return "shell:command:" + command;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string command = commandOf(args);
        if (command.empty()) {
            return beforeStartFailure(MaiErrorCode::InvalidInput,
                                      "the command parameter is required");
        }
        if (context.root.empty()) {
            return beforeStartFailure(
                MaiErrorCode::InvalidInput,
                "this session has no working directory, so shell commands are unavailable");
        }

        MaiProcessOptions options;
        // 整行交给系统 shell 解释。
        //
        // 这里**刻意**打开 useShell：模型写的是终端里那种命令，管道和 && 是它表达
        // 意图的一部分，拆成 argv 数组反而会把意思改掉。代价是元字符会生效——
        // 所以上面的审批逻辑对带控制字符的命令一律要求用户点头。
        options.useShell = true;
        options.argv = {command};
        options.workingDirectory = context.root;
        options.maxOutputBytes = kMaiToolOutputMaxBytes;
        options.cancel = context.cancel;
        if (args.contains("timeout_ms") && !args["timeout_ms"].is_number_integer()) {
            return beforeStartFailure(MaiErrorCode::InvalidInput, "timeout_ms must be an integer");
        }
        const int requested = args.value("timeout_ms", kDefaultTimeoutMs);
        options.timeoutMs = std::max(1000, std::min(requested, kMaxTimeoutMs));

        MaiProcessResult process;
        const MaiError failure = maiRunProcess(options, process);
        if (!process.started) {
            if (process.canceled) {
                return beforeStartFailure(MaiErrorCode::Canceled,
                                          "the command was not started because it was canceled");
            }
            return beforeStartFailure(failure.code(), failure.message());
        }

        MaiToolExecution execution;
        execution.started = true;
        execution.effectStatus = MaiToolEffectStatus::Unknown;
        execution.timedOut = process.timedOut;
        execution.canceled = process.canceled;
        execution.status = process.canceled ? MaiToolExecutionStatus::Canceled
                           : process.timedOut || failure.hasError()
                               ? MaiToolExecutionStatus::Unknown
                               : MaiToolExecutionStatus::Completed;
        if (!process.canceled && !process.timedOut && !failure.hasError()) {
            execution.exitCode = process.exitCode;
        }

        const OutputPreview preview = previewOf(process.output);
        // 中断或收集失败同样是不完整捕获，不能把“已到达的字节都保存了”说成命令输出完整。
        const bool captureIncomplete =
            process.truncated || process.timedOut || process.canceled || failure.hasError();
        execution.outputTruncated = preview.truncated || captureIncomplete;
        bool saved = false;
        if (context.toolOutputStore != nullptr) {
            MaiToolOutputRecord record;
            record.id = MaiIdGenerator::generate("out");
            record.sessionId = context.sessionId;
            record.messageId = context.messageId;
            record.partId = context.partId;
            record.callId = context.callId;
            record.text = std::move(process.output);
            record.truncated = captureIncomplete;
            record.created = MaiTime::getCurrentTime();
            const MaiError storageFailure = context.toolOutputStore->putToolOutput(record);
            saved = !storageFailure.hasError();
            if (saved) execution.outputId = record.id;
        }

        // 先保存再返回句柄。取消与超时同样保留已捕获的部分，不重执行命令来“补”输出。
        std::string out = preview.text;
        if (out.empty()) out = "(no output)";
        if (preview.escapedBytes) {
            out += "\n[invalid source bytes are shown as \\xHH escapes]";
        }
        if (process.canceled) {
            out +=
                "\n\n[the command was interrupted by the user; effects may already have occurred. "
                "Inspect current state before considering another execution.]";
        } else if (process.timedOut) {
            out += "\n\n[timed out after " + std::to_string(options.timeoutMs) +
                   " ms; termination was requested and effects may already have occurred. "
                   "Inspect current state before considering another execution.]";
        } else if (failure.hasError()) {
            out +=
                "\n\n[process execution or collection failed after launch; inspect current state]";
        } else if (process.exitCode != 0) {
            out += "\n\n[exit code " + std::to_string(process.exitCode) + "]";
        }
        if (process.truncated) {
            out +=
                "\n[output capture truncated at the 16 MiB limit; bytes beyond the retained "
                "capture cannot be recovered from this call]";
        } else if (preview.truncated) {
            out += "\n[output preview truncated]";
        }
        if (captureIncomplete && !process.truncated) {
            out += "\n[captured output is partial because execution did not finish normally]";
        }
        if (saved && (execution.outputTruncated || process.canceled || process.timedOut)) {
            out += "\n[Use tool_output_read with output_id=\"" + execution.outputId +
                   "\" to read the retained captured output. Do not rerun the command to recover "
                   "its output.]";
        } else if (!saved && execution.outputTruncated) {
            out +=
                "\n[retained output unavailable: output storage was unavailable or rejected the "
                "snapshot. Omitted output cannot be recovered from this call. Do not rerun "
                "the command merely to recover output; inspect current state first.]";
        }

        if (process.canceled || process.timedOut || failure.hasError() ||
            (!saved && execution.outputTruncated)) {
            execution.nextAction = MaiToolNextAction::InspectState;
        } else if (execution.outputTruncated && saved) {
            execution.nextAction = MaiToolNextAction::ReadOutput;
            execution.recoveryTool = "tool_output_read";
        }
        MaiToolResult result =
            process.canceled ? MaiToolResult::failure(MaiErrorCode::Canceled, std::move(out),
                                                      execution.outputTruncated)
            : failure.hasError()
                ? MaiToolResult::failure(failure.code(), std::move(out), execution.outputTruncated)
                : MaiToolResult::success(std::move(out), execution.outputTruncated);
        result.withExecution(std::move(execution));
        return result;
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiShellTool() {
    return std::make_unique<ShellTool>();
}
