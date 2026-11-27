// 文件作用：以原生管道或 PTY、进程树和有界捕获实现可续读命令，不改变阻塞 maiRunProcess。
#include "MaiShellSessionManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
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
#include <cstdlib>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
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
    // 前台完成可先返回；done 仍只在 worker 释放整组资源后成立，供额度和回收判断。
    bool foregroundCompleted = false;
    bool backgroundRunning = false;
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
    if (!session.foregroundCompleted) session.exitCode = exitCode;
    session.backgroundRunning = false;
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
    if (session.options.timeoutMs == 0) return false;
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

// Job 的 ActiveProcesses/PID 列表会先于进程对象变为 signaled 而清空；终止前必须保留句柄。
// 参考 Microsoft TerminateJobObject/TerminateProcess 契约及 Chromium base/process/kill_win.cc。
// 仅打开本 Job 查询出的进程，并再核对成员身份，不能把已复用 PID 的其他进程纳入清理。
bool captureJobProcesses(HANDLE job, std::vector<HANDLE>& processes) {
    std::set<ULONG_PTR> captured;
    std::size_t capacity = 16;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::vector<ULONG_PTR> storage(capacity + 2);
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
        const auto bytes = storage.size() * sizeof(ULONG_PTR);
        if (bytes > std::numeric_limits<DWORD>::max()) return false;
        const bool complete =
            ::QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                                        static_cast<DWORD>(bytes), nullptr) != FALSE;
        if (!complete && ::GetLastError() != ERROR_MORE_DATA) return false;
        for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
            const auto id = list->ProcessIdList[index];
            if (!captured.insert(id).second) continue;
            HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           static_cast<DWORD>(id));
            if (!process) {
                // 已彻底退出的对象可能不再可打开；其他失败则不能宣称已核实完整进程边界。
                if (::GetLastError() == ERROR_INVALID_PARAMETER) continue;
                return false;
            }
            BOOL belongs = FALSE;
            if (!::IsProcessInJob(process, job, &belongs)) {
                closeHandle(process);
                return false;
            }
            if (belongs)
                processes.push_back(process);
            else
                closeHandle(process);
        }
        if (complete) return true;
        capacity =
            std::max(capacity * 2, static_cast<std::size_t>(list->NumberOfAssignedProcesses));
    }
    return false;
}

using CreatePseudoConsoleFunction = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HANDLE*);
using ClosePseudoConsoleFunction = void(WINAPI*)(HANDLE);

// ClosePseudoConsole 可能等待终端输出被消费；不能在唯一读取线程上同步调用它。
// 关闭线程只拥有伪终端句柄，原 worker 同时继续有界采集，Job 仍负责整棵用户进程树。
void closePseudoConsole(HANDLE& console, ClosePseudoConsoleFunction closeConsole, HANDLE& output,
                        ShellSession& session) {
    if (!console || !closeConsole) return;
    const HANDLE closing = std::exchange(console, nullptr);
    std::atomic<bool> closed{false};
    std::thread closer;
    try {
        closer = std::thread([&] {
            closeConsole(closing);
            closed.store(true);
        });
    } catch (...) {
        // 无法创建关闭线程时先断开输出读端，使终端写入立即失败，不能留下同步死锁。
        closeHandle(output);
        closeConsole(closing);
        std::lock_guard<std::mutex> lock(session.mutex);
        if (!session.error)
            session.error = {MaiErrorCode::Internal, "cannot drain terminal during closure"};
        return;
    }
    while (!closed.load()) {
        DWORD available = 0, got = 0;
        char buffer[16384];
        if (output && ::PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) &&
            available &&
            ::ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) &&
            got) {
            try {
                appendOutput(session, buffer, got);
            } catch (...) {
                // 异常收尾仍须释放输出背压和关闭线程；外层保持已有 worker 错误事实。
                closeHandle(output);
                std::lock_guard<std::mutex> lock(session.mutex);
                if (!session.error)
                    session.error = {MaiErrorCode::Internal, "cannot capture terminal output"};
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    }
    closer.join();
}

