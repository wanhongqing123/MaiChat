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
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <json.hpp>

#include "MaiMemoryStore.h"
#include "MaiAgent.h"
#include "MaiFileSystem.h"
#include "MaiProcess.h"
#include "MaiTool.h"
#include "MaiShellTool.h"
#include "MaiShellSessionManager.h"
#include "MaiShellEnvironment.h"

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

std::string legacyShellPath() {
#if defined(_WIN32)
    wchar_t directory[32768];
    const auto length = ::GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
    CHECK(length > 0 && length < std::size(directory));
    return length > 0 && length < std::size(directory)
               ? MaiFilePath(std::wstring(directory, length))
                     .append(MaiFilePath(L"cmd.exe"))
                     .toUtf8()
               : std::string{};
#else
    return "/bin/sh";
#endif
}

std::string args(const json& j) {
    auto request = j;
    // 旧语法和本机子进程素材固定解释器，避免默认 PowerShell 切换污染旧回归。
    // 真正的默认解释器测试直接构造 JSON，不经过此兼容素材适配器。
    if (request.contains("command")) {
        if (!request.contains("shell")) request["shell"] = legacyShellPath();
        if (!request.contains("login")) request["login"] = false;
    }
    return request.dump();
}

struct Workspace {
    MaiFilePath root;

    Workspace() {
        static std::atomic<unsigned> sequence{0};
        const std::string unique =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++sequence);
        root = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("maiagent-shell-" + unique));
        CHECK(!MaiFileSystem::exists(root));
        CHECK(!MaiFileSystem::createDirectories(root));
    }

    ~Workspace() {
        MaiDirectoryDeleteOptions cleanup;
        cleanup.recursive = true;
        cleanup.pathAllowed = [this](const MaiFilePath& path) {
            return path == root || root.isParentOf(path);
        };
        const auto removed = MaiFileSystem::deleteDirectory(root, cleanup);
        if (removed.error)
            std::printf(
                "Owned fixture cleanup failed: leaf=%s code=%s files=%zu directories=%zu; %s\n",
                root.baseName().toUtf8().c_str(), maiErrorCodeToString(removed.error.code()),
                removed.removedFiles, removed.removedDirectories, removed.error.message().c_str());
        CHECK(!removed.error);
    }

    MaiToolContext context() const {
        MaiToolContext c;
        c.sessionId = "ses_shell";
        c.root = root.toUtf8();
        return c;
    }
};

class ScopedOwnedEnvironment {
public:
    ScopedOwnedEnvironment(std::string name, const std::string& value, bool prependPath = false);
    ~ScopedOwnedEnvironment();

private:
    std::string mName;
    std::string mOriginal;
    bool mPresent = false;
};
ScopedOwnedEnvironment::ScopedOwnedEnvironment(std::string name, const std::string& value,
                                               bool prependPath)
    : mName(std::move(name)) {
#if defined(_WIN32)
    const auto nativeName = MaiFilePath::fromUtf8(mName).value();
    ::SetLastError(ERROR_SUCCESS);
    const auto size = ::GetEnvironmentVariableW(nativeName.c_str(), nullptr, 0);
    mPresent = size != 0 || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    std::vector<wchar_t> original(size ? size : 1, L'\0');
    if (size) CHECK(::GetEnvironmentVariableW(nativeName.c_str(), original.data(), size) < size);
    mOriginal = MaiFilePath(std::wstring(original.data())).toUtf8();
    const auto next = prependPath ? value + ";" + mOriginal : value;
    CHECK(
        ::SetEnvironmentVariableW(nativeName.c_str(), MaiFilePath::fromUtf8(next).value().c_str()));
#else
    const auto* original = ::getenv(mName.c_str());
    mPresent = original != nullptr;
    if (original) mOriginal = original;
    const auto next = prependPath ? value + ":" + mOriginal : value;
    CHECK(::setenv(mName.c_str(), next.c_str(), 1) == 0);
#endif
}
ScopedOwnedEnvironment::~ScopedOwnedEnvironment() {
#if defined(_WIN32)
    const auto name = MaiFilePath::fromUtf8(mName).value();
    const auto original = MaiFilePath::fromUtf8(mOriginal).value();
    CHECK(::SetEnvironmentVariableW(name.c_str(), mPresent ? original.c_str() : nullptr));
#else
    CHECK((mPresent ? ::setenv(mName.c_str(), mOriginal.c_str(), 1) : ::unsetenv(mName.c_str())) ==
          0);
#endif
}

struct StoredCall {
    std::unique_ptr<MaiSessionStore> store = makeMaiMemoryStore();
    MaiToolContext context;

    explicit StoredCall(const Workspace& workspace, const std::string& toolName = "shell")
        : context(workspace.context()) {
        MaiSession session;
        session.id = context.sessionId;
        store->putSession(session);
        MaiToolPart tool;
        tool.tool = toolName;
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
    std::string counterText;
    CHECK(!MaiFileSystem::readFile(
        workspace.root.append(MaiFilePath::fromUtf8("execution-counter.txt")), counterText));
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

std::uint64_t nativeProcessId() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

bool processIsAlive(std::uint64_t processId) {
    if (!processId) return false;
#if defined(_WIN32)
    const HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         static_cast<DWORD>(processId));
    if (!process) return false;
    const bool running = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(processId), 0) == 0;
#endif
}

std::uint64_t spawnMarkerDescendant() {
#if defined(_WIN32)
    const auto executable = MaiFilePath::fromUtf8(executablePath).value();
    std::wstring command = L"\"" + executable + L"\" --capture-child-marker";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return 0;
    const auto processId = process.dwProcessId;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return processId;
#else
    const auto process = ::fork();
    if (process == 0) {
        ::execl(executablePath.c_str(), executablePath.c_str(), "--capture-child-marker",
                static_cast<char*>(nullptr));
        ::_exit(11);
    }
    return process > 0 ? static_cast<std::uint64_t>(process) : 0;
#endif
}

int runInteractiveChild(bool descendant) {
    std::printf("native-pid=%llu\n", static_cast<unsigned long long>(nativeProcessId()));
    if (descendant)
        std::printf("descendant-pid=%llu\n",
                    static_cast<unsigned long long>(spawnMarkerDescendant()));
    std::puts("native-ready");
    char line[4096];
    while (std::fgets(line, sizeof(line), stdin)) {
        const std::string request(line);
        if (request == "exit\n" || request == "exit\r\n") return 0;
        if (request == "utf8-fragment\n" || request == "utf8-fragment\r\n") {
            const unsigned char fragment[] = {0xf0};
            std::fwrite(fragment, 1, sizeof(fragment), stdout);
            continue;
        }
        if (request == "flood\n" || request == "flood\r\n") {
            const std::string block(4096, 'x');
            for (unsigned index = 0; index < 6000; ++index)
                std::fwrite(block.data(), 1, block.size(), stdout);
            std::puts("native-flood-final-marker");
            if (MaiFileSystem::writeFile(MaiFilePath::fromUtf8("flood-complete"), "ready"))
                return 12;
        } else {
            if (request == "write-marker\n" || request == "write-marker\r\n") {
                if (MaiFileSystem::writeFile(MaiFilePath::fromUtf8("stdin-write-marker"),
                                             "written"))
                    return 15;
            }
            std::printf("native-input:%s", line);
        }
    }
    return 0;
}

int runOwnedBashStartupChild();
int runOwnedShStartupChild();
int runOwnedPipelineChild();

