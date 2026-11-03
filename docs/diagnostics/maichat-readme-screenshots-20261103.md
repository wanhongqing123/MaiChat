# README 实际运行截图（2026-11-03）

用户要求在 MaiChat 根 README 展示几张实际运行图。操作与预期：进入仓库首页即可看到
清晰的真实 App 页面及功能说明；使用仓库内相对图片路径，GitHub 可直接加载。

截图从已有正常 App 运行验收中选择，逐张查看；可以使用本地测试账号与演示素材的真实
页面，但必须标明来源，不能用设计稿、AI 生成图或失败/加载画面替代运行图。不公开
个人手机号、登录凭据、真实私人会话或家庭媒体。文档只添加图片与说明，不改产品逻辑。

开始时源码提交为 `ecef0b19`，工作区干净。验收内容为图片来源和隐私检查、相对链接
存在、原始截图一致性、Markdown 布局与 GitHub 回读；不为文档改动重建产品。

## 选择与隐私检查

已用 `view_image` 逐张查看下列四张原图。桌面图使用正式 `MainWindow` 和云盘组件，
IM 为假客户端、账号及云盘为本地 fixture；目录和 `video-lifecycle.mp4` 均为示例。
iOS 欢迎页是静态功能说明，网页内容由本机服务提供；没有私人会话、手机号或家庭照片。
Android 图是模拟器中隔离测试宿主呈现的正式创作中心组件，页面干净，无调试面板、
失败提示或权限弹窗。README 明确说明本地示例及测试环境，未称为真机截图。

iOS 欢迎页保留原始长页面及固定输入栏；部分可滚动内容位于输入栏后方，没有裁切、
重画或修改界面来掩盖原图布局。图片仅逐字节复制，没有使用 imagegen、缩放或重编码。
未复制截图导出目录中的 manifest、录屏、日志或设备标识文件。

## 原图、构建与验收来源

| 仓库图片 | 构建与取景来源 | 原始文件 |
| --- | --- | --- |
| [desktop-cloud-drive.png](../screenshots/desktop-cloud-drive.png) | 2026-10-31，macOS arm64 / Qt 桌面 2.2.32；Cocoa `cloud_drive_dialog_test` 中的正常 MainWindow，基线 `19979472` 加当时工作区。对应日志 `maichat-cloud-player-delivery-20261031.log`，云盘回归记录见[原诊断](maichat-desktop-cloud-media-20261031.md)。 | `/private/tmp/maichat-cloud-player-delivery-20261031/cloud-main-window.png` |
| [ios-creation-center.png](../screenshots/ios-creation-center.png) | 2026-11-03，iOS 2.2.4 (172)，iPhone 17 Pro Max 模拟器；`PortraitUITests/testWelcomeDescribesPortraitToolsWithoutActions()`，附件名 `portrait-tools-visible-on-welcome`。结果归档 `maichat-account172-ios-ui-20261103.xcresult`，构建记录见[账号工具诊断](maichat-account-profile-tools-20261103.md)。 | `/private/tmp/maichat-account172-ios-ui-shots-20261103/B3696047-1CDD-4A83-ACEA-790C88AD8480.png` |
| [ios-browser-task.png](../screenshots/ios-browser-task.png) | 2026-11-02，iOS 2.2.4 (163)，iPhone 17 Pro Max 模拟器；正常 Root 的 `BrowserUITests/testBrowserRetainsInputAndScrollAcrossChatAndRotation()`，附件名 `browser-independent-page-top`。结果归档 `maichat-browser163-ui-20261102.xcresult`，取景来源见[独立网页诊断](maichat-browser-independent-page-20261102.md)。 | `/private/tmp/maichat-browser163-shots-20261102/36309B97-FEA9-44CF-AA36-55BF4538A986.png` |
| [android-creation-center.png](../screenshots/android-creation-center.png) | 2026-11-02，Android 2.2.6 (243)，API 37 arm64 / 16 KiB page 模拟器；隔离测试宿主里的实际 `AIAssistantPanel`，未登录真实账号。此图仅展示欢迎页，不代表后续 245/246 的媒体修复验收；环境记录见[本地修图诊断](maichat-local-retouch-integration-20261102.md)。 | `/private/tmp/maichat-portrait243-android-qa-20261102/welcome.png` |

这些是所注明历史构建的实际页面，不能据其推断未展示路径的验收结果。原始产品截图
不包含源码提交信息；不把当前 README 提交号冒充各截图的编译提交号。

## 原文件一致性

| 文件 | 像素尺寸 | 字节数 | SHA-256（源文件与仓库副本一致） |
| --- | --- | --- | --- |
| desktop-cloud-drive.png | 2560×1640 | 67,876 | `946540888b1ceaefded23ccd9337808feef06b45c7e6059f1253e70df544d495` |
| ios-creation-center.png | 1320×2868 | 296,218 | `dcfdfa50bc9310da2b5e8c03e88cf9ccd1f4389fd66abcf21f37cd1b16f11a89` |
| ios-browser-task.png | 1320×2868 | 444,570 | `fe91c53fe8a97eeb91cbc195662fc975dc664faabf5afb5d71bd2f2ab6ad141e` |
| android-creation-center.png | 1080×2424 | 162,398 | `e38564d2adcb7528aa93576683cd4c01e6d2de70d09c622882d17fca5b63f053` |

README 开头使用桌面宽图和三列移动端原图链接；移动端仅通过 HTML `width="250"`
控制显示尺寸。原始 PNG、所有本地相对链接及 `git diff --check` 已检查。
GitHub 提交后的图片加载和文档回读由主任务在推送后核对；本子任务没有暂存、提交或推送。
