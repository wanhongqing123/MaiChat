# Responses HTTP/2 framing 错误与 Codex 恢复对照

## 请求、现场与范围

2026-11-28，用户在 Mac 创作中心发送后看到红色错误：
`curl: Stream error in the HTTP/2 framing layer`，并明确称使用 gpt-6.1-sol，
要求查看 Codex 如何处理类似错误。截图没有 HTTP 状态、curl 数字错误码、
重试过程或本次运行 exe 的版本/SHA。之前其他轮次曾核过现装 4.0.2，
不能用那次信息代替这次现场绑定；截图上下文计数也不能证明传输失败的原因。

本轮按 Mac 分工只读审 Core 与参考实现，不修改其正在实现的文件拖放 UI，
不改 Core、不读取用户 DB/请求正文/密钥，也不请求真实供应商或变更后台。
仅有本诊断记录发生源码仓库写入。

## 可复核基准

- MaiChat Root HEAD：`9b34810f1d3505285046ec4a16e753b0dacadfc6`。
- 已发布 4.0.2 源：`74470f39c5fc3b6c088bd87770f9bfb2c864dc42`。
  `MaiResponsesClient.cpp`、`MaiModelRetry.cpp`、`MaiModelClient.h` 三件
  当前 Git 内容与该发布源完全相同。
- 实际使用的本机定制 Codex 源：
  `E:/OpenSource/multi-ai-code/third_party/aicli/codex`，HEAD
  `6e09a1b58f759848942e3d5b7653ab714f045ada`。
  结论限定这份源码，不把它当作所有 Codex 版本的行为保证。
- 静态抽取及摘要在 `.cache/responses-http2-audit-20261128/static-source-receipt.json`。
  本轮未运行下列测试源码，不将阅读断言说成执行通过。

## 明确差异：curl 92 未列入恢复分类

仓库两份 vendored curl 的 `lib/strerror.c` 都将截图中的完整错误文案映射到
`CURLE_HTTP2_STREAM`；`curl-apple/include/curl/curl.h` 定义该值为 92。
同头文件的 `CURLE_HTTP2` 为 16，是相关但不同的错误码，不能混用文案。

`MaiAgent/Agent/MaiResponsesClient.cpp:1044` 的 `isRetryable` 只有：
COULDNT_CONNECT、COULDNT_RESOLVE_HOST、OPERATION_TIMEDOUT、RECV_ERROR、
SEND_ERROR、GOT_NOTHING；没有 HTTP2_STREAM，也没有 HTTP2。

调用链为：

1. `MaiResponsesClient.cpp:1282` 将传输白名单结果及 HTTP status 交给
   `MaiModelRetry::isTransient`。当 status 为 0 或正常 2xx，curl 92 不会被标为瞬时故障。
2. `:1284` 的 `advise` 仅对被分类可恢复且网络 generation 未变化的错误附加 retry advice。
   网络变化抑制自动重投是现有独立边界，不应借本次分类修复绕过。
3. `:1306` 起仍将 curl 失败作为 Network 错误返回，文案来自 curl_easy_strerror。
4. `MaiTurnRunner.cpp:1523` 无 retry advice 时直接结束恢复循环，不能仅凭 Network 类型自动重试。

因此存在可验证的分类缺口：对于 status 0/2xx 的 HTTP/2 stream 失败，
已有恢复循环没有机会接手。若实际 HTTP status 是 408、429 或部分 5xx，
仍可能由 HTTP 分类得到 advice，不能从截图反推出“本次绝对没有尝试过重试”。
该缺口解释客户端为何可能直接显示错误，不解释 HTTP/2 本身为何失败。
缺乏现场状态码、响应阶段及传输事实，不能据此认定供应商、代理、Wi-Fi、后台或请求大小的责任。

## Codex 的两层处理

以下路径相对 `codex-rs/`：