int runCaptureChild(const std::string& mode) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char buffer[4096];
    std::fill(std::begin(buffer), std::end(buffer), 'a');
    if (mode == "--capture-child-bash-startup") return runOwnedBashStartupChild();
    if (mode == "--capture-child-sh-startup") return runOwnedShStartupChild();
    if (mode == "--capture-child-pipeline") return runOwnedPipelineChild();
    if (mode == "--capture-child-session") return runInteractiveChild(false);
    if (mode == "--capture-child-descendant") return runInteractiveChild(true);
    if (mode == "--capture-child-big-head-tail") {
#if defined(_WIN32)
        if (::_setmode(::_fileno(stdout), _O_BINARY) < 0) return 24;
#endif
        std::puts("BIG-HEAD-owned");
        const std::string block(4096, 'B');
        for (unsigned index = 0; index < 6000; ++index)
            std::fwrite(block.data(), 1, block.size(), stdout);
        std::puts("\nBIG-TAIL-owned-error");
        return 0;
    }
    if (mode == "--capture-child-head-tail") {
#if defined(_WIN32)
        // 此素材约定精确 UTF-8 字节，不让 CRT 把 LF 隐式改成 CRLF。
        if (::_setmode(::_fileno(stdout), _O_BINARY) < 0) return 16;
#endif
        std::puts("HEAD-parity-marker \xf0\x9f\x99\x82");
        const std::string scalar = "\xf0\x9f\x99\x82";
        for (unsigned index = 0; index < 10000; ++index)
            std::fwrite(scalar.data(), 1, scalar.size(), stdout);
        std::puts("\nTAIL-parity-error \xe4\xb8\xad");
        return 0;
    }
    if (mode == "--capture-child-utf8-incomplete") {
        const unsigned char fragment[] = {0xe4, 0xb8};
        std::fwrite(fragment, 1, sizeof(fragment), stdout);
        return 0;
    }
    if (mode == "--capture-child-marker") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        return MaiFileSystem::writeFile(MaiFilePath::fromUtf8("descendant-marker"), "still-alive")
                   ? 13
                   : 0;
    }
    if (mode == "--capture-child-sleep") {
        std::puts("native-sleep-ready");
        std::this_thread::sleep_for(std::chrono::seconds(4));
    } else if (mode == "--capture-child-oem") {
        const unsigned char bytes[] = {0xCF, 0xB5, 0xCD, 0xB3, 0xD5, 0xD2};
        std::fwrite(bytes, 1, sizeof(bytes), stdout);
    } else if (mode == "--capture-child-cap" || mode == "--capture-child-once-long") {
        if (mode == "--capture-child-once-long") {
            std::string counter;
            (void)MaiFileSystem::readFile(MaiFilePath::fromUtf8("execution-counter.txt"), counter);
            if (MaiFileSystem::writeFile(MaiFilePath::fromUtf8("execution-counter.txt"),
                                         counter + "run\n"))
                return 14;
        }
        for (unsigned index = 0; index < 600; ++index)
            std::fwrite(buffer, 1, sizeof(buffer), stdout);
        std::puts("retained-final-marker");
    } else if (mode == "--capture-child-workdir") {
        const auto error =
            MaiFileSystem::writeFile(MaiFilePath::fromUtf8("workdir-marker"), "native-child");
        if (error) return 9;
        std::printf("native-workdir=%s\n",
                    MaiFileSystem::resolve(MaiFilePath::fromUtf8(".")).toUtf8().c_str());
    } else if (mode == "--capture-child-oversized") {
        for (int i = 0; i < 4097; ++i) std::fwrite(buffer, 1, sizeof(buffer), stdout);
    } else if (mode == "--capture-child-continuous") {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            std::fwrite(buffer, 1, sizeof(buffer), stdout);
        } while (std::chrono::steady_clock::now() < until);
    } else if (mode == "--capture-child-partial") {
        std::printf("captured-before-interruption\n");
        // stdout 无缓冲；就绪文件只在字节已提交给管道之后创建，取消测试据此同步。
        if (MaiFileSystem::writeFile(MaiFilePath::fromUtf8("capture-child-ready"), "ready"))
            return 10;
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

void test_batch6_builtin_schema_and_workdir_contract() {
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    CHECK((registry.find("shell_input") != nullptr) == maiIsProcessExecutionSupported());
    if (!shell) return;
    const auto schema = json::parse(shell->parametersSchema());
    for (const char* key : {"workdir", "yield_time_ms", "max_output_tokens"})
        CHECK(schema["properties"].contains(key));
    CHECK(schema["properties"]["yield_time_ms"].value("default", 0) == 10000);
    CHECK(schema["properties"]["max_output_tokens"].value("default", 0) == 10000);
    CHECK(schema["properties"]["timeout_ms"].value("default", 0) == 120000);
    Workspace workspace;
    const auto root = MaiFilePath::fromUtf8(workspace.root.toUtf8());
    CHECK(!MaiFileSystem::createDirectories(root.append(MaiFilePath::fromUtf8("nested"))));
    const auto result = shell->execute(args({{"command", childCommand("--capture-child-workdir")},
                                             {"workdir", "nested"},
                                             {"yield_time_ms", 1000},
                                             {"max_output_tokens", 1000}}),
                                       workspace.context());
    CHECK(!result.hasError());
    CHECK(MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("nested/workdir-marker"))));
    CHECK(!MaiFileSystem::exists(root.append(MaiFilePath::fromUtf8("workdir-marker"))));
}

void test_explicit_cmd_preserves_quoted_executable_and_argument() {
#if defined(_WIN32)
    Workspace workspace;
    auto shell = makeMaiShellTool();
    const auto result = shell->execute(
        args(
            {{"command", R"MAI("D:/Program Files/CMake/bin/cmake.exe" -E echo "owned space")MAI"}}),
        workspace.context());
    CHECK(!result.hasError());
    CHECK(result.execution() && result.execution()->exitCode == 0);
    CHECK(result.output().find("owned space") != std::string::npos);
#endif
}

void test_powershell_login_startup_restores_approved_workdir() {
    const MaiShellEnvironment fake{MaiShellType::PowerShell, "pwsh", "/owned/pwsh"};
    const auto prepared =
        maiShellStartupCommand(fake, "Write-Output owned-cwd", true, "C:/owned/target a'b");
    CHECK(
        prepared.find(
            R"MAI(Microsoft.PowerShell.Management\Set-Location -LiteralPath 'C:/owned/target a''b' -ErrorAction Stop)MAI") !=
        std::string::npos);
    CHECK(prepared.find("\nWrite-Output owned-cwd") != std::string::npos);
#if defined(_WIN32)
    const auto powershell = maiResolveShellEnvironment();
    CHECK(powershell && powershell.value().type == MaiShellType::PowerShell);
    if (!powershell || powershell.value().type != MaiShellType::PowerShell) return;
    Workspace workspace;
    const auto target = workspace.root.append(MaiFilePath::fromUtf8("ps-target a'b"));
    const auto elsewhere = workspace.root.append(MaiFilePath::fromUtf8("elsewhere"));
    CHECK(!MaiFileSystem::createDirectories(target));
    CHECK(!MaiFileSystem::createDirectories(elsewhere));
    const auto quoted = [](const MaiFilePath& path) {
        std::string value = "'";
        for (char character : path.toGenericUtf8())
            value += character == '\'' ? "''" : std::string(1, character);
        return value + "'";
    };
    const std::string command =
        "if ((Microsoft.PowerShell.Management\\Get-Location).Path -ne [IO.Path]::GetFullPath(" +
        quoted(target) + ")) { exit 21 }; [Console]::Write('owned-ps-cwd-restored')";
    // 模拟自有启动脚本改变目录；仍以 -NoProfile 启动，绝不执行主机 profile。
    const auto script =
        "Microsoft.PowerShell.Management\\Set-Location -LiteralPath " + quoted(elsewhere) + ";\n" +
        maiShellStartupCommand(powershell.value(), command, true, target.toGenericUtf8());
    MaiProcessOptions options;
    options.argv = maiShellCommandArguments(powershell.value(), script, false);
    options.workingDirectory = workspace.root.toUtf8();
    options.timeoutMs = 5000;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    CHECK(result.output.find("owned-ps-cwd-restored") != std::string::npos);
    const auto missing = workspace.root.append(MaiFilePath::fromUtf8("missing-owned a'b"));
    const auto unexpected =
        workspace.root.append(MaiFilePath::fromUtf8("unexpected-command-write"));
    const auto shouldNotRun = "Microsoft.PowerShell.Management\\Set-Content -LiteralPath " +
                              quoted(unexpected) + " -Value owned";
    const auto guarded =
        "trap { continue }; Microsoft.PowerShell.Management\\Set-Location -LiteralPath " +
        quoted(elsewhere) + ";\n" +
        maiShellStartupCommand(powershell.value(), shouldNotRun, true, missing.toGenericUtf8());
    options.argv = maiShellCommandArguments(powershell.value(), guarded, false);
    result = {};
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode != 0);
    CHECK(!MaiFileSystem::exists(unexpected));
#endif
}

void test_shell_startup_script_refreshes_rc_and_restores_workdir() {
    const std::string command = "printf owned-command";
    const std::string workdir = "/owned startup-dir";
    const MaiShellEnvironment bash{MaiShellType::Bash, "bash", "/owned/bash"};
    const MaiShellEnvironment zsh{MaiShellType::Zsh, "zsh", "/owned/zsh"};
    const MaiShellEnvironment sh{MaiShellType::Sh, "sh", "/owned/sh"};
    const auto bashScript = maiShellStartupCommand(bash, command, true, workdir);
    CHECK(bashScript.find(".bashrc") != std::string::npos);
    CHECK(bashScript.find("BASH_ENV") != std::string::npos);
    CHECK(bashScript.find(workdir) != std::string::npos);
    CHECK(bashScript.find("cd ") < bashScript.rfind(command));
    const auto zshScript = maiShellStartupCommand(zsh, command, true, workdir);
    CHECK(zshScript.find(".zshrc") != std::string::npos);
    CHECK(zshScript.find("ZDOTDIR") != std::string::npos);
    CHECK(zshScript.find(workdir) != std::string::npos);
    CHECK(zshScript.find("cd ") < zshScript.rfind(command));
    const auto shScript = maiShellStartupCommand(sh, command, true, workdir);
    CHECK(shScript.find(workdir) != std::string::npos &&
          shScript.find(".bashrc") == std::string::npos);
    const auto nonLoginBash = maiShellStartupCommand(bash, command, false, workdir);
    CHECK(nonLoginBash == command);
    CHECK(nonLoginBash.find(".bashrc") == std::string::npos);
    CHECK(bashScript.find("pipefail") == std::string::npos);
    CHECK(zshScript.find("pipefail") == std::string::npos);
    CHECK(maiShellStartupCommand({MaiShellType::PowerShell, "pwsh", "/owned/pwsh"}, command, false,
                                 workdir) == command);
    CHECK(maiShellStartupCommand({MaiShellType::Cmd, "cmd", "/owned/cmd"}, command, true,
                                 workdir) == command);
}

