# Mac 共用播放器：HUD、全屏关闭与复开

2026-11-14 至 2026-11-15，macOS 27.2 arm64、正常 Qt 6.12 Cocoa cache。最终R4的Mac必要原生前台组已收口，完整验收记录见末节；尚未替换用户App或发布新产物。

## 操作与预期

从 IM 或创作中心的普通 MainWindow::openVideoWindow 打开视频，放大、切回主聊天、全屏关闭、再打开。视频和控制条同步；主窗在前时 HUD 不独自盖住主窗；关闭不先展示还原窗口，再次全屏仍收到原生 DidEnter。按钮 hover 不出现红/灰底，保留按压和无障碍名称。

所有入口复用 MaiFfplayVideoDialog。SDR Graphics Metal 与 HDR AVPlayer 是既有后端；没有新增播放器，没有改 Graphics/Media 解码、Metal resize 或后端选择。

## 来源与用户数据

基线 beee29f4，曾消费 a7012733、3908b464，当前安全 ff38252959。各实际构建保留当时 HEAD+exact overlay，不能用后续 HEAD 倒填。独占四路径 overlay、八份旧 docs、原 index 均保留。协调端核 v2.2.58..a701 视频/Graphics/Media 无差异，几何通过不代表旧版本之后已修。

现装 Mac 只读基线2.2.58、历史 PID45041、exe SHA256 9d29c92ffdb37b719ad359d7fda9d581186b8f38b5e0ee00526c96dcdc434588。未退出/替换/播放真实影片或读账号 DB。仅自有视频、FakeIM、私有 CF home、拒绝测试出网。不请求 TCC、不录用户屏幕，QWidget.grab 不充当原生连续帧证据。

## HUD 原生层级 RED→GREEN

NSWindow.windowNumbersWithOptions:0 包含 Tool panel。原 footer1/header2/main3/player4，HUD level3、主窗/播放器 level0，主窗 raise 后 HUD 仍浮前，原断言 RED。

保留同一 Qt HUD/transient owner，将 HUD 限为 owner 同层；仅在真实倒序或外窗插入时恢复紧邻顺序，不越过盖住 owner 的主窗/Popup。同例 GREEN。NSChildWindow 中间方案导致边缘/退出全屏回归，已撤回，原证保留。

## R1 关闭的真实复开缺陷与 R3

原稳定全屏关闭先 showNormal，immediatelyHidden=false，同例 RED。R1 稳定全屏直接 close，单次隐藏 GREEN，原组曾 Qt6/0、共享入口 Qt8/0。但 R1 不是最终可交付方案。

解锁后追加同进程复开：首次 WillEnter/DidEnter→关闭隐藏，第二次仅 WillEnter、无 DidEnter。creation-reopen-unlocked/cases.txt 真实 RED24.109s。不能把早先锁屏前提失败或单次关闭 GREEN 当复开通过。

R3 保留原 fullscreen 退出生命周期：先请求退出，随后对自己的 closing NSWindow 设置 animationBehavior=None 并 orderOut，不展示还原画面；对象保留到真实 DidExit，再沿原 WA_DeleteOnClose 清理。closing 不重显 HUD，不销毁重建或用截图/opacity 假装关闭。

close-with-native-cleanup/cases.txt Qt3/0、4.959s。两个实际循环均有 WillEnter→DidEnter→关闭后 native hidden→WillExit→DidExit→删除→复开。生产只改 Dialog.cpp/MacLayer.mm，另两源是自有测试/原生属性取证。

## 最后必要回归仍 HOLD

r3-original-native-regression 正常目标 build0/10.487s；关闭前原生前提 active=0、frontmost=com.apple.loginwindow。随后 HUD/边缘/fullscreen 多项失败，私有测试75s deadline exit124。全部原失败保留，不说已绿，不盲改或绕过锁屏。

R3 两循环 GREEN 只证明局部生命周期；仍需解锁时补原边缘/退出全屏/共享入口影响组。未扩跑无关 UI、付费模型或真账号。

