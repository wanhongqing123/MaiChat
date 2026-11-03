# 正常 iPhone App 的本地主模型硬件探针

本探针只用于区分模拟器执行设备限制与真实 iPhone 的主模型 client 性能。普通主模型功能、多轮 UI、Agent 工具执行仍须分别验收，不能用本报告代替。

## 启动与报告

- 仅编译于 `DEBUG && !targetEnvironment(simulator)`。正常 `MaiChatApp.init` 只增加一行 opt-in 启动 hook；不替换 RootView，不创建新 Demo 页面，不显示全屏或测试消息。
- 显式启动参数 `--ai-primary-model-device-probe`。
- `MAICHAT_PRIMARY_DEVICE_MODEL_ID=<UUID>`：只接受 UUID，不接受路径。读取的已知源是本 App `Documents/MaiLocalModels/<UUID>.gguf`，文件名大小写与传入值相同。
- 可选 `MAICHAT_PRIMARY_DEVICE_RUN_ID=<新 UUID>`，否则生成新 ID。重复 run ID 不覆盖既有报告或临时目录。
- 报告为本 App `Documents/MaiChatLocalPrimaryQA/report-<RUN-ID大写UUID>.json`，按来源检查、导入、配置、每轮开始/结束、销毁与最终阶段原子更新。Root 只需提取该报告；探针不向全局日志输出路径或生成文本。
- 另记录实际 `MTLDevice` 的可用性、名称与 `supportsFamily(.apple7/.metal3)` 结果，供模拟器/真机差异对照；这些硬件族事实不代表任意算子都在 GPU 执行。

## 实际路径与隔离

1. 只读已知源，通过普通 `AILocalModelStore` 导入到新建 `tmp/MaiChatLocalPrimaryQA-<runid>/Models`。核对完整 SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db` 和 491400032 bytes，仅接受固定官方 Qwen2.5-0.5B-Instruct-Q4_K_M 候选。
2. 新建独立 `MaiMobileAgent` handle，所有 handle 调用与销毁在专用串行工作队列。configure 的数据库为 `:memory:`，workspace/appRoot/模型副本均在本次新临时目录内。使用真正 `modelProvider=llama`、主模型 client、自动输出、32k、默认硬件选择；不安装 host handler，不请求账号凭据。
3. 运行两次 `op=local_model_probe`。第二次明确带上第一问、真实第一答和第二问。该私有 C ABI 使用配置时同一 baseInstructions、完整 public 工具目录和相同 modelClientFactory；sink 只收集文本与工具调用计数，绝不执行工具。不依赖提示词作为安全闸门，不执行外部工具、网络或付费任务。
4. 每次 `timeout_ms=120000`，C++ 当前调用自身的取消 token 执行软截止，不发普通会话的 stop，不影响用户 handle。报告保存实际首字/总耗时、文本、native model/wire、GPU 是否实际采用、GPU 模型字节与 backend、错误及工具调用数。成功必须两轮文本非空、零工具调用、local ID/wire 正确且 GPU 已采用、驻留字节大于零。
5. 结束销毁本次 handle。共享 runtime 空闲且 generation 仍属于本次测量时才卸载，确认可释放后删除本次临时目录。busy、状态未知或驻留已换代时保留临时文件，避免删除仍可能 mmap 的权重。源文件不改动。

## 报告解释

- `scope=configured_local_client_full_tools_without_tool_execution`，`agent_loop_exercised=false`。没有创建或读取任何实际用户会话，不能称为“真机 Agent 工具闭环通过”。这是正常 App 目标下、完整工具模板输入、真实主模型 client/GPU 的专门测量。
- 文本只来自两条固定合成英文及其真实模型回复。报告清除可能回显的 App/临时绝对路径，错误仅保留有界安全说明；不含用户聊天、账号令牌或模型源路径。
- 正常 App 的既有 Root/账号生命周期保持原样；本探针没有对整个 App 的其他既有活动作网络零请求证明。探针自身无凭据获取、host handler 或工具执行路径。
- 本子任务只完成源码、device SDK parse 及实际 Store 模块/C ABI 头的独立 typecheck，没有操作设备、构建/安装 App 或运行真实 GPU。实际报告与 iPhone 型号/运行产物由 Root 随后取证。