int runOwnedBashStartupChild() {
#if defined(_WIN32)
    std::unique_ptr<ScopedOwnedEnvironment> gitPath;
    for (const char* directory : {"C:/Program Files/Git/bin", "C:/Program Files/Git/usr/bin"}) {
        if (MaiFileSystem::exists(
                MaiFilePath::fromUtf8(directory).append(MaiFilePath::fromUtf8("bash.exe")))) {
            gitPath = std::make_unique<ScopedOwnedEnvironment>("PATH", directory, true);
            break;
        }
    }
#endif
    const auto bash = maiResolveShellEnvironment("bash");
    if (!bash) return 17;
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8("."));
    const auto target = root.append(MaiFilePath::fromUtf8("startup-target a'b"));
    CHECK(!MaiFileSystem::createDirectories(target));
    const std::string rc =
        "shopt -s expand_aliases\n"
        "alias mai_owned_alias='printf owned-alias-ok'\n"
        "export MAI_STARTUP_OWNED=owned-export-ok\n"
        "export PATH=\"$HOME/owned-path:$PATH\"\n"
        "cd /\n";
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8(".bashrc")), rc));
    // 只执行自建 rc；刻意不使用 -lc，避免任何系统/用户 profile。纯 argv 测试另核 -lc。
    ScopedOwnedEnvironment home("HOME", root.toGenericUtf8());
    ScopedOwnedEnvironment bashEnvironment("BASH_ENV", "");
    const std::string command = R"MAI(mai_owned_alias
printf '|%s' "$MAI_STARTUP_OWNED"
[ "${PWD##*/}" = "startup-target a'b" ] || exit 19
printf '|cwd-ok'
case "$PATH" in "$HOME/owned-path:"*) printf '|path-ok';; *) exit 20;; esac)MAI";
    MaiProcessOptions options;
    options.argv = {bash.value().path, "--noprofile", "--norc", "-c",
                    maiShellStartupCommand(bash.value(), command, true, target.toGenericUtf8())};
    options.workingDirectory = root.toUtf8();
    options.timeoutMs = 5000;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    CHECK(result.output.find("owned-alias-ok|owned-export-ok|cwd-ok|path-ok") != std::string::npos);
    std::fwrite(result.output.data(), 1, result.output.size(), stdout);
    return failures == 0 ? 0 : 18;
}

void test_controlled_bash_rc_alias_path_export_and_workdir_restore() {
    if (!maiIsProcessExecutionSupported()) return;
    bool available = static_cast<bool>(maiResolveShellEnvironment("bash"));
#if defined(_WIN32)
    available =
        available ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8("C:/Program Files/Git/bin/bash.exe")) ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8("C:/Program Files/Git/usr/bin/bash.exe"));
#endif
    if (!available) {
        std::puts("SKIP controlled Bash startup: interpreter is unavailable");
        return;
    }
    Workspace workspace;
    // HOME/BASH_ENV 的设置只发生在此自有辅助子进程；主测试进程不修改 HOME。
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-bash-startup"};
    options.workingDirectory = workspace.root.toUtf8();
    options.timeoutMs = 5000;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    CHECK(result.output.find("owned-alias-ok|owned-export-ok|cwd-ok|path-ok") != std::string::npos);
}

void test_sh_env_startup_script_loads_only_non_evaluated_paths() {
    const MaiShellEnvironment sh{MaiShellType::Sh, "sh", "/owned/sh"};
    const auto script =
        maiShellStartupCommand(sh, "printf owned-sh-command", true, "/owned sh-dir");
    CHECK(script.find("ENV") != std::string::npos);
    CHECK(script.find("unset") != std::string::npos);
    CHECK(script.find("/owned sh-dir") != std::string::npos);
    CHECK(script.find("cd ") < script.rfind("printf owned-sh-command"));
    const auto nonLogin =
        maiShellStartupCommand(sh, "printf owned-sh-command", false, "/owned sh-dir");
    CHECK(nonLogin == "printf owned-sh-command");
    CHECK(nonLogin.find("ENV") == std::string::npos);
    CHECK(script.find("pipefail") == std::string::npos);
}

int runOwnedShStartupChild() {
#if defined(_WIN32)
    std::unique_ptr<ScopedOwnedEnvironment> gitPath;
    for (const char* directory : {"C:/Program Files/Git/usr/bin", "C:/Program Files/Git/bin"}) {
        if (MaiFileSystem::exists(
                MaiFilePath::fromUtf8(directory).append(MaiFilePath::fromUtf8("sh.exe")))) {
            gitPath = std::make_unique<ScopedOwnedEnvironment>("PATH", directory, true);
            break;
        }
    }
#endif
    const auto sh = maiResolveShellEnvironment("sh");
    if (!sh) return 22;
    const auto root = MaiFileSystem::resolve(MaiFilePath::fromUtf8("."));
    const auto target = root.append(MaiFilePath::fromUtf8("sh-target a'b"));
    CHECK(!MaiFileSystem::createDirectories(target));
    const std::string rc =
        "alias mai_owned_sh_alias='printf owned-sh-alias'\n"
        "mai_owned_sh_function() { printf '|owned-sh-function'; }\n"
        "export MAI_SH_OWNED=owned-sh-export\n"
        "export PATH=\"$HOME/owned-path:$PATH\"\n"
        "cd /\n";
    CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8("startup.env")), rc));
    ScopedOwnedEnvironment home("HOME", root.toGenericUtf8());
    ScopedOwnedEnvironment ownedRoot("MAI_OWNED_ENV_ROOT", root.toGenericUtf8());
    const std::string command = R"MAI(mai_owned_sh_alias
mai_owned_sh_function
printf '|%s' "$MAI_SH_OWNED"
[ "${PWD##*/}" = "sh-target a'b" ] || exit 29
printf '|cwd-ok'
case "$PATH" in "$HOME/owned-path:"*) printf '|path-ok';; *) exit 30;; esac)MAI";
    for (const auto& env :
         {root.append(MaiFilePath::fromUtf8("startup.env")).toGenericUtf8(),
          std::string("${MAI_OWNED_ENV_ROOT}/startup.env"), std::string("~/startup.env")}) {
        ScopedOwnedEnvironment ownedEnv("ENV", env);
        MaiProcessOptions options;
        options.argv = {sh.value().path, "-c",
                        maiShellStartupCommand(sh.value(), command, true, target.toGenericUtf8())};
        options.workingDirectory = root.toUtf8();
        options.timeoutMs = 5000;
        MaiProcessResult result;
        CHECK(!maiRunProcess(options, result));
        CHECK(result.started && !result.timedOut && result.exitCode == 0);
        CHECK(
            result.output.find("owned-sh-alias|owned-sh-function|owned-sh-export|cwd-ok|path-ok") !=
            std::string::npos);
    }
    const auto marker = root.append(MaiFilePath::fromUtf8("env-must-not-evaluate"));
    ScopedOwnedEnvironment malicious("ENV", "$(printf owned > '" + marker.toGenericUtf8() + "')");
    MaiProcessOptions options;
    options.argv = {
        sh.value().path, "-c",
        maiShellStartupCommand(sh.value(), "printf owned-no-eval", true, target.toGenericUtf8())};
    options.workingDirectory = root.toUtf8();
    options.timeoutMs = 5000;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    CHECK(result.output.find("owned-no-eval") != std::string::npos);
    CHECK(!MaiFileSystem::exists(marker));
    return failures == 0 ? 0 : 23;
}

void test_controlled_sh_env_alias_function_path_and_cwd_restore() {
    if (!maiIsProcessExecutionSupported()) return;
    bool available = static_cast<bool>(maiResolveShellEnvironment("sh"));
#if defined(_WIN32)
    available =
        available ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8("C:/Program Files/Git/usr/bin/sh.exe")) ||
        MaiFileSystem::exists(MaiFilePath::fromUtf8("C:/Program Files/Git/bin/sh.exe"));
#endif
    if (!available) {
        std::puts("SKIP controlled Sh startup: interpreter unavailable");
        return;
    }
    Workspace workspace;
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-sh-startup"};
    options.workingDirectory = workspace.root.toUtf8();
    options.timeoutMs = 10000;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    if (result.exitCode != 0) std::printf("owned Sh fixture output: %s\n", result.output.c_str());
}

