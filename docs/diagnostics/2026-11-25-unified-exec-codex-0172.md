# 命令工具对齐 Codex 0.172.0

## 复现操作与预期

2026-11-25，工作区基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，保留原有 Responses、上下文及联合存储 WIP。
对照官方 `rust-v0.172.0`（peeled `3d86e43b075adeba2193ec5c22ac92f55afd82e6`），在实际工具目录、执行、续读和桌面详情中完成以下操作：

1. 用 `exec_command({cmd:...})` 启动命令；快速退出返回结果，长命令返回数字 `session_id`。
2. 用 `write_stdin` 续读同一会话。等待时间只限制本次调用，不应在两分钟后自动杀掉可续接进程。
3. 默认管道模式的标准输入为 EOF；普通交互须在启动时选择 `tty:true`，Ctrl-C 作用于原进程。
4. 审批后在后续普通 Agent 轮次继续原进程，不能跨所有者操作，也不能靠重跑命令补读输出。
5. 模型结果统一为紧凑 JSON；不再为每次执行重复附加长 `shell_result` 说明。

官方版本仍有 `tools/handlers/shell_spec.rs`，其中定义的是 `exec_command` / `write_stdin`。
本次按注册协议与实际执行行为对齐，没有以文件是否名为 shell 代替检查。

## 实现

- 正式入口改为 `MaiExecCommandTool`，删除旧工具头/实现路径及旧工厂声明。
  注册器只提供 `exec_command`、`write_stdin`，不保留旧工具名或旧参数的执行别名。
- 参数改为 `cmd`、正 i32 数字会话 ID；可续接工具移除 `timeout_ms` 和 `terminate`。
  普通 `MaiShellSessionManager` API 保留显式有界执行、所有者关闭与整树终止能力。
- 默认进程期限为 0（无限）；初次等待默认 10000ms，Windows 下界 10000ms，POSIX 下界 250ms，上界 30000ms。
  普通输入等待 250..30000ms，空轮询 5000..300000ms；默认值与显式值都做同一约束。
- ID 随机分配且在 manager 生命周期内不复用，避免重建后固定从 1 开始的确定性错投。
  跨重建仍是概率性防碰撞；ID 不是认证凭据，权限仍来自既有所有者和审批边界。
- Windows 使用已有 ConPTY，POSIX 使用已有原生 PTY。管道启动时关闭 stdin；精确单字节 Ctrl-C 在管道模式请求终止，PTY 模式交给终端解释。
- 模型仅收到本次新消费的 `output`、`chunk_id`、`wall_time_seconds`，以及存在时的 `session_id` / 已观察到的 `exit_code` / 截断前近似 `original_token_count`。
  默认输出预算 10000 近似 token，策略上限 1 MiB，保留头尾。零预算及无符号大值有明确验证和截断行为。
- 原始字节继续通过原有存储及 `tool_output_read` 补读；原文、隐藏执行事实与模型预览分开。
  `output_id`、捕获丢失、取消、终止等 MaiChat 扩展仅在必要时进入结果，不把未知退出码编成 0。
- 桌面工具目录、入门示例、分组与跨轮续接路径同步使用新入口；旧名称仅用于历史分类。

本次保留 MaiChat 的审批、工作区和进程数量/原文留存策略，不声称提供 Codex 的操作系统沙箱或远端 executor。
没有增加 exec-server、MCP、RPC 或第三方依赖。

## 发现的边界及回归

1. 新协议最小用例先出现 4 个失败，证实旧名仍注册、新名不存在，改动后通过。
2. manager 初始 17 个失败覆盖旧字符串 ID、默认期限、管道输入等；修复后通过。
3. 审查发现空轮询缺省值未真正应用 5000ms 下界，以及有新字节就提前返回。
   无输出和先到一块输出的自有子进程用例确认等待/聚合窗口；聚合用例 RED 1 → GREEN。
4. 无符号整数上界不能先按有符号比较；实际大值用例 RED 3 → GREEN，拒绝负数、浮点和错误类型。
5. manager 重建后固定从 1 开始会把旧句柄指向新进程；相同所有者的重建用例及 300000ms 空等待验证 RED 3 → GREEN。
6. Windows Job 终止是异步的。保留精确 Job 成员进程句柄，核对归属后等待进程对象信号，再发布完成。
   原先两个严格清理失败保留，原断言未放宽；修复后全 PTY 通过，24 次定点清理压力执行通过。
   该等待覆盖终止前已捕获成员，未增加新建后代的 Job 通知账本，不能宣称覆盖所有对抗性 fork 竞态或总清理固定两秒。
7. ConPTY 会在 `native-pid` 标记内部插入 OSC/CSI。保存原始字节后，仅对测试的 PID 投影去除这些控制序列。
   实际 JSON 与原文捕获没有改写；同一桌面续接操作 RED → GREEN。

Windows Job 异步终止依据：
[TerminateJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject)、
[TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)。

## 验证结果与证据

证据目录：`.cache/unified-exec-20261125/`。

- 正常 App、CLI、桌面三个定点目标构建/链接成功，PE 验证为 3 个二进制、114 个导入及 Qt 插件。
- 正常 Core 静态库验证：UnifiedExec 协议、ExecSession、Core、ModelClient、ToolLoop、TurnRunner、Runtime、Store、InputQueue、ToolOutputRead、RequestBodyRecovery、Permission、ShellTool、ShellPty 共 14 套通过。
  其中旧补读夹具首次因旧工具名失败，迁移调用名/参数后同一补读及 SQLite 重开断言通过。
- 原有命令 47、PTY 6、Runtime 6 个用例保留；命令全套正常库运行 85.681s，PTY 12.878s。
- 正常桌面目录测试通过，详情 31 项通过，所选 Panel 用例 7 passed / 0 failed / 0 skipped。
  覆盖真实自有子进程、授权、跨轮续接、输入写标记、Ctrl-C、进程退出、会话切换与分组计数。
- 正常 CLI 端到端全部检查通过，含工具、审批、持久化与硬退出恢复。
- Android NDK 28 / API26 / aarch64：manager、新工具实现及新协议测试语法检查通过。
- 正常打包后，staging 与实际 ZIP 解压目录分别以裸 PATH 执行 `--smoke`，退出码均为 0。
  App SHA256：`3f8e32468e80a10be474f85211710b3e7020e9f53380f27c4903304d77557f8d`。
  ZIP 261414375 bytes，SHA256：`8175600cbfcc3ce54fd79fff37123792fc4f75b68154492f929dab6c5bf91604`。
- 关键来源清单见 `package-validation.json`；各 Suite 回执分别在 `normal-core/`、`final-core/`、`tests-own/`、`manager-own/`、`ui/`，原始失败与后续通过分开保留。

## 交付边界

正常构建沿既有独立 `.cache/r311/src` / `build-msvc`，关键命令源码与当前工作区逐字核对。
候选包名称包含独立副本的旧 Git 基线；本次工作区尚未提交，不能把该名称当成新的发布提交。
未登录真实账号、未调用真实供应商、未迁移用户数据库，也未创建 Release、安装或替换现装。
现装仍为 `D:\Program Files\MaiChat\maichat.exe`、PID 1320（2026-11-24 00:27 启动）。
Windows 原生终端运行已验证；macOS/Linux/Android 原生执行与移动端界面未在本轮实机验证，不能用 Android 语法检查代替。
联合会话存储迁移、小工具集/发现和用量显示仍按各自任务跟进，不将本次命令协议迁移宣称为全部 Codex 对齐。
