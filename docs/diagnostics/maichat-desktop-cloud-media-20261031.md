# Desktop 云盘精简、独立播放器与 HDR 色彩

> 首轮安装后用户于 17:37 再次反馈颜色异常。首轮元数据/出帧回归不构成色彩验收；
> 后续的 iOS HLG/EDR 移植、实际 GPU 像素对照和底部区域移除见文末。

- 时间：2026-10-31，macOS arm64；基线 `1997947219596e6520be27ed892bd6ea1820ff2e`。
- 现场：用户提供根目录、文件夹内容和云盘视频播放三张截图；页面重复显示空间说明、
  查看/播放及每行改名/移动文字，播放时主窗口被分成左右两栏，HDR 视频颜色异常。
- 操作与预期：进入云盘及子目录，双击缩略图或文件名打开图片/视频；单击不打开。
  文件、文件夹的改名和移动使用现有 Agent 工具。云盘视频在独立可移动/缩放窗口中
  播放，原目录与滚动位置保留；关闭回到原列表。下载和删除使用有悬停/无障碍名称的
  图标，删除继续确认。加载、失败、取消和账号切换沿用原请求代际与取消约束。

## 定位证据

- `MainWindow` 创建云盘时把播放回调接到 `openDockedVideo`，与截图中的分屏一致。
  云盘工具也使用此入口；两处改为共用独立云盘播放入口，IM/创作中心本地视频保留原入口。
- `MaiFfplayVideoDialog` 原来无条件使用普通 FFplay 参数，没有设置
  `present_native_frames`。SDL 兼容层 `SDL_RenderPresent` 只回传 8-bit 帧及
  BT.601/709 矩阵，没有 HLG/PQ 传递函数；HDR 信息在到达已有 OBS 转换器前丢失。
- 用户此前提供的原始 MOV 仍可本地只读访问；ffprobe 确认 HEVC Main10、
  `yuv420p10le`、BT.2020/HLG 与 -180° Display Matrix，不改写原文件。
- 对照 iOS 已有 `MaiFfplayIosBridge`、OBS `obs-source.c` 的 HDR 转换和
  `default.effect` 的 `DrawTonemap`，桌面接入相同的原生帧桥接与已有 HDR→SDR 渲染。

## 实现与验证状态

- 云盘移除查看/播放与改名/移动控件及其专用表单代码，工具实现继续共用原核心。
  精简空间卡和重复类型说明；双击与 Enter 复用同一打开操作。
- 云盘普通界面和工具共用独立窗口，保留账号变化关闭和播放器持有点播源的生命周期。
- macOS HDR 使用原生 10-bit 帧；硬解无画面时只尝试保留 HDR 的软件解码，避免静默
  降成丢失 HLG/PQ 的路径。普通 SDR 和带字幕文件保持原有路径。
- 构建缓存包含迁移前绝对路径；在隔离临时构建中修正 CMake 缓存与已生成的 Eigen
  元数据路径后配置成功，没有改动第三方源码或生产服务。
- 本机对运行中 App 的系统截图被拒绝（`could not create image from display`），
  不能把截图命令执行过当作正常界面已目视验收。后续构建、回归和画面证据继续追加。

## macOS 回归

- 隔离构建 `/private/tmp/maichat-quic-desktop` 已从本仓库完整编译正式 `maichat`。
  保留 QUIC、OpenCV、Graphics 和 TRTC；修复缓存中 CMake、Eigen 导出文件及编解码器
  生成依赖的旧路径后通过，没有关闭产品能力来绕过构建。
- 正常 Cocoa 平台的云盘 TLS fixture 通过。使用正式 `MainWindow`、云盘组件和播放器，
  IM 使用本地假客户端，账号/云盘/Range 请求只访问 localhost。覆盖单击不打开、
  双击缩略图打开原图/点播、嵌套目录、图标及无障碍名称、独立窗口不挤压主列表、
  关闭回原目录、重新打开、切换账号关闭播放器、断点恢复和删除。
  日志 `/private/tmp/maichat-cloud-ui-cocoa-final-20261031.log`。
- 已亲看 `/private/tmp/maichat-cloud-ui-cocoa-final-20261031/cloud-main-window.png`：
  页面只保留文件名、大小、简化容量、路径和必要操作；下载/删除/导航为统一线条图标。
  `cloud-independent-player.png` 是 QWidget 的控件截图，不含 Metal 视频像素，不能
  用其黑色背景断言画面正确或失败；实际 GPU 呈现由同次回调检查覆盖。
