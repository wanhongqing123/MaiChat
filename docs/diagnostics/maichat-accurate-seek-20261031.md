# 精准 Seek：2026-10-31

- 用户操作：在 Mac 桌面视频播放器（包括云盘独立播放窗）拖动进度条，在暂停、
  正常播放和倍速下向前/向后跳到非关键帧时间。
- 预期：解码依赖从之前的关键帧开始，但不呈现目标之前的帧；落点误差不超过一帧，
  暂停时保持暂停，音频不回放旧位置；快速连续请求最终落在最后指定的时间。
- 开始时间：2026-10-31 22:16 CST，macOS arm64，源码 HEAD
  `1997947219596e6520be27ed892bd6ea1820ff2e` 加本地工作区。
- 本轮开始时 `/Applications/MaiChat.app` 未运行；上一轮安装主程序 SHA-256 为
  `7dbf6d67861e4e88dabd663c7a09e062d0639625606a0e140bf2ede3560a8d5a`。
- 初步调用链：桌面 `seekPercent` → `maiFfplaySeekAbsoluteUs` →
  事件循环 `stream_seek` → `read_thread` 的 `avformat_seek_file`。
  当前代码刷新 packet serial，但没有解码后丢弃目标之前的帧。
  原测试只检查有限百分比日志和命令接受，不能证明画面落点精度。
- 最小用例：8 秒 / 25 fps / GOP 200 的 H.264，亮度编码帧号，暂停后跳至
  5.240 秒，目标为第 131 帧；观测真正提交给宿主呈现回调的帧内容。

## 根因和修复

修复前真实回调显示第 **0** 帧，状态却显示 `5269299 us`，目标为第 131 帧 /
`5240000 us`。日志 `/private/tmp/maichat-accurate-seek-before-20261031.log`，用例红。
这证明 FFplay 的外部主时钟可以显示请求时间，但关键帧跳转没有保证画面时间。

共享 `MaiFfplayEntry.c` 在原有兼容层包装 `avformat_seek_file` 和
`av_buffersrc_add_frame`：限制关键帧不晚于目标，按 packet serial 发布目标，
在视频/音频进入滤镜前丢弃预滚内容。视频选择覆盖目标时间的帧，音频跨越目标的帧
裁掉前缀采样；因此保持硬件解码、HLG 元数据及倍速前的源时间语义。
100% 跳到末帧，暂停时状态取实际视频时钟；相对命令同样使用源时间。
`ffplay.c` 本轮未改，修改前后 SHA-256 均为
`83b4aae5ff0de00e3b7911c89eb59cda56425c9cb9d18570ed30c5b8db7423c4`。

桌面进度条改为毫秒分辨率，点击直接定位指针对应位置，拖动释放提交最终目标，
避免默认整页步进和固定 1000 格带来的量化。普通控件和 Agent 仍调用同一接口。

## 验证

- 同一红用例修复后回调为第 **131** 帧，暂停状态 `5240000 us`。
- 最终打包库回归：**11 PASS / 0 FAIL / 0 SKIP，8.975 秒**，其中八组数据覆盖
  SDR、含音轨半速/双速、云盘自定义字节源、指定初始时间、HLG VideoToolbox、
  HLG 云盘半速、非零源起点（1.280 秒，1.5 倍速）。每组检查前跳、后跳、
  非帧对齐时间、100% 末帧、零点、连续请求、相对命令、百分比和继续播放时的跳转。
  通过解码画面亮度编码验证源帧；倍速滤镜 time_base 的不足 0.1 ms 舍入按源帧核对。
  `DYLD_PRINT_LIBRARIES` 证实实际加载待安装包的 `libmaichat_ffplay_hosted.dylib`。
  日志 `/private/tmp/maichat-accurate-seek-packaged-regression-20261031.log`。
