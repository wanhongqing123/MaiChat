// 文件作用：集中保存已核官方的主聊天型号、媒体和推理能力，供各宿主共用。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "MaiModelClient.h"

struct MaiCloudModelInfo {
    std::string model;
    std::string provider;
    MaiWireApi wire = MaiWireApi::ChatCompletions;
    bool supportsImages = false;
    bool supportsReasoningEffort = false;
    MaiModelContextLimits contextLimits;
};

// 仅返回已核的公开主聊天型号，不猜别名或前缀。未知返回 nullopt；不取密钥/访问网络。
// 产品云端 GLM、DeepSeek、OpenRouter 使用 Responses；账号权限仍按实际供应商响应判断。
std::optional<MaiCloudModelInfo> maiCloudModelInfo(const std::string& model);

// 空串表示继承宿主/供应商默认；显式值仅 low/high/max。纯校验，不修改输入或会话。
bool maiIsValidReasoningEffort(const std::string& effort);

// 按型号返回当前产品开放的档位，按低到高排列；不包含表示继承的空串。
// 仅精确的 Fable 5.2 开放 medium/xhigh；其余型号（包括普通本地/自定义路由）保留原三档。
// 普通函数、无网络或凭据读取，不修改目录结构体或会话。
std::vector<std::string> maiReasoningEffortsForModel(const std::string& model);

// 空串表示继承；显式值必须属于该型号。未知型号保持原三档契约，不借用 Fable 能力。
bool maiIsValidReasoningEffortForModel(const std::string& model, const std::string& effort);

// 仅对已知跨型号继承值做兼容：medium/xhigh 在非 Fable 型号归一到 high。
// 空串及其他值原样返回；调用方仍须校验，不能把任意非法字符串吞成合法配置。
// 纯读取/计算，不写回会话；显式用户输入应先严格校验，不能借此接受不支持的档位。
std::string maiNormalizeReasoningEffortForModel(const std::string& model,
                                                const std::string& effort);
