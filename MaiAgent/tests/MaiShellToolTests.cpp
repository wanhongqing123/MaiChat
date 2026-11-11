// 文件作用：验证“在权限边界内执行 shell 命令并收集结果”的正确行为和边界条件。
// 验证入口：test_read_only_commands_run_without_asking、test_anything_that_can_change_things_asks_first、
// test_shell_metacharacters_always_ask；检查可观察结果而非实现细节。
// shell 工具的测试。
//
// 审批粒度与执行事实分别验证：权限不得扩大，长输出必须能补读，
// 中断不得丢失已有诊断，也不能让模型误以为命令没有产生副作用。
//
// 这些用例真的会起子进程。挑的都是三个平台都有的命令（echo / 一个不存在的名字），
// 不挑 ls 或 dir——那俩各平台只有一个。
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiMemoryStore.h"
#include "MaiProcess.h"
#include "MaiTool.h"
#include "MaiShellTool.h"

namespace fs = std::filesystem;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

namespace {

std::string executablePath;

std::string args(const json& j) {
    return j.dump();
}

// 一条跑很久的命令，用来验超时和取消。
//
// **不能用 ping 或 timeout 拖时间**：它们在 System32 里，而测试跑在什么 PATH 下不好说——
// 实测这台机器上 ping 就不在 PATH 上，命令瞬间失败，于是「超时」那条用例
// 变成在测「命令找不到」，恒真通过。busy 循环只用 cmd / sh 的内置语法，不依赖任何 PATH。
#if defined(_WIN32)
const char* kSlowCommand = "for /L %i in (1,1,2000000000) do @rem";
#else
const char* kSlowCommand = "i=0; while [ $i -lt 2000000000 ]; do i=$((i+1)); done";
#endif

struct Workspace {
    fs::path root;

    Workspace() {
        root = fs::temp_directory_path() / ("maiagent-shell-" + std::to_string(std::rand()));
        fs::create_directories(root);
    }

    ~Workspace() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_shell";
        c.root = root.string();
        return c;
    }
};

struct StoredCall {
    std::unique_ptr<MaiSessionStore> store = makeMaiMemoryStore();
    MaiToolContext context;

    explicit StoredCall(const Workspace& workspace) : context(workspace.context()) {
        MaiSession session;
        session.id = context.sessionId;
        store->putSession(session);
        MaiToolPart tool;
        tool.tool = "shell";
        tool.callId = "call_shell";
        MaiMessage message;
        message.id = "msg_shell";
        message.role = MaiRole::Assistant;
        message.parts.push_back({"prt_shell", tool, 0});
        store->putMessage(session.id, message);
        context.messageId = message.id;
        context.partId = message.parts.front().id;
        context.callId = tool.callId;
        context.toolOutputStore = store.get();
    }
};

std::size_t executionCount(const Workspace& workspace) {
    std::ifstream counter(workspace.root / "execution-counter.txt", std::ios::binary);
    CHECK(counter.is_open());
    const std::string counterText((std::istreambuf_iterator<char>(counter)),
                                  std::istreambuf_iterator<char>());
    std::size_t executions = 0;
    for (std::size_t offset = 0; (offset = counterText.find("run", offset)) != std::string::npos;
         offset += 3) {
        ++executions;
    }
    return executions;
}

std::string readAll(const StoredCall& call, const MaiToolResult& result) {
    CHECK(result.execution().has_value());
    if (!result.execution()) return {};
    CHECK(!result.execution()->outputId.empty());
    if (result.execution()->outputId.empty()) return {};
    MaiToolOutputQuery query;
    query.outputId = result.execution()->outputId;
    std::string output;
    do {
        const auto page = call.store->readToolOutput(call.context.sessionId, query);
        CHECK(page.isOk());
        if (!page.isOk()) break;
        CHECK(page.value().sourceBytesRead <= query.maxBytes);
        output += page.value().text;
        query.offsetBytes = page.value().nextOffsetBytes;
    } while (query.offsetBytes != 0);
    return output;
}

std::string childCommand(const char* mode) {
    return "\"" + executablePath + "\" " + mode;
}

