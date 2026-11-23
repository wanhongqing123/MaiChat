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
    int timeoutMs = 120000;
    // 空时按本机账户/平台发现；显式 shell 选择受支持类型，login 交给解释器原生启动处理。
    std::string shell;
    bool login = true;
};

// output 为本次新消费的原始字节快照，不假定 UTF-8。累计输出、丢弃和游标均由 manager 持有。
// completed 只有在进程树边界已清理且采集结束后才成立；非零退出不是启动失败。
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
    int timeoutMs = 120000;
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
};

// 线程安全的普通入口；shell/shell_input 复用同一个实例，无工具协议依赖。
// 每 owner 最多 12 个存活进程，manager 最多 24 个存活或尚有未读输出的进程。
// 每进程保留 16MiB 前缀和 1MiB 尾部环，stdin 队列最多 64KiB。已消费终态记录最多 128。
// start/interact 的取消回调仅在调用栈内借用；worker 不保留 caller context/cancel/store。
// stopOwner/stopAll 同步请求整树终止并回收线程，析构执行 stopAll；普通轮次结束不调用它们。
// 解释器选择和 argv 来自 MaiShellEnvironment；Windows 仍使用无窗口 kill-on-close Job，
// POSIX 使用独立进程组。原始环境由系统继承，login 处理 profile/PATH，不捕获环境快照。
class MaiShellSessionManager {
public:
    MaiShellSessionManager();
    ~MaiShellSessionManager();
    MaiShellSessionManager(const MaiShellSessionManager&) = delete;
    MaiShellSessionManager& operator=(const MaiShellSessionManager&) = delete;

    MaiResult<std::string> start(const std::string& owner, const MaiShellSessionOptions& options,
                                 const std::function<bool()>& isCanceled = {});
    // 初次等待/写入/终止 yieldMs 为 0..30000；纯空轮询可到 300000，不关闭 stdin。
    // terminate 与 chars 互斥。取消调用会请求终止对应进程；此回调不进入异步 worker。
    // waitForCompletion=true 用于初次 shell：即使已有输出也等到退出或 yield 截止。
    // 默认 false 的 shell_input 在有新输出时可提前返回，内部消费游标仍只前进一次。
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
