# v2.2.47 桌面现场反馈修正

## 身份与范围

- 日期：2026-11-05，Asia/Shanghai。基线为 main `2c96439d97433b57ab715aa3562e4db8df4908dd`。
- 用户明确截图来自另一台 Windows，运行 v2.2.47。本机 D:/Program Files 的旧进程不能用来解释截图。
- 保留本机正在运行的真实 App 和任务；不读取真实会话正文或凭据，不为界面验收发模型请求。
- 分别验证源码、目标运行构建、发布产物。Mac/iOS 没有构建连接时明确记录未验证。

## 现场步骤与预期

| 反馈 | 操作与预期 |
| --- | --- |
| 窗口标题栏 | 打开消息、创作中心、云盘、设置并切换/滚动；最小化、最大化、关闭固定在完整高度的顶部标题栏，业务动作在下一行。原生拖动、缩放、Snap、最大化工作区与全屏继续正确。 |
| 播放器 | 打开本地视频，拖四边/四角并暂停切速、重播；窗口可自由改变长宽且保持手工几何，画面等比完整呈现，顶部/底部为视频上的渐变浮层。音量使用紧凑竖向滑块和百分比。 |
| Agent 调用限制 | 对照指定 Codex 源码：区分模型进度指令与宿主强制计数。用户已确认中间文字可以显示，本轮不增加每 N 工具强制发言或额外模型请求。 |
| 忙时输入 | 当前工具任务执行中连续输入/发送 B、C；顺序受理、稳定请求身份、没有自动取消当前工具，Stop 单独终止当前任务；终结期间未受理的草稿/附件不丢失，不误发到新轮次。 |
| IM 气泡与输入 | 发送超过 50 字但短于上限的普通文字，气泡按实际正文/内联时间收紧；空输入框没有提示文字和内部滚动条，键盘编辑/滚动继续可用。 |
| Markdown | 同一表格第一列和第二列均可鼠标选取复制，包括行内代码；每条 Agent 消息的引用动作位于所属消息最后一个可见内容之后。检查含汉字及框线字符的等宽代码块。 |
| 历史加载 | 大历史会话切换和扫码登录时不重复全量读取、解析和布局；参考现有 iOS 的最新页、增量补页与滚动锚点策略，以合成大历史验证。 |

## 已确认的 RED

- 标题栏：`windowsCaptionStaysInTitleBarAbovePageActions` 失败，控制区不属于顶部栏矩形。
- 播放器：`windowedVideoFillsViewportAndKeepsUserSize` 失败，实际 viewport 为
  `(0,46,640,312)`，窗口为 `(0,0,640,442)`；固定扣掉 130px 挤压画面。
- 四边缩放：真实事件接收者测试得到 `(0,46,680,300)`，窗口为 `(0,0,680,430)`，同样保留了错误栏位。
- 详细日志在 `.cache/desktop-player-style-20261105/`；后续模块验证和发布结果另补实际执行记录。

## 追加定位与修复（2026-11-05 至 2026-11-06）

- 顶部标题栏为 36 logical px；关闭/最大化/最小化属于该栏，业务标题与动作在下一行。实际 DPI 下验证 frame/client 命中、页面切换与全屏恢复。
- Windowed 视频此前为 `(0,46,640,312)`，固定扣掉 130px；现 surface 覆盖完整 client，标题/控制为真实合成渐变 HUD。首帧初始化比例只执行一次，用户拉伸/移动不被切速或重播覆盖。竖向音量 popup 的百分比、静音、键盘和 owner 生命周期独立回归。
- IM 启动只恢复各会话一条预览，打开后读取 40 条，补页 40 条。删除联系人自动回落、SDK 首选联系人和首条漫游选中也 hydrate。SQLite 使用 `(peer, created_at, id)` 覆盖索引，先枚举 peer key，再 LIMIT 读取 payload，移除全表宽行窗口函数。
- 分页游标来自连续 DB 页，单独收到很早的漫游记录不推进它。85/450 条合成历史测试证明中间消息可达。
- 同一时间戳的 DB 页按 id 保持稳定顺序；preview 先加载不会把最后一条倒放到最前。DB 新恢复记录与既有 live 记录分开排序，类别参与比较以保持传递性；首次 hydrate 前后的 live 同毫秒到达顺序均保留。updateTime/adopt/remove 同步维护索引。
- `>=50` 字普通发信原本被强制固定到最大宽度，现按正文与尾注实测收紧。输入框去掉 placeholder 和内部 scrollbar，保留编辑/滚动与无障碍名称。
- 表格第二列原本被同一行第一列的 y-only hit 捕获；现使用二维文字矩形。Qt 真实 QWindow/QPA 拖选、反向拖选及 Ctrl+C 覆盖普通/混合行内代码。
- 三端共用未修改的 Sarasa Fixed SC Regular，ASCII 与框线半宽、CJK 全宽；源图示原有一列空格差异不偷偷重写。Qt/Android fenced code 使用局部横滚，正文继续换行。
- Label 的旧 paint/probe 曾在 source/theme/note 更新后用新 key 归还，导致旧布局冒充新内容。现先按旧身份归还再修改输入。原失败 naturalWidth 全断言保留。
- View 前一项流式增长会让后项选区全局 offset 串线（实测 selected 变 ed conte）；现按稳定 item ID+局部 offset 保存/恢复，包含 batch/upsert，不重建嵌入控件。
- Core 新增 session+message ID 精读；目标 Stop 在 Runtime 锁内校验原 turn 并取消/唤醒审批及提问。specialist 预占后初始化 id 的 writer 使用 steeringMutex，比较端沿 Runtime→steering 同序同步；Windows 未运行 TSAN。
- 手机 send/steer 回执明确区分 user_message_id 与原 turnId，普通 send 的旧 id 语义保留。两端持久 FIFO 不把未知结果或旧轮次自动转换成新请求。Native 精读 op 复用同一 Core 实现，不用伪造 `id+"~"` 游标。

