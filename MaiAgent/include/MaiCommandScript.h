// 文件作用：定义不创建进程的受限命令脚本核心及宿主命令、文件访问边界。
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiFilePath.h"

struct MaiCommandScriptOptions {
    MaiFilePath workingDirectory;
    std::size_t maxScriptBytes = 64 * 1024;
    std::size_t maxCommands = 128;
    std::size_t maxOutputBytes = 1024 * 1024;
    std::function<bool()> isCanceled;
    std::size_t maxArguments = 256;
    std::size_t maxPipelineCommands = 16;
    // 变量及 argv 展开单独有界，避免通过重复自引用绕过脚本文本和输出预算。
    std::size_t maxExpandedBytes = 1024 * 1024;
};

struct MaiCommandInvocation {
    std::vector<std::string> arguments;
    std::string standardInput;
    MaiFilePath workingDirectory;
    // 只含脚本自己的变量，不导入或修改进程环境；前置赋值只覆盖本次命令。
    std::map<std::string, std::string> variables;
};

struct MaiCommandExecution {
    int exitCode = 0;
    std::string standardOutput;
    std::string standardError;
    bool outputTruncated = false;
};

// 所有回调同步执行，可阻塞当前工作线程；本核心不启动进程，也不直接访问文件系统。
// 相对路径先按当前工作目录拼接，绝对路径原样传递；这不是授权或真实路径校验。
// 宿主必须对 read/write/changeDirectory 及 execute 中的路径执行同一权限和链接边界校验。
// 未知命令由 execute 明确拒绝，不能回退到系统 shell。回调失败立即返回，不回滚已有操作。
struct MaiCommandScriptHost {
    std::function<MaiResult<MaiCommandExecution>(const MaiCommandInvocation&)> execute;
    std::function<MaiResult<std::string>(const MaiFilePath&)> read;
    std::function<MaiError(const MaiFilePath&, const std::string&, bool append)> write;
    std::function<MaiResult<MaiFilePath>(const MaiFilePath&)> changeDirectory;
};

// 完整词法/语法预解析成功后才调用宿主；语法错误或不支持的语法不会执行前缀命令。
// 支持单/双引号、反斜杠、注释、换行、;、&&、||、顺序有界管道、<、>、>>，以及
// $NAME/${NAME}/$?；未引用变量按 ASCII 空白分词，引用变量与显式空引号保留空参数。
// 不支持 glob、波浪号展开、命令替换、后台、子 shell、控制流、here-doc 或 fd 重定向。
// assignment-only 更新脚本变量，NAME=value command 只更新该命令的变量视图；不继承环境。
// pwd/cd 为内建命令，cd 必须经宿主确认目录；不修改进程工作目录。其余命令交给 execute。
// cd 要求一个目录参数；PWD/OLDPWD 由内建 cd 维护。多命令管道各段使用独立变量/目录副本，
// 管道内的赋值和 cd 不影响其他段或后续脚本，只有单命令管道才提交这些脚本内状态。
// 返回最后实际执行管道的末条命令状态，没有 pipefail。被短路跳过的管道没有副作用。
// maxCommands/maxArguments/maxPipelineCommands 限制静态结构及展开后的参数数量；
// maxOutputBytes 限制管道中间值、重定向完整内容及最终 stdout+stderr 的总字节数。
// 最终输出可截断并置 outputTruncated；中间值或重定向被截断则明确失败，不消费残缺输入。
// 最终截断不会在已确认的完整 UTF-8 字符中间切开，管道与文件传输保持原始字节。
// 参数/脚本预算无效返回 InvalidInput，缺回调返回 NotConfigured，取消返回 Canceled。
// 变量、目录与状态只在本次调用内存活；函数可重入，并发安全性由各自宿主回调保证。
MaiResult<MaiCommandExecution> maiRunCommandScript(const std::string& script,
                                                   const MaiCommandScriptOptions& options,
                                                   const MaiCommandScriptHost& host);
