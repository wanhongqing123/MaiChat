# 常驻屏幕采集与 screen-only 工具排障记录（阶段 B）

## 用户需求与可复现操作

时间：2026-11-07，Asia/Shanghai（UTC+08:00）。本阶段用户要求常驻采集线程，通过 Command 取得当前屏幕图像，模型侧只提供屏幕截图，移除窗口截图工具。阶段 A 已处理同一截图文件可引用、可交付的问题，见 [阶段 A 记录](maichat-native-screenshot-handoff-20261107.md)。本记录单独保存阶段 B 的实现和验证边界。

预期操作及结果：

1. 创作中心模型调用 `screenshot {}` 或 `{"action":"capture"}`，第一次启动原生采集 worker/session。
2. 连续请求同一屏幕时复用 worker、device 与可复用的 session；采集持续接收完整帧，Command 才编码 PNG。静止画面可以返回当前最近完整帧，但不能伪造帧序号或接收时间；变化、失效或切换目标后不能返回旧目标帧。
3. 模型使用返回的工作区相对 `image_path` 调用 `agent_send_media`，展示同一文件。
4. `screenshot {"action":"stop"}` 释放原生采集会话并 join worker；重复停止无副作用，随后捕获可以重新启动。
5. 旧 `mode`、`windowTitle` 等参数必须明确拒绝，不能默默改为采集显示器。模型工具表不再注册 `list_windows`；`desktop_computer.observe` 只返回窗口和 UIA 元数据。

这里的 screen-only 是模型工具边界。普通原生 WindowById/WindowByTitle API 保留给兼容调用和受控测试，不再构成模型可选的窗口截图入口。

## 源码、构建与运行身份

| 对象 | 当前记录 |
| --- | --- |
| 阶段 B 源码 | 从 `ecdcc521d390e3e36860138ffa26589fcae30e47` 开始，统一提交并推送到 `main` 的 `85592c748fac52546ed91af9fae241a3d4d77b00`；双方工作区已清理并核对相同输入 |
| Core 构建 | `E:/OpenSource/MaiChat/.cache/maiagent-core-gate/build-followup-20261105`，VS2022 / MSVC 14.44 |
| Windows Desktop 构建 | `E:/OpenSource/MaiChat/MaiChat/desktop/.cache/build-release-62111144`，Windows x64 / Qt 5.15.2 |
| 本地证据目录 | `.cache/resident-screen-20261107/`；下文日志名均相对此目录 |
| Windows 当前构建程序 | `MaiChat/desktop/.cache/build-release-62111144/bin/maichat.exe`，新增 `MaiTool` virtual 后实际重编，主任务核对 SHA256 `f3b4e4f5fdaa8a784456d73dca049e18aea861683039722db2c9c31d624a0c1b` |
| Windows 较早构建快照 | 同路径此前 SHA256 `048bdbf9a588595c76edbc2e825cac9e78566cf401df2df8039491683ed3184c`；只对应新增 virtual 前的快照，不再代表当前 exe |
| 依赖检查 | `desktop-workspace-final-build.txt` 记录新增 header 后相关对象重编和 `MaiChat runtime verified: 3 binaries, 120 PE imports and Qt plugins`；早期记录另存于 `desktop-first-final-build.txt` |
| 安装与发布 | 本轮没有安装、替换或发布；最终私有 ZIP 与 staging/解压目录中的 exe 哈希均等于上述当前构建哈希，详见交付段落 |

主任务在阶段开始时没有主动停止或替换 Windows 现装 PID 584。较新的只读进程查询已经没有 `maichat.exe`，原因尚未调查，本文不写成“PID 584 仍在运行”，也不凭此推定崩溃原因。Mac 现装 PID 96951 按同伴反馈未改动，未以 Windows 本地观测代替 Mac 进程核对。

## 架构与模型工具行为

Windows 常驻后端在专属线程拥有 MTA、D3D device、WGC frame pool/session、最近完整帧、staging 与 WIC 编码资源。更新到达时持续消费当前完整帧；只有 Command 请求才生成 PNG，避免每次调用重新初始化全部原生资源。

