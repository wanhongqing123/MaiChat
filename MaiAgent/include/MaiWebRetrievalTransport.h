// 文件作用：检索客户端内部传输接口；宿主使用对应普通 Search/Reader 客户端。
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

#include "MaiWebRetrieval.h"

struct MaiWebRetrievalTransportOptions {
    std::function<std::string()> apiKey;
    std::string caBundlePath;
    std::string caPem;
    std::string baseUrl;
    // 由普通供应商实现固定，绝不能从工具参数或远端会话输入取得。
    std::string officialBaseUrl;
    bool allowLoopbackTestServer = false;
    unsigned timeoutSeconds = 30;
};
struct MaiWebRetrievalTransportResponse {
    std::string body;
    MaiWebRetrievalError error;
    unsigned attempts = 0;
};

// 内部同步调用，每次连接独立；只允许固定官方 HTTPS，或显式开启的 loopback 测试。
// 不跟随重定向，TLS/主机名校验保持开启；解压后 2 MiB 接收上限、取消及最多两次有限重试。
// 回调在调用线程取一次凭据，必须线程安全；返回值不含凭据或目标网站实际状态。
// endpoint/body 来自供应商客户端的固定编码，不接受模型给出的 API host/headers。
MaiWebRetrievalTransportResponse maiWebRetrievalPost(
    const MaiWebRetrievalTransportOptions& options, const std::string& endpoint,
    const std::string& body, const std::atomic<bool>* cancel);

// 内部共用纯校验/UTF-8 截断；不做网络或文件访问。shorten 要求输入已通过 UTF-8 校验。
bool maiWebRetrievalValidText(const std::string& value, std::size_t maximum, bool required);
std::string maiWebRetrievalShorten(const std::string& text, std::size_t characters, bool& truncated);
bool maiWebRetrievalHttpUrl(const std::string& value);
