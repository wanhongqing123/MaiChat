# 流式内容持久化边界对照

用户于 2026-11-25 明确要求由开发者核对 Codex 后直接采用其策略，不再让用户选择增量格式。

固定参考源码：`d4af2452f7b5e39aadd7abd75b6e141059a4cc5d`，不冒充本机定制二进制
已逐行相同的源码；数据库结构基线另以实际 CODEX_HOME 的只读结构快照为准。

## 确认的源码策略

- `codex-rs/rollout/src/policy.rs:162–228` 将 ItemStarted、AgentMessageContentDelta、
  ReasoningContentDelta、ReasoningRawContentDelta 等归类为 transient/non-durable，返回 None。
- 同文件 `:134–141` 持久化 ItemCompleted、TurnStarted、TurnComplete、TurnAborted 等事件。
- `core/src/session/turn.rs:3409` 在 OutputItemDone 处处理完成 item。
- `core/src/stream_events_utils.rs:79–110/449–465` 记录已完成的 response item。
  完成边界是 item，不要求整轮所有后续输出都成功才保留此前完成的 item。

## MaiChat 联合存储的实施约束

1. 用户输入先可靠保存。生成中的文字增量只做实时展示和进程内草稿，不为每个 delta
   新造 JSONL 持久化事件，也不反复追加整段尚未完成的回答。
2. 已确认完成的 message/reasoning 等 item 作为持久化单位，保存完整内容、身份及来源。
   新后端仍遵守 MaiChat 的先持久化再广播正式完成事件约束。
3. 若 A item 完成、B item 只收到一半就中断，保留 A 和中断事实；不把 B 当完整 item。
   重启只保证恢复已提交内容，不承诺恢复最后未完成的半段。
4. 工具请求、实际执行、结果配对独立校验。持久化 item 不等于自动获准或执行工具；
   不能因重试或恢复重复执行已发生的操作，也不能把未知执行影响标成未执行。
5. 已存在旧库中的草稿是已有数据，迁移时保留并标明 legacy draft 来源；新策略不能成为
   静默删除旧数据的理由。失败草稿不作为正式模型上下文回灌。

此前提出的自定义 item_delta 落盘方案已由本策略替代，未作为正式存储格式实施。
当前旧 SQLite 实现仍有流式分块快照；只有联合存储接入及回归完成后才能宣称其已切换。
