# Windows Shell 后台进程：Codex 固定源码核对

日期：2026-11-26。平台：Windows x64。MaiChat 工作区基线提交：
`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，相关实现含已有未提交改动。
本记录核对后台生命周期，不代表桌面安装包完成最终验收。

## 可复现操作与预期结果

使用官方 Playwright Skill 指示的 CLI，在一次 `exec_command` 中执行命名会话的
`open`，等待该命令返回；再通过另一次独立 `exec_command` 对同名会话执行 `snapshot`。
预期前台 CLI 正常退出后，CLI 启动的后台守护进程继续服务，第二个命令能获取页面。
此处不能用一个长驻父脚本包住所有操作，否则无法覆盖命令返回时的资源释放边界。

修复前，实际 DesktopAgentController 的 workspace-implicit 与 user-explicit 两个场景
都读到了 Skill 正文，`open` 返回 0 和守护进程 PID，随后独立 `snapshot` 返回 1，
提示同名浏览器未打开。原始证据位于：

- `.cache/playwright-skill-20261126/host-chrome/workspace-implicit/evidence.json`
- `.cache/playwright-skill-20261126/host-chrome/user-explicit/evidence.json`

可运行 `python .cache/playwright-skill-20261126/read-evidence.py host-chrome` 查看经过筛选的
命令与结果，避免把完整环境或无关数据写入日志。根因是 Windows Shell worker 在父进程
正常退出后也无条件执行 `TerminateJobObject`，随后关闭 kill-on-close Job；守护进程因此
无法跨独立命令存活。

## 固定 Codex 源码事实

本机只读参考仓库为 `E:/OpenSource/codex`，远端为 `https://github.com/openai/codex.git`。
使用 `git show` 读取固定提交 `3d86e43b075adeba2193ec5c22ac92f55afd82e6`，没有 checkout、
修改参考仓库或以浮动 main 代替固定版本。以下行号均属于这个提交。

