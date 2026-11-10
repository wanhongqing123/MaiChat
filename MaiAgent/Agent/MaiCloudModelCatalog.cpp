// 文件作用：共享已核官方模型能力，避免桌面与移动宿主各自猜测型号前缀。
#include "MaiCloudModelCatalog.h"

std::optional<MaiCloudModelInfo> maiCloudModelInfo(const std::string& model) {
    // 2026-11-10 官方模型页和 /models 文档；主聊天协议是产品选择，不是 Key 格式推断。
    // GLM Coding Plan（含过期订阅）当前只支持 Chat Completions。
    // https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3
    // https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash
    // https://api-docs.deepseek.com/api/list-models/
    if (model == "glm-5.3" || model == "glm-5.3-flash")
        return MaiCloudModelInfo{model,
                                 "glm",
                                 MaiWireApi::ChatCompletions,
                                 model == "glm-5.3-flash",
                                 true,
                                 {1048576, 131072}};
    if (model == "deepseek-flash" || model == "deepseek-v4-pro")
        return MaiCloudModelInfo{
            model, "deepseek",       MaiWireApi::Responses, model == "deepseek-flash",
            true,  {1048576, 393216}};
    return std::nullopt;
}

bool maiIsValidReasoningEffort(const std::string& effort) {
    return effort.empty() || effort == "low" || effort == "high" || effort == "max";
}
