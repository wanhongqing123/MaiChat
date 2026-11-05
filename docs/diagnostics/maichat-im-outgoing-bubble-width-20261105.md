# Windows IM 发送文本气泡右侧空白调查

## 范围与操作

- 时间：2026-11-05，Asia/Shanghai。
- 对照源码：`2c96439d97433b57ab715aa3562e4db8df4908dd`。
- 用户截图：`144115250159782623-1793883484-855336451.png`，红框指向一条发送文本
  在正文、内联时间戳结束后仍保留的大段浅蓝背景。本记录不抄录截图中的聊天正文或账号。
- 预期：发送文本在可用上限以内按实际 Markdown 与时间戳所需宽度收紧；
  超过上限时正常折行，气泡靠右，投递标识仍在气泡外。接收正文的整行阅读布局保持原有语义。
- 本轮只读源码及用户主动提供的截图，没有读取聊天数据库、日志正文或凭据，
  没有发消息、网络请求、构建、启动或替换 App。未完成实际目标构建复现。

## 源码证据

### 主因：字符阈值把最小宽度锁成可用上限

`MaiChat/desktop/src/ui/MainWindow.cpp`：

- `createMessageBubble()` 的 `expandedTextBubble` 条件：纯文本发送消息只要
  `message.text.size() >= 50`，或包含一个换行，就归入 expanded。
- 气泡先后两次调用 `applyMessageBubbleWidth()`；最后一次已计入实际尾部元信息宽度。
- `applyMessageBubbleWidth()` 先由视口、列表边距及气泡外元信息得出上限，
  outgoing 再限制为不超过 1280；随后执行：

  ```cpp
  bubble->setMaximumWidth(maximumWidth);
  bubble->setMinimumWidth(expanded ? maximumWidth : 0);
  ```

因此，达到字符阈值或仅有换行的发送文本，其最小与最大宽度完全相等。
正文和时间戳即使早已结束，背景也不能继续收窄。
这条路径与截图中“文字左侧集中、右侧仍有长条浅蓝空白”的表现一致；
但本次未对应截图进程和二进制，不能将只读调用链核对写成实际产物复现通过。

### 次因：MarkdownLabel 的默认建议宽度固定为 360

`MaiChat/desktop/src/markdown/MarkdownLabel.cpp:199` 的 `sizeHint()`
硬编码 `QSize(360, ...)`，注释明确说明沿用了旧 QTextBrowser 的宽度。
`MarkdownMessageView` 也采用横向 Expanding 策略，并在 `contentRow` 中带 stretch 1。
所以仅删除 50 字分支或将 minimumWidth 改为 0，仍不足以让短文本稳定按实际内容收窄。

### 时间戳与气泡外回执不是这段空白的来源

- `MarkdownMessageView::setMessageMarkdown()` 用 `setTrailingNote()`
  将时间戳交给同一 Markdown 排版层，紧随正文末行。
- 普通纯文本的 `inlineDate` 为 true，不会在 `bubbleRow` 另加一个外部日期控件。
- 气泡外投递标识使用独立 16px 控件；`trailingMetadataWidth` 为它预留约 24px，
  这部分从可用气泡宽度扣除，不会制造气泡内部数百像素的空白。

## 建议的最小修正范围

1. 只对普通 outgoing 文本按真实布局结果确定所需宽度，不再以字符数或是否换行
   强制占满上限。incoming 的 fullWidthBody、审批和附件布局保持各自原语义。
2. 优先复用 Markdown 排版层现有的自然宽度能力，连同 `TrailingNote`、主题字体、
   列表缩进等一起测量，不用 `QFontMetrics` 对 Markdown 原文估宽。
3. 将测量限制为“给定最大内容宽度”，再加当前气泡实际左右 padding，夹到现有上限内。
   `MarkdownLayout::naturalWidth()` 已有对应能力；
   `MarkdownView.cpp:304` 的 Bubble 分支已有“先按上限排，再按自然宽度收窄”的参考路径。
4. 不直接全局修改 `MarkdownLabel::sizeHint()` 的历史约定来解决一个宿主问题。
   可给 IM 宿主提供基于现有文档/布局缓存的受限宽度查询，再由 MainWindow 选择使用；
   避免在窗口每次 resize 时重复解析整段 Markdown 或反复改动绘制用布局。
5. 窗口显示、缩放及消息恢复时仍应重算上限；保留现有按宽度定高、时间戳折行及投递标识定位。

本记录仅提出修正边界；MainWindow 及其测试由主代理统一修改，本轮没有改动这些文件。

## 不使用真实聊天的最小复现与回归

在现有 `MainWindowLayoutTest` 的 FakeRemoteIMClient / `appendMessageForRestore()`
路径构造消息，只本地恢复，不调用真实发送接口：

1. 同一视口添加 49 个 `i` 与 50 个 `i` 的两条 outgoing Sent 文本。
   当前实现会在 50 字处由默认建议宽度跳到整条上限；修正后两者宽度仅随一个字符增长。
2. 添加合成多行 `a\nb`。当前实现也会占满上限；修正后应围住两行可见内容及末行时间戳。
3. 添加一段小于可用上限的合成长中文/中英混合文本，记录实际正文、时间戳与气泡右边界距离；
   应只有正常 padding，而非长段额外蓝底。
4. 在最小窗口、1680px 宽窗口及不同 UI 缩放下重复；真正超长文本仍受上限约束并正确折行，
   时间戳不得截断或被外部投递标识遮挡。
5. 覆盖消息在 `show()` 前已恢复、窗口放大/缩小、包含行内代码/链接/列表的情况。
   代码块、表格等本身具有整行背景的块应保持其布局边界，不能盲目按纯文本收缩。
6. 运行原有 incoming 宽屏及恢复消息回归，另检查引用、图片配文和回执，
   防止为了收紧 outgoing 文本改变其他消息类型。

上述方案尚未在本轮执行。实际截图/录屏、源码提交、运行进程和产物身份应在实现后追加。

## 2026-11-06：受限自然宽度查询的缓存身份回归

主代理已报告 compactBubble 定点回归通过，但随后完整 MarkdownLabel suite 在
`naturalWidthIncludesTrailingNoteAndKeepsCurrentLayout` 失败：添加末行时间戳后的
`notedWidth > shortWidth` 为 false。实际 CDB 日志为
`.cache/desktop-player-style-20261105/code-label-whole-diagnostic-cdb.log`，12 pass / 1 fail。
此前同套 NoWrap、代码局部横滚与精确 Copy 回归均通过，不将这些路径重写为解决宽度问题。

根因为 setter 的缓存归还顺序：`setTrailingNote()` 先写入新 note，再由 `invalidate()`
将无 note 的旧 paint/probe 归还到缓存，旧几何因而被标成新 note 键；随后的宽度查询
命中这份旧 probe。`setMarkdown()` 和 `setTheme()` 的 source/zoom 变更也有同一风险。
修复仅在三个 setter 改输入前先沿旧键归还布局，原测量、绘制和 NoWrap 路径保持。

新增 `changingInputsKeepsCachedLayoutsUnderTheirOriginalIdentity`，在同一控件已 warm
paint/probe 后变更 note、正文和 zoom，再恢复原值并由另一控件复用旧缓存，
检查实际可见文字和宽高。原失败断言保持，等待主代理重编后的完整 suite 结果。
