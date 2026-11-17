# 移动播放器横屏与导航取消误报

## 授权与范围

2026-11-17 用户要求 iOS、Android 播放器支持横屏，明确自行完成、不联系 house-maichat；随后补充快速反复点击底部五个导航时，会在云盘页看到“获取好友申请失败，请稍后重试”。本轮未向 house-maichat 发送消息或文件，未新增第三方依赖、驱动或修改目录方案。

预期操作：播放视频时直接点方向按钮进入横屏，再点返回竖屏；暂停、进度及同一播放会话保留。关闭视频恢复首次手动切换前的方向或请求。连续切换底部导航属于正常取消，不应在新页面弹旧好友请求的错误；仍停留在好友页的真实失败必须提示。

## 切页误报的实际证据

`ContactsView.task` 顺序调用好友列表和申请刷新。离开页面会取消 SwiftUI 任务，但 `refreshAcceptedFriends` 与 `refreshFriendRequests` 原来都把 CancellationError 当一般失败；第一步取消返回后还会继续第二步。账号 HTTP 的 libcurl bridge 只在开始前查取消，回调后也需要宿主拒绝过期结果。

通过可注入的两项普通列表 loader，在隔离 RootView 的真实底部导航中控制请求挂起和取消，没有请求线上账号或读取实际消息。旧代码实际 RED2：取消列表阶段累计两次好友错误，取消申请阶段一次，UIKit 用时 22.427 秒。真实可见失败的对照测试本来就通过。

修复在请求前、结果应用前、catch 中检查取消及账号身份，单独处理 CancellationError；ContactList 自动刷新和手动刷新两步之间检查 Task.isCancelled。默认 loader 仍是原账号客户端，没有改鉴权协议或错误响应。相同两项 UI 用例 GREEN，真实可见失败仍会提示。

## iOS 横屏

Info.plist 已声明横竖屏，原播放器缺少手动方向入口。本轮增加方向按钮，通过系统 [UIWindowScene.requestGeometryUpdate](https://developer.apple.com/documentation/uikit/uiwindowscene/requestgeometryupdate(_:errorhandler:)) 请求横屏/竖屏，没有替换播放视图或重建 FFplay 会话。播放控制使用横屏安全区边界，旋转动画完成前不接受重复点击。

本地与云盘分页容器各持有同一个方向 scope，翻页继续持有，容器 dispose 才恢复。非分页独立播放器由自身 stop 恢复。这样视频/图片连续浏览不会在某一页退出时意外把整个媒体序列转回竖屏。

自有正常创作媒体页面的旧实现实际 RED1：竖屏握持时不存在手动方向按钮。候选的手动横屏、竖屏、再次横屏、关闭恢复及暂停时钟不变用例通过；强化等待真实视频视口完成重排后通过（20.165 秒），并与切页两项用例同跑 3/0。

测试截图曾显示错误裁切，开发者没有据断言绿交付。对照发现 XCUITest 的 app.screenshot 在手动 Scene 旋转时截取坐标不正确；XCUIScreen.main 与独立 simctl framebuffer 都显示完整 874×402 横屏，视频按原比例居中，控件完整。几何记录同时显示 app/video 均为 874×402。已亲看整屏图 `ios-landscape-framebuffer.png`。

边界：既有“只设置 XCUIDevice 物理方向”的自动旋转用例，在当前模拟器的候选和原播放器三源 baseline 中均失败。baseline 验证临时精确回放原三源，结束后 finally 已逐字恢复候选。本轮确认的是手动方向入口，不把该旧自动旋转用例称为通过，也未证明具体系统锁屏/模拟器原因。

## Android 横屏

方向按钮复用系统图标和原控制条。普通 `MaiVideoPreviewDialog.toggleOrientation()` 保存进入手动模式前的 Activity requestedOrientation，再使用 SENSOR_LANDSCAPE/SENSOR_PORTRAIT。Dialog.onStop 恢复自己仍持有的最后请求，不覆盖另一个后来设置的请求。MainActivity 的方向配置变化仍走既有 configChanges，不移动或重建播放器。

API依据：[Android Activity.setRequestedOrientation](https://developer.android.com/reference/android/app/Activity#setRequestedOrientation(int))。大屏/OEM 可能有系统限制，未把手机模拟器验证冒称所有设备通过。

Robolectric 原四项窗口/草稿/边距检查保留，新增普通方向 API 与后继 owner 保护。旧 API 反射查找 RED1（NoSuchMethodException），候选 6/0。默认 GraalVM JDK 的 jlink transform 失败，切换本机已经安装的标准 OpenJDK 17 后通过，没有下载或安装新 JDK。

以当前源正常构建 Debug App/JNI/测试 APK，arm64 构建成功（6 分 8 秒）。appId 为 `com.kongshang.maichat.isolated`，版本 `2.2.38 (280)`，IDE ABI 构建带 testOnly，安装使用 `-t`。这是真实产品入口和原生 FFplay/Graphics，没有用预制 APK 改包代替源码构建。

注意当前 AGP 新 APK 实际位于 `app/build/intermediates/apk/debug`，`outputs/apk/debug` 留有 11 月 6 日的旧产物；本轮核对了新产物版本、SHA 和设备安装路径，未运行旧包冒称新源。新 App SHA256 `89188068c1b376c1c5ad6bdcc4d84b34b163a2c03e0c19878fc1f25a2a58b58e`。

新建仅供本任务的 AVD `MaiChatLandscape_20261117`，复用已安装的 Android API 37 arm64 16K system image，不擦除原用户 AVD。先完成 iOS 模拟器和真机安装，再启动此 Android 模拟器（emulator-5568）。正常无参数 MainActivity 启动成功，已目视登录页。

真实 JNI 播放自有 gallery-sample.mp4：暂停后手动横屏，检查 activeView 身份、原生 playbackHandle、paused=1、进度误差小于 250ms；关闭后原 requestedOrientation 与竖屏配置恢复。首次 1/0 为 4.862 秒，但捕图仍是旋转中间态，因此加窗口重排/空闲等待后重验 1/0（11.336 秒），整屏稳定图 `android-manual-landscape-stable.png` 已亲看，画面、控制条和安全区正常。最终移除不适用于安全区 padding 的“必须等于整个Decor宽度”测试假设，改为实际横屏视口与layout完成；最终同例1/0，4.072秒，截图 `android-manual-landscape-final.png` 与已查看稳定图SHA相等。不是只凭测试绿判断体验通过。

## 产物与验证边界

本地 HEAD 仍为 `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`，保留全部既有 WIP；本轮 before 快照、精确源 manifest 和日志在 `/private/tmp/maichat-mobile-landscape-and-tabs-20261117`。没有把原 Android AssistantController/MessagePages 或其他桌面工作混进本轮源增量。

iOS 224 Release 构建成功，113.294 秒；deep/strict 签名 0。可执行文件 SHA256 `4b79a5c43ade9002d2b1d1bf6df3ea00ec822c6fabd925721403a106d18e1bcf`。iPhone 14 Pro Max（`00008120-001E0DEC0263C01E`）普通就地安装成功，新鲜回读 `2.2.4 (224)`，无参数正常启动 PID `69245`。未从设备独立读回 exe SHA，没有以安装成功替代真机触摸体验验收。

Android 没有连接实体手机，已验证源码构建、JVM 普通窗口和 API 37 模拟器真实解码。没有声称 Android 真机安装完成。两端未触发付费模型，没有读取用户数据库/凭据/实际消息验收，所有媒体素材自有。
