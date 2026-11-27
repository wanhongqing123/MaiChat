// 文件作用：把模型的 tool_search 参数适配到普通工具目录入口。
#pragma once

#include <memory>

#include "MaiModelToolCatalog.h"
#include "MaiTool.h"

// 共享目录核心必须已经绑定同一 Agent 的存储与公开注册表；空目录明确拒绝执行。
// 工具只发现元数据/选择声明，不执行所选工具，不改变权限或工作目录边界。
std::unique_ptr<MaiTool> makeMaiToolSearchTool(std::shared_ptr<MaiModelToolCatalog> catalog);
