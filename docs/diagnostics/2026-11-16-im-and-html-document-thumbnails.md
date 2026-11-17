# IM 与 HTML 文档内容缩略图

## 用户操作和预期

在 IM 好友聊天中接收或发送 PDF、Markdown、HTML 文件，消息直接显示文件内容首屏，下面保留文件名、类型、大小；点击仍打开完整文件，关闭后回到聊天。创作中心补上 HTML 首屏及完整 HTML 预览。普通 ZIP 文件保留文件信息卡。

本轮只实现 iOS。没有修改 PC 输入、驱动、SDK、网络下载或目录整理方案。使用项目已有的系统 WebKit、PDFKit、SwiftUI，没有增加第三方依赖或系统安装项。用户最新要求暂不向 house-maichat 发送消息或文件，本轮后续证据保存在本机，不再发送。

## 源码边界

本地 HEAD `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c` 含此前接收但未在本地提交的 WIP。本轮基于已接收的 221/R4 快照，保留原有账号、导航、媒体和远程会话改动。

最终精确增量为八个文件，56307 字节，SHA256 `c8e37e1e79e3c2029a68681bc13ea525537a4c1bcf9e4ccbdf58d17f33c762cb`。此前 49256 字节只读候选由此替代，不叠加。七个 before 快照及新增文件清单、逐 blob/SHA 在私有 `ios222-im-html-final.json`；未把整份其他 WIP 当作本轮增量提交。

- 原创作中心私有 renderer、许可、图像缓存迁入单一 `Chat/DocumentFileCard.swift`；两处复用。Xcode 工程只增加该文件的引用、分组和编译项。
- 创作产物继续使用原工作区路径策略。IM 附件只读取应用沙盒内的普通本地文件，显示名可以与磁盘 UUID 名不同；fd 实际路径、大小、inode 和时间版本前后核验。
- 缓存及呈现身份包含 IM owner、messageID、路径、MIME、字体和版本。body 不呈现身份已不匹配的旧图；既有 `mediaFileRevision` 驱动下载完成后的重新读取，点击和回前台也会重新核验。
- PDF 完整输入上限 24 MiB，Markdown 64 KiB，HTML 512 KiB。超限/不可解析文件显示文档占位及原元数据，不截断片段重排。
- 共用两项运行、八项元数据等待许可，等待取消按 ticket 移除；覆盖文件读取、后台解析、HTML 原生渲染、校验、发布全过程。原生 PDF 请求已提交时，取消不提前归还许可，等待真实回调或进程终止通知后清理。
- 图像仅在 32 项/16 MiB 的 NSCache 中缓存。Markdown 保留 221/R4 的强 PreparedMarkdown factory，避免缓存驱逐后把 ProgressView 存成缩略图。
- HTML 使用非持久 WKWebsiteDataStore，关闭页面脚本，不提供本地 baseURL；先行 CSP 禁止外部资源，保留内联样式及 data 图片/字体，只允许首次 about:blank 文档导航。捕获固定 300×200 首屏为 PDF 数据，再交给同一 PDFKit raster。未新增磁盘缩略图缓存，WebKit 自身资源由系统 API 管理。
- 代码 Diff 的 SHA256 在生成缩略图之前核验；完整预览继续核验，失败不渲染 HTML。

