# 图片、视频标题与 IM 图片角标清理：2026-11-01

- 起始源码：`main`，`1997947219596e6520be27ed892bd6ea1820ff2e` 加前序媒体修复工作区。
  保留所有前序未提交改动；本轮只补齐 iOS 图片和视频预览的同类入口。
- 本轮现场复现时间：2026-11-01 06:20 CST，macOS arm64，iOS 26.5 模拟器
  `MaiChat Seek Feedback 20261031`（iPhone 17 Pro Max）。
- 起始运行构建：iOS 模拟器与已安装 iPhone 均为 2.2.4 (149)；149 真机安装已回读，
  但此前启动被锁屏拒绝，不能沿用为本轮真机启动通过证据。
- 用户操作：创作中心或 IM 打开图片/视频，左右滑动到其他图片或视频，再滑回；
  IM 会话列表中的图片直接点击打开。
- 预期：图片、视频播放和切换后都不出现文件名/序号标题；图片没有关闭叉号；
  IM 图片缩略图没有右下角放大箭头，点击和回缩仍可用，视频播放控制保持可用。
- 已定位的独立绘制处：`RootView` 视频层、`MaiGalleryImageScreen`（从视频滑到图片）、
  `FullScreenImagePreviewView`（IM 图片）和 `ImageBubbleContent`（IM 图片缩略图）。
  上轮仅移除了 `AIImagePreviewOverlay` 的标题，其他入口仍会在切换后绘制标题。
  若只在初始创作中心图片上验收，会漏掉媒体切换后的这条实际调用链。

## 现场验证与交付

- 修改前运行 149 的三项现有用例，39.793 秒通过；这些旧用例原先依赖标题文字
  判断翻页，因此其通过并不满足用户要求。记录
  `/private/tmp/maichat-media-overlay-before-20261101.xcresult`。
- 已人工查看修改前三张截图：IM 全屏有 `1/3 · 文件名`；视频滑到图片后有
  `2/3 · 文件名` 和关闭叉号；IM 消息缩略图右下角确有放大箭头。
  `/private/tmp/maichat-media-overlay-before-attachments-20261101/`。
  现场录屏 `/private/tmp/maichat-media-overlay-before-20261101.mp4`（45.368 秒）。

## 修复

- 删除 `RootView` 视频层、共用媒体图片页和 IM 图片页的文件名/序号标题，
  共用图片页同时删除重复的关闭叉号，沿用点击图片关闭和无障碍逃离动作。
- IM `ImageBubbleContent` 去掉放大角标，保留覆盖整张缩略图的正式 UIKit 点击控件，
  因此仍采集点击时的窗口坐标与缩略图快照，回缩路径不变。
- 媒体身份保留在实际图片和 Metal 视频画面的无障碍 value 中，供读屏和测试识别，
  不再为测试在界面里绘制文件名。测试宿主也删除原先额外添加的可见标题。
- 创作中心混合媒体测试改为使用正常 `RootView`，走实际分屏视频、全屏和滑回图片的
  调用链，覆盖两次往返、状态栏/底部导航不泄漏、退出后恢复入口。
- Xcode 工程与 `project.yml` 同步为 2.2.4 (150)。模拟器构建与嵌入签名已通过，
  真机构建在模拟器交互和截图/录屏核对之后开始。

## 模拟器实际验收

- **6/6 通过，0 失败，76.366 秒**：正常 Root 创作中心两次图片/视频往返、
  创作中心图片展开/回缩、IM 两图/视频往返、IM 全屏边缘点击关闭、
  共用视频/图片往返、两个视频前后切换。每次媒体切换均用实际画面的无障碍身份
  核对内容，并断言文件名/序号标题没有重新出现。正常 Root 回到图片时同时断言
  状态栏和五个主导航入口未泄漏，点击关闭后主入口恢复。
  `/private/tmp/maichat-media-overlay-ios150-20261101.xcresult`、
  `/private/tmp/maichat-media-overlay-ios150-20261101.log`。
- 已人工查看 IM 缩略图、IM 第二张图片、正常 Root 从视频滑回图片、
  左右切换后视频截图，确认标题和 IM 放大角标消失。附件目录
  `/private/tmp/maichat-media-overlay-ios150-attachments-20261101/`。
- 已录屏并查看全过程缩略图及第一次正常 Root 视频回图片的逐帧片段：
  `/private/tmp/maichat-media-overlay-ios150-20261101.mp4`（93.245 秒）、
  `/private/tmp/maichat-media-overlay-ios150-overview-20261101.png`、
  `/private/tmp/maichat-media-overlay-ios150-return-frames-20261101.png`。
  画面从分屏视频展开、返回图片后无文件名标题、图片关闭叉号或底部导航。
- 日志仍有前序已有的 SwiftUI view-update 发布警告，未将目标用例通过表述为
  无运行时警告；本轮未修改解码、精准 seek 或媒体切换动画实现。
- 此处 IM 验收使用正式 IM 图片控件、窗口快照和导航宿主的本地素材，不发消息，
  尚未进行两个真实账号间的收发网络回归；也不以模拟器结果代替真机手势验收。

## 真机安装与最终边界（06:28 CST）

- 模拟器验收完成后构建真机 arm64 Debug 版，构建成功；App 严格深层签名和
  10 个嵌入框架/动态库的独立严格签名均通过。最终主程序 SHA-256：
  `7ed205c825d511413f0a6e715507a83ad970d69471a806b57d8b917f5c82636f`。
  清单 `/private/tmp/maichat-ios150-artifact-20261101.json` 同时记录源码 HEAD 和
  iOS 当前补丁 SHA-256 `eb5f370f7f1430309b73a5607d92c545ca28a4422840ba096b46eac7c085dca7`。
- 按用户此前安装最新版的授权覆盖原 iPhone 14 Pro Max，保留同一 bundle ID
  `com.kongshang.maichat` 及 App 数据/Keychain。安装成功，设备应用清单回读为
  **2.2.4 (150)**；`/private/tmp/maichat-ios150-installed-20261101.json`、
  `/private/tmp/maichat-ios150-readback-20261101.json`。
- 随后的正常启动被手机锁屏拒绝，错误为 `FBSOpenApplicationErrorDomain 7 / Locked`，
  `/private/tmp/maichat-ios150-launched-20261101.log`。因此 150 真机运行进程和实际手势
  尚未验收，不将已安装或模拟器通过表述为真机体验通过。
- 最后检查 `adb devices -l`，没有连接的 Android 设备或模拟器；本轮仅修改 iOS，
  Android 未实机验证。没有为此启动或停止其他任务的模拟器。
- 最终 `git diff --check` 通过；保留前序 Mac、共享媒体核心和 HDR 修复工作区，
  本轮没有提交或推送，没有修改服务器或用户媒体文件。
