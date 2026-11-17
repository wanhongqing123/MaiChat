# 95 KB Markdown 缩略图及重复文件名

## 现场与定位

2026-11-16 22:42，用户在 iOS 222 的 IM 聊天中查看 `2026-11-16-maichat-directory-plan-readable.md`：文件卡仍显示普通文档图标，下面又出现 `[文件] 文件名`，导致名称重复。

两条可证原因：

- 该报告实际为 95321 字节，超过 222 缩略图的 Markdown 64 KiB 读取预算。此前只用 147 字节正文等小样本验收，漏掉了这份已经发出的真实报告。
- SDK 占位文本 `[文件] 文件名` 保存在 message.text，`MessageBubbleView.attachmentCaption` 又把它当正文显示；该路径还没有优先使用独立 mediaCaption。

复现不读取用户消息数据库：将仓库中同一份 95321 字节原文复制到隔离模拟器 bundle 的 Documents 测试输入，再由已有普通 IM Root、会话及文件气泡呈现。分别验证无配文和有真实配文两种消息。

## 实际 RED → GREEN

模拟器为 iPhone 17 Pro/iOS 26.5，UDID `34893056-B994-4CFE-96C2-5526174AC370`，隔离 bundle `com.maichat.review.nav.MaiChat`。

旧 222 产品实现同例实际 1 test / 5 failures，UIKit 48.546 秒、完整构建测试 95.897 秒：两次均没有报告缩略图；文件名匹配数量均为 2；真实配文没有呈现。不是根据 screenshot 猜测后直接宣布修复。

修复后同例通过，20.242 秒：报告确有内容图像且正文字形像素通过，名称只出现一次，独立真实配文可见。加跑 prepared cache 强制驱逐与超限完整 Markdown，最终 3 / 0（4.981 秒、11.365 秒，完整 72.575 秒）。已亲看两张报告截图和操作录屏。

## 修改

- Markdown 完整缩略图预算从 64 KiB 调整为 256 KiB，覆盖该真实报告；仍完整读取、完整解析、前后核身份，没有改为截取前缀重排。两项运行/八项等待许可及 16 MiB 图像缓存继续共用。
- 文件消息优先显示独立 mediaCaption，精确抑制 `[文件] ` 加当前文件名的系统占位文本。其他媒体的配文路径不变。
- 测试使用完整原文和实际 SDK 占位格式，同时检查真实配文不被删除；超限样本调整为超过新预算，避免把原 64 KiB 断言误当新语义。

本轮仅四源，相对本地 222 快照增量 7439 字节，SHA256 `be72ab3125cc3468d531cc6355b08680bd95fe8d32b996bcd3fbf2a9594b858a`。完整 before/blob、after/blob 和 SHA 在私有 `ios223-report-fix.json`。本地 HEAD 仍为 `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`，保留此前 WIP，不把其他工作混入本轮。

私有证据目录：`/private/tmp/maichat-ios-md-report-fix-20261116`，含 `report-red.xcresult`、`report-green.xcresult`、`green-images`、`report-ui-green.mp4`。原文仍为仓库目录方案文档，未改目录方案内容或实施迁移。

## 交付边界

最终 Release 223 构建 0，117.564 秒；deep/strict 签名验证 0，四源 SHA 与修复 manifest 相等。候选 executable SHA256 `fa7b8d974f46c869d5c76064fd1da9856ee5ce421cd4268bca35d648800cc541`。普通就地安装成功，设备新鲜回读 `2.2.4 (223)`，无参数正常启动及后继进程查询成功，PID `68632`；本机回执 `ios223-delivery-safe.json`。

未声称两套真实 SDK 账号互传验收或设备触摸体验全部验收；没有从设备独立读回 exe SHA，没有读用户数据库/消息核内容保留。按用户要求不向 house-maichat 发消息或文件，本轮证据仅保存在本机；没有修改 PC 输入、依赖、驱动或 Android 源。
