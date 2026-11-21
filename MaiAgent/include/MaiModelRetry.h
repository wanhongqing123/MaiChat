// 文件作用：共享模型请求的恢复等待与服务方等待提示解析。
#pragma once

#include <atomic>
#include <functional>
#include <string_view>

#include "MaiError.h"

enum class MaiRetryWaitResult { Ready, Canceled, InputPending };
struct MaiModelConfig;

// 仅供传输适配器和 TurnRunner 共用，不能据此重新执行已有副作用的工具。
// 所有函数在调用线程运行；wait 阻塞工作线程，不持会话锁，每 25ms 检查取消/新输入。
class MaiModelRetry {
public:
    // 适配器只提供可验证的状态/传输事实；已知鉴权、付费拒绝和硬额度错误不恢复。
    static bool isTransient(long status, bool transportRetryable, std::string_view body);
    // 仅恢复协议中明确的临时错误码，不从人类文案推断；未知/额度/内容错误保持失败。
    // 仅认供应商机器错误码，不将普通 HTTP 400 或人类文案当作容量证据。
    static bool isContextLengthCode(std::string_view code);
    static bool isContextLengthError(std::string_view body);
    static bool isTransientProviderCode(std::string_view code);
    static MaiError advise(MaiError error, const MaiModelConfig& config, bool sawOutput,
                           long retryAfterMs);
    // retryIndex 从 0 开始；指数退避封顶 60s，但不得早于有效服务方等待时间。
    static long delayMilliseconds(const MaiRetryAdvice& advice, int retryIndex);
    // 支持非负数字秒和标准日期；无效/过大值返回 -1，不从正文猜测等待。
    static long parseDelayMilliseconds(std::string_view value);
    static MaiRetryWaitResult wait(long milliseconds, const std::atomic<bool>& cancel,
                                   const std::function<bool()>& hasPendingInput = {});
};
