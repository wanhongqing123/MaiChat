# 创作中心与 IM 媒体连续滑动：2026-11-01

- 开始于 09:20 CST；源码 `main`，HEAD `1997947219596e6520be27ed892bd6ea1820ff2e`
  加前序未提交媒体修复。运行基线为 iOS 2.2.4 (150)。
- 用户操作：在创作中心或 IM 打开图片/视频，左右拖动浏览上一项或下一项。
- 预期：手指拖动时旧项和相邻项在同一容器中连续移动；松手后约半秒平缓完成，
  没有整屏空帧、标题/导航泄漏；短距离反向拖回不切换，不重播原视频；
  图片放大后的拖动仍只平移图片，进度条等控件不触发翻页。
- 定位：视频 `viewDidAppear` 只对新页做 48 pt / 0.22 秒位移淡入；创作中心
  图片切视频先清空图片状态再新建视频；IM 图片切换只淡出旧快照，切视频又关闭
  整个图片 modal。这些路径没有一个持续保留相邻两页的容器，单独放慢淡入无效。
- 修复方向：本地图片/视频复用一个稳定 UIKit 分页容器；相邻页提前准备图像，
  手势中保留两页；只在提交视频切换后交接唯一 FFplay 实例，取消拖动不打断播放。

## 验证记录

- 已重新录制并查看 150 正式 Root 和 IM 图片宿主实际切换：
  `/private/tmp/maichat-media-slide-before-20261101.mp4`，旧内容在手指松开后被直接替换，
  没有两页并排滑动。与新容器首轮录屏用同一合成蓝/橙图片检测，在 60 fps 中线
  像素采样下，旧版无同时保留两种完整颜色的过渡，新容器连续 35 帧（0.583 秒）。
  `/private/tmp/maichat-media-slide-before-trial-pixels-20261101.json`。
- 新增共享 `MaiLocalMediaPager`：拖动实时移动前后两页，松手 0.50 秒整页完成，
  取消 0.40 秒回弹；仅保留当前和相邻两页。邻居视频只准备封面，提交时才交接
  唯一 FFplay；旧视频停止解码但保留 Metal 呈现层至整页滑出，避免黑闪。
- 创作中心、IM 图片 modal 和 Root 的视频入口共用该容器；移除原先多处
  48 pt / 0.22 秒新页淡入、图片 modal 重开和切换媒体时重建 Root 播放器的路径。
  保留点击图片打开/回缩、捏合、双击复位、播放控制和 Agent 直接控制。
- 首轮发现测试宿主未注入 `RemoteIMAppState`，崩溃栈为
  `EnvironmentObject.error → AIAssistantView.content`，已补齐模拟器宿主环境。
  另修复全屏原生宿主的无障碍模态标记，并在关闭/销毁时释放标记和触摸。
- 后续 10 项回归中，分屏/全屏保留暂停位置、IM 混合浏览、缩放后拖图、
  短拖取消保持暂停/倍速、两个视频往返、独立播放控制等 8 项通过，
  两项创作中心退出后的导航触摸检查尚失败，未交付此中间构建。
- 定点日志证实关闭动画 0.385 秒完成，模型清空、Root 可见性 false、原生宿主
  dispose 均已执行；实际返回截图的导航也可见。排查集中在额外添加到 Root 的
  触摸/无障碍遮蔽，而非延长等待时间。生命周期证据：
  `/private/tmp/maichat-media-slide-focus-lifecycle-20261101.log`。

## 最终模拟器验收

- Root 去掉图片模式重复施加到主页面的触摸/无障碍遮蔽，改由原生分页容器单独
  隔离模态焦点；视频展开的遮蔽继续保留。此前只在全屏期间查不可点击，会漏掉
  关闭后的恢复，因此保留了进入与关闭两侧的检查。
- 同一失败路径回归 **2/2 通过，42.614 秒**：创作中心两轮图片/视频往返并关闭、
  两轮原图放大/回缩并恢复五个导航按钮；源缩略图坐标和尺寸误差不超过 2 pt。
  `/private/tmp/maichat-media-slide-modal-20261101.xcresult`。
  已人工查看其展开与最终返回截图，源图位置、状态栏和导航恢复正常。
