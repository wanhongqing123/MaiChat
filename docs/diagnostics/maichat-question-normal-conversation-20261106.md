# Agent 提问统一使用正常正文和输入框

## 用户要求与操作

- 基线：`4e7fc86a08264819450f65610efd0fc319d89fdb`，Windows 已发布 2.2.50。
- 用户要求全平台核对模型向用户提问的交互：问题和建议选项直接进入正常文本流；不弹选择框，不提供第二个答题输入框；正常输入框的下一条文字就是回答。
- 操作：本机或远端会话运行到 `question` → 正文出现问题/选项 → 在原输入框发送文字 → 回答原问题并继续任务。覆盖会话切换、重连、子任务提问、连续发送、过期回答及附件草稿。
- 不调用真实供应商、不读取用户聊天；使用 Fake model 和独立临时数据库验证。权限批准属于独立协议，不能被普通文字回答替代。

## 入口统计

| 平台/入口 | 原呈现 | 原正常输入 | 需要统一 |
| --- | --- | --- | --- |
| Qt 桌面本机 | 灰色居中临时 Notice，选项拼成一行 | 能回答缓存问题 | 正文重放、真实 owner/先后顺序、原位回答展示 |
| Qt 桌面远端 | 独立 QLineEdit + 回答按钮，刷新时重建 | 走 send/steer | 去掉第二输入框，明确 answer 回执与队列绑定 |
| Android 本机/远端 | 问题卡、选项 Button、独立 EditText/回复 | 走 send/steer 队列 | 正文问答、原输入回答、失败保留草稿 |
| iOS 本机/远端 | AIQuestionCard、选项 Button、独立 TextField/Reply | 走 send/steer 队列 | 正文问答、原输入回答、失败保留草稿 |

iOS 源码未找到 `question` 专用 sheet/confirmationDialog；现场记忆中的选择操作对应独立问题卡和选项按钮。本轮移除该答题卡，不把普通输入换成另一种弹窗。

## 统一契约

1. Core `question` 工具和 `MaiReplyQuestion` 保持不变。已持久化的工具 `input` 保存问题/选项，Completed `output` 保存实际回答。仅展示层把它们绘为普通正文及用户回答，不再另存一份 Core 用户消息或重复回灌模型。
2. 普通发送在入队时捕获当前所属会话最早的待回答 ID（asked/id 顺序），并预留该问题，避免连续文字重复回答同一 ID。没有待回答问题才按原 send/steer 流程处理。已捕获的失效回答不能自动改发为新任务。
3. 所属设备校验 questionId 属于所选会话或其后代，返回显式 `question_id` 成功回执；不伪造 `user_message_id`。失败/未知保留可恢复输入。
4. 回答只消费文字，附件/引用继续留在原输入草稿中。缺少文字时不默默丢弃附件。
5. 问题 DTO 补 sessionId/messageId/partId/asked；远端保留 question 工具必需的 input/output。子任务问答用独立 `questionMessages` 展示投影，真实消息/片段 ID，禁止把它当本会话可引用的 Core 消息；root 历史分页和 cursor 不变。
6. 提问与回答使用真实 part ID 派生展示键，历史、当前 pending fallback、已确认回答的临时回显互相去重。确认回执先于持久快照到达时，不让刚发出的回答闪消失。
7. Session Sync 服务保留独立问答投影状态；不混入 root messages 表或改变分页计数。新字段持久化迁移需兼容旧数据和缺字段请求。

## 验证与交付计划

- Qt：普通输入/回执/正文顺序/缓存历史/子任务归属、过期与附件保留，实际截图；正常 `maichat` 构建、windeployqt 和 PE 检查完成后再跑 GUI。
- Android：行为回归、正常 JVM 全套、共享 native 和受控设备普通页面流程；不替换用户正常 App。
- iOS：源码及 Swift 测试按 Windows 环境可用工具验证，缺 Xcode/设备时明确记录未实机验收。
- 服务：字段与迁移先做 RED/GREEN，验证 history/event/restart 保留投影且不改变 root cursor；线上部署按服务 README 和现有部署流程核对。
- 交付时绑定提交、安装/发布产物和运行构建，说明各平台实际运行边界。

