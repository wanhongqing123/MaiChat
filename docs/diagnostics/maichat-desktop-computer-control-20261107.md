# Desktop 通用电脑操作：实现与验证记录

记录时间：2026-11-07，Asia/Shanghai。Core 10 套、5 组相关桌面回归及确定性工具链通过；Mac 最小修正后五目标正式编译链接通过。Windows 与 macOS 的原生输入均尚未验收完成，不能将编译、模拟后端通过或环境跳过视作系统操作成功。

## 需求、源码与运行对象

需求是在 PC 创作中心的真实 Agent 会话中发现窗口、按窗口身份观察画面和控件、操作键盘鼠标，并读回结果。普通 C++ 调用与模型工具调用应使用同一实现。输入发送成功只说明操作已派发，不能据此声称业务提交或保存成功。

- 源码工作区：`E:/OpenSource/MaiChat`，分支 `main`。
- 草稿整理时基线：`17d6d44144ba2b3a2edcd069fbdb7fe636e2e4ed`，另有未提交 overlay。后续构建、测试结果必须与最终 overlay 或提交对应，不能把基线提交当作全部当前源码。
- Desktop 本地构建目录：`MaiChat/desktop/.cache/build-release-62111144`；测试运行产物位于其 `bin/`。
- 本地日志及隔离 fixture 证据：`.cache/computer-control-20261107/`。该目录是本机临时证据，未承诺随提交保存。
- 主任务只读现场核对的现装进程为 PID `584`，安装目录为 `D:/Program Files/MaiChat`。本轮没有替换、停止或升级该现装程序；它不是本次 overlay 的运行验收产物。此项为主任务观测，本文作者未重新检查运行进程。
- 操作验收只针对测试创建的隔离编辑器及自有子进程；未执行用户实际 CR、账号操作或付费模型任务。
- 提交前冻结的产品 tree 为 `4f49a213fcae5e7ce0293099005b32f447e1aad7`，Core tree 为 `43554c22475f509181a78c80bbecbceab24ed897`。十套 Core 执行后只改了 Mac Desktop 适配和测试/文档，Core 输入未变；最终 Windows 正常依赖构建 `frozen-input-build.txt` 为 exit 0 / no work。
- Windows 构建主程序为 163724800 bytes，SHA256 `745e8f955d79aee171e8d3f00c29ead8f3ed820bb9a84e223b62d0bf0f3edf38`。最终提交及私有封装证据另存本机 `.cache/computer-control-20261107/`，不将现装路径当成本次源码产物。

## 现场问题与可证实的根因

现场流程将 `desktop_computer` 的 `clear` 当作删除输入文本。源码中的 `DesktopComputerController::clear()` 实际只从 session 表中移除观察状态，不派发键盘或文本编辑操作；清除之后，旧窗口观察及 snapshot 不再可用。工具说明与回执现在明确表示 `scope=window_observation`、`text_changed=false`，并提示重新 observe。

原有工具还缺少日常编辑需要的通用快捷键与显式文本替换入口。把“清空文本”交给 `clear` 会同时造成文本未删除和后续观察状态丢失。当前实现保留 clear 的原义，使用 `replace_text`（允许空串）或全选快捷键完成编辑；这项判断来自工具契约、控制器源码及隔离测试，不判断用户真实业务的最终结果。

## 复现操作与预期

测试编辑器使用独立标题、空白输入框及 Enter 确认计数。每次实际操作前重新观察同一窗口 ID。

1. 从真实创作中心输入中文任务并发送，由脚本模型驱动正式 Agent 工具循环。
2. `list` 发现测试创建的窗口；从回执读取 `window_id`、平台和 `select_all_key`。
3. `observe` 按 ID 取得截图、控件和 `snapshot_id`；通过唯一可访问名称定位编辑区。
4. 点击编辑区，输入中文；使用 Windows `Ctrl+A` 或 macOS `Cmd+A`，再输入替换内容。
5. 用 `replace_text` 的空串清空，再写入最终中文，发送 Enter。
6. `verify` 读回选中窗口的非安全输入值，同时核对自有子进程记录的文本、确认文本及 Enter 计数。

预期：模型工具结果与实际读回一致，最终会话中出现确认文字。不同 session、重复标题、旧或已消费 token、身份/几何变化、安全输入、失焦、取消及权限拒绝均有明确结果。dry-run 不激活、不注入、不读取焦点文本，也不消费 token。观察 reset 不编辑文本。双击、右击、拖动和滚动也应限制在已观察目标内。

