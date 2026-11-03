# Android 图片展示与 iOS 对齐（2026-11-04）

## 操作与预期

- 从 IM 图片气泡进入预览：完整图片等比从气泡位置放大，不能把裁剪缩略图拉伸成原图。
- 单击预览图片关闭；双击在 1 倍与 2.5 倍间切换；双指缩放范围为 1–4 倍。缩放与滑动不能误触关闭。
- 再次打开、缩放后关闭、系统返回：图片缩回原气泡；预览另一项时不能缩回不属于它的气泡。
- 横竖屏均按预览窗口实际坐标计算，原会话、滚动位置和草稿保持不变。
- 同时检查创作中心与云盘的共享图片入口；没有有效来源视图时使用淡出关闭。

## 现场与根因

- 源码：`2cba039919c0212e2775ee993df46bad4cefa739`；Android 已安装 2.2.16(253)。OPPO PKD130 / Android 15，设备 `89VCD65LEYTGROW8`。
- 真机横屏打开 IM 中已有图片，单击图片后仍保留预览（`single-before.png`，本地 `.cache/android-image-transition/`）。没有修改或发送消息。
- Android `MainActivity.imageMessageContent` 使用 CENTER_CROP 和固定高度；入场浮层是该缩略图的截图、FIT_XY、260ms，退场 220ms；手势没有单击关闭和双击缩放。
- Android 来源使用全局可见矩形，再减 displayMetrics 中心；没有转换到独立预览容器坐标，部分可见气泡也会被裁断。
- iOS `ChatView.ImageBubbleContent` 为 220×180 范围内等比缩略、圆角 8；`WindowFrameActionButton` 保存完整窗口矩形。`MaiLocalMediaPagerController.openImageIfReady/requestClose` 使用完整 UIImage、aspectFit、380ms easeInOut，圆角 8↔0，并转换到实际预览视图坐标。单击关闭，双击 1↔2.5，pinch 1–4；非初始媒体或来源失效时淡出 220ms。
- 本机没有 iOS/Xcode 运行环境；iOS 依据当前仓库源码，不声称完成新的 iOS 实机验收。

## 修改计划

1. 图片气泡使用原有解码器输出的完整图片比例和圆角；创作中心传入实际缩略图视图。
2. 提取宿主图片过渡和手势控制：Graphics 仍负责最终图片渲染；临时浮层只承担完整图的等比移动与圆角动画。
3. 按实际容器位置转换来源矩形，采用 iOS 的进入/关闭/缩放规则，保留媒体来源身份。
4. 构建并原位更新真机；检查真实录屏中的进入、双击、单击缩回、返回及横竖屏；核对 APK、进程与源码并记录验证边界。

## 验证记录

### 实现

- `MaiImageThumbnailView` 保留未裁剪 Bitmap，气泡按 220×180dp 上限等比排版、圆角 8dp。
- `MaiImagePreviewTransition` 使用完整图片的 FIT_CENTER 浮层、380ms UIKit easeInOut 曲线及圆角 8↔0；最终画面仍由 Graphics 渲染。不再截图裁剪后的 View 并使用 FIT_XY。
- 来源按实际窗口坐标转换；仅原始图片及同一登录代际允许缩回。其他媒体淡出 220ms；旋转后不再使用旧方向快照。当前容器的系统栏 padding 在预览期间保持，避免底层位置跳动。
- 单击关闭、双击以点击位置为中心缩放至 2.5 倍/恢复 1 倍，pinch 范围 1–4。加载取消、迟到回调、关闭动画、不可见操作按钮和账号代际均有门禁。
- 创作中心草稿/消息/工具图片和云盘图片入口传递实际图像视图，异步云盘下载只弱引用来源。

### 旋转补充故障

