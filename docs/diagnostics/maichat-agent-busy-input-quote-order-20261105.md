# Agent 忙时输入和引用位置修复计划

## 需求与可复现步骤

源码基线为 `2c96439d`。只使用本地假模型和门控工具验证，不调用供应商或中止正常 App。

1. 发起任务，让工具等待；连续输入并发送 B、C。
   预期输入始终可编辑，B/C 各受理一次、按顺序保存，当前工具不被取消。
2. 工具运行时编辑草稿，点击独立 Stop；在工具取消收尾期间尝试发送。
   预期 Stop 只终止点击时的当前任务，未受理内容和附件/引用留在草稿；
   允许继续编辑，当前轮结束后不自动把该草稿发成新任务。
3. 中间正文 A、工具 X 后切换到别的会话再切回，随后到来正文 B、工具 Y、正文 C。
   预期每条消息的引用控件始终在所属消息最后一个可见片段之后，控件不销毁重建。

## 已确认根因

- Desktop `AgentChatPanel::onSend` 在 running 时直接 interrupt，把键盘提交当作停止。
- `AgentController::sendPrompt` 固定提交 `MaiSendPrompt`，而 Core 默认拒绝忙时新轮。
- Core 已有 `MaiSteerPrompt`：稳定 turnId/requestId、接收后持久化真实用户消息、去重，
  在完整工具批次边界消费；它不会取消正在运行的工具。
- 引用在恢复未完成 assistant 消息时过早加入。后续片段只 append，已存在引用不移动。

## 已认可的最小方案

- 输入/发送与 Stop 分离；发送不再隐式停止。
- 忙时轻宿主适配到既有 Steer，idle 才提交 Send，不修改 MaiAgent 核心。
- 绑定并校验 turnId/requestId；finishing/stopping/轮次变更拒绝时保留未受理内容，
  不自动降级为新轮 Send，不自动重发未知接受状态。
- iOS 保留已有持久 outbox、绑定和未知状态保护。
- Android 在自己的宿主链补发送接收、轮次状态及稳定 ID，附件按入队快照归属。
- MarkdownView 的对象保留顺序 API 由另一代理统一实现；Panel 只记录消息可见片段顺序，
  将所属引用移动到最后一个有效 anchor 后，避免重建整个历史或混入其他消息。

## 分工及验证

- 本代理：Desktop Controller/Panel 与 `AgentPanelSessionTest`。
- Android 子代理：Android Panel/Controller/Coordinator 与纯 Java 回归；不改 Core。
- Markdown 代理：`moveItemAfter(id, anchorId)`、对应测试及同模块已有复制问题。
- Root：统一构建和实际 GUI 验证；本代理不并行占用 Desktop build cache。

先交付以下 RED 测试源，再由 root 执行并记录结果后写实现：

- `busyComposerAcceptsOrderedSteeringWithoutStopping`
- `stopKeepsUnacceptedDraftAndInputEditable`
- `quoteActionFollowsStreamedOwnerAfterSessionRestore`

边界回归还需覆盖稳定请求重试、会话切换、停止收尾、已完成/流式工具和媒体引用位置。
实现和实际结果将在后续记录；当前本文不宣称测试或实机通过。

## Desktop 已执行 RED 与实现交接

Root 使用独立测试进程实跑上述三槽：

- `busy-steering-red-cdb.log`：发送 B 后编辑器仍保留 B，没有受理补充，断言失败。
- `stop-draft-red-cdb.log`：独立 Stop 控件不存在，断言失败。
- `quote-owner-red-cdb.log`：后续工具 Y 已出现，但引用仍在其之前，几何顺序断言失败。

完整日志位于 `.cache/desktop-player-style-20261105/`。三者均是行为失败，不以编译缺失代替。

随后实现已交接 root 统一 GREEN：

- `AgentController::steerPrompt` 是显式普通入口，复用同一媒体校验与既有 Core Steer；
  原 `sendPrompt` 仍代表新一轮，拒绝补充后不降级。
- Panel 保留键盘发送与输入，独立 Stop 不清稿；Core 同步持久受理成功才清内容、附件和引用。
- owner 可见 item 独立登记，新片段与现有引用通过对象保留的顺序 API 重排，
  不销毁引用控件，不在流式期间重建历史。