返回的 `frame_sequence` 与 `frame_received_at_ms` 来自真实接收帧。静态内容可沿用同一完整帧及其元数据，不能把本次 PNG 编码时刻冒充帧更新时间。目标变化、resize、关闭、不可见、最小化、环境失效及停止均需要明确处理，不能靠缓存掩盖失效。

工具契约：

- `screenshot` Schema 只有 `action`，枚举 `capture` / `stop`，默认 `capture`。
- capture 固定构造 `DisplayUnderPointer` 普通请求，保留工作区 JSON 与同文件图片附件；结果 `mode=display` 仅是输出元数据，**输入不接受 `mode`**。
- 返回阶段 A 的 `image_path`、`type`、`mime_type`、`width`、`height`，额外原样返回帧序号和接收时间。
- 合法 stop 不需要截图审批、工作区或视觉能力，取消状态也能执行清理；回执为 `{"action":"stop","state":"stopped"}`，不附图。工具对不可视觉模型的宣告过滤保持不变，不能借 stop 声称该模型支持截图。
- 内置注册表移除 `list_windows`；普通工厂保留兼容。`desktop_computer.observe` 仅提供元数据/UIA，拒绝图像参数，模型像素统一通过屏幕工具取得。
- 受控采集回调供宿主测试复用同一工具和交付实现，空回调才使用普通平台后端。受控来源明确标记，不能混称实际显示器采集。

Windows 还检查 WTS 会话 active/unlocked 和当前交互 desktop。实现的环境检查间隔设置为最多 250 ms 的轮询节奏，并在相关请求路径校验；这不是操作系统硬实时保证，也没有据此声称已测得真实锁屏或 RDP 断连时延。

## 定位、真实 RED 与修正

### 原 one-shot 不满足常驻要求

`session-red.txt`：旧实现已完成受控原生 PNG 捕获及像素验证，随后 `first.workerRunning && first.capturing` 失败。截图成功本身不能证明采集 worker/session 在请求后仍存活。

`screen-tool-red.txt`：新前端契约实跑 **27 项失败**，覆盖旧 Schema/窗口描述、legacy 参数、stop 审批与清理、`list_windows` 注册以及真实帧元数据回传。

`desktop-metadata-red.txt`：新 metadata-only 契约实跑 **33 项失败**，旧工具仍接受并产生窗口图片。以上均为先写契约后执行旧实现得到的 RED，不能因旧图片通路可用而视为本阶段已完成。

### resize 需要更换 pool/session 对

`session-first-green-attempt.txt` 记录改变自有窗口尺寸后等待帧超时，失败点仍是原有颜色、尺寸和新帧验证。随后 `session-resize-stage.txt` 将失败收窄到：

```text
WGC resize pool.CreateCaptureSession failed (0x8000FFFF)
```

据此限定修正为 resize 时更换 frame pool/session 对，保留常驻 worker 与 D3D device，不再在关闭 session 后复用旧 pool 创建新 session。`session-green.txt` 最终输出 `screenshot session lifecycle tests passed`，主任务核对 exit 0。没有放宽像素或尺寸断言，也不把“会话从不重建”当作常驻的验收条件。

### owned fixture 的消息泵互锁

最初 `screen-tool-owned-green.txt` 是 **0-byte 未完成日志**，不能因文件名含 green 视为通过。主任务对挂起的自有测试进程抓取 `owned-hang-stack.txt`：main 在同步请求 CV 等待；`mai-capture` 卡在 `USER32!GetWindowTextLengthW` → `SendMessageWorker` → `EnumWindows`，等待同进程窗口所有者处理消息。

原 fixture 在 main 创建窗口后直接阻塞调用捕获，没有泵窗口消息，违反普通截图 API 在工作线程调用的约定。修正仅作用于测试：

- 截图和工具执行放到独立 caller 线程，窗口 owner/main 持续 PeekMessage/TranslateMessage/DispatchMessage。
- 普通 WindowByTitle 歧义以及 WindowById 的 child、隐藏、最小化、销毁路径均保留；不是只改成按 ID 查找来绕开标题问题。
- 正常路径所有 caller 必须 join，销窗前 stop/join 采集 worker；单调用 20 s、全测试 90 s watchdog 超时只能失败退出 1，不能 skip。
- 仍只捕获自有窗口，不读用户显示器，不注入输入，不更改像素阈值。

