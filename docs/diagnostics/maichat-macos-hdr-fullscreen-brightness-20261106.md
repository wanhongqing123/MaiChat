# Mac 视频全屏切回窗口亮度变化

2026-11-06 00:28 后；main / eee43402 工作区。现场运行 `/Applications/MaiChat.app`
2.2.34、PID 43461。未停止或替换该进程。用户先指出云盘“桂山岛_秋千_草棚旁_20261025.mov”
地面比 iOS 原生播放器偏亮，随后补充稳定操作：全屏播放，再切回窗口，地面亮度发生变化。

复现与预期：打开同一原片，暂停在同一位置，记录窗口 → 全屏 → 窗口的实际图层状态和
画面。排除视频自身帧变化，比较同一地面区域；播放器不应因切换丢失 HLG、色域或 AMVE。
系统 HDR 显示 headroom 的变化需单独观测，不能把截图或传输帧成功当成颜色一致。

00:38 用户发来原片：14,469,514 字节、13.005 秒，1920×1080、HEVC Main 10、有限范围、
BT.2020/HLG、Dolby Vision 8（HLG compatible，含 RPU），AMVE 314 lux/D65。私有原片
保留 IM 附件原路径，不入库。SHA、完整探测及源码快照在
`/private/tmp/maichat-macos-hdr-window-20261106/`。

同时确认缺少独立“下载云盘资源到 Agent 工作区”的工具入口；已有浏览、播放、会话附件
下载。用户授权补齐，计划复用普通云盘元数据及共享 C++ 下载实现，返回本地路径，禁止
将账号令牌、签名 URL 回传模型或写入日志。该入口与本次色彩根因分开验收。

初查：安装包确实含 HDR 探测和每视图 HLG 输出接口，不能把 2.2.34 的问题直接归为
“没有 HDR 支持”。当前源码在首次 HLG 帧启用 FP16/线性 BT.2020/系统 HLG EDR；全屏
只切换窗口状态和 resize。目前尚无同帧对照的决定性证据，不调整固定亮度或伽马。

## 已确认的颜色原因与实验边界

对原片固定帧，在同一 Mac 对照正常 `MaiFfplayVideoDialog` 与苹果 `AVPlayerLayer`，
沙地在前者较亮。只用 FFmpeg `dovi_rpu=strip=1` 去掉 Dolby Vision 元数据后，苹果
原生也变亮并接近当前 HLG 显示。原片及去元数据副本在 4 秒处解码的 YUV420P10LE
SHA256 完全相同：`b9c7ebda25e69b2ed14b493a7d35eba1f218f48b2af24dd2ee592c63b6318e8e`。
因此有明确证据表明 Dolby Vision 显示处理缺失可以解释与原生的亮度差异。

原正常显示链路会把 VideoToolbox 缓冲拷为 P010 平面，只传色域/范围/传递函数；
`MaiVideoFrame` 不包含 Dolby Vision 显示元数据。检查原 VideoToolbox 输出的两种
attachment mode，也没有 DolbyVisionRPUData。Apple AVAssetReader 同原片的帧却有
420 字节的 RPU 和 8 字节 AMVE。

另做窗口 → 全屏 → 窗口的正常原实现同帧对照（`repeat1`，暂停 4.135s）：三个状态都
保留同一个 layer、FP16、线性 BT.2020 和 HLG EDR；窗口前后截图逐像素完全一致。
NSScreen 当前 headroom 从 1.2 升到约 3.998，但这是观测到的动态状态，不足以证明
全屏是其原因；截图也不能代替物理 HDR 屏幕的瞬时亮度验收。因此用户所述切换瞬间
亮度变化尚未被确定复现，不能写为已修复或归因 SDR 重置。

实验编译均在 QA 目录，未改 HDR 生产代码：

1. 给 FFmpeg VideoToolbox 的格式描述保留原 `dvvC` 和颜色标签后，硬件解码失败。
2. 再把 HEVC 解析器已经提取、原硬解路径没有发送的 RPU NAL 附到当前 AU 后，
   解码恢复（1 秒 29 帧），CVPixelBuffer 首次保留 420 字节 DolbyVisionRPUData。
   复现代码 `videotoolbox.c`、`rebuild-vt.py`、`NativeFrameMetadata.cpp` 及
   `native-metadata-with-rpu.log` 都在 QA 目录。仓库里的 FFmpeg 源码未修改。
3. 通过 AVSampleBufferDisplayLayer 直接显示保留 RPU 的缓冲，仍比 AVPlayerLayer 亮。
   同时保留 AMVE 也未解决。将 Apple AVAssetReader 原生 CMSampleBuffer 直接交给
   同一个显示层，截图与实验缓冲完全相同，但仍比 AVPlayerLayer 亮。因此单纯保留
   RPU 或改成 AVSampleBufferDisplayLayer 不能称为完成 Dolby Vision 原生显示匹配。
   AVPlayerItem 有公开的 appliesPerFrameHDRDisplayMetadata（默认 YES）；直接样本显示层
   没有相同开关。后续需采用支持相应显示处理的正式播放链路，或实现完整的映射。

后几次 Qt GUI 观察实验受到其他窗口遮挡，且部分没有保持暂停；这些画面不作颜色
定量证据。后续 `NativeSampleCompare.mm` 使用 ScreenCaptureKit 的
desktop-independent-window 过滤，只截取自身窗口。`native-comparison-reader` 的
decoded/reader/AVPlayer 三组画面已经查看。实验返回 success 只表示采集和解码成功，
不代表颜色通过。原片及派生视频、截图均不入库。

