# Android 忙时输入、补充与停止边界

日期：2026-11-05。调查源码基线：`2c96439d97433b57ab715aa3562e4db8df4908dd`。

## 操作与预期

1. 创作中心任务运行时输入两条不同正文，依次使用键盘发送或发送按钮；输入框始终可编辑，
   两次操作各自捕获原账号、会话、轮次、请求编号、附件和引用，不触发停止。
2. 点击独立停止入口后继续编辑草稿；只取消点击时所属的任务，草稿和未受理内容保持可恢复。
3. 补充绑定的轮次在派发前结束、停止或更换时，明确保留内容，不自动改为新任务。
4. 从另一设备向 Android 所属会话发送补充，正文及附件复用共享 `MaiSteerPrompt`，并返回
   共享核心实际接受的用户消息编号；服务端仅转送控制消息。

## 根因证据

- Panel 的 `updateSend()` 使用 `!busy` 禁用发送；键盘的 `send()` 在 busy 时调用 `stop`。
- Controller 本地固定调用 `op=send`，远程固定 `action=prompt`；Android 所属端的
  `commandForSync()` 拒绝 `steer`，而共享 MobileAdapter 已支持该操作及轮次/请求编号。
- SessionSyncCoordinator 的 presence 与 publish 遗漏 `turnId`、运行状态未参与 digest；
  command fingerprint 遗漏轮次、请求编号和引用，无法区分同文的不同补充。
- Stop 的 worker 回调重新读取 `selectedRemote`，存在点击后切换会话时路由改变的窗口。
- 本地派发仅把图片和视频写入结构化输入，普通文档/音频路径未加入最终正文；队列保存附件
  不等于模型能访问附件。这项本地遗漏在基线旧 send 中也存在，远程入站原有路径提示。
- Android 的回复引用入口已经位于工具和正文之后；iOS 同样放在底部，且仅消息结束时显示。

## 验证记录

首先新增 `SessionSyncPayloadAssemblerTest` 的真实 fingerprint 回归，分别改变轮次、请求编号
和引用。生产代码修改前交由 root 统一执行 RED。本工作单元不运行 GUI、不停止 App、不调用
真实模型、不读取用户日志或会话数据库。后续验证结果和实机边界由集成验收补充。

## 实现与持久化边界

- Panel 不再把忙时键盘发送解释为停止；发送和 Stop 是独立入口，任务执行期间文字和语音
  输入保持可用。私有队列保存成功才分离该条草稿、引用和附件，后续请求不会误带前条附件。
- 新增两个纯 Java 宿主组件：`AIAssistantSubmission` 捕获所有者、轮次与操作编号；
  `AIAssistantOutgoingQueue` 保存待确认快照。实际工具排队/消费仍使用已有 `MaiSteerPrompt`，
  不增加第二套模型循环。空会话第一次创建后，尚未派发的捕获一起迁移到实际 Session。
- 队列使用每账号私有 SharedPreferences 原子 commit，最多 128 条、单条序列化 128 KiB、
  总共 4 MiB；入口另限制正文 64 KiB、附件 10 个。达到上限或写失败不清输入框。
- 同一 owner 按 FIFO 派发；派发 mode/turn/request ID 先持久保存。已绑定轮次结束、停止或
  变化后保留为未受理条目，不自动改发新任务。明确接受收据带实际用户消息 ID 才移除条目。
  网络失败、缺失正式收据、进程退出时的 dispatching 均保留 unknown，阻挡原 owner 后续
  派发；界面仍能接收新输入。没有自动重放 unknown，也不宣称跨进程 exactly-once。
- failed 可手动恢复；当前编辑器非空时不覆盖新稿。恢复内容先另存私有 recovery 再移除队首，
  下一次入队与 recovery 清理使用同一 prefs 事务。待发条目放在最高 132dp 的滚动区域，
  不随队列数量挤走输入框。账号切换保留各自队列；已接受注销在旧线程关闭屏障后清除。