- `MaiFfplayVideoDialogTest` 最终 **11 passed、0 failed、0 skipped**，10.672 秒。
  覆盖 MPEG4+音轨+字幕、H264、AV1、跳转、倍速声音，以及 HLG、PQ 和原始旋转 HLG MOV。
  HDR 用例先核对文件有真实 HDR 位流标记，再检查 HLG/PQ 原生帧进入 Graphics，且
  GPU 实际呈现回调成功。原视频只读，不复制或改写；没有调用模型。
  日志 `/private/tmp/maichat-cloud-video-final-20261031.log`。
- 初次合成 HDR 用例仅设置 FFmpeg 输出色彩选项，HEVC 位流仍标记 unknown；补上 x265
  的真实 VUI 参数后，以生产探测器先校验再播放。最初失败不能记为已测过有效 HDR。
- 首轮回归还发现探测期间立刻设置倍速会因时钟未建立而失败；现在按初始源时间重建，
  同一视频控制用例最终通过。PQ 首次加载完整 OBS effect 曾耗时约 16.8 秒，测试允许
  30 秒冷编译窗口；没有据此宣称产品 PQ 首次打开已优化。HLG 复用现有精简 effect。
- `main_window_layout_test` 通过，17.47 秒。首轮云盘工具 CTest 因尚未构建对应 integration
  可执行文件而未进入测试，随后补构建并单独回归；最终结果与产物检查继续追加。

## 最终候选与待安装

- 云盘工具 integration CTest 最终 **1/1 PASS，6.00 秒**，包含现有改名、移动等工具
  的契约回归。日志 `/private/tmp/maichat-cloud-tools-final-20261031.log`。
- 最终候选为 `MaiChat/desktop/dist-cloud-media-20261031/MaiChat-macos-arm64/MaiChat.app`，
  版本 2.2.32，主程序 69,321,680 B，SHA-256
  `0f918fbd5427d6c45c46ad9f7bf0a49141b1f2d9608c94ba1afa7a14335d8cfa`。
  来源是上述基线加本轮工作区修改，尚未提交或推送；源码差异摘要及产物清单保存于
  `/private/tmp/maichat-cloud-ui-artifact-20261031.json`，不以旧基线 hash 冒称包含修复的提交。
- 深度严格签名校验通过；70 个 Mach-O 未发现开发机绝对路径依赖。
  包含 QtWebSockets、ONNX Runtime 1.26.0、RVM 模型以及 QPDF LICENSE/NOTICE。
- 裸 PATH、禁止网络和写入用户目录的最终 `--smoke` 退出 0，PID 26499，日志核对
  实际候选路径、Qt 5.15.18、OpenSSL 3.5.9，数据实际位于临时目录。
  日志 `/private/tmp/maichat-cloud-ui-package-smoke-final-20261031.log`。
- 第一轮 smoke 把注入环境放在系统 sandbox 启动器之前，变量被剥离，日志仍显示用户
  数据路径；sandbox 当时禁止写入用户目录。最终复核将环境放入 sandbox 后的 env，
  已确认临时数据路径，不能用第一轮代替最终隔离证明。
- 候选已可供安装。现装 App 的 PID 3073 保持运行；按根仓库约定，已请求用户确认退出、
  替换和重启的时间，收到确认前不打断当前会话。未发布 GitHub Release、未部署服务。

## 2026-10-31 17:24：用户确认后的实际安装

- 用户随后明确回复“可以的”，授权退出当前 MaiChat、替换修复版并重新启动。
  先把已验签候选复制到 Applications 下的临时位置，再停止旧进程并替换 App bundle。
  普通 AppleEvent 退出请求首次回报 -128；源码的异步远程桌面关闭流程先忽略 close，
  随后旧 PID 3073 已正常退出。替换前再次核对没有旧进程，没有强制杀进程。
- `/Applications/MaiChat.app` 已替换并正常启动，**新 PID 31483**，进程启动于
  2026-10-31 17:24:01 CST。安装后主程序 SHA-256 与已验证候选一致：
  `0f918fbd5427d6c45c46ad9f7bf0a49141b1f2d9608c94ba1afa7a14335d8cfa`。
  版本仍为 2.2.32，不能只凭版本号判断新旧；以文件摘要和新进程启动信息区分。
- 实际运行日志 `~/Library/Application Support/MaiChat/Desktop IM/logs/maichat-20261031.log`
  的 17:24:02.939 段确认新 PID、实际 Applications 路径、2.2.32 / Qt 5.15.18，
  libcurl 的 TLS 后端为 OpenSSL 3.5.9。安装和进程核对回执：
  `/private/tmp/maichat-cloud-ui-install-20261031.json`。
- 验证新进程和安装摘要后，已清理替换事务中的临时旧 App；验证过的候选包仍保留在
  dist 目录。安装操作只替换 App bundle，没有主动清理或迁移用户账号、会话、草稿、
  云盘文件及数据库。
