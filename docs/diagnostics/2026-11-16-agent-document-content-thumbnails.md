# agent_send_file 文档内容缩略图

## 操作和预期

在创作中心让 `agent_send_file` 交付工作区中的 PDF 或 Markdown。回复中的文件卡显示 PDF 第一页或 Markdown 格式化正文首屏，保留文件名、类型和大小；点击沿用现有完整文件预览，关闭后回到同一回复。普通 ZIP 等文件保留文件信息卡。

本任务与目录整理方案分开：没有迁移、删除目录，也没有增加磁盘缩略图缓存。

## 源码与实现边界

- 本地 HEAD：`c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`，含此前已接收但尚未在本机提交的 WIP；本次只交付 manifest 中五个 iOS 文件的精确增量。第五个 `ChatView.swift` 只增加普通 Markdown 强快照 factory 和 DEBUG 驱逐测试入口，保留已有导航改动。
- 最终 R4 patch：27034 字节，SHA256 `f10936da2794a8a3b7e3c069ed343ed3e8503bd9dc48a3c7a1f1bbcfb7e253b2`；`AIAssistantView.swift` postimage blob `9c6be020b4d2ac6dba6887db17065333a4673f82`，`ChatView.swift` 为 `254dcd6b2b94ed5e152acfa32fcd1231bf8a2035`。此前全部候选由此一次完整增量替代，不叠加应用。
- iOS PDF 复用 PDFKit 第一页 cropBox；Markdown 复用 `MarkdownPreparation` 和 `MarkdownLikeText`，使用有界纸面 `ImageRenderer`，没有第二套 Markdown parser。
- PDF 完整输入上限 24 MiB，Markdown 完整输入上限 64 KiB。超限显示文档占位和原文件元数据，不把截断片段重排后冒充首屏；原完整预览仍可使用。文件通过原工作区策略后，以 fd 再核实际路径、大小、设备/inode、mtime/ctime。读取及发布图像前后再核版本。
- 32 项、16 MiB 内存缓存包含会话工作区、规范路径、版本、内容摘要及字体设置。视图任务取消后不发布旧结果；场景重新激活或点击文件卡会重新验证。未实现后台实时文件监听。
- 读取、准备和 PDF 解码使用现有后台工作器。坏文件、加密 PDF、超限文件显示普通文档占位并保留元数据和原预览操作。
- 独立缩略图许可限制为两项运行、八项仅保存元数据/continuation 的等待项。许可在读取 Source.data 前取得，覆盖后台实际返回、渲染、校验和发布；任务取消不会提前归还仍在后台工作中的许可。等待取消按 UUID 移除，交接时取消的许可归还一次；过载保留文档卡，点击或进入前台可重试。
- Markdown 离屏渲染通过普通 `MarkdownLikeText.preparedDocument` factory 接收并强持有 PreparedMarkdown，不依赖 NSCache 在跨 await 期间继续存在，也不等待离屏 View.task 偶然完成。
- 完整 Markdown 文件沿用已有系统 QuickLook 文本预览；本次新增的是格式化卡片缩略图，没有宣称完整 Markdown 富文本预览已改造。

## iOS 复现与实际验证

时间：2026-11-16。模拟器：`34893056-B994-4CFE-96C2-5526174AC370`，iPhone 17 Pro，iOS 26.5。测试使用隔离 bundle `com.maichat.review.nav.MaiChat`，自有 PDF/MD/ZIP，经普通创作中心消息行和文件卡显示，不调用模型或读取用户消息。

1. 原产品文件卡不含内容图：原同例实际 `1 test / 2 failures`，缺 PDF、MD 两张内容图。记录 `thumbnail-red.xcresult`。
2. 接入候选后原同例 `1 / 0`。增强测试最初选到了屏外会话抽屉，失败属于测试定位；改为查找包含自有 PDF 卡的历史 ScrollView，没有为此改变产品滚动行为。
3. 最终源码同例 `1 / 0`，UIKit 用时 21.959 秒，完整构建测试用时 56.595 秒。检查实际 PDF 红色页眉像素、Markdown 正文字形像素；PDF/MD 点击完整预览、关闭及 MD 再次打开均通过。
4. 已亲看卡片、PDF、MD 实际截图及 24 秒操作录屏。另正常无测试参数启动模拟器 App，PID 26963，显示登录页；该检查只证明正常入口启动，不当作已登录会话验收。

随后独立静态审查指出：Markdown 表格按全部行决定列宽，截取前 64 KiB 重排时，后面的长单元格会丢失，首屏列位置与全文不同。添加自有 750 行、超过 64 KiB、尾部长 A 单元格的表格，实际旧候选 `1 test / 2 failures`：仍出现了应当拒绝的截断缩略图。R2 改为完整有界读取、精确长度校验及完整 UTF-8 解码；同例和小文档原操作合跑 `2 / 0`，UIKit 32.660 秒、构建测试 75.531 秒。小文档用时 22.133 秒，超限表格无图及完整文件打开关闭用时 10.527 秒。已查看超限占位实际截图。

