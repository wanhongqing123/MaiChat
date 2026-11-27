# Unified Exec 0.172 对齐实施计划

**Goal:** 只保留 exec_command / write_stdin 一套正式命令工具，按已核实 Codex 0.172.0 的参数、结果及交互语义工作。

**Architecture:** 复用进程内 MaiShellSessionManager 和既有审批/原文补读存储；不加入 exec-server、MCP、新依赖或第二条旧工具路径。普通 API 与模型工具共用进程实现。历史记录中的旧名称可以显示，但不能注册为可执行别名。

**Reference:** 官方 rust-v0.172.0，peeled commit `3d86e43b075adeba2193ec5c22ac92f55afd82e6`。
参照 `tools/handlers/shell_spec.rs`、`unified_exec/{exec_command,write_stdin,preparation}.rs` 和 `unified_exec/process_manager.rs`。

## 已确认的行为

- exec_command 使用 cmd；write_stdin 使用整数 session_id。默认 tty=false，只有 tty=true 接受普通交互输入；空输入只续读，非 PTY 可接受 Ctrl-C 中断。
- 可续接进程不受旧的默认 120 秒硬超时限制；yield 只是本次等待。显式普通 API 的有界执行仍可供非模型调用。
- 初次等待默认 10000ms，Windows 下界 10000ms，POSIX 下界 250ms，上界 30000ms；非空输入默认 250ms，上界 30000ms；空轮询有效范围 5000..300000ms。
- 统一输出为 JSON 对象：chunk_id、wall_time_seconds、output，运行时给 session_id，结束时给 exit_code；截断时给 original_token_count。不再向每次工具正文添加重复的长状态说明。
- 保留真实 execution 状态和独立原文补读入口。发生捕获丢失/错误必须明确指出，不能为了缩短结果声称完整。
- 现有 MaiChat 审批是产品权限边界；不声称已提供 Codex OS 沙箱，不把不支持的提权参数当成已生效。

## 顺序与文件

- [x] 先备份本轮涉及文件到 .cache/unified-exec-20261125/before，记录原有 WIP；不覆盖其他任务。
- [x] 编写新协议失败用例：工具名/cmd、数字 ID、一致返回对象、拒绝旧名称/参数、输出不带 shell_result。
- [x] 修改 AgentTools/MaiShellSessionManager.cpp、include/MaiShellSessionManager.h：无默认超时、数字进程标识、PTY/管道 stdin 与 Ctrl-C、保持所有者和同步清理。
- [x] 将 MaiShellTool 入口替换为 MaiExecCommandTool，工厂为 makeMaiExecCommandTool / makeMaiWriteStdinTool，注册 registerMaiExecTools。移除旧工厂和注册。
- [x] 将模型可见结果统一为有界 JSON 对象，原始输出与执行事实继续走已有普通 API；区分本次等待与命令总运行时间。
- [x] 迁移既有命令测试，保留审批、路径、捕获、取消、超时普通 API、进程树和跨会话隔离覆盖；增加新契约回归。
- [x] 更新 StarterCatalog、AgentToolGroupWidget 及相应界面测试，只显示和调用新入口；保留历史展示能力。
- [x] 正常 App/Core/Qt 构建与定点回归；Windows 真 ConPTY 用自有子进程测试，不碰用户剪贴板、账号库或真实模型。
- [x] 检查 POSIX/Android 可编译边界；不能实际运行的平台明确标记。打包 staging/ZIP 裸 PATH smoke，独立审查，记录结果。

实现和验证由当前会话直接推进；本任务不自行安装、发布或操作其他机器。
