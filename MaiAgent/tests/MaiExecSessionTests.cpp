// 文件作用：核对命令会话的整数标识、无默认期限、关闭的管道 stdin 和显式 PTY 交互契约。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiShellSessionManager.h"

namespace {
int failures = 0;
std::string executable;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

void writeChild(const char* text) {
#if defined(_WIN32)
    DWORD written = 0;
    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(std::strlen(text)),
                &written, nullptr);
#else
    const auto ignored = ::write(STDOUT_FILENO, text, std::strlen(text));
    (void)ignored;
#endif
}

int child(const std::string& mode) {
#if defined(_WIN32)
    if (mode == "--exec-background" || mode == "--exec-background-root" ||
        mode == "--exec-background-short") {
        const auto application = MaiFilePath::fromUtf8(executable).value();
        std::wstring command =
            L"\"" + application +
            (mode == "--exec-background-short" ? L"\" --exec-delay" : L"\" --exec-idle");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        // 模拟 Node detached + stdio:ignore；仍继承 manager 的 Job，不能用 breakaway 绕过所有权。
        if (!::CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
                              DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                              &startup, &process))
            return 2;
        char pid[64]{};
        std::snprintf(pid, sizeof(pid), "BACKGROUND_PID=%lu\n", process.dwProcessId);
        writeChild(pid);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        if (mode == "--exec-background-root")
            for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
    }
#endif
    writeChild("READY\n");
    if (mode == "--exec-probe") return 0;
    if (mode == "--exec-delay") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        writeChild("DELAY_DONE\n");
        return 0;
    }
    if (mode == "--exec-idle")
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    char bytes[64]{};
#if defined(_WIN32)
    DWORD read = 0;
    const bool okay =
        ::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &read, nullptr);
    const bool eof = !okay || read == 0;
    const std::string received(bytes, read);
#else
    const auto read = ::read(STDIN_FILENO, bytes, sizeof(bytes));
    const bool eof = read == 0;
    const std::string received(bytes, read > 0 ? static_cast<std::size_t>(read) : 0);
#endif
    writeChild(eof                                           ? "EOF_OBSERVED\n"
               : received.find("hello") != std::string::npos ? "INPUT_OK\n"
                                                             : "INPUT_BAD\n");
    return 0;
}

struct Workspace {
    MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-exec_")));
    Workspace() {
        CHECK(!MaiFileSystem::createDirectories(root));
    }
    ~Workspace() {
        MaiFileSystem::removeRecursively(root);
    }
};

MaiShellSessionOptions options(const Workspace& workspace, const char* mode, bool tty = false) {
    MaiShellSessionOptions value;
    value.command = "\"" + executable + "\" " + mode;
    value.workdir = workspace.root.toUtf8();
#if defined(_WIN32)
    value.shell = "cmd";
#else
    value.shell = "sh";
#endif
    value.login = false;
    value.tty = tty;
    return value;
}

bool canonicalId(const std::string& id) {
    if (id.empty() || id.front() < '1' || id.front() > '9') return false;
    for (const auto ch : id)
        if (ch < '0' || ch > '9') return false;
    try {
        const auto number = std::stoull(id);
        return number <= 2147483647ULL && std::to_string(number) == id;
    } catch (...) {
        return false;
    }
}

MaiShellSessionSnapshot collect(MaiShellSessionManager& manager, const std::string& owner,
                                const std::string& id, std::string& output,
                                bool readyOnly = false) {
    MaiShellSessionSnapshot state;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const auto result = manager.interact(owner, id, {}, 100);
        CHECK(result);
        if (!result) break;
        state = result.value();
        output += state.output;
        if (state.completed || (readyOnly && output.find("READY") != std::string::npos)) break;
    }
    return state;
}

void test_ids_are_decimal_i32_and_not_reused() {
    Workspace workspace;
    MaiShellSessionManager manager;
    std::set<std::string> ids;
    for (int index = 0; index < 8; ++index) {
        const auto id = manager.start("owner", options(workspace, "--exec-probe"));
        CHECK(id);
        if (!id) continue;
        CHECK(canonicalId(id.value()));
        CHECK(ids.insert(id.value()).second);
        std::string output;
        CHECK(collect(manager, "owner", id.value(), output).completed);
        manager.stopOwner("owner");
    }
}