- Android 所属端接受 steer 并复用首轮媒体导入路径，返回 Core 实际接受的消息编号。
  presence、历史发布及状态 digest 携带 busy/turnId；远程操作指纹区分轮次、请求编号和引用。
- 本地普通 send 与 steer 共用最终请求构造；图片/视频仍为对应输入，文档和音频在正文中
  附带可供共享文件/媒体工具读取的实际工作区路径，保留原文字、引用和操作编号。
- Stop 捕获点击时的原 session/remote owner/turn，派发前核对，不清草稿。跨设备带可选
  turnId；MobileAdapter 在提交原有 `MaiInterrupt` 前校验，旧无 turnId 的调用保持原语义。
  iOS incoming Stop 仅增加该字段转交，不改变其 outbox 或布局。校验后到提交仍存在极短
  状态变化窗口，本轮没有增加 Core 原子取消 API。
- Android 回复引用按钮原本已在全部工具/正文之后，保持该位置和原 active 行为；iOS 是
  相同的底部位置，但只在消息结束后显示。本轮未为此更改 iOS 页面。

## 本机可重复结果

独立目录：`.cache/android-agent-steering-tests-20261105`，没有使用 Gradle、native、Desktop
构建缓存，未启动设备或 GUI、未停止现装 App。

1. 当前生产 Coordinator 源加原两例及新增 fingerprint 回归：javac exit 0；JUnit 4 项、
   2 项失败、exit 1。失败断言分别是不同轮次仍复用同指纹、不同引用仍复用同指纹；见
   `red-compile.log`、`red-junit.log`。
2. 新 binding/queue/retarget 的测试源先于 API 实现，缺失 API 的编译 RED 分别记录在
   `binding-red-compile.log`、`queue-red-compile.log`、`retarget-red-compile.log`。
3. 最后使用本机已有 JDK、JUnit/JSON 缓存、既有 Android resource jar/classes，只读依赖，
   对本轮 Controller、Panel、Coordinator 和纯 Java 组件源码执行 `javac --release 11`：
   exit 0、无警告。三组 JUnit 共 **15 项通过，exit 0**，见 `freeze-java11-compile.log`
   和 `freeze-java11-junit.log`。覆盖写失败不受理、FIFO、未知收据恢复、payload 深复制、
   容量、空会话迁移、稳定轮次、错误轮次 Stop 和远程操作指纹。
4. `MaiMobileSteeringTests.cpp` 新增真实 C ABI 的错轮 Stop 拒绝断言；本工作单元未构建或
   运行 C++/Swift，须由集成阶段执行，不能据 Java 单测宣称该跨端桥接已验收。
5. 派发构造 review 后补真正请求 JSON 的文档/音频回归。使用与 Controller 相同的构造函数，
   先复现省略路径：javac exit 0；JUnit 16 项、1 失败、exit 1（实际 text 缺文档路径），
   见 `documents-red-compile.log`、`documents-red-junit.log`。补路径后同组 **16 项通过、
   exit 0**，Java 11 源码编译 exit 0 无警告，见 `documents-green-compile.log`、
   `documents-green-junit.log`。附件派发构造阶段以这一轮结果为准。

## 附件 I/O 与停止响应

只读复核确认：原 `drainOutgoing` 在唯一 Agent Handler 同步上传；Stop、状态轮询和远端
命令也投递给该 Handler，因此上传等待会延迟停止。底层已复用共享 multipart 核心，但
Session JNI 曾把 cancel 句柄仅放在调用栈中，Java 无法取消等待。

- 新增可注入 control/IO executor 的 `AIAssistantPreparationJobs`。出站上传、入站附件下载
  及远程预览下载使用独立两线程 I/O 池，Core 调用仍回原 Handler；不占 session-sync 的
  outbound 控制通道，不增加 HTTP 或模型请求实现。同一 owner 的准备任务互斥。
- 队列新增持久 `preparing`，先保存后交给 I/O；该阶段还没发送 command，重启后回到
  queued 并保留原 mode/turn/request ID。已经 dispatching 的恢复仍是 unknown，不盲重播。
