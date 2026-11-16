# Windows 截图与电脑操作的联合核验

## 用户操作与预期

用户要求在 Android 重复答案/图片提示修复后，检查桌面 MaiChat 的实际记录，复测
屏幕分享/截图 → 根据图片选点 → `desktop_computer` 的焦点、点击和拖动链路。
预期：图像范围与输入坐标一致；焦点请求只在真实完成后报告成功；旧快照不产生重复输入；
中文、emoji 和多行文本不丢失或重排。Android 285 的独立验收已记在前一份诊断。

当前用户 Windows App 为 `D:\Program Files\MaiChat\maichat.exe`，PID 56404 / 2.2.65，
本次未停止或覆盖。检查以普通 Windows QPA、144 DPI / 150% 缩放、2560×1600 显示器进行。

## 实际调用记录

按用户明确授权，只读精确对应当前手机会话的 Agent SQLite（mode=ro + query_only）。
16 个手机消息 ID 摘要全部匹配该桌面会话；仅输出工具名、动作、坐标、原生 bounds、
匿名 call/token 标识、状态和错误类别，不输出正文、凭据、URL 或真实消息 ID。

该会话保存了 764 次 `desktop_computer`、12 次 `screenshot`。与用户 22 时照片对应的
2026-11-15 14:00Z 后有 41 次：电脑操作 20 completed / 7 error / 2 canceled，截图
11 completed / 1 canceled。没有独立名为 `screen_share` 的模型工具调用。
桌面 `screenshot.capture` 获取原生常驻采集帧；采集状态/停止 UI 桥接与 TRTC 分享独立，
本轮不将它们混称一个工具，也未连接 TRTC 通话或第三方网站执行验证。

两个确定的调用问题：

- `screenshot` 返回整屏 2560×1600，而随后的 window observe 返回原生窗口 2560×1528，
  `has_image=false`。模型将整屏图中的比例用于窗口输入，导致纵向点位偏上。
- 22:09:31 click 后，22:09:41 type 继续用了已消费的相同 window snapshot；中间再截屏
  不会创建或更新 computer token。此处约 10 秒即被拒绝，不能只解释成 5 分钟自然到期。

这些记录在私有 `actual-sanitized-tool-trace.json`。原始调用中一个 foreground 错误缺少
当时后续 HWND 事件，不能逆推它必然是下述异步误判，或排除真实 Windows 权限拒绝。

## 原生联合用例与焦点缺陷

新增 opt-in `DesktopComputerScreenshotChainNativeTest`，使用真实 screenshot 工具、注册的
computer 工具、普通 DesktopScreenCaptureControl 和原生后端。自有全屏背景覆盖显示器，
自有输入窗口原生高度精确为 1528；外部像素/前台或用户按键干扰会拒绝继续输入。
没有模型 API、账号、真实浏览器 profile 或第三方页面参与。

负对照把整屏比例直接送到窗口：目标 y=1424，实际 WM_LBUTTONDOWN 的 ClientToScreen
坐标 y=1360，稳定偏 64 物理像素且输入框没有焦点。正确转换或绑定 display 图/token 后，
输入框得到焦点，原生文字输入与 UIA 完整读回一致。该数值取事件自身，不以稍后光标代替。

