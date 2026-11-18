# MaiAgent 与 Codex 工具对齐审查

审查日期：2026-11-18。MaiChat 源码基线：`28815effc13f4dd6b9e43a6b304c446ede6e8f7f`。
Codex 使用本次检查时官方 main 的固定提交
[`ca21fe65d18a9b6326759fb4263c65dfbb645aa5`](https://github.com/openai/codex/commit/ca21fe65d18a9b6326759fb4263c65dfbb645aa5)，提交时间 2026-11-18 08:20:26 UTC。

这是静态源码审查：核对定义、注册、模型/平台条件和执行实现，没有调用产品工具或模型，
没有构建、安装依赖、修改产品代码或替换已安装应用。下述行为样例来自源码推导，尚未运行复现。
本地 Codex 工作树仍停在 7 月；本轮从既有仓库取得指定上游提交并导出无 Git 的参考快照，
没有将 Codex 或其他依赖接入 MaiChat 构建。

## 先看结论

建议先处理已有工具的正确性，再完善任务控制，最后按需求扩展生态。优先顺序：

1. **修正 `file_patch` 的定位及内容保留。** 当前接受部分 Codex 补丁语法，却有修改错误位置、改动未变上下文的风险。
2. **完善子 Agent 运行中收消息、重定向和等待。** 已有基本多 Agent；现有 `send_input` 默认可能直接遇到 Busy。
3. **统一图像能力检查。** 能力表与图像工具的型号判断不一致，需要先核实并统一；不能把静态风险报成已经复现的供应商错误。
4. **完善现有进程工具的 PTY、shell/workdir 和等待预算。** 后台进程、增量输出和 stdin 已有，不再新增一套重复执行器。
5. **补可被新输入唤醒的等待，按需补异步提问。** 问完以后可继续独立工作，等待时收到新指令可及时响应。
6. **按需补工具发现与 MCP。** 先做好能力目录及按需 schema，再考虑外部 MCP/插件；不是把几个名字加到工具表就完成。

`file_read/glob/grep/edit`、基本子 Agent、同步 `question`、`todowrite`、`current_time`、
网页搜索/抓取、图片查看及桌面后台进程都已有基础能力。名称不同不等于缺失。
暂停的图像/视频生成仍按产品策略处理，本次不启用，不发付费任务。

## 优先项的具体证据

### 1. file_patch：定位语义和未改内容

MaiChat 对任何 `@@` 行只结束旧块，没有保存后面的命名锚点。例如文件同时有 `first()` 和
`second()`，两者包含相同 `return 0`，补丁指定 `@@ def second():` 时，当前实现仍从前往后
匹配，可能改到 `first()`。应该支持该定位契约，或明确拒绝不支持的锚点，不能静默忽略。

另有三类同工具差异：纯新增的 Update 块当前可能插到文件头，Codex 是文件尾；
`*** End of File` 在末尾仅有空白差异时可能退回较早的同文位置；宽松匹配成功后，
当前实现会用补丁文本覆盖未改的上下文，并把混合 CRLF/LF 统一成一种换行。

MaiChat：`MaiAgent/AgentTools/MaiApplyPatchTool.cpp:151`、`:157`、`:298`、`:537`、`:552`、`:559`。
Codex：[命名锚点和纯插入](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/apply-patch/src/file_update.rs#L69)、
[EOF 搜索](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/apply-patch/src/seek_sequence.rs#L29)、
[保留上下文](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/apply-patch/src/file_update.rs#L124)、
[逐行换行信息](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/apply-patch/src/text_file.rs#L34)。

建议验收：只改变明确修改的片段；同文函数、文件尾、未改空白及混合换行分别做失败→修复对照。
不需要为此新增工具名称或第三方库。保留 MaiChat 对已存在文件的 Add 拒绝规则，
不自动照抄 Codex 的覆盖行为。两者也都不提供一般的多文件 I/O 失败回滚事务，不能虚报原子性。

### 2. 子 Agent：运行中补充及重定向

桌面已有 `spawn_agent/send_input/wait_agent/list_agents/close_agent`。
但 `send_input` 调用 `submit(MaiSendPrompt)`，默认 `rejectWhenBusy=true`，
运行中的孩子会返回 Busy。父任务刚发现的新约束不一定能送到正在工作的孩子。

MaiChat：`MaiAgent/AgentTools/MaiSubAgentTools.cpp:179`、`MaiAgent/Agent/MaiAgent.cpp:1032`、
`MaiAgent/include/MaiAgent.h:238`、`MaiAgent/Agent/MaiAgent.cpp:1646`。
Codex：[V1 排队/打断和 V2 消息/后续任务契约](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/multi_agents_spec.rs#L181)。

建议先复用已有 inbox/steering 核心，明确“仅投递”“触发新轮”“打断当前轮”的差别。
再考虑 wait-any、进度通知、上下文 fork、树形关闭和可恢复中断。移动端目前没有注册这组工具，
需按预算、生命周期和 UI 做产品决定，不能简单复制桌面注册语句。

### 3. 图像工具：能力目录与执行检查不一致

项目能力表将 `deepseek-v4-pro` 的 `supportsImages` 标为 false；
`view_image` 却只排除 `glm-5.3` 与 `local:`，仍会向该型号提供图片工具。
浏览器 screenshot 也有硬编码型号判断。用户直接附图入口反而读取统一能力目录，口径不同。

MaiChat：`MaiAgent/Agent/MaiCloudModelCatalog.cpp:17`、`MaiAgent/AgentTools/MaiViewImageTool.cpp:55`、
`MaiChat/desktop/src/MaiChatTools/DesktopBrowserTools.cpp:619`、`MaiChat/MobileAgentAdapter/MaiMobileHostTools.cpp:831`、
`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:1370`。
Codex：[执行前检查模型图像输入能力](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/view_image.rs#L103)。

这是源码可见的内部契约不一致，尚未请求供应商复现。应先核实实际型号能力，
再统一 schema 暴露、直接执行和图像结果的门禁。已有 JPEG 预览也有尺寸上限；
原图/高细节查看可作为后续能力，不能把现有 `view_image` 整项算缺失。

### 4. 进程执行：已有后台及 stdin，缺真实终端语义

`desktop_process_start/read/write/stop` 已有跨轮进程、输出 cursor、stdin 和停止能力，
能对应 Codex `exec_command/write_stdin` 的大部分基础任务；`shell` 也有保留输出和
`tool_output_read` 分页读取，不能说长输出只能丢弃或重跑。

剩余差异是普通管道不等于 PTY：交互式 CLI 的终端检测、终端界面及信号行为不同。
当前固定 cmd.exe 或 /bin/sh，缺少显式 shell/login/workdir、等待时间和输出预算参数。
嵌套启动 PowerShell 或用 cd 可以完成部分任务，所以属于可靠性和参数完善，不是完全不能做。

MaiChat：`MaiChat/desktop/src/MaiChatTools/DesktopProcessTools.cpp:83`、`:199`、`:232`、`:418`；
`MaiAgent/AgentTools/MaiShellTool.cpp:196`、`:293`、`MaiAgent/AgentTools/MaiToolOutputTool.cpp:47`。
Codex：[exec/terminal 参数](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/shell_spec.rs#L21)、
[有条件注册](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/spec_plan.rs#L1289)。

后续应把现有 MaiProcess 和 Qt DesktopProcessManager 的共同执行能力收敛到一个核心，
保留适合模型的入口；不为 `exec_command` 这个名字再造第三条执行路径。
iOS 外部进程受平台限制，不能照搬桌面终端承诺。

### 5. 等待与提问：减少无意义轮询和停等

MaiChat 的 `question` 已能提问并等回答，`current_time` 已能取 UTC 时间。
真正的新增能力是可被取消/新输入唤醒的原生等待，以及提问返回后继续做无依赖工作的异步提问。
例如等待设备就绪时接到新消息立即醒来，或询问输出格式时先继续检索资料。

MaiChat：`MaiAgent/AgentTools/MaiQuestionTool.cpp:88`、`MaiAgent/AgentTools/MaiTimeTool.cpp:17`。
Codex：[可提前唤醒的 sleep](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/sleep.rs#L108)、
[异步提问](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/request_user_input_async.rs#L131)。
两者在上游仍有开关/模型/模式条件。应复用当前问题身份、回答队列和取消机制，
不把 shell sleep 当成全平台实现，也不照搬上游最长 12 小时的范围。

### 6. 工具发现及外部工具生态

当前 `MaiToolRegistry::specs(model)` 过滤 internal/不可用项后，把所有公开 schema 交给请求。
没有延迟 schema、工具搜索或通用 MCP 连接管理。工具多时可考虑按需发现，
连接文档/数据库等第三方工具时才需要通用 MCP 动态导入。

MaiChat：`MaiAgent/AgentTools/MaiTool.cpp:200`、`:211`、`MaiAgent/Agent/MaiTurnRunner.cpp:476`。
Codex：[按条件启用工具搜索](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/spec_plan.rs#L424)、
[动态工具](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/dynamic.rs#L34)、
[MCP 工具适配](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/handlers/mcp.rs#L56)。

MaiChat 不是没有扩展点：普通 C++ 工具注册、宿主 dispatcher 都已有。
差异在于每线程外部 schema、来源/授权、延迟暴露和通用 MCP 生命周期。
`list_mcp_resources/list_mcp_resource_templates/read_mcp_resource` 是同一 MCP 资源层，
不宜拆成三个互不相关项目。iOS 也不能继承外部可执行 MCP server 的假设。

Codex 自有的 ChatGPT Apps 目录、连接授权、web.run 后端及 imagegen 后端并不会因源码开放
就变成 MaiChat 可直接使用的服务。先确定具体业务接入，再考虑市场/安装建议。
任何新增第三方库仍须用户明确指定或确认，本轮没有接入依赖。

## 逐工具族核对清单

以下是固定上游源码的工具族覆盖清单，不是某个用户模型当前可用工具的查询结果。
V1/V2 多 Agent 互斥，动态/MCP/扩展工具又取决于连接与策略，不能把这些名字相加当默认工具总数。

### 执行与文件

- **exec_command / write_stdin：部分对齐。** MaiChat 桌面端的 shell、四个 desktop_process 工具及 tool_output_read 已覆盖一次性/后台/增量读取/stdin；待补 PTY 和参数/预算。移动端按进程平台支持和实际注册分别核对。上游 ShellTool/UnifiedExec/TTY 各有条件。
- **apply_patch：已有同类工具，需修语义。** 对应 file_patch；JSON patch 包装不是缺口。命名锚点、纯插入、EOF、上下文与换行保留见前文。
- **read_file / list_dir / grep_files 这些旧名称：不列新增任务。** 当前 Codex core 注册入口没有这三个内置名称，读搜通常通过命令或外部工具完成。MaiChat 自己已有 file_read/glob/grep/edit 等独立工具。
- **Code Mode 的 exec / wait：可选。** Codex 可用 JS 组合调用已有工具并等待 cell 输出；受 CodeMode 配置约束，相关开关默认关闭。MaiChat 没有这一脚本外壳，不应为照名字引入新运行库。依据：上游 `core/src/tools/spec_plan.rs:1051`、`features/src/lib.rs:1329`。
- **wait_for_environment：当前非优先。** 上游 DeferredExecutor 条件下等待附加执行环境。MaiChat 现有单宿主模型不等同此多环境协议，需有产品场景再做。

### 计划、提问、时间、上下文与权限

- **update_plan：核心等价。** MaiChat todowrite 已有 pending/in_progress/completed 及单一进行项；差异是 Codex 有计划专用事件/UI，不必再建一套待办实现。依据：MaiTodoWriteTool.cpp:28；上游 plan_spec.rs:7、plan.rs:93。
- **request_user_input：核心等价、批量结构较弱。** question 支持单问题和选项；Codex 支持 1–3 个有 ID/标题/描述的问题并有模式限制。可丰富现有接口，不能只按名字报缺。
- **request_user_input_async：条件缺口。** 同步问题不能覆盖问完继续独立工作的语义；需跨端 UI/回答身份/生命周期设计。
- **send_message_to_user_async：条件差异。** 普通流式回复已有；上游另有独立异步用户通知并限制根线程/模型。先明确和普通消息、提问的职责再决定是否增加。
- **clock.curr_time：已有。** 对应 current_time，UTC 格式差异不构成功能缺失。
- **clock.sleep：缺少原生同类工具。** 重点是取消/新输入唤醒及有界等待，不是简单改名字或调用 shell sleep。
- **get_context_remaining / new_context：可选实验差异。** MaiChat 已有 token 窗口估算和自动摘要压缩；缺模型主动查询/显式换窗。Codex TokenBudget 默认关闭，new_context 本身不先做摘要，不应替代已有持久摘要。依据：MaiContextCompactor.h:17；上游 spec_plan.rs:1425、new_context_window.rs:14。
- **request_permissions：依赖权限体系，不能只补工具名。** 已有每工具审批不等于 OS sandbox；主动临时授权需要路径/网络/期限和撤销约束。上游也有独立开关。

### 多 Agent

- **spawn_agent：基础已有，参数部分对齐。** 当前可创建子任务并继承工作目录/模型；fork 会话历史、可选模型/角色是上游条件能力，不宜默认复制所有参数。
- **send_input（V1）/ send_message + followup_task（V2）：需优先完善。** MaiChat 已有 send_input，但运行中 Busy 与排队/打断/后续轮触发不同；见前文。
- **wait_agent：已有但较弱。** 当前主要等待一个孩子的最终文字；可补 wait-any、多目标、邮箱进度及新用户输入唤醒。
- **list_agents：已有基础，树/状态契约可再完善。** MaiChat 目前列直接孩子；不应因此称完全没有任务发现。
- **close_agent / resume_agent / interrupt_agent：部分。** MaiChat 有关闭/取消；需明确可恢复中断、关闭后再派活和后代树清理语义。Codex V1/V2 并不同时提供同一组名称。
- **CSV Agent jobs：不列当前 Codex 对齐要求。** 本次 Nov 快照的 Rust 注册/实现中没有 spawn_agents_on_csv/report_agent_job_result；不要从 7 月源码或旧清单沿用这个缺口。

### 发现、资源、插件与媒体

- **tool_search：条件缺口。** 上游需模型支持 search tool、策略允许和有 deferred 候选。MaiChat 目前全量 function schema；先建立统一能力目录，兼容不支持特殊协议的供应商。
- **动态 function/namespace 工具：部分。** 已有 C++ 扩展点，缺运行时由外部宿主按线程提供 schema/来源/调用结果的通用合同。
- **通用 MCP 工具：缺通用接入层。** 现有 curl/业务工具不等于 MCP inventory、授权、调用与结构化内容；按具体需求规划。
- **list_mcp_resources / list_mcp_resource_templates / read_mcp_resource：缺通用资源层。** 依赖同一个 MCP 管理器；本地文件、云盘已有，不应一起算缺失。
- **list_available_plugins_to_install / request_plugin_install：缺生态层、非直接移植。** Codex 的 Apps/Plugins/候选目录/账号授权门禁都需要满足；不代表模型可静默安装。MaiChat 应先有可连接能力目录再考虑市场。
- **WebSearch / web.run：已有基础、专用能力部分差异。** 两端已有 web_search/web_fetch 和浏览器操作。上游 hosted 搜索与 standalone web.run 是不同注册路径；image query、PDF 截图、统一引用 ID、天气/金融等是另外的服务合同，不能把上游私有后端当可复制的本地工具。依据：上游 hosted_spec.rs:14、spec_plan.rs:724、codex-api/src/search.rs:32。
- **view_image：已有，需统一能力检查，可选提高细节。** 当前有有界预览；原图细节和多执行环境选择是后续差异，多环境参数不适用于当前所有宿主。
- **image_gen.imagegen：已有供应商实现但产品暂停。** 不列未实现，也不恢复图像/视频生成；上游有套餐/供应商/视觉模型/账号后端权限条件。依据：上游 spec_plan.rs:821；MaiCreativeMediaSupport.cpp:127。
- **test_sync_tool：测试设施。** 上游仅在模型实验工具列表指定时注入，不是面向用户的产品补齐项。

## 注册与验证边界

MaiChat 入口：桌面 `MaiChat/desktop/src/agent/AgentController.cpp:314`，移动
`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:752`。二者并非调用完全相同的注册清单。
实际发送给模型前还经过 `MaiToolRegistry::specs(model)`、internal 和 isAvailableForModel 检查。
本轮没有读取用户账号、凭据、当前模型配置或现场数据库，不给出“当前实际启用数”。

Codex 入口：[spec_plan.rs](https://github.com/openai/codex/blob/ca21fe65d18a9b6326759fb4263c65dfbb645aa5/codex-rs/core/src/tools/spec_plan.rs#L1232)。
已核执行器、文件补丁、计划/提问、时间/上下文、两代协作、Code Mode、MCP/动态发现、插件和托管媒体分支。
工具有实现、被注册、被模型声明和实际可成功执行是四个不同口径；本轮前三者用源码核对，
没有把静态分析当成产品实测或网络服务成功。

## 与旧审查记录的关系

旧版本文档的基线为 bf99cda1d229，不能作为本次现状清单继续使用。
当前已能看到 Responses、usage/context 限制及自动压缩等实现，不能重复报“只有 Chat Completions”
或“没有上下文预算”。旧记录保留在 Git 历史，本次文档以以上两个冻结提交为准。

审查细分证据与无 Git 的上游快照留存在 `.cache/codex-tool-audit-20261118/`；
源码读审只更新本文件，不替用户决定工具 UI 分组或自动接入第三方库。
