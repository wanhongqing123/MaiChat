// 文件作用：集中保存已核官方的主聊天型号、媒体和推理能力，供各宿主共用。
#pragma once

#include <optional>
#include <string>

#include "MaiModelClient.h"

struct MaiCloudModelInfo {
    std::string model;
    std::string provider;
    MaiWireApi wire = MaiWireApi::ChatCompletions;
    bool supportsImages = false;
    bool supportsReasoningEffort = false;
    MaiModelContextLimits contextLimits;
};

// 仅返回已核的四个公开主聊天型号，不猜别名或前缀。未知返回 nullopt；不取密钥/访问网络。
// GLM 使用 Chat Completions 兼容 Coding Plan，DeepSeek 使用 Responses。
std::optional<MaiCloudModelInfo> maiCloudModelInfo(const std::string& model);

// 空串表示继承宿主/供应商默认；显式值仅 low/high/max。纯校验，不修改输入或会话。
bool maiIsValidReasoningEffort(const std::string& effort);