int runCaptureChild(const std::string& mode) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char buffer[4096];
    std::fill(std::begin(buffer), std::end(buffer), 'a');
    if (mode == "--capture-child-oversized") {
        for (int i = 0; i < 4097; ++i) std::fwrite(buffer, 1, sizeof(buffer), stdout);
    } else if (mode == "--capture-child-continuous") {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            std::fwrite(buffer, 1, sizeof(buffer), stdout);
        } while (std::chrono::steady_clock::now() < until);
    } else if (mode == "--capture-child-partial") {
        std::printf("captured-before-interruption\n");
        // stdout 无缓冲；就绪文件只在字节已提交给管道之后创建，取消测试据此同步。
        if (FILE* ready = std::fopen("capture-child-ready", "wb")) {
            std::fputs("ready", ready);
            std::fclose(ready);
        }
        std::this_thread::sleep_for(std::chrono::seconds(4));
    } else if (mode == "--capture-child-utf8") {
        for (int i = 0; i < 11; ++i) std::fwrite(buffer, 1, sizeof(buffer), stdout);
        std::fwrite(buffer, 1, sizeof(buffer) - 1, stdout);
        std::printf("\xf0\x9f\x98\x80utf8-final-marker");
    } else if (mode == "--capture-child-utf8-first-scalar") {
        const unsigned char bytes[] = {0xf0, 0x9f, 0x98, 0x80};
        std::fwrite(bytes, 1, sizeof(bytes), stdout);
    } else if (mode == "--capture-child-invalid") {
        const unsigned char bytes[] = {'a', 0xff, 'b'};
        std::fwrite(bytes, 1, sizeof(bytes), stdout);
    } else {
        return 7;
    }
    return 0;
}

// ── 审批：只读的放行，别的都问 ──────────────────────────────────

void test_read_only_commands_run_without_asking() {
    auto tool = makeMaiShellTool();
    const char* const safe[] = {
        "ls -la",    "pwd",        "echo hello",         "cat README.md", "grep -rn foo src",
        "which git", "git status", "git rev-parse HEAD", "git ls-files",  "git describe --always",
    };
    for (const char* command : safe) {
        CHECK(!tool->requiresApproval(args({{"command", command}})));
    }
}

void test_anything_that_can_change_things_asks_first() {
    auto tool = makeMaiShellTool();
    const char* const risky[] = {
        "rm -rf build",
        "npm install",
        "make",
        "python setup.py install",
        "find . -delete",
        "find . -exec rm {} ;",
        "env rm -rf build",
        "tree -o inventory.txt",
        "diff --output=changes.patch old new",
        "rg --pre 'rm -rf build' needle",
        "date -s tomorrow",
        "hostname changed-host",
        "file -C -m custom.magic",
        // git 不是整个放行的：只读子命令放行，写操作照样问。
        "git push",
        "git commit -m x",
        "git reset --hard",
        "git branch new-branch",
        "git branch -D old-branch",
        "git remote remove origin",
        "git tag v1.0.0",
        "git diff --output=changes.patch",
        "git show --ext-diff HEAD",
        "git log -p --ext-diff",
        "git blame --contents replacement.txt file.txt",
        // 改配置是写操作，即使子命令名在只读名单里。
        "git config user.email a@b.c",
        "git config --global user.email a@b.c",
    };
    for (const char* command : risky) {
        CHECK(tool->requiresApproval(args({{"command", command}})));
    }
}

void test_shell_metacharacters_always_ask() {
    // **这条是白名单能成立的前提。**
    //
    // `ls; rm -rf .` 的第一个词是 ls，可它真正干的是 rm。只看第一个词的话
    // 白名单就成了绕过通道——随便找个只读程序打头，后面接什么都放行。
    auto tool = makeMaiShellTool();
    const char* const sneaky[] = {
        "ls; rm -rf .",    "echo hi && rm x",  "pwd | sh",     "cat x > y",
        "echo `rm -rf .`", "echo $(rm -rf .)", "ls\nrm -rf .",
    };
    for (const char* command : sneaky) {
        CHECK(tool->requiresApproval(args({{"command", command}})));
    }
}

