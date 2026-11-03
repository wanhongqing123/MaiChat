# 移动端修复累计构建验收（2026-11-03）

首次修复提交为 `d8a39b2f`（基线 `99594888`），随后合并 `98e92ae8`；合并及播放器修正以本记录所在提交为准。所有模型测试只使用本地
受控模型或公开图片，没有发送付费模型任务或使用用户私人照片。

## 已完成的行为

- 大图展开先显示当前缩略图，再异步加载清晰图；回缩沿用同一来源身份和位置。
- 创作中心忙碌时可补充当前任务，当前工具不取消；完整工具批次后再将补充交给模型。
  确认落库后才清草稿，未知回执复用操作编号，附件引用随历史正确保留和释放。
- 人像美白改用原图坐标的有界检测/编辑区域，保持原尺寸输出，不要求调用方手动回贴。
  iOS 附完整 L8 诊断 mask；Android 返回原尺寸 PNG 与 ROI/皮肤指标，未导出 mask 文件。
  未知参数、输入未完成、解码和检测失败明确区分。旧美体仍保留原有像素预算。

## 实际验证

| 构建或入口 | 结果及证据 |
| --- | --- |
| 共享 Core | 合并后重新构建八套，8/8 通过，88.986 秒；`/private/tmp/maichat-merged-core-20261103-results.json` 保存源码 SHA 与逐项日志 |
| 真实 Mobile C ABI | `MaiMobileSteeringTests` 重建后 1/1 通过，含确切 user 回执游标、完整工具结果顺序与幂等；`maichat-steer-receipt-validation-20261103-evidence.json` |
| iOS 2.2.4 (176) | 合并后正常 Root 的 14 项 UI：首轮 13 过、旋转失败；同一签名构建在新模拟器上旋转连续两次通过（20.339 秒、17.918 秒），原用例未改断言/超时。保留初次失败，详见下文 |
| Android 2.2.14 (251) | 合并后正式 APK 安装到自有 arm64/API37/16KiB 模拟器，普通 Controller 四项 instrumentation 全过，58.21 秒；JVM 335 项：334 过、1 跳过、0 失败。`maichat-merged251-portrait-instrumentation-20261103.log` / `maichat-merged251-android-unit-20261103.log` |
| Desktop 2.2.35 | 合并后所属端 26/26 过；播放器 Cocoa 定点布局/seek/结束/原生动作通过，隐藏唤醒单项复测通过。正常本地 App bundle 签名验证后 `--smoke` exit 0；未替换用户正在运行的桌面 App，详见播放器专项 |
| Session 同步控制面 | 三组 Node 测试合计 23/23 通过；未部署生产服务 |

iOS 用例涵盖冷解码首图两次展开/回缩、当前位置返回、IM 全窗口、普通补充、未知回执
重试、会话/账号切换、真实屏幕帧归档、24MP 原图+遮罩及旧个人模型训练入口。
首次175的完整录像 `maichat-batch175-ios-ui-20261103.mp4` 和导出截图已人工查看；合并后176首轮录像与失败截图也已查看，冷解码图像
持续可见。24MP QA 附件保持 5712×4284，源 SHA 不变，L* `61.123→62.906`、
C* `42.021→35.874`。这是公开合成图结果，不等于用户原照的色彩与观感验收。

Android 初装 247 的四项测试曾有两项真实像素失败，未计为通过。根因是 JPEG 裸区域
解码边缘的色度上采样与整图不同。加入有界外围上下文、网格对齐和精确裁回后，原断言
不放宽；248 同组全绿，另含八方向高频 JPEG 与整图解码 oracle 的逐像素检查。
检测器生命周期导致的重复内存增长及肩部误检也已有反事实，详见 Android 专项记录。
636.2 MiB 的先前完整探针峰值不是 halo 后正常 App 的重测峰值，不能混用。

## 合并后的 iOS 旋转排查

