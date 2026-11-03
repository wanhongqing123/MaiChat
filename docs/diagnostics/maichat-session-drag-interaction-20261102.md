# 会话侧滑时禁止列表误触：2026-11-02

- 用户追加要求：向右滑或在左侧拖动时，不允许手指触发其他 Session，防止返回时会话已变化；
  同时应阻止目录的其他操作，停稳后恢复正常点击。
- 基线为 156 产品代码；该版已原位安装（`3529C83C-8AD8-40C3-8A32-83DE29CD446B`），
  用户追加要求到达后未继续启动；157 将同时保留聊天内容纵向滚动修复。
- main，HEAD `1997947219596e6520be27ed892bd6ea1820ff2e` 加前序工作区。

## 操作与预期

1. 正常 Root 的 A 会话保留草稿，打开 Session 列表；从 B 行上横向短拖并回弹，重复六次。
2. 拖动、手指松开、回弹未结束期间，列表行、更多菜单、分组和新建入口均不得执行操作。
3. 返回创作中心后仍是 A，原草稿仍在；页面停稳后明确点 B，能够正常切换到 B。
4. 同时回归横向拖动不带动消息纵向滚动、正常纵向阅读、取消、横屏与视频分屏。

## 初始证据

- 目录只按 showSessions 决定命中，closing 拖动期间仍为 true；opening 松手后立即为 true，
  都没有检查手势活动或动画是否停稳。
- 行操作直接 closeSessionDrawer 后执行模型切换，没有动作侧的移动期保护。
- 该故障为用户报告的偶发现象，基线先用正常手势尝试复现，不将代码风险直接记为已复现切换。

## 复现与修复

- 00:37 CST 在正常 Root 156 基线上，A 会话带草稿打开目录，从 B 行内横向短拖，
  第一次就失败。末帧直接显示 **B 历史消息 74–79**，A 草稿也不再显示，证实实际误切换，
  并非单纯目录关闭。`/private/tmp/maichat-session-touch-before-20261102.xcresult`、
  `/private/tmp/maichat-session-touch-before-last-20261102.png`；已查看实际末帧。
- 157 增加贯穿拖动、松手与回弹的交互锁；iOS 17+ 使用 SwiftUI 动画 removed 完成回调，
  再延后一轮 touch-up 处理恢复。iOS 16 用 0.75 秒保守尾段，均用代次防止旧回调解锁新拖动。
- 目录内容和标题栏控件禁用，保留 ScrollView 及外层侧滑；行切换、草稿、新建、分组、
  更多菜单动作再检查实时互锁，防止已按下控件在禁用后仍提交操作。
- 保留 156 消息列表纵向互斥和 155 用户认可的页面样式。

## 首轮修复验证

- 157 的连续六次行内短拖、从行上长拖关闭、A 草稿/会话保留以及停稳后明确点 B
  **通过，32.276 秒**；与基线同一条行内短拖，未再误切换。
- 保留的纵向互斥用例 **通过，18.853 秒**；视频分屏/全屏与图片往返 **通过，20.596 秒**。
  `/private/tmp/maichat-session-touch-after-20261102.xcresult`。
- 该轮另一个正常会话测试失败：打开列表后约 0.36 秒立即点 B，操作在动画保护期被忽略。
  8 秒后层级快照中目录仍展开，B 行和新建控件均已启用，说明停稳恢复有效。
  未把含失败的整轮记为通过；正常点选用例改为等待控件 enabled && hittable 后再点，
  防误触用例仍保留原始拖动，不放宽会话/草稿不变断言。

## 最终验证与产物

- 按 enabled && hittable 等待停稳后，正常 Root 的开合/会话切换/草稿/键盘/导航/横竖屏
  回归 **通过，41.309 秒**。`/private/tmp/maichat-session-touch-ready-20261102.xcresult`。
  产品代码未因这项测试时序调整再次修改，前述三个目标用例的通过结果仍适用。
- 已查看连续拖动录屏采样、基线误切换末帧、保护后的明确点选 B 截图。六次短拖及长拖
  关闭前仍为 A，只有停稳后的明确点击才切 B，A 草稿保留。
  `/private/tmp/maichat-session-touch-verified-20261102.png`、
  `/private/tmp/maichat-session-touch-after-20261102.mp4`、
  `/private/tmp/maichat-session-touch-ready-shots-20261102/`。
- 已通过 `imcli send-video` 向当前用户 `whq-iphone` 发送 8 秒实际录屏
  `/private/tmp/maichat-session-touch-preview-20261102.mp4`，展示在行上拖动而不改变会话。

真机 arm64 Debug **2.2.4 (157)** 构建成功，App 深层严格签名及 10 个嵌入库独立签名
通过。主程序 SHA-256：`0768486a69cd0db03e31e95d7b092bbb5bbec6fac58adca3a83532b9a6443005`。
清单 `/private/tmp/maichat-ios157-artifact-20261102.json` 记录源码 HEAD、产品补丁与分页源码哈希。

01:01 CST 已原位安装并回读确认 **2.2.4 (157)**，目录为
`8A9F30B1-910B-4F56-B10A-87AAC31E2A6B/MaiChat.app`。
`/private/tmp/maichat-ios157-installed-20261102.json`、
`/private/tmp/maichat-ios157-device-app-20261102.json`。安装后再次核对，产品补丁哈希未变。

使用 `--no-activate` 启动时，系统拒绝，CoreDeviceError 10002 / FBSOpenApplicationErrorDomain 7，
明确原因为 **Locked**；进程列表没有 MaiChat。启动未完成，已请求用户解锁保持亮屏后继续。
`/private/tmp/maichat-ios157-launch-20261102.log`、
`/private/tmp/maichat-ios157-device-process-20261102.json`。

触摸交互验收目前为 iOS 26.5 模拟器，尚未完成真机启动与触摸验收；iOS 16 的兼容完成等待
未实测。Android 无连接设备，未验证。没有清理用户 App 数据、Keychain 或媒体，没有提交、
推送或部署服务。