### 最小普通 API / JSON 调用顺序

以下仅说明调用契约，不是本轮原生操作通过的证据。`W` 来自 list，`S1`、`S2` 等来自对应的最新 observe；替换文本前应已按观察结果点击目标编辑控件。

| 步骤 | 普通 API | 工具参数示意 |
| --- | --- | --- |
| 发现目标 | `controller.windows()` | `{"action":"list"}` |
| 按 ID 观察，取得 S1 | `controller.observe(session, "", workspace, true, W, includeImage)` | `{"action":"observe","window_id":"W","include_elements":true,"include_image":true}` |
| 清空编辑文本 | 填写 action=`replace_text`、windowId=W、snapshotId=S1、text=`""` 的 `DesktopComputerAction`，调用 `controller.perform(session, request)` | `{"action":"replace_text","window_id":"W","snapshot_id":"S1","text":""}` |
| 重新 observe 取得 S2，再写最终文本 | 同一 `perform()`，action=`replace_text`、snapshotId=S2 | `{"action":"replace_text","window_id":"W","snapshot_id":"S2","text":"测试最终文本"}` |
| 重新 observe 取得 S3，再读回 | 同一 `perform()`，action=`verify`、snapshotId=S3、expectedText 为预期文本 | `{"action":"verify","window_id":"W","snapshot_id":"S3","expected_text":"测试最终文本"}` |
| 忘记观察 | `controller.clear(session)`；不删除输入文字 | `{"action":"clear"}` |

需要 Enter 或其他操作时，先取得新的观察 token，再发送 key；全选快捷键读取 list 的 `select_all_key`。`glm-5.3` 使用 `include_image=false`，由窗口/控件树观察；显式要求图像会被拒绝。其他不能接收图像的模型也可显式设该参数。

## 当前实现及安全边界

- `DesktopComputerController::perform()` 是普通 API 与 `desktop_computer` 工具共用的 dispatcher；工具负责 JSON Schema、解析、审批和结果适配。公开 `makeDesktopComputerBackend()` 与默认控制器复用生产原生后端，测试可以包装后端以限制可见窗口。
- `list` 返回窗口 ID/PID、标题、应用及几何；同名标题应改用 ID 观察，不能任取一个窗口。回执补充 `platform` 与 `select_all_key`。不接收图像的模型可使用 `include_image=false`，取得窗口和可访问控件信息。
- Core 增加 `MaiScreenshotTarget::WindowById`。零或无效 ID、不可见/最小化目标返回错误，不能回退到标题或显示器截图。控制器在截图前后核对 ID/PID、标题与几何，降低句柄回收及目标变化风险。
- Windows 观察和输入使用一致的物理坐标空间，原生代码设置线程 Per Monitor V2 DPI context，并在结束后恢复。指针位置还要属于所选窗口或可证明所属的弹窗。
- Windows 使用 SendInput，macOS 使用 CGEvent；共享快捷键解析，支持 Unicode、组合键和空串替换。原生多事件操作检查取消及焦点，按作用域配对释放本次合成按下的键和按钮，不主动释放用户原先按住的键。
- 输入阶段允许可证明属于选中窗口的 popup。Windows 检查同 PID 的 owner 链；Mac overlay 增加同 PID 的公开 AX parent/sheets 有界归属检查。不能仅凭同一应用接受无归属的兄弟窗口。
- 显式 focus 和 verify 保持严格窗口身份。Windows 读回校验 UIA 元素 PID 与祖先窗口；Mac 校验 AXWindow；读取后再次检查严格焦点。安全输入、无法证明的焦点和值读取均不能回作成功读回。
- `type`、`replace_text` 及可能写值的按键使用同一安全输入检查，避免 `key=A`、粘贴或删除快捷键绕过。当前 Tab/Escape 保留导航离开的例外。
- snapshot 绑定 session、窗口身份、几何和五分钟有效期。语法或 dry-run 验证不消费 token；一旦尝试激活或实际操作，即使失败或部分执行，也要求重新观察。verify 开始读回时同样消费 token。
- 回执分别给出 `dispatched` 与 `verified`。读回匹配只证明本次读取的值，不证明应用保存、服务端持久化或业务提交。

## 已有本地证据

以下 RED 为真实运行失败，之后以对应 GREEN 日志核对；不是仅新增断言后直接报通过。