- 完整桌面播放回归：**19 PASS / 0 FAIL / 0 SKIP，17.348 秒**，含三种编码、
  音轨/字幕、初始时间、倍速可听输出、HLG/PQ、独立控制及本地视频分栏/全屏。
  这次发现旧分栏测试在新窗刚显示、HDR 探测未结束时立即发送暂停，现明确等待首帧
  后测试控制。未为此修改产品准备阶段逻辑。
  日志 `/private/tmp/maichat-accurate-seek-regression-final-20261031.log`。
- 实际画面截图 `/private/tmp/maichat-accurate-seek-window-20261031.png` 已查看：
  点击/拖动约 75% 后暂停，画面内烧录时间码为 **1.533 秒**，请求为 **1.535 秒**，
  滑块和 `0:01 / 0:02` 一致。此为正常 Cocoa/Metal 窗口截图，非 QWidget 的黑色代替图。
- 宿主、音频 tempo、音频会话、自定义输入、实际 HTTPS Range VOD、HLG GPU 输出
  **6/6 通过，7.01 秒**；云盘 UI 双击播放/关闭返回等 **1/1 通过，3.67 秒**。
- iOS arm64 实际 SDK/构建参数编译共享 `MaiFfplayEntry.c` 通过（仅两条现存
  FFplay enum switch 警告）。本轮没有 iOS/Android 真机或 Windows 实机验收。
- Release App 构建、打包及严格深层签名通过，70 个 Mach-O 的实际加载依赖不指向
  开发机路径（动态库自身 install name 不当作外部依赖）。裸 PATH 隔离 smoke exit 0，
  临时数据路径已回读确认，沙盒禁止网络和用户目录写入。
  第一次 smoke 的环境变量在 Apple 系统程序启动时被移除，虽然用户目录写入仍被
  沙盒阻止，未计为隔离通过；改由沙盒内部 env 设置后重新通过。

## 交付产物

- `MaiChat/desktop/dist-accurate-seek-20261031/MaiChat-macos-arm64/MaiChat.app`
- 版本 `2.2.32`，主程序 SHA-256
  `db3b2ccaf21aa2e0b5431b58039ef52f73d717fbf3d9d9f43d8a474468147d04`。
- FFplay 动态库 SHA-256
  `525f889c0dbd0e37455fc31d685e3fe7985f7d5b06263952bedd50367cd578b0`。
- Metal 库 SHA-256 保持
  `b7442a850ee0eb427d42df1b4e6e7af16f13da9168047a0c9f3cd1c35ac9bf8e`，保留上一轮 HLG/AMVE 修复。
- 清单 `/private/tmp/maichat-accurate-seek-artifact-20261031.json`。
- 已沿用用户此前同意的更新授权，于 **2026-10-31 22:34:38 CST** 安装到
  `/Applications/MaiChat.app` 并启动，PID **61441**。安装前正常 App 未运行；
  安装后主程序/FFplay/Metal 三份哈希及严格深层签名重新核对，启动日志回读确认
  实际 `/Applications` 路径和同一 PID。临时旧 bundle 已删除，未清理或迁移用户数据。
  安装收据 `/private/tmp/maichat-accurate-seek-install-20261031.json`。

本轮精度验收使用可追踪源帧的本地合成素材及本地 TLS VOD，未重新下载用户线上
28.676667 秒原视频，也没有修改服务端、云盘对象或媒体数据。对缺失时间戳/源时间
断裂的内容仍受原始时间轴约束。

## 22:38 后续：画面正常，只有滑块回跳（重新打开排障）

用户明确确认画面已正常定位，回跳仅发生在滑块，并要求一并核对 iOS。
上一轮测试验证了 seek 后的最终落点，没有检查松手后至新时钟生效之间的 UI 更新。
两端现有定时刷新仅在拖动过程中跳过滑块更新，松手后立即恢复读取实际播放时钟，
因此需要复现并修复这个异步窗口，不能用上一轮画面精度通过替代此问题验收。

