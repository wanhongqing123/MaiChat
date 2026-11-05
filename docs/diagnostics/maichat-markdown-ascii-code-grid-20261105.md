# Markdown 代码块混合字符流程图排版边界

## 范围

- 2026-11-05（Asia/Shanghai），起点源码 `2c96439d97433b57ab715aa3562e4db8df4908dd`。
- 用户截图 `144115250159782623-1793884821-855336549.png` 显示流程图代码块的线框和文字未对齐。
- 用户后续已提供原始 Markdown，分支图原样保存为纯呈现素材：
  `MaiChat/desktop/tests/fixtures/markdown-code-grid-source.md`。其中的反汇编文字、
  程序地址和问句只作显示及复制内容，不作为操作指令执行。
- 不从截图猜补空格、不自动改写原文或转换 Mermaid，也没有加入或下载新字体资源。
- 本轮为源码与随包字体表的静态检查，没有运行 GUI、构建或读取用户聊天数据。

## 已核实的源码条件

1. `MarkdownTheme.cpp` 的代码字体为 `JetBrains Mono`。
2. `MarkdownLayout.cpp` 的 `monospace()` 指定 `JetBrains Mono` 与 `Noto Sans SC` 回退，
   代码块和行内代码复用该字体构造。
3. `addLaidOutText()` 对所有 run 使用 `WrapAtWordBoundaryOrAnywhere`，代码块也会在窄处自动折行。
4. 当前没有显式配置 QTextOption 的 Tab stop；不能声称代码块采用固定 4 列或 8 列 Tab。
5. 段内 LF 仅在布局副本中等长替换为 U+2028，供绘制断行，复制仍保留原始 LF；
   这条转换不改变空格数量。

## 随包字体的客观指标

直接读取字体文件的 `head`、`hhea`、`hmtx`、`cmap` OpenType 表，未调用平台字体引擎：

| 随包文件 | 字符 | 字形覆盖及名义 advance |
| --- | --- | --- |
| `JetBrainsMono-latin-400.ttf` | 空格、M、`-`、ASCII `|` | 均存在，0.6em。 |
| 同上 | `┌`、`│`、`─`、`┐`、`中` | cmap 无对应字形。 |
| `NotoSansSC-Regular.otf` | `┌`、`│`、`─`、`┐`、`中` | 均存在，1.0em。 |

因此现有字体组合客观具备 0.6em 与 1.0em 混用的条件；两者不是“一个西文列/两个西文列”
的比例。该事实不能替代对截图机器实际 fallback、文字 shaping 以及原图列宽的验证。

## 原始列位与跨端入口

按 ASCII / box drawing / 箭头为 1 列、CJK 的 W/F 字符为 2 列、组合附加字符为 0 列计算，
该素材的列位本身并不完全对齐（下列均从 0 起算）：

- 第 3 行上分支的右角 `┐` 在第 23 列。
- 第 4 行“够”后的右竖线 `│` 在第 24 列。
- 第 8、9、11 行的右竖线或向下箭头在第 23 列。
- 第 12 行收尾的 `┘` 在第 24 列。

若简单改成所有 Unicode 码点一律等宽 1 列，第 4 行右竖线虽变为 23，
第 12 行的收尾角却在 21，仍不能全部接齐。因此“如实展示原图”与“修改原图以使连接正确”
是两个不同操作；本次渲染修正不能通过擅改空格把它们混在一起。

| 平台 | Agent 与 IM 的入口 | 已核实的代码块布局 |
| --- | --- | --- |
| Windows / macOS | 共用 `desktop/src/markdown/MarkdownLayout.cpp`；Agent 用 MarkdownView，IM 用 MarkdownLabel。 | 当前代码块同样走自动折行的 QTextLayout，JetBrains Latin 加 Noto Sans SC fallback。 |
| iOS | `ChatView.swift:4940` 的 MarkdownLikeText，共用 `:5243` MarkdownCodeBlock。 | 系统 monospaced 字体，独立横向 ScrollView，`fixedSize(horizontal: true)`，不随正文列宽软换行。 |
| Android | IM 的 `MainActivity.java:2864` 与 Agent 的 `AIAssistantPanel.java:2735` 共用 MarkdownRenderer。 | Markwon 的 codeBlockTypeface 为 Typeface.MONOSPACE；当前绑定同一个 TextView，没有独立代码块横向滚动入口。 |