void test_unparsable_arguments_ask() {
    // 兜底方向永远是「不放行」。看不懂的东西不能当成安全的。
    auto tool = makeMaiShellTool();
    CHECK(tool->requiresApproval("not json at all"));
    CHECK(tool->requiresApproval("{}"));
    CHECK(tool->requiresApproval(args({{"command", ""}})));
    CHECK(tool->requiresApproval(args({{"command", "   "}})));
    bool rejectedWrongType = false;
    try {
        rejectedWrongType = tool->requiresApproval(R"({"command":42})") &&
                            tool->execute(R"({"command":42})", MaiToolContext{}).hasError();
    } catch (...) {
        rejectedWrongType = false;
    }
    CHECK(rejectedWrongType);
}

void test_session_approval_is_scoped_to_the_exact_command() {
    // 给 `npm test` 点一次「以后都允许」，不该连 `rm -rf` 一起放行。
    auto tool = makeMaiShellTool();
    CHECK(tool->approvalKey(args({{"command", "npm test"}})) == "shell:command:npm test");
    CHECK(tool->approvalKey(args({{"command", "npm run build"}})) == "shell:command:npm run build");
    CHECK(tool->approvalKey(args({{"command", "rm -rf x"}})) == "shell:command:rm -rf x");
    CHECK(tool->approvalKey(args({{"command", "npm test"}})) !=
          tool->approvalKey(args({{"command", "rm -rf x"}})));

    // 同一程序的不同写命令也不能共用豁免。
    CHECK(tool->approvalKey(args({{"command", "/usr/bin/git push"}})) ==
          "shell:command:/usr/bin/git push");
    CHECK(tool->approvalKey(args({{"command", "C:\\\\tools\\\\git.exe push"}})) ==
          "shell:command:C:\\\\tools\\\\git.exe push");

    // 带控制字符的命令按完整命令收窄，不能让一条复合命令的会话授权放行另一条。
    const std::string firstCompound = tool->approvalKey(args({{"command", "ls; rm -rf ."}}));
    const std::string secondCompound =
        tool->approvalKey(args({{"command", "echo hi && rm other"}}));
    CHECK(firstCompound.find("ls; rm -rf .") != std::string::npos);
    CHECK(secondCompound.find("echo hi && rm other") != std::string::npos);
    CHECK(firstCompound != secondCompound);
}

// ── 执行 ────────────────────────────────────────────────────────

void test_runs_a_command_and_returns_its_output() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const MaiToolResult result =
        tool->execute(args({{"command", "echo maiagent-marker"}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("maiagent-marker") != std::string::npos);
}

void test_a_failing_command_is_a_normal_result_not_an_error() {
    // 命令跑失败不是工具失败：模型要看到退出码和 stderr 才知道下一步怎么办。
    // 当成错误抛上去的话，那一轮直接断掉，模型连失败原因都看不到。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const MaiToolResult result =
        tool->execute(args({{"command", "maiagent-no-such-command-xyz"}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("exit code") != std::string::npos);
    CHECK(result.execution().has_value());
    if (!result.execution()) return;
    CHECK(result.execution()->status == MaiToolExecutionStatus::Completed);
    CHECK(result.execution()->effectStatus == MaiToolEffectStatus::Unknown);
    CHECK(result.execution()->started == true);
    CHECK(result.execution()->exitCode.has_value());
    CHECK(result.execution()->exitCode != 0);
}

void test_stderr_is_interleaved_with_stdout() {
    // 编译器把错误写在 stderr、进度写在 stdout。分开收的话模型对不上号，
    // 所以两股合成一股。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "echo out-marker && maiagent-no-such-command-xyz";
#else
    const char* command = "echo out-marker; maiagent-no-such-command-xyz";
#endif
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("out-marker") != std::string::npos);
    // 找不到命令这句话是 shell 写到 stderr 上的。收不到就说明两股没合。
    CHECK(result.output().size() > std::string("out-marker").size() + 4);
}

void test_the_command_runs_in_the_working_directory() {
    // 不设工作目录的话模型会在**进程的当前目录**里跑命令，而那通常不是用户的项目。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "cd";
#else
    const char* command = "pwd";
#endif
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());
    // 临时目录可能有符号链接（macOS 的 /var -> /private/var），所以只比最后一段。
    const std::string leaf = workspace.root.filename().string();
    CHECK(result.output().find(leaf) != std::string::npos);
}

void test_without_a_working_directory_it_refuses() {
    auto tool = makeMaiShellTool();
    MaiToolContext bare;
    bare.sessionId = "ses_shell";
    const MaiToolResult result = tool->execute(args({{"command", "echo hi"}}), bare);
    CHECK(result.hasError());
    CHECK(result.execution().has_value());
    if (!result.execution()) return;
    CHECK(result.execution()->status == MaiToolExecutionStatus::NotStarted);
    CHECK(result.execution()->effectStatus == MaiToolEffectStatus::None);
    CHECK(result.execution()->started == false);
}

void test_a_hanging_command_is_killed_at_the_timeout() {
    // 模型写出 `tail -f` 这种命令是常事。没有超时的话那一轮就永远不结束。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const char* command = kSlowCommand;
    const auto began = std::chrono::steady_clock::now();
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 1200}}), workspace.context());
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    CHECK(!result.hasError());
    // 到点就杀，不能等它自己跑完 20 秒。
    CHECK(took < 8000);
    // 回给模型的话必须说清是**超时被杀**，不能说成「命令失败了」——
    // 后者会让它以为是代码有问题，然后去改不该改的地方。
    CHECK(result.output().find("timed out") != std::string::npos);
}

