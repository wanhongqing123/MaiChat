// 文件作用：把进程内脚本运行器接到无外部进程能力的移动宿主。
#pragma once

#include <memory>

#include "MaiTool.h"

// 工具名为 exec_command；只由移动宿主注册，不能与系统进程版同时注册。
// 每次调用以会话目录和 App 容器创建独立脚本环境，不继承系统环境，不提供 PTY/session_id。
// 工具不持有本次 context；修改与重定向按宿主现有审批策略执行。
std::unique_ptr<MaiTool> makeMaiInProcessShellTool();
