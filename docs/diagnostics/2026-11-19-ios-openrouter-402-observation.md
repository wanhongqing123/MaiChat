# iOS OpenRouter HTTP 402 现场观察

## 复现与期望

2026-11-19 收到用户的 iOS 创作中心截图：浏览器任务先读取网页，后续一次请求没有可显示正文，底部显示 HTTP 402。期望是保留任务上下文，并区分短暂的请求预算占用、单请求估价过高和其他额度限制，给出相应提示。本记录不保留对话正文。

源码观察基线为 `fcf0c3d8d5e014126dddcffaf364c0939f6e44ef`，产品源等于 `70ee22f62251740bf53fd684a4aa6baef3409734`。此前独立安装回执确认真机为 3.0.1(229)、普通启动 PID 76340；本次没有再次读取设备进程或用户会话，截图本身不作为运行提交的独立证明。

## 已确认的事实

- 截图错误正文来自 OpenRouter，描述当前执行中或近期结算的请求与本次请求合计超过可用额度。它不能用于判断 GLM 账户余额。
- `MaiModelRequest.maxOutputTokens` 默认是 0，普通 Runner 主请求没有把它改为目录上限。`MaiResponsesClient` 仅在非零时发送 `max_output_tokens`。
- Fable 目录中的 128000 是模型容量。压缩器会用目录容量或保留预算计算输入空间，但明确不改变普通请求的输出策略。不能据此声称 App 显式申请了 128000 输出 token。
- 229 基线的 HTTP 错误仅提取 `error.message` 和字符串 `error.code`，没有保留 `metadata.reason`、`metadata.limit_source`；响应头没有保留 `Retry-After`。HTTP 402 最后被映射为 `Protocol`。该请求上下文销毁后，不能从 `MaiError` 的 code/message 恢复这些字段。
- 仅进行一次不生成模型内容的当前 Key 状态查询，HTTP 200，确认当时 Key 自身剩余限制非零。没有保存 Key、账户标识或原始响应。这个接口结果不能重建截图发生时的账户余额、并发占用或结算状态。

## 官方语义及本次边界

[OpenRouter Limits](https://openrouter.ai/docs/api_reference/limits#in-flight-spending-budget) 说明，即使余额为正，请求估价的预留预算也可能不足。未指定输出上限时，估价使用供应商未在该文档公布数值的单请求固定上限。

- `in_flight_budget_exhausted` / `openrouter_in_flight_budget`：执行中或近期结算的请求占满预算；应遵循 `Retry-After`。
- `weight_exceeds_budget` / `openrouter_credits`：单请求估价超过整个预算；单纯等待不一定有用。

官方前一分支示例的英文正文与截图一致，但本次缺少实际 metadata，不能把文字匹配当作已取回的分类证据。不据此自动充值、更换 Key、降低全局输出上限或重发模型请求。

## 共享候选的最窄改动

共享 Responses owner 已用本地假 HTTP 做失败对照，再保留白名单分类与有界的数字等待秒数，沿现有 `MaiError.message` 显示。缺 metadata 时保持未知；不新增原始错误 body、请求正文、思考内容、密钥或整组响应头的诊断记录；不新增自动重试。另补已收到 HTTP 402 后连接中断的守卫，避免沿旧传输错误重试路径再次发出请求。最终共享 v2 的 Responses 源 blob 为 `bdd19b0b18e6c4a5f692215ce5558f2c5f95c775`；本端只消费并编译该源，不并写共享解析。

本记录只完成现场和源码定位，不宣称已复现供应商当时的预算状态，也不把尚未安装的后继错误提示计入 229 的验收。

## 后继离线复现：402 头后 RST 丢失状态

此处是独立的错误优先级缺口，不是对原完整 402 截图或余额的复现。以产品提交
`f785e2c2bae85129774686125828d85b409fa82f` 为生产基线，在 Windows 实际源码的
`MaiModelClientTests --openrouter-402` 中保留原 RST 用例并收紧断言：自有 loopback
服务器先完整发送 402 状态及响应头，声明更长正文，发送未闭合 JSON，再以
`SO_LINGER{1,0}` 关闭。正文中虽然出现完整的已知预算字段文字，文档仍不可解析，
不能用字符串搜索补出来源。

Retry-After 只测试 `7`、`86400`、`86401` 和合成非法头值。前两者必须保留精确的
有界数字 token，后两者不得回显。请求必须仍失败，返回已知 HTTP 402 的 Protocol；
不得猜成 in-flight、发布正文/思考/工具/续接状态或重试。四个场景在旧生产上共
10 个断言失败（四个错误类型、四个 HTTP 前缀、两个合法等待秒缺失）；其他完整
402、未知/外来 metadata、单请求和不泄漏断言均通过。

唯一生产改动在 `MaiResponsesClient.cpp`：已知状态 402 不再经过通用传输错误出口而
丢成 Network；随后仍从 HTTP 错误出口返回，不会进入成功终态。仅传输失败且未能
解析预算分类时，附加已校验的独立数字等待头。完整未知 metadata 的既有行为和其他
HTTP 状态保持原样。生产供件 blob 为 `a2018d445dabc1ac1fb5fb82629ae459c5cd36e1`，
LF SHA256 `335f80115f910b3701c3669a5a6ea8ff29dbc84be739f3aa16053383171aa308`。

同一最终测试已 selector GREEN，正常完整 ModelClient 和其余 Core 共十套回归全部
通过（50.04 秒，其中 ModelClient 33.94 秒）。独立只读审查在本限定范围内未发现
P1/P2。证据在 `.cache/openrouter-402-rst-status-20261119/`。本段 Windows 离线验证
没有真实供应商调用、读取用户数据库、调整输出 cap、缓存、Key 或并发策略；
该变更不证明或解除供应商费用限制，已安装手机的行为也不因源码提交自动更新。
Windows 正常 `maichat` 构建及部署门禁通过，核对三产物、114 个 PE 导入和 Qt 插件。
未替换、安装或启动用户现装程序，未发布 release/tag。