持续 resize 后停旧左上尚未稳定复现：四尺寸、九边缘的实际 SDR Metal model/presentation/drawable 相符，不能证明连续原生像素帧。HDR 自有 HLG 原槽在最后组因前提未完成。没有宣称持续 resize 已修或 Mac 原生连续帧验收。

公开260/261及现装 App 未动。源码哈希、失败和成功原日志保留于 /private/tmp/maichat-macos-video-resize-20261114-s3jdm63r。

## 2026-11-15 R4：补齐 tooltip 与独立 HUD resize 能力

用户原截图还有关闭按钮“关闭视频”的文字 tooltip。R3只清背景 hover，文字仍会弹。正常共享 Dialog 用真实QHelpEvent，在测试窗口显式允许后台 tooltip（防止环境不显示导致假绿），原槽真实RED1；只在共用 eventFilter 消费 ToolTip，保留动作说明和 accessibleName，同槽GREEN。

另一条直接原生证据：Qt Cocoa qcocoawindow.mm 的 windowStyleMask 默认给所有非Popup窗添加 Resizable，包括本播放器两个Qt::Tool HUD。实际标题HUD nativeResizable=true、owner仍480宽，新增原生能力断言RED。用户截图中视频/标题旧宽、底部控制条独自变大，与此独立窗口能力缺口一致；这仍不是原用户原生拖动/合成画面的完整复现证明。

仅在原有Mac HUD对齐函数清除两个HUD的Resizable，owner保留true；不reparent、不NSChildWindow、不更换窗口。Qt的QNSPanel canBecomeKeyWindow对Tool/Dialog明确返回YES，不依赖该Resizable位，键盘/焦点策略保留。同原生例HUD false、owner true通过。

为避免仅用layer几何替代视频内容，本轮仅在自有测试进程临时观察/恢复CAMetalLayer.nextDrawable原IMP，按实际播放layer身份跟踪，Metal blit读回其GPU texture。既不截屏，也不读取其他App或请求TCC。暂停和播放中96次native setFrame后的1040×680、520×360、900×580：实际drawable尺寸和视频区12点RGB映射均符合预期。测试还要求surface与owner目标尺寸相同。独立负对照仅在测试内阻断renderer resize并扩大surface，明确失败“drawable640×400 vs surface1040×680”；证明观测不是盲的，不将注入故障称作产品RED。

HDR用自有HLG素材，明确检查实际ready的AVPlayerLayer.videoRect/bounds/presentation，非闲置Metal层。初轮fixture与250ms普通初次定尺寸竞争导致1失败，原日志保留；只等待原初次布局完成后，四原生尺寸全部通过，未为此改生产resize代码。

最后相关正常target实际8/0/0，包含HUD native capability、tooltip、原共用外壳、SDR暂停/连续resize两行与HDR videoRect。生产增量限Dialog.cpp与MacLayer.mm；Graphics/Media/iOS源不动。

R4原关闭/复开例仍在原前台前提active0处停止，未移除前提或伪造通知；此前R3真实两循环3/0保留。这是本地测试条件读数，不是用户反馈或原故障根因；不能宣称用户电脑锁屏，也不能把该条件冒充整件验收通过。现装App与发布包仍未替换。

## 最后原Mac前台组实际收口

同已审FULL4冻结源，没有新增/跳过断言。r4-final-native-foreground正常build0，
原八selector/十data实际Qt12/0/0，37.436s（含init/cleanup）。
实际own foreground前提通过；完整关闭重开两循环均真实DidEnter→立即native hidden→DidExit/delete→再开；
主聊天/HUD原生Z序、九边缘（含600×350）、HUD按键、标题拖动/resize/min/max/fullscreen、
初次max/fullscreen两行、IM宽窄主窗两行全部通过。
HDR三次max/fullscreen还原 worstPresentationMismatch=0、animatedFrames=0。
四源bytes/SHA精确同FULL4，原本地active0失败历史保留，未改变前提或伪造通知。
Root同时收到原log/run/build和final-mac-safe-receipt.json。R4 Mac必要功能门完成；
不据此声称连续屏幕像素录制、其它系统/显示器或用户真实影片都验过。

