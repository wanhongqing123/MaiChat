# iOS 网页任务入口与独立页面：2026-11-02

后续 164 的表单快照、加载等待、控件标签和短信工具工作见
[浏览器表单与短信工具](maichat-browser-form-and-sms-20261102.md)。以下保留当时 162/163 的实施与安装记录。

## 操作与预期

用户同意尝试聊天任务卡与独立全屏网页页。普通网页正文读取优先使用现有 curl；
mobile_browser 保留脚本渲染、点击、输入和截图能力。

1. 在本机创作中心会话中执行 open：聊天中出现一个更新中的网页入口，页面布局保持稳定，
   用户点击后进入全屏网页，顶部为返回对话、域名和更多菜单。
2. 读取/点击/输入复用同一个页面与任务入口；返回聊天保留网页输入与滚动位置，再打开不重载。
3. 切换会话隐藏其他会话的网页入口，不允许跨会话工具访问；切回原会话可恢复仍存在的页面。
   打开另一会话的网页沿用单浏览器实例约定，替换旧页；退出账号清除实例。
4. 加载和工具运行时显示短状态与停止操作；失败提供重试，结束网页任务显式释放页面。
5. 独立页面覆盖主导航；检查横竖屏、键盘、首行/末行可达和长页面滚动。
   暂时离开创作中心或 App 后保持页面，工具调用仍受前台本机会话条件约束。

## 初始证据和实现边界

基线提交 1997947219596e6520be27ed892bd6ea1820ff2e，main 上叠加已存在的未提交修改。
当前真机已安装 2.2.4 (161)。本次前序补丁与源文件哈希保存在
`/private/tmp/maichat-browser-before-patch-20261102.diff` 和
`/private/tmp/maichat-browser-baseline-20261102.json`，不覆盖其他工作。

AIAssistantView 原来直接把 WKWebView 放在消息区上方，固定为视口约 40% 的高度。
open 会建立临时 WKWebView；切换会话关闭。网页操作依赖前台会话，普通 API 与工具共用 execute。
改为独立页面时必须保留实际 WKWebView 的宿主、布局和渲染，不能只隐藏/卸载后继续宣称可截图。

验证使用隔离模拟器 A15BBB1B-934D-44CC-BDA9-193101D942CF，合成本地网页和假会话；
不调用付费模型、不修改真实账号数据。完成正常页面录屏及截图查看后，再构建安装真机版。

## 实现与验证

### 实现

- `AIAssistantBrowserView.swift`：消息列表内一个更新中的任务入口，明确点击后呈现独立全屏
  NavigationStack。顶部返回、域名、更多菜单；刷新、分享、Safari 打开、结束任务集中在菜单。
  工作、失败和用户输入状态按需显示在底部；不显示主导航。
- `AIMobileBrowserTools` 保留同一个 WKWebView。聊天背景的非交互宿主继续布局/渲染，
  打开全屏时移交，关闭展示时归还；DOM、输入和进度不重建。背景宿主的触摸和 WebKit
  无障碍树显式关闭，收起时释放输入焦点。收起展示与结束网页任务是不同操作。
- 全屏控制器可能被 SwiftUI 缓存，不能只等 dismantle 才归还网页；通过实际
  viewDidAppear/viewDidDisappear 处理移交。恢复消息视口时把任务入口计入真实底部。
  两个宿主高度不同，WebKit 会钳制阅读 offset；在返回前保存，重新进入全屏布局时恢复，
  转场完成后消费恢复值，后续手指、键盘和旋转不继续套用旧 offset。
- 页面所有权仍属于当前本地会话；切换会话/离开前台使未完成操作失效，保留旧页面供返回。
  停止会作废旧快照和异步完成结果，同时取消所属 Agent 轮次；回调不能复活已停止页面。
- 加载失败提供真实重试；首次导航失败没有已提交历史项，需要重新加载原 URL。
  observe 明确返回加载错误；密码和验证码输入框只返回“需要用户输入”的布尔状态。
- C++ `mobile_browser` 描述明确普通读取优先 curl/text_only，宿主决定网页展示；
  工具名称、参数及审批契约保持原有协议。仅 iOS 改展示方式。
- 测试素材和本机自检留在 DebugSupport，XCTest 新增独立 `BrowserUITests.swift`。
  本机网页服务在 `MaiChatUITests/fixtures/browser_server.py`，仅监听 127.0.0.1:18680。

### 构建和已验证路径

版本 **2.2.4 (162)**。Debug build-for-testing、Release 模拟器和 arm64 真机 Debug 构建通过：

- `/private/tmp/maichat-browser162-debug-rotation-build-20261102.log`
- `/private/tmp/maichat-browser162-release-final-build-20261102.log`
- `/private/tmp/maichat-browser162-device-build-20261102.log`

