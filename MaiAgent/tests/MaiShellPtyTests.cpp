// 文件作用：用自有子进程验证共享 shell 会话的显式 PTY、交互和生命周期边界。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiShellSessionManager.h"
#include "MaiExecCommandTool.h"

namespace {
using nlohmann::json;
int failures = 0;
std::string executable;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

bool attached() {
#if defined(_WIN32)
    DWORD input = 0, output = 0;
    return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &input) &&
           ::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &output);
#else
    return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO) && ::isatty(STDERR_FILENO);
#endif
}

std::uint64_t ownProcessId() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

bool processAlive(std::uint64_t id) {
#if defined(_WIN32)
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(id));
    if (!process) return false;
    const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(id), 0) == 0;
#endif
}

std::uint64_t spawnOwnedDescendant() {
#if defined(_WIN32)
    const auto path = MaiFilePath::fromUtf8(executable).value();
    std::wstring command = L"\"" + path + L"\" --pty-idle";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &process))
        return 0;
    const auto id = process.dwProcessId;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return id;
#else
    const auto pid = ::fork();
    if (pid == 0) {
        ::execl(executable.c_str(), executable.c_str(), "--pty-idle", nullptr);
        ::_exit(127);
    }
    return pid > 0 ? static_cast<std::uint64_t>(pid) : 0;
#endif
}

void writeChild(const std::string& text) {
#if defined(_WIN32)
    DWORD written = 0;
    // 控制台原生 Unicode API 绕过机器 OEM 代码页；ConPTY 应将其转换为 UTF-8 字节。
    // 默认管道仍直接写 UTF-8，便于独立核对未改变的字节捕获。
    if (attached()) {
        const auto length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                  static_cast<int>(text.size()), nullptr, 0);
        std::wstring native(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), native.data(), length);
        ::WriteConsoleW(::GetStdHandle(STD_OUTPUT_HANDLE), native.data(),
                        static_cast<DWORD>(native.size()), &written, nullptr);
        return;
    }
    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()),
                &written, nullptr);
#else
    const auto ignored = ::write(STDOUT_FILENO, text.data(), text.size());
    (void)ignored;
#endif
}

int child(const std::string& mode) {
    writeChild(attached() ? "ATTACHED=1\n" : "ATTACHED=0\n");
    writeChild("UTF8=\xE7\x95\x8C\xF0\x9F\x99\x82\n");
    if (mode == "--pty-idle" || mode == "--pty-flood")
        writeChild("PID=" + std::to_string(ownProcessId()) + "\n");
    if (mode == "--pty-flood")
        writeChild("DESCENDANT=" + std::to_string(spawnOwnedDescendant()) + "\n");
    writeChild("READY\n");
    if (mode == "--pty-probe") return 0;
    if (mode == "--pty-eof") {
        char byte = 0;
#if defined(_WIN32)
        DWORD count = 0;
        const bool read =
            ::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), &byte, 1, &count, nullptr) != FALSE;
        const bool eof = (read && count == 0) || (!read && ::GetLastError() == ERROR_BROKEN_PIPE);
#else
        const bool eof = ::read(STDIN_FILENO, &byte, 1) == 0;
#endif
        writeChild(eof ? "EOF=1\n" : "EOF=0\n");
        return eof ? 0 : 3;
    }

    if (mode == "--pty-flood") {
        const std::string row = "FLOOD_" + std::string(1000, 'x') + "\n";
        for (;;) writeChild(row);
    }
    if (mode == "--pty-idle") {
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    char input[128]{};
#if defined(_WIN32)
    DWORD read = 0;
    if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), input, sizeof(input), &read, nullptr))
        return 2;
    const std::string received(input, read);
#else
    const auto read = ::read(STDIN_FILENO, input, sizeof(input));
    if (read <= 0) return 2;
    const std::string received(input, static_cast<std::size_t>(read));
#endif
    writeChild(received.find("hello") != std::string::npos ? "INPUT_OK\n" : "INPUT_BAD\n");
    return 0;
}

struct Workspace {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-pty_")));
    Workspace() {
        CHECK(!MaiFileSystem::createDirectories(root));
    }
    ~Workspace() {
        MaiFileSystem::removeRecursively(root);
    }
    MaiToolContext context() const {
        MaiToolContext context;
        context.sessionId = "pty-owner";
        context.root = root.toUtf8();
        return context;
    }
};

json arguments(const std::string& mode, bool tty) {
    return json{{"cmd", "\"" + executable + "\" " + mode},
                {"shell",
#if defined(_WIN32)
                 "cmd"
#else
                 "sh"
#endif
                },
                {"login", false},
                {"tty", tty},
                {"yield_time_ms", 10000}};
}

