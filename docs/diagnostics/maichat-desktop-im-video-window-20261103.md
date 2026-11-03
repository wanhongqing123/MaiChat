# 桌面 IM 视频使用独立窗口（2026-11-03）

## 复现与预期

用户在普通 IM 或创作中心点击视频后，播放器占据主窗口左侧或上方，挤压原会话区域。
预期所有视频入口打开可独立移动、缩放、关闭的窗口；主界面的尺寸、选中会话与未发送草稿不变。
重复打开替换播放器，播放器进入或退出全屏也不改变主窗口布局。

源码基线为 `main` 的 `946d4a97`，验证构建代码提交为 `311874c116ca2eb9d2c6214356c465877afdef99`。完整 Windows 构建、
Qt 运行依赖封装、测试进程及实际截图均由主代理统一执行，未替换用户正在运行的正式桌面程序。

## 根因证据

- `MainWindow::openVideoPreview` 是 IM 视频消息点击、右键预览的共同入口，仍调用
  `openDockedVideo`；视频圈回调也保留此路径。
- `openDockedVideo` 设置 `FramelessWindowHint`、`setDocked(true)` 并登记 `videoDialog_`。
  `syncVideoDock` 在主界面宽度小于 1100 时切为纵向，否则横向，因此分别出现上方或左侧播放器。
- 2026-11-02 的修复已把 Agent 媒体工具和创作附件接到 `openVideoWindow`，
  云盘本来就走 `openCloudVideo`。这些正确入口没有覆盖 IM 和视频圈的旧分支。

## 最小回归

`MaiFfplayVideoDialogTest::imVideoUsesIndependentWindowAcrossNavigationAndFullscreen`
包含宽窗、窄窗两种数据，使用 `FakeRemoteIMClient` 恢复本地视频消息并点击真实消息按钮。
断言播放器有独立标题栏且没有无边框停靠标记，同时核对：

- 主内容几何尺寸、未发送草稿及选中 IM 会话保持；
- 再次点击替换旧播放器，主内容不重排；
- 全屏往返和主界面导航不显示停靠区域；
- 主窗移动、缩放不牵引独立播放器，关闭播放器保留会话和草稿。

原 `mainWindowDocksSamePlayerAcrossNavigationAndFullscreen` 断言的是旧产品行为，
应由上述真实 IM 入口回归替代。已有 `agentVideoUsesIndependentWindow` 及云盘独立窗口回归继续保留。

## 修复

IM 消息和视频圈改为共用 `openVideoWindow`，移除旧 `openDockedVideo` 公共入口及其实现，
避免后续调用者再次进入停靠行为。独立窗口沿用已验证的播放、转发、播放列表和账号归属保护。
视频圈临时文件的删除绑定实际独立播放器销毁，不再依赖旧停靠字段。
云盘仍保留自己的 `openCloudVideo` 流式点播与账号检查。

Windows 新测试目标首次运行还暴露缺少 `Qt5Test.dll`：正常 App 的运行依赖检查不包含 Qt Test。
在 `maichat_ffplay_test_runtime` 显式复制 `Qt5::Test`，不改变正式 App 的部署路径或通用依赖检查。

## 验证状态

主代理在 Windows 完整依赖部署后实跑新回归，宽窗、窄窗两行均在
`!FramelessWindowHint` 断言失败，退出码 2；日志为 `.cache/video-popup-windows-red-test.txt`。

最终 Windows x64 运行使用 Qt 5.15.2。完整 `maichat` 与测试目标构建成功，
`git diff --check` 通过。同组回归结果为 **6 passed、0 failed**，包含 init/cleanup 及以下四项实际用例：

- IM 宽窗：真实消息点击、独立标题栏、重复打开、全屏往返、导航与草稿保持；
- IM 窄窗：同样操作，不再把播放器放到主内容上方；
- `agentVideoUsesIndependentWindow`：独立窗口、实际控制栏宽度与平台菜单；
- `agentRepeatedTargetDoesNotReplayFrames`：重复目标操作不重播。

最终日志为 `.cache/video-popup-windows-final-test.txt`，记录用时 8545ms。
Agent 控制栏回归同时修正了旧测试的 macOS 专属面板假设：Windows 检查实际滑块所在控制行和
屏内原生菜单，macOS 仍保留面板及菜单位于面板上方的断言，没有删除真实控件可见性与宽度检查。

### 实际截图

截图来自合成测试账号的实际 Qt 主窗及原生视频层，使用 `QScreen::grabWindow` 捕获窗口区域，
不以 `QWidget::grab` 的黑色视频层代替。文件在 `.cache/video-popup-windows-screens/`：

- `wide-main-window-popup.png`
- `wide-main-window-closed.png`
- `narrow-main-window-popup.png`
- `narrow-main-window-closed.png`

主代理已亲看窄窗 popup 为独立播放器，主内容未被挤压；亲看宽窗 closed 无关闭动画残影。
closed 捕获仅在显式启用截图时等待播放器不可见及系统合成关闭动画结束，默认回归不增加等待。

### 打包与启动核对

90 个运行时 PE 及 Qt 插件依赖检查通过。完整封装目录和实际 ZIP 解压目录分别以
`PATH=C:\Windows\System32;C:\Windows` 执行 `maichat.exe --smoke`，均返回 0：

| 运行位置 | 进程 PID | 退出码 |
| --- | --- | --- |
| `.cache/video-popup-win-package-946d/MaiChat-win64` | 60428 | 0 |
| `.cache/video-popup-win-package-946d/zip-unpacked` | 63032 | 0 |

启动日志确认版本 `2.2.34`、Qt `5.15.2` 以及对应封装路径。可执行文件 SHA-256：

`4c75688d53519f687437fd2d4effafa5ec110a9a7b2750c717ca0e99ba4b7c7d`

### 验收边界

本轮完成本机 Windows 的完整构建、独立窗口回归、原生截图及打包启动核对。
用户截图所属操作系统尚未确认，不能用本机结果代替用户平台验收。
macOS 和 iOS 本轮均未构建或实机验证；未安装到用户正式桌面目录，也未发布公开 Release。