- I/O 完成回 Handler 后再次校验关闭状态、账号登录代次、native 配置 epoch、队首 owner/id
  与原轮次；停止、配置替换或错误归属的旧结果不调用 Core。普通输入框的新稿不被触碰。
  Core 明确返回的拒绝使用独立 `NativeAgentFailure` 分类，不能把 stopping/finishing 拒绝
  当作网络未知而丢掉可恢复状态。
- Stop 先核对点击捕获的原 owner/turn，再取消对应准备；错轮 Stop 不取消新任务的附件。
  Session 删除/清空也取消对应准备。只有明确未受理的条目进入 failed，来源附件仍由队列保有。
- Session 上传的 Java Scope 暴露现有共享 cancel 句柄；取消可独立发出，句柄只在 native
  返回后销毁。下载复用既有 Scope。I/O 开始前及凭据读取后核对原账号，避免旧任务使用
  切换后的账号发送文件。尚未交给 Core 的随机导入副本只在本准备被取消时清理。
- `close()` 只发出取消并收集 Future，不在 Handler 自等。已接受账号注销的既有关闭屏障
  同时等待旧控制线程和真实 I/O 退出，之后才清私有队列/允许目录清理；超时仍拒绝放行。
  Native 元数据请求本身没有增加中途取消，若仍等待会保留关闭屏障，不能提前竞删目录。

门控验证：先将原控制线程执行形态接到可注入调度器，上传 latch 未释放时 Stop 不能运行；
另一个回归模拟 preparing 重启。`io-gate-red` 的 Java 11 编译 exit 0，JUnit 20 项中
2 项失败，exit 1，分别是 Stop 等待上传、preparing 没恢复 queued。

拆出独立 I/O 后，追加实际 Controller 的账号注销 Future 屏障断言：上传门控期间 Stop 和
新输入控制任务均能执行；同 owner 不重复准备；旧 epoch 完成不派发；cancel 只发一次；
关闭屏障直到实际写入任务退出才完成。最新 `io-freeze-green` Java 11 编译 exit 0，
无警告，**21 项 JUnit 全部通过，exit 0**。本阶段日志位于同一独立测试目录，未触碰 Gradle
或 native 构建缓存。最终冻结以此阶段结果为准。

Session 上传 JNI 的参数和取消句柄入口有变化，必须统一重编 `maichat_agent` 后整包安装；
Java 编译不能验证 JNI ABI。Android native/实机、Swift/MobileAdapter C ABI 仍待 root 集成，
本工作单元没有联网、启动 GUI、安装 App 或调用真实模型。

待集成验证：Gradle 整包、Android 实机连续键盘/语音补充、跨设备附件、Stop/轮次切换、
恢复稿及重启、截图和录屏；iOS 编译/实机。以上尚未执行，未宣称实际 App 验收完成。

## Android 消息真分页与折叠工具呈现

复现与预期：在包含多于一屏历史的创作中心进入会话，首屏仅查询最新 8 条；点击既有
“显示更早消息”按 before 游标查询前 20 条，反复补页能到最早一条。补页应保留已加载历史、
首条可见消息 ID 和行内像素位置；加载失败可重试同一游标，切会话后旧回执不能串入。
正在输出的 assistant 被连续补充输入挤出最新窗口后，其文本/工具完成状态仍须更新。

源码证据：原 Android refresh 未传 messageLimit，MobileAdapter 默认 -1 会读整个历史；
Panel 的 visibleLimit 只截取已经完整序列化的数组，点击更早并没有查询下一页。
既有原生 messages_page、remote readRemoteHistory 和 Core listMessagesPage 可复用。

- AIAssistantMessagePages 使用最新 8/旧页 20，按稳定 ID 合并，重复记录不会丢掉已加载旧页。
  每次切 owner 生成独立代次；读失败不推进 before。清空明确重置代次。
- Controller refresh 显式请求有限页；普通本机补页走 messages_page，跨设备补页仍走现有
  readRemoteHistory。远端首条广播先于初始历史的竞态会保留最新消息，同时校正更早入口。