Root 已执行三槽 GREEN：busy 787 ms、stop 913 ms、quote 911 ms，全部通过。
这是独立本地 Qt 测试进程的结果；当前本机未重新安装/启动正常 App，不代替跨端实机验收。

## Remote 与历史分页后续

Remote 两条行为 RED 已由 root 执行：

- `remote-queue-red-cdb.log`：忙时提交实际发送 Stop，预期 Steer，失败。
- `remote-draft-red-cdb.log`：旧请求传输完成后新编辑的草稿被清空，失败。

随后 Remote 实现以 owner 的 slot/login/session 和稳定 requestId 保存私有队列，
写盘成功后才分离当前编辑稿。Stop 单独编号并越过消息 FIFO；正式接受回执必须精确匹配
owner、command_id，并包含真实 user_message_id。同步回执先于传输 completion 不改新稿。
首次 Send 后、presence 还未到达的连续输入依赖原请求，只有所属端明确回复原 turnId 后才派发 Steer。
重启/未知接受状态不盲重发，不自动创建新任务；缺关键状态保留原请求和稿件。
默认 spool 路径的本地账号隔离由 root 的 MainWindow 集成完成，不能把 owner 绑定等同于本地账号分区。

分页的产品复现为：打开含 121 条历史的会话，首屏只创建最新 40 条；
点击“加载更早消息”每次补 40 条，当前可见消息的像素位置和既有引用控件对象保持不变，
最早消息最终仍能加载。另有单条消息含 55 个长工具结果的折叠详情回归；
折叠时不格式化隐藏详情，展开单项时才生成有界纯文本显示，完整原数据仍在存储中。
两条 RED 已由 root 实跑：`agent-latest-page-red-cdb.log` 在首屏仍包含第 80 条而失败，
`agent-lazy-tool-red-cdb.log` 在未展开详情已有文本而失败，分别耗时 1117 ms、3899 ms。
随后分页与详情延迟格式化实现交 root 统一 GREEN，本文尚不宣称其 GREEN 已执行。

Remote 五槽已由 root 运行 GREEN：queue、draft、owner-durable、idle-burst、storage 全部 exit 0，
日志同在 `.cache/desktop-player-style-20261105/`。
额外只读审查发现旧 Steer 回执晚于新轮 presence 时不应回退本地 owner 轮次，
已修为仅无轮次的首次 prompt 回执可回填，并补对应稳定身份回归；该新增槽待 root 统一运行。

## 精确读取单条消息的 Core 契约

为避免界面为一次引用/工具更新扫描全部历史，新增只读
`MaiSessionStore::getMessage(sessionId, messageId, out)` 及 `MaiAgent` 普通 façade。
Memory 和 SQLite 均按会话与消息精确身份查询，查无记录、跨会话或失败时不修改 out。
SQLite 只加载匹配消息的片段；不使用 ID 前缀或游标拼接模拟精确查询。
Store 新虚方法追加在既有方法之后，需要完整重编所有消费者；公开 C ABI 未改变。

先加入 Memory/SQLite 共用契约测试及 façade 测试，再用独立 MSVC `/Zs` 执行：

- RED：两测试均 exit 2，明确缺少 getMessage，而非 include 或环境错误。
- 实现后同一命令：两测试均 exit 0。

命令、退出码和编译输出位于 `.cache/agent-exact-message-20261105/`，
分别为 `red-syntax-commands.json`、`green-syntax-commands.json` 和对应日志。
Root 随后执行全新 739 步构建 exit 0，Core 八套测试 8/8 通过、79.94 秒。
完整日志为 `.cache/maiagent-core-gate/followup-build.log` 与 `followup-tests.log`。

## 原子中断指定轮次

Desktop 普通控制和定时器都在同一 Qt 主线程；移动宿主则存在独立的 `mai-schedule` 线程，
其 poll 可直接提交新一轮。因此 Android Handler 的串行性不能保护
“先 activeTurnId 检查、后按 session 中断”之间的窗口。
共享 Core 的 `interruptTurn(sessionId, expectedTurnId)` 在同一 Runtime 锁内比较原轮 ID、
置 cancel 并唤醒审批/提问；旧 `submit(MaiInterrupt)` 转发同一实现，空 ID 保留原有语义。
两 gate 的 cancelSession 仅取得自身锁与 notify，不回调外部代码；ask 广播也在 gate 锁外。
该锁序避免旧轮唤醒操作落到刚接替的新轮。

