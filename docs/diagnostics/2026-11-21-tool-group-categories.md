# Desktop 工具组分类摘要

- 时间：2026-11-21，Windows；起始源码 `38d25b9d7a41ad29128e95289f44937c676ca452`。
- 复现：在 Desktop 创作中心本地或远程会话显示一组连续工具调用，默认折叠；分别准备全内置、全 shell 和混合且含失败的同组工具。点击摘要展开、再次点击折叠，再将窗口缩窄。
- 最终分类依据：产品固定五项名单将可执行任意命令/脚本的 `shell` 与受管进程生命周期工具归入 shell：`shell`、`desktop_process_start`、`desktop_process_read`、`desktop_process_write`、`desktop_process_stop`。这是产品约定，不是泛化的进程推断规则。`ssh_exec` 是明确的产品例外，保持内置（libssh2 不创建本机进程）；`curl`、`openssl` 通过进程内 `mai_*_execute`，也归入内置。`desktop_browser`、`computer` 及其余宿主工具按同一固定契约归入内置，不扩大名单。
- 预期：混合组显示 `内置工具调用 6 次 · shell 调用 2 次 · 已完成`；纯内置显示 `内置工具调用 8 次 · 已完成`；纯 shell 显示 `shell 调用 2 次 · 已完成`。保留原状态优先级，状态始终在末尾；共享控件已添加命令图标和展开箭头，不在标题函数重复添加。缺名称不伪造分类，继续显示旧摘要。
- 验证方案：先在实际 `RemoteAgentPanel` 上通过自有 JSON 历史夹具断言上述标题与折叠行为，观察新增分类断言失败，再修改生产代码并用同一用例回归；保留无工具名调用及状态优先级覆盖。截图仅展示合成卡片，不执行真实 shell/SSH/网络操作。
- 初始观察：本地 `ToolGroupCard::setSummary` 及远程两处摘要只收集 states；共享 `agentToolGroupTitle` 没有工具名信息。共享 widget 已在 `refreshTitle` 添加命令图标及展开箭头，因此格式化函数不应再添加箭头。
- 实现计划：共享函数新增默认空的 tools 参数并集中保留精确五项判定；本地及远程各摘要调用点按同一组收集名称；缺名或数量不匹配时返回旧摘要。先不改变布局与展开状态机，以实际窄窗回归决定是否需要额外处理。
- 回归方案：`RemoteToolDetailTest::groupSummaryCategories` 在实际 `RemoteAgentPanel` 上覆盖完整格式、`ssh_exec`、精确名称边界、混合失败、缺名与窄窗；原状态优先级与详情刷新也断言。`AgentPanelSessionTest::toolGroupCategoriesStayWithinTheirOwnGroup` 用自有 SQLite 历史验证三个由正文分隔的工具组，不跨组累计。
- 格式 RED：计数前置的新断言在旧格式化函数上失败，纯内置用例实际为 `工具调用 8 次 · 已完成 · 内置`，预期为 `内置工具调用 8 次 · 已完成`；调用次数及状态正确，证明新断言能拦住错误格式。最终五项名单正确性由下面最新 GREEN 验证。
- 远程 GREEN：2026-11-21 22:01（Asia/Shanghai），正常缓存 `MaiChat/desktop/.cache/build-qt6-ui-20261109` 以 MSVC 2022 构建两个测试目标成功；运行完整 `remote_tool_detail_test` 返回 24 passed / 0 failed / 0 skipped，见 `tool-group-green-20261121.txt`。测试使用 windows QPA，未设置 offscreen。该次运行 EXE 的 SHA-256 为 `435B80E4CF34DBC5D6CD74D066ED5152645F8D22C524CEC4C3789619334F0481`。
- 截图检查：`tool-group-categories-20261121/` 中 `pure-built-in.png`、`pure-shell-executors.png`、`mixed-failure.png`、`mixed-completed.png`、`ssh-exec-is-built-in.png` 已逐张查看；560×700 逻辑像素（150% 缩放，PNG 为 840×1050）标题完整、状态位于末尾，只有控件原有的一枚图标及箭头，无裁切。点击展开/折叠保持同一标题，且仅组展开没有触发详情读取。
- 本地 GREEN：2026-11-21 22:06（Asia/Shanghai），`agent_panel_session_test toolGroupCategoriesStayWithinTheirOwnGroup consecutiveToolsShareDisclosureWithoutChangingReceipts` 返回 4 passed / 0 failed / 0 skipped，见 `tool-group-local-green-20261121.txt`。构建后的 EXE SHA-256 为 `427116EB50F4921DB3DE308A649858C9D7EBE990901D97B82B03B4816EAFF385`。本地三组截图 `tool-categories-local-owned.png` 已查看，分组次数分别为内置 3、内置 1 + shell 1（有失败）、shell 1，均未跨正文边界累计。测试构造面板时有一条 `QFont::setPointSize(-1)` 警告，截图字号正常；没有为该现有面板警告扩大本次修改范围。夹具曾因片段 ID 字典序不单调重放成一组，已修正夹具并以同一用例通过，生产分组逻辑未改。
- 验证边界：上述是实际桌面 Qt 组件配合自有合成记录，不是安装版正常 App 的完整业务验收；没有执行真实 shell、SSH 或网络请求。正式 App 构建、部署及安装产物核对由主任务统一执行。未修改或运行 Android/iOS。

iOS 只读检查：`MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift:2329` 的 `toolSummary` 按工具名计数，`:2341` 的 `toolDisclosureTitle` 当前使用 `工具调用 N 次，状态 · tool_name ×count`，没有内置/shell 分类。`:2346` 的 `toolGroupStatus` 对全取消显示“已取消”，其余优先级为运行中、等待授权、有失败、部分取消、已完成。本次没有修改 iOS，也没有运行 iOS 模拟器或真机。
