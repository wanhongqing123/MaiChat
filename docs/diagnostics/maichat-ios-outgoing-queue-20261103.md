# iOS 连续发送、本地队列与待确认收据

日期：2026-11-03。基线 `2c823e74a03e17acea5f00baa8ad7dadd77d67c3` 加本批修改。

## 用户操作和结果

创作中心本机会话正在执行时，可以继续输入并连续发送。每次点击捕获账号、工作区、会话、
正文、引用和附件，按点击顺序持久保存；本地保存成功才清除此条对应的草稿/附件。模型或
附件上传尚未确认上一条时，新的发送不再被 `isSubmitting` 禁止。消息区保留“排队中”、
“正在发送”、“送达待确认”或“发送失败，内容已保留”，不是只在内存丢一个异步 Task。
同文连续点击生成独立 UUID，不再按正文 hash 合并。容量/写盘失败保留草稿，不虚报入队成功。
当前持久队列上限为 256 条、每条 payload 1 MiB、整份文件 16 MiB；连续接收不是无限容量。

输入栏在 busy 空草稿时不显示停止；busy 有草稿时仅保留补充箭头，发送/清空后也不恢复停止。
idle 的附件功能保持原行为。普通 stop API 仍供生命周期管理，未增加另一处停止按钮。

## 投递、隔离和引用边界

- `MaiChatCore/Assistant/AIOutgoingMessageQueue.swift` 只负责每账号逻辑工作区的原子持久化、
  owner FIFO 和 CAS revision；独立的 App `AIOutgoingMessagePump` 调用原 backend 普通入口。
  不增模型代理、第三方出站队列或服务端媒体转发。
- 未派发消息绑定原账号、工作区、session 或 remote slot/login/session。派发时读真实会话状态：
  busy 捕获有效 turn 走 steer，idle 走普通 send 开下一轮。这是用户最新的排队授权；
  不把完全未派发消息因上一轮结束永久卡死。
- 派发绑定及原 requestID 先落盘，再调用 native。明确 `busy` 未受理回到 queued，500 ms 后
  fresh state 再试；`not_found` 仅当原 session 仍存在且路线确实变化时重排。无效文件/输入
  留失败记录，不当作可盲重试的 Busy。
- 送达未知保留原 mode/turn/requestID，挡住同会话后续投递但继续接收本地消息。仅同一 native
  epoch 的 steer 可用原 ID 重试确认；普通 send 没有幂等请求 ID，未知时不自动重播，也不显示
  点了无效的重试按钮。核心保留最近 128 份收据，活动轮另保留本轮全部已接受凭据
  （本轮补充上限 64 条）；结束轮凭据被淘汰后，原 turn 重试可被拒绝，但不自动改绑。
  这些均为运行时缓存，**不宣称跨重启 exactly-once**。
  每次 native configure 更新 epoch，恢复中的 dispatching 转 unknown，不假装属于旧 Agent。
- unknown 和 failed 都阻挡该会话之后的派发；后续点击仍可本地入队，但不能因此宣称已交给
  模型。普通 send unknown 没有自动解锁/无限重试机制，当前也没有单条手动确认/丢弃的 UI；
  不能承诺恢复网络就自动解除该头，或用复制成新 send 掩盖不确定性。现有清空/删除会话会
  显式清理整个 owner 队列，不能当作单条确认按钮。本批未验证真实远端离线、重连和 owner 换登录。
- 媒体描述不含字节/凭据。排队期间保护源文件，屏幕帧为每条消息取得独立 outbox pin，
  并在 native 接受前转历史引用；确定拒绝才回 outbox，未知不撤销可能已属于历史的 pin。
  远端附件的稳定 attachment ID 在真正命令派发前写盘，未知重试不变更请求/重新导入。
- 切会话不改捕获目标，旧确认不清新的输入。普通换号只停止继续派发；旧确认仅可写旧队列。
  清空/删除会话先作废对应 owner 准备代际，成功后移除该会话队列和历史引用。
- 删账号另走 `await AIAssistantModel.invalidateOutgoingForDeletedAccount()`：封旧入口、
  await queue 永久失效，再等待本任务媒体/cleanup/派发结束，主任务才删除工作区。
  actor 集中 commit/write 拒绝迟到 accepted/enqueue，避免删除后重建旧账号目录。

## 可复现验证与当前收据

Debug 模拟器普通 Root 的 `--ai-ui-test --ai-steering-ui-test` 使用隔离队列文件与确定性收据，
不访问真实账号或付费模型；原生工具批次消费顺序另由真实 C ABI 测试覆盖。新 UI case：

