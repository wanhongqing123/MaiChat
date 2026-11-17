# iOS 消息列表右滑误进入与群聊返回

2026-11-16 14:54 起，iOS 26.5 Simulator，正常 RootView 独立 bundle com.maichat.review.nav.MaiChat。parent HEAD c3ea0bbb，保留既有 Android/远端页/播放器 WIP；不把本地树说成已消费远端全部 PC 提交。

## 复现与根因

用户最初报告好友聊天右滑返回失效，后补充当前又恢复；再明确“消息列表右滑反而进入某个好友”。先核原普通导航：从左边缘返回、缓存聊天复进、从正文中间返回、键盘弹出后返回并保草稿均实际触摸通过。没有据此声称用户间歇失效已定位。

明确反向用例：消息列表好友行内，从宽度 10% 拖至 90%，松手仍在该行。原 plain Button 会打开聊天，chatMoreMenu 出现，普通 XCUITest 实际 RED 1/1（不是原生方法模拟调用）。改为真正轻点识别，默认无障碍 action 与轻点复用原选择动作；同例 GREEN。好友与群聊行同一边界，保留 List 滚动、清空侧滑。

补群聊真实触摸回归时，群详情右滑也实际 RED。群历史独有 simultaneousGesture(DragGesture) 会参与竞争；好友页没有这条。改为复用现有 MessageScrollPositionReader 的 onUserScroll，取消额外 SwiftUI drag；同一用例 GREEN。未改 ChatNavigationController 的转场/缓存/关闭逻辑，也没有全屏截图或用户消息读取。

## 验证

最初列表 RED1 归档 list-swipe-baseline.xcresult。好友返回基线和再进/键盘基线分别通过；群场景失败与改后相同用例 group-scroll-candidate.xcresult 已保留。候选两条 UI 测试 2/0（共 83.972 s，含编译/启动），实际覆盖：右滑不误开好友/群聊、真点击能进入、群返回、原左滑清空弹确认再取消、长列表上下滚动、缓存好友再次进入、正文右滑、键盘/输入草稿保留。实际截图已亲看，正常 App 录屏及分段帧已核页面连续移动。

首轮扩展测试因缺 --typography-ui-test 前置导致群夹具不存在，按已有夹具参数修正；保留原日志。群阅读追加补测第一次与 device build 同目录并发导致 Xcode build.db 锁，属于构建调度失败，已改串行重跑，不当产品 RED。

## 边界与归档

全过程仅自有 Simulator 内存消息。默认无障碍激活保留静态审查，未声称 VoiceOver 实机验收。未跑 Android 平台；本变更没有 Android 源。普通 iPhone 安装经用户持续授权，采用正常 bundle 保数据覆盖；安装/实际版本/进程最终另存回执，不将本地产物 SHA 冒称独立读回设备二进制 SHA。

证据目录：/private/tmp/maichat-ios-swipe-back-20261116。

最终普通 UI 同组 3/0（70.754 s；含构建/启动 driver 100.059 s）。补入实际群历史滚动、阅读旧消息时追加一条保持锚点、回到最新群消息按钮、返回列表再进入。60 条样本和追加按钮仅在已有 AppBars 的独立 Simulator 测试参数启用，不发送/读取线上消息。

217 正常 Release device build 0（118.110 s），签名严格校验 0，bundle com.kongshang.maichat / 2.2.4 (217)，传给安装器的主程序 SHA256 449f6224240734de439cf77e647733344308dce3fb7728927e3b8ae0e3827371。首次安装器在 0.345 s 返回 CoreDeviceError 4016；fresh device list 显示目标 physical iPhone unavailable。没有安装成功，也未卸载/重置/改变配对或设备设置。已请求用户连接并解锁，等待恢复。

house-maichat 已独立核最终 19339 B/SHA256 ad7036d9e5bcbcb2f6e9213de6d8879d6c484ae3d4484ad85deed7b773c629be 的四个 preimage，apply 一次后逐 postimage 核等，并提交 a924e217（远端回执）。旧 13426 B 未叠加。合源后再次只读查询，目标 iPhone 仍 unavailable；没有再次提交安装，也未改配对/TCC。

2026-11-16T16:06:30+08:00 用户再次明确“安装到真机”。fresh connection 从 unavailable 恢复为 paired/disconnected 后，使用原完整正常217候选再次保数据安装成功 0（12.787 s）；正常无测试参数启动成功 0，PID 67368；fresh apps metadata 核 2.2.4 (217)。候选主程序 SHA256 449f6224240734de439cf77e647733344308dce3fb7728927e3b8ae0e3827371 与安装前一致；没有把本地候选 SHA 说成独立读回设备 SHA，没有改配对/TCC/账号容器。三条完整触摸 UI 回归为 Simulator 3/0，手机本次核安装/版本/实际进程，不冒称已在用户会话完成全部手势验收。
