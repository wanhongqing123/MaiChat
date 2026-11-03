# 本地主模型核心接入与真实工具闭环

## 需求与可复现操作

用户要求本地模型与 DeepSeek/GLM 并列作为 Agent 主模型，沿用完整消息、流式事件和工具回路。
本次检查从 `5216279acbfdf5a05944c6f0d00ff0de8a142b63` checkpoint 开始，包含随后工作区改动；
测试在 2026-11-03 至 11-04 的 macOS 27.2 arm64 主机执行，不代替各产品平台的交互验收。

1. 以 `MaiLlamaModelConfig` 指定宿主已授权的本地 GGUF、CPU 线程、上下文和生成预算。
2. 替换现有 Agent 前调用 `validateMaiLlamaModelConfig`；失败保留原配置。
3. 将 `makeMaiLlamaModelClient(config)` 交给现有 `MaiAgent`，只注册真实 `file_read`。
4. 在独立临时工作区生成随机标记文件。用户提示仅要求读取文件，不包含标记内容。
5. 观察真实模型生成调用、真实文件读取、相同调用 ID 的工具结果回灌及第二次模型回答。

预期是现有 `MaiTurnRunner` 完成整轮：主模型客户端不执行工具，也不通过调用 `llama` 工具
替代主模型。当前真实测试得到两次模型请求、一次模型生成的文件调用和多次文本增量，最终回答
包含最初提示未知的随机标记；保存的工具片段为 Completed，结果与下一次请求的调用 ID 匹配。

## 实现与边界

- `MaiLlamaModelClient` 使用中立的 `MaiModelRequest`/`MaiStreamSink`，复用唯一驻留 runtime、
  串行锁、CPU abort 与每次请求的 KV 清理。基础 system、完整历史、工具 Schema、assistant
  调用与 tool 结果由 GGUF 原生模板处理；不摊平成单条 user prompt。
- 正文/思考分别经上游 `common_chat_session::feed` 的原生 parser 交付 UTF-8 增量。完整正常
  结束后先校验整批调用，再交付 `onToolCall`。取消、参数错误、原生解析失败和 token/字节截断
  均不会交付未完成工具调用。超上下文返回实际所需 token 下界、当前上下文与调整建议，不裁掉
  历史或工具声明，也不回退云端。
- 首版仅支持已核验的官方 Qwen2.5 工具模板原文摘要。所有非空 default/tool_use 模板都在任何
  Jinja 执行前逐个核准；其他模板明确 NotSupported。上游 Jinja 没有通用执行预算，因此不把
  模板源长度限制称为沙箱，不宣称任意 GGUF 兼容。
- 配置预检只用 `MaiFileSystem` 读取最多 64 MiB 前缀，检查版本、计数、字符串/数组长度、
  张量形状、类型、对齐和文件数据范围，不创建推理实例或装载张量。它不证明模型权重完整性、
  生成质量或设备内存足够。TinyStories 无聊天模板，明确拒绝用作主模型。
- 主模型暂不支持图片或 projector，输入时明确 NotSupported；旧独立单图 runtime 的能力不被
  冒称为主模型多模态能力。现有 CPU/生成长度/上下文限制继续生效。
- 专用 `mai_llama_chat` 只编固定上游的 chat、Jinja、PEG、Schema、parser 与纯字符串/token
  辅助闭包，不编 CLI、下载器、HTTP server、`common.cpp` 文件工具或上游日志线程。`mtmd`
  仍不链接 `llama-common`。公开头不暴露上游或 JSON 类型。

上游来源为仓库内 llama.cpp `ee17f293b638295651c6ff4752a4dce102c6b8e4`（MIT，见其 LICENSE）。
源码清单固定在 `MaiLlamaDependencies.cmake`，辅助函数注明从该版本的 common.cpp/sampling.cpp
摘取；敏感模板/工具内容的上游日志被禁用，上游源码没有修改。

## 模型与证据

实际推断使用原作者公开的 Qwen2.5-0.5B-Instruct Q4_K_M，Apache-2.0：

- 来源：<https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/tree/9217f5db79a29953eb74d5343926648285ec7e67>
- GGUF：491400032 字节，SHA256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`。
- 实际 metadata 模板：2509 字节，SHA256 `d5495a1e5db0611132a97e46a65dbb64a642a499421228b9c8b93229097fa9a4`。
- 权重、下载回执及测试产物均在 `/private/tmp/maichat-local-primary-model-20261103`，未提交模型。

定点测试覆盖真实文本 UTF-8 流、主动取消、token/字节限制零工具交付、超上下文错误、损坏/巨大
GGUF 头、Tiny 拒绝、预检不改变驻留状态，以及真实 Agent 文件读取闭环。摘要门禁额外以“核准
default + 修改后的 tool_use”和反向组合构造负例，不能只验证最后选择的模板。

`MaiLlamaModelClientTests`、旧 `MaiLlamaToolsTests` 的真实 CPU 回归通过；旧 VLM 因没有匹配的
视觉权重/projector 明确跳过。最终稳定核心构建后八套原有单元测试全部通过，`ctest -j2` 用时
119.17 秒。原始源码/产物哈希及退出码见 `client-final-evidence.json`，日志包括：

- `client-final-complete-tests.log`、`resident-exit-final-complete.log`
- `runtime-tools-final.log`、`core8-final.log`
- `production-freeze.json`、`resident-exit-regression.json`

后续正常 App 的完整工具目录暴露了 CPU 长上下文性能问题。最终主模型已改为 Apple 上
自动优先实际可用 Metal，独立 generate 仍固定 CPU；默认输出预算 0 代表使用真实上下文
余量自然生成至 EOG。GPU 合作取消、有序驻留退出及最终 Core8 77.05 秒复测，详见
[完整工具目录性能记录](maichat-local-primary-prefill-20261104.md)。前文的 119.17 秒及
`client-final-evidence.json` 保留为首次 CPU 实现的历史证据。

## 两个定点诊断

中文探针要求逐字复述“明亮的早晨”，小模型实际输出“亮的早晨”。独立原生 parser 输入完整
短语时逐字完整保留，绕过 parser 的同一真实模型普通 generate 原始输出也为“亮的早晨”。因此
该红测是模型复述语义不符合预期，最终流式测试验证实际非 ASCII 输出、多增量与 UTF-8 完整性。
保留 `client-tests-iteration2.log`、`client-tests-utf8-probe.log` 和 `resident-exit-red-lldb.log`，
不把小模型协议可用说成具有云模型同等生成质量。

同一独立 probe 还复现了“不显式 unload 直接退出”的真实崩溃：旧产物退出 138，堆栈为
`exit -> shared_ptr destructor -> resetResident -> llama_free -> llama_context destructor`，
后端 registry 已先于 runtime 静态析构。修复参考 Chromium `NoDestructor` 的生命周期原则，
保留进程级 shared owner，不再在后端析构后释放模型；显式 unload 仍释放资源。相同 probe
对象仅重新链接后退出 0；生产客户端另有保留真实驻留模型直接正常退出的子模式回归。

上述检查没有登录、线上账号数据、用户聊天、付费推断或真实设备操作。产品正常 App 的模型
选择、会话、iOS/Android/桌面启动与实际安装产物另由各平台验收记录覆盖。
