// 文件作用：独占租用 libcurl easy handle，避免凭据、回调及连接状态跨请求串用。
#include "MaiModelConnectionPool.h"
#include <mutex>
#include <utility>
#include <vector>
#include <curl/curl.h>

struct MaiModelConnectionPool::State {
    std::mutex mutex;
    std::vector<CURL*> idle;
    ~State() {
        for (auto* handle : idle) curl_easy_cleanup(handle);
    }
};
MaiModelConnectionPool::MaiModelConnectionPool() : mState(std::make_shared<State>()) {}
MaiModelConnectionPool::~MaiModelConnectionPool() = default;
MaiModelConnectionPool::Lease MaiModelConnectionPool::acquire() {
    CURL* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mState->mutex);
        if (!mState->idle.empty()) {
            handle = mState->idle.back();
            mState->idle.pop_back();
        }
    }
    return Lease(mState, handle ? handle : curl_easy_init());
}
MaiModelConnectionPool::Lease::Lease() = default;
MaiModelConnectionPool::Lease::Lease(std::shared_ptr<State> state, void* handle)
    : mState(std::move(state)), mHandle(handle) {}
MaiModelConnectionPool::Lease::~Lease() {
    finish(false);
}
MaiModelConnectionPool::Lease::Lease(Lease&& other) noexcept
    : mState(std::move(other.mState)), mHandle(std::exchange(other.mHandle, nullptr)) {}
MaiModelConnectionPool::Lease& MaiModelConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        finish(false);
        mState = std::move(other.mState);
        mHandle = std::exchange(other.mHandle, nullptr);
    }
    return *this;
}
void* MaiModelConnectionPool::Lease::nativeHandle() const {
    return mHandle;
}
void MaiModelConnectionPool::Lease::finish(bool reusable) {
    auto* handle = static_cast<CURL*>(std::exchange(mHandle, nullptr));
    if (!handle) return;
    if (reusable && mState) {
        // curl_easy_reset 清除借用的 POSTFIELDS/回调/代理/头选项，保留连接与 TLS 缓存。
        // 来源：third_party/curl/docs/libcurl/curl_easy_reset.md。
        curl_easy_reset(handle);
        std::lock_guard<std::mutex> lock(mState->mutex);
        if (mState->idle.size() < 4) {
            mState->idle.push_back(handle);
            return;
        }
    }
    curl_easy_cleanup(handle);
}