复现：在现有诊断回调中暂缓播放事件线程执行 seek，让正式 UI 的 250 ms
进度定时器继续刷新；滑块应保持用户提交位置，直到本次 seek 的新队列时钟生效。
本次边界包括暂停/播放、前后拖动、后一个请求覆盖前一个请求、失败/关闭/重播。

### 滑块异步状态：证据与交付

- 使用当时已安装的 FFplay 库确定性复现：暂停时滑块 `1535 → 67 ms`，
  播放时 `1535 → 148 ms`；画面 seek 命令仍正常。两个用例均红。
  `/private/tmp/maichat-slider-bounce-before-20261031.log`。
- `MaiFfplayPlaybackStatus` 分开真实 `position_us` 和 UI 的 `seeking/seek_target_us`。
  目标在 API 接受时立即发布，等新 packet serial 的画面/主时钟实际生效才解除；
  旧请求的 CAS 不能清除新的目标。定位失败/停止时清除反馈。
  Qt 和 iOS 的滑块与时间标签都消费同一状态，不用固定延时猜测 seek 完成。
- Mac 源码 25 项播放回归通过（27.058 秒）；打包库的六种延迟前/后/连续跳转
  和滑块操作共 9 PASS / 0 FAIL / 0 SKIP（7.226 秒）。
  云盘 VOD 测试曾把 Range 开始当作 seek 完成，过早发送暂停切换；增加实际目标
  画面、新 serial 时钟及暂停就绪检查后，正常恢复播放与缓存检查通过（2.06 秒）。
  状态结构变化后所有测试调用方均重新构建，早期旧二进制结果不作为通过依据。
- Mac 已于 **23:01:19 CST** 更新启动，PID **51508**；主程序 SHA-256
  `299cd18c190b42918aecd06d87bd4ebdf26a6c58b459bef1703da70b3da53b00`，FFplay
  `49d29d54a6e8dde495a34b71343a3cab4cb5684ca82d59ab78176ee3f434bdf8`。
  收据 `/private/tmp/maichat-slider-bounce-install-20261031.json`，保持同一 App 和数据。
- iOS 模拟器实际拖动（含 4 秒诊断延迟）31.193 秒通过：暂停前拖/后拖保持目标，
  播放恢复后继续前进。`/private/tmp/maichat-slider-ios-ui-resigned-20261031.xcresult`。
  已查看三个附件；等待中时间标签显示目标而画面保留原帧，真实位置没有伪造。
- iOS 验收过程先遇到第三方 `nuisdk.framework` 的无效模拟器签名，再遇到
  SwiftUI `.task` 内 C 回调继承 MainActor 的隔离断言。逐个签名验证嵌入项，
  将验收回调移到 `nonisolated` 全局函数后才通过；此前失败没有计为通过。
- 用户要求安装最新版后，签名正常的 **iOS 2.2.4 (146)** 已覆盖安装到原 iPhone
  `com.kongshang.maichat`，真机回读为 146 并正常启动。主程序 SHA-256
  `2814555e712830c439ead49b6f94f3e1d70a34a8ba0b07b7d4abefc6f1544536`；
  `/private/tmp/maichat-slider-ios-device-install-20261031.json`、
  `/private/tmp/maichat-slider-ios-device-installed-20261031.json`。

## 23:07 后续范围：结束状态、播放栏和图片回缩

用户随后新增：播放结束按钮仍显示暂停、从结尾回拖时画面跳跃；Mac 播放栏过大；
创作中心图片关闭时回缩到缩略图上方。146 版不包含这些后续修复。

- EOF 最小用例证实：时长耗尽后 `running=1/paused=0`，时钟继续走。共享兼容层
  采用上游队列耗尽条件，在单次非 autoexit 播放结束时稳定暂停并停在时长端点；
  从结尾 seek 走暂停单帧路径，结尾点击播放则回到零点开始。显式循环保持原逻辑。
  红日志 `/private/tmp/maichat-eof-before-20261031.log`；结束/回拖与滑块首轮 9 项通过。
  末帧像素仍是第 199 帧，EOF 的进度端点为 8.000 秒，测试分别核对这两种语义。