void test_shell_environment_typed_resolution_and_login_arguments() {
    const std::string command = "echo owned-shell-state";
    for (const auto type : {MaiShellType::Zsh, MaiShellType::Bash, MaiShellType::Sh}) {
        const MaiShellEnvironment fake{type, "fixture", "/owned/fixture-interpreter"};
        CHECK(maiShellCommandArguments(fake, command, true) ==
              std::vector<std::string>({fake.path, "-lc", command}));
        CHECK(maiShellCommandArguments(fake, command, false) ==
              std::vector<std::string>({fake.path, "-c", command}));
        CHECK(maiShellCommandArguments(fake, command) ==
              maiShellCommandArguments(fake, command, true));
    }
    const MaiShellEnvironment fakePowerShell{MaiShellType::PowerShell, "pwsh", "/owned/pwsh"};
    CHECK(maiShellCommandArguments(fakePowerShell, command, true) ==
          std::vector<std::string>({fakePowerShell.path, "-Command", command}));
    CHECK(maiShellCommandArguments(fakePowerShell, command, false) ==
          std::vector<std::string>({fakePowerShell.path, "-NoProfile", "-Command", command}));
    const MaiShellEnvironment fakeCmd{MaiShellType::Cmd, "cmd", "/owned/cmd"};
    CHECK(maiShellCommandArguments(fakeCmd, command, false) ==
          std::vector<std::string>({fakeCmd.path, "/d", "/s", "/c", command}));
    CHECK(!maiResolveShellEnvironment("not-a-supported-shell"));
    const auto actualDefault = maiResolveShellEnvironment();
    CHECK(actualDefault && !actualDefault.value().path.empty());
    if (actualDefault)
        CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(actualDefault.value().path)));
#if defined(_WIN32)
    CHECK(actualDefault && actualDefault.value().type == MaiShellType::PowerShell);
    // 路径选择的是解释器类型，不能把模型传入的同名假路径当作程序直接执行。
    const auto explicitCmd = maiResolveShellEnvironment("C:/owned-nonexistent/cmd.exe");
    CHECK(explicitCmd && explicitCmd.value().type == MaiShellType::Cmd);
    if (explicitCmd) CHECK(MaiFileSystem::exists(MaiFilePath::fromUtf8(explicitCmd.value().path)));
#endif
}

void test_windows_shell_discovery_honors_owned_path_fixture_without_execution() {
#if defined(_WIN32)
    Workspace workspace;
    const auto interpreter = workspace.root.append(MaiFilePath(L"pwsh.exe"));
    CHECK(!MaiFileSystem::createEmptyFile(interpreter));
    const DWORD count = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::vector<wchar_t> original(count ? count : 1, L'\0');
    if (count) CHECK(::GetEnvironmentVariableW(L"PATH", original.data(), count) < count);
    const std::wstring fixturePath = workspace.root.value() + L";" + std::wstring(original.data());
    CHECK(::SetEnvironmentVariableW(L"PATH", fixturePath.c_str()));
    // 只解析，不启动这个故意不可执行的空素材，也不输出/保存继承的 PATH。
    const auto discovered = maiResolveShellEnvironment();
    const bool restored =
        ::SetEnvironmentVariableW(L"PATH", count ? original.data() : nullptr) != FALSE;
    CHECK(restored);
    CHECK(discovered && discovered.value().type == MaiShellType::PowerShell);
    if (discovered)
        CHECK(MaiFileSystem::resolve(MaiFilePath::fromUtf8(discovered.value().path)) ==
              MaiFileSystem::resolve(interpreter));
#endif
}

void test_powershell_expression_punctuation_requires_approval() {
    const auto tool = makeMaiShellTool();
    for (const char* command :
         {"echo (Write-Output owned-marker)", "echo { Write-Output owned-marker }", "echo @(1,2)",
          "echo safe\rWrite-Output owned-marker"}) {
        CHECK(tool->requiresApproval(args({{"command", command}})));
    }
}

void test_codex_shell_parity_schema_default_and_head_tail_preview() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    const auto schema = json::parse(shell->parametersSchema());
    CHECK(schema["properties"].contains("shell"));
    CHECK(schema["properties"].contains("login"));
    StoredCall previewCapture(workspace);
    const auto preview = shell->execute(
        args({{"command", childCommand("--capture-child-head-tail")}, {"max_output_tokens", 128}}),
        previewCapture.context);
    CHECK(!preview.hasError());
    CHECK(preview.output().find("HEAD-parity-marker") != std::string::npos);
    CHECK(preview.output().find("TAIL-parity-error") != std::string::npos);
    CHECK(preview.output().find("omitted") != std::string::npos);
    bool validPreviewJson = true;
    try {
        (void)json(preview.output()).dump();
    } catch (...) {
        validPreviewJson = false;
    }
    CHECK(validPreviewJson);
    std::string complete = "HEAD-parity-marker \xf0\x9f\x99\x82\n";
    for (unsigned index = 0; index < 10000; ++index) complete += "\xf0\x9f\x99\x82";
    complete += "\nTAIL-parity-error \xe4\xb8\xad\n";
    CHECK(readAll(previewCapture, preview) == complete);

    for (const unsigned tokens : {1u, 2u, 3u}) {
        const auto tiny =
            shell->execute(args({{"command", childCommand("--capture-child-head-tail")},
                                 {"max_output_tokens", tokens}}),
                           previewCapture.context);
        CHECK(!tiny.hasError());
        const std::string marker = "[shell_result]\n";
        const auto position = tiny.output().rfind(marker);
        const auto meta = json::parse(position == std::string::npos
                                          ? tiny.output()
                                          : tiny.output().substr(position + marker.size()),
                                      nullptr, false);
        CHECK(meta.is_object());
        if (meta.is_object()) {
            CHECK(meta.value("output_token_estimate", 99999u) <= tokens);
            CHECK(meta.value("preview_omitted_bytes", std::size_t{0}) > 0);
            CHECK(!meta.value("invalid_source_bytes_escaped", true));
        }
        bool serializable = true;
        try {
            (void)json(tiny.output()).dump();
        } catch (...) {
            serializable = false;
        }
        CHECK(serializable);
        CHECK(readAll(previewCapture, tiny) == complete);
    }

    const auto finished =
        shell->execute(args({{"command", "echo completed-parity"}}), workspace.context());
    CHECK(!finished.hasError());
    const std::string footer = "[shell_result]\n";
    const auto at = finished.output().rfind(footer);
    const auto metadata = json::parse(
        at == std::string::npos ? finished.output() : finished.output().substr(at + footer.size()),
        nullptr, false);
    CHECK(metadata.is_object());
    if (metadata.is_object()) {
        const auto id = metadata.value("session_id", "");
        const auto began = std::chrono::steady_clock::now();
        const auto poll =
            input->execute(args({{"session_id", id}, {"chars", ""}, {"yield_time_ms", 300000}}),
                           workspace.context());
        CHECK(!poll.hasError());
        CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    }
