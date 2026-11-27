# 上下文增长、费用与 Codex 用量计算对照

核验日期：2026-11-24，Windows。本文记录排查证据，不代表已经完成用量计算改造。

## 复现和预期

用户在同一开发会话中使用 Opus 5.5 和 GLM，观察到请求费用增长快、GLM 连续触发压缩；
同等主观工作强度下，Codex 很少连续压缩。预期是保留对话正文，优先逐步压缩工具输入输出，
每次尽量只释放约 20%，并用可靠的当前窗口用量决定是否压缩。

核验边界：仅只读目标会话的统计和代码，不访问其它会话正文，不请求收费模型；
没有改数据库、停止程序或安装候选包。消息频率不是 token 增长的充分度量，
但不能据此忽视用户提供的反例，更不能把实际供应商用量全部解释成界面误差。

## 源码与实际运行构建

- 工作区 HEAD：`3c4a739a49d5273bc6f3cb76b74ee357b3d9d75e`，另有未提交修改。
- Codex：从官方 origin/main 获取 `d4af2452f7b5e39aadd7abd75b6e141059a4cc5d`。
  只 fetch/show，未切换或清理原有 Codex 工作区。
- 现场程序：PID 1320，`D:\Program Files\MaiChat\maichat.exe`，启动于 2026-11-24 00:27:23。
  EXE SHA256：`cd295edb7aab7976974e978579b0e5a3eab50f379ff47c6d79bedbb52c9a295f`，
  是公开 3.2.2。正文保全候选包没有安装到该位置。

## 已确认的差异

### 窗口计算没有完整采用真实 usage

`MaiModelClient.cpp::estimateText` 对 ASCII 段按约三字符一 token 估算，
对非 ASCII 标量按其 UTF-8 长度计数；常见中文因此按每字三 token 计入。
它是保守估算，不是实际 tokenizer。

`MaiTurnRunner.cpp::requestCompletion` 收到供应商 inputTokens 后，只把
`usage.inputTokens / inputEstimate.tokens` 用于向上调整 multiplier。
multiplier 从 1 开始，不会用真实较低用量向下校准。
实际输入计数没有作为下一次请求的历史基准持久化；`MaiContextStatus::after` 仍是估算。
这种误差会作用于压缩触发，而不仅是显示百分比。

Codex 的 `context_manager/history.rs::get_total_token_usage` 则以最近一次服务端
`last_token_usage.total_tokens` 为基础，只估算尚未被该 usage 覆盖的新增 item；
并根据 reasoning 是否已计入做额外处理。累计费用用量和最近请求用量分开维护。
这不是把所有请求的 input token 累加后当作窗口，也不是每次只用完整历史的粗估替代真实 usage。