## 正式实现与当前验证

Mac 上符合已有 HDR 资格、且探测为 MP4/MOV 容器的视频，现由
`MaiMacVideoPlayback` 调用 AVPlayerItem/AVPlayerLayer，明确保留
`appliesPerFrameHDRDisplayMetadata`。这是系统的逐帧显示处理，没有增加固定亮度或
伽马补偿。其余格式仍走原有 FFplay 路径，选中系统路径后失败会显示错误，不悄悄
退回已知丢失 Dolby Vision 映射的显示方式。未将实验 FFmpeg 修改加入产品。

原有播放窗口、暂停、滑块、倍速和 Agent 命令复用同一状态接口。原生首帧就绪单独
查询，不伪造 FFplay 的帧计数。停止/换片让旧打开和 seek 回调失效。云盘使用
AVAssetResourceLoader 适配共享 C++ MaiCloudVod；TLS、代理、短期链接续期、有限
Range 缓存仍由现有内核处理，没有媒体服务代理，也没有先落盘整片。

实际验证与证据：

- `product-second` 和 `product-cloud` 在原片同一暂停时间，对窗口画面与独立
  AVPlayerLayer 参考取相同 ROI（1920×1344 截图内 y=180..1100），RGB 差逐像素为零。
  逻辑全屏前后窗口的该 ROI 也完全一致。图片已查看，包括沙地、阴影、人物和控制栏。
- 本地 HTTPS 云盘入口实际使用正常 DesktopCloudVod 和正式播放组件，29 次 Range，
  全量 GET 为 0；账号 Authorization 未被带给存储。缓存约 7.8 MiB。暂停、7 秒精确
  seek、1.5 倍速保持暂停、恢复播放都通过，并与原生同帧窗口对照一致。
- Cocoa 组件回归 7 项通过（9.325 秒）：HLG、PQ、现场原片的首帧、暂停倍速、精确
  seek、逻辑全屏状态保持、结束与重播、探测过程中立即关闭；另验证原 SDR 起点及
  到尾 seek/重播。新增回归先抓到“到尾后立刻重播仍暂停”，由界面轮询状态滞后
  引起；改读原生播放器意图并延后 seek 完成前的恢复播放后，同一用例通过。
- `product-recorded` 的 40 帧窗口控制采集已保存为带真实间隔的录像，逐帧联系表用于
  检查画面与控制布局。该采集处于后台，不能用于宣称真实全屏转场通过。

### 重要验收边界：锁屏

追加原生窗口状态后发现：早先新实现测试的 Qt fullscreen=true，而 NSWindow
fullscreen=false、尺寸仍 960×672，应用也不在活动 Space。故不能把这些“全屏”
返回成功当作 macOS 全屏验收，前面的结果只支持窗口颜色和控制结论。
03:53 左右读取当前会话的 `CGSSessionScreenIsLocked=true`、console_session=true，
确认 Mac 已锁屏。以 Launch Services 正常启动测试 App 仍在后台，独立 App 的
ScreenCaptureKit 返回 -3801。没有解锁系统、修改录屏权限或把不相关桌面画面用于验收。
真正的前台 macOS 全屏 → 窗口、转场瞬时亮度和物理 HDR 屏幕表现，仍须解锁后验收。
SDR 截图不能代替物理 HDR 显示测量。

Mac 2.2.51 已从当前 main/eee43402 工作区构建并完成打包；App 的 deep/strict 签名
校验通过，从独立 staging 清除开发机 Qt/DYLD 搜索路径后 `--smoke` 退出 0，日志版本
2.2.51。产物：`/private/tmp/maichat-macos-2251-20261106/MaiChat-macos-arm64/MaiChat.app`。
同目录 `delivery-manifest.json` 记录工作区文件与产物 SHA256。安装位置仍为 2.2.34、
PID 43461，未停止或替换。已请求用户解锁及确认验收后的更新/重启安排。

最后同步头文件注释并重新链接后，一次直接替换 staging 可执行文件漏了 Qt 部署处理，
便携启动检查抓到 Homebrew Qt 与包内 Qt 混载。已重新执行正式 macdeployqt 部署步骤、
deep/strict 签名校验和清除 Qt/DYLD 搜索路径的 `--smoke`，最终退出 0；可执行文件不再
引用 Homebrew Qt。最终证据为 `final-redeploy.log`、`final-package-smoke.log` 及刷新后的
交付清单，不能使用中间失败包作为交付依据。

## 同步 9459e7ec 后的播放器

用户指出播放器外观已重构后，main 已从 eee43402 快进远端 17 个提交至 9459e7ec，
并合并本地修改。现采用远端共享无边框窗口、标题栏、HUD、倍速与音量菜单，未回退
旧两排按钮实现。系统 HDR 后端现同时适配原生音量/静音、Agent 状态回显与视频比例；
新文字控件按同一偏好缩放。现场原片和 HLG/PQ 的首帧、暂停/倍速、精确 seek、结束
重播以及新控件回归通过，最终定点 8 项通过。Mac 2.2.53 便携包已完成签名和启动检查，
当前运行的 2.2.34 未被替换。物理 HDR 屏幕的真实全屏切换瞬时亮度仍待现场验收，
此前关于窗口颜色对照的结论不能扩大到该边界。