void test_cancel_stops_a_running_command() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    std::atomic<bool> cancel{false};
    MaiToolContext context = workspace.context();
    context.cancel = &cancel;

    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.store(true);
    });
    const char* command = kSlowCommand;
    const auto began = std::chrono::steady_clock::now();
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 60000}}), context);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    stopper.join();

    CHECK(took < 8000);
    CHECK(result.hasError());
    CHECK(result.error().code() == MaiErrorCode::Canceled);
}

void test_output_is_capped_and_says_so() {
    // 不封顶的话一条 `find /` 就能把整个上下文顶掉。
    // 封了就必须如实说截断过——不说的话模型会把看到的当成全部。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command = "for /L %i in (1,1,20000) do @echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
#else
    const char* command = "for i in $(seq 1 20000); do echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; done";
#endif
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 30000}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.isTruncated());
    CHECK(result.output().find("truncated") != std::string::npos);
    // 上限是 48KB，加上尾巴那几句提示，总长不该失控。
    CHECK(result.output().size() < 64 * 1024);
}

void test_long_output_can_be_read_without_running_the_command_again() {
    // 计数文件独立于返回文本；补读输出时若误执行原命令，这个文件会立刻暴露副作用。
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    StoredCall call(workspace);
    auto tool = makeMaiShellTool();
#if defined(_WIN32)
    const char* command =
        "echo run>>execution-counter.txt & "
        "for /L %i in (1,1,5000) do @echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa & "
        "echo retained-final-marker";
#else
    const char* command =
        "printf run >> execution-counter.txt; i=0; while [ $i -lt 5000 ]; do "
        "echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; i=$((i+1)); done; "
        "echo retained-final-marker";
#endif
    const MaiToolResult result =
        tool->execute(args({{"command", command}, {"timeout_ms", 30000}}), call.context);
    CHECK(!result.hasError());
    CHECK(executionCount(workspace) == 1);
    CHECK(result.output().size() < 64 * 1024);
    CHECK(result.output().find("tool_output_read") != std::string::npos);
    const std::string captured = readAll(call, result);
    CHECK(captured.size() > 100 * 1024);
    CHECK(captured.find("retained-final-marker") != std::string::npos);
    CHECK(executionCount(workspace) == 1);
    if (!result.execution()) return;
    CHECK(result.execution()->status == MaiToolExecutionStatus::Completed);
    CHECK(result.execution()->effectStatus == MaiToolEffectStatus::Unknown);
    CHECK(result.execution()->nextAction == MaiToolNextAction::ReadOutput);
    CHECK(result.execution()->exitCode == 0);
}

