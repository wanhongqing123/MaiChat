# iOS 创作中心忙碌时右侧单按钮

日期：2026-11-03。基线 `2c823e74a03e17acea5f00baa8ad7dadd77d67c3` 加本批修改。
用户真机截图指出忙且已有输入时仍同时显示黑色停止、蓝色发送。

## 可复现操作与预期

用户随后明确更改要求：输入栏完全移除停止，不再采用“清空草稿恢复停止”。
正常 Root 的创作中心保持一个正在执行的任务：空草稿时无右侧停止/更多；输入补充文字后，
右侧只有可用“补充当前任务”。不同时挂载 stop/send，不另加第二处停止入口。
清空草稿或本地持久入队成功后清空该次对应草稿，均不重新挂载停止；后者不等待 native ACK。
派发时仍 busy 的消息补充到有效 turn，不取消正在运行的工具；尚未派发消息遇原轮结束，
按用户后续排队要求可在同一会话走普通 send 开下一轮。已派发后送达未知则固定原绑定，
不能自动换成新 turn。未知/拒绝内容保留在队列，不再要求一直留在输入框中。
idle 的附件/发送布局保留；语音转写也可继续生成排队消息，原“忙时禁止转写”条件已移除。

## 定位与验证计划

`AIAssistantView.swift` 的 composer 原先无条件挂载更多/停止 Button，随后只要有草稿又挂载
发送 Button；在 `snapshot.busy && hasDraftContent` 时两者都会出现。此前旧 UI 测试甚至
断言补充按钮出现时停止仍存在，此断言与用户新要求冲突，必须更新而非忽略截图。

先只更新实际 Root UI 断言并添加
`AIAssistantUITests/testBusyComposerOmitsStopAndShowsOnlySupplementWhenDraftExists`。
新用例检查空草稿无 stop/更多 → 输入后仅可用补充 → 删除草稿后仍无 stop/更多，
并保留各状态截图；
原 `testBusyComposerSupplementsCapturedTurnAfterReceipt` 继续检查合成宿主的接收回执及原任务
仍 busy；清稿时机由本地持久入队确认决定。Debug fixture 的只读 AX 状态取实际 model.busy、selected、turnId，
不能以停止图标是否存在替代业务状态。

主任务已在旧 App 177 + 新 Runner 实际取得红证据：
`/private/tmp/maichat-composer-no-stop-red-20261103.xcresult`，同名 `.log`，
16.057 秒，失败位于 `AIAssistantUITests.swift` 当时第 73 行
“忙且空草稿也不显示停止”；不是新 fixture probe 缺失导致的假红。
随后产品仅在 busy 时不挂载原 stop/plus block，保留 idle 原附件按钮；busy 有草稿保持补充发送，
含切换到语音模式后已有草稿的情况。没有新增停止菜单/另一处按钮。

去 stop 的这一子修复只改 composer 挂载条件和对应 UI 测试，没有另造停止入口。
后续连续发送另改 Model 的本地入队/投递接线，仍复用原核心 send/steer；不能把整批变更
描述为“只隐藏按钮”。本子任务不改工程版本或真机用户会话，不触发付费模型。
实际构建、录屏、安装由主任务协调。

用户后续明确连续发送要求，已进一步接通持久队列；不能只隐藏图标后宣称满足连续发送。
对应行为和实际验证边界见 `maichat-ios-outgoing-queue-20261103.md`。

178 首次最终同包中，本 case 在清空输入步骤失败：聚合 4 个 delete 后实际值为“补充细”，
当时 stop 不存在的断言已通过。按现场要求改为先确认“补充细节”并 focus，再逐次一个
delete，分别等待并断言“补充细”“补充”“补”“”；最终空/no-stop/原 turn busy 不变。
只改测试输入刺激，不放宽断言或改产品 binding；是否存在真实回写问题仍以定点复测为准。

最终定点复测本 case 实际通过，回执
`/private/tmp/maichat-batch178-two-recheck-20261103.xcresult`；已查看空稿、输入、清稿三张
实际截图以及录屏抽帧，原任务 busy 保持，stop 均不存在。完整平台回执见
[178 交付记录](maichat-batch178-validation-20261103.md)。
两项复测位于 `/private/tmp/maichat-batch178-two-recheck-20261103.xcresult` 和同名日志/录像；
当前不提前记录通过。