| 位置 | 核对到的行为 |
| --- | --- |
| [core/src/unified_exec/process.rs:367–418](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/core/src/unified_exec/process.rs#L367-L418) | 本地进程使用 `BoundedAfterExit`。收到根进程退出通道后更新退出状态并通知输出等待；不会把所有后台后代的退出作为前台完成条件。 |
| [core/src/unified_exec/process_manager.rs:1570–1663](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/core/src/unified_exec/process_manager.rs#L1570-L1663) | 输出缓冲暂时为空且已收到退出信号时，等待关闭的额外窗口最多 50ms，同时受本次调用期限限制。这个窗口用于输出采集，不是后台进程寿命。 |
| [core/src/unified_exec/process_manager.rs:620–626、1179–1204](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/core/src/unified_exec/process_manager.rs#L1179-L1204) | 活跃前台进程保留在管理表；普通本地路径在退出后可移除记录。`WaitForOutputClosure` 路径另有等待未关闭、未消费输出的保留规则，不能混为所有后代存活规则。 |
| [utils/pty/src/pipe.rs:149–207](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/utils/pty/src/pipe.rs#L149-L207) | 管道子进程使用 `ReapOnly`。Windows 创建 Job 后尝试把已启动的根进程加入 Job；源码承认启动与分配间存在竞态，分配失败可退回仅根进程终止。 |
| [utils/pty/src/pipe.rs:246–275](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/utils/pty/src/pipe.rs#L246-L275) | 根进程 `wait()` 成功时，先调用 Job 的 `preserve_descendants()`，再设置退出标志、退出码并发送退出通知。这里不是在等待全部后代退出。 |
| [utils/pty/src/win/job.rs:71–94、240–277](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/utils/pty/src/win/job.rs#L240-L277) | 默认 Job 含 kill-on-close 和允许显式 breakaway 的限制标志。`preserve_descendants()` 把限制改成仅保留 breakaway 标志，并置位保存状态；后续 `terminate()` 发现该状态会直接成功返回，不再执行 `TerminateJobObject`。保存与终止由同一锁排序。 |
| [utils/pty/src/windows_tests.rs:196–267](https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/codex-rs/utils/pty/src/windows_tests.rs#L196-L267) | 现有真实 Windows 测试让子进程以 DETACHED_PROCESS、CREATE_NEW_PROCESS_GROUP 和空标准输入输出启动。根进程退出 0 后释放 session，随后断言后台子进程写入存活标记；分别覆盖 pipe 与 ConPTY。 |

不能从表面上的 Drop 调用推出“Codex 在普通命令完成后杀掉全部 Windows 后代”。真实调用链是：
`UnifiedExecProcess::drop()` → `terminate()` → 本地 `ProcessHandle::terminate()` →
`request_terminate()` → `PipeChildTerminator::kill()` → `JobObject::terminate()`。
最后一步受上述保存状态控制，已经保存后代时不再终止 Job。
对应源码为 `core/src/unified_exec/process.rs:244–254、713–716`、
`utils/pty/src/process.rs:227–280`、`utils/pty/src/pipe.rs:69–80` 与
`utils/pty/src/win/job.rs:260–277`。I/O reader 的中止与进程后代的终止也是不同动作。

ConPTY 的 `WinChild` 在确认根进程退出后同样调用保存后代逻辑，见
`utils/pty/src/win/mod.rs:68–91、127–145`。这是 Codex 的范围；本次 MaiChat 修复仅改变
Windows 正常完成路径，随后按上游双模式回归扩展到管道和 ConPTY；两者保留既有显式终止契约。

固定版本还包含 exec-server/远端输出路径。`process_manager.rs:1366–1399` 按环境选择
路径，远端使用 `WaitForOutputClosure`；`process.rs:488–505、659–664` 在前台退出、初次
采集结束后设置 5 秒输出排空宽限，并在最终排空后请求终止。这说明其传输层有独立的
输出关闭策略，不意味着 MaiChat 需要引入该架构。

## 运行观察与证据边界

本次主执行代理在当前 Codex 工具环境中，使用独立调用运行官方 CLI 的命名会话
`codex-parity-chromium`：headless Chromium `open` 返回后，下一次独立 `snapshot` 成功。
该观察支持“实际工具环境允许后台浏览器跨命令存活”，与固定源码的保存后代测试一致。
它不能单独证明当前桌面工具后端恰好运行上述提交，也不能识别私有桌面 helper 的
具体实现。公开源码核对与当前工具实测属于两组证据。

此前 Chrome/Edge 配置相关报错与 MaiChat 的“open 成功后同名会话丢失”是不同现象，
不能合并成相同失败或相同根因。这里也没有运行 Codex 的 Rust 测试套件；表内测试仅指
已逐行读取的上游测试源码。

## MaiChat 的适配与已验证范围

采用前台完成与后台生命周期分离的行为，但保留 MaiChat 的进程所有权约束：

- Windows 管道/ConPTY 根进程正常退出后发布原始前台退出码，快照 `completed` 成立；Job 后代尚存时
  `backgroundRunning` 单独说明后台仍在运行。普通等待不受守护进程寿命阻塞。
- 原 worker 继续持有原 Job，沿用有界输出捕获并检查停止、取消与用户显式硬期限。
  没有放开 breakaway、关闭 kill-on-close，或借由弃置 Job 放任孤儿进程。
- 内部 `done` 只在整组资源回收结束后成立；仍有后台成员的组继续占用每 owner 12、
  每 manager 24 的既有额度，不能被已完成记录回收逻辑提前删除。
- 显式终止、取消、stopOwner、stopAll 与 manager 析构仍请求整树清理并同步回收。
  普通后台自然退出后释放资源；默认期限仍为 0。POSIX 的既有策略保持原状；Windows ConPTY 与管道使用相同的后台保留契约。
- 产品继续直接调用进程内普通核心和事件总线。没有引入 app-server、独立 Agent 服务、
  进程间 RPC、MCP 或新第三方库；工具入口复用同一核心。

真实 Windows 自身进程 fixture 在修复前得到 RED：父进程返回后，四个场景的后台对象
均已无法打开。修复后，同一回归及扩展生命周期检查通过，证据是
`.cache/playwright-skill-20261126/shell-background/red/receipt.json` 与
`shell-background/green-final/receipt.json`。最终独立构建的 `MaiExecSessionTests.exe`
测试退出码为 0，耗时 11.34 秒，SHA-256 为
`477ad07d0e967a9ceae61e504717ed21802f31f045a4878995abe0f0907b7a1f`。
构建只链接复制到私有目录的依赖，未锁定正常桌面构建的库。

测试覆盖正常完成后跨独立命令存活、后台标志、原始前台状态、终止、取消、所有者清理、
析构、运行中根进程取消后的整树清理、显式期限、后台自然退出、12/24 容量以及容量释放。
这证明核心层回归；正常桌面全量 ABI 重建、安装产物与进程核对、原 Skill 操作回归的
最终结果应以主验收记录为准，本记录不提前宣称这些步骤完成。