void test_process_launch_failure_is_distinct_from_a_nonzero_exit() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiProcessOptions options;
    options.workingDirectory = workspace.root.string();
    options.argv = {"maiagent-no-such-program-xyz"};
    MaiProcessResult process;
    CHECK(maiRunProcess(options, process).hasError());
    CHECK(!process.started);
    options.argv = {executablePath, "--capture-child-exit-nonzero"};
    CHECK(!maiRunProcess(options, process).hasError());
    CHECK(process.started);
    CHECK(process.exitCode == 7);
    std::atomic<bool> cancel{true};
    options.cancel = &cancel;
    CHECK(!maiRunProcess(options, process).hasError());
    CHECK(!process.started);
    CHECK(process.canceled);
}

void test_continuous_output_obeys_timeout_and_cancellation() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiProcessOptions options;
    options.workingDirectory = workspace.root.string();
    options.argv = {executablePath, "--capture-child-continuous"};
    options.timeoutMs = 200;
    options.maxOutputBytes = 1024;
    MaiProcessResult process;
    auto began = std::chrono::steady_clock::now();
    CHECK(!maiRunProcess(options, process).hasError());
    CHECK(process.started);
    CHECK(process.timedOut);
    CHECK(!process.output.empty());
    CHECK(process.truncated);
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));

    std::atomic<bool> cancel{false};
    options.cancel = &cancel;
    options.timeoutMs = 10000;
    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });
    began = std::chrono::steady_clock::now();
    CHECK(!maiRunProcess(options, process).hasError());
    stopper.join();
    CHECK(process.started);
    CHECK(process.canceled);
    CHECK(!process.output.empty());
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
}

void test_interruption_retains_partial_output_and_requires_state_inspection() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    StoredCall call(workspace);
    auto tool = makeMaiShellTool();
    const MaiToolResult timeout = tool->execute(
        args({{"command", childCommand("--capture-child-partial")}, {"timeout_ms", 1000}}),
        call.context);
    CHECK(!timeout.hasError());
    CHECK(timeout.execution().has_value());
    if (!timeout.execution()) return;
    CHECK(timeout.execution()->timedOut);
    CHECK(timeout.execution()->nextAction == MaiToolNextAction::InspectState);
    CHECK(timeout.execution()->effectStatus == MaiToolEffectStatus::Unknown);
    CHECK(timeout.execution()->outputTruncated);
    CHECK(timeout.isTruncated());
    CHECK(!timeout.execution()->exitCode.has_value());
    CHECK(readAll(call, timeout).find("captured-before-interruption") != std::string::npos);
    MaiToolOutputQuery timeoutQuery;
    timeoutQuery.outputId = timeout.execution()->outputId;
    const auto timeoutPage = call.store->readToolOutput(call.context.sessionId, timeoutQuery);
    CHECK(timeoutPage.isOk());
    if (timeoutPage.isOk()) CHECK(timeoutPage.value().truncated);

    std::atomic<bool> cancel{false};
    call.context.cancel = &cancel;
    std::atomic<bool> readyObserved{false};
    const fs::path readyPath = workspace.root / "capture-child-ready";
    fs::remove(readyPath);
    std::thread stopper([&cancel, &readyObserved, &readyPath] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code error;
            if (fs::exists(readyPath, error)) {
                readyObserved.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        cancel.store(true);
    });
    const MaiToolResult canceled = tool->execute(
        args({{"command", childCommand("--capture-child-partial")}, {"timeout_ms", 10000}}),
        call.context);
    stopper.join();
    CHECK(canceled.hasError());
    CHECK(readyObserved.load());
    CHECK(canceled.error().code() == MaiErrorCode::Canceled);
    CHECK(canceled.execution().has_value());
    if (!canceled.execution()) return;
    CHECK(canceled.execution()->canceled);
    CHECK(canceled.execution()->outputTruncated);
    CHECK(canceled.isTruncated());
    CHECK(canceled.execution()->nextAction == MaiToolNextAction::InspectState);
    CHECK(canceled.error().message().find("captured-before-interruption") != std::string::npos);
    CHECK(readAll(call, canceled).find("captured-before-interruption") != std::string::npos);
    MaiToolOutputQuery canceledQuery;
    canceledQuery.outputId = canceled.execution()->outputId;
    const auto canceledPage = call.store->readToolOutput(call.context.sessionId, canceledQuery);
    CHECK(canceledPage.isOk());
    if (canceledPage.isOk()) CHECK(canceledPage.value().truncated);
}