真实 Root 和免费本机网页的 7 项回归通过：输入与阅读位置返回保留、会话隔离和显式结束、
慢加载停止/迟到响应、隐藏页面 observe/type/click/screenshot、加载失败入口、
原图片展开返回和快速侧滑。
`/private/tmp/maichat-browser162-final-ui-20261102.xcresult`。
进一步补验失败后点击重试真实恢复页面，以及隐藏页面完整工具契约，两项通过：
`/private/tmp/maichat-browser162-recovery-20261102.xcresult`。

已亲看任务入口、独立页首尾、返回聊天、停止状态截图以及录屏连续取帧；没有把只设置
设备 orientation 当作真实横屏验收。原速 29 秒录屏已通过 imcli 发给 whq-iphone：
`/private/tmp/maichat-browser162-preview-20261102.mp4`。
完整录屏：`/private/tmp/maichat-browser162-final-ui-20261102.mp4`。

### 横屏复核边界

加强测试为等待实际视口 width > height 后发现此前“设置了方向”的截图仍是竖屏。
`/private/tmp/maichat-browser162-rotation-20261102.xcresult` 明确失败，不能列为横屏通过。
对照原有 IM 横屏用例也没有形成双栏；原有创作中心横屏用例虽然断言通过，截图仍是竖屏。
系统日志中 XCTest 报 App orientation=0，SpringBoard 收到解锁方向请求但没有相应横屏画面。
只重启本次专用模拟器后，窗口能真正转为横屏；进一步等待实际屏幕缓冲也变为横向，
再用 XCUIScreen 截图，避免 app.screenshot 在过渡中裁出黑边/不完整内容。
加强后的完整用例通过：`/private/tmp/maichat-browser162-screen-rotation-20261102.xcresult`。
已亲看完整横屏截图，返回、域名、更多和网页内容均正常；未改动另一台正在使用的模拟器。

### 真机产物

严格深层签名与 10 个嵌入库独立签名通过，主程序 SHA-256：
`5362b2641087e52dc984bfc412386e70b1e45fa2d92f5822ad3700afc266f544`。
`/private/tmp/maichat-ios162-artifact-20261102.json` 记录 HEAD、版本和 App 源码哈希。
162 已成功安装，设备回读安装目录为 `09421EE0-93BF-4C49-A80D-FA1A55B7D8F7/MaiChat.app`。
当时手机锁屏，系统以 Locked 拒绝远程启动；未据此宣称真机交互已验收。
Android 没有连接设备，未验证 Android UI。

## 最终 163：等待确认的提示

独立网页呈现时，原会话的权限卡片/提问仍可能需要处理。163 在底部显示“需要你确认”和
“查看”，点击回到原会话继续使用原来的确认界面；任务入口也显示对应状态，没有新增审批规则。
新增测试在全屏期间投递隔离的确认请求，验证提示和原会话卡片可见；截图已亲看。

163 的真实横竖屏、输入/滚动保留、失败重试恢复、隐藏网页工具契约和等待确认返回，
4 项定点回归全部通过：`/private/tmp/maichat-browser163-ui-20261102.xcresult`。
Debug、Release 模拟器和真机 arm64 构建通过，日志为
`/private/tmp/maichat-browser163-debug-build-20261102.log`、
`/private/tmp/maichat-browser163-release-build-20261102.log`、
`/private/tmp/maichat-browser163-device-build-20261102.log`。
前序 162 的会话隔离/停止、图片和侧滑回归仍保留上述独立回执；未改共用媒体分页实现。

163 严格深层签名和 10 个嵌入库签名通过，主程序 SHA-256：
`b8022adec339c389bc03104931a755dddcb3b992effc03f431979f9ef8375e17`。
`/private/tmp/maichat-ios163-artifact-20261102.json` 保存 App 源码及产物身份。
安装 163 时设备从 available 变为 unavailable，CoreDevice 返回 **4016**，受信连接与开发服务
不可用；`/private/tmp/maichat-ios163-install-20261102.log` 保留错误回执。
**163 尚未安装/启动，最后成功回读的真机版本是 162**；162 的启动也被锁屏拒绝。
已经提示用户解锁以核对启动，未绕过系统锁屏或重启真机。恢复开发连接后可原位更新 163。
因此本次完整交互验收来自正常 App 的隔离模拟器，不能宣称真机交互已通过。

最终已查看 163 的完整横屏页、确认提示和返回原会话的截图；新增测试支持全部留在
DebugSupport/MaiChatUITests。仅停止本次启动的 18680 本机网页服务，诊断产物保留。
没有清空用户数据、调用付费模型、部署服务、提交/推送或覆盖其他人的未提交改动。