- MobileAdapter 仅 Android opt-in 记录该 session 的 dirty message IDs，每次最多精确查询
  20 条并返回 messageUpdates。summary snapshot0 不消费该游标，不传随历史增长的 retained IDs。
  iOS 未启用该选项，其原有 8/20 分页与布局不变；不改变 MaiAgent 生产代码。
- Panel 使用真正已读取的页；前插按 ID/像素还原，而不是跳到列表尾部。
  只读工具卡片 JSON 使用 64 项、256 KiB UTF-16 源文预算的有界缓存；更改输出会重新解析。
  普通折叠 shell 原始正文并不在 bind 时解 JSON，此次也未将其强行解析或展开。

独立 Java 11 验证：初始缺页组件为编译 RED；另用实际断言重现远端 broadcast 先到导致更早
入口丢失，pages-broadcast-red 编译 exit 0，27 项中 1 失败、JUnit exit 1。补充工具缓存
定点断言后，最新 pages-freeze-green 编译 exit 0、无警告，**29 项 JUnit 全通过、exit 0**。
日志均在 .cache/android-agent-steering-tests-20261105，不使用 Gradle/native 构建缓存。

MaiMobileSteeringTests 新增真实 C ABI 的最新八条上限、十次 steer 推出原 assistant 后仍接收
完成状态、旧页上限及独占 before 游标直到最早记录等断言，沿用已有环回假模型 fixture。
本工作单元没有构建/运行该 C++ 测试，也没有执行设备/GUI/网络/真实模型验证；由 root 统一
执行 native/Gradle 和实际列表滚动验收，不能以纯 Java 通过宣称实机体验已完成。

后续独立审查复现连续页游标缺口：首屏 013..020、dirty update 插入活动消息 005 后，原
loaded.firstKey 会错误以 005 查询更早，跳过 006..012。pages-cursor-red 编译 exit 0，
31 项中两项失败，实际游标为 005、预期 013/041；改为独立 oldestPageCursor，仅初始连续页
与真实 older 页推进，不由 dirty updates 改写。pages-cursor-green 的 31 项全部通过。

## 明确 user/turn 回执与原轮次绑定

原 Android local outbox 与远端 host 都直接将 native 的兼容 id 命名为 user_message_id，
但普通 send 的 id 实际是原 assistant turn ID。现统一只消费明确 user_message_id 和 turnId；
steer 回执还须匹配原请求 turn。缺字段或错轮成功回执保留为 unknown，不能当明确拒绝重发。
所属设备通过同一规范化入口回传字段；附件装饰采用已批准的普通 op=message/session/id 精读，
当前权限与 busy 元数据单独取有限 remote_snapshot，不从后来运行态推测原 ACK 轮次。

队列确认在一次持久事务中移除已受理队首，并将同 owner 已排队、尚未绑定的尾项固定为
该 receipt.turnId 的 steer；requestId、正文、附件与 quote 保留。原轮结束后会明确拒绝并留稿，
不会自动改成新 send。事务写失败不部分移除或绑定。

canonical-receipt-red 对新增规范化/确认契约编译 exit 1；新增四项回执定点用例覆盖普通 send
的 user/assistant 区分、缺字段与错轮 unknown、真实 Coordinator host 透传、持久 FIFO 原轮
绑定与写失败保留。最新 receipt-markdown-freeze-green 用本机现有 Markwon/CommonMark 缓存，
连同实际 Renderer/MarkdownContentView 和 Panel 源码执行 Java 11 编译 exit 0，35 项 JUnit
全部通过、exit 0；Renderer 有既有 deprecated API 提示。并未运行 Android GUI 或真实模型。

Panel 的 assistant text 接入共享 MarkdownContentView；同一正文片段保留容器，普通段落和
代码块使用共享组件提供的各自布局/横滚。原始复制仍读 part source；用户气泡、工具摘要和
展开详情继续沿用既有 TextView 路径。真实文字宽度、原生选择/复制、流式更新保选区及横滚
必须由 root 的 Android 图形/实机验证，不能据本阶段 JVM 通过宣称视觉验收完成。
