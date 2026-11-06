// 文件作用：共享四个人像工具的名称、说明、schema 与校验；平台只提供普通服务适配。
#pragma once

#include <functional>
#include <string>
#include "MaiTool.h"

// handler 在当前后台工具调用内同步执行，不得持有 context 或其中的取消指针。
// 工具仅校验模型参数；账号、工作区授权和完整文件发布由普通宿主服务负责。
using MaiPortraitHostHandler =
    std::function<MaiToolResult(const std::string&, const std::string&, const MaiToolContext&)>;
void registerMaiPortraitHostTools(MaiToolRegistry& tools, const MaiPortraitHostHandler& handler);
