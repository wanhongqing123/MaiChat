// 文件作用：管理跨普通 Agent 轮次存活的单次 shell 子进程。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "MaiError.h"

struct MaiShellSessionOptions {
    std::string command;
    std::string workdir;
    // 0 不设进程期限；普通调用可显式指定 1000..600000 ms 的硬期限。
    int timeoutMs = 0;
    // 空时按本机账户/平台发现；显式 shell 选择受支持类型，login 交给解释器原生启动处理。
    std::string shell;
    bool login = true;
    // 显式请求本机伪终端；默认仍为原始管道。平台不支持时返回 NotSupported，不回退。
    bool tty = false;
};

// output 为本次新消费的原始字节快照，不假定 UTF-8。累计输出、丢弃和游标均由 manager 持有。
// completed 表示前台命令退出并采集其剩余输出，或终止清理结束；非零退出不是启动失败。
// Windows 的后台后代可在 completed 后继续运行，由 backgroundRunning 单独说明。
struct MaiShellSessionSnapshot {
    std::string sessionId;
    std::string output;
    bool started = false;
    bool completed = false;
    bool timedOut = false;
    bool canceled = false;
    bool terminated = false;
    bool captureTruncated = false;
    bool hasMoreOutput = false;
    int timeoutMs = 0;
    std::uint64_t totalOutputBytes = 0;
    std::uint64_t discardedBytes = 0;
    std::uint64_t ringDiscardedBytes = 0;
    std::optional<int> exitCode;
    MaiError error;
    std::string shellName;
    std::string shellPath;
    bool login = true;
    // 仅供显示的当前尾部，不消耗原始输出/内部游标；仅前缀捕获返回时可带上。
    // offset 是尾部在实际已观察字节流中的绝对位置，不能把前缀和尾部当成连续原始记录。
    std::string previewTail;
    std::uint64_t previewTailOffset = 0;
    // 实际已启动的传输模式；终端输出保留原始 VT/回显字节，不假装成独立 stdout/stderr。
    bool tty = false;
    // 仅 Windows：前台已退出，但仍有本 Job 拥有的后台进程；其他平台恒为 false。
    // 它们继续占用存活额度，保留原 ID 的普通入口终止能力，直到自然退出、期限或所有者清理。
    bool backgroundRunning = false;
};

// 线程安全的普通入口；exec_command/write_stdin 复用同一个实例，无工具协议依赖。
// 每 owner 最多 12 个存活进程组，manager 最多 24 个存活或尚有未读输出的进程组。
// Windows 前台正常退出后，后台成员仍由原 Job 和有界 worker 持有，不回收或脱离所有权。
// ConPTY 也持续有界采集到后台结束；前台完成后关闭逻辑 stdin，普通输入不再排队。
// 每进程保留 16MiB 前缀和 1MiB 尾部环，PTY stdin 队列最多 64KiB。已消费终态记录最多 128。
// 返回的 ID 是生产随机分配的规范正十进制 i32 字符串，降低重建后的旧句柄碰撞概率。
// 同一 manager 生命周期内不复用（含已回收记录）；它不是系统 PID 或鉴权秘密。
// start/interact 的取消回调仅在调用栈内借用；worker 不保留 caller context/cancel/store。
// stopOwner/stopAll 同步请求整树终止并回收线程，析构执行 stopAll；普通轮次结束不调用它们。
// 解释器选择和 argv 来自 MaiShellEnvironment；Windows 仍使用无窗口 kill-on-close Job，
// POSIX 使用独立进程组，tty 模式使用独立会话及控制终端。Windows tty 使用动态 ConPTY。
// 原始环境由系统继承，login 处理 profile/PATH，不捕获环境快照。
class MaiShellSessionManager {
public:
    MaiShellSessionManager();
    ~MaiShellSessionManager();
    MaiShellSessionManager(const MaiShellSessionManager&) = delete;
    MaiShellSessionManager& operator=(const MaiShellSessionManager&) = delete;

    MaiResult<std::string> start(const std::string& owner, const MaiShellSessionOptions& options,
                                 const std::function<bool()>& isCanceled = {});
    // 空输入且不终止时 yieldMs 为 0..300000；写入/终止为 0..30000，不受等待模式影响。
    // 非 PTY 在启动时关闭 stdin，普通写入被拒绝；精确单字节 Ctrl-C 请求终止对应进程。
    // PTY 保留交互输入，含交由终端解释的 Ctrl-C；空轮询不改变其 stdin 状态。
    // terminate 与 chars 互斥。取消调用会请求终止对应进程；此回调不进入异步 worker。
    // 默认 waitForCompletion=false 为普通调用在有输出时提前返回；true 聚合到前台退出或 yield 截止。
    // 显式终止和取消仍等待整个拥有的进程组清理；后台进程不会延迟普通前台完成结果。
    // exec_command/write_stdin 适配器可使用 true；空输入仍允许最长 300000 ms 的完整等待窗口。
    // 无论等待模式如何，内部消费游标仍只前进一次。
    MaiResult<MaiShellSessionSnapshot> interact(const std::string& owner,
                                                const std::string& sessionId,
                                                const std::string& chars, int yieldMs,
                                                bool terminate = false,
                                                const std::function<bool()>& isCanceled = {},
                                                bool waitForCompletion = false);
    void stopOwner(const std::string& owner);
    void stopAll();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