修正后的 `screen-owned-pump-green.txt` 输出 `screenshot tool tests passed`，主任务核对 exit 0。该结果与最初挂起分开记录。

### 非输入 desktop 的环境错误分类

`private-desktop-red.txt` 中，自有 private desktop 子进程得到“target is closed, hidden, minimized, or disconnected”并被归类为 NotFound，环境错误没有被准确识别。日志最后即使存在 lifecycle passed 文本，前面的断言和子进程失败仍使该次运行失败，不能取末行判绿。

增加 WTS / 交互 desktop guard 后，`session-environment-green.txt` 返回预期的 NotConfigured：`screen capture requires the current interactive desktop`，并输出 `private non-input desktop capture guard test passed`，主任务核对 exit 0。

该用例只在自有、非当前输入 desktop 的子进程验证拒绝行为；没有调用 SwitchDesktop，没有实际锁屏或断开 RDP，没有输入注入。日志里的 PowerShell NativeCommandError 包装来自预期 stderr 诊断，最终结果应结合断言及主任务核对的真实退出码判读。

### stop 还需要通过调度层的工作区守卫

`workspace-stop-red.txt` 实跑 **6 项 FAIL**：工具自身的合法 stop 已不访问工作区，但 `MaiTurnRunner` 仍在进入工具前要求工作区，导致清理回执和 Completed 状态未落地。仅在 `execute()` 中免除路径检查不足以完成独立 stop。

修正增加 `MaiTool::requiresWorkingDirectory(argumentsJson)`，默认 `true`，保留其他工具原有守卫。仅完整且合法的 `screenshot {"action":"stop"}` 返回 `false`；capture、畸形参数或附带额外参数继续要求工作区。调度层复用该契约，不赋予 stop 文件访问能力，也不以此绕过工具参数校验。

新增 virtual 改变 Core/宿主共同依赖的头文件布局。对应变更记录在 `common-workspace-stop-delta-manifest.json`；全部相关对象重编后的真实 ToolLoop 用例通过，Core 最终 14/14 PASS（122.11 s），没有用直接工具测试或 Desktop 结果代替这条闭环。

### Core 缓存未跟踪头依赖导致混合对象

新增 virtual 后，`core-workspace-final-gates.txt` 记录 **14 项中 10 项 SEGFAULT**，不是最终 Core 通过。主任务定位到本地缓存的 MSVC `/showIncludes` 中文前缀乱码、Ninja 对象依赖记录 `#deps 0`，因此头文件变更没有触发全部相关对象重编，形成旧布局和新布局混合的构建输入。

处理限定于本机构建缓存：该工具链只有 zh-CN UI 资源，单设 VSLANG=1033 不会改变输出；必须让 includes 前缀与实测 UTF-8 字节一致。主任务重编全部相关对象，实核 `MaiFileTools.cpp.obj` 的依赖记录恢复为 **`#deps 15`，包含 `MaiTool.h`**；106 步构建 exit 0，最终 14/14 测试 exit 0。证据保存在 `core-dependency-final-configure.txt`、`core-dependency-final-rebuild.txt`、`core-dependency-final-readback.txt` 和 `core-clean-final-gates.txt`。先前失败日志保留，没有改生产门槛或测试断言。

## 已结束的 Windows 验证

| 日志 / 项目 | 实际结果与边界 |
| --- | --- |
| `screen-tool-green.txt` | 工具验证模式通过，exit 0；Schema、参数拒绝、stop、工作区和受控 PNG 帧元数据，不捕获用户显示器 |
| `removed-window-tool-green.txt` | `window list tool tests passed`，exit 0；验证从模型注册表移除，普通工厂兼容保留 |
| `session-green.txt` | 常驻生命周期及真实自有窗口像素验证通过，exit 0 |
| `session-environment-green.txt` | 生命周期与 private non-input desktop 环境拒绝用例通过，exit 0 |
| `screen-owned-pump-green.txt` | 修正 owner 消息泵后的自有窗口工具链通过，exit 0 |
| `desktop-screen-only-regression.txt` | 较早输入快照：**8/8 PASS，0 SKIP，总耗时 91.72 s**；不覆盖后续新增 virtual |
| `actual-display-metadata.txt` | 显式 `--display-metadata-only` 实际显示器路径，主任务核对 exit 0；两次 2560×1600，worker/device/session 均为 1，真实帧序号和计数 1→2，最后 stop 后 inactive |
| `desktop-workspace-final-build.txt` | 新 header 触发相关对象正常重编，120 PE imports / Qt plugins 检查成功；对应当前 exe SHA256 `f3b4e4f5fdaa8a784456d73dca049e18aea861683039722db2c9c31d624a0c1b` |
| `desktop-workspace-final-regression.txt` | 新增 virtual 后的当前 Desktop 输入：**8/8 PASS，0 SKIP，总耗时 89.89 s**；主任务已核对 |