- 当前自动化进程的 `AXIsProcessTrusted()` 和 `CGPreflightScreenCaptureAccess()`
  均为 false。新安装已运行，但现场云盘操作与 HDR 色彩无法通过系统自动化截图核对；
  不把此前隔离组件的回归宣称为安装版真实账号的现场视觉验收。

## 17:33：屏幕录制权限归属与拒绝原因

- 用户追问权限原因后，检查执行进程链和 macOS TCC 日志。当前宿主为
  `/Applications/Multi-AI Code.app`，Bundle ID `com.multiaicode.app`，版本 2.2.5；
  当前 App 使用 ad hoc 签名，没有 Team Identifier。
- TCC 的 `AUTHREQ_ATTRIBUTION` 明确把 Swift 检测进程归属到该宿主（responsible PID
  43258），不是根据文件路径猜测授权对象。录屏预检仍为 false，辅助功能仍为 false。
- 17:33:36 的系统日志对 `kTCCServiceScreenCapture` 明确报
  `Failed to match existing code requirement for subject com.multiaicode.app`。
  因此已定位为当前代码身份与既有权限记录不匹配；不能继续只笼统记为用户从未授权。
  未取得历史权限快照，不推断具体哪次更新或签名操作造成变化。
- 正常 MaiChat 的 PID 31483 仍在运行，WindowServer 可列出其窗口，但窗口标题不可见；
  这与上述录屏预检失败一致。没有修改 TCC 数据库、重置权限或自行开启系统权限。
- 恢复入口以 Apple 当前 macOS 27 说明为准：系统设置→隐私与安全→录屏与系统录音，
  重新授权当前 Multi-AI Code；若旧条目无法生效，可移除后重新添加当前 Applications
  下的 App。界面操作还需在辅助功能中授权同一宿主。恢复授权后才能继续现场视觉验收。

## 17:37 后：复用 iOS 已有 HLG/EDR 修复

- 新截图显示独立播放窗口已生效，但用户仍报告颜色不正确；截图中的文件为
  “sample-hlg-video.mov”，28.68 秒。随后用户提醒此问题以前在 iOS 解决过，
  又要求移除云盘底部的进度条及状态文字/暂停区域。两项都属于本轮继续修复范围。
- 对照 `maichat-ios-hlg-rotated-mov-playback-20261026.md`：iOS 的颜色修复不仅保留
  原生 HDR 帧，还把 AMVE、FP16、扩展线性 BT.2020 和 HLG 元数据交给系统 EDR。
  首轮 Desktop 只接了解码路径，Presenter 和 Metal setter 仍限定 iOS，Mac 的
  `device_load_swapchain` 还会按 standalone OBS 的全局 SDR 配置重置图层。因此 Mac
  仍使用固定 OOTF、1000/300 缩放和 Reinhard，遗漏了用户明确指出的既有显示方案。
- 使用服务器只读数据库查询定位同名唯一素材，复用现有素材签名服务取得短时读取地址，
  视频由本机直接从 OSS 读取；没有修改用户文件、数据库或线上服务，也没有输出凭据。
  原始媒体只在仓库外私有临时目录作为输入，未加入源码或测试输出。文件 31,858,100 B、
  28.676667 秒，SHA-256 `b697f7008b8277f0b80e62c9bbd923d6a8b56f6b0834893a357390a42fd2eba9`。
  ffprobe 确认 HEVC Main10/BT.2020/HLG、Dolby Vision 8.4 兼容及 AMVE 314 lux/D65；
  该次原片没有旋转矩阵，不能用此前 16.505 秒旋转 MOV 代替本次复现。
- macOS 与 iOS 现在复用同一 per-player HLG setter。Mac 13+/iOS 16+ 使用系统 HLG EDR，
  Mac 14+/iOS 17+ 使用 AMVE。Mac 动态 Metal 模块沿用 OBS 模块加载器调用此扩展；
  iOS 保留原静态调用。播放器显式模式不再被 OBS 全局 SDR 配置覆盖，其他视图仍按原逻辑。
- Desktop 收到 P010/BT.2020/HLG 后，先设置该播放器图层及 AMVE，再提交画面；RGBA 或
  SDR 切换时复位。转换复用 iOS 已验证的逆 HLG OETF、0–12 场景线性值和纯 Draw，
  不再给系统 HLG 路径重复施加固定 OOTF 或 Reinhard。
- 云盘底部进度条、状态标签和通用暂停按钮已从布局及成员中移除；同时删除其进度轮询。
  请求取消、检查点恢复、账号隔离仍走原核心；忙态仅更新控件可用性/光标，必要错误由
  MainWindow 现有轻提示承接，不再创建底部文字区域。

### 实际像素对照与回归

