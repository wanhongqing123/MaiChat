# Desktop shell 会话入口整合

- 时间：2026-11-21，Windows；起始源码 `38d25b9d7a41ad29128e95289f44937c676ca452`，保留前五批现有未提交修改。
- 复现：打开 Desktop 创作中心新会话，查看欢迎页“终端与进程”能力卡；当前列出 `shell` 与四个 `desktop_process_*`，没有统一 `shell_input` 会话入口。旧 Qt 进程管理器在 `AgentController` 独立注册，与 Core 的 shell 执行路径分离。
- 预期操作：从同一能力卡开始长时间命令；`shell` 返回可继续使用的会话 ID；用 `shell_input` 补读该会话输出，需要时写入输入或停止。完成后仍在同一创作会话，输出与退出状态清楚。欢迎页只列当前可用的 `shell`、`shell_input`，不再推荐已退役的四个工具。只公开 shell_input 时，说明应指向已有会话，不承诺启动新命令。
- 摘要预期：`shell_input` 计为 shell；当前活动工具只有 `shell` 与 `shell_input`。历史消息中的四个旧名仍按 shell 显示，以保持旧记录语义，但不得再注册成运行时工具。`ssh_exec` 保持内置；其余第五批的计数、状态优先级、缺名兼容和折叠行为不变。Android/iOS 标签不修改。
- 执行顺序：先新增目录、真实注册表与摘要回归并运行 RED；等待 Core shell/shell_input 实现通过后，才移除 DesktopProcessTools 源码、独立测试、注册和构建项。生产只保留 Core 统一执行路径，不长期桥接两套进程状态。
- 验证方案：`starter_catalog_test` 检查 shell/shell_input 同卡、旧名不属于该活动工具家族、仅 shell_input 的已有会话文案；实际 Controller/欢迎页用例检查公开注册表只有新入口、没有旧四项；远程真实 Qt 卡片检查 shell_input 摘要及保留的历史旧名。用自有夹具截图，不执行真实网络或用户命令。完整正常 App、打包及 smoke 由主任务协调。
- RED：2026-11-21 22:26（Asia/Shanghai），MSVC 2022 / Qt 6.12.0 / Windows 11 windows QPA，正常桌面缓存重建 `starter_catalog_test remote_tool_detail_test agent_panel_session_test` 成功。目录测试返回 125 checks / 10 failures：`shell_input` 未同卡、缺长时间命令补读文案、旧四项仍在终端家族；远程 shell_input 单卡实际为“内置工具调用 1 次”而非“shell 调用 1 次”（2 passed / 1 failed）；真实欢迎页的文本模型、图像模型都在缺少 shell_input 处失败（2 passed / 2 failed），日志 `shell-session-desktop-summary-red-20261121.txt`、`shell-session-desktop-registry-red-20261121.txt`。
- Core 门槛：Core 原生 31 项测试全部通过，覆盖输入、轮询、进程数上限、输出捕获及生命周期；主任务确认后端源码冻结并明确开放删除门槛。随后删除 DesktopProcessTools 两个源文件、旧独立测试，移除 AgentController 注册及 Desktop CMake 项；统一使用 Core shell 会话实现。源码搜索确认运行时工具目录、AgentController、StarterCatalog 和 Desktop CMake 已无旧注册或旧名。

实际组件验收新增 `AgentPanelSessionTest::shellSessionSurvivesAnOrdinaryTurnAndContinuesThroughApproval`：通过 `MAICHAT_SHELL_CHILD_FIXTURE` 指定自有 `MaiShellToolTests` 可执行文件的 `--capture-child-session` 模式。第一轮模型调用真实 shell，点击现有授权按钮，结束普通 Agent 轮次后验证进程仍存活；第二轮调用真实 shell_input，分别经输入、停止授权，验证同一会话 ID、原生 PID 已退出、`stdin-write-marker` 以及原生回显。截取两轮实际 Windows 面板，确认总计三次 shell 调用。没有新增产品测试入口，夹具只在临时目录创建自有标记；未提供夹具路径的普通全量测试会显式跳过此专项，验收运行必须提供路径并无跳过。

2026-11-21 22:45—22:46（Asia/Shanghai）GREEN：在 `MaiChat/desktop/.cache/build-qt6-ui-20261109` 以 MSVC 2022、`-j 2` 重建三个目标成功。新 `MaiTool.h` 时间为 22:31:17，Core MaiAgent、Desktop AgentController、AgentPanelSessionTest 对象分别重编译于 22:44:19、22:44:34、22:45:22。Ninja 的 Controller 依赖记录为 VALID 并包含 MaiTool.h，避免混用旧虚表布局。

- `starter_catalog_test`：129 checks / 0 failures，日志 `shell-session-desktop-catalog-green-20261121.txt`。
- 完整 `remote_tool_detail_test`：26 passed / 0 failed / 0 skipped，日志 `shell-session-desktop-summary-green-20261121.txt`；包含新活动工具对、shell_input 单独分类，以及保留的第五批历史工具分类用例。
- `agent_panel_session_test` 的两轮流程、欢迎页两种模型、历史三组摘要和已有连续工具组用例：7 passed / 0 failed / 0 skipped，日志 `shell-session-desktop-flow-green-20261121.txt`。三次授权均点击真实面板按钮，原生 PID 在第一轮结束后仍活着、停止后退出，第二轮输入确实写入同一进程。已有面板构造产生一条 QFont::setPointSize(-1) 警告，截图字号正常，本次未扩大修改范围。
- 使用 windows QPA，未设置 offscreen；`shell-session-desktop-20261121/shell-session-turn-one-owned.png` 与 `shell-session-turn-two-owned.png` 已逐张查看，第一轮 shell 1 次、第二轮 shell 2 次均显示已完成。该目录的 `active-shell-session-pair.png`、`shell-input-session-is-shell.png` 也已查看，560 像素逻辑宽度下文字与箭头完整，无裁切。

验证二进制 SHA-256：

| 产物 | SHA-256 |
| --- | --- |
| starter_catalog_test.exe | BBEF706264C3F9520B62F6AABCA90A3C5EA2D098B14D013D0C476072A689F9AF |
| remote_tool_detail_test.exe | 5EF5504EFEA5AAB01FB79B509440C3488B93C8C0F4318F8185457CA70DCFD02B |
| agent_panel_session_test.exe | 46D0F4AD6155184F2DFA499D172D942A248E039185EED90CE76D1474FCA40A25 |
| MaiAgent/.cache/build-file-batch2-20261121/bin/MaiShellToolTests.exe | F3EDE05D42CE5F8AB790C875B3305F9AB8309155335357E0F3FF3207EE00A9F4 |

边界：以上覆盖真实 Controller、真实 Qt 面板、真实授权和 Core 原生进程，模型是进程内自有夹具，没有真实供应商、账号或网络请求。不是正常 MainWindow 登录后的供应商完整业务验收。最终正常 App 构建、staging/ZIP 裸 PATH smoke 和安装产物核对由主任务统一执行；Android/iOS 界面未修改。