void test_zero_is_unlimited_but_explicit_timeout_still_applies() {
    CHECK(MaiShellSessionOptions{}.timeoutMs == 0);
    CHECK(MaiShellSessionSnapshot{}.timeoutMs == 0);
    Workspace workspace;
    for (const bool tty : {false, true}) {
        MaiShellSessionManager manager;
        auto request = options(workspace, "--exec-delay", tty);
        request.timeoutMs = 0;
        const auto id = manager.start("owner", request);
        CHECK(id);
        if (!id) continue;
        const auto result = manager.interact("owner", id.value(), {}, 3000, false, {}, true);
        CHECK(result);
        if (result) {
            CHECK(result.value().completed && !result.value().timedOut &&
                  result.value().exitCode == 0);
            CHECK(result.value().timeoutMs == 0);
            CHECK(result.value().output.find("DELAY_DONE") != std::string::npos);
        }
        request.command = options(workspace, "--exec-idle", tty).command;
        request.timeoutMs = 1000;
        const auto bounded = manager.start("owner", request);
        CHECK(bounded);
        if (!bounded) continue;
        const auto timeout = manager.interact("owner", bounded.value(), {}, 3000, false, {}, true);
        CHECK(timeout && timeout.value().completed && timeout.value().timedOut &&
              !timeout.value().exitCode);
    }
}

void test_pipe_stdin_is_closed_and_exact_ctrl_c_stops_owned_process() {
    Workspace workspace;
    MaiShellSessionManager manager;
    auto request = options(workspace, "--exec-read");
    request.timeoutMs = 2000;
    const auto reader = manager.start("owner", request);
    CHECK(reader);
    if (reader) {
        const auto result = manager.interact("owner", reader.value(), {}, 3000, false, {}, true);
        CHECK(result && result.value().completed && !result.value().timedOut);
        if (result) CHECK(result.value().output.find("EOF_OBSERVED") != std::string::npos);
    }
    request.command = options(workspace, "--exec-idle").command;
    const auto id = manager.start("owner", request);
    CHECK(id);
    if (!id) return;
    std::string output;
    collect(manager, "owner", id.value(), output, true);
    CHECK(manager.interact("owner", id.value(), "hello\n", 0).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(manager.interact("owner", id.value(), std::string("\x03", 1) + "x", 0).error().code() ==
          MaiErrorCode::InvalidInput);
    CHECK(manager.interact("foreign", id.value(), "\x03", 0).error().code() ==
          MaiErrorCode::NotFound);
    const auto stopped = manager.interact("owner", id.value(), "\x03", 3000, false, {}, true);
    CHECK(stopped && stopped.value().completed && stopped.value().terminated &&
          !stopped.value().timedOut);
    if (stopped) CHECK(!stopped.value().exitCode && !stopped.value().canceled);
}

void test_tty_retains_stdin_and_native_ctrl_c() {
    Workspace workspace;
    MaiShellSessionManager manager;
    auto request = options(workspace, "--exec-read", true);
    request.timeoutMs = 5000;
    const auto id = manager.start("owner", request);
    CHECK(id);
    if (!id) return;
    std::string output;
    CHECK(!collect(manager, "owner", id.value(), output, true).completed);
    CHECK(manager.interact("foreign", id.value(), "hello\r", 0).error().code() ==
          MaiErrorCode::NotFound);
    const auto sent = manager.interact("owner", id.value(), "hello\r", 3000, false, {}, true);
    CHECK(sent && sent.value().completed && sent.value().exitCode == 0);
    if (sent) output += sent.value().output;
    CHECK(output.find("INPUT_OK") != std::string::npos);
    request.command = options(workspace, "--exec-idle", true).command;
    const auto interruptible = manager.start("owner", request);
    CHECK(interruptible);
    if (!interruptible) return;
    output.clear();
    collect(manager, "owner", interruptible.value(), output, true);
    const auto interrupted =
        manager.interact("owner", interruptible.value(), "\x03", 3000, false, {}, true);
    CHECK(interrupted && interrupted.value().completed && !interrupted.value().timedOut &&
          !interrupted.value().canceled);
    if (interrupted) CHECK(!interrupted.value().exitCode || *interrupted.value().exitCode != 0);
}

void test_concurrent_ids_and_independent_manager_native_handles() {
    Workspace workspace;
    MaiShellSessionManager manager;
    std::vector<std::string> ids(4);
    std::vector<int> errors(4, 0);
    std::vector<std::thread> starts;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        starts.emplace_back([&, index] {
            const auto id = manager.start("owner", options(workspace, "--exec-idle", true));
            if (id)
                ids[index] = id.value();
            else
                errors[index] = static_cast<int>(id.error().code());
        });
    }
    for (auto& thread : starts) thread.join();
    std::set<std::string> distinct;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        CHECK(errors[index] == 0 && canonicalId(ids[index]));
        CHECK(distinct.insert(ids[index]).second);
    }
    MaiShellSessionManager independent;
    const auto other = independent.start("owner", options(workspace, "--exec-idle", true));
    CHECK(other && canonicalId(other.value()));
    // 公共随机 ID 的作用域独立；原生命名管道仍须独立命名，不能依赖碰撞概率低这一点。
    if (other) {
        const auto stopped = independent.interact("owner", other.value(), {}, 3000, true);
        CHECK(stopped && stopped.value().completed);
    }
    for (const auto& id : ids) {
        if (id.empty()) continue;
        const auto state = manager.interact("owner", id, {}, 0);
        CHECK(state && !state.value().completed);
    }
    manager.stopAll();
}