参考：[Codex 固定版本源码](https://github.com/openai/codex/blob/d4af2452f7b5e39aadd7abd75b6e141059a4cc5d/codex-rs/core/src/context_manager/history.rs#L1234)。

### 工具结果确实是主要的原始数据来源

目标会话同一只读事务的统计如下，单位为 UTF-8 字节，不是 token，也不是当前 wire 请求：

| 类型 | 原始字节 |
| --- | ---: |
| 用户及助手正文 | 110465 |
| 工具输入 | 336141 |
| 工具输出 | 994212 |
| 工具错误字段 | 6415 |
| 可见思考文本 | 536663 |

例如 shell/shell_input 共 288 次，输出合计 551011 字节，其中 `[shell_result]` 后的
结构化后缀合计 141172 字节。Builder 还会增加独立的执行观察说明。
应继续核减重复的宿主说明和非必要元数据，保留状态、结果、退出码、执行影响和原文定位。
不能据此删除聊天正文或伪造工具成功。

仅缩小单条输出上限不能解决全部问题：已保存的模型工具表示最大 41220 字节，
所有这些表示超过 40 KiB 的部分合计仅 260 字节。把 64 KiB 改成 40 KiB，
对此现场几乎没有效果。

Codex 在工具结果写入模型历史时按 policy 截断，且工具可请求的输出预算受到模型历史策略约束。
但 Codex exec 默认也可请求 10000 token，不能笼统声称“Codex 的 shell 默认只输出很少”。

参考：[Codex 工具预算](https://github.com/openai/codex/blob/d4af2452f7b5e39aadd7abd75b6e141059a4cc5d/codex-rs/core/src/tools/context.rs#L549)。

### 思考文本、续接状态和实际计费不能混算

536663 字节是整个数据库原史中的可见思考文本，不是每次请求都携带的有效思考量。
当前 checkpoint 之外尾部的思考文本为 166735 字节，且模型切换会影响续接状态是否可回放。

数据库还保存 Responses 的原始 output 快照。原史 OpenRouter/Opus 快照合计 719442 字节，
其中 signature 字符串合计 408224 字节；GLM 快照合计 904854 字节。
这些是归档统计，不能直接相加得出当前模型 token，也不能与可见思考统计重复相加。
现有估算在匹配 continuation 时估其 payload，跳过同消息的显示投影，未发现这里重复计入两份。

协议签名、加密状态与普通可见思考草稿不同；不能为省空间把它们当普通日志删除。
当前产品模型目录默认走 Responses。Chat Completions 的 `clear_thinking` 文档，
不能直接证明本现场 Responses 会忽略历史 reasoning；需要该协议及实际 usage 的证据。
因此“历史思考一定被供应商忽略”不是此次成立的根因结论。

## 用户截图能够与不能证明的事情

OpenRouter 截图可见 11 次请求，每次输入约 38.9 万至 40.3 万 token，合计费用约 $1.081。
从可见最早至最新一条，输入仅增加 13152 token。
每轮带着已有上下文重新请求，会反复产生计费输入；不能把各次输入累加后说窗口新增了几百万。
也不能否认这里确有约四十万 token 的实际输入。

列表截图没有显示 cached_tokens/cache_write_tokens，无法据此认定缓存未命中。
Responses 编码器已为目标 Opus 型号发送 `session_id` 与顶层 `cache_control`；
不能把 Chat Completions 中缺少同字段当作当前产品的故障根因。
是否命中仍须核验实际 usage 明细，启用缓存不保证每次命中。

参考：[OpenRouter 缓存与 usage 字段](https://openrouter.ai/docs/guides/best-practices/prompt-caching)。

目标会话最早记录到的 GLM 估算已约 56.2 万，而不是空会话从零开始。
现场后续约 42 万的界面数值也标记为 exact=false。
没有持久化实际 GLM input usage，不能据此认定其真实输入已经接近 1M，
也不能仅凭粗估算证明其真实输入很小。

## 修正方向与验证要求

1. 建立服务端 usage 覆盖边界：最近一次实际用量加未覆盖新增项估算。
   压缩、模型或协议切换、历史重写时使旧基准失效；不能只给全历史乘一个更小系数。
   输入、输出、reasoning、缓存读写和累计费用分别记录，缺字段保持未知。
2. 校准必须同时影响压缩判断和显示。无 usage 时才回退保守估算；不能把“约”改成“精确”。
3. 工具模型表示在首次生成时控制冗余，已发送的旧前缀保持稳定，或仅在正式压缩边界更换。
   数据库原文和补读能力保留；正文不因工具优化被裁剪。
4. 回归须覆盖真实 usage 小于估算、连续工具循环、缺 usage/失败重试、当前推理链、
   签名/加密续接、模型切换、重启恢复，以及两次相邻压缩。不能只验证百分比变低。

本文是调查结论。此前正文保全候选的通过记录不等于这些用量计算与费用问题已经修复，
本次也未对收费供应商发起验证请求。

## 补充：尚未对齐的范围

以下只涉及上下文、工具历史和用量，不声称已经审完 Codex 的所有功能。

| 环节 | 确认的现状差异 | 影响及边界 |
| --- | --- | --- |
| 实际用量基准 | Codex 保存最近实际 usage 与覆盖位置；MaiChat 主要重估完整请求并只向上校准 | 影响压缩时机，不能只改显示 |
| 原始续接快照计量 | Codex 按模型可见字段分类，排除传输 ID、外层 JSON；MaiChat 对匹配的 continuation.payload 全串估算 | 签名和结构字节也会影响本地估算；协议必需状态仍应回传 |
| 思考重放计量 | Codex 对无 encrypted_content 的 reasoning item 返回零重放估算，加密 reasoning 专门估算，并标记 usage 是否包含历史 reasoning；MaiChat 没有等价分类与标记 | 这是计量规则差异，不能推导 GLM 必须删除所有历史思考 |
| 工具模型输出预算 | Codex exec 使用请求输出预算与模型 truncation policy 中较小者，写历史时还有结果截断；MaiChat 采用每条 64 KiB、已知窗口总计最多 4 MiB 等自有预算 | 当前已有预算与原文补读，并非完全没有限制；尚非等价策略 |
| 用量明细 | Codex TokenUsage 区分 cached_input/cache_write_input/reasoning_output 等；MaiModelUsage 仅 input/output | 缺少定位缓存失效、费用与推理占用的必要明细，不能推断缓存一定没命中 |

另有两项应单独描述，不能冒充已经证实的 Codex 缺陷：

- MaiChat 的额外 tool_execution_observation 和 shell 元数据具有取消、执行影响、补读等语义，
  同时带来重复说明的实际开销。这里需要减少冗余；尚未证明它与 Codex 在相同任务上的净成本差值。
- 固定约 20% 的渐进释放、聊天正文保全是用户指定的产品策略，不是 Codex 的通用保证。
  Codex 有摘要替换及 retained-message 预算裁剪路径。对齐其计量和工具预算时，仍须保留用户的策略。

补充依据：Codex `history.rs:1392–1547`、`tools/context.rs:551–557`、
`protocol.rs:2630–2674`；MaiChat `MaiModelClient.cpp`、`MaiTurnRunner.cpp`、
`MaiContextBuilder.cpp` 与 `MaiModelClient.h` 的当前工作区源码。

## 六项补充核验：追加、传输、工具、图片、突增与思考

仍以 Codex d4af2452 为固定版本，不把某项功能存在当作用户的每个请求都已启用。

1. `client.rs::get_incremental_items` 比较上一请求 input 加上供应商已返回 output，
   要求本次 input 完整延续该前缀，并检查模型、tools、tool_choice、reasoning 等请求属性。
   满足时 WebSocket 用 previous_response_id 和剩余新增 input；不能续接时采用完整输入路径。
   普通 HTTP 路径仍构造完整的有效上下文。MaiChat 当前没有对应的 WebSocket 差量续接。
   本地 A+B+C 的语义追加、网络只传 C、缓存命中，是三种不同机制；后两种均不代表模型窗口只占 C。
2. 工具输出在模型表示/历史记录时受限，exec 的请求预算与模型 truncation policy 取较小者；
   常用文本截断保留头尾并标明省略。exec 默认请求上限仍为 10000 token，不能一概说很小。
   未发现等价的“任意函数参数先自动摘要再执行”通用机制；裁坏 patch/JSON/命令会改变执行。
   进程保留输出可继续读取，不等于所有工具都有永久、无限的原文补读存档。
3. 普通 Responses 请求带当前可见 tools 定义，调用的 name/arguments 与 call_id 结果另属历史 input。
   `tools/handlers/tool_search.rs` 支持 Deferred 工具搜索后提供可加载定义，避免把所有扩展定义
   首先全部展开。是否启用取决于能力与配置。WebSocket 的 input 差量并不代表 tools 字段也只发送差量，
   其 ResponseCreateWsRequest 仍有当前 tools；工具集变化还会阻止原差量续接。
4. `image_preparation.rs` 结合 detail、型号和 feature 选择 resize 模式；utils/image 的 high 模式
   限长边 2048/2500 patches，original 模式另有 6000/10000 patches 等预算。
   history 按 detail/视觉成本估算媒体，压缩保留窗口时可按媒体预算裁剪历史媒体。
   检查的这些路径没有固定“超过 8/12 张就滚动删旧图”的规则；并不保证任意数量图片都能容纳。
5. history 以最近真实 usage 加未覆盖项估算窗口；session/context_window.rs 计算可用容量与
   配置的自动压缩阈值，session/turn.rs 在轮前和工具后继处检查并进入压缩。
   突发大输入仍可能被拒绝。ContextWindowExceeded 被单独识别；压缩请求自己超限时，
   compact.rs 的本地摘要路径还会移除最旧 item 再尝试，单 item 仍不适配则失败。
   这些策略不能用作 MaiChat 自动牺牲用户正文的依据，也不能宣称所有超限都自动恢复。
6. Reasoning 是独立 ResponseItem，不是把思考全文拼入 assistant 正文。
   client 请求 reasoning.encrypted_content 以保留可续接状态；协议还可序列化某些明文 reasoning。
   history 对无 encrypted_content 的普通 reasoning 计零重放估算，对加密状态作专门长度估计，
   并通过 last_usage_includes_reasoning 决定补计。**计零不等于不发送**，不能由此得出
   “Codex 总是删除全部历史思考”或“GLM 可以直接套用同样规则”。

可复查入口：`client.rs:1808–1871`、`client.rs:2567–2578`、
`codex-api/src/common.rs:467–503`、`core/src/image_preparation.rs:499–545`、
`utils/image/src/lib.rs:80–95`、`session/turn.rs:1750–1779`、
`compact.rs:355–370`、`protocol/src/models.rs:1187–1199/1902–1908`。
本次只读源码核验，无供应商调用、生产代码修改或运行程序操作。
