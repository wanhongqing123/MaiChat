// 文件作用：模型 HTTP 适配器内部的连接句柄租约；不作为宿主网络 API。
#pragma once
#include <memory>

// 每个不可变供应商客户端独占一个池，凭据或代理配置变化必须换客户端。
// acquire 不阻塞：并发请求各借独立句柄；仅缓存最多 4 个空闲句柄。
// 失败/取消句柄关闭，完整传输句柄 reset 后可保留连接和 TLS 会话。
class MaiModelConnectionPool {
    struct State;

public:
    class Lease {
    public:
        Lease();
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        // 内部 opaque native handle；只有云模型适配器可转换，公开头不依赖 curl。
        void* nativeHandle() const;
        void finish(bool reusable);

    private:
        friend class MaiModelConnectionPool;
        Lease(std::shared_ptr<State> state, void* handle);
        std::shared_ptr<State> mState;
        void* mHandle = nullptr;
    };
    MaiModelConnectionPool();
    ~MaiModelConnectionPool();
    MaiModelConnectionPool(const MaiModelConnectionPool&) = delete;
    MaiModelConnectionPool& operator=(const MaiModelConnectionPool&) = delete;
    Lease acquire();

private:
    std::shared_ptr<State> mState;
};
