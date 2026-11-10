# 长会话上下文压缩与原文回查（2026-11-10）

## 复现与预期

用户先提供原始下载链接，经过长文本、工具结果和多轮工作后，再要求从原链接继续。原始消息一直存在数据库中，模型却丢失链接，要求用户重发。

可重复的本地用例包含：1.2 MB 用户原文（链接位于中段）、180 KB 助手正文、1.6 MB 工具结果（中段有独立证据）、60 个后续轮次和 1.2 MB 当前输入。预期是按选中模型窗口压缩已完成的旧轮次，保留当前完整输入和工具配对；摘要遗漏细节时，模型能从当前会话原史自行查回精确链接和工具字段。

本次环境为 macOS 27.2 / arm64。源码基线为 `8cde6a87418c84691598b567f49a68255880bd10`，分支 `main`，修改未独立提交。冻结的 31 个源码/构建文件及各自 SHA-256 见 `/private/tmp/maichat-context-compaction-20261110/source-freeze-v6.json`；聚合 SHA-256 为 `0dccb88ad2c5606c72519308c89b6b0c25899ee8490bcbc823e118d211acbc52`。其中 CMake 的四个源文件和一个测试目标接线由协调端提供。

## 根因证据

原 `MaiContextBuilder` 默认设置四个字节预算：历史 256 KiB、旧消息 32 KiB、工具结果 16 KiB、思考 8 KiB。构造请求时先截短文本中段，再直接删除旧消息，只补省略提示，未生成摘要。字节数既不是模型 token 窗口，也不能证明被删除的目标、约束和链接已经由其他上下文覆盖。

最初长原文回归在旧实现中真实产生 8 个失败；把默认字节预算改为 0 后变为 0 失败。这只是取消静默裁剪的中间状态，并非语义压缩完成。原始记录保留在 `/private/tmp/maichat-context-history-20261110/{red-receipt.json,red-original.log,green-receipt.json,green-original.log}`，未覆盖。

## 实现与边界

`MaiContextBuilder` 默认保持全部文本；原有非零字节预算仍只供调用者显式选择。`MaiContextCompactor::prepare` 是独立普通 C++ 入口，`MaiTurnRunner` 在每次主请求前使用它。摘要模型请求仍由客户端直连已选择供应商，不增加服务端转发，也没有为兼容接口虚构原生压缩参数。

