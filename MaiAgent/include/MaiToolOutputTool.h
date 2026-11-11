// 文件作用：声明会话工具输出的模型补读入口。
#pragma once

#include <memory>

#include "MaiTool.h"

// 只读当前会话中已保留的输出，不执行原工具，也不接受目标会话或文件路径。
// 使用 MaiToolContext::toolOutputStore 的普通分页 API；宿主未接入时返回 NotSupported。
// 无工作目录和审批要求，所有权、预算和 UTF-8 边界由同一存储核心核验。
std::unique_ptr<MaiTool> makeMaiToolOutputReadTool();