| 检查 | RED 证据 | 修复后证据及边界 |
| --- | --- | --- |
| 工具公开契约 | `contracts-red.txt`：缺少 list/focus/双击/右击/拖动/replace_text/verify、快捷键受枚举限制及空串替换，共 9 个失败 | `contracts-agent-green.txt`，以及最终 `contracts-agent-final.txt` 通过；原生系统输入不在该契约测试内 |
| Core 按窗口 ID 截图 | `core-window-id-red.txt`：零 ID 被错误当作显示器捕获，错误码及清空输出共 4 个失败 | `core-window-id-green.txt` 输出 `screenshot tool tests passed`；后续全截图组最终见 DPI 修正日志 |
| owner popup 输入与严格读回 | `popup-red.txt`：popup 被激活路径破坏、后续输入与严格 verify 边界，共 5 个失败 | `contracts-reviewed-green.txt` 与最终契约通过；这是后端 seam 的确定性验证，不能证明 Mac/Windows 原生 popup 行为 |
| 安全输入的 key 绕过 | `secure-key-red.txt`：危险按键未拒绝且仍写入 backend，共 12 个失败 | `contracts-reviewed-green.txt` 与最终契约通过；验证密码框不能借字符/粘贴等 key 绕过控制器检查 |

### 最终契约与确定性 Agent 链

`contracts-agent-final.txt` 记录两项通过：

- `desktop_computer_contracts`。
- `desktop_computer_agent_integration`，命令明确选择 `chinesePromptTraversesRealAgentAndReadsBackEditor`。

此处是 **2/2 确定性测试通过**。第二项经过真实 `AgentChatPanel`、`AgentController`、Agent 循环及生产工具/dispatcher，但编辑器由进程内测试 backend 操作，不使用系统键盘鼠标注入。`agent-ui-final/computer-agent-transcript.json` 明确记录 `backend=in_process_editor_no_native_injection`、17 次调用与 17 次结果。不能把这 2/2 写成 Windows 原生电脑操作或独立进程原生验收通过。

早期 `agent-ui-final/` 保存中文输入、7 次输入步骤和最终会话/编辑器截图。最后专项的截图位于 `final-computer-suite/fake/computer-agent-verified-editor-and-conversation.png`；主任务已亲自查看，最终中文文字和 17 个已完成工具行可见。此为主任务的视觉核对，本文作者未重新看图；图中仍使用 fake backend，不能证明系统输入成功。

### Core 10 套与受影响回归

`core-ten-results.json` 记录以下 10 套均实际执行且退出 `0`：`MaiCoreTests`、`MaiFilePathTests`、`MaiStoreTests`、`MaiModelClientTests`、`MaiTurnRunnerTests`、`MaiToolTests`、`MaiToolLoopTests`、`MaiPermissionTests`、`MaiQuestionTests`、`MaiInputQueueTests`。每项在 JSON 中列有对应本地日志路径。

`affected-regression.txt` 记录 5/5 通过，总用时 84.12 秒：

| 用例 | 结果 | 用时 |
| --- | --- | --- |
| `desktop_session_sync_host_test` | PASS | 36.92 s |
| `agent_controller_test` | PASS | 9.97 s |
| `agent_panel_session_test` | PASS | 8.69 s |
| `main_window_layout_test` | PASS | 27.93 s |
| `ui_zoom_test` | PASS | 0.59 s |

### Windows Core 完整截图组与 DPI fixture

早期 `core-screenshot-full-green.txt` 实际记录 `screenshot.height < 300` 失败；文件名中的 green 不是通过证据。当时 fixture 所在线程继承 150% DPI 行为，创建窗口尺寸和截图物理像素的假设不一致。

fixture 在创建同名窗口前启用 Per Monitor V2 DPI，退出时用 RAII 恢复原 context；原窗口尺寸和 `<300` 断言均保留。`core-screenshot-dpi-fixed.txt` 最终输出 `screenshot tool tests passed`，是本次 Windows 完整截图组通过的依据。没有通过放宽阈值消除失败。

### Windows 原生输入尚未验收

`computer-suite-first.txt` 中，契约和确定性 Agent 链通过，但两个原生用例均跳过：

- 进程内 `desktop_computer_tools` 的自有 fixture 被 Windows 拒绝成为前台，日志明确表示没有发送鼠标输入。
- 双进程 `desktop_computer_native_integration` 在 focus 阶段得到 `Windows did not allow the selected window to become foreground`，尚未进入编辑输入。

`native/native-transcript.json` 保留了自有子进程及该 focus 错误。随后 `native-interactive-a.txt` 记录启动自有编辑器、等待用户实际选中该窗口并释放输入，等待 120 秒后仍未达到条件；CTests 显示 Skipped，用时约 120.92 秒，没有继续原生输入。

