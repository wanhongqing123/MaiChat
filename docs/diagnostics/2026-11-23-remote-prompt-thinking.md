# 跨设备发到当前会话后缺少等待状态

2026-11-23，Windows 11、Qt 6.12.0、MSVC 14.44。

## 复现与预期

当前 Windows 创作中心停留在本机所属会话；同账号另一设备向它发送消息。用户消息应立即出现，在模型首个可见输出前显示“正在思考”和停止按钮。切换会话后恢复该状态，完成后收起，保留本机尚未发送的草稿。

用户现场进程为 PID 55288，`D:/Program Files/MaiChat/maichat.exe`。该进程启动日志中的版本为 3.0.1、启动时间为 2026-11-20 23:24:14；磁盘 EXE SHA-256 为 `2e79dbe81e9dbf7c700d5ed0c0609f5a0e35dd7f83c58c26ff6aa87770a2653c`。本次只读取启动版本、路径、PID，未读取用户会话数据库或终止该进程。

版本差异本身不是根因证据。以公开 3.1.3 源码 `266480465a6476985cb5b837dc35ea69274c9439` 和该版本正常 App 依赖库独立复现，也能看到同样的缺陷。

## 根因

`DesktopSessionSyncHost::applyCommand(prompt)` 提交 `MaiEnqueuePrompt`。Core 落库用户消息并启动模型 worker，但 `MaiEnqueuePrompt` 首次启动和 `Runtime::retire` 接续排队轮次都只发布 `MessageUpdated`，遗漏 `emitTurnStarted`。

`AgentController` 只从 `SessionStatus(detail=running)` 发出 `turnStarted`；`AgentChatPanel` 依靠这个事件切换等待状态。本机按钮还有 `onSend` 的即时反馈，跨设备入口则不经过按钮。因此文字能够出现，等待状态却没有出现。既有远程消息用例只验证气泡，使用瞬间完成的模型，未覆盖首个输出前的状态。

## 修复

只在上述两条队列启动路径补发带真实 turn ID 的开始事件。在 Runtime 锁外、worker 的 `signalReady` 前发布，保证宿主可安全回调只读状态，并先收到启动通知再收到模型输出。未改 UI 生产代码、数据库或协议字段。

加强既有测试：

- 通过真实 `DesktopSessionSyncHost` 和 `AgentController` 进入面板，控制模型在首个输出前等待，验证一次开始通知、一个可见思考提示、停止按钮、会话切换、正常完成和草稿保留。
- 输入队列首次启动只发一次通知；忙碌追加及幂等重送不重复通知；最终回答后的排队接续发出新的开始通知。事件回调重入 `isBusy`、`activeTurnId` 验证不在 Runtime 锁内发出。

## 验证与边界

独立验证目录为 `.cache/remote-prompt-thinking-20261123/`。测试链接正常 App 同次 Core/桌面库，以 `26648046` 加本次四行生产差异为候选，不包含 Root 其他 WIP。

- 原版业务 RED：Qt 的 `accepted.count()` 为 0、预期 1；Core 输入队列测试四个开始事件断言失败。保留原日志。
- 修复后正常 `maichat` 构建、部署和 PE 依赖检查通过。Core 队列套 exit 0（0.397s）；Qt 远程等待与本机消息回归 4 passed / 0 failed，含 init/cleanup（0.887s）。
- 亲看 `green-captures/remote-prompt-before-first-token.png`：消息后显示“正在思考…”、停止按钮和原草稿；完成截图显示回答，等待提示与停止按钮消失，草稿保留。
- 自有工程第一次默认选到 clang++ 导致 MSVC 参数不识别，第二次缺正常目标的媒体夹具宏；均为验证工程设置错误。改用与正常 App 相同的 MSVC 和夹具定义后完成编译，不计作产品 RED。
- 没有真实模型、付费 API 或真实跨设备网络往返；使用真实命令接收、Core 和 Qt 组件配合受控模型验证。Mac、iOS、Android 未实机验证。
- 未安装、重启当前 3.0.1，也未修改已发布 3.1.3 资产。本次源码修复尚未发布到用户运行程序。
