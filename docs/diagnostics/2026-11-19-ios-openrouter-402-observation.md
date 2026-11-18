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