- 交付录屏使用该修复后的实际 App：正常 Root、IM 图片宿主、共用视频入口
  **3/3 通过，45.362 秒**，
  `/private/tmp/maichat-media-slide-delivery-20261101.xcresult`、
  `/private/tmp/maichat-media-slide-delivery-20261101.log`。
  录屏 `/private/tmp/maichat-media-slide-delivery-20261101.mp4`。
- 已人工逐帧查看图片→图片、图片→视频、视频→图片三种过渡，原项平移出屏时
  邻项同时移入，没有整屏清空；视频的旧 Metal 画面在解码停止后仍保留到滑出。
  联系表分别为 `/private/tmp/maichat-media-slide-delivery-pictures-20261101.png`、
  `/private/tmp/maichat-media-slide-delivery-image-video-20261101.png`、
  `/private/tmp/maichat-media-slide-delivery-video-image-20261101.png`。
  最终图片换图片的 60 fps 像素检查为连续 34 帧（0.567 秒）并排可见；
  `/private/tmp/maichat-media-slide-delivery-pixels-20261101.json`。
- 其余已通过的定点回归覆盖：正常 Root 分屏/全屏不重播当前暂停视频、输入栏和
  五个主入口恢复、两个视频前后浏览、短拖取消保持暂停进度/倍速、图片放大后的
  横拖不翻页、双击复位后正常翻页、独立播放器暂停/seek/关闭。
  记录在 `/private/tmp/maichat-media-slide-ios151-final-20261101.xcresult`；该轮原先
  失败的两项 Root 关闭检查以上述 2/2 回归结果为准，不把原轮整体记作通过。
- 最后补核初始视频缺失：封面不能遮住正式播放器的错误反馈，VOD 的非本地显示
  路径不触发无关的封面错误；仍能滑到下一张图片并关闭。
  **1/1 通过，9.158 秒**，
  `/private/tmp/maichat-media-slide-missing-video-20261101.xcresult`。
- 11 个不同的目标用例已分别通过；验证采用模拟器中的正常 Root、正式 IM 图片
  控件和本地免费素材，不发真实 IM 消息、不触发模型任务。未做两个真实账号的
  收发网络验收，也不将这些结果替代真机上的手势体验验收。

## 真机产物与安装条件（09:57 CST）

- 真机 arm64 Debug 版 **2.2.4 (151)** 构建成功；逐一验证 App 严格深层签名和
  10 个嵌入框架/动态库的严格签名。主程序 SHA-256：
  `ead701cc3d2ed94daaaa73531bb8ab449cc922c518550f461bdeed0ccbd8d3ca`。
  产物 `/private/tmp/maichat-slider-ios-device-derived-20261031/Build/Products/Debug-iphoneos/MaiChat.app`。
- 清单 `/private/tmp/maichat-ios151-artifact-20261101.json` 同时记录源码 HEAD、
  iOS 已跟踪补丁 SHA-256 `24289c77325559f3807be49ae7da86cb500bbdea93b85b9e0df75b02631915de`
  和新增 `MaiLocalMediaPager.swift` 的 SHA-256
  `6455b88feca74827e7da2eceae6058307444ef558224ce43740a9d0adead5f58`。
- 按既有更新授权尝试安装，但设备服务立即返回 **CoreDeviceError 4016**，
  `CurrentlyAssertableStates` 为空；设备清单中 iPhone 14 Pro Max 为 **unavailable**。
  `/private/tmp/maichat-ios151-install-20261101.log`、
  `/private/tmp/maichat-ios151-devices-20261101.json`。
  **151 尚未安装或启动，真机手势未验收**；已请求重新连接 iPhone 并解锁后继续。
- 本轮没有清理 App 数据、Keychain 或用户媒体；没有重启系统设备服务或干扰其他
  模拟器来恢复连接。当前新增源码和相关改动均留在 main 工作区，未提交、未推送。
- 最后检查 Android，`adb devices -l` 没有连接设备或模拟器，Android 未验证；
  `/private/tmp/maichat-media-slide-android-availability-20261101.log`。