void test_ordinary_timeout_rejects_out_of_contract_values() {
    Workspace workspace;
    MaiShellSessionManager manager;
    auto request = options(workspace, "--exec-probe");
    for (const int value : {-1, 1, 999, 600001}) {
        request.timeoutMs = value;
        CHECK(manager.start("owner", request).error().code() == MaiErrorCode::InvalidInput);
    }
}

void test_reconstructed_manager_rejects_stale_id_for_same_owner() {
    Workspace workspace;
    std::string stale;
    {
        MaiShellSessionManager first;
        const auto id = first.start("persisted-owner", options(workspace, "--exec-probe"));
        CHECK(id);
        if (!id) return;
        stale = id.value();
        std::string output;
        CHECK(collect(first, "persisted-owner", stale, output).completed);
    }
    MaiShellSessionManager reconstructed;
    const auto current =
        reconstructed.start("persisted-owner", options(workspace, "--exec-idle", true));
    CHECK(current);
    if (!current) return;
    CHECK(current.value() != stale);
    CHECK(reconstructed.interact("persisted-owner", stale, "unexpected\r", 0).error().code() ==
          MaiErrorCode::NotFound);
    const auto state = reconstructed.interact("persisted-owner", current.value(), {}, 0);
    CHECK(state && !state.value().completed);
}

void test_full_window_poll_aggregates_queued_output_and_accepts_long_empty_wait() {
    Workspace workspace;
    MaiShellSessionManager manager;
    const auto id = manager.start("owner", options(workspace, "--exec-idle"));
    CHECK(id);
    if (!id) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto before = std::chrono::steady_clock::now();
    const auto result = manager.interact("owner", id.value(), {}, 250, false, {}, true);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(result && !result.value().completed);
    CHECK(elapsed >= std::chrono::milliseconds(200));
    if (result) CHECK(result.value().output.find("READY") != std::string::npos);
    const auto canceled =
        manager.interact("owner", id.value(), {}, 300000, false, [] { return true; }, true);
    CHECK(canceled && canceled.value().completed && canceled.value().canceled);
}

