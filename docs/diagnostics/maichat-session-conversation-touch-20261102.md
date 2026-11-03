# 侧滑时右侧创作页面误打开图片：2026-11-02

用户追加要求：从右侧聊天图片或其他控件上向右滑时，拖动与回弹期间均禁止点击，
松手不能打开图片或执行原控件操作，停稳后才恢复。保留已确认的侧滑样式和正常纵向阅读。

基线为 iOS 2.2.4 (157)，main HEAD `1997947219596e6520be27ed892bd6ea1820ff2e`
加前序工作区；上次真机已安装 157，但因锁屏未启动，不能视为真机验收。

## 复现操作与预期

1. 在正常 Root 创作中心显示本地图片消息，从图片内部 x=28 pt、左侧侧滑有效区起拖。
2. 向右短拖至 x=110，停留后松手，让目录回弹，连续六次；不能打开图片，也不能切会话。
3. 同一位置长拖打开目录，图片仍不能打开；收起目录并停稳后明确点击图片应正常放大、再点缩小。
4. 回归左右两页点击互锁、横向拖动不带动消息纵向滚动、正常纵向阅读、键盘、导航和视频分屏。

## 初始定位

157 只在 Session 目录内容和目录动作中应用交互锁。右侧主页面未禁用，
`AIWorkspaceImage` 的 Button 松手时直接调用 `openImage`，未检查横向拖动或回弹状态。
先通过同一操作验证是否实际触发，再实施修复。

05:10 CST，157 基线第一次短拖即打开预览，`preview.exists` 为 true，测试在 8.447 秒失败。
已查看测试录屏末段截图，图片进入放大转场，背景创作页面变暗，证实是真实误打开。
证据：`/private/tmp/maichat-conversation-touch-before-20261102.xcresult`、
`/private/tmp/maichat-conversation-touch-before-last-20261102.png`。

## 修复设计

- 右页包含消息、标题、输入及底部入口，横向拖动、回弹和目录展开时共同禁止命中和控件操作；
  目录返回透明按钮放在该范围之外，保留收起能力，外层侧滑手势也保持可用。
- 通过主线程环境回调读取实时互锁，消息图片、视频和草稿附件打开/移除再次检查。
  不传递起拖前的 Bool 快照，避免旧的 Button 回调在 touch-up 时沿用允许状态。
- 普通纵向阅读不进入此互锁，继续使用既有方向锁和动画完成回调恢复。版本升至 158。

## 模拟器验证

05:16 CST，iPhone 17 Pro Max / iOS 26.5 独立模拟器的正常 Root 页面，以下五项全部通过，
总计 142.665 秒，0 失败。`/private/tmp/maichat-conversation-touch-after-20261102.xcresult`：

- 从图片内部边缘短拖回弹六次、长拖打开目录、返回后明确点图放大再缩小：31.750 秒。
- 从其他 Session 行开始短拖六次及长拖关闭，保持 A 会话和草稿，停稳点 B 才切换：32.369 秒。
- 带纵向分量的横向拖动不改变消息位置，正常纵向阅读仍可用：18.886 秒。
- 正常开合、取消、草稿、会话切换、键盘、底部导航与横竖屏：39.330 秒。
- 视频分屏/全屏、相邻图片浏览、不重复播放当前视频：20.330 秒。

已实际查看正常 Root 的原始录屏、图片短拖的 4 fps 连续帧、长拖展开/关闭以及明确点图的
连续采样；图片只在最后的明确点击后放大，拖动中原缩略图随右页移动，未出现全屏预览。
同时查看了回到原消息的截图及横屏固定目录截图。使用内存消息与免费本地素材，未调用模型。

- 原始录屏：`/private/tmp/maichat-conversation-touch-after-20261102.mp4`。
- 连续帧：`/private/tmp/maichat-conversation-touch-short-frames-20261102.jpg`、
  `/private/tmp/maichat-conversation-touch-long-frames-20261102.jpg`。
- 实际截图目录：`/private/tmp/maichat-conversation-touch-after-shots-20261102/`。

## 真机产物与运行核对

05:19 CST，arm64 真机 Debug **2.2.4 (158)** 构建成功。App 深层严格签名及 10 个嵌入库
独立签名检查通过。主程序 SHA-256：
`4de6e9a1d08ca9100d59e027d6ebfc74efd65bb9b2d090697192646a39974482`。
`/private/tmp/maichat-ios158-artifact-20261102.json` 记录 HEAD、产品补丁和未跟踪分页源码哈希；
安装后再次检查源码补丁和主程序哈希均未变化。

已原位安装到 iPhone 14 Pro Max（`<connected-ios-device>`），设备回读确认为
2.2.4 (158)，安装目录：`74AF30F4-0AC4-4E31-90E0-1720FE64E819/MaiChat.app`。
使用 `--no-activate` 启动成功，未抢占手机前台；回读进程 PID **17166**，可执行文件路径
与本次安装目录一致。

- `/private/tmp/maichat-ios158-installed-20261102.json`
- `/private/tmp/maichat-ios158-device-app-20261102.json`
- `/private/tmp/maichat-ios158-launch-20261102.json`
- `/private/tmp/maichat-ios158-device-process-20261102.json`

真机完成构建、安装和启动核对，**尚未完成真机触控验收**；触摸回归结果来自上述模拟器。
iOS 16 兼容动画等待未实测；Android 的 `adb devices -l` 无设备，未验证。
没有清理用户数据、触发付费模型调用、提交/推送代码或部署服务。
