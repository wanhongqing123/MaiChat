# Android 远端会话图片与 iOS 对齐

日期：2026-11-15（Asia/Shanghai）。排查源码：`b16d71f4e1c199a18df567e3d3469f4bc904c5e0`，直接在 `main` 开发。

## 复现操作与预期

同一个账号在电脑和 Android 登录，在 Android 创作中心选择电脑上的远端会话。电脑历史里具有 `remoteImage` 临时图片回执的图片，应像 iOS 一样显示同步状态、下载完整图片后预览，并在失败时可重试。远端工作区路径不得在手机本地解析；没有跨设备回执的旧图片、视频继续说明保存在所属设备。

若临时对象到期，已经完整下载并校验的本地副本仍可预览；没有本地副本时显示到期。切换会话、账号或原消息图片元数据后，旧回调不得覆盖新页面。同一图片行反复绑定不得反复发送下载请求。

## 定位证据

- Android `AIAssistantController.sanitizeRemoteMedia` 只识别顶层 `attachment_id`。即使图片有 `remoteImage.state=ready`，也会追加“图片在所属设备，暂不可跨设备预览”。原 `downloadRemoteAttachment` 只支持普通会话附件入口。
- iOS `AIAssistantModel.swift` 的 `AIRemoteImageRequest` 与 `loadRemoteImage` 接收独立 `remoteImage` 回执，以原发布账号、设备、登录会话、会话、消息、片段、附件标识构造缓存，调用 `MaiCloudDriveClient.downloadTemporarySessionImage`。
- 共享 `MaiChat/Network/MaiCloudDriveClient.cpp` 已有 `MAI_SESSION_IMAGE_DOWNLOAD`，验证固定到期、完整请求归属和下载回执，并通过共享下载器直取 OSS。无需新增服务端媒体转发，也无需修改共享网络协议。
- 桌面 `DesktopSessionSyncHost.enrollLiveImages` 只为启动后已观察到的 live 图片建立发布，明确不回扫历史；`decorateImages` 只附加既有发布。因此没有回执的历史图片是另一种边界，iOS 也显示所属设备占位。本次不会回扫并上传用户旧素材。
- 最小测试 `temporaryImageReceiptDoesNotGetAnUnavailablePlaceholderOrLocalPath`：给现有 Android 投影输入 `ready` 临时回执和电脑路径，期望只有原图片片段，实际出现两个片段。无回执旧图片的占位对照用例通过。红测日志：`.cache/android-remote-images-20261115/red.log`。
- 此次红测使用已有 release 编译类且仅编译新增测试，原因是并行工作的 Panel 当时处于未完成编辑状态。该投影方法与当前修改前源码一致。最终验证必须重新编译全部当前 Java 源码，不能拿该旧类测试作为交付绿测。

## 修复设计

- Controller 保留临时回执、清除电脑路径，同时清理主消息、旁路更新和问题投影中的远端路径。
- Android 的附件适配仅提交完整归属和七字段下载载荷给共享 `MAI_SESSION_IMAGE_DOWNLOAD`；不把签名 URL 暴露给 UI，不另写 HTTP 下载。
- 新 `AIAssistantRemoteImages` 保存账号与原发布登录隔离的身份和缓存。完整文件需有相同元数据签名、精确字节数及 SHA-256 完成回执。未完成暂存文件不对外显示；缓存先于 OSS 到期检查。
- Controller 图片 I/O 走现有附件工作线程和取消句柄。可见快照保存内部账号 ID 和登录代次，普通入口也独立拒绝另一账号的旧快照；原设备元组和选择编号相同不代表同一个账号。按选择代次及请求签名合并下载，A→B→A 的旧回调也失效。Panel 通过只读缓存查询与异步普通代码入口展示图片状态及显式重试。
- 同账号远端控制继续使用现有 `SessionSync` 原设备、登录会话、会话与 `turnId` 校验。陌生好友 IM 绑定的控制租约属于独立链路，本次不改动其十个已通过验证的源文件。

## 验证边界

新增测试采用自有四字节图片 fixture，覆盖原发布登录与当前目录登录分离、跨房间或畸形元数据拒绝、缓存完整性、损坏修复、过期后的完整副本、并发请求合并，以及离开后返回拒绝旧回调。测试不发起账号请求、OSS 请求、IM 消息或模型任务。

在 JDK 17、Android SDK 34、Gradle 离线环境上，重新编译当前 Java 源码后运行 `AIAssistantRemoteImagesTest`（8 项）与 `AIAssistantRemoteImageControllerTest`（6 项），共 14 项通过，零失败或跳过。命令使用 `:app:testDebugUnitTest --tests com.kongshang.maichat.AIAssistantRemoteImagesTest --tests com.kongshang.maichat.AIAssistantRemoteImageControllerTest --offline --no-daemon`，加载既有 `.cache/android-install-19979472/local-build.init.gradle`，仅跳过 Native 构建任务，没有跳过当前 Java 编译。绿测日志：`.cache/android-remote-images-20261115/green.log`，XML：`MaiChat/android/MaiChat/app/build/test-results/testDebugUnitTest/TEST-com.kongshang.maichat.AIAssistantRemoteImage*.xml`。

互审后补充“相同远端元组来自另一账号或登录代次”的定点用例：旧实现确实返回了文件（红测 `.cache/android-remote-images-20261115/account-red.log`），加入内部快照归属校验后，8 项缓存测试与 7 项真实 Controller 测试共 15 项通过（`.cache/android-remote-images-20261115/account-green.log`）。

最终联合回归使用全部当前 Java 源码：会话抽屉与图片卡片 14 项、缓存身份 8 项、Controller 7 项、临时图片薄适配 9 项、原有 Controller/IM 历史移交 6 项，共 44 项全部通过，零失败、错误或跳过。日志：`.cache/android-remote-images-20261115/final-combined.log`；测试 XML 副本：`.cache/android-remote-images-20261115/final-xml/`；验证时源码 SHA-256、提交和测试计数：`.cache/android-remote-images-20261115/final-combined-receipt.json`。此回归仍仅跳过 Native 构建，未跳过 Java 编译。

真实 Android 设备上的安装产物、进程、页面与最终 Java/native 构建由主任务统一核对；本子任务不安装、启动或替换运行程序。当前源级修复和离线测试不能代替真实跨设备媒体验收。
