// 文件作用：模型和普通下载共用的逐请求网络租约，不改变进程默认网络。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 宿主只发布系统允许的备选网络；0 保留系统默认路由。generation 必须在默认网络、
// 能力、代理或链路变化时更新。实例只能在一个请求线程使用；publish/setBackend 线程安全。
// prepare 不发送业务请求，不重试 POST；DNS 和新 socket 始终使用同一捕获的网络。
// nativeHandle 是 curl easy handle；返回值为 CURLcode，公开头不暴露 curl 类型。
class MaiCurlNetworkRoute {
public:
    struct Backend {
        std::function<bool(uint64_t, const std::string&, std::vector<std::string>&)> resolve;
        std::function<bool(uint64_t, intptr_t)> bindSocket;
    };
    MaiCurlNetworkRoute();
    ~MaiCurlNetworkRoute();
    MaiCurlNetworkRoute(const MaiCurlNetworkRoute&) = delete;
    MaiCurlNetworkRoute& operator=(const MaiCurlNetworkRoute&) = delete;
    int prepare(void* nativeHandle, const std::string& url, bool hasProxy, long timeoutMs,
                const std::function<bool()>& isCanceled = {});
    uint64_t networkHandle() const;
    uint64_t capturedGeneration() const;
    // 只计数量，供诊断验证备选 DNS/socket 回调确实执行；不包含地址、域名或业务数据。
    uint64_t dnsCalls() const;
    uint64_t socketCalls() const;
    bool hasChanged() const;
    static uint64_t generation();
    static void publish(uint64_t alternateNetwork, uint64_t generation);
    // Android 在启动时安装 NDK 后端；测试可安装可控后端。替换不会影响已有租约。
    static void setBackend(Backend backend);

private:
    struct State;
    std::unique_ptr<State> mState;
};
