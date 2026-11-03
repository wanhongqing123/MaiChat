# Android 视频预览挤占原页面

## 复现与预期

- 源码基线：`946d4a97`；上一轮真机为 Android `2.2.10 (247)`。
- 在创作中心已有草稿和待发送附件时打开视频；IM、Agent 视频工具、云盘点播也汇入同一个 `MainActivity.openVideoPlayer`。
- 现场截图：`.cache/android-merged-install/video-playing.png`。视频占据上方区域，创作中心标题、附件和编辑器被向下推。
- 预期：视频在独立窗口覆盖原页；关闭后回到相同会话、滚动位置和未发送草稿。

## 原因与修改

旧 `updateVideoDockLayout` 按视频窗口高度/宽度直接写 Activity `root` 的 `topMargin`/`leftMargin`，同时缩窄横屏会话侧栏。视频窗口的 `NOT_FOCUSABLE`、`NOT_TOUCH_MODAL` 标志让原页面继续可操作。它虽使用 Dialog，仍在页面中为视频预留了布局空间。

所有视频入口现在共用 `MaiVideoPreviewDialog` 全屏模态窗口；窗口不修改宿主 View 层级或布局。删除分屏/放大切换和视频布局预留，只保留按宿主完整宽度计算的横屏会话侧栏适配。

`MaiFfplayVideoView`、媒体序列、图片/视频交接、播放命令，以及 dismiss 时停止播放器和释放云盘流的原有实现保持共用。关闭视频不调用页面 render，也不重建编辑器或消息列表。

## 本地验证

- JDK 17、缓存 Robolectric、Android 34 独立执行，不运行 Gradle/native 构建或 ADB。
- `MaiVideoPreviewDialogTest`：新增组件前编译失败；实现后 2 项通过。覆盖原页面尺寸/父子对象/草稿/滚动位置保留，以及全屏窗口、聚焦/模态标志、Back 只关闭一次。
- 执行入口：`.cache/video-preview-review/run-robolectric.ps1`。
- 当前 `MainActivity.java` 与窗口组件独立 `javac` 检查退出码 0；`git diff --check` 通过。
- 真机复验由 root 统一构建、安装和操作。上述 fixture 不加载原生播放器，不能替代真机播放、方向变化和媒体切换验收。

## 全屏窗口顶部白带

root 在 `2.2.11 (248)` 上确认视频已覆盖原页、Back 能恢复原布局，但 `.cache/video-popup-android/popup-playing.png` 顶部仍有 83px 无图标白带。全屏主题并未显式配置 Dialog 的 cutout/窗口 inset；Activity 则独立设置了白色系统栏。

定点修复只调整 `MaiVideoPreviewDialog` 自身 Window：关闭 decor fitting、允许短边 cutout、API 30+ 不按系统栏给窗口预留 inset，显式设置 fullscreen/layout-in-screen 和黑色系统栏，并关闭浅色栏外观。Activity/root/播放器均未修改，关闭窗口也不需要回写宿主属性。

新增 fixture 先在默认 cutout 模式 `0` 处失败，再验证新模式、窗口 inset/flags、黑色系统栏，以及关闭后 Activity 的白色系统栏与原 flags 保持原值。修复后同组 3 项通过。此次只运行缓存 Robolectric/JDK 17，不构建安装 APK、不操作设备；顶部白带消失仍待 root 真机复验。

后续 review 指出导航栏仍可见，云盘播放的底部控件仅保留原 4dp 边距，取消窗口 inset 后可能被三键导航或横屏侧栏遮挡。Dialog 现在仅在自身 `android.R.id.content` 容器按 `navigationBars | displayCutout` 的安全边距追加 padding；黑色窗口仍覆盖物理全屏，不修改 Activity、播放器或子视图原 padding。以初始容器 padding 为基准重算，重复分发或旋转不会累加。新增实际分发 fixture 先在安全 padding 断言失败，修复后同组 4 项通过，覆盖底部导航、侧边导航/cutout、重复分发和子视图 padding 保留。导航栏设备实测仍由 root 执行。

root 对 248 的现场验证已确认视频窗口 `[0,0][720,1604]`、黑色顶部及混合媒体切换后 Back 恢复草稿/几何；但相邻图片仍出现同类顶部白带（`.cache/video-popup-android/final-swiped-image.png`）。图片预览保留原生 Dialog 和透明 bubble 收回动画，仅在 SDK 28+ 给该图片 Window 设置短边 cutout 模式，不改背景、导航边距、图片布局或缩放手势。

## 最终统一构建、安装及实际验收

- 代码提交 `311874c116ca2eb9d2c6214356c465877afdef99`；2026-11-03 15:55 将最终 2.2.11 (248) 原地装到 OPPO PKD130 / Android 15，没有卸载、清数据或改包名。
- 最终 Gradle 构建通过；335 个 JVM 测试无失败/错误/跳过（本组件 4 项包含在内），Android 测试 Java 编译通过。15 份 ARM64 库、6 份运行库哈希与 7 项必需资产门禁通过。
- APK SHA-256 `172065f0b888c8baab225b4aefca5ff39fc6d0051020676294eb726e08af29ce`；从设备实际安装路径拉回 `base.apk` 哈希一致；PID 3250 当前 crash buffer 无 fatal/fdsan。
- 最终实际图片与视频窗口均不再出现白带；真实视频帧、播放进度、图片视频互滑正常。本人查看 `.cache/video-popup-android/record-video.png`、`record-seek.png`、`record-image.png`、`record-video-return.png` 及 `record-page-restored.png`。Back 后前后 UI XML 中输入框/按钮文本与 bounds 相等，未发送草稿 `popup-check` 和两附件完整保留。
- 已清除本轮自制公共视频/MediaStore 项及本轮未发送草稿/附件，保留原头像、测试截图、账号及聊天记录。设备停在正常创作中心，空态右侧仅一个加号。
- 手机录屏输出被设备拒绝（Permission denied），本轮以实际截图和现场操作验收，不宣称逐帧视频验收。三键/侧边导航只做 insets fixture；远程云盘/网络 IM 视频仅核共用入口，未用真实账号内容补测。详见同日 `maichat-media-modal-preview-20261103.md`。
