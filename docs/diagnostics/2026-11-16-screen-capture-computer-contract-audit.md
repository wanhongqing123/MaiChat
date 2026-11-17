# screen_share → screenshot → desktop_computer 只读合同核对

2026-11-16，Mac 侧只读审计；本地 HEAD 为 `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`。Windows 协作者负责实际运行构建、用户已授权的相关历史记录及自有原生夹具。本记录没有运行模型、原生输入、截图、构建、设备或服务操作，没有修改生产代码或测试。

## 操作与预期

按实际工具调用顺序还原：开启用户所说的屏幕共享、取得截图、依据该图选择目标、发出鼠标或键盘操作。预期是截图来源与动作目标一致，坐标只转换一次，焦点符合动作要求，派发后的新图与原动作对应。失败必须区分选点、目标、焦点、原生落点与应用处理结果。

本地桌面工具注册源码没有字面名称 `screen_share`；存在移动端 `mobile_start_screen_share`，以及桌面 TRTC 分享实现和本机采集状态条，不能推定它们是同一入口。已请求 Windows 侧仅提供真实首个工具的 name/action/target 元数据。该项未确认前不建立“共享目标就是截图目标”的假设。

## 已核实的来源与坐标合同

| 入口 | 图像/观察范围 | 动作关联边界 |
| --- | --- | --- |
| `screenshot` | 固定 `DisplayUnderPointer`，原生捕获时用 `GetCursorPos` 选择显示器；只支持 capture/stop | 回执包含 PNG 尺寸及原生帧序号/时间，没有 `display_id` 或 computer `snapshot_id` |
| `desktop_computer.observe(target=display)` | 按显式原生 `display_id` 捕获整屏，保存实际 PNG、native bounds、帧元数据 | token 绑定 Session、显示器、工作区、几何、输入代际；返回 `x_ratio/y_ratio` 所依据的整屏比例空间 |
| `desktop_computer.observe(target=window)` | 模型入口显式 `includeImage=false`，返回窗口元数据/UIA | 使用窗口范围 token 和窗口相对比例；说明中另行建议调用 `screenshot`，因此不能把整屏比例直接当窗口比例 |
| `browser.screenshot` | CDP `Page.captureScreenshot`，`captureBeyondViewport=false`；当前可见页面 viewport，校验 expected URL | browser 的 snapshot/element 身份属于独立协议，不是 computer token；浏览器内容原点、窗口边框与整屏原点不能互换 |

源码位置：`MaiAgent/AgentTools/MaiScreenshotTool.cpp:29,155,193`；`MaiAgent/AgentTools/MaiScreenshotWindows.cpp:715,727`；`MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp:315,370,475,573,613,711,876`；`MaiChat/desktop/src/MaiChatTools/DesktopBrowserTools.cpp:492`。

computer 将比例映射为 `origin + ratio * max(1, extent - 1)`。Windows 的显示器、UIA/DWM 窗口边界与 `SetCursorPos` 都使用 native 坐标，工作线程采用 Per-Monitor V2，落点只取整一次。若模型依据图片像素取点，应先按**实际模型所见图片**的范围归一化；从整屏改用窗口时还须扣除窗口原点。混合 DPR、负屏幕原点或移动窗口不能再额外乘一次缩放。

有效 computer token 只能验证宿主保存的观察，不知道模型实际依据的是哪张别的工具图片。因此“另一张 screenshot + 仍有效的 computer token”不能仅靠现有 token 门检出图源混用。这是已确认的合同边界，尚不是本次现场根因。

## 焦点、时间和图片回灌