Desktop 八组为：`desktop_session_sync_host_test`、`desktop_computer_contracts`、`desktop_computer_agent_integration`、`desktop_screenshot_integration`、`agent_controller_test`、`agent_panel_session_test`、`main_window_layout_test`、`ui_zoom_test`。其中 Agent integration 是确定性脚本槽，不能视为已完成系统键鼠操作。

产品截图用例通过真实 `AgentController` / `AgentChatPanel` 和同文件媒体交付链，采集回调将屏幕工具请求收窄到自有原生窗口，最新构建图证位于 `product-final-controlled-source/`（`product-final-visible.txt`，exit 0，0.99 s，主任务查看实际图片）；较早图证保留在 `product-controlled-source/`。它证明 screen-only 模型路由与受控原生来源可交付；实际全显示器采集由下述独立 metadata-only 运行验证，不将两种来源混称。

实际显示器验收已由主任务单独显式运行 `--display-metadata-only`，日志为 `actual-display-metadata.txt`。两次普通 `DisplayUnderPointer` 请求均成功，WIC 只核验 PNG 解码尺寸；worker/device/session 计数均保持 1，帧序号由 1 增至 2，接收时间从 `1794022471982` 增至 `1794022472067`。该次运行没有报告 source changed，并确认停止后 inactive。

该模式没有保存、展示或上传显示器 PNG，不调用模型，不创建覆盖窗口、不移动鼠标；PNG 字节只留在测试进程内存，日志仅记录尺寸、帧元数据和资源状态。此结果证明该次实际屏幕原生路径与同 monitor 资源复用，**不证明云端模型理解了画面或已向用户交付全屏图像**。默认测试仍只使用 owned/private desktop 来源。

## macOS 竞态修正与最终验收边界

主任务已合入同伴提供的 Mac 常驻采集增量。早期传输快照为 50517 bytes（SHA 前缀 `7af37`）及随后 36841-byte 增量（前缀 `fa359`）；这些早期短前缀不能代替最终输入身份。

本次新增回执包括 **20142-byte delta**，SHA256 `5a36281df832afefe2b7d2c65693062c52e2ddf88f9d92a7cc9c0897cd273d75`。合入该增量后的本地只读 blob 核验为：

- `MaiAgent/AgentTools/MaiScreenshotMac.mm`：`388a7c81b1a6e03b450e490676004440bc99e52c`。
- `MaiAgent/tests/MaiScreenshotSessionTestsMac.mm`：`d2cf079931db943cbd52f36c9044f76cf373c468`。

同伴针对 Blank → Complete 跨失效阶段增加 `availabilityEpoch`、public session/loginwindow guard 及离线对照，回执为 **4 项 FAIL → 0 FAIL**。这对应上述增量的局部修正，不将较早组件 Session GREEN 追认为最终通过。

该输入的最终 native 运行遇到目标 Mac 真实会话处于 locked 状态，后端返回 **NotConfigured**；CTest 退出码为 **8**，保留 **43 处下游断言失败**。失败记录保留，不能写成全绿、skip 或以早期 GREEN 覆盖。该结果没有完成该最终输入的可用交互桌面 native 验收。

复审又发现 `failed` 锁存可能被后续 Blank/Suspended 状态清除，再收到 Complete 后复活已失败 session。最终 2613-byte 小补丁（SHA256 `cd4f33fb90c092cc987e1e819c94b54ec1f0b2f00b5eea2d41cb27626b7c6ef4`）让同 generation 内的 failed 单向锁存，只在新 generation 后重置。六种终止原因与中间状态组合的离线用例真实 **18 FAIL → 0 FAIL**，Root 已查看原日志并独立复核代码。

