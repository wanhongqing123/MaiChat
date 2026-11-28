# 命令结果的模型上下文与显示

## 现场与预期

2026-11-28，用户在 macOS 4.0.2 创作中心展开 `exec_command`，看到整段
`chunk_id/exit_code/output` JSON，其中换行显示为 `\n`，询问这些内容是否进入模型。
对照发布源 `74470f39` 与本次工作基准 `6adfe4c02` 的调用链，不读取用户会话或抓取真实请求。
预期是保留命令结果的模型含义，在界面展开时显示可读命令、状态和原有输出。

## 查证

`MaiExecCommandTool::renderSession` 将命令输出与状态、恢复句柄封装为 JSON。
`MaiTurnRunner::projectContextToolOutput` 在预算内保留原文，超出预算才进行投影；
Responses 客户端将其作为 `function_call_output.output` 发送。首次发送的表示会冻结，
后续上下文沿用，不因本次显示修改而重写已发送历史。界面的 `cmd:` 标签本身是显示层生成的。

因此截图中的命令结果确实进入模型上下文。输出和退出状态用于后续判断，仍运行任务的
`session_id`、较长输出的恢复句柄也有实际作用，不能以隐藏界面为由删除。
定制 Codex 的 Code Mode 同样存在 JSON 结果；其 TUI 会另外组织紧凑摘要和可读输出。
默认折叠不表示结果未发送。普通 Codex 工具还存在可读文本形式，不能将两种路径混为一谈。

另有每批工具后附加的 `tool_execution_observation` 执行事实与指导文本，
它不重复 stdout，但部分状态信息与 receipt 重叠；本次未改这项模型协议。

## 修改与证据

仅调整 `AgentChatPanel` 的显示：识别 `exec_command/cmd` 与
`write_stdin/session_id`；展开时解析 receipt，将 JSON 转义换行还原为实际输出，
把执行状态与恢复句柄分行显示。保持原显示长度上限；较长 JSON 延迟到展开后解析，
避免先截断 JSON 导致无法解码。Windows 路径中的字面反斜线不作二次转义。
模型载荷、原始结果及持久历史均不变，不增加按钮。

`.cache/command-receipt-ui-20261128/` 保存同一测试红绿：旧 shell 行通过、
四项新增场景先失败；修改后五行均通过，旧懒加载与连续工具回归通过。
使用正常 MSVC 构建和 Windows 测试包装器、独立 profile/offscreen，
配置真实 Windows 字体后生成并人工查看 `images-green-02/command-receipt-exec-completed.png`。
截图中输出为真实换行，可读命令和状态分开显示。

Windows 验证属于独立测试宿主，不代表 macOS 新版已安装；用户现装 4.0.2 与公开资产未替换。

后续 Header 窄补丁与本项合并后，正常 App/组件重编通过，相关七个 slot 合计
13 passed、0 failed，包含五行 command receipt、原懒加载与连续工具及 Header 状态回归。
Root 重新查看 `images-final-header/command-receipt-exec-completed.png`，
没有因 Header 聚合回退结果格式化逻辑。
