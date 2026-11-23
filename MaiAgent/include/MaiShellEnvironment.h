// 文件作用：按本机账户和现有 PATH 选择 shell，并构造与启动模式一致的普通 argv。
#pragma once

#include <string>
#include <vector>

#include "MaiError.h"

enum class MaiShellType { Zsh, Bash, PowerShell, Sh, Cmd };

struct MaiShellEnvironment {
    MaiShellType type = MaiShellType::Sh;
    std::string name;
    std::string path;
};

// 查询不运行 shell、不读取 profile、不保存或返回环境变量的值。path 是已存在的原生绝对路径；
// Windows 规范化实际程序路径，POSIX 保留末级 sh 等入口名，防止改变 argv[0] 的运行模式。
// Windows 默认 pwsh -> powershell -> cmd；POSIX 首选 getpwuid_r 的账户 shell，
// macOS 再试 zsh/bash，其他 POSIX 再试 bash/zsh，最后 /bin/sh。不依赖 GUI 的 SHELL 值。
// requestedShell 可为受支持的名称或路径；与 Codex 一致，路径只选择解释器类型，再发现该类型。
// 只支持 zsh/bash/pwsh/powershell/sh/cmd；显式未知类型或不可用解释器返回错误，不静默换类型。
MaiResult<MaiShellEnvironment> maiResolveShellEnvironment(const std::string& requestedShell = {});

// 纯参数构造，不启动进程。POSIX login 使用 -lc，否则 -c；PowerShell login=false
// 加 -NoProfile，login=true 沿用原生 profile 启动，再 -Command。cmd 使用 /d /s /c，
// 禁用 AutoRun，并配合原生命令体外层引号；这是相对于 Codex 仅 /c 的明确差异。
// argv[0] 为 shell.path。
std::vector<std::string> maiShellCommandArguments(const MaiShellEnvironment& shell,
                                                  const std::string& command, bool login = true);

// 直接、每进程的启动准备：login Bash/Zsh 按 Codex startup.rs 加载当前账户 rc，
// 再恢复授权 workdir；Sh 按 frozen Codex 的非求值规则展开 ENV，只 source 可读且非目录的路径。
// 登录 PowerShell 用模块限定 Set-Location 恢复目录；
// Cmd 不改命令体；login=false 不加载 rc/profile。
// 不读取/捕获环境值，不写快照；用户命令另起一行，以保留 rc 中配置的 alias 解析规则。
// 与 Codex 一致，不额外配置 pipefail/-e 或覆盖用户最终状态；沿用解释器和 profile 的选项。
std::string maiShellStartupCommand(const MaiShellEnvironment& shell, const std::string& command,
                                   bool login, const std::string& workdir);