## 已执行证据

- Core 最后一次 8/8 PASS：`.cache/maiagent-core-gate/final-tests.log`，125.33 秒。此前 739 步 clean 重编公开 ABI 消费者；后续仅实现/测试更新均重编并重链八个程序。源文件/程序 SHA 在 `final-provenance.json` 绑定到发布门禁。
- Windows own CXX 的 86 个规则 clean 后全目标 391 步 PASS；审查后的局部更新再次全目标 64 步 PASS。每次启动 GUI 前 maichat 完成 windeployqt 与 95 PE gate。
- Local busy/Stop/quote、Remote FIFO/owner/unknown、最新页/懒详情、标题栏、气泡、播放器八边/HUD/音量及同毫秒/漫游游标均有定点 RED→GREEN；全套 Windows 发布矩阵及产物/ZIP smoke 的最终归属以本次 release-manifest.json 为准。
- Android 正常 application ID 的全量 Gradle JVM：442/442、0 skipped；`.cache/android-agent-steering-tests-20261105/android-final-normal-unit.log`。isolated 变体的一项 SQLite shadow 用例失败单独保留，不能用该变体宣称全量 JVM 通过。
- OPPO PKD130 Android 15 arm64 原生 C ABI：`native-final-device.log` PASS；含 11 条连续输入、调用/结果相邻、明确原轮回执、精读会话隔离、8/20 分页。只在 `/data/local/tmp` 运行合成任务，不启动供应商请求。
- 实际 Windows 截图/录像：`.cache/desktop-player-style-20261105/final-overlay-volume/`；IM/Agent 原图示、标题栏与忙时输入见 `.cache/window-chrome-20261105/`。均为隔离 fixture，没有停止/替换原安装的 App。
- Android 真实字体/NoWrap/局部滚动/Copy 仪器测试使用独立 `.isolated` debug 包；安装与结果单独记录。该包仅作 native 组件验收，不冒充已安装正常 App 的全流程验收。
- macOS/iOS 无构建宿主，本轮为共享源码/资源与静态接线检查，未宣称编译或实机验收。Windows release 只包含 Windows 资产。

### Android 原生组件最终结果

- 独立 `.isolated` 包在 OPPO PKD130 Android 15 执行 `MarkdownCodeFontInstrumentedTest` 和 `MarkdownViewportInstrumentedTest`：**3/3 PASS**，91.731 秒，日志 `markdown-native-instrumentation.log`。
- 覆盖真实字形 advance、窄代码一行不软折行、正文仍换行、滚到尾部、完整空白/TAIL 原生 Copy、流式 append 保持 child/offset/选区，以及 pending 布局不覆盖后到的新 scroll（80→180）。
- 桌面实际图示截图已分别查看 Agent/IM；首版视觉探针取到了隐藏远程 View，已改按实际 AgentChatPanel 归属取 View 后重新截图。此为测试探针错误，未据此修改产品导航。
- 真实安装包 `com.kongshang.maichat` 未被替换，独立包不初始化 Agent 或账号；原生 Agent 合成回归在私有 temp 路径单独执行。