- `testBusyComposerOmitsStopAndShowsOnlySupplementWhenDraftExists`
- `testBusyComposerSupplementsCapturedTurnAfterReceipt`
- `testOutgoingQueueAcceptsFiveMessagesBeforeAcknowledgementsAndKeepsFIFO`
- `testOutgoingQueueUnknownReceiptRetriesSameIDAndKeepsRejectedMessage`
- `testOutgoingQueueKeepsNewDraftAndAccountIsolatedAfterSelectionChanges`

五条连发 case 使用 DEBUG 专用显式 ACK gate：首个合成宿主 submit 等待 continuation，
每条仍须本地入队后 1.5 秒内清稿；五条都已本地保存后检查 0 accepted、1 delivery attempt、
实际 outgoingEntries=5，保存排队截图，再点击 QA 放行首条回执。随后检查最后顺序、
5 个独立 request ID（含两条相同文字）及原 turn 不变。没有加大 sleep、放宽原状态断言或
改变生产队列时序。未知确认 case 检查相同 ID 重试只有一条合成接收记录；不拿箭头可用
替代收据验证。
原 `testSteeringArchivePinsSurviveRejectionAndReleaseWithHistory` 已按本地入队回执语义更新，
仍用真实 C++ archive 检查拒绝/未知保图、原 ID 重试不改 manifest、最后历史清理释放原图。

本子任务已完成所有修改 Swift 的 parse/diff check、独立 Pump 的 Swift 6 类型检查。
队友实际 Core store 13/13 typed XCTest 通过，含持久化、同文独立点击、并发入队、unknown FIFO、
旧 epoch/CAS、写盘失败与损坏文件保护、永久失效后晚写不能重建被删目录；
`/private/tmp/maichat-ai-outgoing-queue-validation-20261103/invalidation-evidence.json`。
原 stop UI 已有真实红对照，见同日单按钮诊断。

178 完整 App 的上述 6 个 UI + camera/models 同包实测/录屏由主任务执行；不能把 parse、
Core store 测试或合成 ACK 当作真机验收/真实远端重连验收。
未请求付费模型、未改变用户会话或独立安装 App。

主任务最终日志 `/private/tmp/maichat-batch178-final-ios-build-20261103.log` 已实际
`TEST BUILD SUCCEEDED`（22:40:16）；包含 Controller/helpers/fixtures 的平台类型编译，
首次同包 8 case 的实际结果是 **6 pass / 2 fail**：
`/private/tmp/maichat-batch178-final-ui-20261103.xcresult`，同名 `.log`。
已过：原 turn 补充 14.591 秒、账号/会话隔离 19.563、未知回执重试 18.065、真实 archive
24.120、本地模型 26.705、相机 31.732。首次失败不被后续测试修改抹去。

五条 case 在第二条入队后检查 accepted=0 时实际为 1。日志证明首条点击事件 t=7.30 秒，
第二条清稿/读取 ledger 已 t=11.35/11.41 秒，超过原先固定 3 秒 ACK 延时；第一、二条
清稿断言已通过，不能据此认定生产队列阻挡第二条。改为上述显式 gate，仍须真实五条入队
且全部未确认后才放行，待主任务定点实跑，不先宣称绿。

去 stop case 的聚合四次 delete 后实际仍为“补充细”。保留完整/中间/最终文本断言，改为
focus 后逐次一个 delete，等待每步实际值精确少一字，再检查最终空草稿/no-stop/原任务 busy。
若仍发生回写，应按实际 binding 缺陷继续定位；当前不能只凭测试刺激变更把失败称为假红。
此次只改 DebugSupport 与 UI 测试，生产队列/Model/View 未改，主任务仅重建复测这两项。
复测收据路径为 `/private/tmp/maichat-batch178-two-recheck-20261103.xcresult`、同名 `.log`/`.mp4`；
本文暂不记这两项已通过，等待主任务完成实际结果和录像审阅。

当前已知 P2 边界（主任务明确不在本轮 freeze 后扩改）：native configure 更换 epoch 后，
如果尚无新 enqueue/restore，Model/queue 的 epoch 门禁缓存可暂仍旧，旧 unknown steer 的
“重试确认”按钮可能看似可用。新 core 仍拒绝旧 turn，everUnknown 仍禁止自动改发普通 send，
不会因此自动重播或跨新任务；本批未专项覆盖“unknown 后更改主模型配置”。

最终定点实跑 `/private/tmp/maichat-batch178-two-recheck-20261103.xcresult`、同名 `.log/.mp4`：
上述两项 **2/2 通过，45.148 秒**。已查看实际单箭头与五条排队截图及录屏抽帧；
与首次已过六项共同覆盖全部八项。真实 178 安装、运行 PID 与 Android/Desktop 回执见
[178 交付记录](maichat-batch178-validation-20261103.md)。首次失败及其成因仍保留以上记录。