第一次旋转录屏发现：切回横屏时窗口已为 1604×720，图像却约 153×331，顶部小图、底部黑块。根因是 Android `gl_update` 原为空：Presenter 更新 viewport，EGL buffer 仍保持旧尺寸。按 OBS 平台 resize hook 的位置，Android 后端现在在原渲染线程、原 EGLContext 上重建 window surface 并设置新 buffer geometry，保留纹理和播放状态。

对照了本机 OBS `libobs-opengl/gl-x11-egl.c` 的 resize 调用位置；EGL 的重建要求参见 [SurfaceTexture 官方契约](https://developer.android.com/reference/android/graphics/SurfaceTexture#setDefaultBufferSize(int,%20int))。实际新录像横屏图片约 332×720，回到竖屏为 720×1561，旧小图和黑块消除。

### 构建、测试与实机

- Android Release **2.2.17(254)**，包名不变：`com.kongshang.maichat`，ARM64，`testOnly=false`，以原证书原位更新；未卸载生产 App 或清除其数据。
- APK：`.cache/android-image-transition/MaiChat-Android-v2.2.17-local.apk`；最终 SHA-256：`8d03014d7845025e3ae20c5dfead81b27044d73b440c77032c13954205a062af`。
- Release 构建通过；15 个 ARM64 库、6 项运行库 hash、8 项必需资源检查通过。打包曾因默认 2GB JVM 内存不足失败，使用本地 `-Xmx4096m`、单 worker 后通过，未改仓库构建配置。
- JVM **374/374** 通过，包括缩略图比例、窗口坐标、来源身份、单/双击以及关闭后迟到入场的回归。
- 真机 Graphics/解码 **5/5** 通过：PNG、RGBA、视频实际像素、三次尺寸切换后图像居中及 letterbox、源文件删除后仍保持纹理、异步成功回调和请求替换。
- 尺寸回归作红绿对照：旧包 `be747c93…` 在 160×96 像素断言失败，新包相同测试通过，输入和阈值未放宽。测试图层改为独立 Dialog，避免真实账号的异步 Main 页面重建移除测试 View，隔离前的捕获/等待失败不作为产品回归结论。
- 按 MaiAgent 规范，当前源码重新编译并运行 Windows 核心八套测试：**8/8**，198.96s。记录 `.cache/maiagent-core-gate/ctest.log`、166 文件指纹；该组 `GRAPHICS=OFF`，不能代替 Android GLES 验证。
- 设备原生 screenrecord 写文件被拒绝；使用校验过官方发行 hash 的 scrcpy 5.0.1、只读无音频无控制模式录屏。逐帧检查：
  - `fixed-valid.mp4`：竖屏进入、双击放大/恢复、单击缩回、再次打开及 Back；无拉伸、重影、黑帧或返回错位。
  - `fixed-landscape.mp4`：同样操作，底层左右标题、输入栏可见帧位置保持，系统栏恢复不再带动页面位移。
  - `fixed-ai.mp4`：真实创作中心草稿选本机已有图片，仅预览、不发送；进入/缩放/关闭后附件保留，再移除本轮附件恢复原空草稿。
  - 最终 GLES 包 `fixed-egl-rotate.mp4`：竖→横→竖后图像完整居中，来源快照失效时正常淡出回原页。亲看了原生分辨率关键帧。
- 真机测试包已移除，旋转恢复原来的自动模式。测试中没有发送 IM 消息或触发模型请求；用户测试期间新收到的消息保留。

### 验证边界

- 以上 iOS 手势/几何参数来自当前仓库代码，没有新的 iOS 运行录像或实机验收。
- 本轮真机覆盖 IM 单张图片、创作中心草稿图片、旋转、单/双击和系统返回；pinch 的范围/手势仲裁已实现，未单独完成双指实机动作验收。
- 云盘接入和来源失效回退通过源码与编译检查，没有为验动画上传/下载真实云盘图片。生成图片入口与混合媒体翻页连续动画也不列入本轮新验收结果。
- 运行源码提交与最终安装进程核对在提交后补记。
