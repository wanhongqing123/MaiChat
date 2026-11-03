# Android 视频倍速按钮视觉调整（2026-11-03）

## 需求、复现与预期

用户指出上一轮实际截图中，播放器右下角的倍速圆形按钮过大、难看。现场为 OPPO PKD130 / Android 15，已装 2.2.11 (248)，源码基线 `c65c6d0b`；之前的实际截图在 `.cache/video-popup-android/record-video-return.png`。

从创作中心导入本地合成视频并预览：右下角 `1×` 以 64×58dp 的浅灰圆角块呈现，占据较多控制条空间。点击后展开 0.25×、0.5×、1×、1.5×、2×；选择后收起选项、显示新倍率，继续由同一播放器执行。

本轮只把入口改为紧凑、无常驻灰底的白色倍速文字，保留至少 44dp 的点击区域和按压反馈；其余播放、选项、进度、媒体切换与模态窗口保持原有路径。验证点击入口、切换倍率和关闭返回，查看正常/选项展开两张真机截图，并核对最终安装包、版本和进程。

## 定位

`MaiFfplayVideoView` 为整个 `speedButton` 设置 24dp 圆角、半透明白色底，并使用 64×58dp 的固定尺寸。和进度区域相比，这个常驻大色块视觉过重。问题位于 Android 共用播放视图，IM、创作中心和云盘共用该控件。

## 验证记录

代码提交 `bc258b5d7370efa86d0648b00ec95113cdfeedea`。入口改为 56×44dp 的透明文字 Button，14sp medium，清除系统最小尺寸和多余 padding；限定形状的轻量 ripple 仅在按压时出现。五档选项、点击事件及两处原生倍率同步保持原逻辑。

2026-11-03 16:20 原地安装 2.2.12 (249)，Gradle Release 组装与 Android 测试 Java 编译通过。15 份 ARM64 库、6 份运行库哈希及 7 项资产校验通过，原生库未改动、未重编。最终 APK `.cache/video-speed-android/MaiChat-Android-v2.2.12-local.apk` SHA-256 `38f10ba43e28206d0a1b4e6196c10d1851793d55c75a29632333f1d20b22fac5`；从设备实际安装路径拉回 `base.apk` 哈希一致，PID 6860，当前进程 crash buffer 无 fatal/fdsan。

本人查看 `.cache/video-speed-android/speed-compact.png`、`speed-options.png`、`speed-quarter-paused.png`：大圆底消失，正常入口清晰，五档选项显示完整；选择 0.25× 后选项收起、按钮完整显示 `0.25×`，原暂停状态保留。UI XML 中点击范围 `[552,1336][664,1424]`，为 56×44dp。点击关闭返回同一页面及未发送附件。已清理本轮自制公共视频、MediaStore 项及未发送附件，未发送模型任务或 IM 消息。

此次是可逆的低影响样式修改，使用实际装机界面及既有倍速交互验证，不新增仅复述布局代码的测试。字体缩放使用设备当前设置，未声称覆盖所有字号；macOS/iOS 没有改动。

## 用户后续指出的三个大图标

用户在同一张实际图中继续指出右下角三个圆形动作也过大。它们分别为转发/分享、保存到相册和复制文件路径。原实现用 50dp 圆底和文字符号；本轮继续在原动作行改用项目统一小图标、透明常态底及至少 44dp 点击区，不新增操作菜单或改变动作回调。修改后统一升版安装，实际截图和验证结果在后文记录。

三个按钮改为 24dp 矢量/44dp 触摸范围，复用 `ic_ui_download` 并增加同风格的分享/复制图标。底部动作行缩为 60dp，与播放器预留高度使用同一个常量。第一次 250 真机截图 `.cache/video-controls-android/controls-compact.png` 已亲看无大圆底，且点击复制有“已复制”提示；但点击分享出现“无法分享视频”（`share-tapped.png`）。

## 实点暴露的视频分享路径故障

系统存在 ChooserActivity；Manifest 的 `.files` authority 与调用一致，`file_paths.xml` 只有 cache/files 两种根。`AIAssistantController` 的图库导入保存在 `no_backup/AIAssistant/.../Workspace`，因此 `MainActivity.sharePreviewVideo` 直接为这个源文件生成 FileProvider URI 必然失败。`RemoteIMMediaPaths` 的既有注释已说明同一平台限制，而 `AIAssistantPanel.shareAgentMedia` 已采用后台复制缓存再分享的模式。

后续定点修复只在该视频分享入口异步导出到临时 cache，再在 UI 线程打开系统分享面板。保留源文件，失败/页面销毁时清理临时导出，不放宽 Provider 根，也不在接收方读取前删除成功导出的文件。红测为实际按钮触发失败提示，修复后使用同一素材和入口复验。

## 最终 250 验证与交付

- 图标与视频分享修复提交 `4878833ffc946c663d56ba826b9772ee7a7ccaee`，包含前序倍速提交 `bc258b5d`。复制开始、过程中和 UI 分享前均绑定账号代际，UI 额外核对原 owner；取消/切号只清理临时副本，不在新账号页弹旧分享。
- 2026-11-03 16:34 在同一 OPPO PKD130 / Android 15 原地更新 **2.2.13 (250)**，没有卸载或清数据。Release 组装、资源编译、Android 测试 Java 编译通过；APK 的 15 份 ARM64 库、6 份运行库哈希与 7 项资产校验通过。此次未重跑全 JVM 测试或重编原生库，不把先前结果计为本轮全套测试。
- 最终 APK `.cache/video-controls-android/MaiChat-Android-v2.2.13-local.apk` SHA-256 为 `3d531445804873ffa5cdca119357b77131a6a79262d1f3f3128f70e5c311b37c`。从手机实际安装路径拉回 `base.apk` 哈希相同，PID 9611，当前进程 crash buffer 无 fatal/fdsan；版本与实际进程路径核对完成。
- 本人查看 `final-controls.png`：三个白色矢量图标统一、无常驻灰圆，倍速文字紧凑。真机分享同一创作附件成功打开系统分享面板（`final-share-panel.png`），取消返回原视频，未选择接收人或发送给第三方。
- 实点保存按钮生成相册视频，318623 字节及 SHA-256 与自制源视频一致；已删除本轮新建的 MediaStore 行 1000000657 及其文件。复制按钮有“已复制”提示（`final-copy-toast.png`）。点击关闭返回后，前后 UI XML 的输入框与按钮文本、bounds 完全相等，未发送附件仍在。
- 已移除本轮自制公共视频、对应 MediaStore 项及未发送附件，原头像、截图、账号和聊天记录保留；手机停在正常创作中心。成功分享的单个缓存副本保留供接收方异步读取，属于正常应用缓存。
- 最终实际截图已发回用户 IM。验证在手机当前字号/导航模式下进行；没有切换真实账号、触发付费任务或公开发布 Release。本轮没有修改或重新验证 macOS/iOS。
