# 首批架构借鉴：持久轮次结果

日期：2026-11-21，Windows x64。本批基于 MaiChat `cdf82acffde9565ae4cec88e4ca37db75d76bbef`，参考本地 Codex `553c764e8b99fa519e9c6eb9b657875ef0f5e1c1` 的轮次结果、请求身份与生命周期分层。不是全部 Agent 差异的完成声明。

## 可复现操作与原始观察

1. 用无网络的固定模型创建父子会话：子轮次 A 返回 `old answer`，轮次 B 返回 Network 且没有正文。预期 B 结果为失败，不能返回 A 的答案。
2. 查询空回答、取消、另一会话的轮次及不存在的子会话。预期错误分类清楚，不能把缺失解释成仍在工作。
3. 用内存和临时 SQLite 保存轮次状态；重新打开，并恢复没有任何工具的遗留 Running 文本请求。预期状态保留且中断恢复不触发请求。
4. 经 steering 生成多个 assistant 分段后，按原始 turnId 查询终态及结果消息。预期新消息段不能变成新轮次或覆盖旧轮次身份。

旧行为实际 RED：

- `MaiSubAgentTests --turn-outcomes`：8 条失败，包括返回旧答案、无类型化结果、缺失子会话被报成仍在执行。
- `MaiStoreTests --turn-outcomes`：9 条失败，涉及 SQLite 元数据丢失与遗留轮次未修复。
- 首次编译曾因未加载 VS 环境找不到标准头；加载既有 enter-vs 后编译 0，再取得上述运行 RED。编译环境错误没有当作功能 RED。

## 实现

- 新增 MaiTurnOutcome，明确 Running、Completed、Failed、Interrupted。稳定身份为初始 assistant ID，分段通过 turnId 指回原轮次。
- 在现有消息模型末尾追加可选状态和身份，不把它们拼进模型正文。MemoryStore 与 SQLite 读取返回同一普通数据。
- SQLite v14 仅新增可空 turn_state 元数据列，写锁内复核迁移；旧记录保持未知。真缺列 v13 与“列已存在但版本较旧”均有独立用例。
- Runner 在工具清理和普通消息收尾后保存原始轮次结果，再发 turn.outcome.updated。错误存储不发布成功结果。取消也同步影响 succeeded()，不与持久状态矛盾。
- 复用 recoverInterruptedTools 的既有启动恢复事务修复遗留 Running 轮次（包括无工具文本）；不自动重发模型或重做工具。
- 新增普通 getTurnOutcome、受直接父子关系限制的 subAgentOutcome，以及固定 turnId 的 subAgentReportForTurn。
- 新增只读 session_status 工具，仅当前绑定会话可读；wait_agent 明确缺失、失败和中断，按已捕获 turnId 取报告，避免两次查询“最新”造成跨轮次竞争。
- 旧消息无记录时返回 NotSupported；不从 completed 时间、有无正文或旧报告推测成功。普通空回答原有的一次有界补答行为保持，仍无回答记录为 Failed。

## 审查中补充的失败用例

- 无主模型的专业任务通知曾被本批中间实现误标 Running；原取消通知用例新增两条断言，实际 RED 后修正。专业通知保留专业任务账本的状态，不冒充普通模型轮次。
- cancel flag 单独取消时，旧 succeeded() 可能仍为 true。原 turn-cleanup 用例新增 !succeeded，取得实际 RED 1 后统一取消事实。
- 已捕获 A 的完成结果后，即使 B 已失败，仍能精确读取 A 的结果；不能再次查询最新结果，也不能跨父会话读取。
- 独立只读审查及对上述修复的窄复审均结束，未发现剩余 P1/P2；审查本身不是运行验证。

## 实际验证

构建目录：`MaiChat/desktop/.cache/build-qt6-ui-20261109`。使用既有 VS 环境和普通 CMake/Ninja 构建；不混用旧布局消费者。

- 正常 Windows Core、受影响消费者、控制台、ModelClient 测试目标及 maichat 编译/链接 exit 0。maichat 构建后的依赖部署与 PE 检查成功：3 个 binary、114 个导入及 Qt 插件。
- 经 `run-tests-windows.ps1` 运行 11 套 Core 回归：11/11，0 失败，60.69 秒。覆盖 Core、Store、InputQueue、ToolOutputStore、ContextCompactor、SpecialistNotification、SubAgent、TurnRunner、ContextBuilderImage、ContextImport、ContextImportPurge。
- 同源 `console_e2e.py` exit 0，65 个 OK 检查；实际启动新构建控制台，通过聊天、工具、审批、会话、持久化及强制退出恢复。HTTP 仅连接用例自建 127.0.0.1 假模型。
- 普通桌面 candidate exe SHA256：`9c5657a068e6cf81af1a8233412dd6466b8e099d98bf3f637da4ed0d615770d2`。
- 控制台 exe SHA256：`abc02821effc453551a317256f8404732f49fdefd78d47b41ed5d89cb8b0dcf4`。
- 日志保存在 `.cache/turn-outcomes-*-build.log`、`turn-outcomes-*-test.log`、`turn-outcomes-regression-tests.log`、`turn-outcomes-console-e2e.log`，不提交日志或临时数据库。

## 边界与保留项

- 未替换用户现装程序，未启动桌面 GUI 验收，未重发 GitHub Release。
- macOS/iOS/Android 本轮没有重新构建或运行；MaiMessage 布局和 MaiSubAgentHost vtable 已追加，必须同源重编相关消费者，不能复用旧 ABI 对象冒称通过。
- 本批不实现上下文余量展示、累计用量账本、共享消费预算、Agent 邮箱或整树配额修正；这些仍按已有审查记录分批推进。
- 无新库、MCP、模型代理、真实模型调用、凭据读取、用户数据库读写或服务部署。
- qtbase 原有 `qinputcontrol.cpp`、`qwindowskeymapper.cpp` 工作保持，既有未提交的三份源码对照文档不混入本批提交。