json metadata(const MaiToolResult& result) {
    const auto& wire = result.hasError() ? result.error().message() : result.output();
    const auto value = json::parse(wire, nullptr, false);
    CHECK(value.is_object());
    if (value.is_object()) {
        CHECK(value.contains("chunk_id") && value["chunk_id"].is_string());
        CHECK(value.contains("wall_time_seconds") && value["wall_time_seconds"].is_number());
        CHECK(value.contains("output") && value["output"].is_string());
        CHECK(!value.contains("tty"));
    }
    return value.is_object() ? value : json::object();
}

MaiShellSessionOptions options(const Workspace& workspace, const std::string& mode, bool tty) {
    MaiShellSessionOptions value;
    const auto args = arguments(mode, tty);
    value.command = args["cmd"].get<std::string>();
    value.shell = args["shell"].get<std::string>();
    value.workdir = workspace.root.toUtf8();
    value.login = false;
    value.tty = tty;
    value.timeoutMs = 5000;
    return value;
}

std::uint64_t markerId(const std::string& output, const std::string& marker) {
    const auto start = output.find(marker);
    if (start == std::string::npos) return 0;
    const auto begin = start + marker.size();
    const auto end = output.find_first_not_of("0123456789", begin);
    try {
        return std::stoull(output.substr(begin, end - begin));
    } catch (...) {
        return 0;
    }
}

MaiShellSessionSnapshot collect(MaiShellSessionManager& manager, const std::string& owner,
                                const std::string& id, std::string& output,
                                const std::string& until = {}) {
    MaiShellSessionSnapshot snapshot;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto result = manager.interact(owner, id, {}, 100);
        CHECK(result);
        if (!result) break;
        snapshot = result.value();
        output += snapshot.output;
        if (snapshot.completed || (!until.empty() && output.find(until) != std::string::npos))
            break;
    }
    return snapshot;
}

void test_manager_interaction_and_default_pipe_bytes() {
    Workspace workspace;
    {
        MaiShellSessionManager manager;
        const auto id = manager.start("owner", options(workspace, "--pty-eof", false));
        CHECK(id);
        if (!id) return;
        std::string output;
        const auto state = collect(manager, "owner", id.value(), output);
        CHECK(state.started && state.completed && !state.tty && state.exitCode == 0);
        CHECK(output == "ATTACHED=0\nUTF8=\xE7\x95\x8C\xF0\x9F\x99\x82\nREADY\nEOF=1\n");
        const auto polled = manager.interact("owner", id.value(), {}, 0);
        CHECK(polled && polled.value().completed && polled.value().output.empty());
        const auto live = manager.start("owner", options(workspace, "--pty-idle", false));
        CHECK(live);
        if (live) {
            CHECK(manager.interact("owner", live.value(), "hello\n", 100).error().code() ==
                  MaiErrorCode::InvalidInput);
            CHECK(manager.interact("other-owner", live.value(), "BAD\n", 0).error().code() ==
                  MaiErrorCode::NotFound);
        }
    }
    MaiShellSessionManager manager;
    const auto id = manager.start("owner", options(workspace, "--pty-interactive", true));
    CHECK(id);
    if (!id) return;
    std::string output;
    auto state = collect(manager, "owner", id.value(), output, "READY");
    CHECK(state.started && !state.completed && state.tty);
    CHECK(output.find("ATTACHED=1") != std::string::npos);
    CHECK(manager.interact("other-owner", id.value(), "BAD\r", 0).error().code() ==
          MaiErrorCode::NotFound);
    const auto sent = manager.interact("owner", id.value(), "hello\r", 100);
    CHECK(sent);
    if (sent) output += sent.value().output;
    state = collect(manager, "owner", id.value(), output);
    CHECK(state.completed && state.exitCode == 0 && state.tty);
    CHECK(output.find("INPUT_OK") != std::string::npos);
    CHECK(output.find("INPUT_BAD") == std::string::npos);
    const auto polled = manager.interact("owner", id.value(), {}, 0);
    CHECK(polled && polled.value().completed && polled.value().output.empty());
}

void test_timeout_cancel_and_explicit_termination() {
    Workspace workspace;
    for (int mode = 0; mode < 3; ++mode) {
        MaiShellSessionManager manager;
        auto request = options(workspace, "--pty-idle", true);
        if (mode == 0) request.timeoutMs = 1000;
        const auto id = manager.start("owner", request);
        CHECK(id);
        if (!id) continue;
        std::string output;
        const auto initial = collect(manager, "owner", id.value(), output, "READY");
        CHECK(!initial.completed);
        const auto pid = markerId(output, "PID=");
        CHECK(pid && processAlive(pid));
        const auto before = std::chrono::steady_clock::now();
        const auto result = manager.interact(
            "owner", id.value(), {}, 3000, mode == 2, [mode] { return mode == 1; }, true);
        CHECK(result);
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));
        if (!result) continue;
        const auto& state = result.value();
        CHECK(state.completed && state.tty && !state.exitCode);
        CHECK(state.timedOut == (mode == 0));
        CHECK(state.canceled == (mode == 1));
        CHECK(state.terminated == (mode == 2));
        CHECK(!state.error);
        CHECK(!processAlive(pid));
    }
}

