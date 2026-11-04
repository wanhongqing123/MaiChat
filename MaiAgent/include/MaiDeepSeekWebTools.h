// 文件作用：DeepSeek Search/Fetch 的普通客户端及共享 Agent 工具，独立于浏览器与 curl 命令。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// code 为空表示无错误；httpStatus=0 表示尚未收到 HTTP 响应。
// HTTP 状态属于供应商 API，不能据此虚构目标网站的实际状态。404 的多种来源无法进一步判定。
// attempts 是实际已发请求数；最多两次。retryable 表示错误类别可重试，不代表客户端会无限重试。
struct MaiWebRetrievalError {
    std::string code;
    std::string reason;
    long httpStatus = 0;
    unsigned attempts = 0;
    bool retryable = false;
};

// query/oriQuestion 最多128个Unicode字符；query原样发送，保留site/lang/日期等操作符。
// localization 只接受用户已提供的 IPv4/IPv6，不是语言或地区代码；未知时留空，不能猜造或查 IP。
// 不自动读取或上送设备 IP；needContent 默认关闭以控制正文大小。
struct MaiWebSearchRequest {
    std::string query;
    unsigned count = 10;
    std::string oriQuestion;
    std::string localization;
    bool needContent = false;
};

// 元数据仅取供应商原值，缺失日期/网站为null，authority=-1代表未知，不推测发布时间或权威度。
struct MaiWebSearchItem {
    std::string title;
    std::string url;
    std::string snippet;
    std::string summary;
    std::string content;
    std::optional<std::string> site;
    std::optional<std::string> date;
    int authority = -1;
    double rerankScore = 0;
};

struct MaiWebSearchResponse {
    std::vector<MaiWebSearchItem> results;
    MaiWebRetrievalError error;
    unsigned attempts = 0;
};

struct MaiWebFetchResponse {
    std::string title;
    std::string content;
    bool truncated = false;
    MaiWebRetrievalError error;
    unsigned attempts = 0;
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
