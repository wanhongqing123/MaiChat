# Codex Core 与 MaiAgent 架构对照

时间：2026-11-21，Asia/Shanghai。核查方式：读取本地源码、公开入口和主调用链；没有运行产品或测试，没有修改产品代码。

- MaiChat：`cdf82acffde9565ae4cec88e4ca37db75d76bbef`。
- 参考 Codex：`553c764e8b99fa519e9c6eb9b657875ef0f5e1c1`。
- 问题：两者是否采用相同架构，哪些差异来自职责边界，哪些只是能力或实现选择不同。
- 方法：从公开库入口追踪会话管理、操作受理、轮次、上下文、模型、工具、事件、存储及子 Agent，不能按目录名或文件数判断。
- 这是核心运行架构核查，不是 Codex 全产品、所有特性开关及每个模块的完整审计。

## 结论

两者采用相同的基本运行范式：宿主调用核心库，核心组织上下文，调用模型，根据模型输出执行工具并回传结果，发布事件并持久化。

它们不是对象结构和执行机制完全相同的实现。MaiAgent 采用多会话门面、每轮工作线程、普通接口注入、同步事件订阅与消息状态存储；Codex 将多线程管理、单会话运行、轮次和请求步骤拆开，并通过异步任务、消息通道、执行快照、历史记录重建及树级协调连接各层。

Codex 的 `codex-rs/core` 是编排核心，但并不包含所有依赖实现。`core/Cargo.toml:14` 起明确依赖同仓库的 protocol、model-provider、history、rollout、thread-store、sandboxing、exec-server 等 crate。不能把这些接口在 core 被调用误报为全部实现在 core。

## 职责映射

| 职责 | Codex | MaiChat / MaiAgent | 判断 |
|---|---|---|---|
| 可嵌入核心库 | codex-core，lib.rs | maiagent，MaiAgent 普通 C++ API | 同一方向，界面不是执行核心 |
| 多会话管理 | ThreadManager / ThreadManagerState | MaiAgent::Runtime 的 store、active、worker 管理 | 有对应职责，但对象粒度不同 |
| 单会话运行对象 | CodexThread + Session | 持久 MaiSession + Runtime 中按 sessionId 定位的运行状态 | 我们没有同粒度的独立 Session 运行对象 |
| 操作与输出 | CodexThread、SessionIo、Op、Event | MaiOperation、submit、MaiEventBus | 同类边界，通道机制不同 |
| 一轮模型/工具循环 | tasks/regular、session/turn.rs | MaiTurnRunner | 主要控制流相同 |
| 每次请求的环境与配置 | StepContext、ResolvedStepSettings、ToolRouter | Runner 固定本轮模型/effort，buildRequest 和 MaiToolContext | 已有部分快照，但没有同样的版本化请求对象 |
| 上下文 | context、context_manager、world_state、compact 系列 | MaiContextBuilder、MaiContextCompactor、MaiContextCheckpoint、原史查询 | 都有历史/预算/摘要，状态组织方式不同 |
| 模型接口 | ModelClient、每轮 ModelClientSession、SharedModelProvider | MaiModelClient、具体 wire/路由客户端 | 都隔离供应商；生命周期与传输状态不同 |
| 工具 | ToolRouter、ToolRegistry、ToolCallRuntime、handlers | MaiToolRegistry、MaiTool、Runner::executeTools | 都有声明/路由/执行；调度与扩展分层不同 |
| 审批与执行约束 | approval、exec_policy、guardian、sandboxing 等协作 | MaiPermissionGate、工具风险检查、路径与平台能力边界 | 两边已有约束，不能按名称视为一比一等价 |
| 持久化 | rollout/history、thread-store、state 索引与历史重建 | MaiSessionStore、Memory/SQLite、消息/片段原位更新和检查点 | 都有持久化，记录与恢复模型不同 |
| 多 Agent | AgentControl、tree runtime、ThreadManager | MaiSubAgentHost，由 MaiAgent 管理父子会话 | 子 Agent 都复用会话；协调层独立程度不同 |

## 有实质意义的差别

### 1. 运行对象粒度

