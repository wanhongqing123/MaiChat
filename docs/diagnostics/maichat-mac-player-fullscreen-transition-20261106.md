# Mac 播放器全屏与窗口反复切换跳变

2026-11-06 23:34 起；用户说明 Windows 正常，Mac 在放大、全屏、还原时窗口不规整，
视频先缩成小块再放大。按 14.268 秒现场录像逐段检查，约 2.75、6.75、9.25、10.25 秒
发生全屏/窗口切换，部分中间帧出现很小的视频矩形。

复现操作：在正常云盘/IM 播放器打开同一 HLG MOV；保持普通窗口，连续点击最大化、
全屏、退出全屏和还原；也覆盖进入过程尚未完成时再次切换。预期：只有一次连续的
窗口转场，视频始终填充当前视口并等比显示；退出后恢复进入前的窗口位置和尺寸，
不经过 480×270 最小窗口，不重新打开或重播视频，保持暂停/进度/倍速/音量状态。

本机起始源码 d7165212（2.2.52 发布），读取并同步 house-maichat 的独立八文件 UI 提交
ace8651 后排查，不修改其 AgentChatPanel/AgentSessionList。当前运行的正式 App 为
/Applications/MaiChat.app 2.2.52，PID 96951，主文件 SHA256
2e41d88a070a8be9b2fbc07ef84de16053f68a2ac5e7121049a54a0304b5aed9（fa95 候选）；
播放器实现与 d716 相同，不停止或替换它。后续复现使用独立自有播放器测试进程。

现场素材原件为既有用户附件 144115250159782623-1793896724-627349435.mov，
13.005 秒、HEVC 1920×1080、BT.2020 / HLG；新录屏为手机拍摄屏幕，不能当作源视频。
只在本地使用这些附件，不放入源码或公开测试产物。QA 目录：
/private/tmp/maichat-mac-fullscreen-transition-20261106/。

初步调用链：GUI、键盘与工具共用 MaiFfplayVideoDialog::applyFullscreen，Qt showFullScreen /
showNormal；Mac HDR MP4/MOV 使用 MaiMacVideoPlayback / AVPlayerLayer，普通帧使用
CAMetalLayer，Windows 为不同原生呈现后端。需要用 Qt/NSWindow/NSView/显示层几何
采样确定异常，不能仅因平台不同就归咎于系统。

## 根因与修复

原生显示层采样同时记录 Qt/NSView 目标尺寸、父层 presentation bounds、AVPlayerLayer
实际 presentation frame 和 animationKeys。原实现的最大化/还原同用例有 110 个动画
采样，子层与当时父视口的最大宽度差 758.36056px；Qt 和 model layer 已经是目标
1512px，实际视频层仍约 753px。`MaiMacVideoPlayback::resize` 独立赋 frame 触发了
Core Animation 隐式动画。现将 frame/contentsScale 放在禁用 actions 的同一事务中，
继续使用原 AVPlayer/图层，不重新加载视频、不调整 HLG/PQ 色彩或亮度映射。

连续四次快速全屏输入还暴露原生状态和 Qt 目标标志不同步：AppKit 转场未完成，
新的 showNormal/showFullScreen 反向请求已发出。现按 WillEnter/DidEnter/WillExit/
DidExit 观察原窗口，保留最后一次用户意图；完成通知先回到 AppKit 主 dispatch 队列，
再交 GUI 执行下一次请求。只用 Qt 队列推进的候选曾在压力用例停在 WillEnter，
这些失败日志保留，未作成功交付。观察器不替换 Qt delegate，释放后取消迟到回调。

退出全屏尚在进行时最小化、或进入时关闭，也会打断后续窗口的 Space 转场。最大化、
最小化和关闭按钮均通过同一个状态协调入口，退出完成后才执行排队动作；关闭前保留
原窗口和图层。后到的默认视频尺寸/位置不能覆盖用户已调节的窗口。

实际录屏发现透明 Tool 控件会以旧尺寸浮在转场快照上。最终保留原持久 Tool 窗口，
在原生转场中仅将 alpha 置零，完成后恢复；没有销毁/重建控件。将其改成 Cocoa
子视图、或转场中 orderOut 的候选未通过，均已撤回。

云盘等待首帧不再复用红色错误横幅，改为 44px 居中旋转环，鼠标可穿透；首帧到达
隐藏并停动画。真实错误继续使用错误说明；暂停/seek/倍速状态不依赖加载控件。

## 本机验证记录

macOS 27.2 / Apple M3 Pro / arm64 / Qt 5.15.18，独立 Cocoa 测试进程，未操作现装
App。先合入 ef31e763 的远端工具呈现改动，再构建真实 maichat、播放器、云盘和
SessionSync 宿主目标。

- `final-product-regressions.log`：13 PASS / 0 FAIL / 0 SKIP，35.827s。包括连续
  最大化/全屏/还原、转场关闭后重开、退出时立刻最小化、HUD 键盘/切窗、真实 IM
  MainWindow 宽窄布局、HLG/PQ、首帧后尺寸保留、菜单/音量弹出时关闭。
- `final-actual-hlg.log`：用户实际 1920×1080 HLG MOV，同用例 3 PASS，12.208s；
  最大子层/父视口宽差 0px、额外视频层动画 0，恢复 (100,120) 740×440，暂停进度、
  倍速、同一图层身份均保持。
- `final-product-capture.log`：合成 HLG 素材，独立 AVFoundation 录屏，3 PASS，
  13.413s，同样 0px/0 动画。不是把 QWidget::grab 当作实际原生视频证据。
  逐张 QScreen 抓屏会阻塞 GUI 并放大转场问题，最终录屏改用独立进程；只在隔离
  测试桌面显式启用，系统 Space 动画可能带入系统壁纸，发布前需查看全部录屏。
- `merged-session-host.log`：house 的最终远端工具组宿主 53 PASS / 0 FAIL / 0 SKIP，
  33.649s；没有修改其独立七文件实现。
- 补全观察器释放时取消主队列迟到回调后，`final-lifetime-regressions.log` 再次
  13 PASS / 0 FAIL / 0 SKIP，36.187s。`cloud-final.log` 为正式 CTest 的 Cocoa
  云盘回归，6.67s PASS，加载图标、真实首帧后隐藏和再次打开均通过。
  测试只截图加载控件；原生视频沿用独立录屏证据。一次手工传相对素材路径的
  非标准调用未上传成功，改用仓库注册的绝对路径 CTest 后通过，保留原失败日志。

QA 原始日志、几何 JSONL 和隔离素材录屏在上述 /private/tmp 目录，不提交用户素材。
原有整套播放器扩展验收中的原生分享/保存等历史边界不随本次局部回归宣称全部通过；
本次不宣称已经完成跨设备实际 HDR 亮度一致性验收，不改公开 v2.2.52 资产。