#if defined(_WIN32)
    // 不运行用户 profile；只验证默认解析成 PowerShell 且可访问解释器自身状态。
    const json defaultRequest = {
        {"command", "Write-Output ('MAI-PS-' + $PSVersionTable.PSEdition + '-' + [bool]$PSHOME)"},
        {"login", false}};
    const auto defaultShell = shell->execute(defaultRequest.dump(), workspace.context());
    CHECK(!defaultShell.hasError());
    CHECK(defaultShell.output().find("MAI-PS-") != std::string::npos);
    CHECK(defaultShell.output().find("True") != std::string::npos);
    const json quotedRequest = {
        {"command", R"MAI($value='owned a"b\tail'; [Console]::WriteLine($value))MAI"},
        {"login", false}};
    const auto quoted = shell->execute(quotedRequest.dump(), workspace.context());
    CHECK(!quoted.hasError());
    CHECK(quoted.output().find(R"MAI(owned a"b\tail)MAI") != std::string::npos);
#endif
}

json sessionSnapshot(const MaiToolResult& result) {
    CHECK(!result.hasError());
    if (result.hasError()) {
        std::printf("shell failure: %s\n", result.error().message().c_str());
        return json::object();
    }
    const std::string footer = "[shell_result]\n";
    const auto position = result.output().rfind(footer);
    const auto payload = position == std::string::npos
                             ? result.output()
                             : result.output().substr(position + footer.size());
    const auto snapshot = json::parse(payload, nullptr, false);
    CHECK(snapshot.is_object());
    return snapshot.is_object() ? snapshot : json::object();
}

int runOwnedPipelineChild() {
#if defined(_WIN32)
    // 只改变自有辅助进程的查找路径，不修改主机 PATH/HOME 或加载用户 profile。
    ScopedOwnedEnvironment gitPath(
        "PATH",
        "C:/Program Files/Git/usr/bin;C:/Program Files/Git/mingw64/bin;C:/Program Files/Git/bin",
        true);
#endif
    ScopedOwnedEnvironment bashEnvironment("BASH_ENV", "");
    Workspace workspace;
    StoredCall capture(workspace);
    auto shell = makeMaiShellTool();
    const auto invoke = [&](const std::string& command, const std::string& interpreter) {
        return shell->execute(json{{"command", command},
                                   {"shell", interpreter},
                                   {"login", false},
                                   {"yield_time_ms", 30000}}
                                  .dump(),
                              capture.context);
    };
    const std::string failure = "(printf 'owned-producer-error\\n' >&2; exit 7) | wc -l";
    const auto failed = invoke(failure, "bash");
    CHECK(!failed.hasError());
    // 与 Codex 一致，默认沿用解释器自身的管道状态；不把 stderr 文本推断成退出码。
    CHECK(failed.execution() && failed.execution()->exitCode == 0);
    CHECK(readAll(capture, failed).find("owned-producer-error") != std::string::npos);
    const auto failedMetadata = sessionSnapshot(failed);
    CHECK(failedMetadata.value("exit_status_scope", "") == "shell_process");
    CHECK(failedMetadata.value("pipeline_stage_status", "") == "not_observed");
    CHECK(!failedMetadata.contains("initial_pipeline_policy"));
    CHECK(failedMetadata.value("output_streams", "") == "combined_stdout_stderr");

    const auto handled =
        invoke("set -o pipefail; " + failure + " || printf 'owned-handled\\n'", "bash");
    CHECK(!handled.hasError());
    CHECK(handled.execution() && handled.execution()->exitCode == 0);
    CHECK(handled.output().find("owned-handled") != std::string::npos);
    const auto explicitPipefail = invoke("set -o pipefail; " + failure, "bash");
    CHECK(!explicitPipefail.hasError());
    CHECK(explicitPipefail.execution() && explicitPipefail.execution()->exitCode == 7);
    CHECK(readAll(capture, explicitPipefail).find("owned-producer-error") != std::string::npos);
    const auto intentional =
        invoke("printf 'fatal: owned-but-handled\\n' >&2; false || true", "bash");
    CHECK(!intentional.hasError());
    CHECK(intentional.execution() && intentional.execution()->exitCode == 0);

    // 读取当前仓库的错误查询：保留真实诊断，但不能把 Bash 原生的最终状态 0 改写为 128。
    const auto repository =
        MaiFileSystem::resolve(MaiFilePath::fromUtf8(__FILE__)).dirName().dirName().dirName();
    std::string quoted = "'";
    for (char ch : repository.toGenericUtf8()) quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    quoted += "'";
    const auto git = invoke("git -C " + quoted + " log --since=30 days ago | wc -l", "bash");
    CHECK(!git.hasError());
    CHECK(git.execution() && git.execution()->exitCode == 0);
    std::printf("pipeline-owned git_bash_exit=%d controlled_exit=%d handled_exit=%d\n",
                git.execution() ? git.execution()->exitCode.value_or(-1) : -1,
                failed.execution() ? failed.execution()->exitCode.value_or(-1) : -1,
                handled.execution() ? handled.execution()->exitCode.value_or(-1) : -1);

    const auto sh = invoke(failure, "sh");
    CHECK(!sh.hasError());
    CHECK(sh.execution() && sh.execution()->exitCode == 0);
    CHECK(!sessionSnapshot(sh).contains("initial_pipeline_policy"));
#if defined(_WIN32)
    const auto powershell =
        invoke("git -C " + quoted + " log --since=30 days ago | wc -l", "powershell");
    CHECK(!powershell.hasError());
    CHECK(powershell.execution() && powershell.execution()->exitCode == 1);
    CHECK(!sessionSnapshot(powershell).contains("initial_pipeline_policy"));
    const auto cmd = invoke("(echo owned-cmd-error 1>&2 & exit /b 7) | more", "cmd");
    CHECK(!cmd.hasError());
    CHECK(cmd.execution() && cmd.execution()->exitCode == 0);
    const auto cmdMetadata = sessionSnapshot(cmd);
    CHECK(cmdMetadata.value("pipeline_stage_status", "") == "not_observed");
    CHECK(!cmdMetadata.contains("initial_pipeline_policy"));
#endif
    return failures == 0 ? 0 : 31;
}

void test_pipeline_failure_policy_and_native_status_scope() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-pipeline"};
    options.workingDirectory = workspace.root.toUtf8();
    options.timeoutMs = 15000;
    options.maxOutputBytes = 16 * 1024;
    MaiProcessResult result;
    CHECK(!maiRunProcess(options, result));
    CHECK(result.started && !result.timedOut && result.exitCode == 0);
    if (result.exitCode != 0)
        std::printf("Owned pipeline fixture: %s\n", result.output.c_str());
    else {
        const auto status = result.output.find("pipeline-owned ");
        if (status != std::string::npos) {
            const auto end = result.output.find('\n', status);
            std::printf("%s\n", result.output.substr(status, end - status).c_str());
        }
    }
}

std::uint64_t outputProcessId(const std::string& output, const char* prefix) {
    const auto position = output.find(prefix);
    return position == std::string::npos
               ? 0
               : std::strtoull(output.c_str() + position + std::char_traits<char>::length(prefix),
                               nullptr, 10);
}

struct InteractiveChild {
    std::string id;
    std::string output;
    std::uint64_t processId = 0;
    std::uint64_t descendantId = 0;
};

InteractiveChild startInteractive(MaiTool& shell, MaiTool& input, const MaiToolContext& context,
                                  bool descendant = false) {
    const auto began = std::chrono::steady_clock::now();
    const auto result = shell.execute(
        args({{"command",
               childCommand(descendant ? "--capture-child-descendant" : "--capture-child-session")},
              {"yield_time_ms", 50},
              {"timeout_ms", 30000}}),
        context);
    auto snapshot = sessionSnapshot(result);
    CHECK(snapshot.value("state", "") == "running");
    InteractiveChild child;
    child.id = snapshot.value("session_id", "");
    CHECK(!child.id.empty());
    CHECK(result.execution() && result.execution()->status == MaiToolExecutionStatus::Running);
    if (result.execution()) CHECK(result.execution()->taskId == child.id);
    child.output = snapshot.value("output", "");
    for (unsigned attempt = 0;
         !child.id.empty() && child.output.find("native-ready") == std::string::npos &&
         attempt < 40;
         ++attempt) {
        snapshot = sessionSnapshot(input.execute(
            args({{"session_id", child.id}, {"chars", ""}, {"yield_time_ms", 50}}), context));
        child.output += snapshot.value("output", "");
    }
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));
    CHECK(child.output.find("native-ready") != std::string::npos);
    child.processId = outputProcessId(child.output, "native-pid=");
    child.descendantId = outputProcessId(child.output, "descendant-pid=");
    CHECK(child.processId != 0 && processIsAlive(child.processId));
    return child;
}

void stopInteractive(MaiTool& input, const MaiToolContext& context, const InteractiveChild& child) {
    if (child.id.empty()) return;
    auto stopped = input.execute(
        args({{"session_id", child.id}, {"terminate", true}, {"yield_time_ms", 100}}), context);
    auto snapshot = sessionSnapshot(stopped);
    for (unsigned attempt = 0; snapshot.value("state", "") == "running" && attempt < 3; ++attempt) {
        stopped = input.execute(args({{"session_id", child.id}, {"yield_time_ms", 1000}}), context);
        snapshot = sessionSnapshot(stopped);
    }
    CHECK(snapshot.value("state", "") == "completed");
    CHECK(snapshot.value("terminated", false));
    CHECK(snapshot.contains("exit_code") && snapshot["exit_code"].is_null());
    CHECK(snapshot.contains("exit_status") && snapshot["exit_status"].is_null());
    CHECK(stopped.execution() && !stopped.execution()->exitCode);
    if (stopped.execution()) {
        CHECK(stopped.execution()->status == MaiToolExecutionStatus::Unknown);
        CHECK(stopped.execution()->nextAction == MaiToolNextAction::InspectState);
    }
}

bool waitForProcessExit(std::uint64_t processId) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (processIsAlive(processId) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return !processIsAlive(processId);
}

void test_shell_sessions_input_poll_cross_turn_and_owner_isolation() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    auto context = workspace.context();
    context.turnId = "turn_start";
    const auto child = startInteractive(*shell, *input, context);
    if (child.id.empty()) return;
    CHECK(!registry.finishTurn(context.sessionId, context.turnId));
    context.turnId = "turn_next";
    auto snapshot = sessionSnapshot(input->execute(
        args({{"session_id", child.id}, {"chars", ""}, {"yield_time_ms", 0}}), context));
    CHECK(snapshot.value("state", "") == "running");
    CHECK(processIsAlive(child.processId));
    const auto write =
        args({{"session_id", child.id}, {"chars", "echo-after-turn\n"}, {"yield_time_ms", 500}});
    CHECK(input->requiresApproval(write));
    CHECK(!input->requiresApproval(args({{"session_id", child.id}, {"chars", ""}})));
    CHECK(input->requiresApproval(args({{"session_id", child.id}, {"terminate", true}})));
    snapshot = sessionSnapshot(input->execute(write, context));
    CHECK(snapshot.value("output", "").find("native-input:echo-after-turn") != std::string::npos);
    auto other = context;
    other.sessionId = "ses_other_owner";
    CHECK(
        input->execute(args({{"session_id", child.id}, {"chars", "stolen\n"}}), other).hasError());
    CHECK(input->execute(args({{"session_id", child.id}, {"terminate", true}}), other).hasError());
    CHECK(input->approvalKeys(write, context) != input->approvalKeys(write, other));
    CHECK(input->approvalKeys(write, context) !=
          input->approvalKeys(args({{"session_id", child.id}, {"chars", "other\n"}}), context));
    snapshot = sessionSnapshot(input->execute(
        args({{"session_id", child.id}, {"chars", "exit\n"}, {"yield_time_ms", 1000}}), context));
    CHECK(snapshot.value("state", "") == "completed");
    CHECK(snapshot.contains("exit_code") && snapshot["exit_code"] == 0);
    CHECK(waitForProcessExit(child.processId));
}

