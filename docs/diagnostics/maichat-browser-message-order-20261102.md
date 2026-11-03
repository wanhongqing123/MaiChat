# 网页任务入口在会话中的顺序（2026-11-02）

## 需求与复现

- 用户 17:38 截图中，虎牙网页任务入口在后续无关回复之后，一直停在消息列表最末尾。用户明确指出问题是入口顺序，不是截图中的 DNS 加载错误。
- 基线 main：`1997947219596e6520be27ed892bd6ea1820ff2e`，保留已有未提交的媒体、浏览器和短信工作。
- 操作：在创作中心会话打开网页，随后追加用户消息和助手回复；再执行 observe/输入/重试、收起和重新打开网页，切换会话后返回。
- 预期：入口跟随创建网页的工具调用消息，后续消息位于它之后。状态变化不重排入口，返回网页保留 DOM、输入和位置；其他会话不显示它；关闭任务后移除活动入口。分页未加载到原消息时不把入口挪到最后。

## 根因证据

`AIAssistantMessageList` 在整个 `ForEach(model.visibleMessages)` 之后固定追加 `AIBrowserTaskEntry`，所以任何新消息都会出现在入口之前。`MaiToolContext` 已有真实 messageId/partId，但 browser 宿主调用只传 session_id，未把创建它的消息身份交给 UI。

## 验证计划

在正常 Root 的无付费测试会话中，构造真实顺序的工具调用和后续消息，先证明当前顺序失败，再按同一路径检查修复。检查开页、后续追加、状态更新、同网址重新 open、会话切换和分页边界，并查看截图/录屏。

## 实现

- C++ mobile_browser 的 open 从现有 `MaiToolContext.messageId/partId` 传递可信来源身份。模型参数不开放这两个字段，伪造来源的调用在到达宿主前被拒绝。
- `AIMobileBrowserTools` 在建立页面时保存来源消息和片段 ID；observe、输入、重试、停止、收起和返回都不重算位置。新的 open 使用新调用身份，即使网址相同也不按 URL 查找旧位置。
- 入口放进 `AIMessageRow` 的对应工具片段，沿用 parts 顺序，因此同轮工具后生成的正文也在入口之后。Equatable 行显式包含网页片段和审批状态，避免卡片不刷新或跨行残留。
- 普通代码调用可提供来源 ID；未提供时固定挂在调用当时的末条消息后，没有前置消息则留在时间线起点。移除原来所有消息之后的全局入口。
- 来源消息不在当前分页时不补挂到列表末尾；加载历史后在原处恢复。包含入口的完整消息作为滚动定位单元，保留原有跟随最新与阅读历史的意图。

## 证据与回归

- 17:46 CST，修改前的正常 App 回归失败：网页卡片底部 y=764.7，后续回复顶部 y=627.0，证明网页被排在新回复之后。`/private/tmp/maichat-browser-order-red-20261102.xcresult`，截图已查看。
- 第一轮修复检查：追加回复、返回网页、加载失败/重试、会话隔离与结束、输入/阅读位置和旋转，共 4 项通过：`/private/tmp/maichat-browser-order-green-20261102.xcresult`。
- 补充同一助手消息中的正文与工具片段顺序后，审批提示与返回原对话通过：`/private/tmp/maichat-browser-order165-final-20261102.xcresult`。其中新增分页用例首次选到隐藏抽屉的 ScrollView，测试操作失败；改为实际可交互的消息视口后重新回归。
- 最终顺序用例在 18:07 CST 通过：同轮正文、追加回复、observe 状态更新、收起/复开、来源消息移出当前页时隐藏、加载旧页恢复位置。`/private/tmp/maichat-browser-order165-pagination-20261102.xcresult`。已查看初始和历史页截图，并检查录屏分段帧：`/private/tmp/maichat-browser-order165-pagination-ui-20261102.mp4`。
- C++ `MaiMobileAgentTests` 编译并在同一 iOS 模拟器执行，通过消息/片段身份转发及拒绝模型伪造来源的检查，完整进程测试输出 PASS：`/private/tmp/maichat-browser-order-core-test-20261102.log`。测试目标原有 Swift 语言作用域及 FFmpeg Apple framework 链接缺失通过临时构建参数/链接命令处理，没有修改无关项目配置；测试后恢复缓存中的测试开关和链接参数。
- 同网址不同 open 的定位依据为可信调用身份；本次没有另建同网址重开的独立 UI 用例。

## 产物与边界

- iOS Debug 模拟器和 Debug 真机版 **165** 构建成功。模拟器为 A15BBB1B-934D-44CC-BDA9-193101D942CF，iOS 26.5、SDK 27.1；真机包签名严格校验通过。
- 真机包：`/private/tmp/maichat-slider-ios-device-derived-20261031/Build/Products/Debug-iphoneos/MaiChat.app`。安装再次失败，CoreDevice 4016，<connected-ios-device> 离线；`/private/tmp/maichat-browser-order165-device-install-20261102.log`。**165 未安装到真机，不声称 iOS 18.7 实机已验收。**
- 正式源码和真机可执行文件哈希：`/private/tmp/maichat-browser-order165-artifact-20261102.json`；交付前核对源码一致。main 保留原提交，没有提交或覆盖其他未提交工作。
- 共用 C++ 宿主变更后 Android Debug APK 也重新构建，沿用 versionCode 239；在独立 Android 17/API 37 模拟器 emulator-5580 安装并启动正常 App。Android 浏览器界面本次没有改动。
- 截图中的虎牙加载失败是另一个状态，本次不对该网站的可用性下结论，也没有替用户登录或发送验证码。

## 19:07 CST：真机安装状态更新

用户要求安装后，设备 <connected-ios-device> 恢复 available/paired，解锁状态 passcodeRequired=false。
先核对原 165 真机包的签名、可执行文件哈希与正式源码哈希，再安装成功；设备回读版本为 **MaiChat 2.2.4（165）**。
正常启动未带测试参数，进程 PID **19863**，可执行文件为
`/private/var/containers/Bundle/Application/A61D31FC-F131-4024-8EDB-B32D65C7B7BB/MaiChat.app/MaiChat`，
与本次安装路径一致。上文“165 未安装”是当时设备离线的历史状态，本条更新为已安装并启动。

证据：`/private/tmp/maichat-ios165-install-confirmed-20261102.{json,log}`、
`/private/tmp/maichat-ios165-device-app-20261102.json`、
`/private/tmp/maichat-ios165-device-launch-20261102.json`、
`/private/tmp/maichat-ios165-device-processes-20261102.json`。
本次核对安装和进程，未据此宣称网页顺序或短信自动化已完成真机交互验收。