void runNative(ShellSession& session) {
    HANDLE outputRead = nullptr, outputWrite = nullptr, inputWrite = INVALID_HANDLE_VALUE,
           inputRead = INVALID_HANDLE_VALUE, job = nullptr, console = nullptr;
    const auto kernel = ::GetModuleHandleW(L"kernel32.dll");
    const auto createConsole = reinterpret_cast<CreatePseudoConsoleFunction>(
        ::GetProcAddress(kernel, "CreatePseudoConsole"));
    const auto closeConsole = reinterpret_cast<ClosePseudoConsoleFunction>(
        ::GetProcAddress(kernel, "ClosePseudoConsole"));
    PROCESS_INFORMATION process{};
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto cleanup = [&] {
        // 清理异常或启动失败路径也先关 Job；ConPTY 的关闭不能替代进程树所有权。
        if (job) ::TerminateJobObject(job, 1);
        closePseudoConsole(console, closeConsole, outputRead, session);
        closeHandle(inputRead);
        closeHandle(inputWrite);
        closeHandle(outputRead);
        closeHandle(outputWrite);
        closeHandle(process.hThread);
        closeHandle(process.hProcess);
        closeHandle(job);
    };
    NativeCleanup nativeCleanup(cleanup);
    if (session.options.tty && (!createConsole || !closeConsole)) {
        const MaiError error{MaiErrorCode::NotSupported, "Windows ConPTY is unavailable"};
        startup(session, error);
        finish(session, {}, error);
        return;
    }
    const auto fail = [&](const char* message) {
        const MaiError error{MaiErrorCode::Internal, message};
        startup(session, error);
        finish(session, {}, error);
        cleanup();
    };
    // stdin 使用 overlapped 命名管道；不读 stdin 的子进程不能卡住取消或超时线程。
    // 公共十进制 ID 只在当前 manager 内唯一；命名管道另用不透明唯一值，避免实例间碰撞。
    const auto pipeName =
        MaiFilePath::fromUtf8("\\\\.\\pipe\\MaiShell-" + MaiIdGenerator::generate("pipe_")).value();
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
    if (session.options.tty &&
        FAILED(createConsole(COORD{120, 30}, inputRead, outputWrite, 0, &console))) {
        const MaiError error{MaiErrorCode::NotSupported, "cannot create Windows ConPTY"};
        startup(session, error);
        finish(session, {}, error);
        return;
    }
    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    // ConPTY 也显式选择无效标准句柄，由控制台绑定补齐；省略此标志会沿用宿主重定向句柄。
    // 参考 Codex 所用 portable-pty 的 Windows spawn：STARTF_USESTDHANDLES + INVALID_HANDLE_VALUE。
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    startupInfo.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    startupInfo.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    if (!session.options.tty) {
        startupInfo.StartupInfo.hStdInput = inputRead;
        startupInfo.StartupInfo.hStdOutput = outputWrite;
        startupInfo.StartupInfo.hStdError = outputWrite;
    }
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
                                     session.options.tty ? PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
                                                         : PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     session.options.tty ? console : static_cast<void*>(inherited),
                                     session.options.tty ? sizeof(console) : sizeof(inherited),
                                     nullptr, nullptr)) {
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
        ::CreateProcessW(application.c_str(), mutableLine.data(), nullptr, nullptr,
                         session.options.tty ? FALSE : TRUE,
                         (session.options.tty ? 0 : CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP) |
                             CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                         nullptr, directory.c_str(), &startupInfo.StartupInfo, &process);
    ::DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    closeHandle(inputRead);
    closeHandle(outputWrite);
    if (!session.options.tty) closeHandle(inputWrite);
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
        if (session.options.tty && !pending && writing.hEvent) {
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
    // 与 pinned Codex 的前台退出状态分离相同：命令结果不应由后台守护进程的寿命决定。
    // 管道与 ConPTY 正常退出均保留 Job 所有权；终端也留给同一 worker 排空，直到后代结束。
    // 不开放 breakaway，停止和期限仍进入原有整树终止及 ConPTY 关闭路径。
    if (!error && !stopped) {
        DWORD foregroundCode = 0;
        if (!::GetExitCodeProcess(process.hProcess, &foregroundCode) ||
            foregroundCode == STILL_ACTIVE) {
            error = {MaiErrorCode::Internal, "could not confirm shell termination"};
        } else {
            drain();
            bool published = false;
            for (;;) {
                if (mustStop(session, deadline)) {
                    stopped = true;
                    break;
                }
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                                 &accounting, sizeof(accounting), nullptr)) {
                    error = {MaiErrorCode::Internal, "cannot monitor shell background processes"};
                    break;
                }
                if (accounting.ActiveProcesses == 0) break;
                if (!published) {
                    std::lock_guard<std::mutex> lock(session.mutex);
                    session.foregroundCompleted = true;
                    session.backgroundRunning = true;
                    // 前台解释器已退出，不能把输入继续排入无人消费的 PTY 队列。
                    session.stdinClosed = true;
                    session.input.clear();
                    session.exitCode = static_cast<int>(foregroundCode);
                    session.changed.notify_all();
                    published = true;
                }
                // 后台若仍持有输出管道，也沿用有界捕获；不会创建额外线程或借用调用者回调。
                drain();
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
        }
    }
    // 先固定仍属于本 Job 的进程对象，再触发异步终止；事后枚举会漏掉尚未 signaled 的对象。
    std::vector<HANDLE> treeProcesses;
    NativeCleanup treeHandles([&] {
        for (auto& handle : treeProcesses) closeHandle(handle);
    });
    const bool treeCaptured = captureJobProcesses(job, treeProcesses);
    // 此处仅处理已无后台成员的正常退出，或取消、期限和失败路径的整树终止。
    ::TerminateJobObject(job, 1);
    ::WaitForSingleObject(process.hProcess, 2000);
    if (pending) {
        ::CancelIoEx(inputWrite, &writing);
        DWORD ignored = 0;
        ::GetOverlappedResult(inputWrite, &writing, &ignored, TRUE);
        pending = false;
    }
    closeHandle(writing.hEvent);
    closePseudoConsole(console, closeConsole, outputRead, session);
    // 前台可能早已完成；done 和终止调用仍必须等到整棵树已清空。
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
    // ConPTY 输出已经抽干，进程异步 I/O 可完成取消；对每个原进程对象等待，而非仅看 Job 计数。
    for (const auto handle : treeProcesses) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(treeDeadline - Clock::now())
                .count();
        if (::WaitForSingleObject(handle, remaining > 0 ? static_cast<DWORD>(remaining) : 0) !=
            WAIT_OBJECT_0)
            treeStopped = false;
    }
    if ((!treeCaptured || !treeStopped) && !error)
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
    if (session.options.tty) {
        // master/slave 分别打开；父端副本共享非阻塞 master，绝不把非阻塞状态施加到 slave。
        // 不依赖 libutil/openpty；fork 前完成路径和授权处理，子进程只做原生会话/句柄绑定。
        output[0] = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        char name[1024]{};
        bool named = false;
        if (output[0] >= 0 && ::grantpt(output[0]) == 0 && ::unlockpt(output[0]) == 0) {
#if defined(__APPLE__)
            named = ::ioctl(output[0], TIOCPTYGNAME, name) == 0;
#else
            named = ::ptsname_r(output[0], name, sizeof(name)) == 0;
#endif
        }
        if (named) {
            output[1] = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
            input[1] = ::dup(output[0]);
            // 与 ConPTY 一致的固定初始尺寸；普通输入沿用已有 API，本次不引入 resize 协议。
            struct winsize size{};
            size.ws_col = 120;
            size.ws_row = 30;
            named = ::ioctl(output[0], TIOCSWINSZ, &size) == 0;
        }
        if (!named || output[1] < 0 || input[1] < 0) {
            const MaiError error{MaiErrorCode::NotSupported, "native PTY is unavailable"};
            startup(session, error);
            finish(session, {}, error);
            return;
        }
    }
    if ((!session.options.tty && (::pipe(output) || ::pipe(input))) || ::pipe(launch)) {
        fail("cannot create shell pipes");
        return;
    }
