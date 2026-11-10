// 文件作用：智谱原生 Web Search/Reader 普通入口和薄工具适配，不经主聊天模型或浏览器。
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "MaiWebRetrieval.h"

class MaiToolRegistry;

struct MaiGlmWebOptions {
    // 在调用线程按当前账号取一次；须线程安全且不把凭据写入日志/会话/模型输出。
    std::function<std::string()> apiKey;
    std::string caBundlePath;
    std::string caPem;
    std::string baseUrl = "https://open.bigmodel.cn/api/paas/v4";
    bool allowLoopbackTestServer = false;
    unsigned timeoutSeconds = 30;
};

struct MaiGlmWebSearchRequest {
    // 官方 GLM 上限为 70 Unicode 字符，不能沿用 DeepSeek 的 128 或静默截断。
    std::string query;
    unsigned count = 10;
    std::string searchEngine = "search_std";
    bool searchIntent = false;
    std::string domainFilter;
    std::string recencyFilter = "noLimit";
    std::string contentSize = "medium";
};

// 每次调用独立连接，可跨工作线程共用。与 DeepSeek 共用有界 TLS/取消/有限重试传输。
// 只连接官方 /web_search、/reader（显式测试模式例外）；不自动改用其他模型、供应商或 MCP。
// search 的可选来源/日期仅保留供应商原值；fetch 输出至多 maximumCharacters 个 Unicode 字符。
// 空/坏字段在取凭据前拒绝；取消、鉴权、限流、坏响应均返回稳定 error，绝不伪造成功。
class MaiGlmWebClient {
public:
    explicit MaiGlmWebClient(MaiGlmWebOptions options);
    MaiWebSearchResponse search(const MaiGlmWebSearchRequest& request,
                                const std::atomic<bool>* cancel = nullptr) const;
    MaiWebFetchResponse fetch(const std::string& url, unsigned maximumCharacters = 20000,
                              const std::atomic<bool>* cancel = nullptr) const;

private:
    MaiGlmWebOptions mOptions;
};

// 两个只读工具与普通入口共用同一实现；API 按次计费不等于购买或媒体任务提交。
void registerMaiGlmWebTools(MaiToolRegistry& registry, MaiGlmWebOptions options);