- 新增 `MaiGraphicsHlgMacTests`，调用正式 Presenter，只读回测试自己拥有的 drawable。
  测试观察实际 Metal 命令完成后再拷贝像素，不依赖系统录屏权限；macOS 的渲染回调
  早于 CVDisplayLink 交换 drawable，因此同时等待 HDR/SDR 对应的新格式。
- 同一个最终测试，单独链接修复前 `MaiGraphicsPresenter.cpp` 后退出 4：
  `hlg_layer=0 bt2020=0 gpu_read=1 rgb=0.678431,0.682353,0.678431`。
  日志 `/private/tmp/maichat-hlg-pixel-before-edr-20261031.log`。此对照不改动当前安装的 App。
- 修复后正式 Presenter 与 Metal 后端 **1/1 PASS，1.05 秒**：
  `hlg_layer=1 bt2020=1 gpu_read=1 rgb=3.099609,3.160156,3.089844`；HLG 0.75 的
  逆 OETF 参考值为 3.179551，仍保留超过 SDR 1.0 的实际输出。切回同一视图的 SDR
  绿色读回 `0,1,0`，EDR/元数据已复位。像素结果验证了最终渲染，不再仅断言元数据存在。
- 使用本次 28.68 秒原片的正常 Cocoa 播放器回归 **11 passed、0 failed、0 skipped**；
  生产日志确认 `HLG EDR enabled ambient=1`。同次覆盖 SDR、音轨/字幕、倍速、跳转、
  合成 HLG/PQ。日志 `/private/tmp/maichat-hlg-video-edr-20261031.log`。
- 正常 Cocoa 云盘/独立窗口/账号切换/恢复流程通过，新增断言三种底部控件不存在。
  已亲看 `/private/tmp/maichat-cloud-edr-footer-20261031/cloud-main-window.png`，
  底部没有新增进度或状态区域。iOS bridge 的既有 HLG/AMVE/SDR fallback 契约也通过。
- MaiAgent 八套核心回归 **8/8 PASS，152.40 秒**；使用当前源码、QUIC ON，未触发模型任务。
  日志 `/private/tmp/maichat-hlg-core-ctest-20261031.log`。iOS Metal 类型检查与新包安装结果
  继续追加。

### 18:12：HLG/EDR 与底部区域修复实际安装

- iOS 16 arm64 目标的整套 Metal Swift 源码类型检查通过，原 iOS bridge fallback/HLG/
  AMVE 契约通过；没有为这次 Mac 修复替换手机 App。所有相关回归完成后封装新候选
  `MaiChat/desktop/dist-cloud-hlg-edr-20261031/MaiChat-macos-arm64/MaiChat.app`。
- 新候选主程序 69,321,984 B，SHA-256
  `7dbf6d67861e4e88dabd663c7a09e062d0639625606a0e140bf2ede3560a8d5a`；Metal 模块 SHA-256
  `b7442a850ee0eb427d42df1b4e6e7af16f13da9168047a0c9f3cd1c35ac9bf8e`，导出 HLG 扩展。
  深度严格签名通过，70 个 Mach-O 无开发机绝对路径依赖，QPDF 两份许可哈希与源码一致。
  禁止网络/写入用户目录的裸 PATH 隔离启动退出 0，数据实际落临时目录。
  清单 `/private/tmp/maichat-hlg-edr-artifact-20261031.json`。
- 沿用用户对本修复已给出的更新/重启授权，退出旧 PID 31483 后替换并重新启动。
  新 PID **88432**，实际安装时间 **2026-10-31 18:12:28 CST**。已核对 Applications
  下的主程序和 Metal 模块均与候选一致，并从正常运行日志核对新 PID、实际路径和
  2.2.32 / Qt 5.15.18 / OpenSSL 3.5.9。临时旧 App 已清理，用户数据未主动清理。
  回执 `/private/tmp/maichat-hlg-edr-install-20261031.json`。
- 已删除本轮从云盘取得的私有原片输入副本；云盘对象及此前用户发来的其他原片未修改。
- 本轮已验证实际 GPU 像素、系统 HLG 图层/色域、原片 AMVE 路径与安装运行身份；
  没有把 PNG 截图当成完整 HDR 显示效果或 Dolby Vision RPU/Photos 色彩严格等价的证明。

## 验证边界

- 本轮目标是 Mac Desktop。Windows 未编译或实机验证，iOS/Android 未改 UI。
- 上述是正式组件的隔离桌面回归，不能替代用户当前安装包与真实账号、云盘原素材的
  完整现场验收。HDR 已验证到原生元数据和实际 GPU 呈现，尚未对运行中的安装版做
  目视色彩对照。`/Applications/MaiChat.app` 的旧进程 PID 3073 尚未替换。