void test_capture_limit_and_utf8_preview_are_explicit() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    StoredCall call(workspace);
    auto tool = makeMaiShellTool();
    const MaiToolResult oversized =
        tool->execute(args({{"command", childCommand("--capture-child-oversized")}}), call.context);
    CHECK(!oversized.hasError());
    CHECK(oversized.output().find("capture truncated") != std::string::npos);
    CHECK(oversized.execution().has_value());
    if (!oversized.execution()) return;
    MaiToolOutputQuery query;
    query.outputId = oversized.execution()->outputId;
    auto page = call.store->readToolOutput(call.context.sessionId, query);
    CHECK(page.isOk());
    if (page.isOk()) {
        CHECK(page.value().totalBytes == kMaiToolOutputMaxBytes);
        CHECK(page.value().truncated);
    }

    const MaiToolResult utf8 =
        tool->execute(args({{"command", childCommand("--capture-child-utf8")}}), call.context);
    CHECK(!utf8.hasError());
    bool serializable = true;
    try {
        (void)json(utf8.output()).dump();
    } catch (...) {
        serializable = false;
    }
    CHECK(serializable);
    CHECK(utf8.output().find("\xf0\x9f\x98\x80") == std::string::npos);
    CHECK(readAll(call, utf8).find("\xf0\x9f\x98\x80utf8-final-marker") != std::string::npos);
#if defined(_WIN32)
    // Windows 的原始捕获边界也不能将完整 UTF-8 前缀整段误解码为 OEM。
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-utf8"};
    options.workingDirectory = workspace.root.string();
    options.maxOutputBytes = 48 * 1024;
    MaiProcessResult captured;
    CHECK(!maiRunProcess(options, captured).hasError());
    CHECK(captured.truncated);
    CHECK(captured.output == std::string(48 * 1024 - 1, 'a'));
#endif
}

void test_windows_capture_can_end_inside_the_first_utf8_scalar() {
#if defined(_WIN32)
    // 输出开头就是四字节标量；前三字节不能被当成 OEM 文本，完整 UTF-8 前缀为空。
    Workspace workspace;
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-utf8-first-scalar"};
    options.workingDirectory = workspace.root.string();
    const std::size_t limits[] = {3, 0, 1};
    for (const std::size_t limit : limits) {
        options.maxOutputBytes = limit;
        MaiProcessResult captured;
        CHECK(!maiRunProcess(options, captured).hasError());
        CHECK(captured.started);
        CHECK(captured.truncated);
        CHECK(captured.output.empty());
    }
#endif
}

void test_unavailable_or_rejected_storage_does_not_advertise_a_handle() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const std::string command = childCommand("--capture-child-utf8");
    const MaiToolResult unavailable =
        tool->execute(args({{"command", command}}), workspace.context());
    CHECK(unavailable.execution().has_value());
    if (!unavailable.execution()) return;
    CHECK(unavailable.execution()->outputId.empty());
    CHECK(unavailable.output().find("cannot be recovered") != std::string::npos);
    CHECK(unavailable.output().find("tool_output_read") == std::string::npos);

    StoredCall call(workspace);
    call.context.callId = "call_wrong";
    const MaiToolResult rejected = tool->execute(args({{"command", command}}), call.context);
    CHECK(rejected.execution().has_value());
    if (!rejected.execution()) return;
    CHECK(rejected.execution()->outputId.empty());
    CHECK(rejected.output().find("cannot be recovered") != std::string::npos);
    CHECK(rejected.output().find("tool_output_read") == std::string::npos);
}

void test_invalid_posix_bytes_are_retained_and_previewed_truthfully() {
#if !defined(_WIN32)
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    StoredCall call(workspace);
    auto tool = makeMaiShellTool();
    const MaiToolResult result =
        tool->execute(args({{"command", childCommand("--capture-child-invalid")}}), call.context);
    CHECK(!result.hasError());
    CHECK(result.output().find("a\\xFFb") != std::string::npos);
    CHECK(readAll(call, result) == std::string("a\xff"
                                               "b",
                                               3));
#endif
}