void test_default_powershell_native_child_remains_interactive() {
#if defined(_WIN32)
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    std::string quoted = "'";
    for (char character : MaiFilePath::fromUtf8(executablePath).toGenericUtf8())
        quoted += character == '\'' ? "''" : std::string(1, character);
    quoted += "'";
    const json request = {{"command", "& " + quoted + " --capture-child-session"},
                          {"login", false},
                          {"yield_time_ms", 50},
                          {"timeout_ms", 10000}};
    const auto context = workspace.context();
    auto result = shell->execute(request.dump(), context);
    auto snapshot = sessionSnapshot(result);
    const auto id = snapshot.value("session_id", "");
    CHECK(snapshot.value("state", "") == "running");
    if (id.empty()) return;
    std::string ready = snapshot.value("output", "");
    for (unsigned attempt = 0; ready.find("native-ready") == std::string::npos && attempt < 40;
         ++attempt) {
        snapshot = sessionSnapshot(
            input->execute(args({{"session_id", id}, {"yield_time_ms", 50}}), context));
        ready += snapshot.value("output", "");
    }
    CHECK(ready.find("native-ready") != std::string::npos);
    InteractiveChild child;
    child.id = id;
    child.processId = outputProcessId(ready, "native-pid=");
    CHECK(child.processId && processIsAlive(child.processId));
    snapshot = sessionSnapshot(input->execute(
        args({{"session_id", id}, {"chars", "write-marker\n"}, {"yield_time_ms", 500}}), context));
    std::string response = snapshot.value("output", "");
    for (unsigned attempt = 0;
         response.find("native-input:write-marker") == std::string::npos && attempt < 20;
         ++attempt) {
        snapshot = sessionSnapshot(
            input->execute(args({{"session_id", id}, {"yield_time_ms", 50}}), context));
        response += snapshot.value("output", "");
    }
    CHECK(response.find("native-input:write-marker") != std::string::npos);
    CHECK(
        MaiFileSystem::exists(workspace.root.append(MaiFilePath::fromUtf8("stdin-write-marker"))));
    stopInteractive(*input, context, child);
    CHECK(waitForProcessExit(child.processId));
#endif
}

void test_first_completed_big_capture_preview_shows_actual_tail_without_changing_store() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    StoredCall capture(workspace);
    auto shell = makeMaiShellTool();
    const auto result =
        shell->execute(args({{"command", childCommand("--capture-child-big-head-tail")},
                             {"yield_time_ms", 30000},
                             {"timeout_ms", 30000},
                             {"max_output_tokens", 128}}),
                       capture.context);
    CHECK(!result.hasError());
    CHECK(result.execution() && result.execution()->status == MaiToolExecutionStatus::Completed);
    CHECK(result.output().find("BIG-HEAD-owned") != std::string::npos);
    CHECK(result.output().find("BIG-TAIL-owned-error") != std::string::npos);
    const auto snapshot = sessionSnapshot(result);
    CHECK(snapshot.value("state", "") == "completed");
    CHECK(result.execution() && !result.execution()->outputId.empty());
    if (!result.execution() || result.execution()->outputId.empty()) return;
    MaiToolOutputQuery query;
    query.outputId = result.execution()->outputId;
    const auto first = capture.store->readToolOutput(capture.context.sessionId, query);
    CHECK(first && first.value().truncated && first.value().totalBytes == kMaiToolOutputMaxBytes);
    if (first) CHECK(first.value().text.find("BIG-HEAD-owned") != std::string::npos);
    query.offsetBytes = kMaiToolOutputMaxBytes - 128;
    query.maxBytes = 128;
    const auto last = capture.store->readToolOutput(capture.context.sessionId, query);
    CHECK(last && last.value().text == std::string(128, 'B'));
}

void test_shell_session_workdir_budget_validation_and_token_preview() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    Workspace outside;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    auto context = workspace.context();
    const auto command = childCommand("--capture-child-workdir");
    CHECK(shell->execute(args({{"command", command}, {"workdir", outside.root.toUtf8()}}), context)
              .hasError());
    CHECK(!MaiFileSystem::exists(outside.root.append(MaiFilePath::fromUtf8("workdir-marker"))));
    CHECK(!MaiFileSystem::createDirectories(workspace.root.append(MaiFilePath::fromUtf8("other"))));
    CHECK(shell->approvalKeys(args({{"command", command}, {"workdir", "."}}), context) !=
          shell->approvalKeys(args({{"command", command}, {"workdir", "other"}}), context));
    for (const json& invalid :
         {json{{"yield_time_ms", -1}}, json{{"yield_time_ms", 30001}},
          json{{"max_output_tokens", 0}}, json{{"workdir", 42}}, json{{"yield_time_ms", "1"}}}) {
        auto request = invalid;
        request["command"] = command;
        CHECK(shell->execute(request.dump(), context).hasError());
    }
    const auto preview = shell->execute(
        args({{"command", childCommand("--capture-child-cap")}, {"max_output_tokens", 64}}),
        context);
    const auto snapshot = sessionSnapshot(preview);
    CHECK(snapshot.value("output", "").size() <= 256);
    CHECK(snapshot.value("output_truncated", false));
    CHECK(snapshot.value("output_token_estimate", std::size_t{99999}) > 0 &&
          snapshot.value("output_token_estimate", std::size_t{99999}) <= 64);
    CHECK(preview.output().find("retained-final-marker") != std::string::npos);
    CHECK(preview.output().find("omitted") != std::string::npos);
    CHECK(snapshot.value("output_token_estimate_method", "") ==
          "utf8_bytes_divided_by_4_approximation");
    const auto capped = shell->execute(
        args({{"command", childCommand("--capture-child-cap")}, {"max_output_tokens", 1000000}}),
        context);
    const auto cappedSnapshot = sessionSnapshot(capped);
    CHECK(cappedSnapshot.value("output_token_estimate", std::size_t{0}) > 0 &&
          cappedSnapshot.value("output_token_estimate", std::size_t{0}) <= 12288);
    CHECK(capped.output().find("retained-final-marker") != std::string::npos);
    CHECK(capped.output().find("omitted") != std::string::npos);
    const auto child = startInteractive(*shell, *input, context);
    if (child.id.empty()) return;
    for (const json& invalid : {json{{"chars", 42}}, json{{"yield_time_ms", -1}},
                                json{{"yield_time_ms", 30001}, {"chars", "write-marker\n"}},
                                json{{"terminate", true}, {"chars", "bad"}}}) {
        auto request = invalid;
        request["session_id"] = child.id;
        CHECK(input->execute(request.dump(), context).hasError());
    }
    stopInteractive(*input, context, child);
    CHECK(waitForProcessExit(child.processId));
}

void test_shell_partial_utf8_fragments_are_visible_and_recoverable() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    StoredCall completed(workspace);
    const auto result = shell->execute(
        args({{"command", childCommand("--capture-child-utf8-incomplete")}}), completed.context);
    CHECK(!result.hasError());
    CHECK(result.output().find("(no output)") == std::string::npos);
    CHECK(readAll(completed, result) == std::string("\xe4\xb8", 2));
    const auto child = startInteractive(*shell, *input, workspace.context());
    if (child.id.empty()) return;
    StoredCall running(workspace, "shell_input");
    const auto fragment = input->execute(
        args({{"session_id", child.id}, {"chars", "utf8-fragment\n"}, {"yield_time_ms", 500}}),
        running.context);
    const auto snapshot = sessionSnapshot(fragment);
    CHECK(snapshot.value("state", "") == "running");
    CHECK(snapshot.value("output", "").find("\\xF0") != std::string::npos);
    CHECK(readAll(running, fragment) == std::string("\xf0", 1));
    stopInteractive(*input, running.context, child);
    CHECK(waitForProcessExit(child.processId));
}

void test_shell_input_retains_original_hard_timeout() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    const auto context = workspace.context();
    auto result = shell->execute(args({{"command", childCommand("--capture-child-sleep")},
                                       {"timeout_ms", 1200},
                                       {"yield_time_ms", 0}}),
                                 context);
    auto snapshot = sessionSnapshot(result);
    const auto id = snapshot.value("session_id", "");
    CHECK(snapshot.value("state", "") == "running");
    if (id.empty()) return;
    for (unsigned attempt = 0; snapshot.value("state", "") == "running" && attempt < 15;
         ++attempt) {
        result = input->execute(args({{"session_id", id}, {"yield_time_ms", 250}}), context);
        snapshot = sessionSnapshot(result);
    }
    CHECK(snapshot.value("state", "") == "completed");
    CHECK(snapshot.value("timed_out", false));
    CHECK(result.output().find("timed out after 1200 ms") != std::string::npos);
    CHECK(result.execution() && result.execution()->timedOut && !result.execution()->exitCode);
}

