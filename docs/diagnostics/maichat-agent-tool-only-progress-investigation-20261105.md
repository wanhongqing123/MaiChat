# Windows Agent 连续工具调用与 Codex 调用限制对照

## 范围与现场操作

- 调查时间：2026-11-05 20:57（Asia/Shanghai）。
- 源码：`2c96439d97433b57ab715aa3562e4db8df4908dd`。
- 前轮用户提供两张 Windows 正常 App 截图，同一轮工具计数由 33 增至 51。
  用户后续明确：截图来自另一台运行 **MaiChat 2.2.47** 的 Windows，
  不是本次调查机器；且**现在中间文字能够显示**，本轮只要求比较工具调用限制。
- 当前范围收敛为：核对 Codex 客户端是否存在按连续工具次数或时间强制输出正文的机制，
  以及 MaiAgent 的循环和重复结果限制。不继续把截图当成已确认的正文丢失故障。
- 预期：持续任务在工具前和阶段完成后提供真实、可见的中间说明，继续执行原任务；
  不把思考草稿当作正文，不伪造完成情况。
- 本次只读源码、进程身份与 App 日志中的非敏感元信息，没有停止任务、替换 App、
  构建、发出模型请求、读取或输出用户聊天正文与凭据。

## 本次调查机器已确认的运行身份（不是截图机器）

本次调查机器当前唯一正常 `maichat.exe` 的身份为：

| 项目 | 观察值 |
| --- | --- |
| PID | `584` |
| 进程路径 | `D:/Program Files/MaiChat/maichat.exe` |
| 进程启动时间 | `2026-11-05T08:46:01.6112721+08:00` |
| 同 PID 的 App 启动日志版本 | `2.2.29` |
| 同启动块的 Qt 版本 | `5.15.2` |
| 磁盘 exe 字节数 | `90,355,712` |
| 磁盘 exe 修改时间 | `2026-10-27 11:28:32` |
| 磁盘 exe SHA-256 | `3a1c25a73e7510ae1e35366854d9e673d03754748bc6a09277145c816ca37a70` |

版本、路径与 PID 来自正常 App 日志 `maichat-20261105.log` 中同一启动块；
仅提取这些字段，没有复制日志中的其他内容。Windows 文件版本资源未提供版本，
因此版本结论来自启动日志，不能把 exe 的修改时间当作源码提交证明。

当前 exe 中没有以下三段新共享进度指令的 ASCII 标记：

- `# Progress updates`
- `Before starting tool work, send a brief visible assistant message`
- `keep no more than 60 seconds between updates`

共享进度指令及宿主接入在 `27cf9755`（2026-11-05 15:55）才提交。
所查本机进程早于该提交启动，且仍运行旧安装路径中的 `2.2.29`。
用户已确认截图来自另一台运行 `2.2.47` 的 Windows。
**以上元信息只证明本次调查机器的进程身份，不能用来解释另一台机器的截图。**
没有采集截图机器的进程路径、PID 或产物哈希；本轮调用限制对照不依赖该映射。
未从这些元信息推断旧安装包的准确 Git 提交。

## 当前源码的行为边界

### 连续仅工具响应不会触发中间正文检查

`MaiAgent/Agent/MaiTurnRunner.cpp`：

- `340–343`：同步收完一次模型流，记录本次是否有可见正文，再提交片段。
- `346–368`：只有 `calls.empty()` 时才检查工具后的最终答复缺失，并最多重试一次。
- `373–375`：只要仍有工具调用，就直接执行这批调用；本次没有可见正文不会阻止继续。
- `588`、`652`：顺序执行整批工具；工具调用之间没有新的模型进度生成步骤。
- `677–680`：每个工具返回后保存结果、发送工具状态事件；这不是可见的 assistant 正文。
- `378–391`：相同调用及结果重复三次，或达到迭代硬上限，才进入禁止工具的收尾请求。

`MaiAgent/include/MaiAgent.h:196` 的默认上限为 **64 次模型工具循环**，不是 64 个工具。
一轮模型响应可以包含多个工具，所以截图中的 33/51 计数不能直接换算为循环次数。
重复结果检测用于制止绕圈；它不是可见进度更新机制。