新增五条真实本地门控用例覆盖错轮拒绝、正确取消、旧轮完成后不取消新轮、
权限/提问等待的错轮保持与正确唤醒、空 expected 的兼容。
先执行独立 `/Zs`：exit 2 明确缺 interruptTurn；实现后同命令 exit 0。
证据在 `.cache/agent-target-interrupt-20261105/`，此阶段仅为语法 RED/GREEN；
新原子实现仍需 root 再跑完整 Core 门禁及移动 native 构建，不能借前一版 getMessage 门禁代替。

Root 的首轮原子门禁为 7/8：基础错轮/正确轮及下一轮保护通过，两个 pending fixture 未进入门禁。
定位为 fixture 建会话时未提供工作目录；`MaiTurnRunner` 会在工具执行/审批前拒绝空 root，
因而不能拿“没有 pending”推断取消实现失败。仅将两个 fixture 改为当前目录解析后的真实工作区，
未降低等待、错轮保持、相同 pending ID、未执行工具及正确中断唤醒的断言。
随后独立构建 `MaiCoreTests` 两步 exit 0，执行输出 `all tests passed`、exit 0；
证据为同目录 `workspace-fixture-commands.json`、`workspace-fixture-build-correct-toolset.log`、
`workspace-fixture-tests.log`。这次编译明确使用与 Core cache 一致的 VS2022 14.44 compiler、INCLUDE、LIB；
此前误用 Desktop 环境造成的标准库链接失败保留在 `workspace-fixture-build.log`，不当作行为回归。

## 手机所属端的真实接收回执

普通 Native `send` 的旧 `id` 是新 assistant/turn ID，而 `steer` 的旧 `id` 是受理的 user ID。
Android 所属端和本地 outbox 先前统一把旧 `id` 当 `user_message_id`，会把普通发送的 canonical ID
错误指向 assistant。Desktop 的 idle 连发队列又需要原 turnId，而手机所属端回执未提供该字段。
本轮保留旧 `id`，追加明确 `user_message_id` 与原 `turnId`，由 C ABI 角色/原轮稳定测试验证；
宿主消费此收据，不从之后的 active turn 或最近两条消息推断身份。
旧 user ID 的附件装饰改由普通 Native `message(session,id)` 精确查询共享 Core `getMessage`，
不使用 `id + "~"` 游标技巧或全历史扫描。此节记录已确认缺口及实现方向，Native RED/GREEN 待 root 执行。

Root 已在 OPPO arm64 运行 Native receipt RED：C++ 121 步构建成功，设备测试 exit 1，
明确失败为 `send receipt did not identify its original assistant turn`。
证据为 `.cache/android-agent-steering-tests-20261105/native-receipt-red-*`，
测试依赖推送到私有 `/data/local/tmp`，没有停止或替换正常安装的 App。
实现后设备已通过新字段前置断言，随后暴露旧 fixture 的固定下标假设：
加入 10 条分页补充后，最后一条 user 之前应是第 10 条 user，而非 tool result。
已改为按唯一 `synthetic-profile` 识别调用/结果并强制相邻，
另外逐条断言 11 条补充都在完整工具对之后且顺序正确；
保留原最后 user、总请求数 2、host 执行 1 次的断言。只打印本地 fixture 的 role 序列，
未删除工具调用与结果的原子性约束。最终设备 GREEN 仍由 root 记录。

Swift 仅接明确收据、精读和 unknown 标志，不改已有 outbox actor。
当前 Windows 环境没有 Swift/iOS 编译器，Swift 编译与 iOS 实机属于未验证边界。

## 并发审查补充

后续审查发现 `forwardSpecialistReply` 会先在 Runtime 表中预占一个空 ActiveTurn，
然后只持 steeringMutex 初始化它的 id；因此不能把“普通 Send 在发布前写 id”
推广为所有路径的不可变发布保证。`interruptTurn` 的非空目标比较已改为与
`activeTurnId` 相同的 Runtime → steering 锁序。空 expected 不读 id，保持旧取消预占轮次语义；
比较后释放 steering，但 Runtime 继续覆盖 cancel 与两 gate 唤醒。
源码已加 writer/锁序注释；Windows 没有运行 TSAN，普通功能门禁不冒充数据竞争检测。
此生产修改需要最终 Core 门禁和 Native 直接消费者重编，不能复用修改前产物结论。