用户已明确要求立即发布正式桌面产物，Root协调最终同源commit及Win/MacRelease窗口；
Mac按最终源码正常打包，不能将未含R4的旧b16包改名。现装Mac不退出/不替换。


### Final Desktop 2.2.62 source and normal Mac artifacts (2026-11-15)

The final source is facf79126d7dbd149a326bb83cb3ac24d05cc184. Its four R4 video blobs exactly match the source used by the final native foreground 12/0/0 run and the prior own-GPU/HDR 8/0/0 run. The safe fast-forward preserved 75 local paths and the original index, and all 12 frozen compile subtrees match the coordinator manifest.

The normal Release configure, App build, DMG/ZIP packaging, three-copy verification and six actual App smoke processes completed with 22 recorded exits of zero. Staging, read-only mounted DMG and extracted ZIP have 338 identical regular files and 37 identical symlinks; all 26 Mach-O dependency audits and strict signatures pass. The five model files match the v2.2.58 reference bytes and SHA256. These are newly built 2.2.62 artifacts, not renamed earlier payloads.

The necessary newly imported ordinary Cocoa consumers also pass against the same normal libraries: Desktop bridge 54/0, binding UI 10/0, relevant MainWindow selectors 5/0, accepted non-SDK-friend profile refresh 3/0 and actual Agent tool registration 3/0. No paid provider, real SDK message, user database, TCC request or screen recording was used.

The package uses ad-hoc signing without notarization. Actual runtime testing is macOS 27.2 Apple Silicon; dependency minimum-OS metadata includes versions higher than the bundle's 14.4 label, so this is not an older-OS compatibility claim. The installed Mac application was not stopped or replaced.

- MaiChat-macos-arm64-v2.2.62-20261115-facf7912.dmg: 214590201 bytes; SHA256 c5efc7b5247cb5b0d5b3e25586dece3ce2c22fb090cda88c23074811595820bd
- MaiChat-macos-arm64-v2.2.62-20261115-facf7912.zip: 215114865 bytes; SHA256 53189ce985fe27c84f3ec43197d28dd6504133078da1196b40865987c32f8a1d
- release-manifest-macos-arm64.json: 21231 bytes; SHA256 41fcecf14e2a9f91a8bed34309c280948071a94a88a60ce4f831a193c842d623
- SHA256SUMS-macos-arm64.txt: 332 bytes; SHA256 1221c1c9acb3b9e36019042ef4a4c0aa45bdd6a9b7e665bd87906556f42c1fc5

### Final Mac four-asset upload

All four Mac assets have been uploaded to the unique coordinated v2.2.62 draft ReleaseId 445671715 and individually read back with exact size and API SHA256 digest. The original system-LibreSSL DMG upload failed with curl56 after 81,657,856 bytes; a fresh release readback proved there was no partial asset. Under a separate exact two-POST continuation lease, existing OpenSSL curl uploaded the unchanged DMG and ZIP once each with HTTP201. The initial two small assets were retained without another POST. Original transport failure, renewed scope and successful receipts remain separate.

The annotated tag object 5c282470c3f6fc41ff9aac4ece048153695a1cc5 was independently read back as peeling to facf79126d7dbd149a326bb83cb3ac24d05cc184. Public status is still pending the coordinator's Windows four-asset completion and single publish operation; no claim of public availability is made from a draft upload.

### Public v2.2.62 closure

The coordinator published ReleaseId 445671715 at 2026-11-14T23:53:02Z. A separate official GitHub API readback confirmed draft=false, prerelease=false, latestId=445671715, eight public uploaded assets, the annotated tag peeling to facf, and all four Mac assets with exact original sizes/SHA256 and final v2.2.62 download URLs. The prior v2.2.61 release remains public with its original ReleaseId and eight assets; the coordinator separately verified its unchanged body and digests. The first gh latest GET ended with EOF; the successful independent readback used the official connected GitHub API and did not repeat any POST or local tests.

Public release: https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.62

The desktop release request is complete. Android 280 interface work remains the Windows coordinator’s independent task; it was not a condition or content change for this desktop release. The installed user Mac application remains untouched.