| 层次 | 参考路径 | 已核行为 |
| --- | --- | --- |
| 请求建立 | `http-client/src/transport.rs:126`；`codex-client/src/retry.rs:28` | reqwest 非策略/连接/超时错误映射 Network；RetryOn 对 Network/Connection/Timeout 按 retry_transport 与次数处理 |
| Provider 请求配置 | `model-provider-info/src/lib.rs:501` | retry_transport=true、5xx 开启，request 默认最多恢复 4 次；429 在此配置未开启，不能泛称每一层都重试所有429 |
| 已建立 SSE | `codex-api/src/sse/responses.rs:628` | SSE 读取错误转 ApiError::Stream；没有 response.completed 就 EOF 也转 Stream；策略拒绝保留 typed 错误 |
| 统一分类 | `codex-api/src/api_bridge.rs`；`protocol/src/error.rs:481` | Stream 映射 CodexErr::Stream，retry_policy 为 Backoff；终态拒绝、额度等保留终态规则 |
| 会话恢复 | `core/src/responses_retry.rs:72` | 次数预算、退避/jitter、服务方等待期限、Reconnecting 事件及耗尽后的原错误 |
| 重建请求 | `core/src/session/turn.rs:2060` | 重试后取 clone_history、附加真实已执行工具事实，再构造下一次采样请求 |

`model-provider-info/src/lib.rs:74` 和 `:553` 的 stream 默认恢复上限为 5 次，
可被 provider 配置覆盖，并非无限重连。另有只针对 ConnectionFailed 的特性开关路径，
不能把它泛化到所有流内 HTTP/2 错误。

`core/tests/suite/stream_no_completed.rs::retries_on_early_close` 明确配置一次 stream retry，
让第一次 SSE 缺完成事件、第二次完成，断言两次请求及 TurnComplete。
`protocol/src/error_tests.rs` 检查退避与服务方建议不会让终态错误变成可恢复。
这些是已阅读的源码证据，本轮没有执行 Rust 测试。

`responses_retry.rs:135` 的 transport fallback 明确是 WebSocket→HTTPS，
`protocol/src/error.rs:473` 也如此定义；这不是 HTTP/2→HTTP/1.1 的等价处理。
源码对照不足以支持全局设置 HTTP1_ONLY、关闭 TLS 校验或把模型请求移到后台转发。

## MaiChat 已有的恢复能力

`MaiModelConfig` 默认 maxRetries=2、初始退避 250ms；实际宿主配置可覆盖。
`MaiModelRetry` 指数退避封顶 60 秒，并保留有效 Retry-After 截止。
这与 Codex 默认参数不同，但不能只复制数字而忽略两层预算的差异。

`MaiTurnRunner::buildRequest` 设置 callerManagesRetries=true，
由 Runner 统一持有 Agent 恢复预算、重建上下文；直接 stream 调用仅在尚未交付输出时自行重试。
失败后普通 HTTP2 连接 lease 以不可复用结算并 curl_easy_cleanup，
不是有意把已失败的同一 easy handle 放回连接池。

原生已完成 item 由 onOutputItemCompleted 独立记录，失败增量保持草稿；
可执行工具调用与整批终态校验分开。已有 `MaiModelClientTests.cpp:3505` 附近覆盖
半截 SSE、EOF/停滞/server_error 后恢复及失败草稿不入下一请求，`:3570` 覆盖唯一预算及永久拒绝。
尚未找到这些测试对真实 HTTP/2 RST_STREAM/错误 92 的定点覆盖。
“已有恢复”与“所有传输错误均已正确接入恢复”是不同结论。

## 最窄后续修复与验证建议

本轮没有修改实现。下一步可将 HTTP2_STREAM 纳入受限瞬时传输恢复，
同组核查 HTTP2 分类，继续使用现有唯一预算、取消、网络 generation 和 Retry-After 约束。
不重新提交整条用户轮次，不通过增加全局重试次数或固定 HTTP/1 来替代分类修复。

需要自有 HTTP/2 fixture 或可靠传输注入的同用例 RED/GREEN，至少验证：

- 无输出发生 RST_STREAM/错误 92 后，在预算内重连，耗尽保留原错误。
- 半截正文后中断，失败草稿不进入下一请求，已确认完成项保留且不重复。
- 不完整工具参数不执行；已执行工具不因恢复重复执行。
- 已有完整终态之后的传输尾部异常不得造成无依据的重复采样。
- Stop 可取消等待；Steer 的新输入进入重建请求；网络 generation 边界保持。
- 证书/鉴权/额度等永久错误不因白名单扩张而自动重投。

仅采集必要的非敏感事实（curl 数字码、协商 HTTP 版本、status、响应阶段、尝试次数及字节计数），
不记录认证材料或用户正文。重复 POST 及未返回 usage 的计费归属不能凭恢复成功推定已结算。
需要真实现场绑定才能继续判断链路失败的原因；本轮没有模拟成“已修复/已真机验收”。
