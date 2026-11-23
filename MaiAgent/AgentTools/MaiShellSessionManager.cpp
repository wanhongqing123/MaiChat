// 文件作用：以原生管道、进程树和有界捕获实现可续读 shell 进程，不改变阻塞 maiRunProcess。
#include "MaiShellSessionManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiProcess.h"
#include "MaiShellEnvironment.h"
#include "MaiThread.h"
#include "MaiToolOutput.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kRingBytes = 1024 * 1024;
constexpr std::size_t kStdinBytes = 64 * 1024;
constexpr int kPollMs = 10;

struct ShellSession {
    std::string id;
    std::string owner;
    MaiShellSessionOptions options;
    MaiShellEnvironment environment;
    std::vector<std::string> arguments;
    std::mutex mutex;
    std::mutex calls;
    std::mutex joining;
    std::condition_variable changed;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> cancel{false};
    bool startupChecked = false;
    bool started = false;
    bool done = false;
    bool timedOut = false;
    bool terminated = false;
    bool stdinClosed = false;
    std::string input;
    std::string capture;
    std::vector<char> ring;
    std::uint64_t total = 0;
    std::uint64_t cursor = 0;
    std::uint64_t discarded = 0;
    std::optional<int> exitCode;
    MaiError error;

    ~ShellSession() {
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id())
                worker.detach();
            else
                worker.join();
        }
    }
};

class NativeCleanup {
public:
    explicit NativeCleanup(std::function<void()> callback) : mCallback(std::move(callback)) {}
    ~NativeCleanup() {
        mCallback();
    }

private:
    std::function<void()> mCallback;
};

void appendOutput(ShellSession& session, const char* bytes, std::size_t size) {
    std::lock_guard<std::mutex> lock(session.mutex);
    if (session.ring.empty()) session.ring.resize(kRingBytes);
    const auto retained = std::min(size, kMaiToolOutputMaxBytes - session.capture.size());
    session.capture.append(bytes, retained);
    const std::size_t position = static_cast<std::size_t>(session.total % kRingBytes);
    const auto first = std::min(size, kRingBytes - position);
    std::memcpy(session.ring.data() + position, bytes, first);
    if (first < size) std::memcpy(session.ring.data(), bytes + first, size - first);
    session.total += size;
    session.changed.notify_all();
}

void startup(ShellSession& session, const MaiError& error = {}) {
    std::lock_guard<std::mutex> lock(session.mutex);
    session.error = error;
    if (!session.startupChecked) session.started = !error;
    session.startupChecked = true;
    session.changed.notify_all();
}

void finish(ShellSession& session, std::optional<int> exitCode, const MaiError& error = {}) {
    std::lock_guard<std::mutex> lock(session.mutex);
    if (error) session.error = error;
    session.exitCode = exitCode;
    session.done = true;
    session.stdinClosed = true;
    session.input.clear();
    session.changed.notify_all();
}

std::string takeInput(ShellSession& session, std::size_t limit) {
    std::lock_guard<std::mutex> lock(session.mutex);
    const auto size = std::min(limit, session.input.size());
    auto bytes = session.input.substr(0, size);
    session.input.erase(0, size);
    return bytes;
}

bool mustStop(ShellSession& session, const Clock::time_point& deadline) {
    if (session.stop.load() || session.cancel.load()) return true;
    if (Clock::now() < deadline) return false;
    std::lock_guard<std::mutex> lock(session.mutex);
    session.timedOut = true;
    return true;
}