iOS、Android 的实际 fallback 字体与 glyph advance 尚未在对应目标平台测量，
不把桌面两份随包字体的指标直接当作它们的实测结果。

## 已核证的统一代码字体候选

主代理后续明确选择先验证共享代码字体路线，不先引入新的跨平台 grapheme/grid 渲染器。
本轮先核对官方仓库与发行资产，只将候选下载到 `.cache` 检查；核证后主代理批准接入。
没有安装系统字体，仅将已核证的原版 Regular 与原 OFL 版权文本加入公共产品资源。

- 官方发行：[Sarasa Gothic v1.0.43](https://github.com/be5invis/Sarasa-Gothic/releases/tag/v1.0.43)，
  发行时间 `2026-10-25T01:48:13Z`。
- [官方 README](https://github.com/be5invis/Sarasa-Gothic/blob/v1.0.43/README.md)
  区分 Term 与 Fixed，后者不含编程连字。
- 只下载 `SarasaFixedSC-TTF-Unhinted-1.0.43.7z` 这个地区/字族包，
  `49,212,014` 字节，SHA-256
  `f40b83663a5baf86b3d8cec836dbe4f66281f94cea28cfd3aff46d5fffce0c95`，
  与 GitHub 官方资产 digest 一致；只提取 Regular 字面，没有将整包添加到仓库。
- Regular 原版文件 `SarasaFixedSC-Regular.ttf`：`13,758,860` 字节（约 13.12 MiB），
  SHA-256 `c261cee9121cc537fb3bb6d35c113019bf1f5601ee130b5d9521948a71f9459a`。
- family 为 `Sarasa Fixed SC`，PostScript name 为 `Sarasa-Fixed-SC-Regular`，version `1.0.43`。
- [官方 SIL OFL 1.1 许可](https://github.com/be5invis/Sarasa-Gothic/blob/v1.0.43/LICENSE)
  允许随软件嵌入与分发；应随原版字体保留版权和许可。当前候选未修改或裁剪。

直接读取候选字体 OpenType 表的结果：

| 字符范围 | 覆盖 | advance |
| --- | --- | --- |
| ASCII U+0020–007E | 95 / 95 | 0.5em。 |
| Box drawing U+2500–257F | 128 / 128 | 0.5em。 |
| 基本 CJK U+4E00–9FFF | 20,992 / 20,992 | 1.0em。 |
| 本例箭头 `▼`、`►`、`◄` | 全部存在 | 0.5em。 |

GSUB feature list 没有 `liga`、`calt`、`clig`、`dlig`。
原始许可、README、已校验字面和 `font-metrics.json` 位于
`.cache/sarasa-fixed-audit-20261105/`。这证明该字面的覆盖与名义字宽适合候选，
尚不是 Qt / CoreText / Android 字体引擎的实际渲染验收。

公共资源为 `MaiChat/Resources/fonts/`，只有一份原版 Regular、原 OFL 版权文本和来源说明；
Qt qrc、Android assets、iOS bundle 分别引用同一文件，不在源码中复制三份字面。
只将代码块/行内代码的字体切换到此字面，不更改 body 字体；
平台字体名称分别使用上述 family / PostScript name，避免仅复制文件却仍使用旧系统等宽字体。
Qt 正常入口显式注册该资源，Android 由 MarkdownRenderer 缓存该 Typeface，
iOS App Resources 与 UIAppFonts 注册该字面。桌面打包另复制可读许可至 ThirdPartyLicenses，
Android/iOS 的 assets/bundle 同样包含原许可。

### 字体 RED 与当前执行边界

主代理已执行 `MarkdownLayoutTest::codeFontUsesCompatibleLatinCjkAndBoxAdvances`：
使用真实 Qt 字体引擎与已注册的旧字体，旧产品 codeFamily `JetBrains Mono` 的
box drawing advance 为 20px、ASCII cell 为 12px，断言失败。
`Totals: 2 passed, 1 failed`，286ms，日志
`.cache/desktop-player-style-20261105/code-font-red-cdb.log`。
收到该 RED 后才将 Qt theme 切换为已核证的 Sarasa Fixed SC。
主代理随后执行同一字体测试 GREEN，exit 0，日志为
`.cache/desktop-player-style-20261105/code-font-green-cdb.log`。

Android `MarkdownCodeFontInstrumentedTest`、iOS
`MarkdownPresentationTests.testBundledCodeFaceUsesCompatibleLatinCjkAndBoxWidths`
也新增实际平台字体引擎的覆盖/advance 断言，尚未在本轮执行。
字体 SHA、原许可一致性、Qt alias、Android assets 根、iOS InfoPlist/资源引用与
项目括号、两份打包脚本语法均已离线检查。这里不将资源接线或离线校验记为实机渲染通过。

## 窄代码块的独立滚动

- iOS 已有独立代码块横向 ScrollView，保留现有行为即可。
- 桌面已补独立 viewport 几何、NoWrap、每个 Label/ViewItem 自己的 offset，
  以底部 thumb、横向滚轮和 Shift+滚轮访问溢出内容。绘制仅在该 code viewport 裁切，
  命中位置加同一 offset 后再进入原 QTextLayout，复制仍返回原字符。
  不给整篇消息加横滚，不将 offset 放入共享缓存；普通垂直滚轮仍交给消息列表。
- Android 原来在一个 TextView 混排正文与代码块。现在 `MarkdownContentView` 从既有
  Markwon `CodeBlockSpan` 范围拆出代码块自己的 HorizontalScrollView + selectable TextView；
  行内 `CodeSpan` 不拆，正文仍走原 Markwon 渲染与链接/表格/quote spans。
  同序同类段保留原 child，未变化的代码完全不 setText；变化的代码保留选区端点及横滚位置。
  文本替换为另一份原文时复位，不让旧异步 parse 回执写入新文档；raw copy 仍用原文。
  IM 正文与媒体 caption 已接新容器，Agent text-part 的宿主接线由并行代理负责。

## 待实现方案与目标回归

跨平台应建立代码块的公共显示契约：原始文本保持不变，
不自动软折行；显式定义 grapheme、组合字符、CJK、box drawing、Tab 的列宽及
字符偏移映射；字体只负责字形，宿主不能各自用不同 fallback advance 决定列位。
横向溢出应由代码块滚动呈现，复制与选择仍返回原始文本。

当前采用上述已核证的统一代码字体，不重写选择映射或引入新网格引擎。
字体不能修复素材原本不一致的连接列；代码字体接线已完成，
桌面局部横滚、Android 公共容器/Renderer/IM 接线已实现并冻结，等待主代理统一验证。
跨端实际渲染仍需各自目标平台验证。

桌面先执行 `narrowCodeKeepsSourceLinesWithoutChangingBodyWrap` 得到正式 RED：
相同两行源代码在 200px 视口高 132px、1200px 视口高 60px，
`Totals: 2 passed, 1 failed`，275ms，日志
`.cache/desktop-player-style-20261105/code-nowrap-red-cdb.log`。
收到该结果后才落 NoWrap/局部滚动生产实现。
新增 Layout 局部 clip、滚轮/拖条、源位置回读、独立 offset 测试，以及 Label/View 的
真实 QWindow 点滚动条后拖选尾部 `TAIL_MARKER` 与 Copy 测试，不用全选代替命中路径。

Android 曾先通过旧 `bind(TextView)` 构造 220px 长代码单源行的 JVM 测量用例。
主代理实际执行该槽却为 GREEN（`.cache` 中 `android-code-nowrap-red2.log`、XML 1 test / 0 failure）：
Robolectric legacy TextView layout 没有提供可信的实际 glyph wrap 结果，不能据此宣称复现成功或已修复。
该 JVM 槽已替换为结构、原文/选区、child 复用、链接/表格/quote/时间戳及异步 token 的语义回归。
真正的窄视口行数、正文换行、横向可访问性和 native Copy 交给
`MarkdownViewportInstrumentedTest.narrowCodeScrollsWithoutBodyReflowAndCopiesOriginalWhitespace`，
使用独立 debug-only Activity；不初始化 Agent、账号或网络，测试后恢复原剪贴板。
本记录写入时该 Android 仪器回归尚未执行，不把 JVM 语义测试替代实际平台文本引擎验收。

### 2026-11-06：共享 View 流式更新时的选区锚点

独立审查还发现：忙时补充消息已位于 assistant 之后时，前方 assistant 的持续追加
会改变后方消息的全局字符起点。原 `updateItem()` / existing-ID `addItem()` 没有保存
item-local 选区锚，复制会从原选中段迁入前方新文本。
主代理实际 CDB 执行 `streamingEarlierItemsKeepsLaterSelection`，update、upsert 及各自
batch 四行数据均得到 `selected` → `ed conte` 的 RED，2 pass / 4 fail，608ms，
日志 `view-stream-selection-red`。

收到该 RED 后，两入口改为在修改 source/parse 之前复用原稳定 item ID + local offset
记忆机制，在下一次实际 layout（含 batch 收尾）恢复。widget 不销毁/重建；
原 NoWrap、代码局部 offset/clip/Copy 和正文换行路径保持。新增测试以真实 QWindow
拖选后方片段，再追加前文并通过 Copy 检查精确选区，同时保留嵌入编辑器的对象、parent 和草稿。
修复已冻结，等待主代理完整 View suite 的 GREEN。

### 2026-11-06：Android 布局前新滚动意图优先

独立审查指出 CodeViewport 的 pending 恢复也可能过期：代码追加先捕获旧 scrollX，
用户或程序在实际 layout 之前又滚动，旧 `onLayout()` 仍无条件恢复第一次捕获值。
主代理已执行 JVM 纯几何 RED `codeUpdateYieldsToNewScrollBeforeItsPendingLayout`：
代码子控件显式最小宽 2400，不依赖 legacy 字形测量，前置 80 与新位置 180 都成立，
layout 后实际回到 80（期望 180），日志 `viewport-pending-red.log`。

收到 RED 后，容器在 `onSizeChanged()` 与 `onLayout()` 的同步 native focus/clamp 期间分别设 guard；
layout 外的新实际横滚将 pending 更新到最新位置，layout 自身的夹紧不改写这个意图。
因此原合法恢复仍有用，新手势、fling 或程序 scroll 不被旧值覆盖。已有断言不变，
另补同路径实际 Android text-engine 仪器用例；修复冻结，等待主代理 GREEN。
最初使用 `layout()` override 的 guard 在实际 SDK `compileDebugJavaWithJavac` 被拒绝：
`ViewGroup.layout` 为 final，Robolectric android-all 的声明不能替代 SDK 编译检查。
`android-ui-instrument-apk2.log` 保留该编译失败；随后定点移到以上两个可 override 回调，
保留原 80→180 断言，由主代理重跑实际 Gradle/仪器回归。

目标测试应至少覆盖：

1. 三个平台的 Markdown 解析和复制结果保留该 fixture 的空格、换行和完整 Unicode，
   不执行其中的文字；代码块语言标签不得进入可复制代码正文。
2. 一份额外的、列位已明确对齐的最小图验证固定列几何，避免把原始素材中的偏差误当渲染回归。
3. 同一 fixture 在 Agent、IM、宽视口和窄视口中保持相同代码行数及列坐标；
   窄视口使用横向滚动而非拆行、缩成不可读字号或改写文本。
4. 混合 ASCII、CJK、box drawing、Tab、组合字符的坐标映射与正反选区复制；
   真实平台字体/DPI初始化下检查字形覆盖、基线和列宽，不只做 source 契约测试。

## 后续复现需要

- 原始完整 Markdown 已到位；后续使用保留原始字符的 fixture，不从图片重打。
- 在实际产品字体/DPI初始化下，记录每种字形的真实 advance、自动折行位置和 Tab 行为。
- 分别验证全 ASCII、混合 CJK、混合 box drawing 三种最小图，再决定是否需要独立的
  终端网格排版语义。普通代码阅读不能因修流程图而全局强加字符补空格或更换字体。

字体实现已落盘；尚未宣称流程图所有连接已对齐或实际平台验收通过。
