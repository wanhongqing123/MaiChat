#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>

class MaiAgent;
class MaiToolResult;
struct MaiToolContext;

// 共用 Router 的移动端普通适配器；只由既有 worker/pump 驱动，不创建执行线程。
// JSON 仅是进程内边界，公开头不依赖 JSON、Qt、JNI 或 Swift 类型。
class MaiMobileSessionIM final {
public:
    MaiMobileSessionIM(MaiAgent& agent, std::string currentWorkspace,
                       std::function<void(const std::string&)> requireConfiguredModel = {},
                       std::uint64_t readinessTimeoutMillis = 120000, std::uint64_t outputHandoffTimeoutMillis = 30000);
    ~MaiMobileSessionIM();
    MaiMobileSessionIM(const MaiMobileSessionIM&) = delete;
    MaiMobileSessionIM& operator=(const MaiMobileSessionIM&) = delete;

    std::string request(const std::string& requestJson);
    // 只访问 weak registry 与自有路由授权；不触碰 Agent、数据库、Host 回调或后端句柄。
    static std::string control(const std::string& requestJson);
    // 普通 preparation I/O worker 调用；不持有后端句柄，不调用 Agent 或 Host。
    // open/read/close 串行访问单个 owned reader；Main/control 仅设置原子取消标志。
    static std::string historySnapshot(const std::string& requestJson);
    MaiToolResult executeBindingTool(const std::string& arguments, const MaiToolContext& context);
    // 普通请求/工具改变绑定后发通知；回调只唤醒既有前端 worker，不可重入后端。
    // static control 永远不调用该 Host 通知。
    void setChangeHandler(std::function<void(const std::string&)> changed);
    void setInputValidator(std::function<void(const std::string&, bool)> validate);
    // 须在旧 Agent 仍存活、开始 join/destruct 之前调用；之后迟到控制请求全部拒绝。
    void invalidate();
    void revokeSession(const std::string& sessionId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