最终 Mac 两文件 canonical blob 为 `82722faeea96d3054d3b7e8251632e63d312049e` 与 `82be5ef9e6b985b9cf1d47c99936a1364cfd2448`；双方最终 23/23 源码 blob 一致，产品 tree `e0dc4d994bade89fbb529f03ad8003acde26fa5c`，Core tree `69e18eb1e4b587b30ad8b467dd3172bea0443b8e`。

同伴最终限定 Desktop 11 目标正常编译链接通过（49.28 s），Core all 构建通过（76.34 s），记录的 62 个 Core 头文件消费者全部重编。Desktop 7/7 无采集 CTest 通过，使用 offscreen；MainWindow 内部 4 个明确 Qt SKIP 为 3 个 Windows caption 专属用例及 1 个无 live 头像 URL，用例数量与 CTest 整项跳过分别记录。最终独立离线状态测试重新链接新 Core 后 0 FAIL。App SHA256 为 `d0dcc1ec3ad8c8db79f7cf89c42cda700440844745971f24129fe596c107886c`。

Root 已读取最终 Core 逐项 command/time/binary SHA256 回执：14/14 全部 exit 0，总 369.25 s，包含 required 十组，以及 Screenshot / WindowList 的 `--request-validation-only`、ViewImage、ContextBuilderImage。这里没有默认 Session native 测试，不能与 Windows 包含 Session 的十四组混作同一覆盖范围。最终未运行真实产品捕获、系统输入或付费供应商。

**不能把离线 GREEN、offscreen 回归或较早窗口结果写成最终 native 通过**。Mac 现装 PID 96951 按同伴反馈未改，系统键鼠 native 验收未运行。

Mac 额外执行 `all` 暴露 `reply_suggestion_controller_test` 链接缺口：未解析符号包含 MaiFfplayVideoDialog、DesktopMusicPlayerDialog、DesktopCameraDialog、MessageImageLoader 的实现。本轮未修改这些源文件或 Desktop CMake，冻结输入未为此扩大修改；原失败日志保留，指定 App/本轮消费者目标另外构建和验收，不宣称 Mac 全目标构建通过，也未把仅有源码无差异当成旧版本已实跑复现。

## 验证边界、待办与最终回填

- 阶段 B Core 最终 required gates：正确 UTF-8 includes 前缀和全部相关对象重编后 **14/14 PASS、0 SKIP、122.11 s**，包含 required Core 十组及 ScreenshotSession / ScreenshotTool / WindowListTool / ViewImageTool 四组；真实无工作目录 stop 闭环已从 6 FAIL 变为通过。先前 10 项 SEGFAULT 原日志保留。
- Windows 实际全显示器 metadata-only 捕获：已完成上述单独运行，exit 0；只有元数据与停止/资源复用证据，没有全屏图像交付或云端识图结论。
- Windows Desktop：当前新增 virtual 后输入已完成 8/8 PASS、0 SKIP、89.89 s；91.72 s 的八组结果保留为较早快照，不能与当前产物混用。
- macOS：availabilityEpoch 离线 4 FAIL→0 FAIL、failed 锁存离线 18 FAIL→0 FAIL 已完成；最终限定编译、Core 14 组及无采集 Desktop 7 组通过。locked 环境 NotConfigured 的 CTest exit 8 / 43 处下游断言失败保留，最终 native 仍未验收，不把早期组件结果并入最终通过。额外 all 链接缺口未修，单列记录。
- DeepSeek：没有运行付费的 live 云端识图，没有把当前 raw frame / PNG 回灌描述为云端已理解画面。阶段 B 最终 MaiModelClientTests 全套已包含真实 view_image→Agent→两轮本地 HTTP 的 ChatCompletions/Responses 图片字节验证并通过；仍不等同 live 供应商验收。
- 系统键鼠操作：原生 key/mouse acceptance 仍 pending；本阶段没有用截图成功替代输入验收。
- 交付：阶段 B 代码 `85592c748fac52546ed91af9fae241a3d4d77b00` 已提交并推送；Windows 私有 ZIP 的目录/解压后运行验证通过，未安装或发布。Windows 现装进程当前去向未调查，不能报告仍运行；Mac 仅按同伴提供的未替换信息记录。