void test_windows_shell_output_is_valid_utf8() {
#if defined(_WIN32)
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const std::string command =
        "powershell.exe -NoProfile -NonInteractive -Command "
        R"MAI("$b=[byte[]](0xCF,0xB5,0xCD,0xB3,0xD5,0xD2);)MAI"
        R"MAI([Console]::OpenStandardOutput().Write($b,0,$b.Length)")MAI";
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());

    bool serializable = true;
    try {
        (void)json(result.output()).dump();
    } catch (...) {
        serializable = false;
    }
    CHECK(serializable);
#endif
}

void test_windows_localized_shell_output_is_preserved() {
#if defined(_WIN32)
    Workspace workspace;
    auto tool = makeMaiShellTool();
    const char* command = "echo \u7cfb\u7edf\u627e\u4e0d\u5230\u6307\u5b9a\u7684\u8def\u5f84";
    const MaiToolResult result = tool->execute(args({{"command", command}}), workspace.context());
    CHECK(!result.hasError());
    CHECK(result.output().find("\u7cfb\u7edf\u627e\u4e0d\u5230") != std::string::npos);
#endif
}

void test_shell_is_only_registered_where_processes_are_allowed() {
    // iOS 上跑不了外部进程。摆一个永远失败的工具比不摆更糟：模型会反复试，
    // 而它收到的「失败」听起来像临时故障，于是换个写法再试一遍。
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    const bool listed = registry.find("shell") != nullptr;
    CHECK(listed == maiIsProcessExecutionSupported());

    // 其余工具不受平台影响，哪儿都该在。
    CHECK(registry.find("file_read") != nullptr);
    CHECK(registry.find("file_edit") != nullptr);
    CHECK(registry.find("current_time") != nullptr);
}

}  // namespace

#define RUN(f)                      \
    do {                            \
        std::printf("-> %s\n", #f); \
        std::fflush(stdout);        \
        f();                        \
    } while (0)

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]).find("--capture-child-") == 0) {
        return runCaptureChild(argv[1]);
    }
    executablePath = fs::absolute(argv[0]).string();
    if (argc == 2 && std::string(argv[1]) == "--utf8-small-cap") {
        RUN(test_windows_capture_can_end_inside_the_first_utf8_scalar);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--partial-capture") {
        RUN(test_interruption_retains_partial_output_and_requires_state_inspection);
        return failures == 0 ? 0 : 1;
    }
    RUN(test_read_only_commands_run_without_asking);
    RUN(test_anything_that_can_change_things_asks_first);
    RUN(test_shell_metacharacters_always_ask);
    RUN(test_unparsable_arguments_ask);
    RUN(test_session_approval_is_scoped_to_the_exact_command);
    RUN(test_runs_a_command_and_returns_its_output);
    RUN(test_a_failing_command_is_a_normal_result_not_an_error);
    RUN(test_stderr_is_interleaved_with_stdout);
    RUN(test_the_command_runs_in_the_working_directory);
    RUN(test_without_a_working_directory_it_refuses);
    RUN(test_a_hanging_command_is_killed_at_the_timeout);
    RUN(test_cancel_stops_a_running_command);
    RUN(test_output_is_capped_and_says_so);
    RUN(test_long_output_can_be_read_without_running_the_command_again);
    RUN(test_process_launch_failure_is_distinct_from_a_nonzero_exit);
    RUN(test_continuous_output_obeys_timeout_and_cancellation);
    RUN(test_interruption_retains_partial_output_and_requires_state_inspection);
    RUN(test_capture_limit_and_utf8_preview_are_explicit);
    RUN(test_windows_capture_can_end_inside_the_first_utf8_scalar);
    RUN(test_unavailable_or_rejected_storage_does_not_advertise_a_handle);
    RUN(test_invalid_posix_bytes_are_retained_and_previewed_truthfully);
    RUN(test_windows_shell_output_is_valid_utf8);
    RUN(test_windows_localized_shell_output_is_preserved);
    RUN(test_shell_is_only_registered_where_processes_are_allowed);
    if (failures == 0) std::printf("shell tool tests passed\n");
    return failures == 0 ? 0 : 1;
}
