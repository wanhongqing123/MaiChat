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

MaiToolResult afterStartFailure(const MaiError& error, const std::string& processId) {
    MaiToolExecution execution;
    execution.started = true;
    execution.status = MaiToolExecutionStatus::Unknown;
    execution.effectStatus = MaiToolEffectStatus::Unknown;
    execution.canceled = error.code() == MaiErrorCode::Canceled;
    execution.taskId = processId;
    execution.nextAction = MaiToolNextAction::InspectState;
    return MaiToolResult::failure(
               error.code(),
               error.message() +
                   "; process was launched; inspect its current state before another execution")
        .withExecution(std::move(execution));
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

std::string validateSessionArguments(const json& arguments, bool input) {
    const std::vector<std::string> allowed =
        input ? std::vector<std::string>{"session_id", "chars", "terminate", "yield_time_ms",
                                         "max_output_tokens"}
              : std::vector<std::string>{"command",       "workdir",           "timeout_ms",
                                         "yield_time_ms", "max_output_tokens", "shell",
                                         "login"};
    for (auto item = arguments.begin(); item != arguments.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
            return "unsupported shell option: " + item.key();
        if (item.key() == "terminate" || item.key() == "login") {
            if (!item.value().is_boolean()) return item.key() + " must be a boolean";
        } else if (item.key() == "timeout_ms" || item.key() == "yield_time_ms" ||
                   item.key() == "max_output_tokens") {
            if (!item.value().is_number_integer()) return item.key() + " must be an integer";
            if (item.key() == "yield_time_ms" &&
                (item.value() < 0 || item.value() > (input ? 300000 : 30000)))
                return "yield_time_ms exceeds its supported range";
            if (item.key() == "max_output_tokens" && item.value() < 1)
                return "max_output_tokens must be positive";
        } else if (!item.value().is_string())
            return item.key() + " must be a string";
        else if (item.key() != "chars" &&
                 item.value().get_ref<const std::string&>().find('\0') != std::string::npos)
            return item.key() + " contains NUL";
    }
    if (input &&
        (arguments.value("session_id", std::string{}).empty() ||
         arguments.value("chars", std::string{}).size() > 64 * 1024 ||
         (arguments.value("terminate", false) && !arguments.value("chars", std::string{}).empty())))
        return "session_id is required; stdin is at most 64 KiB and cannot accompany terminate";
    if (input && arguments.value("yield_time_ms", 0) > 30000 &&
        (arguments.value("terminate", false) || !arguments.value("chars", std::string{}).empty()))
        return "stdin writes and terminate limit yield_time_ms to 30000";
    return {};
}

int boundedInteger(const json& arguments, const char* key, int fallback, int minimum, int maximum) {
    if (!arguments.contains(key)) return fallback;
    if (arguments[key] < minimum) return minimum;
    if (arguments[key] > maximum) return maximum;
    return arguments[key].get<int>();
}
std::size_t previewLimit(const json& arguments) {
    return static_cast<std::size_t>(boundedInteger(arguments, "max_output_tokens", 10000, 1,
                                                   static_cast<int>(kPreviewBytes / 4))) *
           4;
}

// 同一同步适配器保存每次冻结的原始字节；后台 worker 不持有输出存储或调用上下文。
MaiToolResult renderSession(const MaiShellSessionSnapshot& snapshot, std::size_t previewBytes,
                            int timeoutMs, const MaiToolContext& context) {
    MaiProcessResult process;
    process.output = snapshot.output;
    process.started = snapshot.started;
    process.canceled = snapshot.canceled;
    process.timedOut = snapshot.timedOut;
    process.truncated = snapshot.captureTruncated;
    process.exitCode = snapshot.exitCode.value_or(0);
    const MaiError failure = snapshot.error;
    MaiProcessOptions options;
    (void)timeoutMs;
    options.timeoutMs = snapshot.timeoutMs;
    MaiToolExecution execution;
    execution.started = true;
    execution.effectStatus = MaiToolEffectStatus::Unknown;
    execution.timedOut = process.timedOut;
    execution.canceled = process.canceled;
    execution.status = !snapshot.completed ? MaiToolExecutionStatus::Running
                       : process.canceled  ? MaiToolExecutionStatus::Canceled
                       : process.timedOut || snapshot.terminated || failure.hasError()
                           ? MaiToolExecutionStatus::Unknown
                           : MaiToolExecutionStatus::Completed;
    if (snapshot.completed && !process.canceled && !process.timedOut && !snapshot.terminated &&
        !failure.hasError()) {
        execution.exitCode = snapshot.exitCode;
    }

    // 完成且未触及捕获上限时保留原 Windows OEM 后备语义；流式块可能切开 UTF-8，
    // 不使用会丢弃尾部标量的截断解码，直接以 \xHH 展示片段并保留原始快照。
    std::string previewSource = snapshot.completed && !snapshot.captureTruncated
                                    ? maiProcessOutputText(process.output, false)
                                    : process.output;
    // 原始补读仍保存连续前缀；超过捕获上限时，显示额外的实时尾环，避免把前缀末端
    // 误称为真实命令结尾。尾环展示不推进普通续读游标，也不拼入原始输出快照。
    if (!snapshot.previewTail.empty()) previewSource += snapshot.previewTail;
    const OutputPreview preview = previewOf(previewSource, previewBytes);
    // 中断或收集失败同样是不完整捕获，不能把“已到达的字节都保存了”说成命令输出完整。
    const bool captureIncomplete = process.truncated || process.timedOut || process.canceled ||
                                   snapshot.terminated || failure.hasError() ||
                                   !snapshot.completed || snapshot.discardedBytes != 0;
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

    execution.taskId = snapshot.sessionId;
    json metadata = {{"session_id", snapshot.sessionId},
                     {"state", snapshot.completed ? "completed" : "running"},
                     {"output", preview.text},
                     {"exit_status", snapshot.exitCode ? json(*snapshot.exitCode) : json(nullptr)},
                     {"exit_code", snapshot.exitCode ? json(*snapshot.exitCode) : json(nullptr)},
                     {"timed_out", snapshot.timedOut},
                     {"canceled", snapshot.canceled},
                     {"terminated", snapshot.terminated},
                     {"output_truncated", execution.outputTruncated},
                     {"discarded_bytes", snapshot.discardedBytes},
                     {"ring_discarded_bytes", snapshot.ringDiscardedBytes},
                     {"total_output_bytes", snapshot.totalOutputBytes},
                     {"has_more_output", snapshot.hasMoreOutput},
                     {"output_token_estimate", (preview.text.size() + 3) / 4},
                     {"output_token_estimate_method", "utf8_bytes_divided_by_4_approximation"}};
    metadata["shell"] = snapshot.shellPath;
    metadata["shell_name"] = snapshot.shellName;
    metadata["login"] = snapshot.login;
    metadata["exit_status_scope"] = "shell_process";
    metadata["pipeline_stage_status"] = "not_observed";
    metadata["output_streams"] = "combined_stdout_stderr";
    metadata["pipeline_status_note"] =
        "Per-stage statuses are not observed; a zero shell status does not prove every producer "
        "succeeded. Native shell options and script-level error handling determine status. Output "
        "combines "
        "stdout and stderr.";
    metadata["output_preview_policy"] = "head_tail";
    metadata["preview_omitted_bytes"] = preview.omittedBytes;
    metadata["preview_omission_unit"] = "preview_source_bytes";
    if (!snapshot.previewTail.empty()) {
        metadata["preview_tail_from_stream"] = true;
        metadata["preview_tail_offset"] = snapshot.previewTailOffset;
        metadata["preview_gap_bytes"] = snapshot.previewTailOffset > kMaiToolOutputMaxBytes
                                            ? snapshot.previewTailOffset - kMaiToolOutputMaxBytes
                                            : 0;
    }
    metadata["invalid_source_bytes_escaped"] = preview.escapedBytes;
    if (saved) metadata["output_id"] = execution.outputId;
    if (!snapshot.completed) {
        execution.nextAction =
            process.canceled ? MaiToolNextAction::InspectState : MaiToolNextAction::QueryStatus;
        execution.recoveryTool = "shell_input";
        auto result = process.canceled
                          ? MaiToolResult::failure(MaiErrorCode::Canceled, metadata.dump(), true)
                          : MaiToolResult::success(metadata.dump(), execution.outputTruncated);
        return result.withExecution(std::move(execution));
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
    } else if (snapshot.terminated) {
        out +=
            "\n\n[the process tree was explicitly terminated; effects may already have occurred. "
            "Inspect current state.]";
    } else if (failure.hasError()) {
        out += "\n\n[process execution or collection failed after launch; inspect current state]";
    } else if (process.exitCode != 0) {
        out += "\n\n[exit code " + std::to_string(process.exitCode) + "]";
    }
    if (process.truncated) {
        out +=
            "\n[output capture truncated at the 16 MiB prefix limit; the preview may also "
            "show the live stream tail. Raw output snapshots do not contain synthesized "
            "omission markers. Check has_more_output and discarded_bytes; use shell_input "
            "for additional retained tail data. Lost collection gaps cannot be recovered.]";
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

    if (process.canceled || process.timedOut || snapshot.terminated || failure.hasError() ||
        (!saved && execution.outputTruncated)) {
        execution.nextAction = MaiToolNextAction::InspectState;
    } else if (execution.outputTruncated && saved) {
        execution.nextAction = MaiToolNextAction::ReadOutput;
        execution.recoveryTool = "tool_output_read";
    }
    metadata.erase("output");
    if (snapshot.hasMoreOutput)
        out +=
            "\n[Additional retained tail bytes remain; call shell_input with this session_id. Do "
            "not relaunch.]";
    if (snapshot.discardedBytes)
        out += "\n[" + std::to_string(snapshot.discardedBytes) +
               " unread output bytes were discarded and cannot be recovered.]";
    out += "\n\n[shell_result]\n" + metadata.dump();
    MaiToolResult result =
        process.canceled ? MaiToolResult::failure(MaiErrorCode::Canceled, std::move(out),
                                                  execution.outputTruncated)
        : failure.hasError()
            ? MaiToolResult::failure(failure.code(), std::move(out), execution.outputTruncated)
            : MaiToolResult::success(std::move(out), execution.outputTruncated);
    result.withExecution(std::move(execution));
    return result;
}

class ShellTool final : public MaiTool {
public:
    explicit ShellTool(std::shared_ptr<MaiShellSessionManager> manager)
        : mManager(std::move(manager)) {}
    std::string name() const override {
        return "shell";
    }

    std::string description() const override {
        // Shell 本身也是内置核心能力，用于通用命令与脚本；结构化文件操作仍优先复用文件工具。
        return "Run system commands and scripts in the selected shell and working directory, "
               "returning combined stdout and stderr or a live process session. Prefer the "
               "structured file tools for reading, editing, copying, moving and searching files. "
               "Read-only commands may run without approval; other commands follow the selected "
               "Agent approval policy. "
               "workdir is resolved under the workspace policy. timeout_ms is a hard process-tree "
               "limit (default 120000, clamped 1000..600000). yield_time_ms defaults to 10000 "
               "(0..30000); running commands return JSON session_id/state/output. Continue using "
               "shell_input in later turns. max_output_tokens uses a four-bytes-per-token "
               "approximation, capped at a 48 KiB preview. Completed output keeps the existing "
               "format plus a shell_result footer containing exit_status. Oversized previews "
               "retain "
               "both the beginning and end with an omission notice. shell selects zsh/bash/sh, "
               "pwsh/powershell or cmd; omitted shell uses account discovery (Windows prefers "
               "PowerShell). login defaults to true; false selects non-login mode, including "
               "-NoProfile for PowerShell. "
               "Use command syntax appropriate for that interpreter. Like Codex, preserve native "
               "shell status rules without automatically enabling pipefail. exit_status belongs "
               "to the shell process, not every "
               "pipeline stage. Inspect combined stdout/stderr before interpreting a zero count; "
               "scripts may explicitly enable pipefail or handle failures. No PTY or shell reuse.";
    }

    std::string parametersSchema() const override {
        // command 是完整命令行；timeout_ms 为有界等待，执行前还要过 shell 审批。
        return R"({"type":"object","properties":{"command":{"type":"string"},)"
               R"("timeout_ms":{"type":"integer","default":120000},"workdir":{"type":"string"},)"
               R"("yield_time_ms":{"type":"integer","minimum":0,"maximum":30000,"default":10000},)"
               R"("shell":{"type":"string","description":"Shell name or path selecting a supported interpreter type; omitted discovers the account default"},)"
               R"("login":{"type":"boolean","default":true,"description":"Use login startup; false selects non-login mode; PowerShell uses -NoProfile"},)"
               R"("max_output_tokens":{"type":"integer","minimum":1,"default":10000}},)"
               R"("required":["command"],"additionalProperties":false})";
    }

    // 只有能确定是只读的才放行。看不懂、解析不了、带控制字符——一律问。
    // 这一层的兜底方向永远是「不放行」。
    bool requiresApproval(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        return maiShellCommandRequiresApproval(commandOf(args));
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
        const auto valid = validateSessionArguments(args, false);
        if (!valid.empty()) return beforeStartFailure(MaiErrorCode::InvalidInput, valid);
        if (commandOf(args).empty())
            return beforeStartFailure(MaiErrorCode::InvalidInput,
                                      "the command parameter is required");
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
        options.timeoutMs =
            boundedInteger(args, "timeout_ms", kDefaultTimeoutMs, 1000, kMaxTimeoutMs);
        const auto canceled = [&context] { return context.isCanceled(); };
        const auto started = mManager->start(context.sessionId, options, canceled);
        if (!started) return beforeStartFailure(started.error().code(), started.error().message());
        const auto snapshot = mManager->interact(
            context.sessionId, started.value(), {},
            boundedInteger(args, "yield_time_ms", 10000, 0, 30000), false, canceled, true);
        if (!snapshot) return afterStartFailure(snapshot.error(), started.value());
        return renderSession(snapshot.value(), previewLimit(args), options.timeoutMs, context);
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
        // 同一命令换解释器或登录模式可能有完全不同的语义；批准不能跨模式复用。
        return {"shell:scope:" +
                json::array({approvalKey(raw), directory,
                             resolved ? resolved.value().path : "unresolved", login})
                    .dump()};
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

class ShellInputTool final : public MaiTool {
public:
    explicit ShellInputTool(std::shared_ptr<MaiShellSessionManager> manager)
        : mManager(std::move(manager)) {}
    std::string name() const override {
        return "shell_input";
    }
    std::string description() const override {
        return "Send stdin or poll one shell process owned by this Agent session. session_id is "
               "returned by shell. Empty chars only polls, without approval; nonempty stdin and "
               "explicit terminate follow the approval policy for this process, never a program. "
               "yield_time_ms is 0..30000 for writes/termination and 0..300000 for empty polls, "
               "default 250 for writes and 5000 for polls. "
               "terminate=true stops the owned process tree; do not combine it with chars. "
               "Output is incremental, with explicit lost-byte counters; clipped retained chunks "
               "can be read using tool_output_read when an output_id is returned. "
               "max_output_tokens is an approximate four-byte budget capped at 48 KiB. "
               "exit_status describes the shell process; per-pipeline-stage statuses are not "
               "observed, and output combines stdout/stderr. No PTY.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"session_id":{"type":"string"},)"
               R"("chars":{"type":"string"},"terminate":{"type":"boolean","default":false},)"
               R"("yield_time_ms":{"type":"integer","minimum":0,"maximum":300000},)"
               R"("max_output_tokens":{"type":"integer","minimum":1,"default":10000}},)"
               R"("required":["session_id"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string& raw) const override {
        const auto args = parseArguments(raw);
        return !validateSessionArguments(args, true).empty() || args.value("terminate", false) ||
               !args.value("chars", std::string{}).empty();
    }
    std::string approvalKey(const std::string& raw) const override {
        const auto args = parseArguments(raw);
        return "shell_input:" + args.dump();
    }
    std::vector<std::string> approvalKeys(const std::string& raw,
                                          const MaiToolContext& context) const override {
        return {"shell_input:owner:" + context.sessionId + ":" + approvalKey(raw)};
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const auto args = parseArguments(raw);
        const auto invalid = validateSessionArguments(args, true);
        if (!invalid.empty()) return beforeStartFailure(MaiErrorCode::InvalidInput, invalid);
        const auto chars = args.value("chars", std::string{});
        const auto snapshot = mManager->interact(
            context.sessionId, args["session_id"].get<std::string>(), chars,
            boundedInteger(args, "yield_time_ms", chars.empty() ? 5000 : 250, 0,
                           chars.empty() && !args.value("terminate", false) ? 300000 : 30000),
            args.value("terminate", false), [&context] { return context.isCanceled(); });
        if (!snapshot)
            return beforeStartFailure(snapshot.error().code(), snapshot.error().message());
        return renderSession(snapshot.value(), previewLimit(args), kDefaultTimeoutMs, context);
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

bool maiShellCommandRequiresApproval(const std::string& command) {
    return shellCommandRequiresApproval(command);
}

std::unique_ptr<MaiTool> makeMaiShellTool() {
    return std::make_unique<ShellTool>(std::make_shared<MaiShellSessionManager>());
}
std::unique_ptr<MaiTool> makeMaiShellTool(std::shared_ptr<MaiShellSessionManager> manager) {
    return std::make_unique<ShellTool>(manager ? std::move(manager)
                                               : std::make_shared<MaiShellSessionManager>());
}
std::unique_ptr<MaiTool> makeMaiShellInputTool(std::shared_ptr<MaiShellSessionManager> manager) {
    return std::make_unique<ShellInputTool>(manager ? std::move(manager)
                                                    : std::make_shared<MaiShellSessionManager>());
}
void registerMaiShellTools(MaiToolRegistry& registry) {
    if (!maiIsProcessExecutionSupported()) return;
    auto manager = std::make_shared<MaiShellSessionManager>();
    registry.add(makeMaiShellTool(manager));
    registry.add(makeMaiShellInputTool(std::move(manager)));
}