## 当前验证与边界（2026-11-06）

- Qt 本机定点 RED：旧实现没有 `partId-question` 正文条目；修改后本机面板全套 67/67，通过原输入回答、冷缓存回放、子任务附件保留、父消息前后片段间的子任务问题顺序、停止/外部回答/失效草稿。
- Qt 远端定点 RED：旧独立 QLineEdit 仍存在、缺少子任务问答投影。已实现按真实片段分段、明确 question_id 回执、失败项恢复时仍绑定原问题；最终回归在收敛中。
- Shared C ABI 在 OPPO PKD130 Android 15 私有 /data/local/tmp 运行：归属隔离、媒体拒绝、question_id 回执、陈旧 ID 拒绝、子任务投影、root 分页不混入 child、独立远端历史缓存均通过。未替换生产 Android App。
- 服务本机 Node 24.14、私有依赖 NODE_PATH：session-sync / HTTP / WebSocket / account purge 共 27/27 通过；包含旧表自动增加 question_messages、进程重启保持记录、状态事件、无字段旧客户端、重复/超量投影拒绝。
- iOS 当前机器无 Xcode/xcrun；只运行源码契约检查，Swift XCTest 及 iOS/macOS 实机尚未验收。
- 服务只读预检：旧本机 SSH alias 指向另一台服务器；当前 ichat.life 的 SSH 主机校验未通过，未接受未知主机指纹，未改动线上进程。升级独立同步服务的入口已向用户询问。部署需一致 SQLite 备份、运行文件备份、迁移/health/quick_check 与回读哈希；回滚不删除新列、不回滚用户新消息。

本机日志和截图在 `.cache/question-text-flow-20261106/`；平台日志在对应 android/ios question cache，均为受控替身数据，未使用真实供应商或读取用户历史。

## 最终源码回归

- Windows Release 2.2.51：maichat/windeployqt/95 PE 检查已通过；本机面板 69/69，同步 Host 与远端页面 42/42。远端旧 thinking QLabel 延迟销毁时未立即 hide 的截图问题，追加可见控件数量 RED→GREEN 验证后修复。
- 同步完整帧预算用真实超过 95 KiB 的组合得到 RED；修复只裁展示投影，消息帧全部确认后补最新完整小状态，覆盖两代消息回执倒序与旧代失败。
- 根消息出生较早而后续 part 较晚时按真实 part 时序显示，包括本机 root 补充输入、子任务问答和三端显示分段；引用在消息尾，子任务投影不能冒充 root 引用。
- 最新页之外的活动原消息用事件 ID 精读，每批最多 20 条；C ABI remote_snapshot 仅 producer 显式 opt-in 消费独立远端更新队列，不抢本机队列或普通 runtime 查询。宿主缓存已消费但未成功发布的页，未知旧 ID 进入最多 20 条显示旁路，不改变连续分页游标；历史实际补到该页后接管。
- Native 最终 OPPO arm64 七组全部 PASS，包括真实 SSE 持流期间追加 21 条 steer 后的旧尾正文、question 输出及终态；最终日志 native-shared-final-old-window-green.log。64 历史 owner 窗口和独立非 delta 缓存各有定点 RED→GREEN。
- Android 普通 JVM 最终 473/473，正常 instrumented Java 编译通过。最终隔离 UI 包不含未使用的 GGUF，仅用于本轮 HTTP Fake 问答页面验证；完整 1.49 GB APK 首次安装超时，不计完整安装验收。生产 App 未卸载、替换或停止。
- iOS question 源码契约 5/5、原片段顺序检查 6/6，总计 11 项通过；Swift XCTest/iOS/macOS 编译和实机未运行。Windows 2.2.51 不包含 iOS/Android 分发产物。
- 本轮未更改 MaiAgent Core，九套 Core 测试沿用同日已跑完且源文件/测试程序哈希完全一致的证据，不伪造新的执行时间。

最终提交和 Windows 安装包以交付信息及 release-manifest.json 为准。跨端完整子任务问答需要相关客户端和独立同步服务一起更新；线上服务目前仍未部署，等待已验证的 SSH 入口。
