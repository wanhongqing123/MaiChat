# Markdown 表格第二列选区调查

## 操作与预期

- 2026-11-05（Asia/Shanghai）；起点源码 `2c96439d97433b57ab715aa3562e4db8df4908dd`。
- 用户提供截图 `144115250159782623-1793884273-855336495.png`。
  表格第一列“PowerShell + System.Drawing”可选，第二列的失败说明无法选择复制。
- 合成复现使用同一表格的纯文本和行内代码两种版本，宽 1200，使两格位于同一视觉行。
  逐格拖选应能得到该格完整可见文字，去掉 Markdown 标记，正反拖动结果一致。
- 只使用主动提供的截图与合成文本，没有读取用户数据库、请求供应商或发出聊天消息。

## 已定位的调用链

`MarkdownView::positionAt()` 和 `MarkdownLabel` 的拖选事件最终都进入
`MarkdownLayout::positionAt()`。同一表格行的每个 cell 各自形成一个 `TextRun`，
拥有不同 x 坐标，但 y 与行高可以相同。

原实现只比较 y，找到第一个包含该 y 的 `TextRun` 就 `break`，然后才把 x 换成 cursor。
因此在第二列横向拖动时，起止位置都被第一列的 `xToCursor()` 夹到第一列行尾，
选区为空。混合普通文字与 inline code 仍在同一 `QTextLayout` 内，根因不是字体颜色或代码格式。

`TextRun` 没有保存排版时的实际可用宽度；链接命中也将每个 run 当成整篇宽度，
同样无法准确表达并排的 cell。

## RED 与验证边界

新增 `MarkdownLayoutTest::tableSelectionReachesEveryColumn`，两种数据分别为普通单元格、
普通文字与行内代码混排。测试先证明输入已解析为预期可见文字，再检查每格首尾位置能由指针命中，
随后断言正反选区文本完全相同。

主代理已统一构建并执行 RED：普通单元格和混合行内代码两种数据均在第二列指针边界断言失败，
`Totals: 2 passed, 2 failed`，301ms。本地日志为
`.cache/desktop-player-style-20261105/table-column-red-cdb.log`。

修复为记录每个 run 的真实排版宽度，并按二维内容矩形寻找最近的 run；
链接命中和可见区域判断也使用该 run 宽度，避免把每列当成整篇宽度。
`MarkdownLabelTest` 与 `MarkdownViewTest` 另覆盖实际鼠标正反拖选和 Copy 快捷键，
按预期文字核对剪贴板，不通过手动全选绕过指针命中路径。
主代理执行 Layout 回归后，两种 cell 数据均通过。最初 QWidget 拖选测试仍报空选区，
另查本机 Qt 5 `qtestmouse.h`：`mouseMove(QWidget*)` 只移动系统光标，而 press/release
直接向给定控件注入事件；其他窗口在前台时，三段事件可能不走同一目标。
这不是降低选区断言的理由，测试改为全程 `QWindow*` 的 QPA 事件路径，
按真实窗口/控件坐标转换，仍断言精确文字与 Copy 剪贴板结果。

主代理随后执行两套真实控件回归均 exit 0：
`.cache/desktop-player-style-20261105/table-label-qpa-green-cdb.log`、
`.cache/desktop-player-style-20261105/table-view-qpa-green-cdb.log`。
这覆盖第二列普通/inline-code 混排及正反拖选；正常产品与各平台实际画面验收仍由主代理单独记录。