#if defined(_WIN32)
// 与 Windows 原生 argv 约定一致：只在引号前和被引用参数末尾加倍反斜杠。
// cmd 的命令体仍交给 /d /c 原样解释，其他解释器的 -Command/-c 命令体必须是一个参数。
std::wstring quoteWindowsArgument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(ch == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        result.push_back(ch);
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

void closeHandle(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    handle = nullptr;
}

void runNative(ShellSession& session) {
    HANDLE outputRead = nullptr, outputWrite = nullptr, inputWrite = INVALID_HANDLE_VALUE,
           inputRead = INVALID_HANDLE_VALUE, job = nullptr;
    PROCESS_INFORMATION process{};
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto cleanup = [&] {
        closeHandle(inputRead);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(outputWrite);
        closeHandle(process.hThread);
        closeHandle(process.hProcess);
        closeHandle(job);
    };
    NativeCleanup nativeCleanup(cleanup);
    const auto fail = [&](const char* message) {
        const MaiError error{MaiErrorCode::Internal, message};
        startup(session, error);
        finish(session, {}, error);
        cleanup();
    };
    // stdin 使用 overlapped 命名管道；不读 stdin 的子进程不能卡住取消或超时线程。
    const auto pipeName = MaiFilePath::fromUtf8("\\\\.\\pipe\\MaiShell-" + session.id).value();
    inputWrite = ::CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, static_cast<DWORD>(kStdinBytes),
        static_cast<DWORD>(kStdinBytes), 0, nullptr);
    if (inputWrite == INVALID_HANDLE_VALUE) {
        fail("cannot create shell stdin pipe");
        return;
    }
    inputRead =
        ::CreateFileW(pipeName.c_str(), GENERIC_READ, 0, &inherit, OPEN_EXISTING, 0, nullptr);
    if (inputRead == INVALID_HANDLE_VALUE) {
        fail("cannot connect shell stdin pipe");
        return;
    }
    if (!::ConnectNamedPipe(inputWrite, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) {
        fail("cannot initialize shell stdin pipe");
        return;
    }
    if (!::CreatePipe(&outputRead, &outputWrite, &inherit, 64 * 1024) ||
        !::SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) {
        fail("cannot create shell output pipe");
        return;
    }
    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = inputRead;
    startupInfo.StartupInfo.hStdOutput = outputWrite;
    startupInfo.StartupInfo.hStdError = outputWrite;
    SIZE_T attributeBytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributes(attributeBytes);
    startupInfo.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    HANDLE inherited[] = {inputRead, outputWrite};
    if (!::InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeBytes)) {
        fail("cannot initialize shell handle inheritance");
        return;
    }
    if (!::UpdateProcThreadAttribute(startupInfo.lpAttributeList, 0,
                                     PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                     sizeof(inherited), nullptr, nullptr)) {
        ::DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        fail("cannot restrict shell handle inheritance");
        return;
    }
    const auto application = MaiFilePath::fromUtf8(session.environment.path).value();
    std::wstring line;
    if (session.environment.type == MaiShellType::Cmd) {
        line = quoteWindowsArgument(application);
        for (std::size_t index = 1; index + 1 < session.arguments.size(); ++index)
            line += L" " +
                    quoteWindowsArgument(MaiFilePath::fromUtf8(session.arguments[index]).value());
        // /s 让 cmd 只剥离这对外层引号，保留命令体中“带空格程序 + 带引号参数”。
        line += L" \"" + MaiFilePath::fromUtf8(session.arguments.back()).value() + L"\"";
    } else {
        for (const auto& argument : session.arguments) {
            if (!line.empty()) line.push_back(L' ');
            line += quoteWindowsArgument(MaiFilePath::fromUtf8(argument).value());
        }
    }
    std::vector<wchar_t> mutableLine(line.begin(), line.end());
    mutableLine.push_back(0);
    const auto directory = MaiFilePath::fromUtf8(session.options.workdir).value();
    const BOOL created =
        ::CreateProcessW(application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP |
                             EXTENDED_STARTUPINFO_PRESENT,
                         nullptr, directory.c_str(), &startupInfo.StartupInfo, &process);
    ::DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    closeHandle(inputRead);
    closeHandle(outputWrite);
    if (!created) {
        fail("could not start shell in the requested working directory");
        return;
    }
    job = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job ||
        !::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits)) ||
        !::AssignProcessToJobObject(job, process.hProcess)) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 2000);
        fail("could not configure shell process-tree termination");
        return;
    }
    if (::ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        ::TerminateJobObject(job, 1);
        ::WaitForSingleObject(process.hProcess, 2000);
        fail("could not resume shell process");
        return;
    }
    startup(session);
    const auto deadline = Clock::now() + std::chrono::milliseconds(session.options.timeoutMs);
    OVERLAPPED writing{};
    writing.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool pending = false;
    std::string pendingBytes;
    // 此守卫在写缓冲之后构造，异常展开时先取消并等待 I/O，再销毁其借用的缓冲。
    NativeCleanup inputCleanup([&] {
        if (pending) {
            ::CancelIoEx(inputWrite, &writing);
            DWORD ignored = 0;
            ::GetOverlappedResult(inputWrite, &writing, &ignored, TRUE);
            pending = false;
        }
        closeHandle(writing.hEvent);
    });
    MaiError error;
    char buffer[16384];
    const auto drain = [&] {
        bool received = false;
        // 每轮最多消费 256KiB；持续输出不会饿死 timeout、stdin 或停止检查。
        for (int chunk = 0; chunk < 16; ++chunk) {
            DWORD available = 0, got = 0;
            if (!::PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr) ||
                !available)
                break;
            if (!::ReadFile(outputRead, buffer, std::min<DWORD>(available, sizeof(buffer)), &got,
                            nullptr) ||
                !got)
                break;
            appendOutput(session, buffer, got);
            received = true;
        }
        return received;
    };
    if (!writing.hEvent)
        error = {MaiErrorCode::Internal, "cannot create shell stdin completion event"};
    bool stopped = false;
    while (!error) {
        const bool received = drain();
        if (mustStop(session, deadline)) {
            ::TerminateJobObject(job, 1);
            stopped = true;
            break;
        }
        if (::WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;
        if (pending) {
            DWORD written = 0;
            if (::GetOverlappedResult(inputWrite, &writing, &written, FALSE)) {
                pending = false;
                pendingBytes.erase(0, written);
            } else if (::GetLastError() != ERROR_IO_INCOMPLETE) {
                pending = false;
                pendingBytes.clear();
                std::lock_guard<std::mutex> lock(session.mutex);
                session.stdinClosed = true;
            }
        }
        if (!pending && writing.hEvent) {
            if (pendingBytes.empty()) pendingBytes = takeInput(session, 4096);
            if (!pendingBytes.empty()) {
                ::ResetEvent(writing.hEvent);
                DWORD written = 0;
                if (::WriteFile(inputWrite, pendingBytes.data(),
                                static_cast<DWORD>(pendingBytes.size()), &written, &writing)) {
                    pendingBytes.erase(0, written);
                } else if (::GetLastError() == ERROR_IO_PENDING)
                    pending = true;
                else {
                    pendingBytes.clear();
                    std::lock_guard<std::mutex> lock(session.mutex);
                    session.stdinClosed = true;
                }
            }
        }
        // 有输出时立即继续检查状态，避免每次管道唤醒都引入一次固定吞吐等待。
        if (received)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    // 普通父进程退出也关闭 Job，后代不能脱离 session 继续持有管道或执行副作用。
    ::TerminateJobObject(job, 1);
    ::WaitForSingleObject(process.hProcess, 2000);
    if (pending) {
        ::CancelIoEx(inputWrite, &writing);
        DWORD ignored = 0;
        ::GetOverlappedResult(inputWrite, &writing, &ignored, TRUE);
        pending = false;
    }
    closeHandle(writing.hEvent);
    // 父进程退出不等于 Job 后代退出；在宣告 completed 前确认整棵树已清空。
    bool treeStopped = false;
    const auto treeDeadline = Clock::now() + std::chrono::seconds(2);
    do {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                        sizeof(accounting), nullptr) &&
            accounting.ActiveProcesses == 0) {
            treeStopped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    } while (Clock::now() < treeDeadline);
    if (!treeStopped && !error)
        error = {MaiErrorCode::Internal, "could not confirm shell process-tree termination"};
    // 子孙已终止，抽干有限的剩余管道字节。
    for (;;) {
        DWORD available = 0;
        if (!::PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr) || !available)
            break;
        drain();
    }
    DWORD code = 0;
    const bool haveExit = ::GetExitCodeProcess(process.hProcess, &code) && code != STILL_ACTIVE;
    if (!haveExit && !error)
        error = {MaiErrorCode::Internal, "could not confirm shell termination"};
    const auto exit =
        haveExit && !stopped && !error ? std::optional<int>(static_cast<int>(code)) : std::nullopt;
    cleanup();
    finish(session, exit, error);
}
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
void runNative(ShellSession& session) {
    const MaiError error{MaiErrorCode::NotSupported,
                         "process execution is unavailable on this platform"};
    startup(session, error);
    finish(session, {}, error);
}
#else
void runNative(ShellSession& session) {
    int output[2] = {-1, -1}, input[2] = {-1, -1}, launch[2] = {-1, -1};
    pid_t child = -1;
    const auto closePair = [](int* pair) {
        for (int index = 0; index < 2; ++index) {
            if (pair[index] >= 0) ::close(pair[index]);
            pair[index] = -1;
        }
    };
    NativeCleanup nativeCleanup([&] {
        if (child > 0) {
            ::kill(-child, SIGKILL);
            ::kill(child, SIGKILL);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
        closePair(output);
        closePair(input);
        closePair(launch);
    });
    const auto fail = [&](const char* message) {
        MaiError error{MaiErrorCode::Internal, message};
        startup(session, error);
        finish(session, {}, error);
        closePair(output);
        closePair(input);
        closePair(launch);
    };
    if (::pipe(output) || ::pipe(input) || ::pipe(launch)) {
        fail("cannot create shell pipes");
        return;
    }
    for (int fd : {output[0], output[1], input[0], input[1], launch[0], launch[1]})
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    // argv 和字符串全部在 fork 之前准备，子进程只调用 async-signal-safe 的系统入口。
    std::vector<char*> argv;
    argv.reserve(session.arguments.size() + 1);
    for (auto& argument : session.arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto pid = ::fork();
    if (pid < 0) {
        fail("cannot fork shell process");
        return;
    }
    if (pid == 0) {
        if (::setpgid(0, 0) != 0 || ::chdir(session.options.workdir.c_str()) != 0 ||
            ::dup2(input[0], STDIN_FILENO) < 0 || ::dup2(output[1], STDOUT_FILENO) < 0 ||
            ::dup2(output[1], STDERR_FILENO) < 0) {
            const int cause = errno;
            const auto ignored = ::write(launch[1], &cause, sizeof(cause));
            (void)ignored;
            ::_exit(127);
        }
        for (int fd : {output[0], output[1], input[0], input[1], launch[0]}) ::close(fd);
        ::execv(argv.front(), argv.data());
        const int cause = errno;
        const auto ignored = ::write(launch[1], &cause, sizeof(cause));
        (void)ignored;
        ::_exit(127);
    }
    child = pid;
    ::setpgid(pid, pid);
    ::close(output[1]);
    output[1] = -1;
    ::close(input[0]);
    input[0] = -1;
    ::close(launch[1]);
    launch[1] = -1;
    int cause = 0;
    ssize_t launched;
    do {
        launched = ::read(launch[0], &cause, sizeof(cause));
    } while (launched < 0 && errno == EINTR);
    ::close(launch[0]);
    launch[0] = -1;
    if (launched != 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        child = -1;
        fail("could not start shell in the requested working directory");
        return;
    }
    ::fcntl(output[0], F_SETFL, ::fcntl(output[0], F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(input[1], F_SETFL, ::fcntl(input[1], F_GETFL, 0) | O_NONBLOCK);
    // 仅本 worker 屏蔽 SIGPIPE；写入已退出进程不能终止宿主，也不修改全局 signal handler。
    sigset_t blocked, previous;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    startup(session);
    const auto deadline = Clock::now() + std::chrono::milliseconds(session.options.timeoutMs);
    char buffer[16384];
    std::string pending;
    int status = 0;
    bool reaped = false, stopping = false;
    Clock::time_point killedAt;
    MaiError error;
    const auto drain = [&] {
        bool received = false;
        for (int chunk = 0; chunk < 16; ++chunk) {
            const auto got = ::read(output[0], buffer, sizeof(buffer));
            if (got > 0) {
                appendOutput(session, buffer, static_cast<std::size_t>(got));
                received = true;
            } else
                break;
        }
        return received;
    };
    for (;;) {
        const bool received = drain();
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            reaped = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            error = {MaiErrorCode::Internal, "cannot wait for shell process"};
            break;
        }
        if (!stopping && mustStop(session, deadline)) {
            stopping = true;
            killedAt = Clock::now();
            ::kill(-pid, SIGTERM);
        }
        if (stopping && Clock::now() - killedAt >= std::chrono::milliseconds(250))
            ::kill(-pid, SIGKILL);
        if (!stopping) {
            if (pending.empty()) pending = takeInput(session, 4096);
            if (!pending.empty()) {
                const auto written = ::write(input[1], pending.data(), pending.size());
                if (written > 0)
                    pending.erase(0, static_cast<std::size_t>(written));
                else if (written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    pending.clear();
                    std::lock_guard<std::mutex> lock(session.mutex);
                    session.stdinClosed = true;
                }
            }
        }
        if (received)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    ::kill(-pid, SIGKILL);
    if (!reaped)
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    child = -1;
    drain();
    closePair(output);
    closePair(input);
    // 不恢复可能带 pending SIGPIPE 的 worker mask；线程在此返回，不影响调用线程。
    const auto exit =
        !stopping && !error
            ? std::optional<int>(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status))
            : std::nullopt;
    finish(session, exit, error);
}
#endif

void joinSession(const std::shared_ptr<ShellSession>& session) {
    std::lock_guard<std::mutex> lock(session->joining);
    if (session->worker.joinable()) session->worker.join();
}
}  // namespace

struct MaiShellSessionManager::Impl {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<ShellSession>> sessions;
};

MaiShellSessionManager::MaiShellSessionManager() : mImpl(std::make_unique<Impl>()) {}
MaiShellSessionManager::~MaiShellSessionManager() {
    stopAll();
}

MaiResult<std::string> MaiShellSessionManager::start(const std::string& owner,
                                                     const MaiShellSessionOptions& options,
                                                     const std::function<bool()>& isCanceled) {
    maiAssertBlockingAllowed("MaiShellSessionManager::start");
    if (!maiIsProcessExecutionSupported())
        return {MaiErrorCode::NotSupported, "process execution is unavailable on this platform"};
    if (owner.empty() || owner.size() > 256 || owner.find('\0') != std::string::npos ||
        options.command.empty() || options.command.size() > 64 * 1024 ||
        options.command.find('\0') != std::string::npos || options.workdir.empty() ||
        options.workdir.find('\0') != std::string::npos || options.timeoutMs < 1000 ||
        options.timeoutMs > 600000 ||
        !MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(options.workdir)))
        return {MaiErrorCode::InvalidInput, "invalid shell command, working directory or timeout"};
    if (isCanceled && isCanceled())
        return {MaiErrorCode::Canceled, "shell was canceled before launch"};
    const auto environment = maiResolveShellEnvironment(options.shell);
    if (!environment) return environment.error();
    auto session = std::make_shared<ShellSession>();
    session->id = MaiIdGenerator::generate("shell_");
    session->owner = owner;
    session->options = options;
    session->environment = environment.value();
    const auto prepared = maiShellStartupCommand(environment.value(), options.command,
                                                 options.login, options.workdir);
    session->arguments = maiShellCommandArguments(environment.value(), prepared, options.login);
    if (session->arguments.empty())
        return {MaiErrorCode::InvalidInput, "cannot derive shell arguments"};
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        std::size_t live = 0, retained = 0;
        for (const auto& pair : mImpl->sessions) {
            std::lock_guard<std::mutex> state(pair.second->mutex);
            if (!pair.second->done && pair.second->owner == owner) ++live;
            if (!pair.second->done || pair.second->cursor < pair.second->total) ++retained;
        }
        if (live >= 12 || retained >= 24)
            return {MaiErrorCode::Busy,
                    "shell session capacity reached; poll or terminate existing processes"};
        if (mImpl->sessions.size() >= 128) {
            for (auto item = mImpl->sessions.begin(); item != mImpl->sessions.end();) {
                const auto candidate = item->second;
                std::lock_guard<std::mutex> state(candidate->mutex);
                if (candidate->done && candidate->cursor >= candidate->total)
                    item = mImpl->sessions.erase(item);
                else
                    ++item;
            }
        }
        mImpl->sessions.emplace(session->id, session);
        session->worker = std::thread([session] {
            MaiThread::setCurrentName("MaiShellSession");
            try {
                runNative(*session);
            } catch (...) {
                // 原生循环的资源由正常路径清理；异常仅用于防止线程未捕获异常结束宿主。
                const MaiError error{MaiErrorCode::Internal, "shell worker failed"};
                startup(*session, error);
                finish(*session, {}, error);
            }
        });
    }
    std::unique_lock<std::mutex> state(session->mutex);
    while (!session->startupChecked) {
        if (isCanceled && isCanceled()) session->cancel.store(true);
        session->changed.wait_for(state, std::chrono::milliseconds(kPollMs));
    }
    if (session->error) return session->error;
    return session->id;
}