`core/src/thread_manager.rs:586` 的映射保存多个 CodexThread；`codex_thread.rs:198` 的每个对象持有一个 Session 和自己的 SessionIo。Session 再持有本会话服务、状态和 active turn。

`MaiAgent/Agent/MaiAgent.cpp:221` 的 Runtime 则持有一个存储、模型、工具注册表，并用 active 映射和队列表管理多个会话。`MaiSession` 是纯持久数据，不是带独立执行循环的运行对象。

因此 MaiAgent 不能简单等同于 CodexThread，更接近 ThreadManager 与一部分 Session 门面的组合；MaiTurnRunner 对应 run_turn 一带。

### 2. 操作与事件的调度

Codex `session/mod.rs:455` 定义 SessionIo 的提交/事件通道，`:749` 创建通道，`:1140` 在 Tokio 任务中运行 submission_loop；公开 next_event 在 `codex_thread.rs:954` 读取事件。另有专门的 typed input 等入口，不能声称所有公开方法都只往一个队列投递。

我们 `MaiAgent.cpp:1227` 的 submit 直接 visit 操作并同步完成受理校验/相关存储；发送模型任务时在 `:1751` 建立本轮 std::thread。MaiEventBus 的订阅者在发布线程同步执行，见 `MaiEventBus.h:96`，慢处理需宿主另行排队。

这不是 Rust 与 C++ 必然造成的差别，而是当前选用的调度策略。没有证据支持必须为对齐而把整个核心改成异步框架。

### 3. 轮次与请求步骤

Codex `session/step_context.rs:30` 每次请求持有不可变配置、环境和已定稿工具路由；原 turn 和取消/授权状态仍有各自生命周期。

我们 `MaiTurnRunner.cpp:248` 在轮次开始固定模型和 effort，工具声明在 buildRequest 取得，执行再按名称查注册表。注册表在活动运行期间禁止修改，见 `MaiTool.h:283`。这能维持当前静态目录的一致性，但不能直接支持运行中热更新而不补版本绑定。

### 4. 工具并行

我们同一模型返回的工具批次在 `MaiTurnRunner.cpp:684` 顺序执行；不同会话和子 Agent 可并行。Codex `tools/parallel.rs:183` 检查工具是否支持并行，并在 `:292` 附近使用共享/独占执行闸门控制调度，不是所有工具无条件并发。

若要借鉴，先定义读写冲突、权限等待、取消和回执顺序，不能仅给 for 循环套线程。

### 5. 持久化与重建

Codex `session/rollout_reconstruction.rs:15` 的重建结果不仅是消息，还包含保留上下文、轮次归属、设置、world-state 基线和窗口身份。其持久化还连接 thread-store 与 state DB，不能简单称为“只有事件文件”。

我们 `MaiSessionStore.h:28` 起以会话、消息和片段读写为核心，并另存输入凭据、工具原始输出、专业任务与摘要检查点。普通轮次缺少同等的持久终态/执行快照，见 session 能力对照。也不能据此说所有存储都必须替换为事件溯源。

### 6. 协调与扩展

Codex `agent/api.rs:1` 定义独立于线程运行地点的树级协调接口；`agent/host/ownership.rs:15` 支持核心或宿主拥有协调权，并注入 admission/accounting 等服务。它不等于已经实现任意分布式 Agent。

我们的 MaiSubAgentHost 已是接口，但主要由 MaiAgent 自己承担协调。独立普通 C++ 工具和模型工具共用实现仍应保持；需要借鉴的是生命周期、结果、预算和权限契约，不需要为架构对齐引入 MCP、新依赖或模型代理。

## 对后续工作的意义

保留现有普通 C++ 核心与跨端适配。优先把轮次结果、用量/上下文状态、输入来源与树级共享约束变成清晰的数据和接口；只有在恢复、运行中配置更新或多会话资源隔离确实需要时，再考虑独立会话运行对象或更细的控制器。

本次未构建、运行故障夹具或更换安装产物，不作平台验收或性能优劣结论。原 qtbase WIP 保留；新增仅为本架构记录。