- display 输入有意允许切换应用；`type/key` 使用当时前台，`move` 本身不激活窗口。window 输入才执行 `activateForInput` 并核对实际前台/选定窗口所属关系；同 PID 的 owned popup 可按原规则参与，普通同级窗口不能冒充目标。OS 拒绝激活会返回失败。相关位置：`DesktopComputerTools.cpp:580,680`；`DesktopComputerWindows.cpp:240,691`。
- screenshot 和 computer 非 clear 操作都遵循 Agent policy 的逐次审批要求。需记录现场是否真的出现审批，以及前后前台 HWND；现有截图后端未见 `SetForegroundWindow/SetFocus`。不能从“捕获到了目标”推出“键盘焦点仍在目标”，也不能未经实测归咎于审批 UI。
- token 每次尝试输入后必须重新观察；display 成功派发后要求更晚的原生帧。`dispatched_observation_failed` 表示输入已派发但后续取图失败，应重新观察，不能据此重复输入。新帧同样不等于目标应用已处理成功。
- 图片超过 2 MiB 时，OpenAI Chat/Responses 客户端可用嵌入 FFmpeg 转为最长边 1280 的等比 JPEG；原工具 JSON 的尺寸仍描述原图。正确比例不会因等比预览改变，拿预览像素除原图尺寸会改变比例。位置：`MaiOpenAiClient.cpp:201`、`MaiResponsesClient.cpp:128`、`MaiFfmpegTools.cpp:493`。原已归档 19 条拖拽记录的关联 PNG 均为 2560×1600、约 0.6–1.7 MB，没有命中这项本地转换。
- ContextBuilder 按工具组追加图片，明确标记 `ownership=tool_group_only`，没有逐图调用归属；不能按文件名或批内位置臆认对应调用。应比对真实输出路径/图片哈希和消息、part、call 关联。位置：`MaiAgent/Agent/MaiContextBuilder.cpp:35,62,365`。

## 最小联合矩阵（建议，未在本轮执行）

| 槽位 | 自有夹具操作 | 需要同时观察的判据 |
| --- | --- | --- |
| 1. 坐标基线 | 同一显式显示器，150% 缩放，两个窗口位置；点击/拖拽自有标记中心 | 实际 PNG 尺寸、比例、native bounds、原生消息自身坐标经 ClientToScreen 的落点；不用稍后读取的当前鼠标位置替代事件坐标 |
| 2. 图源负对照 | observe A 屏后，鼠标移到 B 屏执行普通 screenshot；单屏可先用合同夹具构造不同图源 | 图片与目标独立记录，识别错图而非把合法 token 当作图像一致性证明；再用显式 observe A 作正对照 |
| 3. 范围转换 | 同一自有窗口/页面，分别取整屏、窗口元数据和浏览器 viewport | 原点/内容区/边框/DPR 换算后落同一点；直接复用另一范围比例的负对照应产生可见误差；窗口移动/缩放后旧 token 应被拒绝 |
| 4. 焦点交接 | 按真实策略经过审批/共享条/聊天界面，再 move→type；另测窗口激活失败 | 实际前台 HWND/PID、原生 focus、输入接收者；display 与 window 两种合同分别断言，不用测试 helper 强制激活掩盖失败 |
| 5. 连续动作与失败 | 每次输入检查新 token/原生帧；故意使后续观察失败或取消 | 已派发回执保留、不重复打入；按键/鼠标释放完成；应用实际结果与“派发成功”分开验 |
| 6. 模型看到的图片 | 单调用与同批多图，原图在 2 MiB 两侧各一张自有素材 | 工具 call/part、原始 PNG、最终出站图片哈希与解码尺寸、实际动作比例关联；脚本模型通过不能代替真实模型选点证据 |

## 现有测试能证明什么

`DesktopComputerContractsTest.cpp` 已包含负原点、显示器身份/几何、帧新鲜度、一次性 token、跨会话输入代际、取消、密码域、owned popup、失败回执与 metadata-only 等合同夹具。

`DesktopComputerAgentIntegrationTest.cpp:205,445,827,914,1081` 使用 `ComputerScriptModel/DisplayComputerScriptModel` 并设置 `MaiApprovalPolicy::Never`；display 例的后端也是 fake。它核对真实 Agent 调度及返回 PNG 对应关系，但没有真正供应商视觉选点或审批弹窗焦点过程。

`DesktopComputerNativeIntegrationTest.cpp` 使用自有独立编辑器进程；`DesktopComputerDisplayNativeTest.cpp` 使用自有全屏编辑器和原生读回。`DesktopComputerBrowserNativeFixture.h:731,786` 使用私有浏览器 profile、127.0.0.1 页面、Ctrl+L、UIA/URL/cookie 读回，明确 `visual_model=false`。浏览器成功不代表真实模型按截图选点成功，也没有覆盖独立 screenshot 与 computer 的混合入口。