MaiResult<MaiShellSessionSnapshot> MaiShellSessionManager::interact(
    const std::string& owner, const std::string& sessionId, const std::string& chars, int yieldMs,
    bool terminate, const std::function<bool()>& isCanceled, bool waitForCompletion) {
    maiAssertBlockingAllowed("MaiShellSessionManager::interact");
    const int maximumYieldMs = !waitForCompletion && chars.empty() && !terminate ? 300000 : 30000;
    if (yieldMs < 0 || yieldMs > maximumYieldMs || chars.size() > kStdinBytes ||
        (terminate && !chars.empty()))
        return {MaiErrorCode::InvalidInput, "invalid stdin, terminate combination or yield time"};
    const bool canceledBeforeInput = isCanceled && isCanceled();
    auto deadline = Clock::now() + std::chrono::milliseconds(yieldMs);
    std::shared_ptr<ShellSession> session;
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        const auto found = mImpl->sessions.find(sessionId);
        if (found == mImpl->sessions.end() || found->second->owner != owner)
            return {MaiErrorCode::NotFound, "shell process is unavailable for this Agent session"};
        session = found->second;
    }
    if (canceledBeforeInput) session->cancel.store(true);
    std::unique_lock<std::mutex> call(session->calls, std::defer_lock);
    while (!call.try_lock()) {
        if (isCanceled && isCanceled()) {
            session->cancel.store(true);
            return {MaiErrorCode::Canceled,
                    "shell interaction canceled while waiting; no stdin was queued"};
        }
        if (Clock::now() >= deadline)
            return {MaiErrorCode::Busy,
                    "another interaction is still consuming this process output; poll again"};
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    std::unique_lock<std::mutex> state(session->mutex);
    if (!chars.empty() && !canceledBeforeInput) {
        if (session->done || session->stdinClosed)
            return {MaiErrorCode::InvalidInput, "shell stdin is closed"};
        if (chars.size() > kStdinBytes - session->input.size())
            return {MaiErrorCode::Busy, "shell stdin queue is full"};
        session->input += chars;
    }
    if (terminate && !session->done) {
        session->terminated = true;
        session->stop.store(true);
    }
    bool cancellationSeen = false;
    for (;;) {
        if (isCanceled && isCanceled()) {
            session->cancel.store(true);
            if (!cancellationSeen) {
                cancellationSeen = true;
                deadline = Clock::now() + std::chrono::seconds(3);
            }
        }
        if (session->done || Clock::now() >= deadline ||
            (!waitForCompletion && !terminate && !cancellationSeen &&
             session->total > session->cursor))
            break;
        session->changed.wait_for(state, std::chrono::milliseconds(kPollMs));
    }
    MaiShellSessionSnapshot result;
    result.sessionId = session->id;
    result.started = session->started;
    result.completed = session->done;
    result.timedOut = session->timedOut;
    result.canceled = session->cancel.load();
    result.terminated = session->terminated;
    result.exitCode = session->exitCode;
    result.error = session->error;
    result.totalOutputBytes = session->total;
    result.captureTruncated = session->total > kMaiToolOutputMaxBytes;
    result.ringDiscardedBytes = session->total > kRingBytes ? session->total - kRingBytes : 0;
    if (session->cursor < session->capture.size()) {
        result.output = session->capture.substr(static_cast<std::size_t>(session->cursor));
        session->cursor += result.output.size();
        if (session->total > session->capture.size() && !session->ring.empty()) {
            // 原始快照仍是 16MiB 前缀；显示尾部只借用 ring，不推进读取位置或伪造连续记录。
            const auto beginning = std::max<std::uint64_t>(
                session->capture.size(),
                session->total > kRingBytes ? session->total - kRingBytes : 0);
            const auto size = static_cast<std::size_t>(session->total - beginning);
            const auto position = static_cast<std::size_t>(beginning % kRingBytes);
            const auto first = std::min(size, kRingBytes - position);
            result.previewTail.assign(session->ring.data() + position, first);
            if (first < size) result.previewTail.append(session->ring.data(), size - first);
            result.previewTailOffset = beginning;
        }
    } else if (session->cursor < session->total) {
        const auto beginning = session->total > kRingBytes ? session->total - kRingBytes : 0;
        if (session->cursor < beginning) {
            session->discarded += beginning - session->cursor;
            session->cursor = beginning;
        }
        const auto size = static_cast<std::size_t>(session->total - session->cursor);
        const auto position = static_cast<std::size_t>(session->cursor % kRingBytes);
        const auto first = std::min(size, kRingBytes - position);
        result.output.assign(session->ring.data() + position, first);
        if (first < size) result.output.append(session->ring.data(), size - first);
        session->cursor = session->total;
    }
    result.discardedBytes = session->discarded;
    result.hasMoreOutput = session->cursor < session->total;
    result.timeoutMs = session->options.timeoutMs;
    result.shellName = session->environment.name;
    result.shellPath = session->environment.path;
    result.login = session->options.login;
    if (session->done && !result.hasMoreOutput) {
        session->capture.clear();
        session->capture.shrink_to_fit();
        session->ring.clear();
        session->ring.shrink_to_fit();
    }
    return result;
}

void MaiShellSessionManager::stopOwner(const std::string& owner) {
    std::vector<std::shared_ptr<ShellSession>> stopping;
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        for (auto item = mImpl->sessions.begin(); item != mImpl->sessions.end();) {
            if (item->second->owner == owner) {
                item->second->stop.store(true);
                stopping.push_back(item->second);
                item = mImpl->sessions.erase(item);
            } else
                ++item;
        }
    }
    for (const auto& session : stopping) joinSession(session);
}
void MaiShellSessionManager::stopAll() {
    std::vector<std::shared_ptr<ShellSession>> stopping;
    {
        std::lock_guard<std::mutex> lock(mImpl->mutex);
        for (auto& pair : mImpl->sessions) {
            pair.second->stop.store(true);
            stopping.push_back(pair.second);
        }
        mImpl->sessions.clear();
    }
    for (const auto& session : stopping) joinSession(session);
}