当前 Runner 没有记录“上次可见正文距今多久”，也没有在静默超过阈值后生成真实中间正文的执行路径。
`MaiAgentProgressInstructions.cpp` 中的 60 秒要求目前是模型指令，运行时没有对应计时或输出检查。
这意味着：模型持续返回合法工具调用而不返回正文时，代码仍会继续执行；
不能把共享提示词的存在写成“保证长任务必有中间正文”。
后续 Codex 源码核对确认，其工具数和 60 秒说明也不是宿主强制生成正文的阈值；
因此这里是待对齐和验证的行为边界，不能仅因没有强制阈值就断言实现偏离 Codex。

### 入口覆盖与现存会话

当前源码的主产品入口没有发现按会话年龄漏加共享指令：

- 桌面两种构造在 `AgentController.cpp:419`、`:478` 统一调用 `buildAgent`，
  `:401` 追加 `maiAgentProgressInstructions()`。
- `DesktopSessionSyncHost.cpp:238` 的远程请求回到同一 `controller.sendPrompt`。
- `MaiAgent.cpp:293` 将当前宿主 Options 传给 Runner，`MaiTurnRunner.cpp:411`
  每次构造模型请求时加入 `baseInstructions`；它不是仅在新建会话时保存的一次性值。
- iOS/Android、云端/本地模型统一经过 `MobileAgentAdapter/MaiMobileAgent.cpp:879`
  的宿主指令与 `:917` 的 Agent 构造。
- **CLI 仍有覆盖缺口**：`MaiAgent/cli/MaiConsoleMain.cpp:691–699` 没有设置
  `baseInstructions`；核心不会自动补充共享指令。此缺口不能解释当前 Windows App 现场。

更新磁盘源码不会改变已经创建的旧进程 Options，也不会替换当前正在运行的模型请求。
本次没有为验证提示词而中止用户任务。

### 正文接收链与调查边界

当前源码可将普通正文交付到独立的正文事件与存储：

- Responses：`MaiResponsesClient.cpp:458–507` 处理正文 delta/done/item 完成，
  `:403–405` 补充终止快照。`commentary` phase 不作为隐藏条件。
- Chat：`MaiOpenAiClient.cpp:374–379` 将字符串 `delta.content` 立即送入 `onText`。
- 本地模型：`MaiLlamaTools.cpp:1003–1012`、`:1103` 逐增量交付 `content_delta`。
- Runner：`526–537` 处理可见正文，`544–552` 单独处理 reasoning。

这证明当前源码具备接收和展示模型中间正文的路径，不能据此反推截图进程的供应商实际发送过哪些事件。
本次调查机器的正常 App 日志没有 Agent 请求、模型名或正文/工具事件类型的可用计数记录；
对 `MaiAgent`、`AgentController`、`MessagePartDelta`、`MessagePartUpdated`、
`text_delta`、`reasoning_delta`、`tool_call` 的匹配计数均为 0。
因此没有证据把现场原因直接归给供应商不输出、供应商适配器丢字或 UI 隐藏。

另有协议兼容边界：Responses/Chat refusal 正文、Chat 数组正文和非流式 message 快照尚未完整消费。
没有证据证明这些事件出现在本次现场，不将它们列为已确认现场根因。

## 方案边界与后续核对

- 前轮曾提出 `tools.clear()` 额外调用模型生成中间说明的候选方案。
  **该方案未采纳、不实施**：它会增加请求，并非当前 Codex 的进度机制；用户尚未要求这种新策略。
- 先比较实际提示词、请求角色、item 正文事件与异步工具调度，不杜撰“每 N 次工具强制输出”规则。
- 先用假模型验证正文接收、保存、顺序和工具后续流程；再对应截图进程及安装产物。
  不用仅含提示词契约的测试替代真实供应商行为验收。
- 忙时仍能输入发送、内部接收排队、停止只终止执行而不阻挡输入，属于另一项需求，
  由独立实现记录处理，不将其混入本文件的工具调用限制结论。
- 后续诊断仅记录版本、请求来源、模型名称和事件类型计数，不记录凭据或聊天正文。
  不为调查自动发出模型请求或停止当前任务。

## 当前结论

用户已经确认另一台 Windows 使用 `2.2.47`，且中间文字可以显示。
本轮结论是：**所查 Codex 客户端没有“连续 N 个工具后必须输出中间文字”的规则**；
60 秒是模型提示要求，没有对应的宿主强制正文/禁用工具一轮机制。
MaiAgent 现有 **64 轮模型工具循环**与**相同调用及结果重复三批**限制用于收尾和防绕圈，
不等于工具个数限制，也不是进度文字触发条件。
本次调查机器的旧 `2.2.29` 与另一台截图机器无关，不再据此推断现场根因。

