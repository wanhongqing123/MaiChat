# README 实际运行截图（2026-11-03）

用户要求在 MaiChat 根 README 展示几张实际运行图。操作与预期：进入仓库首页即可看到
清晰的真实 App 页面及功能说明；使用仓库内相对图片路径，GitHub 可直接加载。

截图使用产品页面及控件运行画面，逐张查看；可以使用本地测试账号与演示素材的真实
页面，但必须标明来源，不能用设计稿、AI 生成图或失败/加载画面替代运行图。不公开
个人手机号、登录凭据、真实私人会话或家庭媒体。文档只添加图片与说明，不改产品逻辑。

开始时源码提交为 `ecef0b19`，工作区干净。验收内容为图片来源和隐私检查、相对链接
存在、原始截图一致性、Markdown 布局与 GitHub 回读；不为文档改动重建产品。

## 选择与隐私检查

用户随后认为独立演示网页和 Android 欢迎页内容过于空白，要求换图。经逐张复查，
现有群聊/图片翻页测试图含调试浮层或纯色测试素材，不作为替换。最终保留三张：
新增 macOS 消息、引用与文件大图，保留 macOS 云盘及 iOS 172 功能网格。
本次文档换图基线为 `52774f2b`；产品并行开发中的改动不属于截图验收范围。

新消息图已经主任务实际 `view_image` 查看并确认。它来自真实产品 `MainWindow`、
消息、引用和文件控件，用已有 2.2.34 编译库及资源运行独立 Qt 宿主；通过普通历史
恢复接口载入三组离线示例会话，没有修改产品页面源码、绘制假界面或重画控件。
示例昵称、消息和文档均为人工合成，头像为产品默认卡通占位图。

宿主使用 `QT_QPA_PLATFORM=offscreen`、2 倍像素输出和独立应用标识，开启 Qt 测试目录，
运行时禁止全部网络并禁止写入真实 MaiChat 数据目录。截图只来自本进程的窗口控件，
没有接触当前已登录的线上 App、真实账号、私人会话、家庭照片或付费模型请求。
因此 README 描述为产品控件与本地示例会话，不称为当前线上账号的实录。

云盘图同样使用正式 `MainWindow` 和云盘组件，IM 为假客户端、账号及云盘为本地
fixture；目录和 `video-lifecycle.mp4` 均为示例。iOS 欢迎页是模拟器静态功能说明。
三张图均无 PASS/调试面板、失败提示或权限弹窗。

iOS 欢迎页保留原始长页面及固定输入栏；部分可滚动内容位于输入栏后方，没有裁切、
重画或修改界面来掩盖原图布局。图片仅逐字节复制，没有使用 imagegen、缩放或重编码。
未复制截图导出目录中的 manifest、录屏、日志或设备标识文件。

## 原图、构建与验收来源

| 仓库图片 | 构建与取景来源 | 原始文件 |
| --- | --- | --- |
| [desktop-messages.png](../screenshots/desktop-messages.png) | 2026-11-03，macOS arm64 / Qt 2.2.34 已有产品库与资源，独立 `MainWindow` 宿主。`FakeRemoteIMClient` 通过联系人及历史恢复信号提供自然示例讨论、引用回复和 Markdown 文件卡；所有对话均是离线示例。 | `/private/tmp/maichat-readme-chat-p9tyi31q/macos-local-chat.png` |
| [desktop-cloud-drive.png](../screenshots/desktop-cloud-drive.png) | 2026-10-31，macOS arm64 / Qt 桌面 2.2.32；Cocoa `cloud_drive_dialog_test` 中的正常 MainWindow，基线 `19979472` 加当时工作区。对应日志 `maichat-cloud-player-delivery-20261031.log`，云盘回归记录见[原诊断](maichat-desktop-cloud-media-20261031.md)。 | `/private/tmp/maichat-cloud-player-delivery-20261031/cloud-main-window.png` |
| [ios-creation-center.png](../screenshots/ios-creation-center.png) | 2026-11-03，iOS 2.2.4 (172)，iPhone 17 Pro Max 模拟器；`PortraitUITests/testWelcomeDescribesPortraitToolsWithoutActions()`，附件名 `portrait-tools-visible-on-welcome`。结果归档 `maichat-account172-ios-ui-20261103.xcresult`，构建记录见[账号工具诊断](maichat-account-profile-tools-20261103.md)。 | `/private/tmp/maichat-account172-ios-ui-shots-20261103/B3696047-1CDD-4A83-ACEA-790C88AD8480.png` |

消息图的临时宿主、源码、编译/运行日志仅留在
`/private/tmp/maichat-readme-chat-p9tyi31q/`，不提交到仓库。宿主复用现有
`/private/tmp/maichat-desktop-agent-window-build-20261102` 中的产品静态库及资源，
仅在上述临时目录编译截图入口，没有重建或替换正式 App。
临时入口 `capture.cpp` SHA-256 为
`cca15ddecb7dee37975b924c3f5a0e65c0c5f5a3f6122b9d46339213ce36bbdb`；
截图宿主 `capture` 为
`e14c5d973b6db3c6121ab79ed8f34a179eb2cf9750feae04d0c94faf0d0a8865`。
这些摘要用于核对截图来源，不是正式 App 可执行文件的摘要。

这些是所注明历史构建的实际页面，不能据其推断未展示路径的验收结果。原始产品截图
不包含源码提交信息；不把当前 README 提交号冒充各截图的编译提交号。

## 原文件一致性

| 文件 | 像素尺寸 | 字节数 | SHA-256（源文件与仓库副本一致） |
| --- | --- | --- | --- |
| desktop-messages.png | 2800×1920 | 326,848 | `efe2f0a7e6d014f2f67b3d41e7428d9a6f20d23607a70abb25a721f5dca05873` |
| desktop-cloud-drive.png | 2560×1640 | 67,876 | `946540888b1ceaefded23ccd9337808feef06b45c7e6059f1253e70df544d495` |
| ios-creation-center.png | 1320×2868 | 296,218 | `dcfdfa50bc9310da2b5e8c03e88cf9ccd1f4389fd66abcf21f37cd1b16f11a89` |

README 开头先放消息宽图，下方并排放云盘宽图（HTML width=680）及 iOS 网格
（width=260），没有把桌面聊天塞进手机窄列。三张图片均可点击原图。
被用户否定的两个旧资产及其图片引用已移除，原始临时文件保留。
原始 PNG、所有本地相对链接及 `git diff --check` 已检查。
GitHub 提交后的图片加载和文档回读由主任务在推送后核对；本子任务没有暂存、提交或推送。
