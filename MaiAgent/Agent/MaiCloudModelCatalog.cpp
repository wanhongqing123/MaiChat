// 文件作用：共享已核官方模型能力，避免桌面与移动宿主各自猜测型号前缀。
#include "MaiCloudModelCatalog.h"

std::optional<MaiCloudModelInfo> maiCloudModelInfo(const std::string& model) {
    // 2026-11-10 官方模型页和 /models 文档；主聊天协议是产品选择，不是 Key 格式推断。
    // 产品云模型统一 Responses；具体账号可用性仍由供应商真实响应决定。
    // https://docs.bigmodel.cn/cn/guide/develop/responses/introduction
    // https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3
    // https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash
    // https://api-docs.deepseek.com/api/list-models/
    if (model == "glm-5.3" || model == "glm-5.3-flash")
        return MaiCloudModelInfo{
            model, "glm", MaiWireApi::Responses, model == "glm-5.3-flash", true, {1048576, 131072}};
    if (model == "deepseek-flash" || model == "deepseek-v4-pro")
        return MaiCloudModelInfo{
            model, "deepseek",       MaiWireApi::Responses, model == "deepseek-flash",
            true,  {1048576, 393216}};
    if (model == "anthropic/claude-fable-5.2")
        return MaiCloudModelInfo{model, "openrouter", MaiWireApi::Responses,
                                 true,  true,         {1000000, 128000}};
    return std::nullopt;
}

bool maiIsValidReasoningEffort(const std::string& effort) {
    return effort.empty() || effort == "low" || effort == "high" || effort == "max";
}

std::vector<std::string> maiReasoningEffortsForModel(const std::string& model) {
    // 官方模型记录 reasoning.supported_efforts；不可把某型号扩展成所有供应商的能力。
    // https://openrouter.ai/anthropic/claude-fable-5.2
    if (model == "anthropic/claude-fable-5.2") return {"low", "medium", "high", "xhigh", "max"};
    return {"low", "high", "max"};
}

bool maiIsValidReasoningEffortForModel(const std::string& model, const std::string& effort) {
    if (maiIsValidReasoningEffort(effort)) return true;
    return model == "anthropic/claude-fable-5.2" && (effort == "medium" || effort == "xhigh");
}

std::string maiNormalizeReasoningEffortForModel(const std::string& model,
                                                const std::string& effort) {
    if (model != "anthropic/claude-fable-5.2" && (effort == "medium" || effort == "xhigh"))
        return "high";
    return effort;
}
