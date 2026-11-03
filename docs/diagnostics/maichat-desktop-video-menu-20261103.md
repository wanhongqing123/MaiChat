# macOS 两排透明视频控件与合并后回归

日期：2026-11-03。源码基线 `d8a39b2f54e1fe5d459fe8a65e76901b6b16d574`
合并 `origin/main` 的 `98e92ae8a583c94815b94dc24892d4e2e7aa4ed0`，加下述未提交修正。

## 用户操作与修改

用户先指出原六项文字菜单过多，随后明确要求直接图标、尽量透明，并提供两排布局参考。
最终 macOS 控制条由既有 Qt Widgets 绘制：上排只有尽量全宽的进度条；下排保留播放、
紧凑时间、倍速、全屏、关闭，文件动作只有 **分享**、**保存** 两个 `QToolButton`。
默认 `IconOnly`，复用原统一 stroke/HiDPI 图标；标签放在 tooltip/AX，默认透明底，
hover 才轻微反馈。控制条背景改为 alpha 105 的薄透明圆角，不增加白色方角或新弹层。

Mac 的 more 文字菜单和额外重播图标移除；重播可定位进度到开头，原 replay/forward
普通代码能力保留，forward 现在可由普通调用独立执行，不依赖隐藏按钮。分享继续调用
既有 AppKit 系统分享，保存继续调用既有另存为，不新增分享网络或文件处理实现。
此改动只针对用户截图所在的 macOS 控制条，不宣称 Windows 已有相同新布局。

`agentVideoUsesIndependentWindow` 检查宽窗、640×420 窄窗、全屏的进度宽度，
进度占控制板宽度超过 90%、动作位于下一排、分享/保存始终可见且没有 more 按钮。
图标模式、非空图标、tooltip/AX 及独立普通 forward 回调另有断言。

## 实际运行结果

- 隔离构建目录：`/private/tmp/maichat-desktop-agent-window-build-20261102`，最多两个编译任务。
  先构建静态依赖及 `desktop_session_sync_host_test`、`MaiFfplayVideoDialogTest` 测试 exe；
  随后正常 `maichat` 目标构建为2.2.35，本地 build bundle 经 adhoc 签名/strict验证，Cocoa `--smoke` exit0，3.496秒。其Qt仍指向Homebrew，不能当作可分发包。
  未替换/停止 `/Applications/MaiChat.app`，用户PID43461前后相同。
- 合并后 host 组：`QT_QPA_PLATFORM=offscreen`，**26 pass / 0 fail / 0 skip**。
  日志 `/private/tmp/maichat-postmerge-session-host-test-20261103.log`。
- 两排图标版的宽/窄/全屏布局、普通 handler、暂停保持可见已经通过；最终关联
  controls/seek/ended 回归日志为 `/private/tmp/maichat-video-two-row-final-tests-20261103.log`，
  **8 pass / 0 fail / 1 skip**，21.22 秒。该次 wake 原有前台/Space guard 跳过，未改 skip；
  使用同一个最终 exe 单独重跑 `fullscreenControlsHideAndWake`，
  **3 pass / 0 fail / 0 skip**（含 init/cleanup），6.21 秒，
  `/private/tmp/maichat-video-windowmove-final-20261103.log`。两份结果均保留，不沿用旧菜单结果。
  所选七个功能依次是图标/普通 forward、独立窗口与三种尺寸布局、原生分享/保存取消、
  暂停保持控件、全屏隐藏/唤醒、鼠标定位进度、结束态暂停及 seek。
- 实际图标点击定点 **3 pass / 0 fail / 0 skip**（含 init/cleanup），6.62 秒，
  `/private/tmp/maichat-video-native-actions-final-test-20261103.log`：向测试自身 NSWindow
  投递原生 down/up，确实打开 `_NSPopoverWindow` 系统分享，再取消而不选择目标；保存
  进入真实 `NSSavePanel`，以原生 modal-mode timer 取消，不写副本、不关闭播放器。
  仅 Qt QWidget 合成 click/窗口类猜测会造成 harness 假失败，没有据此改分享产品核心。
- 已实际查看高亮彩色视频上的两排图标截图：
  `/private/tmp/maichat-video-two-row-icons-20261103.png-compact.png`、
  `/private/tmp/maichat-video-two-row-icons-20261103.png-fullscreen.png`。
  背景颜色确实透出，时间及白色图标可辨，无文字菜单/白色方角。宽窗截图的底部可能
  受系统 Dock 遮挡，不将其作为可公开完整截图；窄窗/全屏素材是仓库测试视频，无私人媒体。
  后续 `two-row-final` 同名截图被系统崩溃报告/权限对话框遮挡，不作为清晰 UI 证据或发给用户；
  上述较早清晰图对应同一产品控件绘制代码，后续只改测试刺激及非 UI trace。