void test_large_output_does_not_deadlock_stop_owner() {
    Workspace workspace;
    MaiShellSessionManager manager;
    const auto flood = manager.start("flood-owner", options(workspace, "--pty-flood", true));
    const auto idle = manager.start("other-owner", options(workspace, "--pty-idle", false));
    CHECK(flood && idle);
    if (!flood || !idle) return;
    std::uint64_t bytes = 0;
    std::string output;
    for (int attempt = 0; attempt < 100 && bytes < 128 * 1024; ++attempt) {
        const auto result = manager.interact("flood-owner", flood.value(), {}, 50);
        CHECK(result);
        if (result) {
            bytes = result.value().totalOutputBytes;
            output += result.value().output;
        }
    }
    CHECK(bytes >= 128 * 1024);
    const auto pid = markerId(output, "PID=");
    const auto descendant = markerId(output, "DESCENDANT=");
    CHECK(pid && processAlive(pid));
    CHECK(descendant && processAlive(descendant));
#if defined(_WIN32)
    // 保留被测进程对象，避免停止后仅凭已可复用的 PID 误认另一个进程；不增加等待宽限。
    const HANDLE childHandle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    const HANDLE descendantHandle =
        ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(descendant));
    CHECK(childHandle != nullptr && descendantHandle != nullptr);
#endif
    const auto before = std::chrono::steady_clock::now();
    manager.stopOwner("flood-owner");
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));
#if defined(_WIN32)
    CHECK(::WaitForSingleObject(childHandle, 0) == WAIT_OBJECT_0);
    CHECK(::WaitForSingleObject(descendantHandle, 0) == WAIT_OBJECT_0);
    ::CloseHandle(childHandle);
    ::CloseHandle(descendantHandle);
#else
    CHECK(!processAlive(pid));
    CHECK(!processAlive(descendant));
#endif
    CHECK(manager.interact("flood-owner", flood.value(), {}, 0).error().code() ==
          MaiErrorCode::NotFound);
    const auto unaffected = manager.interact("other-owner", idle.value(), {}, 0);
    CHECK(unaffected && !unaffected.value().completed && !unaffected.value().tty);
    const auto stopped = manager.interact("other-owner", idle.value(), {}, 3000, true);
    CHECK(stopped && stopped.value().completed && stopped.value().terminated);
}

void test_large_terminal_output_timeout_drains_and_reaps() {
    Workspace workspace;
    MaiShellSessionManager manager;
    auto request = options(workspace, "--pty-flood", true);
    request.timeoutMs = 1000;
    const auto id = manager.start("owner", request);
    CHECK(id);
    if (!id) return;
    const auto before = std::chrono::steady_clock::now();
    const auto result = manager.interact("owner", id.value(), {}, 3000, false, {}, true);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(4));
    CHECK(result);
    if (!result) return;
    const auto& state = result.value();
    CHECK(state.completed && state.timedOut && state.tty && !state.exitCode);
    CHECK(!state.error && state.totalOutputBytes >= 64 * 1024);
    CHECK(state.output.size() <= 16 * 1024 * 1024);
    CHECK(state.captureTruncated == (state.totalOutputBytes > 16 * 1024 * 1024));
    const auto pid = markerId(state.output, "PID=");
    const auto descendant = markerId(state.output, "DESCENDANT=");
    CHECK(pid && !processAlive(pid));
    CHECK(descendant && !processAlive(descendant));
}

