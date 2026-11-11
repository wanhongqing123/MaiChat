# 远端工具参数、输出和错误的只读展开

## 需求与复现

- 基线 `04890bb7f0162f43967602f668bf4c1b631a56ff`，2026-11-11。
- 同一账号在另一设备查看正在执行或已完成的会话。工具组能看到完成状态，但不能像所属设备一样查看原调用的参数、输出与错误。预期展开同一条工具记录即可读取这些内容，状态更新和消息/part 归属保持一致。
- 验证仅使用自有临时数据库、合成工具记录和 loopback 传输；不重新执行原工具，不读现场数据库，不发付费模型请求。

## 定位证据

- DesktopSessionSyncHost.cpp 的 serializePart 仅为 question 发布 input/output/error；普通工具只发名称和状态，curl 错误另有静态摘要。
- Android AIAssistantController.sanitizeHistoryForSync 主动删除非 question 工具的 input/output/error；本地/远端共用的 UI 原本已有详情展开。
- iOS AIAssistantModel.remoteHistoryMessage 删除非 question 工具的 input/output，并删除所有工具的 error。精确生产 DTO/函数体的独立 Foundation probe 编译成功，19 检查中 6 项失败，恰为三种状态的普通工具详情缺失。该结果不是正常 App/UIKit 验收。
- Desktop RemoteToolGroupCard 在接收阶段又只保留工具名称和状态，展开后仅渲染元数据。
- 原服务与共享 WS 没有普通 tool 字段白名单。真实 loopback WS 在 owner 到四个 viewer 槽的 running/completed/error 往返保全合成字段及归属，history 一致；大状态包经过两块重组。跨账号读取 404。原件在 `.cache/remote-tool-detail-20261111/service/`。这不是线上部署验收。

## 只读详情协议

普通快照继续只携带轻量工具元数据，增加可选 callId、detailRevision、detailsAvailable。不把大段 input/output/error 塞入受 48 KiB 单消息限制的同步记录；question 与既有 curl 静态摘要处理保留。

展开单项后发送 `action=get_tool_detail`，请求包含 requestId、messageId、partId、field（input/output/error）、offset（源字节）、limit（1..4096）和可选 detailRevision。offset 大于零时版本必填。owner 从已鉴权的信封取得 session，不接受请求替换 owner。

Core 普通入口从一次已落库消息快照读取目标工具字段；Mobile 复用现有 JSON C ABI 的 `op=tool_detail`，不增加 ABI 符号。Desktop Host 复用同一普通入口。操作不调用模型、工具或外部文件，不改变历史。

成功结果保留 requestId/sessionId/messageId/partId/callId/tool/state/field/detailRevision，返回 offset、sourceBytesRead、totalBytes、nextOffset（零表示结束）、hasMore、text、textIsValidUtf8；无效 UTF-8 可附 rawBase64，原字节不改写。错误也保留有效请求绑定及标准 errorCode。版本不匹配为 busy，必须重新从首屏读取。

版本为 tool/state/callId/input/output/error 原字节的长度分帧 SHA256。完整 UTF-8 标量不能跨页拆分；无效旧字节按单字节推进。预算不足容纳下一个完整标量时明确 InvalidInput，不越预算或返回无法推进的成功页。

## 客户端行为与验收

- 复用普通工具组和单项展开：先读三个字段首屏，显示参数、输出和错误，可选择文字和继续加载。不自动拉完任意大字段，不进入会重试执行的业务 outbox。
- pending/ACK 绑定 account、generation、owner slot/login/session、commandId/requestId、message/part/field/offset/revision。错 ACK 不消费正确请求；切换会话、owner 替换、旧页迟到不能串内容。
- 同一 part/state 下的详情改变通过 detailRevision 触发失效；旧分页不能拼入新版字段。已缓存内容离线可看，未读内容显示所属设备不可达并提供重试。
- 历史消息、实时 point update、重连、跨页和同状态迟到详情均需覆盖。旧 owner 的已有 inline 字段可读；缺少新能力时明确提示，不能冒称详情为空或重跑命令。
- 超过同步单消息上限的工具结果仍须能按页读全；普通快照不泄漏正文。原 reasoning、媒体路径和会话隔离回归保留。

## 已执行的分层验证

各结果按自己的输入和运行层级分列，不把单元测试、普通消费者构建和真实设备验收合成一次通过。

- Core：原库对新增普通读取入口编译成功、链接缺符号，作为入口缺失证据；实现后普通
  `MaiToolTests` 26/0。最终包含 reader 和摘要格式修复的正常 Core 13 组 13/13 通过，
  189.39 秒。原日志和产物指纹在 `.cache/remote-tool-detail-20261111/root/final-core-proof.json`。
