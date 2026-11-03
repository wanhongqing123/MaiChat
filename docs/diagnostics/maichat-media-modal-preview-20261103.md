# 消息视频独立呈现，保留原页面（2026-11-03）

## 用户要求、现场与预期

用户提供桌面IM视频截图：播放器位于原主窗口上方/左侧，挤占会话页面。明确要求桌面IM和创作中心用独立弹窗播放，并检查移动端，禁止播放时给原界面重新分配空间。

源码基线 `946d4a97`；本轮代码提交 `311874c116ca2eb9d2c6214356c465877afdef99`，构建时工作树代码与该提交一致。Android现场为OPPO PKD130 / Android15 / 2.2.10(247)。本轮未收到截图所在桌面OS的确认，先在本机Windows/Qt5.15.2构建实际窗口，并在已连接Android验证；iOS源码同步修正，Mac/iOS实际运行边界必须单独标明。

复现及验收步骤：

1. 桌面宽/窄主窗口点击实际IM视频消息、创作附件/结果和播放工具；应出现独立可移动的视频窗口。播放器播放、全屏/退出全屏、重复打开、主窗导航/缩放及关闭，都不改变原会话内容geometry、选中联系人、草稿或滚动位置。
2. Android创作中心选入本轮合成视频和已有测试截图，点击预览。应由覆盖原页的全屏Dialog呈现，底层Activity root尺寸、父子关系和margin不变，不能把AI或IM页面下推。横滑图库、进度播放、Back/关闭后恢复原草稿和附件，不触发发送/付费任务。
3. iOS应统一使用全屏模态承载既有图库播放器，不能给Root加leading/top padding或保留返回分屏入口。没有Mac/Xcode时只能标为源码修正，不能宣称iOS构建/实机通过。

## 已核根因

- Qt：IM `MainWindow::openVideoPreview` 与视频圈仍调用旧 `openDockedVideo`，设置frameless/docked并交给 `syncVideoDock` 按主窗宽度占上方或左侧；Agent/创作附件、云盘已有独立窗口路径，因此此前局部修复未覆盖IM。
- Android：各视频入口最终汇入 `openVideoPlayer`；`updateVideoDockLayout` 向Activity root写入top/left margin，又用非模态窗口标志允许底页继续交互，直接导致分栏。
- iOS：RootView给主内容增加dockHeight/dockWidth的padding，再overlay局部播放器；这也是正式消息入口的同类问题。

## Windows 实际验证

Windows初次测试目标未单独部署Qt5Test.dll，不能把启动失败当业务红测。完整App构建/windeployqt和90项PE检查已通过；再对测试目标运行windeployqt、核对其90个PE导入后，实际新IM用例在wide/narrow均于 `!FramelessWindowHint` 失败（exit2）。有效红测日志 `.cache/video-popup-windows-red-test.txt`。后续测试目标将显式复制Qt5Test运行库，避免重现缺DLL启动。

修复后 `.cache/video-popup-windows-final-test.txt` 为 6 passed / 0 failed（含初始化与清理；宽窗、窄窗IM点击，Agent窗口和重复目标）。同一实际窗口用例检查播放、全屏切换、导航及关闭不改变IM区域和草稿。本人查看 `.cache/video-popup-windows-screens/` 的实际屏幕截图：播放器有原生独立标题栏，关闭后原页恢复且无转场残影。

完整 `maichat` 及测试目标构建、windeployqt 和 90 项 PE/Qt 插件检查通过。仅在本地 `.cache/video-popup-win-package-946d/` 生成验证包；staging `MaiChat-win64` 与实际 ZIP 的 `zip-unpacked` 目录均使用裸 PATH `C:\Windows\System32;C:\Windows` 执行 `--smoke`，退出 0，无缺 DLL 框。两次 PID 为 60428 / 63032，日志版本 2.2.34、Qt 5.15.2，exe 路径分别属于以上目录。两份 exe SHA-256 均为 `4c75688d53519f687437fd2d4effafa5ec110a9a7b2750c717ca0e99ba4b7c7d`。未运行安装器、替换用户桌面安装或上传 Release。

## 移动端验证与边界

Android 的所有视频入口继续共用原播放器和媒体序列，改由独立全屏 `MaiVideoPreviewDialog` 呈现。初次装机发现 Dialog frame 从 y=83 开始，露出底层 Activity 白色状态栏；`dumpsys window` 明确显示 Dialog `[0,83][720,1604]`，底层 Activity 为 `[0,0][720,1604]`。后续仅在 Dialog Window 设置 cutout、insets 和黑色系统栏，不修改宿主属性。

iOS 将 RootView 原来的 padding/局部 overlay 改为 `fullScreenCover(item:)`，复用同一个媒体分页宿主、presentation ID、点播资源、选中项与关闭动作；相关正常 App UI 测试入口已同步。当前没有 Mac/Xcode 和 iOS 设备，也未收到用户对截图所在桌面 OS 的回复，因此 **macOS/iOS 仅完成源码检查，未构建、安装或实机验收**，不能用 Windows/Android 结果替代。

## Android 最终装机结果

2026-11-03 15:55，OPPO PKD130 / Android 15 / ARM64 / 720×1604，包名仍为 `com.kongshang.maichat`。原地更新为 **2.2.11 (248)**，没有卸载或清数据。最终 APK `.cache/video-popup-android/MaiChat-Android-v2.2.11-local.apk`，SHA-256 为 `172065f0b888c8baab225b4aefca5ff39fc6d0051020676294eb726e08af29ce`。从手机实际 `pm path` 拉回 `base.apk` 核对同一哈希；进程 PID 3250，当前进程 crash buffer 无 fatal/fdsan。签名与原安装证书一致，普通非 testOnly Release 包。

Gradle `testDebugUnitTest`、Android 测试 Java 编译及 Release APK 组装成功；335 个 JVM 测试无失败、错误或跳过。APK 校验 15 个 ARM64 `.so`、6 份运行库哈希及 7 份必需资产通过。此次只改 Java/UI；原生运行库沿用此前完整构建且已装机验证的 Build ID `03a5b53c5dd863044fb5203f99a5d66bdb0109f8`，未声称此次重新编译原生库。

实际创作中心选入自制 8 秒测试视频及已有测试截图，输入未发送草稿 `popup-check`，打开播放、操作进度、左滑至图片、右滑回视频、Back 关闭。本人查看 `.cache/video-popup-android/record-video.png`、`record-seek.png`、`record-image.png`、`record-video-return.png`、`record-page-restored.png`：视频真实帧与控件正常，图片和视频顶部都无白带，底层页面未被挤占。最终前后 UI XML 中 EditText/ImageButton 文本及 bounds 完全相等，草稿与两个附件保留；实际 Dialog frame 为 `[0,0][720,1604]`。

设备 `screenrecord` 对测试输出路径返回 Permission denied，未取得录屏，不宣称已逐帧验收动画连续性。三键导航和横屏侧导航安全边距由实际分发 insets 的 fixture 覆盖；本机使用当前手机导航模式，未改变系统导航设置。云盘远程点播和现存 IM 网络视频本轮未做账号内容实测，已检查它们共用同一视频窗口入口。

已移除本轮未发送草稿/两个附件、自制公共视频及 MediaStore 项；原有截图、头像、账号和聊天记录保留。手机停留在正常创作中心，右侧为空态单一加号按钮。Windows 与 Android 实际截图已发回用户 IM。各端细节另见同日 desktop-im-video-window、android-video-preview-window 记录。
