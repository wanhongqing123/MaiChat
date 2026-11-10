// 文件作用：DeepSeek Search/Fetch 的普通客户端及共享 Agent 工具，独立于浏览器与 curl 命令。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MaiWebRetrieval.h"

class MaiToolRegistry;

// 回调在阻塞工作线程读取当前账号凭据；不得返回给模型、写日志或持久化。
// 生产请求只允许官方 HTTPS 地址。测试可显式开启仅 loopback 的 HTTP(S) 假服务。
struct MaiDeepSeekWebOptions {
    std::function<std::string()> apiKey;
    std::string caBundlePath;
    // 桌面资源系统可提供受信CA的PEM；不是用户令牌。非空时优先于文件路径。
    std::string caPem;
    std::string baseUrl = "https://api.deepseek.com";
    bool allowLoopbackTestServer = false;
    unsigned timeoutSeconds = 30;
};

// 每次调用独立连接，可跨工作线程共用。供应商从其服务端检索/抓取，客户端不抓搜索HTML。
// 不跟随API重定向，不转发凭据到其他host；网络取消、30秒默认超时、2MiB正文有界。
// 仅临时网络错误与408/429/500/502/503/504最多自动重试一次；其他错误立即返回。
// fetch最多输出maximumCharacters个Unicode字符，不在UTF-8字符中间截断。
// 抓取受目标站点策略限制；不会保证绕过验证码/登录墙。所有错误均为稳定类型和安全文案。
class MaiDeepSeekWebClient {
public:
    explicit MaiDeepSeekWebClient(MaiDeepSeekWebOptions options);
    MaiWebSearchResponse search(const MaiWebSearchRequest& request,
                                const std::atomic<bool>* cancel = nullptr) const;
    MaiWebFetchResponse fetch(const std::string& url, unsigned maximumCharacters = 20000,
                              const std::atomic<bool>* cancel = nullptr) const;

private:
    MaiDeepSeekWebOptions mOptions;
};

// web_search/web_fetch 薄包装普通客户端，沿用普通只读工具的权限策略。
// 供应商按次收取 API 使用费，但检索不属于需逐次确认的购买或媒体任务提交。
// 注册不会发网或取密钥。模型参数无API Key、host、代理设置；未知字段严格拒绝。
void registerMaiDeepSeekWebTools(MaiToolRegistry& registry, MaiDeepSeekWebOptions options);
