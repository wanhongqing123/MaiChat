# 移动端修复累计构建验收（2026-11-03）

源码基线为 main `99594888`，修复内容以本记录所在提交为准。所有模型测试只使用本地
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
| 共享 Core | 八套按修改后的源码重新构建，8/8 通过；`maichat-steer-core-final-20261103-results.json` 保存源码 SHA 与逐项日志 |
| 真实 Mobile C ABI | `MaiMobileSteeringTests` 重建后 1/1 通过，含确切 user 回执游标、完整工具结果顺序与幂等；`maichat-steer-receipt-validation-20261103-evidence.json` |
| iOS 2.2.4 (175) | 正常 Root 的九项 UI 用例全部通过，153.876 秒；`/private/tmp/maichat-batch175-ios-ui-20261103.xcresult` |
| Android 2.2.6 (248) | 正式 APK 安装到自有 arm64/API37/16KiB 模拟器，普通 Controller 的四项 instrumentation 全部通过，60.377 秒；`maichat-batch248-portrait-instrumentation-20261103.log` |
| Desktop 所属端 | `DesktopSessionSyncHostTest` 26/26 通过，offscreen；未替换用户正在运行的桌面 App |
| Session 同步控制面 | 三组 Node 测试合计 23/23 通过；未部署生产服务 |

iOS 用例涵盖冷解码首图两次展开/回缩、当前位置返回、IM 全窗口、普通补充、未知回执
重试、会话/账号切换、真实屏幕帧归档、24MP 原图+遮罩及旧个人模型训练入口。
完整录像 `maichat-batch175-ios-ui-20261103.mp4` 和导出截图已人工查看；冷解码图像
持续可见。24MP QA 附件保持 5712×4284，源 SHA 不变，L* `61.123→62.906`、
C* `42.021→35.874`。这是公开合成图结果，不等于用户原照的色彩与观感验收。

Android 初装 247 的四项测试曾有两项真实像素失败，未计为通过。根因是 JPEG 裸区域
解码边缘的色度上采样与整图不同。加入有界外围上下文、网格对齐和精确裁回后，原断言
不放宽；248 同组全绿，另含八方向高频 JPEG 与整图解码 oracle 的逐像素检查。
检测器生命周期导致的重复内存增长及肩部误检也已有反事实，详见 Android 专项记录。
636.2 MiB 的先前完整探针峰值不是 halo 后正常 App 的重测峰值，不能混用。

## 构建、安装产物与边界

- iOS 真机包已构建，`/private/tmp/maichat-batch175-device-artifacts-20261103/MaiChat.app`，
  build=175；11 个 Mach-O 及外层签名逐项验证通过。产物 SHA 清单在同目录 `receipt.json`。
- 实际尝试 `devicectl device install app` 失败，CoreDevice **4016**：设备不能满足
  `coreDeviceServicesLoaded` 和 `remoteServiceDiscoveryTrustedConnectivityAvailable`
  等连接条件。设备已配对但开发连接 unavailable。安装未成功，也未启动真机新版本；
  需要重新连接并解锁手机后继续，不把 Mac 解锁等同于手机开发通道可用。
- Android 已安装/运行 248；最终 APK SHA-256
  `8484ca01f5dd7d9e84c5e88658eb72738ae7909ee19c0c58d1ea845fe320b498`。
  `/private/tmp/maichat-batch248-artifacts-20261103` 保留 App、测试包及各自校验值。
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