最后 `computer-suite-final.txt` 是 **2 PASS + 3 SKIP**，总用时 7.42 秒：契约与 fake Agent 链通过；`desktop_computer_tools`、`desktop_computer_native_integration`、`desktop_computer_creation_native` 均为 Skipped。创作中心原生槽实际返回 `77`（主任务核对），CTests 正确识别为跳过；`final-computer-suite/native/creation-native-ready.json` 记录 `state=not_foreground_no_input_sent`。

Windows Qt GUI 测试进程的 stdout 在本轮采集为空，因此弃用依赖 `SKIP_REGULAR_EXPRESSION` 识别 QSKIP 的方式；测试入口将原生 readiness 的 QSKIP 显式映射为进程退出 `77`，CMake 使用 `SKIP_RETURN_CODE 77`。所有 native 用例在真实 input 尝试后发生失败必须退出 `1`，不能降为 `77`。只有尚未发送输入时缺少交互或权限条件，才允许按 readiness 跳过。

CTests 对包含 SKIP 的测试集合仍可能显示 `100% tests passed`。这里应按具体的 2 PASS / 3 SKIP 及未注入事实判读，不能视作操作成功。

主任务另只读核对：当时实际前台仍为现装 MaiChat PID 584，测试与现装处于同一 Session 1 / Default desktop；`SPI_GETFOREGROUNDLOCKTIMEOUT` 返回 `2147483647`。这些环境观测解释当前缺少交互条件，不证明某个输入实现已经正确或错误。本轮没有更改 foreground lock 设置，没有使用 AttachThreadInput 绕过限制，也没有改动或替换现装 App。此段为主任务提供的观测，本文作者未独立运行系统检查。

## macOS 同伴反馈

主任务收到 Mac peer 对较早版本的回报：受影响源码编译、contract 与 fake backend 的 Agent 链通过；两个原生测试在窗口 discovery 阶段失败，均退出 `1`，没有作为 SKIP 处理，也没有进入输入。本文作者没有 Mac 编译/运行环境，未亲验该反馈，亦未把它等同于 macOS 原生验收。

peer 随后提供的决定性环境观测：

- `CGSessionCopyCurrentDictionary` 中 `CGSSessionScreenIsLocked=true`，且 `frontmost_is_loginwindow=true`。
- `display_active=true`、`asleep=false`，不是以显示器休眠推断锁屏。
- 捕获 preflight 返回 `true`。Swift 只读探针的 `AXIsProcessTrusted=false`；该结果属于 Swift 探针，不能代替实际 C++ receiver 的 TCC 权限结论。
- 自有 child PID 出现在 all CG 窗口列表，`layer=0`、`alpha=1`、标题符合自有 fixture；但对应 on-screen 列表为空。

这些观测表明 Mac 当前锁屏，尚不具备完成可见窗口原生验收的条件。本轮没有绕过锁屏或执行解锁；需要可交互的已解锁会话后重新验证。这里保留两个测试退出 `1` 的实际结果，不改写成原生通过或跳过。

源码复审还发现并修正 Qt 5 下 QString 下标重载问题。Mac 输入归属增加公开 AX 有界父链和 sheet 子节点，严格 focus/readback 未放宽。peer 在 `b1169b` 验证时出现 `kAXSheetsAttribute` 未声明及 `std::any_of` / `__invoke` 两条诊断；lambda 原文实际上已经是正确的 `const SheetNode&`，先前的参数类型归因已更正。peer 用原编译参数在精确私有副本中只换成 SDK 公开属性后编译 exit 0，两条诊断同时消失，确认后者属于连带诊断。

最终修正使用 `kAXChildrenAttribute`，只遍历同 PID 且 role 为 `kAXSheetRole` 的子节点，保持父链 16 级、sheet 深度 4、总项数 64、去重和引用释放。主任务核对补丁为 2012 bytes、SHA256 `0ff2bc515ee08fb9bd5f4df20038a9132a231074c1aa16fa45f5d21028c2e290`，apply-check 后合入；再次独立源码复核未发现新增阻塞。无法证明归属、超过遍历上限或中间非 sheet 容器均保守拒绝。