| 最终引用 | 核对结果与边界 |
| --- | --- |
| 阶段 B commit / 产品与 Core tree | `85592c748fac52546ed91af9fae241a3d4d77b00`；MaiChat `e0dc4d994bade89fbb529f03ad8003acde26fa5c`；Core `69e18eb1e4b587b30ad8b467dd3172bea0443b8e` |
| 最终 Core 门禁 | `core-dependency-final-rebuild.txt`、`core-dependency-final-readback.txt`（含 MaiTool.h，#deps 15）及 `core-clean-final-gates.txt`：14/14 PASS、0 SKIP、122.11 s，主任务核对 exit 0 |
| Windows 全显示器实测 | 最终 `actual-display-final-metadata.txt` exit 0、2560×1600、帧 1→1 / 接收时间保持原值、资源计数复用与停止后 inactive；较早动态 1→2 单独保留，不保存或展示用户桌面像素 |
| Mac 最终输入及验收 | 两次修补完整 SHA、最终 827/82be blob、离线红绿、App SHA 及限定编译/Core 14/Desktop 7 结果已列；原始逐项回执留存本地证据。额外 all 链接失败单列；最终 native/真实 display 验收仍受锁屏环境限制 |
| Windows 最终构建与包 | `private-85592c74-20261107T120311/` 完成完整 staging、实际 ZIP 解压及两处 120 PE / 4 模型 / ORT / 裸 PATH smoke 检查，均 exit 0；源码与产物身份详见下文 |
| 安装及运行身份 | 本轮无安装/发布；如后续执行，应另核对实际运行路径、PID 与同一产物 SHA |

### 最终 Windows 显示器静止帧复验

正确依赖重编后的 Session 测试二进制显式运行 `--display-metadata-only`，`actual-display-final-metadata.txt` 核对 exit 0：两次 2560×1600，同一 worker/device/session 各 1；这次屏幕静止，两次保留同一 `frameSequence=1` 和 `frameReceivedAtMs=1794023408150`，没有伪造新帧时间。最后 stop 后 inactive。较早运行的帧 1→2 与本次静止帧复用分别记录，均只保留元数据，不输出用户屏幕图片。

## Windows 私有包交付

以干净 `main` 的 `85592c748fac52546ed91af9fae241a3d4d77b00` 正常构建并打包，证据在 `.cache/resident-screen-20261107/private-85592c74-20261107T120311/`。归档名 `MaiChat-win64-v2.2.52-20261107-85592c74.zip`，244314981 bytes，SHA256 `85ea5aa4bbb99498258e60ada9d679ea61b823e3a02a2c7ff1a90b24f0837dad`。

构建目录、完整 staging、实际 ZIP 解压目录的 `maichat.exe` 均为 163755520 bytes / SHA256 `f3b4e4f5fdaa8a784456d73dca049e18aea861683039722db2c9c31d624a0c1b`。staging 与解压目录分别通过 120 PE imports / Qt plugins、4/4 Portrait 模型及 ORT 检查，使用 `C:\Windows\System32;C:\Windows` 的裸 PATH 执行 `--smoke` 均 exit 0，实际子进程分别为 PID 8924 与 4256，均已退出。smoke 跳过登录/账号恢复，不据此声称已登录用户会话运行过新功能。

结构化交付回执见 [delivery JSON](maichat-resident-screen-capture-delivery-20261107.json)。其 `sourceCommit` 指向实际打包代码提交；后续仅有诊断文档提交时，产品/Core tree 与已验二进制保持不变，不为文档重打包。Mac 最终完整回执的 23/23 canonical 和两棵 tree 已由 Root 独立比对，并已归档原始 42 文件证据；额外 all 失败、Qt 内部跳过与原生未验收边界均保留。

Windows 本文的“0 SKIP”指 CTest 整项计数，不包含未运行的系统键鼠验收；Mac 的四个 Qt 内部跳过单独列出。公开 v2.2.52 资产、现装程序及系统设置均未由本轮更改。这个私有包通过打包与 Windows 屏幕路径验证，系统输入、Mac 最终原生采集及 live 供应商视觉理解仍未验收。
