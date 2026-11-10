// 文件作用：按显式型号目录选择模型客户端，供桌面和移动宿主共用。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiModelClient.h"

// 一条不可变路由。只接受 modelNames 中精确相等的型号，不按前缀猜供应商。
struct MaiModelRoute {
    std::vector<std::string> modelNames;
    MaiModelConfig config;
    // 每次 stream 在调用线程获取一次，覆盖 config.apiKey；空值明确 NotConfigured。
    // 可能跨会话并发调用，必须线程安全且活到客户端析构；不得发模型请求或记录密钥。
    // 未提供时使用 config.apiKey，便于普通固定凭据调用和独立 loopback 测试。
    std::function<std::string()> apiKeyProvider;
    // 本地模型等宿主实现可提供工厂，此时 config.wire 必须是实际协议（通常 Local）。
    // 每次 stream 创建一个客户端，返回空值视为未配置；不与远端路由互相兜底。
    // config.contextLimits 仍用于无副作用的容量查询，查询不会创建/加载本地模型。
    std::function<std::unique_ptr<MaiModelClient>()> clientFactory;
};

struct MaiRoutedModelConfig {
    std::vector<MaiModelRoute> routes;
};

// 工厂不访问网络或凭据。空目录、空/重复型号、Mixed 路由和没有工厂的 Local 路由
// 产生明确 InvalidInput 的拒绝客户端。其余错误只影响选中的路由；永不改用另一供应商。
// wireApi() 对含不同协议的目录返回 Mixed；wireApiForModel() 精确返回所选协议。
// 各次 stream 独立创建传输实例，sink/cancel 生命周期和 MaiModelClient 契约一致。
std::unique_ptr<MaiModelClient> makeMaiRoutedModelClient(MaiRoutedModelConfig config);
