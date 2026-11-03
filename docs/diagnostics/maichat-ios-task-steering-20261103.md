# iOS 当前任务补充与跨设备控制面

- 时间：2026-11-03，Asia/Shanghai。
- 源码基线：`99594888`，下述结果对应其上的未提交工作区修改。
- 范围：iOS 创作中心输入栏、本地 MobileAgent 普通调用、iOS/Desktop 所属设备执行端、
  Session 同步服务控制字段；核心调度实现及 C ABI 用例另见
  [核心任务补充记录](maichat-core-steering-20261103.md)。

## 可复现操作与预期

1. 创作中心已有模型/工具任务运行时，空输入栏只显示停止动作；输入文字或保留媒体草稿后，
   主发送按钮可用，无障碍说明为“补充当前任务”，停止仍是独立动作。
2. 发送补充时捕获原 Session、账号/选择代次、工作区、`turnId` 与 `requestId`。
   当前工具不被取消，完整批次结束后核心在下一次模型请求前消费补充。
3. 接受成功必须包含已落库的 user 消息 ID，之后才能清除对应草稿和捕获的附件。
   等待期间切换 Session/账号，或核心在收尾、停止、已结束时拒绝，保留草稿。
4. 未确认投递可重试相同内容，复用原轮次与操作编号；不能自动改为普通新任务。
   已接受但切换页面的回执保留在内存，返回原会话明确重试可以确认，避免重复写入。
5. 远端忙会话必须提供 `turnId`；旧所属端缺少编号或不支持 `steer` 时明确失败。
   服务信箱的入队 ACK 不代表所属 Agent 已接受。

## 关键边界与实现

- 本地普通 Swift actor 方法提交 `AISteeringRequest`，MobileAgent 请求使用
  `op:"steer"`；协议不涉及服务端代理模型请求。
- 远端使用 `action:"steer"`，JSON 字段 `turnId`、`requestId`、`text`、可选附件引用和
  `quoteMessageId`。所属端使用同一核心普通 API，返回
  `{ok:true,user_message_id:...}`。新的信箱投递编号与稳定的核心操作编号区分开，
  允许回执丢失后重投；核心负责避免重复落库。
- 重试的附件引用及导入路径必须稳定。iOS 所属端有界缓存导入元数据；Desktop
  路径前缀绑定账号、登录、会话、轮次和操作编号，复用已有路径前按流式 SHA-256
  核对内容并拒绝符号链接。异步附件解析完成后再次核对账号、登录及 Agent 实例。
- Session `presence`、`publish`、`history` 和目录同步 `busy`、`turnId`。
  SQLite 自动迁移新增空默认值的 `turn_id`，不修改现有消息内容。
- 此次不扩改 idle 普通远端 `prompt` 既有的乐观发送/入队确认策略；真实落库回执要求
  应用于新增的 busy 补充路径。生产同步服务尚未部署本协议。

## 本地验证与边界

- macOS Qt 5.15.18，`/private/tmp/maichat-desktop-agent-window-build-20261102`，
  仅构建 `desktop_session_sync_host_test`，两个编译任务；未运行或替换真实 App。
- `QT_QPA_PLATFORM=offscreen`：完整 `DesktopSessionSyncHostTest` **26/26 pass**。
  新增持有本地 fake model 的实际 Agent 用例验证原轮次绑定、busy 不被普通 prompt
  替换、重复回同一 user ID、结束后的已接受请求重试以及新请求 idle 拒绝。
  日志：`/private/tmp/maichat-steering-desktop-build-20261103.log`、
  `/private/tmp/maichat-steering-desktop-tests-20261103.log`。
- Node 本地测试：`session-sync.test.js`、`session-sync-websocket.test.js`、
  `session-sync-server.test.js`，**23/23 pass**；新增 turn 传播、owner 回执、
  不合法补充拒绝、旧数据库迁移用例。
  日志：`/private/tmp/maichat-steering-session-sync-tests-20261103.log`。
- Swift 相关正式文件与 DebugSupport/UITests 仅完成 `swiftc -frontend -parse`，
  `git diff --check` 通过，尚未由本任务执行 iOS 编译、模拟器或真机验收。
  最终统一构建/安装由主任务负责，不能将语法通过称为 iOS 体验验收。
- 已加入正常 `RootView` + 真实 Composer 的隔离 fixture：
  `--ai-ui-test --ai-steering-ui-test`。延迟 900 ms 的本地后端接收完整普通请求，
  返回 user ID 或明确拒绝/未知回执；不调用付费模型，不使用真实账号。
  UI 用例：`testBusyComposerSupplementsCapturedTurnAfterReceipt`、
  `testSteeringUnknownDeliveryReusesRequestAndRejectionKeepsDraft`、
  `testSteeringSelectionAndAccountChangesKeepOriginalDraft`。
  它们检查真实提交的轮次、稳定重试编号与落库结果，核心工具批次顺序由 C ABI 用例验证。
- 尚未完成真实两设备、生产 WSS、iOS 真机及 Android 所属端 steering 的联合验收。
  不启动生产服务、不修改真实账号、不发送付费模型请求。

## 174 iOS 定点 UI 初跑与 harness 修正

- 主任务运行 174 模拟器构建：批次 8 项中 7 pass，仅首个 busy Composer 用例未进入
  创作中心；另两项 steering 用例通过。结果包
  `/private/tmp/maichat-batch174-ios-ui-20261103.xcresult`，日志同名 `.log`，
  attachments 导出至 `/private/tmp/maichat-batch174-ios-ui-shots-20261103`。
