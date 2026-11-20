# Responses 线协议与 DeepSeek 主模型

`MaiModelClient` 是中立接口，调用方通过 `MaiModelConfig::wire` 选择
`ChatCompletions` 或 `Responses`。两者都接收完整的 `MaiModelRequest`，
模型、历史、工具和图片的线格式转换只在客户端边界发生。

DeepSeek V4.1 Flash 的 API 模型 ID 是 `deepseek-flash`，官方地址是
`https://api.deepseek.com`，Responses 请求发到 `/responses`。DeepSeek 的该接口
**无状态**；不能依赖 `previous_response_id`，每轮都须在 `input` 带完整历史。
资料：[V4.1 Flash 公告](https://api-docs.deepseek.com/zh-cn/news/news260910/)、
[Responses API](https://api-docs.deepseek.com/zh-cn/api/create-response/)、
[Responses 指南](https://api-docs.deepseek.com/zh-cn/guides/responses_api/)。

| 中立表示 | Responses 请求或事件 |
|---|---|
| 基础指令 | 顶层 `instructions` |
| 用户、系统、助手消息 | `input[]` 的 `message` item |
| 用户图片 | `input_image` 内容块；本地图片按原有 2 MB 请求预算准备 data URL |
| 助手工具调用 | `function_call` item，保留 `call_id`、名称及完整参数 |
| 工具结果 | `function_call_output` item，用同一个 `call_id` 配对 |
| 已保存的思考内容 | 独立 `reasoning` item；Chat 路径不把它混入正文 |
| 工具清单 | 顶层扁平的 `tools[]`，每项含 `type=function`、名称、描述和参数 Schema |
| 增量正文、思考、参数 | `response.output_text.delta`、`response.reasoning_text.delta`、`response.function_call_arguments.delta` |
| 请求结束 | 仅 `response.completed` 为成功；`response.incomplete` 和 `response.failed` 返回错误 |

流式事件可能按任意字节边界拆分。客户端按行重组 SSE，只向上层追加增量；
工具参数等完整后才发起工具调用。结束事件没有 Chat 的 `[DONE]`，
连接提前关闭不能当成功。模型请求的恢复由两种调用契约明确分工：

- `MaiAgent` 内的主采样、工具后继、无工具补答及上下文摘要由 `MaiTurnRunner`
  统一持有恢复预算。发生可恢复断流后，留在同一会话和 turn，重新构造请求；
  已接受的补充输入及已执行工具结果随历史回传。
- 直接使用 `MaiModelClient::stream` 的调用方默认仅在尚未交付内容时重试，
  避免无法撤回增量的普通调用收到重复正文。Agent 设置
  `callerManagesRetries=true`，此时适配器只尝试一次，返回结构化恢复建议，
  不在适配器与 Agent 两层分别重试。

默认首次请求之外最多恢复两次，指数退避从 250 ms 开始、封顶 60 s；
有效 `Retry-After`（数字秒或 HTTP 日期）可要求更长等待，解析上限为一天。
等待检查取消及补充输入；新输入被消费后仍遵守原截止时间。
连接/接收/发送故障、传输/模型进度超时、缺协议终止事件、HTTP 408/429/5xx
及明确的流内 `server_error`、`rate_limit_exceeded`、`overloaded_error` 可恢复。
鉴权、402、明确硬额度拒绝、长度或内容过滤、未知协议错误保持失败；
不自动换模型、换供应商或切到付费路由。

每次采样的正文与推理从第一次持久化起标记 `modelDraft=true`，完整响应成功
持久化后才确认。失败/取消草稿仍可本地显示和回查，但普通上下文与摘要来源
都排除它；SQLite 重开后仍保持该边界。失败响应的工具调用不会派发，
已执行工具不会被恢复层重新派发。恢复不是工具副作用的全局 exactly-once 保证：
若模型在新的成功响应里主动请求一个新调用，仍按正常工具与审批规则执行。
仅成功持久化的响应产生 `MessageTextCommitted`；恢复进度通过独立
`ModelRequestRetrying` 事件发布，不创建新 turn，也不伪装成助手正文。
这不是对半截文本逐字节续传，而是从最后已确认历史重新采样。

确定性验证使用本地 HTTP/SSE 假服务：两个 wire 的半截正文 EOF、
输出后停滞超时、流内临时错误、429 等待及硬拒绝、恢复预算、工具结果回传、
取消/补充输入、草稿重开与摘要隔离；CLI 端到端还覆盖真实进程内的重连状态与恢复。

iOS 的 DeepSeek Key 存在独立 Keychain 项；桌面与 Android 也使用独立于 GLM 的
配置项。切换模型不复用另一服务商的 Key。用户在应用设置中填写一次 Key 后，
模型菜单可在 `glm-5.3`、`glm-5.3-flash` 和 `deepseek-flash` 之间切换。

本地模拟服务覆盖：SSE 任意分片、正文/思考去重、工具参数增量与完整结果、
完整历史及图片序列化、缺失结束事件和不完整响应。使用用户提供的 Key 做了
两次受控实时验收：普通 Responses 回复 `OK`，以及“模型调用 echo → 回传结果 →
模型继续回复 `OK`”。Key 未写入仓库或测试日志。