私有证据目录：`/private/tmp/maichat-ios-file-thumbnails-20261116`。R2 阶段对应 `thumbnail-r2-green.xcresult`、`r2-images`、`oversize-r2-visible.png` 和 `ios221-file-thumbnails-r2.json`。此前 `thumbnail-ui-green4.mp4` 保留普通小文档操作过程，随后 cropBox 和完整读取限制均已在 R2 两条原用例重编验证。

资源边界补核：旧候选在文件 worker 读取 Source.data 后把闭包排入共享串行解析队列；队列取消标志不会立即移除闭包，16 MiB 最终图像缓存无法限制这些排队原始字节。R3 仅增加上述缩略图专用许可。精确抽取产品 gate 源码以 Swift 6 编译，113 个合同检查通过，覆盖两项运行、八项等待、过载拒绝、取消移除、100 次交接取消；不是实机洪泛/OOM 实测。R3 原两条 UIKit 用例亦通过（完整 94.231 秒）。

缓存交付补核：在 Markdown prepare 完成后强制移除同一 NSCache entry，旧候选实际 `1 / 1` 失败，图像存在但正文字形像素为 0（UIKit 6.008 秒，全程 60.983 秒）。R4 普通 factory 直接返回并持有 prepared 快照；三条 UIKit 用例已 `3 / 0`，普通流程 21.985 秒、强制驱逐 4.854 秒、超限表格 10.410 秒，合 37.249 秒、完整构建测试 77.539 秒。已亲看 `r4-images` 普通卡片截图和 `eviction-r4-visible.png` 强制驱逐后的正文图。DEBUG 强制驱逐入口不进入 Release 运行路径。

## Mac PDF adapter 私有消费验证

只消费协作者冻结包中的 `AgentDocumentThumbnailMac.cpp`，未整体应用其 CMake、Core、Panel 或替换用户桌面 App。

原 adapter 使用额外 Y 翻转，并直接把较小 PDF 页交给较大目标矩形的 drawing transform。自有红上绿下 PDF 在 0/90/180/270 度、正常及非零 cropBox 的 8 个样本中实际全部失败：小块内容居中且上下颠倒。候选去掉冗余翻转，显式像素缩放后组合逻辑页旋转和 crop 变换，原 8 样本全部通过，已查看输出图片。

adapter blob：`3dbe1139e9d606ea98272fa0736829d7c82bb60a` → `7921a0cc9cc7990dc76d9f2cc3c8aa8e6e1b0f15`；canonical SHA256 `5872362ea128b0cf1f1d725dcf54f3b9ad9365cc43e8e62c5e93c4f2358a78cd`。1474 字节 patch 已交协作者，收到只应用一次、逐 blob/hash 相等回执。本机没有把独立 adapter 验证冒称完整桌面产品验收。

私有 Mac 证据：`/private/tmp/maichat-mac-document-thumbnail-20261116`，含 `OrientationTest.cpp`、原/新图片、`orientation-baseline.txt` 和 `orientation-green.txt`。

## 真机更新边界

iOS 220 Release 构建成功（123.671 秒），签名校验 0；在收到 Markdown P2 审查前，已按既有用户授权完成正常安装，设备版本回读 `2.2.4 (220)`。本地传输候选 executable SHA256 `8760adc3cc3ae7f9c8c29e9daa0092a9e023be4c2c77be8a7736e42e0f8da0d1`。无参数启动被 OS 因手机锁定拒绝（CoreDevice 10002 / FBS Locked），没有声称 220 已启动或实机体验验收。

R2 正常 Release 221 曾构建但未安装。最终 R4 重新构建同一版本 221 成功（117.262 秒），五个源码文件均与 R4 manifest 的 SHA256 相等，deep/strict 签名验证 0。本地传输候选 executable SHA256 `da4b81abda3c367faa205b38c76aed4eb616097430ebcd22c1a35da732ba14a9`。普通安装成功，设备新鲜回读 `2.2.4 (221)`；无参数正常启动成功，PID `68310`，执行路径对应本轮安装的 bundle。没有从设备独立读回可执行文件 SHA，不声称做过该项。

设备为已配对 iPhone 14 Pro Max，UDID `00008120-001E0DEC0263C01E`。使用普通就地更新流程；未读取用户数据库/消息验证数据内容，不把应用路径或安装成功当作内容保留验收。模拟器已按上述真实页面操作验收；真机本轮只验证安装、版本和正常启动，未把它称为真实触摸全部验收。Windows/Android 由各自协作者验证，不能从本机测试推断其整包已安装。