- Mac 控制栏已合并为单行紧凑线条图标，复制路径保留在更多菜单，按钮提示和
  无障碍名称保留。后续将核对云盘/本地实际截图与最终安装版本。
- 用户提供的 iPhone 录屏为 8.290 秒、1290×2796，已读取查看。原实现将
  命名页面空间的缩略图 CGRect 直接放到忽略安全区的全屏 overlay，混用了坐标。
  改为屏幕坐标并减去 overlay 自己的屏幕原点；每个缩略图用稳定 UUID 发布实时位置，
  同一文件重复出现时也能对应到实际点击的视图。展开期间只隐藏源图和它的放大标记，
  保留布局；预览序列异步更新继续保留源 UUID。
- 最新 iOS 147 真机构建已准备，正在进行本轮结束状态、图片回缩及最终 UI 回归，
  尚未把构建成功或未完成的动画验收标记为通过。

### 最终播放回归与 Mac 交付（23:45）

- 实际 Cocoa/Metal 窗口播放回归 **27 PASS / 0 FAIL / 0 SKIP，29.763 秒**，
  日志 `/private/tmp/maichat-media-endpoint-regression-20261031.log`。覆盖精准画面、
  seek 等待期间的滑块、播放结束暂停状态、结束后回拖单帧保持、再次播放从零开始。
  暂停跳到 100% 时，以末帧就绪确认结束，不等待无法在暂停中耗尽的不足一帧音频。
- 音频、宿主、VOD、HLG GPU 与云盘 **7/7 通过，8.18 秒**，日志
  `/private/tmp/maichat-media-delivery-host-20261031.log`。云盘实际独立 Cocoa 窗口
  另行截图并通过双击播放/关闭回归；输入素材使用绝对路径，避免相对路径失效。
- 本地和云盘实际截图已核对为单行紧凑控制栏；打包产物严格签名、裸 PATH 隔离
  smoke 通过，`/private/tmp/maichat-media-delivery-smoke-20261031.log`。
- 最终包 `MaiChat/desktop/dist-media-delivery-20261031/MaiChat-macos-arm64/MaiChat.app`，
  版本 `2.2.32`，于 **2026-10-31 23:45:20 CST** 覆盖安装并启动，PID **2018**。
  实际进程和安装文件已核对，保留用户数据。最终哈希替代前述各轮旧安装值：
  主程序 `eefe1f6712ed952e7124b438d1220ce316a3285d17e292a587264928b3c7e7e5`；
  FFplay `74a5792fca64dadff4f3c3afead207b5212ab64326be8c424e5e865e96518515`；
  Metal `b7442a850ee0eb427d42df1b4e6e7af16f13da9168047a0c9f3cd1c35ac9bf8e`。
  清单和收据为 `/private/tmp/maichat-media-delivery-artifact-20261031.json`、
  `/private/tmp/maichat-media-delivery-install-20261031.json`。

### 图片全屏真实父层级修复（23:45–2026-11-01 00:10）

- 用户后续截图证实，原先独立 `AIAssistantView` 验收没有覆盖正常 `RootView`：
  外层状态栏规则和底部五个入口仍盖在图片上。因此旧独立预览测试通过，不能说明
  正常 App 的全屏已通过。此处重新以真实五个底部入口复现和验收。
- 复现操作：在创作中心点击消息中的缩略图，预览全屏后再点图片回缩，连续两次；
  预期全屏不出现系统状态栏、底部导航、标题、关闭叉号，缩略图无放大角标；
  图片回缩至刚才点击的同一缩略图位置，原页面布局和底部入口恢复。
