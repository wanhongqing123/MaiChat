// 文件作用：系统代理查询的平台边界。进程环境/no_proxy 和 URL 归一化由共用层先处理。
#pragma once

#include "MaiSystemProxy.h"

// 仅接收已校验并归一化的 HTTP(S) URL。在工作线程调用；实现只读系统配置和原生 PAC。
// 不修改配置，不记录目标 URL/代理凭据；无法解析配置时明确返回 Unavailable/Unsupported。
MaiSystemProxyResult maiNativeSystemProxyForUrl(const std::string& targetUrl);
