// 文件作用：实现兼容 Chat Completions 的模型客户端。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiModelClient.h"

// 按配置造一个模型客户端。
//
// Chat Completions 与 Responses 各有独立实现；此工厂按 MaiModelConfig::wire 分发。
// GLM 等兼容前者，DeepSeek 等兼容后者；调用方只使用中立的 MaiModelClient 接口。
// Local 不含本地文件配置，因此返回明确 NotConfigured 的客户端且绝不发网络；
// 本地推理应使用 MaiLlamaModelClient.h 中的独立配置和工厂。
std::unique_ptr<MaiModelClient> makeMaiModelClient(MaiModelConfig config);