HTML 首屏原生 API 依据：[Apple WKPDFConfiguration](https://developer.apple.com/documentation/webkit/wkpdfconfiguration)、[Apple createPDF](https://developer.apple.com/documentation/webkit/wkwebview/createpdf(configuration:completionhandler:))、[Apple nonPersistent websiteDataStore](https://developer.apple.com/documentation/webkit/wkwebviewconfiguration/websitedatastore)。

## 实际复现与修复

2026-11-16，模拟器 iPhone 17 Pro/iOS 26.5，UDID `34893056-B994-4CFE-96C2-5526174AC370`。隔离 bundle `com.maichat.review.nav.MaiChat`，自有 PDF 红色页眉、Markdown 标题/正文及 HTML 绿色 CSS 页眉样本。通过正常 RootView、IM 会话/文件气泡、创作中心和完整预览入口操作，没有调用付费模型。

1. 创作中心 HTML 原无内容图：真实 RED1。IM 首轮被通知引导层阻挡属于测试定位，测试按正常“稍后”按钮关闭后重跑；收、发两侧 PDF/MD/HTML 三类图片均缺失，真实 RED6。
2. 接共用组件后五项普通 UI 用例首次通过。随后加实际 PDF 页眉、Markdown 正文字形、HTML CSS 颜色像素检查，HTML 脚本改变背景的负例，以及无效 Diff SHA 不生成图和完整校验错误路径。
3. 六项回归虽通过，但开发者看录屏发现创作中心旧 QuickLook 打开 HTML 是白页，close 按钮存在不能证明正文呈现。补正文断言，真实 RED1（15.692 秒，完整 50.704 秒）。复用 IM 已有完整 HTML viewer 后同例转绿，并亲看完整页面截图。
4. 完整文本预览限制为 8 MiB，fd seek 确认长度后核验读取结果，不返回已知短读或截断片段；超限仍可通过原分享工具栏保存文件。HTML/Markdown 类型识别兼容 MIME 参数和文件后缀。没有复制第二套 HTML 或 Markdown renderer。

## 最终验证

最终源码六条普通 UIKit 用例 `6 / 0`，UIKit 合 116.630 秒、构建测试 160.790 秒：

- 创作 HTML 缩略图、脚本负例和完整正文：11.787 秒。
- 原 PDF/MD 卡片、完整预览、关闭/再入：21.350 秒。
- IM incoming/outgoing 三类像素、滚动和打开/关闭：56.762 秒。
- 无效 IM HTML Diff 的缩略图拒绝及完整校验错误：12.056 秒。
- 强制 Markdown prepared 缓存驱逐后仍有正文：4.877 秒。
- 超限完整 Markdown 表格不生成截断首屏、仍能打开文件：9.797 秒。

精确抽取新共用 gate 源码，以 Swift 6 独立编译，113 个合同检查通过（两项运行、八项等待、过载拒绝、取消移除、100 次交接取消）。不是设备洪泛或 OOM 实测。

已亲看收/发截图、HTML 缩略图和完整正文；录屏记录普通加载与打开/关闭，未用测试绿掩盖中间的白页问题。最终 Source 的 `git diff --check` 为 0。

私有证据：`/private/tmp/maichat-im-html-documents-20261116`。`im-html-final.xcresult`、`final-images`、`final.log` 对应最终源码；`im-html-ui-green2.mp4` 保留中间录屏，正是在该录屏中发现并随后修复旧 QuickLook 白页。`SlotsContract.swift`/`slots-contract.log` 为 gate 合同证据。

本轮 IM UI 验证使用隔离内存中的双方消息角色；没有将它描述为两套真实 SDK 账号互传验收。HTML 已验 CSS 与脚本负例；外联限制为配置/源码核对，没有声称做过所有 URL scheme 的网络捕获。Root 当前暂停新源应用，且用户已要求停止向其发送消息，因此本轮没有声称新的独立 peer review 或跨端合源完成。

## 真机

最终 Release 222 构建 0，116.908 秒；deep/strict 签名验证 0，八个文件与最终 manifest SHA 相等。本地传输候选 executable SHA256 `4d44e39ee48205400925ebd27b6f92a3846aba40016fd83ca3285b480585e326`。已正常就地安装，设备新鲜回读 `com.kongshang.maichat / 2.2.4 (222)`，无参数正常启动成功，PID `68595`。对应 iPhone 14 Pro Max，UDID `00008120-001E0DEC0263C01E`。回执保存在本机 `ios222-delivery-safe.json`；遵守用户停止外发要求，未向 house-maichat 发送安装/启动回执或本诊断。

不以安装成功替代文档触摸验收；没有读用户数据库/消息核数据内容保留，也没有从设备独立读回 executable SHA。未实施目录迁移或替换桌面 App，未宣称 Android/Windows 已有本轮新功能。
