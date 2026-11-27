// 文件作用：在权限边界内执行 shell 命令并收集结果。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>

#include "MaiTool.h"

class MaiShellSessionManager;

// exec_command / write_stdin：启动和续读一个归当前会话所有的命令进程。
//
// **平台不一定支持**（iOS 的沙箱不允许 exec）。注册之前先问
// maiIsProcessExecutionSupported()——摆一个永远失败的工具比不摆更糟。
// 纯查询普通入口，与 exec_command 共用审批分类；未知/控制字符/不可信路径默认要求批准。
// 只做执行前分类，不是操作系统沙箱，也不授予访问文件或启动进程的权限。
bool maiExecCommandRequiresApproval(const std::string& command);
std::unique_ptr<MaiTool> makeMaiExecCommandTool();
// 工厂本身不启动进程；仅 execute 阻塞等待并启动/访问 native manager。
// 成对使用时必须传入同一共享 manager；所有者身份来自每次调用的 MaiToolContext。
// 注册器只在支持本机进程的平台上成对注册，关闭会话/宿主时同步清理其进程树。
// manager 线程安全，工厂返回对象由调用方持有；空 manager 创建独立实例，不继承其他资源。
std::unique_ptr<MaiTool> makeMaiExecCommandTool(std::shared_ptr<MaiShellSessionManager> manager);
std::unique_ptr<MaiTool> makeMaiWriteStdinTool(std::shared_ptr<MaiShellSessionManager> manager);
void registerMaiExecTools(MaiToolRegistry& registry);
