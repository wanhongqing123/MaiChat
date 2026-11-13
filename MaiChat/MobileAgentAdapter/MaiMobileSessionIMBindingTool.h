#pragma once

#include "MaiTool.h"

#include <functional>
#include <memory>
#include <string>

// 模型入口仅适配审批/参数；普通宿主与工具共用同一个 Session IM 实例。
using MaiMobileSessionIMBindingHandler =
    std::function<MaiToolResult(const std::string&, const MaiToolContext&)>;
std::unique_ptr<MaiTool> makeMaiMobileSessionIMBindingTool(MaiMobileSessionIMBindingHandler handler);
