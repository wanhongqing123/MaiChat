# Codex session 与 MaiChat 会话运行能力对照

核查时间：2026-11-21（Asia/Shanghai）。本次为本地源码静态核查，不是运行回归或功能交付。

- MaiChat：`cdf82acffde9565ae4cec88e4ca37db75d76bbef`。
- 本地参考 Codex：`553c764e8b99fa519e9c6eb9b657875ef0f5e1c1`，不代表最新上游。
- 核查范围：Codex `core/src/session/` 及其直接连接的 context、history、budget 实现；MaiAgent 普通公开接口、Runner、存储数据、事件及移动宿主快照。
- 本次未修改产品、索引、安装程序、供应商配置或凭据；未调用模型、读取用户数据库或运行测试。原有 qtbase 未提交工作保持原状。

## 可复查步骤和预期

1. 记录两仓库 HEAD，列出 `session/` 模块，沿 `turn.rs` 的实际调用核对能力；不能仅按文件名下结论。
2. 对照 `MaiAgent.h` 的操作集合、`MaiTurnRunner` 的请求/终态、`MaiSession`/`MaiMessage`/事件和存储入口。
3. 对“已有”项寻找正式调用路径；对“缺失”项同时检查普通 API、实际使用点及数据载体。
4. 预期得到分级差异，明确特性开关、供应商限制和静态推导边界；不把不同目录布局算作缺失，不把示例场景称作已复现故障。

## 已有基础，不重复实现

- 模型流式输出、思考与正文分离、完整响应的续接状态及工具结果回灌：`MaiTurnRunner.cpp:505`、`MaiModelClient.h:194`。
- 同会话单活动轮、多会话并行；用户取消、审批、问答、资源清理：`MaiAgent.h:210`、`MaiTurnRunner.h:40`、`MaiTurnRunner.cpp:180`。
- 指定活动轮次的 steering、幂等持久输入队列、安全点消费：`MaiAgent.h:100`、`:127`，`MaiAgent.cpp:1580`。
- 增量摘要、持久检查点、原史回查、工具调用与结果成对保护：`MaiContextCompactor.h:25`。
- 默认原窗口 90% 触发、95% 输入硬预算，显式输出预留可收紧：`MaiContextCompactor.cpp:696`。
- 先落库再广播；成功响应正文与流式增量有独立边界：`MaiTurnRunner.cpp:1006`。
- 部分资料来源已有独立标识，专业生成任务也已有持久恢复/结果投递。不能将其误报为全项目没有来源管理或恢复。

## 优先补齐的缺口

### 1. 持久化的轮次终态与结果归属（高）

Codex `session/mod.rs:2879` 构造带 thread ID、turn ID、父轮次、状态和错误信息的 `AgentTurnOutcome`，连接原生父 Agent 和可选宿主通知。

我们有稳定活动 turn ID、错误事件和消息完成时间，但没有普通持久化轮次结果记录。`MaiMessage.h:150` 保存 completed 时间；`MaiTurnRunner.cpp:1051` 的 finish 不区分结果地填 completed；错误另走 `:202`，活动表在 `MaiAgent.cpp:559` 清理。`subAgentReport` 在 `MaiAgent.cpp:1091` 扫描最后一个非空 assistant 正文，不按目标轮次定位。

影响：界面重连或父 Agent 查询时，无法仅凭统一记录明确区分指定轮次成功、失败、中断、暂停；上一轮有正文、下一轮失败无正文时存在返回旧正文的静态风险。该场景尚未运行复现。

验收建议：两轮固定假模型，第二轮失败且没有正文；结果必须指向第二轮失败，不能返回第一轮答案。重建 Agent 后查询仍一致。

### 2. 可查询的上下文窗口状态及低余量提醒（高）

Codex `session/context_window.rs:8` 同时表示活动占用、压缩统计口径、完整窗口硬上限、余量及阈值状态。`session/token_budget.rs:159` 在开关及配置允许时把低余量提醒、兜底整理提示写入上下文。

我们 `MaiContextPreparation` 已有 inputEstimate、复用/推进检查点标志，但 Runner 在 `MaiTurnRunner.cpp:490` 内部消费结果；公开 Agent 查询、事件和移动快照没有统一窗口状态记录，也没有同类按阈值去重的模型提醒。缺的是状态贯通，不是摘要算法。

建议统一返回：当前估算/已观测用量、计数依据、模型窗口、有效输入上限、余量、压缩阶段和检查点版本。未知窗口/用量必须保持未知，不能展示假精确百分比。压缩占用与累计消费分开。

### 3. 实际用量账本与整树消费预算（高）

Codex `protocol.rs:2567` 的 TokenUsage 区分缓存输入、缓存写入、推理输出等；`session/mod.rs:5770` 分别处理上下文与纯消费用量，记录按模型使用量并调用共享预算。`core/src/rollout_budget.rs:48` 根据实际 usage 累加权重并判断可选限额。

我们 `MaiModelUsage` 在 `MaiModelClient.h:177` 只有输入/输出，供应商已上报部分实际 usage，但 Runner `MaiTurnRunner.cpp:534` 仅用来提高本轮估算倍率。没有普通持久累计账本、缓存命中计数、按请求/模型归属的用量查询和主子 Agent 共享额度。