初轮 `/private/tmp/maichat-merged176-ios-ui-20261103.xcresult` 中只有新增的横屏用例失败；
不是旧175已通过的同名用例回归。旧模拟器 A15BBB1B 上图片 overlay、视频 cover、普通
Root 都不旋转。只读探针显示 App 内 `UIDevice.orientation=Unknown`，Scene 仍 Portrait，
scene orientation lock=false；App/Root/视频 controller 的方向 mask 均包含横屏。
没有据此修改产品方向策略或强制 geometry request。临时探针代码已从交付源码移除。

新建同为 iOS26.5、iPhone17ProMax 的独立模拟器 19D02616，完成首次数据迁移后，用
同一176 App/Runner 重跑原用例两次均通过：`maichat-merged176-fresh-rotation-20261103.xcresult`
和 `maichat-merged176-fresh-rotation-visual-20261103.xcresult`。第二次录屏
`maichat-merged176-fresh-rotation-20261103.mp4` 已逐段提帧查看；测试快速旋转再回竖屏，
录屏没有足够长的稳定横屏停留，故此记录只确认自动化方向/暂停时间/浏览/关闭断言，
不把它扩张成真机横屏观感验收。旧环境失败及探针数据均留在 `/private/tmp`。

## 构建、安装产物与边界

- iOS 真机包已构建，`/private/tmp/maichat-merged176-device-artifacts-20261103/MaiChat.app`，
  build=176；11 个 Mach-O 及外层签名逐项验证通过。产物 SHA 清单在同目录 `receipt.json`。
- 此前175实际尝试 `devicectl device install app` 失败，CoreDevice **4016**：设备不能满足
  `coreDeviceServicesLoaded` 和 `remoteServiceDiscoveryTrustedConnectivityAvailable`
  等连接条件。设备已配对但开发连接 unavailable。安装未成功，也未启动真机新版本；
  176构建后再次查询仍 unavailable，未声称176已安装；需要重新连接并解锁手机后继续，不把 Mac 解锁等同于手机开发通道可用。
- Android 已安装/启动251，普通 MainActivity 的登录画面已人工查看；核实 package versionCode=251、versionName=2.2.14，PID26419。APK SHA-256
  `eb536e403d474d685cffc6b3aef32dc5c32f7ba59417c9b56171f24a8d36c437`。
  `/private/tmp/maichat-merged251-artifacts-20261103` 保留 App、测试包、运行回执及截图。
- Android 模拟器出现页面大小兼容性告警；新旧 APK 的 ZIP/ELF 检查通过，被点名的
  供应商依赖与 246 字节相同，实际已提取库 SHA 也与 APK 相同。记录现场告警和检查
  矛盾，不擅自抑制系统警告，不据此宣称所有真实 16KiB 设备已验收。
- 本地 iOS 输入补充不依赖同步服务；跨设备补充仍需所属端及服务协议升级。
  生产服务未部署，真实跨设备和 Android 所属端 steering 未联合验收。
- 联网检索本轮只完成[现状与方案评估](maichat-web-retrieval-evaluation-20261103.md)，
  未实现或发布 `web_search`/`web_fetch`，未实测收费 API 的召回质量。
- 工作区 `compile_commands.json` 已按实际构建命令刷新为 445 项，包含新 ROI 源文件，
  仍是本机生成文件，不提交带本机绝对路径的数据库。

专项记录：[任务核心](maichat-core-steering-20261103.md)、
[iOS 输入及远端](maichat-ios-task-steering-20261103.md)、
[图片预览](maichat-image-fullscreen-spinner-20261103.md)、
[人像普通服务](maichat-portrait-whitening-large-images-20261103.md)、
[Android ROI](maichat-android-portrait-roi-20261103.md)。

合并后的非 Apple curl resolver 另在真实 Android 内核做 eventfd/pipe 的旧版失败→修复通过对照；8/8 修复用例通过，见 `/private/tmp/maichat-merged-validation-20261103.json`。

桌面专项：[两排透明控件](maichat-desktop-video-menu-20261103.md)。本记录不包含后续 llama/mtmd 新集成的验收。