void test_shell_session_ring_discard_count_and_per_owner_limit() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiToolRegistry registry;
    registerMaiBuiltinTools(registry);
    auto* shell = registry.find("shell");
    auto* input = registry.find("shell_input");
    CHECK(shell && input);
    if (!shell || !input) return;
    const auto context = workspace.context();
    const auto child = startInteractive(*shell, *input, context);
    if (child.id.empty()) return;
    StoredCall capture(workspace, "shell_input");
    const auto floodStarted = std::chrono::steady_clock::now();
    (void)sessionSnapshot(input->execute(args({{"session_id", child.id},
                                               {"chars", "flood\n"},
                                               {"yield_time_ms", 0},
                                               {"max_output_tokens", 64}}),
                                         capture.context));
    const auto ready = workspace.root.append(MaiFilePath::fromUtf8("flood-complete"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!MaiFileSystem::exists(ready) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(MaiFileSystem::exists(ready));
    const auto floodMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - floodStarted)
                             .count();
    auto prefix = input->execute(
        args({{"session_id", child.id}, {"yield_time_ms", 0}, {"max_output_tokens", 64}}),
        capture.context);
    auto snapshot = sessionSnapshot(prefix);
    std::printf(
        "shell-perf flood_bytes=24576000 elapsed_ms=%lld captured_total=%llu\n",
        static_cast<long long>(floodMs),
        static_cast<unsigned long long>(snapshot.value("total_output_bytes", std::uint64_t{0})));
    CHECK(snapshot.value("ring_discarded_bytes", std::size_t{0}) > 0);
    CHECK(snapshot.value("output", "").size() <= 256);
    CHECK(snapshot.value("has_more_output", false));
    CHECK(prefix.execution() && !prefix.execution()->outputId.empty());
    const auto tail = input->execute(
        args({{"session_id", child.id}, {"yield_time_ms", 0}, {"max_output_tokens", 12288}}),
        capture.context);
    snapshot = sessionSnapshot(tail);
    CHECK(snapshot.value("discarded_bytes", std::size_t{0}) > 0);
    CHECK(snapshot.value("output", "").size() <= 48 * 1024);
    CHECK(!snapshot.value("has_more_output", true));
    auto retained = readAll(capture, tail);
    CHECK(retained.size() <= 1024 * 1024);
    for (unsigned attempt = 0;
         retained.find("native-flood-final-marker") == std::string::npos && attempt < 40;
         ++attempt) {
        const auto last = input->execute(args({{"session_id", child.id}, {"yield_time_ms", 50}}),
                                         capture.context);
        const auto lastSnapshot = sessionSnapshot(last);
        if (!lastSnapshot.value("output", "").empty()) retained += readAll(capture, last);
    }
    CHECK(retained.find("native-flood-final-marker") != std::string::npos);
    std::vector<InteractiveChild> children{child};
    for (unsigned index = 1; index < 12; ++index)
        children.push_back(startInteractive(*shell, *input, context));
    const auto excess = shell->execute(args({{"command", childCommand("--capture-child-session")},
                                             {"yield_time_ms", 0},
                                             {"timeout_ms", 10000}}),
                                       context);
    CHECK(excess.hasError());
    CHECK(excess.error().code() == MaiErrorCode::Busy);
    auto other = context;
    other.sessionId = "ses_limit_other";
    const auto independent = startInteractive(*shell, *input, other);
    stopInteractive(*input, other, independent);
    for (const auto& current : children) stopInteractive(*input, context, current);
    for (const auto& current : children) CHECK(waitForProcessExit(current.processId));
}

void test_ordinary_shell_manager_precancel_writes_nothing_and_terminate_kills_descendants() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiShellSessionManager manager;
    MaiShellSessionOptions options;
    options.shell = legacyShellPath();
    options.login = false;
    options.command = childCommand("--capture-child-session");
    options.workdir = workspace.root.toUtf8();
    options.timeoutMs = 10000;
    const auto started = manager.start("ordinary-owner", options);
    CHECK(started);
    if (!started) return;
    std::string output;
    for (unsigned attempt = 0; output.find("native-ready") == std::string::npos && attempt < 40;
         ++attempt) {
        const auto next = manager.interact("ordinary-owner", started.value(), "", 50);
        CHECK(next);
        if (!next) break;
        output += next.value().output;
    }
    const auto pid = outputProcessId(output, "native-pid=");
    CHECK(pid != 0 && processIsAlive(pid));
    const auto canceled = manager.interact("ordinary-owner", started.value(), "write-marker\n",
                                           1000, false, [] { return true; });
    CHECK(canceled ? canceled.value().canceled : canceled.error().code() == MaiErrorCode::Canceled);
    CHECK(waitForProcessExit(pid));
    CHECK(
        !MaiFileSystem::exists(workspace.root.append(MaiFilePath::fromUtf8("stdin-write-marker"))));
    options.command = childCommand("--capture-child-descendant");
    const auto tree = manager.start("ordinary-owner", options);
    CHECK(tree);
    if (!tree) return;
    output.clear();
    for (unsigned attempt = 0; output.find("native-ready") == std::string::npos && attempt < 40;
         ++attempt) {
        const auto next = manager.interact("ordinary-owner", tree.value(), "", 50);
        CHECK(next);
        if (!next) break;
        output += next.value().output;
    }
    const auto parentPid = outputProcessId(output, "native-pid=");
    const auto childPid = outputProcessId(output, "descendant-pid=");
    CHECK(parentPid != 0 && processIsAlive(parentPid));
    CHECK(childPid != 0 && processIsAlive(childPid));
    const auto stopped = manager.interact("ordinary-owner", tree.value(), "", 1000, true);
    CHECK(stopped && stopped.value().completed && stopped.value().terminated);
    CHECK(waitForProcessExit(parentPid));
    CHECK(waitForProcessExit(childPid));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    CHECK(
        !MaiFileSystem::exists(workspace.root.append(MaiFilePath::fromUtf8("descendant-marker"))));
}

void test_ordinary_shell_manager_prunes_completed_records_without_losing_capacity() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiShellSessionManager manager;
    MaiShellSessionOptions options;
    options.shell = legacyShellPath();
    options.login = false;
    options.command = "echo bounded-prune-marker";
    options.workdir = workspace.root.toUtf8();
    options.timeoutMs = 1000;
    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + std::chrono::seconds(10);
    std::string firstId, lastId;
    unsigned completed = 0;
    for (; completed < 132 && std::chrono::steady_clock::now() < deadline; ++completed) {
        const auto started = manager.start("prune-owner", options);
        CHECK(started);
        if (!started) break;
        if (firstId.empty()) firstId = started.value();
        lastId = started.value();
        const auto result =
            manager.interact("prune-owner", started.value(), "", 1000, false, {}, true);
        CHECK(result && result.value().completed);
        if (!result || !result.value().completed) break;
        CHECK(result.value().exitCode == 0);
        CHECK(result.value().output.find("bounded-prune-marker") != std::string::npos);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    std::printf("shell-perf completed_record_prune count=%u elapsed_ms=%lld\n", completed,
                static_cast<long long>(elapsed));
    CHECK(completed == 132 && std::chrono::steady_clock::now() < deadline);
    if (completed == 132) {
        const auto evicted = manager.interact("prune-owner", firstId, "", 0);
        CHECK(!evicted && evicted.error().code() == MaiErrorCode::NotFound);
        const auto retained = manager.interact("prune-owner", lastId, "", 0);
        CHECK(retained && retained.value().completed && retained.value().output.empty());
    }
}

void test_ordinary_empty_poll_supports_300000_and_remains_cancellable() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    MaiShellSessionManager manager;
    MaiShellSessionOptions options;
    options.shell = legacyShellPath();
    options.login = false;
    options.command = "echo extended-poll-owned";
    options.workdir = workspace.root.toUtf8();
    const auto completed = manager.start("poll-owner", options);
    CHECK(completed);
    if (!completed) return;
    const auto initial =
        manager.interact("poll-owner", completed.value(), "", 1000, false, {}, true);
    CHECK(initial && initial.value().completed);
    const auto began = std::chrono::steady_clock::now();
    const auto reread = manager.interact("poll-owner", completed.value(), "", 300000);
    CHECK(reread && reread.value().completed);
    CHECK(!manager.interact("poll-owner", completed.value(), "", 300001));
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
    options.command = childCommand("--capture-child-session");
    const auto running = manager.start("poll-owner", options);
    CHECK(running);
    if (!running) return;
    std::string ready;
    for (unsigned attempt = 0; ready.find("native-ready") == std::string::npos && attempt < 40;
         ++attempt) {
        const auto page = manager.interact("poll-owner", running.value(), "", 50);
        CHECK(page);
        if (!page) break;
        ready += page.value().output;
    }
    const auto processId = outputProcessId(ready, "native-pid=");
    CHECK(processId && processIsAlive(processId));
    CHECK(!manager.interact("poll-owner", running.value(), "write-marker\n", 30001));
    CHECK(
        !MaiFileSystem::exists(workspace.root.append(MaiFilePath::fromUtf8("stdin-write-marker"))));
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel.store(true);
    });
    const auto waitBegan = std::chrono::steady_clock::now();
    const auto canceled = manager.interact("poll-owner", running.value(), "", 300000, false,
                                           [&] { return cancel.load(); });
    canceller.join();
    CHECK(canceled ? canceled.value().canceled : canceled.error().code() == MaiErrorCode::Canceled);
    CHECK(std::chrono::steady_clock::now() - waitBegan < std::chrono::seconds(2));
    CHECK(waitForProcessExit(processId));
}

