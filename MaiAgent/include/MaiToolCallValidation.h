// 文件作用：在任何工具启动前核对模型调用身份；具体参数语义仍由工具校验。
#pragma once
#include <vector>
#include "MaiError.h"
#include "MaiModelClient.h"
class MaiToolCallValidation {
public:
    // 一次成功响应的调用 ID 必须非空且唯一；失败不允许执行该批任何工具。
    // 仅在同一响应内判重，供应商可在后续响应重新使用 ID，不能跨轮误杀合法调用。
    static MaiError validateBatch(const std::vector<MaiToolInvocation>& calls);
    // 工具入参必须是 JSON object；不会把 malformed/scalar 转成空对象后执行。
    static MaiError validateArguments(const MaiToolInvocation& call);
};