## Codex 客户端源码证据

### 范围与来源

- 本地参考仓库：`E:/OpenSource/multi-ai-code/third_party/aicli/codex`。
- 核对 HEAD：`2165b1da63527b66f4e491ff5e5c96d763eab973`。
- 下列路径与行号均相对此 Codex 仓库，结论只针对该版本客户端；
  不据此推断供应商服务端内部限制，也不宣称所有模型始终遵守进度提示。
- 只读源码检查；未构建、发出模型请求、联网或执行新的真实用户任务。

### 连续工具调用没有次数触发的纯文字回合

| 位置 | 已核实行为 |
| --- | --- |
| `codex-rs/core/src/client.rs:1076` | Responses 请求固定使用 `tool_choice: "auto"`，没有按连续工具数切成 `none` 的分支。 |
| `codex-rs/codex-api/src/common.rs:279` | `ResponsesApiRequest` 没有 `max_tool_calls` 字段。 |
| `codex-rs/core/src/stream_events_utils.rs:323`、`:356` | 工具 item 完成后安排工具执行并设置 `needs_follow_up = true`，不要求本次先有可见文字。 |
| `codex-rs/core/src/session/turn.rs:454`、`:604`、`:718`、`:827` | 外层循环根据工具后续、待处理输入、预算和取消等条件继续或结束；没有每 N 个工具切换为纯文字回合的计数。 |

这不是说客户端没有预算、取消、并发或工具自身的边界，而是这些边界没有被实现为
“达到某个连续工具次数后强制补一条 commentary”。

### 进度要求属于模型指令

`codex-rs/models-manager/models.json:76`、`:250` 的模型 `instructions_template`
在 `Intermediate commentary` 节要求：需要工具时先发送 commentary，持续工作期间
不应超过 60 秒没有 commentary 更新。旧模板
`codex-rs/core/gpt_5_1_prompt.md:34`、`:186` 也要求工具工作期间及时更新。
这些文本会进入模型指令，但没有配套的客户端计数器或计时器强制生成可见正文。

`codex-rs/core/src/session/time_reminder.rs:157` 确实可以按配置注入当前时间，
但注入内容在 `codex-rs/core/src/context/current_time_reminder.rs:40` 只是当前时间，
不禁用工具、不合成进度，也不强制下一响应只能是文字。

### `message_admission` 不是先文字后工具的闸门

- `codex-rs/core/src/tools/parallel.rs:134`、`:204` 调用并持有消息准入令牌。
- 实际匹配在 `codex-rs/core/src/tools/user_messaging.rs:19`：只处理 CodeMode 中
  以 `send_message` 结尾的消息发送工具，不处理普通工具前是否已有 commentary。
- `codex-rs/core/src/session/retained_context.rs:175` 检查任务追踪器是否关闭并取得令牌，
  `:184` 在收尾时关闭并等待这些任务；作用是消息发送的取消、生命周期与持久化协调。
- `parallel.rs` 的读写锁负责可并行工具与串行工具之间的执行调度，
  也不要求先输出一段正文再允许下一批工具。

### 可见正文与 reasoning 分开

`codex-rs/protocol/src/models.rs:956` 将 `MessagePhase::Commentary` 定义为回合中的
assistant 正文，后面可以继续工具或其他输出；同文件 `:1069` 的 `Reasoning`
是另一种 item。正文 delta 在 `core/src/session/turn.rs:3120` 处理，
reasoning summary/raw delta 在 `:3170`、`:3246` 等分支独立处理。
用户需要的进度属于可见 assistant 正文，不能用隐藏 reasoning 替代。

### 不要把记录容量误读为执行限制

- `codex-rs/core/src/session/turn.rs:2695` 的 256 只限制本次响应保留的分析用调用 ID，
  `:2805` 只决定是否继续向该分析列表添加 ID，不阻止后续工具执行。
- `codex-rs/core/src/tools/executed_tool_calls.rs:49` 的 256 是尽力记录的调用元数据容量，
  不是每回合工具执行上限，也不会触发 commentary。

因此，本轮没有依据将“每 N 个工具强制正文”作为 Codex 已有流程移植；
`tools.clear()` 额外请求生成进度的旧候选方案仍为**未采纳、不实施**。
