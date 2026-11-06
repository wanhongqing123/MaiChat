# 已结束的 Agent 提问残留在桌面发送入口

## 现场与验收步骤

- 日期：2026-11-06，Windows；用户截图中连续出现两条红字 `question is no longer pending`，输入框仍保留下一条消息。
- 调查基线：`ce82cb5b62b1c6ac43a0edfebfc9a369dc54799d`（Windows 已发布 2.2.48）。截图所在另一台 Windows 的实际进程与安装版本本次尚未读取，不能由本机旧进程代替核实。
- 使用内存存储和本地 fake model 发起 `question`，工作区为非空临时/测试目录，不调用供应商、不读取用户历史。
- 操作一：等待问题出现，输入下一条草稿，点击独立 Stop；结束后应恢复普通发送，草稿保留，再点 Send 应新增正常用户消息。
- 操作二：问题通过 Core API 从另一入口完成回答；当前页面应收到一次回答通知，恢复普通发送且保留本地草稿。
- 操作三：Core 已完成回答但 GUI 尚未处理通知时点击 Send；该点击应保留草稿并解除旧问题路由，不能将过期答案自动提交为新任务，也不能重复展示底层英文错误。用户再次明确发送时才走普通消息入口。
- 会话隔离：结束 A 的问题不能清除 B 的有效问题；取消或超时不能被广播成用户已经回答。

## 调用链证据

- `MaiQuestionGate::reply` 对已完成/不存在的请求返回 false；`MaiAgent::submit(MaiReplyQuestion)` 将其返回为 NotFound。这是有效的重复提交保护，不能通过把错误改成成功规避。
- Core 已定义 `QuestionAnswered` 和 emitter，但基线没有任何调用方；`QuestionTool` 仅等待并返回结果。
- 桌面 `AgentChatPanel` 仅依赖未发出的 `questionAnswered` 清理已展示问题，任务结束/失败没有协调旧问题状态。
- `onSend` 优先使用缓存 `pendingQuestionId`，失败后保留该 ID。故每次点击都会再次提交同一个失效问题，形成重复红字。
- Android/iOS 普通消息输入与问题卡独立，不存在桌面同型的输入路由；共用 Core 的真实回答事件仍需补齐，以便移动快照及时更新。

## 修复和验证

### 修复

- `MaiToolContext` 增加可选真实回答回调，`MaiTurnRunner` 绑定已有 emitter；`QuestionTool` 仅在等待线程获得非空的闸门返回值后通知。用户明确提交空回答仍由闸门标准化为有效回答；取消、超时不通知为已回答。
- 桌面在相关工具状态及任务终态事件中读取权威 pending，解除已结束问题；当前会话和直属子任务参与协调，无关会话不能清除仍有效的问题。
- 发送前核对问题仍有效，提交失败后再次协调结算竞态。问题失效时恢复普通发送，保留草稿和附件，并显示一次可理解的说明；该次点击不自动变为新任务。
- `clearQuestion` 通过现有媒体感知 placeholder 更新入口恢复界面，不清空用户输入。
- Desktop 版本升至 `2.2.49`。Windows 包对应的完整源码提交、资源树与 EXE 身份由发布的 `release-manifest.json` 绑定。

### RED → GREEN

| 本地真实 Qt 面板操作 | 基线 RED | 修复后 GREEN |
| --- | --- | --- |
| 等待问题 → 输入草稿 → Stop → 普通发送 | 发送按钮仍为“回答问题” | 按钮恢复，草稿保留，再次发送产生第 2 条正常用户消息 |
| 从另一个 Core 入口回答 | 缺少 `questionAnswered` 通知 | 恰好收到一次通知，草稿保留 |
| Core 已结算但 GUI 尚未处理通知时点击发送 | 正文含现场英文 NotFound | 不显示该英文错误，不自动创建新消息；保留草稿，再点 Send 正常发送 |

- 三项各自的 QtTest：RED 均为预期失败，GREEN 各 `3 passed, 0 failed`（含 init/cleanup）。本机 VS2019/MSVC 14.29、Qt 5.15.2，内存数据库、Fake model，无供应商请求。
- 修改公开 C++ 上下文后显式移除 **516 个自有 C++ 旧对象**，当前目标重编 **440 步**；`maichat` 正常构建后的 `windeployqt` 与 **95 项 PE 导入/插件检查通过**，随后才启动 GUI 回归。
- Core：新用例先精确失败两条缺少回答事件的断言；清理 739 个旧产物，重编 741 步，**9/9 套通过**（172.12 秒）。覆盖事件归属和恰好一次、重复 NotFound、空回答、跨会话取消隔离。原 8 套必需门禁与 `MaiQuestionTests` 均执行。
- Android：显式清理 110 个 native 产物、重编 115 步，`maichat_agent` 与 `MaiMobileSteeringTests` 构建通过。OPPO PKD130 / Android 15 实际执行 C ABI 测试通过，涵盖回答通知、非强制快照及远端快照的 pending 清空、重复回答 NotFound。测试程序本地与设备 SHA256 相同：`f3d28a7e458adff1952e776399c80f88a71bd55eccbd9c3b4286413c3c591833`。
- 独立 Core 与 Desktop 代码审查均未发现本次引入的阻断问题。

### 证据与边界

- 本机独立证据目录：`.cache/question-lifecycle-20261106/`。`core-provenance.json` 绑定四个 Core 改动源码、九个 EXE 和测试日志；`desktop-focused-validation.json` 记录三个用例 RED/GREEN 与构建身份；`mobile-validation.json` 记录设备和 native 产物身份。
- 已人工查看三个真实面板截图：`screenshots/question-stopped-draft-preserved.png`、`question-external-answer-draft-preserved.png`、`question-expired-draft-preserved.png`。停止后输入保留，外部回答后恢复普通发送，竞态下显示保留草稿提示，无重复红字。
- 未替换或停止用户已运行的 Windows/Android App，未在用户截图的另一台 Windows 账号上操作。当前截图版本及真实账号现场仍需在升级后的实际使用中核对；不宣称已完成该现场验收。
- Android 此轮为真实设备 native 集成测试，不是正常 App UI 验收；macOS/iOS 当前环境未构建或运行。共享 Core 改动不能推导为所有平台已验收。
- 多个子任务同时提问时，桌面仍使用现有单一问题输入路由；本次不扩大为多问题交互重设计。子任务取消／超时在 Qt 层未另建专项用例，共享 Core 取消与会话隔离已覆盖。