`DesktopScreenCaptureBar` 是当前进程共享采集状态/停止按钮，不是图片 target 或输入焦点回执；其测试不覆盖上述联合链路。

## 验证边界

本轮结论仅为当前本地源码的合同核对与测试缺口。未读取用户历史日志、数据库或凭据，未操作 Windows/Mac 原生桌面、设备、账号、现装 App 或服务，未运行构建或测试。Windows 侧实际构建/历史调用来源应另核源码指纹；没有据此宣布坐标漂移或第三方页面问题已修复。Sync R2 部署和 Windows 播放器替换仍不属于此次只读审计授权。

## 后续收到的五件 Windows 增量静态审阅

**该候选已被发送方明确作废。** 后续原生用例又出现工具返回 Busy、GUI 处理后实际前台已到目标的失败。Windows 侧现改为一次 SetForegroundWindow 后轮询真实 GetForegroundWindow，250 ms/5 ms，并在期间和结束核对 PID/visible/iconic；没有 WM_NULL/AttachThreadInput。本侧尚未收到并独立审阅该轮询增量，下面 e074a5f0 的结论仅是旧候选的历史记录，不得用于新源码验收。发送方也将原夹具升级为 MouseWitness，混域偏差 64 px 来自 WM_LBUTTONDOWN 自身经 ClientToScreen 的坐标；不是下面旧版稍后读取的光标采样。

输入：`1794760121-84116755.html` 与独立 `1794760121-53784300.cpp`。仅在新私有目录重建 postimage，未应用到仓库。两个生产文件与原合同测试的 preimage 均与本地精确匹配；CMake 的 preimage 不同，仅审 Windows 条件内新增的五行目标配置，没有强行套用。

| 文件 | 核实的 postimage Git blob |
| --- | --- |
| DesktopComputerTools.cpp | `3901856966a8015826e854e1c7f836fa9a52c29d` |
| DesktopComputerWindows.cpp | `e074a5f06b8b9013a35b9d1e895c2bbcdda97343` |
| DesktopComputerContractsTest.cpp | `08dc064fe5fd5c4c0cbbddd1a656626fe61f9ef3` |
| DesktopComputerScreenshotChainNativeTest.cpp | `52471647fe50be982fd1e15a0a21b3d6d1d1bd57` |

独立 CPP 原文件 18932 B、SHA256 `97cfa4d6ed024675335198f2eac5a71f5a80359cd1f4b029112d8bd5ba42db76`，CRLF 归一为 LF 后与 HTML 的 389 行新增文件逐字相等。

静态结论：没有发现具体生产 P1/P2。Windows 改动在 `SetForegroundWindow` 后增加跨线程 `WM_NULL` 消息往返，等待 250 ms 失败就返回 Busy，随后重新核对进程、可见性、最小化和真实前台；没有修改坐标转换、附加输入队列或强制取得前台权限。这与微软对跨输入队列异步激活的[官方说明](https://devblogs.microsoft.com/oldnewthing/20161118-00/?p=94745/)一致。[SendMessageTimeout 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw)说明同队列调用会忽略 timeout，因此有界结论限定当前未附加输入队列的普通 worker 路径，不扩展为任意共享队列环境下的保证。

新原生夹具具有明确的整屏比例误用于较矮窗口的负对照，以及转换后命中、文本 UIA 读回、一次性 token、focus 后重新观察、移动后旧 token 拒绝、整屏新帧、常驻采集 worker 和停止状态检查。也覆盖自有 GUI 暂停处理时的激活失败。

验收边界保持：夹具直接调用工具 `execute`，不经过真实模型或 Agent 审批；capture control 读取的是采集状态，不代表实际执行了名为 screen_share 的工具。`missPixelsY` 来自稍后读取的当前光标，可配合未聚焦证明此例打偏，不等于原生按下事件自身的精确坐标。没有在 Mac 执行这组 Windows 测试。

已回传一条非阻塞说明建议：共享工具说明应显式给出 `nativeX = displayX + imageX * (displayWidth-1)/(imageWidth-1)`（Y 同理），再扣窗口原点、除窗口 extent-1。这样涵盖 Mac native 指针范围与 PNG 像素不同，以及模型预览缩小的情况；不能将图片像素默认为 native 指针坐标。本次 Windows 等尺寸素材的换算不受此澄清影响。