容量由 `MaiModelClient::contextLimits(model)` 提供。官方目录只匹配真实官方地址和已核对型号；未知代理、未知型号不套用一个公共窗口。当前目录为 GLM-5.3/Flash 的 1M 窗口、128K 最大输出，以及 DeepSeek 当前 `deepseek-flash` 别名的 1M 窗口、384K 最大输出。本地模型使用已配置的 `contextSize`。来源核对于 2026-11-10：[GLM-5.3](https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3)、[GLM-5.3-Flash](https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash)、[DeepSeek 目录](https://api-docs.deepseek.com/quick_start/pricing/)、[DeepSeek 模型接口](https://api-docs.deepseek.com/api/list-models/)。宿主可注入更准确的容量查询。

没有供应商 tokenizer 时，计数明确标为估计：按 ASCII 段、Unicode 标量及消息、工具声明和图片余量计算；不会把 UTF-8 字节数冒称实际 token usage。Chat 的独立 usage 帧和 Responses 的终止 usage 使用真实协议字段单独回传。若真实 usage 较估计更高，同轮后续请求提高估计倍数。

压缩在扣除输出及工具余量后，接近输入容量 80% 时触发，并尝试留下约 60% 的工作空间。输出余量与 wire 生成上限分开：主请求原来的 `maxOutputTokens=0` 继续省略供应商输出上限，显式值保持原样；摘要子请求单独限额。远端默认保守预留目录最大输出，本地 automatic 模式的余量策略不限制实际生成。

检查点位于 SQLite v8 的独立 `context_checkpoints` 表和内存后端对应结构，保存会话身份、版本、覆盖的第一/最后消息 ID、消息数、源前缀 SHA-256、摘要、完整引用索引、模型和时间。原始消息不删除、不改写。摘要仅接收尚未覆盖的旧完整轮次，超长字段按 UTF-8 边界分块并带原消息、片段、字段和字节偏移；每块合并上一份摘要，全部成功后才推进检查点。

存储层在同一锁/事务内核对检查点版本与原源前缀。边界之后追加消息允许；覆盖源被修改、竞争压缩已推进、摘要网络失败、取消、空/无效结果或写入失败均不覆盖已有检查点。重启后源身份仍匹配即复用摘要，不重复总结已覆盖原文。

完整引用索引持久保存，模型只接收按窗口推导的 token 预算内的完整条目，并收到总数、`has_more` 和回查提示。优先展示用户链接，不切短某条 URL。两万链接/路径的工具输出生成 40003 条完整引用，主请求只带其中 12 条，避免摘要被索引再次撑爆。

普通原史入口为 `maiReadConversationHistory` / `MaiAgent::readHistory`，模型入口为 `session_history`，共享同一实现。工具会话固定来自 `MaiToolContext.sessionId`，不接受目标会话参数；精确消息和游标都验证归属。支持字面检索、完整字段的字节分页，以及 `list_parts` / `field_offset` 的有界字段元数据分页。模型可发现同一助手消息内正文之后每个工具的片段 ID，再精确读取输入、结果或错误，无需猜 ID。

摘要和回查结果标为历史数据；可信系统 guard 不插入摘要原文字节，摘要放在标记的 user 数据消息中。摘要请求不提供业务工具；若模型仍返回工具调用，该摘要被拒绝。摘要不会增加工具授权。语义摘要仍可能遗漏或误述细节，不能宣称绝对无损，因此保留原史和可验证的回查入口。

旧工具可能含无效 UTF-8 原字节。压缩与 `session_history` 的 JSON 表示沿用既有模型 wire 的 replacement 策略，普通读取仍保留原字节；摘要不能因此抛出编码异常，也不改写源字节或指纹。

## 验证记录

所有推理均为确定假模型或本机 loopback，数据库为临时数据库。未读取用户数据库，未访问真实模型、付费供应商，未修改手机或现装 App。

验证复用了现有第三方库并重编自有 Core。完整公开头闭包的 78 个自有翻译单元先由正常 desktop 编译参数独立重编；随后使用正式 CMake Core targets 正常构建全部相关测试及 console。旧私有缓存缺失的 OpenSSL 3.5.9 生成头与 zlib 模板从相同 pin 的正常缓存恢复，未改依赖 pin、测试开关或平台配置。

- `/private/tmp/maichat-context-compaction-20261110/full-core-receipt.json`：78 个自有翻译单元的私有完整重编记录。
- `native-core-build-v4.log` / `native-regression-v4.log`：主输出行为及公开头闭包后的原回归 12/12 通过，154.28 秒。范围为 Core、Store、ModelClient、TurnRunner、Tool、ToolLoop、Permission、SubAgent、Question、ContextBuilderImage、InputQueue、SessionRename。
- `green-compactor-v5.log`：长原文/工具、检查点重启复用、只总结新增旧轮次、成功一块后失败仍不推进、取消、并发追加/源修改/竞争提交、跨会话拒绝、精确 URL 与 1.6 MB 工具分页读取、4 万引用索引、有界字段发现、注入 guard、主输出 0/显式值保持，均通过。
- `native-core-build-v6.log`：最后编码兼容修改后的正式目标构建。`MaiLlamaModelClientTests` 仅编译检查消费者，未运行真实本地推理。
- `final-targeted-gate-v6.log`：最后编码兼容修改后的 Compactor、TurnRunner、ModelClient、Tool 定点回归。
- `final-console-e2e-v6.log`：正式 console 产物的 loopback 端到端，包含持久化与硬终止重启，全部通过。

定点实现回退只在独立临时副本编译，源码工作区始终保留修正版。`red-variants-receipt.json` 记录：绕过压缩 48 FAIL；摘要计数丢失模型身份 2 FAIL；全量注入引用索引 1 FAIL；把生成预留写成主输出上限 2 FAIL；移除片段发现 5 FAIL。`red-utf8-receipt.json` 中恢复严格 JSON 编码真实产生 1 FAIL，捕获旧工具第 20000 字节的 `0xFF` 编码异常。原始 RED 日志均保留在相应 `red-*` 目录。

最终定点回归的退出码和源码/产物哈希另见同目录 gate 回执。12 组完整回归与末次编码定点回归是明确的两个源码 cohort，不能把前者表述为最后一版全部重新运行。

## 验收边界

当前证据覆盖 C++ Core、临时 SQLite、实际 console 构建和 loopback 线协议。真实模型的语义质量与耗时未做付费实盘验证；假模型长用例约半秒的数据流耗时不能代表真实摘要生成耗时。未知模型容量需要宿主提供可靠目录或计数器。

冻结后，协调端已用原 normal Qt6 native-images 缓存正常重编 macOS `maichat`，exit 0、70.779 秒；同一份 31 源还通过正常 unsigned iOS Simulator Release 编译/链接，exit 0、51.319 秒。两个实际产物均读回了压缩、检查点和 `session_history` 的代码标记，构建日志也包含 `MaiContextCompactor.cpp` 的实际编译。源码前后 SHA 全部等于 v6 冻结清单，构建时本地 HEAD 为 `7dffeb5bf8b45a1ebec6a780106b86dc619dabde` 加本轮未提交 overlay。

- macOS 正常构建版本仍为 2.2.54，实际可执行文件 SHA-256 为 `88a86cd8bad61cbef9fac7f81164146c5ba5cb89b9efee1ae9f29cbb9fb66652`。
- iOS Simulator Release 为原有 2.2.4 / 203，实际可执行文件 SHA-256 为 `f801950bb61134dc8e5eb7bbe077672052bb683293bd7a44377a7a246184891f`；未签名、未安装。

同一 macOS 可执行文件还通过私有 CFFIXED_USER_HOME、offscreen、禁止网络与真实用户 AppData 的 sandbox 启动/退出 smoke，owned PID 97236、exit 0；这不是实际模型或 UI 交互验收。

原 macOS 依赖中已有高于 14.4 的部署版本链接警告保持记录，本次只在 macOS 27.2 / arm64 构建及验证，不将配置中的 14.4 当作实测支持。消费者原始日志、源码与产物回执位于 `/private/tmp/maichat-context-history-20261110/`。本记录不把编译和 Core 单元测试宣称为手机或 UI 实机验收，也不替换现装程序。Windows、Android 和真机交互未在本任务中验证。