- 证据：已实际查看失败录屏 `78D43D7B-464D-49E6-B974-C5387C56C251.mp4`
  的 5.9 s 和 10 s 帧。5.9 s 为正常竖屏，“开启消息通知”预提示覆盖页面，
  底下创作中心 tab 仍在 AX 中；日志只等待 `exists` 后 tap，计算命中点为 `{-1,-1}`。
  10 s 时预提示被这次点击背景关闭，但停留在“消息”页，未真正点击 tab。
  Root 的关闭动作写入 `maichat.notification-preprompt-dismissed`，后续用例不再被挡。
  失败 AX `A16B07DC-657D-4B0B-8095-F88891DB3552.txt` 同样显示消息 tab 已选择、
  没有 Composer；不是图片旋转或补充请求失败。
- 定点帧：`/private/tmp/maichat-steering174-failed-tap.png` 与
  `/private/tmp/maichat-steering174-failed-late.png`，仅用于本地排障，不加入公开截图。
- 修正仅在测试 helper：通过启动参数的 `NSArgumentDomain` 把通知预提示标记覆盖为
  `YES`，不持久修改用户设置；点击前等待 tab 实际 `hittable`，首个前置断言失败立即
  停止，避免继续点击不存在的 Composer 产生级联误报。不改产品 Root 或业务路由。
- 本次修正仅 parse/diffcheck；未重新构建或操作设备，待主任务重建并定点回归三项 UI。

## 交付审计后的附件修正（待 175 验证）

- 主任务重建后的 174 定点三项 steering UI 已全部通过；通知引导修正有效。
- 源码审计发现本地补充缺少普通发送的屏幕帧历史引用准备：原先直接接受后删除 pending，
  归档的 `usage_session_id` 仍为 `draft:UUID`，正式会话 `release_session` 无法释放。
  已复用 `prepareScreenFrameHistory`，只准备尚未持有的引用；明确未派发或原生明确拒绝时
  恢复 draft 归属，未知投递保持 history pin，避免清理已经接受的像素。
  换账号或已移除的草稿在恢复时按既有引用释放语义处理。
- 所属 iOS 原先从 `remoteSnapshot(limit:2)` 中寻找 receipt user；等待中另一次补充或
  assistant 分段可把该 user 挤出窗口，跳过附件 ID 装饰。已新增普通 actor 查询
  `userMessage(session:id:)`，按确切 user ID 加 `~` 的排他游标读取一条，并核对 ID/role，
  不依赖最新两条；首次导入的名称、类型与 MIME 一并缓存，重试不改附件装饰身份。
- 新增 UI 用例 `testSteeringArchivePinsSurviveRejectionAndReleaseWithHistory`：真实 sampler
  与 C++ archive 生成/保留原图，通过正式 `model.sendMessage` 补充路径检查明确拒绝后
  清理不会误删草稿，未知回执重试保持操作编号、相对路径与 manifest 字节，接受后正式
  `release_session` 确实删除已 discard 批次中的原图。模型接受/拒绝仍用本地隔离后端，
  不伪造归档实现；测试入口加 `--ai-steering-archive-ui-test`。
- `MaiMobileSteeringTests.cpp` 补确切 receipt user 的 `messages_page` 游标断言，后面已有
  assistant 时仍应取回原 user；此小段由核心任务明确移交编辑，不改核心产品文件。
- 此次仅完成相关 Swift parse 与 `git diff --check`，没有构建、设备或付费请求。
  174 已过的产物不含本节正式修正，须主任务 175 重建并验证；新增 archive UI 与 C ABI
  小断言尚未运行，不能沿用旧测试结果声称覆盖它们。

## 有界导入收据与未知原生回执

- 移除导入缓存满 128 条时整体清空的行为。空附件请求不占导入缓存。
  每条导入记录保持原 Session/turn/附件元数据，以及首次原生接受的顺序；重试不推进顺序。
- 保护最近已确认接受的 128 个远端附件操作，以及仍在活动轮次/状态未知的旧附件操作。
  这两组可能不重合，不能声称 128 个总槽覆盖它们；当前总上限为 256，活动/未知项
  准入上限 128，并且只允许一个未确认新导入，计入总上限。
  已有未确认操作可按原编号重试，新的不同附件请求明确拒绝，不挤掉旧收据。
- 达到核对阈值时按 distinct Session 调用真实 `backend.remoteSnapshot` 读取 busy/turnId，
  不使用 UI 心跳缓存；失败/缺失快照保护旧记录。每次 await 后核对账号/缓存 revision，
  最终再核对所属登录，变化重查或拒绝。只可淘汰已被证明结束/换轮、且不在最近接受
  128 条窗口中的旧项；不会因新请求最后被核心拒绝而损失原最近收据。
  原生明确接受后才标记接受序号；明确拒绝删除该未接受的新导入记录，未知回执保留。
  原生确认删除 Session 或退出账号会使对应运行时收据失效，再移除相应内存条目；
  不以缓存淘汰删除历史文件。
- Swift 草稿尝试表也改为达到 128 条时拒绝新操作，保留所有原请求编号，不整体清空。
- `maiMobileAgentRequest` 返回 nil：仅 steer 视作投递未知，复用原 requestId 和 history pin；
  其他操作维持既有错误语义。所属 iOS 将未知回执标为 `delivery_unknown`，发送端据此
  保留操作编号；成功仍必须含真实 user 消息 ID。
- 普通缓存策略从产品源直接提取、使用同一 DebugSupport 断言独立运行：
  `/private/tmp/maichat-steering-import-cache-policy-20261103.swift`，日志同名 `.log`，
  PASS。覆盖全活动/缺失状态保护、旧轮次与最旧淘汰、最近 128 条保护、新操作未接受
  不推进窗口、旧活动项与最近收据并集。该断言也在新增真实归档 UI case 开始运行。
  未构建 iOS 或操作设备；最终类型编译与四项 UI 仍由主任务 175 验证。