void test_agent_session_delete_and_destructor_stop_owned_process_trees() {
    if (!maiIsProcessExecutionSupported()) return;
    Workspace workspace;
    InteractiveChild survivor;
    {
        auto registry = std::make_unique<MaiToolRegistry>();
        registerMaiBuiltinTools(*registry);
        auto* shell = registry->find("shell");
        auto* input = registry->find("shell_input");
        CHECK(shell && input);
        if (!shell || !input) return;
        MaiAgent agent(makeMaiMemoryStore(), nullptr, std::move(registry));
        const auto firstOwner =
            agent.submit(MaiCreateSession{workspace.root.toUtf8(), "First", ""});
        const auto secondOwner =
            agent.submit(MaiCreateSession{workspace.root.toUtf8(), "Second", ""});
        CHECK(firstOwner && secondOwner);
        if (!firstOwner || !secondOwner) return;
        auto context = workspace.context();
        context.sessionId = firstOwner.value();
        const auto removed = startInteractive(*shell, *input, context, true);
        CHECK(removed.descendantId != 0 && processIsAlive(removed.descendantId));
        auto other = context;
        other.sessionId = secondOwner.value();
        survivor = startInteractive(*shell, *input, other, true);
        CHECK(survivor.descendantId != 0 && processIsAlive(survivor.descendantId));
        CHECK(agent.submit(MaiDeleteSession{firstOwner.value()}));
        CHECK(waitForProcessExit(removed.processId));
        CHECK(waitForProcessExit(removed.descendantId));
        CHECK(input->execute(args({{"session_id", removed.id}, {"yield_time_ms", 0}}), context)
                  .hasError());
        const auto snapshot = sessionSnapshot(
            input->execute(args({{"session_id", survivor.id}, {"yield_time_ms", 0}}), other));
        CHECK(snapshot.value("state", "") == "running");
        CHECK(processIsAlive(survivor.processId));
    }
    CHECK(waitForProcessExit(survivor.processId));
    CHECK(waitForProcessExit(survivor.descendantId));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    CHECK(
        !MaiFileSystem::exists(workspace.root.append(MaiFilePath::fromUtf8("descendant-marker"))));
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
    const std::string leaf = workspace.root.baseName().toUtf8();
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
    const std::string command = childCommand("--capture-child-sleep");
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
    const std::string command = childCommand("--capture-child-sleep");
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
    const std::string command = childCommand("--capture-child-cap");
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
    const std::string command = childCommand("--capture-child-once-long");
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
    options.workingDirectory = workspace.root.toUtf8();
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
    options.workingDirectory = workspace.root.toUtf8();
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
    const MaiFilePath readyPath =
        workspace.root.append(MaiFilePath::fromUtf8("capture-child-ready"));
    if (MaiFileSystem::exists(readyPath)) CHECK(!MaiFileSystem::removeFile(readyPath));
    std::thread stopper([&cancel, &readyObserved, &readyPath] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (MaiFileSystem::exists(readyPath)) {
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
    CHECK(utf8.output().find("\xf0\x9f\x98\x80") != std::string::npos);
    CHECK(readAll(call, utf8).find("\xf0\x9f\x98\x80utf8-final-marker") != std::string::npos);
#if defined(_WIN32)
    // Windows 的原始捕获边界也不能将完整 UTF-8 前缀整段误解码为 OEM。
    MaiProcessOptions options;
    options.argv = {executablePath, "--capture-child-utf8"};
    options.workingDirectory = workspace.root.toUtf8();
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
    options.workingDirectory = workspace.root.toUtf8();
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
    const std::string command = childCommand("--capture-child-oem");
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
    executablePath = MaiFileSystem::resolve(MaiFilePath::fromUtf8(argv[0])).toUtf8();
    if (argc == 2 && std::string(argv[1]).find("--capture-child-") == 0) {
        return runCaptureChild(argv[1]);
    }
    if (argc == 2 && std::string(argv[1]) == "--pipeline-status") {
        RUN(test_pipeline_failure_policy_and_native_status_scope);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-capture-tail") {
        RUN(test_first_completed_big_capture_preview_shows_actual_tail_without_changing_store);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-sh-env") {
        RUN(test_sh_env_startup_script_loads_only_non_evaluated_paths);
        RUN(test_controlled_sh_env_alias_function_path_and_cwd_restore);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-ps-cwd") {
        RUN(test_powershell_login_startup_restores_approved_workdir);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-cmd-quote") {
        RUN(test_explicit_cmd_preserves_quoted_executable_and_argument);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-startup") {
        RUN(test_shell_startup_script_refreshes_rc_and_restores_workdir);
        RUN(test_controlled_bash_rc_alias_path_export_and_workdir_restore);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-native") {
        RUN(test_shell_environment_typed_resolution_and_login_arguments);
        RUN(test_windows_shell_discovery_honors_owned_path_fixture_without_execution);
        RUN(test_ordinary_empty_poll_supports_300000_and_remains_cancellable);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--codex-shell-parity") {
        RUN(test_shell_environment_typed_resolution_and_login_arguments);
        RUN(test_windows_shell_discovery_honors_owned_path_fixture_without_execution);
        RUN(test_powershell_expression_punctuation_requires_approval);
        RUN(test_codex_shell_parity_schema_default_and_head_tail_preview);
        RUN(test_default_powershell_native_child_remains_interactive);
        RUN(test_ordinary_empty_poll_supports_300000_and_remains_cancellable);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--batch6-red") {
        RUN(test_batch6_builtin_schema_and_workdir_contract);
        RUN(test_shell_sessions_input_poll_cross_turn_and_owner_isolation);
        RUN(test_shell_session_workdir_budget_validation_and_token_preview);
        RUN(test_shell_partial_utf8_fragments_are_visible_and_recoverable);
        RUN(test_shell_input_retains_original_hard_timeout);
        RUN(test_shell_session_ring_discard_count_and_per_owner_limit);
        RUN(test_ordinary_shell_manager_precancel_writes_nothing_and_terminate_kills_descendants);
        RUN(test_ordinary_shell_manager_prunes_completed_records_without_losing_capacity);
        RUN(test_agent_session_delete_and_destructor_stop_owned_process_trees);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--batch6-fragments") {
        RUN(test_shell_partial_utf8_fragments_are_visible_and_recoverable);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--utf8-small-cap") {
        RUN(test_windows_capture_can_end_inside_the_first_utf8_scalar);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--partial-capture") {
        RUN(test_interruption_retains_partial_output_and_requires_state_inspection);
        return failures == 0 ? 0 : 1;
    }
    RUN(test_pipeline_failure_policy_and_native_status_scope);
    RUN(test_explicit_cmd_preserves_quoted_executable_and_argument);
    RUN(test_powershell_login_startup_restores_approved_workdir);
    RUN(test_shell_startup_script_refreshes_rc_and_restores_workdir);
    RUN(test_controlled_bash_rc_alias_path_export_and_workdir_restore);
    RUN(test_sh_env_startup_script_loads_only_non_evaluated_paths);
    RUN(test_controlled_sh_env_alias_function_path_and_cwd_restore);
    RUN(test_shell_environment_typed_resolution_and_login_arguments);
    RUN(test_windows_shell_discovery_honors_owned_path_fixture_without_execution);
    RUN(test_powershell_expression_punctuation_requires_approval);
    RUN(test_codex_shell_parity_schema_default_and_head_tail_preview);
    RUN(test_default_powershell_native_child_remains_interactive);
    RUN(test_ordinary_empty_poll_supports_300000_and_remains_cancellable);
    RUN(test_batch6_builtin_schema_and_workdir_contract);
    RUN(test_shell_sessions_input_poll_cross_turn_and_owner_isolation);
    RUN(test_first_completed_big_capture_preview_shows_actual_tail_without_changing_store);
    RUN(test_shell_session_workdir_budget_validation_and_token_preview);
    RUN(test_shell_partial_utf8_fragments_are_visible_and_recoverable);
    RUN(test_shell_input_retains_original_hard_timeout);
    RUN(test_shell_session_ring_discard_count_and_per_owner_limit);
    RUN(test_ordinary_shell_manager_precancel_writes_nothing_and_terminate_kills_descendants);
    RUN(test_ordinary_shell_manager_prunes_completed_records_without_losing_capacity);
    RUN(test_agent_session_delete_and_destructor_stop_owned_process_trees);
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