建议先把单次 usage 的来源、完整性、请求身份和去重语义定义清楚，再累加、展示并实现可选预算。未报告不能填 0；Token 预算不等于现金余额。Codex 也是响应后记账，不是绝不超额的未来费用预留。

### 4. 可恢复的活动轮次快照与显式挂起（中高）

Codex `session/daemon_recovery.rs:23` 保存已记录输入的普通活动轮次启动信息，并限制为可恢复的本地环境；`turn_suspension.rs:16` 先 flush、再复核活动任务、取消执行，故意不写终态以保留恢复资格。

我们启动时 `MaiAgent.cpp:649` 恢复孤立工具状态和专业任务状态；持久 inbox 也可恢复。但 `MaiAgent.h:132` 明确退出不自动重放模型请求。缺少普通对话当前轮次的暂停快照、显式继续接口和原 turn ID 恢复链路。

补齐不意味着自动重发：需要区分未消费输入、流中断、已执行工具、未知外部副作用；未知执行结果不能盲重试，更不能在启动时自动制造付费请求。

### 5. 统一输入来源与 Agent 邮箱语义（中高）

Codex `session/input_queue.rs:24` 的输入元信息携带 origin，TurnInput 另区分用户、函数结果、ResponseItem 和 Agent 通信；`:97` 区分只入邮箱与触发轮次。steering 与邮箱唤醒有不同优先级。

我们用户 steering/inbox 已完整，资料导入和专业任务回复也有来源保护；但普通子 Agent `MaiAgent.cpp:1022` 仍直接 submit MaiSendPrompt，运行中按默认 Busy 拒绝，任务指令和用户输入复用用户消息。缺少统一的发送者身份、消息类型、仅投递/启动闲置轮次选择及普通子 Agent 完成通知接入。

应复用现有持久输入基础；保留模型消息来源，不能把转述的任务文本升成新用户授权，也不要另建竞争队列。

## 已有部分覆盖、按需求补齐

### 6. 版本化执行配置与动态上下文状态（中）

我们在 `MaiTurnRunner.cpp:248` 固定本轮模型/effort，`MaiAgent.cpp:1266` 拒绝活动轮次修改这两项；这是一种有效的一致性策略。工具注册表在活动期间也不允许修改，见 `MaiTool.h:283`。

Codex `session/step_context.rs:30` 为每次请求持有不可变 settings/environment/tool_router 快照；`step_activation.rs:234` 受 StepModelSwitching 开关控制，在轮次身份复核和约束校验后更新后续步骤。`session/thread_settings.rs:27` 支持先持久化再发布动态工具目录。`context/world_state/mod.rs:363` 对指令、权限、工具等有独立持久比较状态和差量渲染。

缺口是可在请求边界生效的受控更新及统一状态差量协议；不是已有轮次必然配置串扰。实现时必须保证模型看到的工具与执行工具同版本，并明确哪些变更只影响下轮。不要仅为“像 Codex”引入其服务集成。

### 7. 手动整理、轮末整理与后台候选压缩（中低）

我们普通 Compactor 可直接调用，但标准 MaiOperation 无手动整理操作，普通 Runner 在下次请求构造时同步 prepare，没有后台候选管理与统一压缩开始/完成事件。

Codex `session/handlers.rs:271` 有手动 compact，`context_window.rs:117` 有轮末阈值；`session/async_compaction.rs:44` 明确受 AsyncCompaction 开关、RootOnly、TokenBudget 互斥及供应商 RemoteCompactionSupport::V2 限制。候选还绑定历史版本、模型和认证归属，不能直接装过期结果。

手动整理及状态可先做。后台推测整理可能增加模型消费，只能在供应商支持且明确启用后考虑，不属于当前必须照搬的基础能力。

### 8. 可回放的历史分叉与逻辑回退（中低）

Codex `session/rollout_reconstruction.rs:135` 在重放中处理用户轮次回退，同时重建摘要基线、保留上下文与设置；这为 resume/fork 提供一致历史。

我们的会话存储、原史查询和检查点存在，但公开 MaiOperation 没有从指定轮次分叉/逻辑回退的普通操作，也没有对应的设置与检查点联动协议。删除消息或清空会话不是等价替代。

这类能力只回退对话状态，不会撤销已经发生的文件修改、发送消息或外部操作；UI 必须说明边界。子 Agent 可选继承上下文是相关但不同的需求，见已有 agent 对照。

## 更后面的选项

- 结构化“已批准计划”转执行，避免附件不可读时静默漏掉部分计划：`session/approved_plan.rs:26`。我们已有 todo、问答和普通工具审批，不能把这项写成没有计划或没有审批。
- 当前时间按阈值提醒、新上下文窗口重发和时钟不可用状态：`session/time_reminder.rs:36`。已有时间工具不等于同类自动提醒。
- 通用扩展生命周期与指标贡献入口：Codex 在 session 中连接多个扩展点。可评估普通 C++ 宿主回调，不为对齐加入 MCP、代理或新库。

## 建议次序与验证边界

先做轮次结果/事件归属、窗口状态、实际用量账本；在这些可观察事实之上做预算、持久挂起与 Agent 邮箱。随后按产品需求增加请求边界配置变更、手动压缩、历史分叉。后台推测压缩最后评估。

以上是实现存在性和调用链对照；没有运行失败用例或作出目标平台验收声明。后续实现必须先用固定模型/存储夹具复现具体边界，不能把本记录中的静态场景当作已完成 RED。