- 正常 App 将共享图片预览层放到 `RootView` 的底部导航 safe-area inset 和视频层
  之后；预览可见性由 preference 交给 Root 统一隐藏状态栏。底下页面继续布局，
  避免退出全屏时源缩略图因导航收起/展开而改位。位置缓存仅由预览层观察，
  不让整页导航在滚动或流式消息过程中反复重建。
  缓存尚未提供某个源 UUID 的位置时，使用点击瞬间保存的 CGRect；通过 `flatMap`
  消除缓存查询的双层 Optional，确保缺少缓存不会吞掉原始回缩锚点。
- 删除图片关闭按钮、文件名/序号标题和缩略图右下角放大箭头。保留点击图片展开、
  再点回缩、捏合和媒体左右切换；无障碍逃离动作仍可关闭。图片序列切到视频时
  继续走现有播放器，普通入口和 Agent 共用媒体模型。
- 148 版真实 RootView 与独立全屏 **2/2 通过，18.394 秒**，并已安装启动到 iPhone；
  `/private/tmp/maichat-root-image-fullscreen-20261031.xcresult`。随后用户要求移除标题
  和缩略图箭头，148 的结论不替代下述 149 版验收。
- **149 版 3/3 通过，0 失败，27.095 秒**：正常 RootView 两轮展开/回缩、独立宿主
  全屏、图片/视频往返切换。断言全屏时五个底部入口不可点击且无系统状态栏，
  回缩后入口恢复，缩略图坐标和尺寸与展开前误差不超过 2 pt。
  `/private/tmp/maichat-image-clean-20261101.xcresult`、
  `/private/tmp/maichat-image-clean-20261101.log`。
  切换视频用例仍有一条 SwiftUI view-update 发布警告，前一轮同一用例也有；
  本次未将通过结果描述为无运行时警告。
- 补齐缓存缺失时的回缩锚点后，最终构建同三项 **3/3 再通过，27.156 秒**，
  `/private/tmp/maichat-image-clean-final-20261101.xcresult`、
  `/private/tmp/maichat-image-clean-final-20261101.log`。最终截图另存
  `/private/tmp/maichat-image-clean-final-attachments-20261101/`，展开截图已人工查看。
- 已人工查看展开和返回截图：全屏纯图片，没有标题、叉号、状态栏和五个底部入口；
  返回的缩略图没有角标。附件目录
  `/private/tmp/maichat-image-clean-attachments-20261101/`。录屏
  `/private/tmp/maichat-image-clean-20261101.mp4`（34.437 秒），回缩逐帧图
  `/private/tmp/maichat-image-clean-close-frames-20261101.png` 已查看，回缩落回源图。

### iOS 149 安装与验证边界

- 源码 HEAD 仍为 `1997947219596e6520be27ed892bd6ea1820ff2e` 加本地工作区。
  Xcode 工程和 `project.yml` 的版本均同步为 **2.2.4 (149)**。
  模拟器和真机 arm64 构建成功；逐个校验 10 个嵌入动态库/框架及 App 严格签名。
  主程序 SHA-256 为
  `8b288dd214e4d087417bedb6874e5bfe4c2330338b8a81623b6402e8c17d9c35`，
  清单 `/private/tmp/maichat-ios149-artifact-20261101.json`。
- 已按用户安装最新版的授权覆盖原 iPhone 的 `com.kongshang.maichat`，
  安装后通过设备应用清单回读确认为 **2.2.4 (149)**，未清理 App 数据或 Keychain。
  最终构建安装和版本回读为 `/private/tmp/maichat-ios149-final-installed-20261101.json`、
  `/private/tmp/maichat-ios149-final-readback-20261101.json`。
- 00:05 与最终 00:09 后启动检查均被设备锁屏拒绝，明确错误为
  `FBSOpenApplicationErrorDomain 7 / Locked`，
  `/private/tmp/maichat-ios149-final-launched-20261101.log`。已请求用户解锁以继续检查。
  当前证据是正常 RootView 的模拟器交互、截图和动画，以及真机安装版本回读；
  尚不能宣称 149 真机正常启动或真机手势验收通过。