void test_tool_input_preserves_terminal_metadata() {
    Workspace workspace;
    auto manager = std::make_shared<MaiShellSessionManager>();
    auto shell = makeMaiExecCommandTool(manager);
    auto input = makeMaiWriteStdinTool(manager);
    auto request = arguments("--pty-interactive", true);
    request["yield_time_ms"] = 100;
    const auto initial = shell->execute(request.dump(), workspace.context());
    const auto initialMetadata = metadata(initial);
    CHECK(!initial.hasError() && initialMetadata.contains("session_id"));
    if (!initialMetadata.contains("session_id")) return;
    CHECK(initialMetadata["session_id"].is_number_integer());
    const auto id = initialMetadata["session_id"].get<int>();
    CHECK(id > 0);
    CHECK(initial.execution() && initial.execution()->status == MaiToolExecutionStatus::Running);
    CHECK(initialMetadata.value("output", "").find("ATTACHED=1") != std::string::npos);
    const auto result = input->execute(
        json{{"session_id", id}, {"chars", "hello\r"}, {"yield_time_ms", 3000}}.dump(),
        workspace.context());
    CHECK(!result.hasError());
    std::string output = initialMetadata.value("output", "") + metadata(result).value("output", "");
    for (int attempt = 0; attempt < 30 && output.find("INPUT_OK") == std::string::npos; ++attempt) {
        const auto polled = input->execute(json{{"session_id", id}, {"yield_time_ms", 100}}.dump(),
                                           workspace.context());
        CHECK(!polled.hasError());
        output += metadata(polled).value("output", "");
    }
    CHECK(output.find("INPUT_OK") != std::string::npos);
    // 终端事实留在普通快照，模型 JSON 不重复带出 tty 或 shell 解释器元数据。
    std::string trailing;
    const auto native =
        collect(*manager, workspace.context().sessionId, std::to_string(id), trailing);
    CHECK(native.tty && native.completed && native.exitCode == 0);
}

void test_tool_contract_and_terminal_attachment() {
    Workspace workspace;
    auto manager = std::make_shared<MaiShellSessionManager>();
    auto shell = makeMaiExecCommandTool(manager);
    auto input = makeMaiWriteStdinTool(manager);
    const auto schema = json::parse(shell->parametersSchema());
    CHECK(schema["properties"].contains("tty"));
    if (schema["properties"].contains("tty")) {
        CHECK(schema["properties"]["tty"]["type"] == "boolean");
        CHECK(schema["properties"]["tty"]["default"] == false);
    }
    const auto request = arguments("--pty-probe", true);
    auto pipeRequest = request;
    pipeRequest["tty"] = false;
    auto omitted = pipeRequest;
    omitted.erase("tty");
    CHECK(shell->approvalKeys(omitted.dump(), workspace.context()) ==
          shell->approvalKeys(pipeRequest.dump(), workspace.context()));
    CHECK(!MaiShellSessionOptions{}.tty && !MaiShellSessionSnapshot{}.tty);
    const auto pipeResult = shell->execute(omitted.dump(), workspace.context());
    CHECK(!pipeResult.hasError());
    CHECK(metadata(pipeResult)
              .value("output", std::string{})
              .find("ATTACHED=0\nUTF8=\xE7\x95\x8C\xF0\x9F\x99\x82\nREADY\n") == 0);
    const auto pipeMetadata = metadata(pipeResult);
    CHECK(pipeMetadata.is_object() && !pipeMetadata.contains("tty"));
    CHECK(shell->approvalKeys(request.dump(), workspace.context()) !=
          shell->approvalKeys(pipeRequest.dump(), workspace.context()));
    CHECK(shell->requiresApproval(json{{"cmd", "echo safe"}, {"tty", true}}.dump()));
    const auto result = shell->execute(request.dump(), workspace.context());
    CHECK(!result.hasError());
    CHECK(metadata(result).value("output", std::string{}).find("ATTACHED=1") != std::string::npos);
    CHECK(
        metadata(result).value("output", std::string{}).find("UTF8=\xE7\x95\x8C\xF0\x9F\x99\x82") !=
        std::string::npos);
    const auto details = metadata(result);
    CHECK(details.is_object());
    if (details.is_object()) CHECK(!details.contains("tty"));
    auto invalid = request;
    invalid["tty"] = "true";
    CHECK(shell->execute(invalid.dump(), workspace.context()).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(input->execute(json{{"session_id", 1}, {"tty", true}}.dump(), workspace.context())
              .error()
              .code() == MaiErrorCode::InvalidInput);
}
}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    wchar_t path[32768]{};
    const auto size = ::GetModuleFileNameW(nullptr, path, 32768);
    executable = MaiFilePath(std::wstring(path, size)).toUtf8();
#else
    executable = MaiFileSystem::resolve(MaiFilePath::fromUtf8(argv[0])).toUtf8();
#endif
    if (argc == 2 && std::strncmp(argv[1], "--pty-", 6) == 0) return child(argv[1]);
    test_tool_contract_and_terminal_attachment();
    test_manager_interaction_and_default_pipe_bytes();
    test_timeout_cancel_and_explicit_termination();
    test_large_output_does_not_deadlock_stop_owner();
    test_large_terminal_output_timeout_drains_and_reaps();
    test_tool_input_preserves_terminal_metadata();
    std::printf("PTY failures=%d\n", failures);
    return failures ? 1 : 0;
}