同时复现真正的生产焦点错误：worker 调用 SetForegroundWindow 后立即检查前台返回 Busy，
但目标 GUI 处理消息后（50ms）已成为真实前台。微软说明跨输入队列激活可以异步完成。
[Microsoft 的异步激活说明](https://devblogs.microsoft.com/oldnewthing/20161118-00/?p=94745)
与本例原生观察一致。

最终修复只提交一次正常 SetForegroundWindow，在普通跨线程 worker 上最多等待 250ms、
每 5ms 读取真正前台，期间及之后重核 PID/可见/非最小化；同线程保持原同步判断。
拒绝聚焦仍返回 Busy，不附加输入队列、不发送 Alt、不重复强取前台。底层 Windows API
自身的调度语义仍适用，250ms 指本实现增加的等待上限。

早期 WM_NULL 往返候选曾首轮通过，后续仍复现“返回 Busy 时实际前台随即已正确”，
因此已作废，不能把旧 review 当最终方案。当前轮询同例通过，自有 GUI 暂停 400ms 时
约 254ms 返回失败且没有文字输入；晚到激活不允许拿旧 token 继续输入。

工具说明同步明确整屏/窗口范围、u/v 到原生坐标的换算、缩小预览不能套原图像素尺寸，
以及 screenshot 不刷新 computer token、focus 也消费 window token。未改原生坐标数学
或放宽 PID/前台/安全输入检查。

## Unicode 输入中独立发现的 Qt 缺陷

原生 WM_CHAR UTF16 原序与输入完全一致，而控件把 emoji 插在前面的普通字符之前。
锁定 Qt 6.12.0 / qtbase `025bdad181de241e81bf853c8a2d7bf3d19261a9` 中，完整代理对
同步发送 QInputMethodEvent，普通 BMP 则排入 WSI；这是两种派发顺序混用。
仅针对 VK_PACKET 的完整 pair 改用同文件已有 sendExtendedPressRelease，同 WSI 队列
传完整 QString，保留非 packet 的 IME 分支及高代理等待/reset。原同例 RED→GREEN。

组合矩阵又发现 retained VK_PACKET record 可把不同的下一汉字重播成上一字：
`原生中文、标点 A` 变成 `原生中文、、点 A`，原生字符仍正确。只在 packet 新旧文本不同
时清理旧 record，保留普通物理键和同文重复行为，原同例已通过。

多行测试另确认 LF 已到达，但 TextEdit 的 text-key 可接受性将它过滤。Windows overlay
为多行 TextEdit 接受文字 LF，单行仍拒绝；不映射成物理 Enter，避免触发表单发送。

补丁保存在 `qt/patches/windows-packet-surrogate-order.patch`，SHA256
`a66f64b74b19c5b10e2a0f6892451bd8617d05be18429c401174d8ecb97e67de`，锁两个源文件的
before/after Git blob。上游 submodule commit 不变，未将本地修改伪称上游新版本。
bootstrap 校验 patch 摘要、精确 pre/postimage、幂等性，SDK fingerprint 记录 patch 及
tracked/untracked 源内容摘要，避免 dirty SDK 在恢复源码后冒充 clean SDK。

独立 review 的真实 Windows checkout 补出构建 P2：core.autocrlf=true 会改写 patch 字节。
已加 patches/.gitattributes 固定 LF，并有私有真实 Git 重检出用例。原 1/4 失败→4/0；
校验保持同一个 LF 摘要，未靠改变哈希或放过不匹配修复。

## 实际验证结果与边界

- 最终 8 组原生样本（ASCII、BMP、emoji 首/中/尾、CJK、ZWJ、连续 pair）均经过完整
  截图/坐标/焦点/单次 token/移动失效/文字替换/UIA读回/超时/采集停止链，Qt 10/0/0
  （含 init/cleanup）。原生字符和控件文本均一致，每例记录实际加载的平台与 Gui DLL 摘要。
- 最终实际加载 qwindows.dll SHA256
  `28fc68b25b5efce19c813844f5597ada72c3604d8735156cf7422863c15ff264`；Qt6Gui.dll
  `359f8042598672569c4ae09b24765915643b1a242b4722554f8aa9487943c89e`。
- 普通 computer contract 全组 exit 0；Qt source overlay 私有 Git 验证 4/0。
- 独立 Chrome 的新私有 profile、仅 loopback 两页地址栏导航/页面标记/cookie/URL读回通过，
  同常驻采集 worker/session；visual_model=false，不是重跑付费大模型选点。
- 两进程文字/指针用例 `--text-pointer`：中文、emoji、LF、全选替换、清空、Enter、
  双击、右键、拖选及完整 UIA 读回通过。自有测试接收器原来每字符/selection 同步写全份
  键盘日志造成 GUI 拥堵，改由原 100ms heartbeat 发布真实状态，不减少业务断言。
- 完整原始物理键目录在当前 CJK IME 下有吞数字/Quote/小键盘键的边界，保留失败记录，
  默认工作流和断言未删除。它未被当作本轮已通过项；文字/指针场景使用新增显式入口分开验证。
- 播放器原 7 selectors Qt 9/0，新 Windows HWND 几何 4/0，使用更新 SDK 回归。

本轮覆盖实际记录与原生工具直接调用，尚不等于真实 VLM 选点、Agent 审批弹窗、多屏或
超 2MiB 图片出站预览的全部验收。实际输入全部限自有窗口/私有浏览器；正常用户 App 和
Chrome profile 保持不变，没有服务重启。源码提交、完整候选包、DLL/EXE 以及裸 PATH
staging/ZIP smoke 回执在冻结后补记，运行中 App 的替换仍待此前已询问的用户确认。

## 冻结候选与交付状态

源码提交 `40d92ed15c9ac37fe1a4827f88b8e92a59d80027`（包含前一轮播放器修复），正常
Windows 目标构建及运行依赖门通过。候选继续沿用 2.2.65，仅为私有修复验证包，未覆盖
公共 v2.2.65 Release，也未替换当前用户进程。

- ZIP `MaiChat-win64-v2.2.65-20261116-40d92ed1.zip`，242131535 bytes，SHA256
  `33cd47edc380748f2c3b1f75ba8be85186a771e081643054b9e09cf288778b55`。
- EXE 166899200 bytes，SHA256
  `659a5504a393970a048b73b4dea68ba1c2d83a674e6fb0184859912e94e24938`。
- staging 与实际 ZIP 解压目录 81 文件逐字节核等，EXE 与本次正常 build/bin 相同；
  qwindows/Qt6Gui 与受控 SDK、最终原生测试实际加载文件摘要一致。
- 两份目录均通过 3 binaries / 114 PE imports 门，裸 PATH / 隔离账号数据 `--smoke`
  各 exit 0，实际新进程 PID 50704、58160。原用户进程 PID 56404 未停止或覆盖。

候选生成时核验 Qt 两个 postimage 后，仅恢复本任务修改的两个受控源文件至锁定上游，
没有修改 submodule commit 或清除其他工作。今后 bootstrap 会先依据 tracked overlay
重新应用，再计算 source fingerprint；不靠恢复 clean 状态复用无法追溯的 dirty SDK。
当前仓库只剩任务开始前原有三路径 WIP，摘要保持，未混入交付。

完整证据和候选 receipt 位于 `.cache/windows-computer-chain-20261116/`。
Android 2.2.43 / 285 的两项 UI 问题已实际更新并验证；Windows 更新和 Sync 服务 R2
部署仍是各自独立、此前已询问但尚未收到明确答复的授权，不能由本轮测试指令推定同意。

## 2026-11-16：补全模型直接读取的坐标说明

用户要求把坐标相关使用说明写清楚，避免模型混用整屏截图与窗口坐标。本次在
`bb3903b7` 基础上修改 `desktop_computer` 的 description、参数 description，以及
独立 `screenshot` 的 description；没有修改输入、快照校验或截图行为。完整说明及
调用示例见 [坐标与截图使用说明](../development/desktop-computer-coordinates.md)。

预期调用是 `list_displays → observe(target=display) → 根据绑定图片计算比例 → 输入
→ 检查返回图片并使用 observation.snapshot_id`。说明同时覆盖窗口 UIA 比例、整屏到
窗口的原生 bounds 换算、缩略图与原图尺寸、负原点、多屏、DPI、单次快照，以及截图
不代表键盘焦点等边界。窗口模式保留，默认值及参数约束不变。

Windows 普通缓存增量构建 exit 0；当前源码链接后的八套 Core 回归、
`desktop_computer_contracts_test` 和 `MaiScreenshotToolTests --request-validation-only`
十个执行入口全部 exit 0。Screenshot 既有测试直接用普通 Core 目标的编译参数、当前
`maiagent` 库和同一传递依赖编译链接，没有新增产品构建入口。静态核验剥离 description
后 schema 约束完全相同，两份 C++ 文件除模型描述方法外完全相同，四个 JSON 示例可解析，
原有三路径 WIP SHA256 保持。

证据位于 `.cache/desktop-coordinate-help-20261116/` 的 `build.log`、`build-final.log`、
`test-results.json`、`static-checks.json` 和各执行日志。本次未运行付费模型、未重跑原生
输入或实际屏幕采集，也未重新包装、替换或重启用户 App。新说明需要后续客户端构建及
更新后才会进入用户当前运行版本；不将本次说明检查当作新的模型选点实机验收。
