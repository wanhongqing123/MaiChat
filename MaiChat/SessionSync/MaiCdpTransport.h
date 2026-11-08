// 文件作用：为桌面浏览器提供不依赖 Qt 的本机 CDP 请求通道。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"

// 一个实例同时处理一个命令；不自动重连或重放命令，避免重复点击或输入。
// 网络回调只访问内部共享状态，不暴露 WebSocket++ 或 Asio 类型给调用方。
class MaiCdpTransport final {
public:
    struct Options {
        // 正数毫秒：从连接启动到 WebSocket 握手成功，以及发送命令后等待匹配回复的期限。
        int connectTimeoutMs = 6000;
        int responseTimeoutMs = 6000;
        // 完整收发消息的字节上限，包含分片组装后的消息；默认覆盖 20 MiB PNG 的 Base64。
        std::size_t maximumMessageBytes = 32u * 1024u * 1024u;
    };

    MaiCdpTransport();
    explicit MaiCdpTransport(Options options);
    ~MaiCdpTransport();

    MaiCdpTransport(const MaiCdpTransport&) = delete;
    MaiCdpTransport& operator=(const MaiCdpTransport&) = delete;

    // 阻塞调用，放在业务工作线程；只接受数字 loopback 地址的 ws://，不经过代理或账号重连。
    // parametersJson 必须是 JSON 对象。内部生成请求 ID，忽略通知和其他 ID，
    // 成功返回匹配回复的 result JSON 对象；CDP error、无效回复或消息超限返回 Protocol。
    // URL、方法、参数或 Options 无效返回 InvalidInput；并发 call 返回 Busy。
    // 连接/回复超时、对端断开返回 Network，取消或 stop 返回 Canceled。
    // isCanceled 可为空；非空时必须线程安全且不抛异常，call 返回后不会再调用它。
    // 每次调用结束前收回网络线程；同一实例可以随后执行下一条新命令。
    MaiResult<std::string> call(const std::string& socketUrl, const std::string& method,
                                const std::string& parametersJson = "{}", std::function<bool()> isCanceled = {});

    // 可从另一线程取消当前 call，唤醒其等待并 join 网络线程；重复调用无副作用。
    // stop 返回后没有悬空网络回调。析构前，宿主仍须等待调用 call 的业务线程返回。
    void stop();

private:
    struct Runtime;
    std::shared_ptr<Runtime> mRuntime;
};
