# 连续输入与 Codex 排队策略对照

## 基准与方法

用户问连续输入多条消息何时读入模型、MaiChat 是否对齐。实际定制 Codex 位于 `E:/OpenSource/multi-ai-code/third_party/aicli/codex`，HEAD `6e09a1b58f759848942e3d5b7653ab714f045ada`，describe `rust-v0.172.0-32-g6e09a1b58`，Cargo版本 `0.172.0+multi-ai.3d86e43b07`，工作区干净。另一份 `E:/OpenSource/codex` 更新源码不是本次现场比较基准。

按 openai-docs 入口查阅官方 CLI 概述；概述没有建立这些具体消费时机，以下细节以该定制版调用链与测试断言为依据。只读核对，未读取用户配置或会话、未重跑 Codex 测试、未修改队列实现。`instant_interrupt=true` 是源码默认值，不保证现场配置未覆盖。

MaiChat 比较发布源码 `74470f39`；相关实现与本次修复前 `be27b3c2` targeted diff 为空。实现集中在 `MaiAgent.cpp` / `MaiTurnRunner.cpp`，不能把测试目标名当作独立源码模块名。

## 结论

MaiChat 已支持忙时补充输入在后续模型请求前消费，不必等整个任务结束；但不完全对齐即时steering、明确下一轮队列及等待唤醒等语义。

| 场景 | 定制 Codex 默认实现 | MaiChat 4.0.2 |
| --- | --- | --- |
| 忙时普通发送 | TUI Enter提交，有活动轮次则turn/steer | Desktop/iOS/Android本地忙时提交steer |
| 明确下一轮 | TUI Tab留前端FIFO，空闲后每次发一条 | enqueue忙时仍可offer到当前轮，非明确下一轮 |
| 模型正在生成 | 默认instant_interrupt唤醒采样中断，排空与计费归属确定后再请求 | 当前stream返回后才消费新输入并补请求，不即时中断 |
| 普通工具正在执行 | 先收真实工具结果，再构造后续请求 | 当前工具结束后吸收输入，旧批后续未启动工具取消并重规划 |
| 多条steer | 同一消费边界前的多条按顺序一起进入后续请求 | 同样批量消费，保留每条User消息与顺序 |
| 特殊等待 | sleep/agent wait/CodeMode可因输入醒来或yield | 未见向工具传递新输入唤醒/yield的通用入口 |
| 结束边界 | 显式下一轮仍在前端等 | closeSteering前有pending则继续，关闭后下一轮可整批消费 |
| 失败退避 | pending_input测试要求steer跳过stream retry backoff | 消费steer但保留原Retry-After截止时间 |

因此“请求A已经发出后，B/C追加到A正在执行的HTTP请求中”不是上述普通消费模型。它们在新的请求构建边界进入历史；即时steer还涉及结束/排空当前采样，普通已执行工具不统一强杀。

## 源码与测试定位

定制 Codex 下列路径均相对于 `codex-rs/`：

- Enter/Tab：`tui/src/bottom_pane/chat_composer.rs:897`、`:3714`；提交分流 `tui/src/chatwidget/input_flow.rs:66`，运行中steer `tui/src/app_server_session/user_message_submission.rs:224`；明确队列仅空闲发一条 `input_flow.rs:254`、`input_restore.rs:259`。
- 下一请求构建前drain：`core/src/session/turn.rs:545`、`core/src/session/input_queue.rs:481`；首轮原始输入和压缩等路径有明确延迟例外。
- 默认instant_interrupt：`features/src/lib.rs:1415`；watcher `core/src/session/input_queue.rs:341`；响应排空/计费边界 `core/src/session/turn.rs:3210`。
- 两条steer与真实工具输出同进第二请求：`core/tests/suite/pending_input.rs:1963`；stream retry跳过backoff `:1908`。
- 未启动旧调用拒绝的已核实决定性路径：`core/src/stream_events_utils.rs:398`，限定 `AfterCompleteResponse + instant_interrupt`。不能泛化为Codex所有串行工具都逐项丢弃。

MaiChat：

- `MaiAgent/Agent/MaiAgent.cpp:848` 忙时offer，`:858` 下一轮整批消费，`:2176` 所属端持久收件箱enqueue入口。
- `MaiAgent/Agent/MaiTurnRunner.cpp:1715` 同步stream，`:942` 消费补充；`:1518` 保留Retry-After。
- `MaiAgent/tests/MaiInputQueueTests.cpp:205` 当前A结束、旧B/C取消再规划，`:478` 80条按顺序同轮消费。此次核对的是既有断言，不宣称新增运行验证。

Skills 目录另已核：`MaiSkillManager.cpp:591` 工作区 `.agents/skills`，`:596` 用户 `.agents/skills`；没有默认 `.codex/skills`。工作区逐层发现到仓库根，正文按需读取。