## 保留的环境和失败证据

- 初次按指定 offscreen 运行 video 全组，在 `maiGraphicsStyleMacVideoWindow` 崩溃：
  Qt offscreen `WId=0x2` 被当成 `NSView*` 发送 ObjC 消息。已加最小正式平台 guard，
  只有 `QGuiApplication::platformName()=="cocoa"` 才使用该原生样式桥。
  WId 是平台插件的不透明句柄，不能跨插件解释。Cocoa 行为不变。
- 直接编译实际 `MaiGraphicsMacLayer.mm` 的独立 offscreen bridge probe **exit 0**：
  `/private/tmp/maichat-style-bridge-offscreen-probe-20261103/`。
  不修改测试 skip，不为此扩展新的软件渲染架构。
- 整个原生 Metal 播放测试依然不能在 offscreen 上执行：样式 guard 后下一处既有
  `maiGraphicsCreateMacLayer` 用 offscreen `WId=0x3` 作 NSView 而崩溃。这是测试后端
  不支持原生视图的边界，不能将 offscreen 139 写成视频业务全组通过。
- 经主任务明确授权，随后运行独立测试 exe 的 Cocoa 原生窗口全组：
  **31 pass / 2 fail / 3 skip**，exit 2，102.44 秒，日志
  `/private/tmp/maichat-postmerge-video-dialog-cocoa-20261103.log`。
  三项 skip 是两项须显式授权的真实摄像头测试和未提供私人本地视频的像素测试。
  没有启用摄像头、付费模型、真实账号或用户媒体。
- 该轮两项失败单独重跑：`opensControlsAndClosesVideoPopup(video-audio-subtitles)` **通过**，
  首轮 QTest 指出冷启动需约 9.55 秒而该断言只有 4 秒；没有放宽断言。
  `fullscreenControlsHideAndWake` 在原 QWidget move 刺激下仍失败，不能只凭
  active=1/space=1 推断产品分支故障。后续已定点定位并回归，见下一节。
  原失败日志 `/private/tmp/maichat-postmerge-video-dialog-cocoa-fail-recheck-20261103.log`。

## 输入刺激与原生取消的决定性证据

- 本机 Qt 5.15.18 `qtestmouse.h` 的 QWidget MouseMove 分支只 `QCursor::setPos` 后等待，
  不直接派发 MouseMove；QWindow overload 调用 `qt_handleMouseEvent`。测试改用后者，
  断言和 timeout/skip 均未放宽，也未给产品增加全局轮询。
- `/private/tmp/maichat-video-share-and-windowmove-probe-20261103.log` 中实际收到
  `QWidgetWindow → MaiFfplayVideoDialog` MouseMove，local `(775,474)` / global `(775,507)`，
  此前 panel hidden=true、hide timer inactive/-1；事件经原有 filter/reveal 后 hidden=false，
  相同 `fullscreenControlsHideAndWake` **通过**。此证据覆盖 Qt 输入链/真实控件状态，
  不能替代物理鼠标/CGEvent 的硬件验收。
- 分享探针确认 anchor.window=true、active 状态、服务数 7，未记录文件路径或账号。
  所有权 strong-retain A/B 没有改善，已撤回该猜测。原生点击正确后可观察系统 Popup，
  不需要修改原分享核心或依赖私有菜单类。
- Save 点击测试一度用 Qt timer 找 QFileDialog 来取消而阻塞。只 sample/终止了自己的
  测试 PID 55583；`/private/tmp/maichat-video-native-click-hang-20261103.sample.txt` 确认栈已进
  `saveCopy → QFileDialog::getSaveFileName → NSSavePanel::runModal`。改成原生 modal-mode timer
  向 `NSApp.modalWindow` 发送 cancel 后通过，保留真实系统面板，不选文件、不用 fake 保存。
- 真实 NSSavePanel 仍可能记住先前目录：最终测试触发系统提示“Multi-AI Code.app 想访问
  桌面文件夹”，负责进程属于测试启动宿主。未点击允许/不允许、未授予 Desktop 权限，
  也未把带该弹层的截图作为 UI 完成证据。该系统提示与自己的旧测试崩溃报告已告知主任务，
  本任务没有关闭用户 App 或处理其他任务的窗口。

最终源/测试 exe SHA-256、前后运行日志与崩溃报告位置记录在
`/private/tmp/maichat-postmerge-desktop-20261103-manifest.json`。
本记录不表示用户正在运行的 App 已更新，正式 App 构建和重开由主任务继续处理。

正常本地App回执：`/private/tmp/maichat-desktop235-smoke-20261103.json`；`--smoke` 不调用登录恢复、不进入IM/模型，仅打开自身登录窗口后退出，普通本机日志仍由既有AppLog写入。