peer 最终报告平台为 Apple M3 Pro / arm64、macOS 27.2（26B123）、Apple clang 21、Qt 5.15.18 Cocoa。最小修正和第二测试增量合入后，maichat、contracts、AgentIntegration、nativeIntegration、ToolsTest 五目标编译链接 exit 0（28.69 s，`final-build.log`）；contracts 与 fake Agent 链 CTest 2/2 PASS、0 SKIP（5.85 s），后者 17 调用/17 结果，Qt setup/cleanup 加槽为 3 PASS。Core Screenshot 仅 `--request-validation-only` exit 0，未捕获其他窗口。

主任务已读取 peer 的 Markdown、构建/执行 receipt 和 canonical JSON，17 项生产/测试/CMake Git blob 与本次冻结清单逐项一致；此核对不包含计划和诊断，也不是整棵 Mac 工作区比较。Mac 构建主程序 154230120 bytes，SHA256 `1f61b06385fe9dbe39a3c97f850f4b46a12ac8765b7b8320f70aa4f006f2c61b`。日志位于 Mac `/private/tmp/maichat-computer-mac-20261107-0ad70810`；截图仅是 Widget 隔离后端画面，不能视为解锁桌面原生验收。最后三项原生测试没有在锁屏条件下重跑，仍为 pending；现装 Mac PID 96951 未替换。

待补：peer 的源码提交/overlay 标识、设备与系统版本、构建命令、日志位置、discovery 失败原文、实际 C++ receiver 的权限核对和解锁会话后的复测结果。

## 验证状态与待补结果

以下按实际已结束的运行区分通过、跳过及 pending；源码审查和构建日志不能代替对应的原生操作证据：

| 项目 | 当前证据 | 待填写 |
| --- | --- | --- |
| 最终源码与构建身份 | 当前为 main `17d6d441...` + overlay；`build-final-native-creation.txt` 记录本地相关目标构建和 runtime PE 检查 | 最终提交/overlay、各产物路径与哈希、完整构建结束状态 |
| Core 10 套 required gates | `core-ten-results.json`：10/10 实际执行，全部 exit 0 | 已完成本轮日志核对；最终源码身份仍与主任务冻结结果关联 |
| 受影响 Desktop 回归 | `affected-regression.txt`：5/5 PASS，84.12 s | 已完成本轮日志核对 |
| 电脑专项与创作中心原生全链 | `computer-suite-final.txt`：2 PASS + 3 SKIP；creation-native-ready 为未成为前台、未输入，真实退出 77 | 原生系统操作闭环仍未验收；需要前台交互条件后才有后续结果 |
| macOS 新增量及原生操作 | peer 最小公开 AXChildren 修正后五目标编译链接 exit 0；2/2 无注入 CTest PASS、0 SKIP，17/17 canonical blob 一致；锁屏时先前两个 native discovery 失败、退出 1、未输入 | 解锁交互会话、实际 receiver 权限核对及受控原生复现 |
| 最终运行与交付 | 现装 PID 584 未动；当前测试产物不代表安装包 | 若有交付，补目标运行进程、安装/打包产物身份及必要的 staging/ZIP 验证 |

新增真实创作中心原生用例只把 `windows()` 收窄到测试拥有的 child PID，capture、activate、input、secure-input 与 focusedText 均转发生产 backend。它要求实际前台及只读 preflight 就绪才发送任务；未开始 input 的失焦可以跳过，任何原生 input 尝试后的工作流失败必须退出 `1`，并保存部分证据。最后实际运行止于 readiness，尚没有这个用例的原生输入成功结果。

后续结果填写处：

- 最终源码/产物：以包含本文的提交、上述产品/Core tree 和私有封装清单为准；现装 PID 584 未替换。
- Core 10 套：10/10 exit 0；受影响回归：5/5 PASS，证据见上。
- Windows 创作中心原生闭环：pending。
- Mac peer 编译/无注入补证已收到并核对；原生复测仍 pending。
- 最终交付判断：代码与已完成的编译/确定性回归可以交接；系统键鼠能力尚未验收，不能宣称已让 Agent 完整操作用户电脑。未安装或发布本轮源码对应的新版本。

下一次 Windows 原生验收选择 `desktop_computer_creation_native`，设置 `MAICHAT_COMPUTER_NATIVE_WAIT_FOCUS_SECONDS=120`，从正式 Windows 测试脚本启动。用户实际点击独立的 `MaiChat Creation Native` 编辑器并释放键鼠后才继续；必须同时核对 17 次工具调用、真实文本读回和子进程 Enter 状态。Mac 在解锁后先核对实际接收进程的 AX/捕获授权，再执行同一自有窗口验收。若条件未就绪，保持失败或明确 skip，不重复尝试输入。