#if defined(__APPLE__) && defined(F_SETNOSIGPIPE)
    // 独立宿主可能仍把 SIGPIPE 设为默认动作；Darwin 通过 fd 级抑制让闭管道只返回 EPIPE，
    // 不依赖 Qt 的进程级策略，也不修改宿主全局 signal handler。
    if (::fcntl(input[1], F_SETNOSIGPIPE, 1) < 0) {
        fail("cannot protect shell stdin from SIGPIPE");
        return;
    }
#endif
    for (int fd : {output[0], output[1], input[0], input[1], launch[0], launch[1]})
        if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
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
        // setsid 必须在尚非进程组首领时调用；PTY 分支禁止再执行父/子 setpgid。
        const bool groupFailed = session.options.tty
                                     ? (::setsid() < 0 || ::ioctl(output[1], TIOCSCTTY, 0) < 0)
                                     : ::setpgid(0, 0) != 0;
        if (groupFailed || ::chdir(session.options.workdir.c_str()) != 0 ||
            ::dup2(session.options.tty ? output[1] : input[0], STDIN_FILENO) < 0 ||
            ::dup2(output[1], STDOUT_FILENO) < 0 || ::dup2(output[1], STDERR_FILENO) < 0) {
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
    if (!session.options.tty) ::setpgid(pid, pid);
    ::close(output[1]);
    output[1] = -1;
    ::close(input[0]);
    input[0] = -1;
    if (!session.options.tty) {
        ::close(input[1]);
        input[1] = -1;
    }
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
    if (input[1] >= 0) ::fcntl(input[1], F_SETFL, ::fcntl(input[1], F_GETFL, 0) | O_NONBLOCK);
    // 仅本 worker 屏蔽 SIGPIPE；写入已退出进程不能终止宿主，也不修改全局 signal handler。
    sigset_t blocked, previous;
    // macOS SDK 将这两项定义为函数式宏，不能加 ::；POSIX 函数形式也兼容此写法。
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
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
        if (!stopping && session.options.tty) {
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
    // 参考 pinned Codex 的生产随机 ID，降低 manager 重建后旧显示句柄误命中新进程的概率。
    // 覆盖完整正 i32 空间；这不是鉴权秘密。所有已分配值保留到析构，回收记录不回收 ID。
    std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<std::int32_t> idDistribution{
        1, std::numeric_limits<std::int32_t>::max()};
    std::set<std::int32_t> allocatedIds;
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
        options.workdir.find('\0') != std::string::npos ||
        (options.timeoutMs != 0 && options.timeoutMs < 1000) || options.timeoutMs > 600000 ||
        !MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(options.workdir)))
        return {MaiErrorCode::InvalidInput, "invalid shell command, working directory or timeout"};
    if (isCanceled && isCanceled())
        return {MaiErrorCode::Canceled, "shell was canceled before launch"};
    const auto environment = maiResolveShellEnvironment(options.shell);
    if (!environment) return environment.error();
    auto session = std::make_shared<ShellSession>();
    session->owner = owner;
    session->options = options;
    session->stdinClosed = !options.tty;
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
        if (mImpl->allocatedIds.size() >=
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return {MaiErrorCode::Busy, "shell session identifier space is exhausted"};
        std::int32_t id = 0;
        do {
            id = mImpl->idDistribution(mImpl->random);
        } while (!mImpl->allocatedIds.insert(id).second);
        session->id = std::to_string(id);
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
    const int maximumYieldMs = chars.empty() && !terminate ? 300000 : 30000;
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
    const bool pipeInterrupt = !session->options.tty && chars == std::string(1, '\x03');
    if (!chars.empty() && !canceledBeforeInput) {
        if (pipeInterrupt) {
            // 与原生终端控制分开：关闭的管道没有可写句柄，只请求已有所有权边界内的终止。
            if (!session->done) {
                session->terminated = true;
                session->stop.store(true);
            }
        } else if (session->done || session->stdinClosed) {
            return {MaiErrorCode::InvalidInput, "shell stdin is closed"};
        } else if (chars.size() > kStdinBytes - session->input.size()) {
            return {MaiErrorCode::Busy, "shell stdin queue is full"};
        } else {
            session->input += chars;
        }
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
        const bool stopping = terminate || pipeInterrupt || cancellationSeen ||
                              session->stop.load() || session->cancel.load();
        if (session->done || (!stopping && session->foregroundCompleted) ||
            Clock::now() >= deadline ||
            (!waitForCompletion && !terminate && !pipeInterrupt && !cancellationSeen &&
             session->total > session->cursor))
            break;
        session->changed.wait_for(state, std::chrono::milliseconds(kPollMs));
    }
    MaiShellSessionSnapshot result;
    result.sessionId = session->id;
    result.started = session->started;
    result.completed = session->foregroundCompleted || session->done;
    result.backgroundRunning = session->backgroundRunning;
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
    result.tty = session->started && session->options.tty;
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
