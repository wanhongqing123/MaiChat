// 文件作用：读取系统/进程代理配置，供普通网络实现选择 curl 或 WebSocket 的实际连接策略。
#pragma once

#include <string>

enum class MaiSystemProxyMode { Direct, Http, Socks5, Unsupported, Unavailable };

struct MaiSystemProxyResult {
    MaiSystemProxyMode mode = MaiSystemProxyMode::Unavailable;
    // Http 使用 http://（或明确配置的 https://），Socks5 使用 socks5h://；包含凭据时不得记录。
    // Direct 必须为空。Unsupported/Unavailable 不得被调用方静默当成 Direct。
    std::string url;
};

// 在工作线程查询，可能执行系统 PAC/WPAD 解析。只读配置，不改系统或进程代理。
// HTTP/HTTPS 和 WS/WSS 目标共用同一套语义；WebSocket 按对应 HTTP(S) URL 查询。
// 进程代理环境及 no_proxy 优先，之后查询原生系统设置；本地 fixture 可用本进程变量隔离。
// 不返回用户身份/原始 PAC 内容；错误用 Unavailable/Unsupported 表达。空或无效目标被拒绝。
MaiSystemProxyResult maiSystemProxyForUrl(const std::string& targetUrl);