- 服务：未修改服务生产源码。原服务与真实 loopback WS 的四 viewer、三状态、history、
  分块和跨账号拒绝检查 exit 0；它证明服务能保留字段，不代表线上跨设备验收。
- Windows Desktop：旧 Host 对新详情操作/元数据用例失败；旧 UI 对单项展开用例失败。
  最终包含历史请求代际修复的普通 UI 15/0，原 Host 完整 90 PASS / 0 FAIL / 1 SKIP，
  36.420 秒。较早的 UI 14/0、Host 37.493 秒结果作为独立 cohort 保留。
  SKIP 是另需自有 TLS fixture 的图片缓存用例，不是本次工具详情跳过。
  同一 UI exe 的两个原生 Qt 自有组件例 4/0，开发者已亲看真实控件截图，参数和输出
  使用阅读列宽，没有窄条裁切；传输为合成命令 handler，不称真实跨设备 WS。
- Mobile C ABI：正常 Simulator 最终测试源 `cae1aec5...20ed` 编译为同一对象
  `c76ae9e9...5dd6`，换原 op 库实际 exit 1；换 v1 库时非法绑定响应超限和巨大可选
  callId 两 selector 各 exit 1；换 v2 普通新库完整 `--tool-detail-contract-only` exit 0。
  原 log 明确验证 legacy 源字节为 FF8078 三字节、终页 nextOffset 为零、身份/修订/
  Unicode/跨会话和有界响应。它是正常库的真实 C ABI 验证，不是 UIKit 验收。

保留的中间失败包括：旧 legacy fixture 经普通 Store 写入时已被规范化，不能用它冒充
坏字节数据库；后来用自有 SQL fixture 读回精确三字节后保留原分页断言。旧 UI 的 Busy
测试错误回了空修订、首轮阅读列过窄、command_id-only ACK 互通及 Busy 状态变化等
也分别保留原失败/修正输入记录，没有把链接失败、启动阻断或修正后的 fixture 倒填成旧例通过。

## 最终历史绑定及消费者边界

最终 Desktop review 又确认了跨历史请求代际缺口：H1 在途，重连启动 H2，按真实 WS
顺序 H1 先成功时会提前清掉 H2 的实时工具保护。已在真实 Coordinator→Panel 桥建立
失败例。Coordinator 现按 slot/login/session 绑定单调递增的本地序号，断线撤销请求，
迟到旧页明确完成为 superseded，不投递、不清除新请求。相同原例已经通过，并保留
不同房间并行及新历史仍可更新的断言。没有修改服务 wire，也没有用不符合该 WS 串行处理
的 H2/H1 逆序来冒充生产复现。

最终 10 个 Desktop 源从构建前到测试后逐 SHA 不变，正常 App build/link/deploy/PE exit 0。
Root 独立私有 `--smoke` exit 0，PID 44232，实际 exe SHA256 为
`d677f5cafe94a11698bc55dc286cbd7c7cc9d903acd470b6d06948f0ef3e3c38`。
正常开发产物版本仍 2.2.56，没有提升版本或替换公开发布资产。最终 proof 位于
`.cache/remote-tool-detail-20261111/ui-cross-history-final-20261111T064236969/`，Root 已亲看
该同 cohort 原生控件图。Mac 正常消费者结果继续单独收集。

iOS 首件正常 Debug 编译成功，但实际三 UIKit 用例失败；第一项确定为 Swift 通用桥
给详情操作附加 `force:false`，被新 Core 的严格字段白名单拒绝。仅宿主详情请求移除
此字段，没有放宽 Core 白名单。随后 known callId 缺失/空串、Busy 可见状态、旧 owner
error-only 展示及 65 个全 loading 项超出缓存硬上限，也在同正常 App 上分别失败并修复。
最终三 UIKit 原 log 为 3/0，34.745 秒：实际普通 C ABI 三字段/分页/重读/缓存、上述
身份与状态边界、原本地工具顺序。独立静态复核未发现剩余阻断；首件和修正后的四文件
delta 已在 Root 逐 previous/new blob、canonical/raw 核合。

