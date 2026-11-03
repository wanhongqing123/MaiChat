# Agent 桌面视频使用独立窗口（2026-11-02）

用户在移动端接入任务进行中补充桌面播放器问题；两项工作均保留。

复现：Qt 桌面创作中心让 Agent 播放本地视频，`maichat_play_video` 返回成功后，
播放器占据主窗口左侧区域，没有独立标题栏，主内容和会话列表被重新分配宽度。
预期：Agent 播放在可独立移动、缩放、关闭的窗口，主界面布局和会话不受影响。

源码：main `1997947219596e6520be27ed892bd6ea1820ff2e` 加当前工作区。
调用链证据：`DesktopMediaTools.cpp` 的 MainWindow 分支调用 `openDockedVideo`；
该函数明确设置 FramelessWindowHint、setDocked(true)，登记 videoDialog_ 并调用
syncVideoDock。云盘的独立窗口路径没有被 Agent 使用，因此此前云盘修复没有覆盖这里。

修复：新增 `MainWindow::openVideoWindow`，Agent 工具和创作中心视频附件均使用它。
播放器保留父对象生命周期管理，但使用 Qt::Window 和原生标题栏，不登记到 dock，
不调整主内容尺寸；账号变更后关闭该窗口。已有云盘独立窗口路径继续使用。

验证（2026-11-02，macOS arm64）：

- 先以原来的 dock 调用运行同一 `agentVideoUsesIndependentWindow` 用例，
  `!FramelessWindowHint` 断言失败，记录为 `maichat-agent-video-window-red-20261102.log`。
- 当前源码在隔离目录完成 `maichat` 和 `MaiFfplayVideoDialogTest` 的完整构建。
  同一用例通过（3 passed，0 failed，包含 Qt init/cleanup）；实际执行工具注册和调用，
  检查原生窗口、帧呈现、主内容尺寸、主窗缩放不移动播放器、关闭后保留主界面。
- Qt 的 QWidget::grab 无法捕获独立原生视频层，初次截图为黑色；改用窗口屏幕区域
  捕获后，已自行查看实际彩条视频、系统红黄绿标题栏和播放控制的截图。
  这只是验收采集方式修正，生产渲染路径没有因此改动。
- 完整 2.2.32 App 已打包，`codesign --verify --deep --strict` 及 `--smoke` 均返回 0。

产物：

- `/private/tmp/maichat-desktop-agent-window-build-20261102/`：完整隔离构建；
  原有因仓库搬迁失效的缓存未修改，克隆后重新生成路径和 FFmpeg vendor 构建。
- `/private/tmp/maichat-desktop-agent-window-package-20261102/MaiChat-macos-arm64/MaiChat.app`：
  已封装 Qt、IM、TRTC、渲染和推理运行依赖的 App。
- `/private/tmp/maichat-agent-video-window-native-20261102.log`：最终聚焦回归。
- `/private/tmp/maichat-agent-video-window-20261102/current-native-window.png`：实际窗口截图。
- `/private/tmp/maichat-desktop-agent-window-smoke-20261102.log`：封装 App 启动检查。

边界：以上均在独立测试或 smoke 进程执行，没有关闭或替换用户正在运行的
`/Applications/MaiChat.app`（观察 PID 94198）。Windows 未在本机验收。

后续用户明确回复“现在更新并重开”，已将签名通过的相同产物安装到
`/Applications/MaiChat.app` 并启动。旧 PID 94198 已退出，新 PID 77299；可执行文件
SHA-256 `bcc7b928c4444bfa2325a1c7c68220723da82a861b9857c1664a1ff90dd993f8`
与候选完全一致。安装核对记录
`/private/tmp/maichat-desktop-agent-window-installed-20261102.json`。
用户随后实际播放视频，新增反馈为大窗口进度条太短，以及更多菜单／图标观感；
这两个后续要求继续处理，不回退已完成的独立窗口行为。