#if defined(_WIN32)
HANDLE backgroundProcess(const std::string& output) {
    const auto marker = output.find("BACKGROUND_PID=");
    CHECK(marker != std::string::npos);
    if (marker == std::string::npos) return nullptr;
    const auto pid = std::strtoul(output.c_str() + marker + 15, nullptr, 10);
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    CHECK(process != nullptr);
    return process;
}

void test_normal_exit_preserves_owned_background_until_explicit_cleanup() {
    Workspace workspace;
    // 各种清理入口都操作此测试创建且持有句柄的进程，避免通过复用 PID 误杀其他程序。
    for (const int testCase : {0, 1, 2, 3, 4, 5, 6, 7}) {
        const int cleanup = testCase % 4;
        const bool tty = testCase >= 4;
        auto manager = std::make_unique<MaiShellSessionManager>();
        const auto id =
            manager->start("background-owner", options(workspace, "--exec-background", tty));
        CHECK(id);
        if (!id) continue;
        const auto result =
            manager->interact("background-owner", id.value(), {}, 3000, false, {}, true);
        CHECK(result && result.value().completed && result.value().exitCode == 0);
        if (!result) continue;
        CHECK(result.value().tty == tty);
        CHECK(result.value().backgroundRunning && !result.value().error);
        CHECK(manager->interact("background-owner", id.value(), "hello\n", 0).error().code() ==
              MaiErrorCode::InvalidInput);
        HANDLE process = backgroundProcess(result.value().output);
        if (!process) continue;
        CHECK(::WaitForSingleObject(process, 200) == WAIT_TIMEOUT);
        // 独立命令不得替换旧 Job，也不得因前台完成而回收仍有后台成员的记录。
        const auto next =
            manager->start("background-owner", options(workspace, "--exec-probe", tty));
        CHECK(next);
        if (next) {
            const auto probe =
                manager->interact("background-owner", next.value(), {}, 3000, false, {}, true);
            CHECK(probe && probe.value().completed && probe.value().exitCode == 0);
        }
        CHECK(::WaitForSingleObject(process, 0) == WAIT_TIMEOUT);
        if (cleanup == 0) {
            const auto stopped =
                manager->interact("background-owner", id.value(), {}, 3000, true, {}, true);
            CHECK(stopped && stopped.value().completed && stopped.value().terminated);
            if (stopped) CHECK(!stopped.value().backgroundRunning && !stopped.value().error);
        } else if (cleanup == 1) {
            const auto canceled = manager->interact(
                "background-owner", id.value(), {}, 3000, false, [] { return true; }, true);
            CHECK(canceled && canceled.value().completed && canceled.value().canceled);
            if (canceled) CHECK(!canceled.value().backgroundRunning && !canceled.value().error);
        } else if (cleanup == 2) {
            manager->stopOwner("background-owner");
        } else {
            manager.reset();
        }
        CHECK(::WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
        ::CloseHandle(process);
    }
}

void test_owned_background_obeys_explicit_deadline_and_natural_exit() {
    Workspace workspace;
    for (const int testCase : {0, 1, 2, 3}) {
        const bool timeout = testCase % 2 == 1;
        const bool tty = testCase >= 2;
        MaiShellSessionManager manager;
        auto request =
            options(workspace, timeout ? "--exec-background" : "--exec-background-short", tty);
        request.timeoutMs = timeout ? 1000 : 0;
        const auto id = manager.start("owner", request);
        CHECK(id);
        if (!id) continue;
        auto result = manager.interact("owner", id.value(), {}, 3000, false, {}, true);
        CHECK(result && result.value().completed && result.value().exitCode == 0 &&
              result.value().backgroundRunning);
        if (!result) continue;
        HANDLE process = backgroundProcess(result.value().output);
        if (process) CHECK(::WaitForSingleObject(process, 3000) == WAIT_OBJECT_0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // completed 属于前台；后台自然结束后还须等 worker 释放资源，不能靠它替代后台状态。
        while (result && result.value().backgroundRunning &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            result = manager.interact("owner", id.value(), {}, 0);
        }
        CHECK(result && !result.value().backgroundRunning && result.value().completed &&
              result.value().timedOut == timeout && result.value().exitCode == 0 &&
              !result.value().error);
        if (process) ::CloseHandle(process);
    }
}

void test_background_jobs_keep_owner_and_manager_capacity_until_reaped() {
    Workspace workspace;
    MaiShellSessionManager manager;
    std::vector<HANDLE> processes;
    for (const auto* owner : {"first-owner", "second-owner"}) {
        for (int index = 0; index < 12; ++index) {
            const auto id =
                manager.start(owner, options(workspace, "--exec-background", index % 2 == 1));
            CHECK(id);
            if (!id) continue;
            const auto result = manager.interact(owner, id.value(), {}, 3000, false, {}, true);
            CHECK(result && result.value().completed && result.value().backgroundRunning);
            if (result) processes.push_back(backgroundProcess(result.value().output));
        }
        CHECK(manager.start(owner, options(workspace, "--exec-probe")).error().code() ==
              MaiErrorCode::Busy);
    }
    CHECK(manager.start("third-owner", options(workspace, "--exec-probe")).error().code() ==
          MaiErrorCode::Busy);
    manager.stopAll();
    for (auto process : processes) {
        if (!process) continue;
        CHECK(::WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
        ::CloseHandle(process);
    }
    const auto next = manager.start("third-owner", options(workspace, "--exec-probe"));
    CHECK(next);
    if (next) {
        const auto result =
            manager.interact("third-owner", next.value(), {}, 3000, false, {}, true);
        CHECK(result && result.value().completed && result.value().exitCode == 0);
    }
}

void test_cancel_running_root_still_cleans_detached_descendant() {
    Workspace workspace;
    MaiShellSessionManager manager;
    const auto id = manager.start("owner", options(workspace, "--exec-background-root"));
    CHECK(id);
    if (!id) return;
    std::string output;
    for (int attempt = 0; attempt < 30 && output.find("BACKGROUND_PID=") == std::string::npos;
         ++attempt) {
        const auto result = manager.interact("owner", id.value(), {}, 100);
        CHECK(result && !result.value().completed);
        if (result) output += result.value().output;
    }
    HANDLE process = backgroundProcess(output);
    const auto result =
        manager.interact("owner", id.value(), {}, 3000, false, [] { return true; }, true);
    CHECK(result && result.value().completed && result.value().canceled &&
          !result.value().exitCode);
    if (process) {
        CHECK(::WaitForSingleObject(process, 0) == WAIT_OBJECT_0);
        ::CloseHandle(process);
    }
}
#endif
}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    wchar_t path[32768]{};
    const auto length = ::GetModuleFileNameW(nullptr, path, 32768);
    executable = MaiFilePath(std::wstring(path, length)).toUtf8();
#else
    executable = MaiFileSystem::resolve(MaiFilePath::fromUtf8(argv[0])).toUtf8();
#endif
    if (argc == 2 && std::strncmp(argv[1], "--exec-", 7) == 0) return child(argv[1]);
    test_ids_are_decimal_i32_and_not_reused();
    test_zero_is_unlimited_but_explicit_timeout_still_applies();
    test_pipe_stdin_is_closed_and_exact_ctrl_c_stops_owned_process();
    test_tty_retains_stdin_and_native_ctrl_c();
    test_concurrent_ids_and_independent_manager_native_handles();
    test_ordinary_timeout_rejects_out_of_contract_values();
    test_reconstructed_manager_rejects_stale_id_for_same_owner();
    test_full_window_poll_aggregates_queued_output_and_accepts_long_empty_wait();
#if defined(_WIN32)
    test_normal_exit_preserves_owned_background_until_explicit_cleanup();
    test_cancel_running_root_still_cleans_detached_descendant();
    test_owned_background_obeys_explicit_deadline_and_natural_exit();
    test_background_jobs_keep_owner_and_manager_capacity_until_reaped();
#endif
    std::printf("Exec session failures=%d\n", failures);
    return failures ? 1 : 0;
}