同 cohort 原 PNG 长度 202527 字节、SHA256
`20e853f2ec73c82ff83a3ee5fe92c04eb8fb1b8161e3b2ae1444983c0c99e596`，Root 与 Mac 均已查看。
它证明原参数/错误、已读源字节计数和分页/重读控件可见。但输出首 4096 字节含 1359 个
零宽 U+200B，原图的省略显示不能证明普通长 ASCII 已完整可读。补充的可见 ASCII 自有
fixture 首页实际高 2230.33 pt；Root 与 Mac 已亲看滚动后原 PNG 的 `VISIBLE PAGE TAIL`
及页尾字符、4096/4113 计数，正文未被截断。补充 OCR 测量读反了像素，失败单独保留，
不将错误观测当作产品缺陷或 OCR 通过，也不以 AX label 含全文代替画面核对。
最终只追加一个无 OCR 依赖的正式几何槽，实际 1/0，12.542 秒；检查多行高度、真实滚动
坐标、正文末尾之后的继续/重读按钮，并保留原截图。它与此前 3/0 的 UIKit 分列，未重跑
原三例或 Release。五个 App/DebugSupport Swift 保持原字节；最终 Debug 主 exe 为
`f22181aa...8000d`，实际代码所在 debug.dylib 仍为 `df0ad87d...b4afb`，与此前三例相同。
最后六源在 Root 逐 Git blob/canonical 核准；Mac 的 LF raw 和 Root 的 CRLF raw 分别记录，
也已验证 Root 字节恰为同一 canonical 的 CRLF 展开，没有更改换行策略。

正常 unsigned Simulator Release compile/link exit 0，104.177 秒，实际版本 2.2.4/build 203，
exe SHA256 `0053fd2e1079b34559dab1bd85ecddd00b96e15f78212cc924952b403dda48c1`。
全部 9 个 Release Mach-O 均无三个详情测试 seam 标记；Debug 11 个镜像中实际检出这些
标记作阳性对照。Release 的 `maiReadToolDetail` 与 `maiMobileAgentRequest` 符号实际存在。
未将 Simulator 版本冒称桌面版本或已安装的真机 build 204，也未安装/替换真机 App。
上述 UIKit 是 Simulator 自有数据和合成 peer I/O，不是线上 WS 或实际手机验收。

Mac 正常 App/Host 消费 build 0；已注册的正常 App/Host/UI 三 target 最后重入 build 0，
6.145 秒，App 与已 smoke 的 c00f4db9...b32f 逐字节相同。新 UI target 在 Make 重新生成后的首次调用曾报
No rule，随后独立 target 正常 build 0，新 UI 完整 15/0。原 Host 全组为 89 PASS / 1 FAIL /
1 SKIP，唯一失败为旧图片 pending publish 用例的 3000 ms 断言；同 exe、同原 HOME/
CFFIXED/Qt 插件与 sandbox 环境的原单槽 3/0，884 ms，ACK 前 pending、ACK 后 failed、
断连后 failed 均通过。没有修改原预算或将单槽通过倒填为完整组全绿；完整组的首次失败
原因尚未确定。静态调用链显示新增 history/detail 分支在该首错前不可达，未凭此断言已排除
所有间接影响。
正常 Mac Cocoa 私有 `--smoke` exit 0，PID 10914，3.419 秒，exe SHA256
`c00f4db9537cbc9bc7f6513664485cfc4dd30db86a37392ae15db20a6713b32f`；没有启动或停止现装 App。

Android 最终 7 项源码已冻结：正常双 ABI App 与 androidTest 完整构建成功，11 分 5 秒；
最后 Java 状态/可选 outer request_id 修正后，正常编译、两个 APK 打包及 9 个定点组
83 测试再次成功，21 秒。随后 Busy 首屏状态枚举/64 位小写 SHA 的两个非法页断言
真实失败，窄修后同 83 例及两个正常 APK 增量再次成功，18 秒，0 failure/error/skip。
各轮构建分别记录，不声称未及时取 SHA 的
首轮 APK 与最终 APK 字节相同。当前 Core reader、Mobile、JNI 的六个新对象及两个 SO
按架构和 SHA 保全，未替换旧 App SO 冒充新链接。
最终 proof 为 `.cache/remote-tool-detail-20261111/android/final-proof/android-final-proof.json`。
实际组件 UI 例已正常编译打包，未执行。
本机只有不支持 ARM 翻译的 x86_64 完整镜像且没有连接设备，Android 实际手势和画面未验。
未为迁就环境改变产品 ABI、替换旧 App SO 或启动用户现有 AVD。

没有升级现场 App、发布新包、执行原工具、读取真实数据库或触发付费请求。语义文件检索
为另一项待确认范围的工作，没有用本任务替代它。

## 统一源码冻结

组合基线为 `04890bb7f0162f43967602f668bf4c1b631a56ff`；两个摘要源已经先提交到
`d27cb80b7ffd2a039f4877d58cf474af3486dc03`。最终组合共 34 项产品/测试源（含该两个
已提交的摘要源），各 raw/canonical/blob 与分层验证原件 SHA 见同名 JSON。
产品树为 MaiAgent `807679c649b37e4f7157a86ed38d15625e791513`、MaiChat
`ed41543fadb21e07a95cb8f16eec4170e4029d8d`。根目录诊断文档不参与这两个产品树。
构建发生在带明确 overlay 的工作区；提交之后没有重建或把旧产物冒称新打包产物。
