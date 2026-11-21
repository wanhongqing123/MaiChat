// 文件作用：统一身份和参数容器校验，先校验整批身份再进入执行边界。
#include "MaiToolCallValidation.h"
#include <set>
#include <json.hpp>
MaiError MaiToolCallValidation::validateBatch(const std::vector<MaiToolInvocation>& calls) {
    std::set<std::string> ids;
    for (const auto& call : calls) {
        if (call.id.empty() || call.name.empty())
            return {MaiErrorCode::Protocol, "Model tool calls require a nonempty ID and name."};
        if (!ids.insert(call.id).second)
            return {MaiErrorCode::Protocol, "Model response contains duplicate tool call IDs."};
    }
    return {};
}
MaiError MaiToolCallValidation::validateArguments(const MaiToolInvocation& call) {
    if (!nlohmann::json::parse(call.arguments, nullptr, false).is_object())
        return {MaiErrorCode::InvalidInput, "Tool arguments must be a JSON object."};
    return {};
}
