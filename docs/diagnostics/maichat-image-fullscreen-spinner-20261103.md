# iOS 创作中心图片全屏黑屏转圈（2026-11-03）

用户提供 9.159 秒 iPhone 录屏，反馈点击图片进入全屏时黑屏转圈。录像保留在本机 IM
附件目录，抽帧在 `/private/tmp/maichat-image-fullscreen-spinner-20261103/frames/`；
包含用户私人图片和会话，不提交视频或帧到公开仓库。开始排查时 main 为 `ecef0b19`，
iOS 已安装版本为 2.2.4 (172)，本轮尚未修改产品代码。

## 现场操作与预期

创作中心消息内已有可见图片，点击展开，点击回缩，再次展开；现场同时有美颜工具
执行。预期已显示的图片从当前缩略图位置连续放大，在清晰图准备期间仍有画面，不能
先隐藏缩略图再显示黑底转圈。关闭后回到原位置，反复打开、左右切换和 IM 入口应正常。

逐帧查看确认：首次点击后已有黑底 spinner（约 0.5 秒），随后图片显示；回缩后再次
进入时也出现黑底 spinner，之后恢复同一图片。不是持续无法解码。工具调用次数也在
变化，但没有证据证明工具替换预览是根因，不能只据同时发生就归因。

## 调用链证据

- `AIWorkspaceImage` 已通过 `RemoteIMAsyncImage` 获得 UIImage，但只存 decodedSize，
  点击闭包只传尺寸、窗口坐标及 sourceID。
- `AIImagePreview` 没有 thumbnail 字段；`AIAssistantImagePreviewLayer` 创建
  `MaiLocalMediaPager` 时没有传 thumbnail。
- `MaiLocalMediaPageController` 因此从 image=nil 开始，黑背景上启动 spinner，直到
  `MaiPreviewImageLoader` 的完整图片读取解码结束后才能 show(image)。
- 同一容器的 IM 入口已传 presentation.thumbnailImage，会先显示缩略图再异步升级。
  创作中心与 IM 的行为差异由源码直接证实。

全图目标为 4096 像素，缓存预算为 32 MiB，12MP RGBA 图可能超过单个缓存预算；这能
解释为何重复打开仍可能冷读，但用户原图尺寸与实际排队耗时尚无现场证据，不据此
扩大缓存或改变原图质量。

## 修复与验收计划

沿现有普通预览入口传递当前已显示的 UIImage，预览生命周期持有首张缩略图；保持
坐标与来源实例标识，不借缓存命中掩盖冷读问题。完整图继续后台解码，不阻塞主线程。
测试逻辑放在 DebugSupport 和独立 XCTest target，不混入正式页面实现。

先用正常 Root、大尺寸合成图片与可控的全图解码等待复现，再回归相同操作。等待期间
核对实际图片像素和 spinner，不能只检查空 UIImageView 的 accessibility 存在性。
按模拟器、真机、实际截图/录屏记录结果，未完成的路径明确标注。

## 红绿对照与实际运行

保留修复前 172 App；使用相同的最终 XCTest Runner、正常 Root、4032×3024 蓝黄合成图，
显式延迟全图读取三秒（仅 DebugSupport 的模拟器测试入口）。在打开后 1.479 秒取样，
修复前图片区域蓝像素覆盖率为 0，像素断言真实失败；截图为黑底 spinner，已人工查看。
结果 `/private/tmp/maichat-image-spinner-red-pixels-20261103.xcresult`，退出 65，
不是把启动失败或元素不存在误算作黑屏复现。

修复后构建 2.2.4 (173)，分别从消息图片、生成结果图片、远端附件已下载图片的入口
传递当前 UIImage；保留来源坐标及稳定 ID，历史媒体补齐时保留缩略图。同一分页容器
继续异步升级清晰图，原有 IM 入口未改。

最终三个模拟器 UI 用例全部通过，测试命令正常退出 0：

- 大图冷读两次展开/回缩：13.294 秒；取样分别为 1.705、1.662 秒，均早于清晰图
  准备；图片蓝黄像素持续可见、没有 spinner。
- 现有图片返回原缩略图位置与主导航恢复：23.324 秒。
- 现有 IM 图片覆盖整个窗口：12.317 秒。

结果 `/private/tmp/maichat-image-spinner-green173-20261103.xcresult`，录像
`maichat-image-spinner-green173-20261103.mp4`。两次冷读展开图、回缩后正常 Root 截图
已逐张查看；后续累计功能构建还需回归，不把此独立 173 构建当作后续改动的验收。
真机开发连接在本次检查时 unavailable，尚未安装或宣称真机通过，已请用户连接。

累计构建 2.2.4 (174) 又实际通过同三个 UI 用例：冷读 13.644 秒、返回当前缩略图
24.470 秒、IM 覆盖全窗口 12.989 秒。新增来源路径与 UIImage 绑定检查，防止视图
复用时将上一文件像素当作新文件缩略图。冷读蓝黄实像截图已查看，无黑底 spinner。
结果包 `maichat-batch174-ios-ui-20261103.xcresult` 同时含其他功能用例，不能因本组
通过就说整个八例批次全绿。

## 测试环境签名问题

最初误用 workspace 命令，Pods 的旧 deployment target 在当前 SDK 下失败；改用既有
project build-for-testing 入口。`CODE_SIGNING_ALLOWED=NO` 还暴露供应商 simulator
`nuisdk` 自带过期 CodeDirectory：codeLimit=4952800，大于文件 3947264 字节，
slot0 等 68 个页哈希不匹配，内核日志明确在该文件 offset 0x0 拒绝执行。
顶层 App deep 验签只把该库当资源封存，不能证明库的内部签名正确。

最终在独立临时产品副本中逐一重签每个裸 Mach-O，再签 framework/App/Runner，
逐个验证 11 个 App Mach-O 与 2 个 Runner Mach-O，正常启动后才执行红绿用例。
没有换 SDK 代码、关闭校验或修改系统签名策略。失败的启动尝试保留日志，不计作
黑屏测试结果；XCTest 使用 `-collect-test-diagnostics never`，保留截图及结果包，
避免失败后长时间采集无关系统诊断。
