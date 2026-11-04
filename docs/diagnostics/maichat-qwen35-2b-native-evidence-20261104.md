# Qwen3.5-2B 本地主模型原生证据（2026-11-04）

本记录只覆盖 macOS 27.2 arm64 的原生客户端、真实工具回灌及明确标注的本机检索 fixture，不能替代正常 App、Simulator 或 iPhone 验收。本分工没有操作设备、访问用户数据库或凭据，也没有请求线上检索/推理厂商 API。公开模型下载和 `127.0.0.1` 合成检索服务是网络范围。

复现输入使用正式 Mobile C ABI 的公开合成工具目录：完整 system、119 个原始工具声明、截图中已知的 assistant/user 历史，末问原样为“我要看今天的新闻”。截图顶部未显示完整的首问没有补写。预期是实际生成检索调用；普通“你好”应无宿主动作，相机仅观察选择，文件读取须一次真实读取并准确消费匹配 ID 的结果。

源码 HEAD 为 `b7560a61ddc03d054ac3a6f70128dd1ed88c85da` 加当时原生工作区改动；具体源文件 SHA、实际链接命令和产物 SHA 见下列证据。运行 profile 为 **32768 context、temperature 0、seed 0、maximumTokens 0、非思考、原生 AUTO**。最终代表复测重新编译、链接生产 archive，`extra_objects=[]`，没有私有 validator/runtime 覆盖，没有强制路由、关键词分流或脚本回答。

| 对象 | 固定身份 |
| --- | --- |
| 权重 | `bartowski/Qwen_Qwen3.5-2B-GGUF`，revision `7d26695454df6de5fbcce2e58681e62dae06ce43`，`Qwen_Qwen3.5-2B-Q4_K_M.gguf`，1396198496 bytes |
| 权重完整 SHA256 | `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8` |
| 原作者资料 | `Qwen/Qwen3.5-2B`，revision `15852e8c16360a2fea060d615a32b45270f8a8fc`；许可证及来源保存在模型目录 `ORIGINAL_*` |
| 实际模板 | 7755 bytes；SHA256 `273d8e0e683b885071fb17e08d71e5f2a5ddfb5309756181681de4f5a1822d80` |
| 最终生产 archive | `/private/tmp/maichat-pdf-core-gate/lib/libmaiagent.a`；SHA256 `6cd5308f078a82d91b1735a5942504506098ed1a94ae914dbca2a89376361fa8` |
| 最终选择 probe | SHA256 `7f441e94f6e79a057d88da55a6075477b06f16e6ca4bf45fe9df520c54295d19` |
| 最终 relative nonce probe | SHA256 `1326452c58065b706e7da6ff7c37b9b1d4be9b64701249a64ba0f3a82d1b3feb` |

实际权重路径为 `/var/folders/30/c_mhtwjd49xgfqyq61r1_57m0000gn/T/maichat-local-qwen352b-20261104-kardv2x7/Qwen_Qwen3.5-2B-Q4_K_M.gguf`。根任务和本分工分别完整重读核对 SHA。模型 profile、工具目录及历史没有为某条问题单独改写。

| 原生操作 | 实际结果 | 总耗时 |
| --- | --- | ---: |
| 已知污染历史＋今天新闻，最终 archive | `web_search({"query":"今天新闻"})`，调用 ID `call01M6JBYFVP0001DE3D34V` | 11.219s |
| fresh 今天新闻 | `web_search({"query":"今天新闻","count":5})` | 7.877s |
| 你好 | 自然中文问候，0 宿主动作 | 9.783s |
| 打开相机 | `mobile_take_photo({})`；没有执行相机 | 7.106s |
| 截图较早的短问“新闻” | `web_search({"query":"新闻"})` | 8.852s |
| relative 文件名、最终 archive | 一次真实 `file_read`，nonce 初始不可见，结果 ID 匹配，中文完整返回真实 nonce，`clean_completion=true` | 10.669s |
| 很长的系统绝对路径 | 模型把路径中 `m0000gn` 复制成 `m0000gn0000gn`，范围闸门正确拒绝；未读文件，模型报告失败 | 13.358s |

首次完整选择矩阵、绝对路径失败与 fixture 使用生产 archive `72fe11dae8fcb98941c7683ad45ba2917f204f39631c83a164e52a8336800717`；表中污染新闻和 relative nonce 两行已采用最终复测数值。最后的 `6cd5308f…` 变更只在 Metal 能力检查中提前拒绝未支持的整数/F64/Q8_K 权重类型；候选实际 `{F32,Q4_K,Q5_K,Q6_K,Q8_0}`、FA/SSM/GDN 形状、采样和历史不变。聚焦能力回归验证该边界后，仅按根任务要求重跑这两项；未把旧矩阵冒称全部在最终 archive 重跑。

实际 MTL 权重驻留为 **1320574208 bytes**。32K 选择矩阵峰值 RSS 为 **1.93–2.00 GB**，最终 nonce 为 1999437824 bytes；这些是独立原生进程的 `/usr/bin/time -l` 观察，不是 iPhone 内存或延迟保证。实际 GGUF 的 hybrid cache 张量预算为约 403.27 MiB@32K，另有权重和计算缓冲。

检索闭环使用原 system/history/119 声明、独立 `:memory:` Agent 和真实检索核心。唯一 HTTP 目标固定为本机 fixture；返回内容明确声明合成测试数据，实际 `current_time` 也可执行，但本例没有选择它，其余 117 个工具硬阻断。

- 首轮一次真实 loopback 检索、一次匹配结果回灌、正常结束，无权限请求、重复、预算耗尽或其他动作。回复明确说“测试数据，不是真实的新闻”，没有把 fixture 当作今天的真实新闻。
- 首轮原始严格 `passed=false` 保留：没有主动展示随机标题 marker；临时判分器的字面列表还漏掉“不是真实**的**新闻”中的“的”。没有改写模型结果或把原标志改绿。
- 同一会话实际工具历史后的独立自然追问，原样列出完整随机标题和 marker，并注明本地合成测试数据。`followup.passed=true`，0 新动作、0 新 HTTP，原工具结果及调用 ID 保持不变。marker 和标题都没有出现在追问中。

因此已证明选择、真实工具协议、回灌以及观察数据消费；本记录没有声称获取过线上实时新闻。原始自动判分与独立语义复核分别保留。

当前质量边界还包括：长随机绝对路径复制不可靠；metadata 发现会多取无关声明，例如问候时取账号/联系人声明后，第二阶段才正确选择自然回复。这些查的是本地声明，没有读取实际账号、联系人或位置。不能从这组原生用例推断任意任务或实机体验均已通过。

证据目录为 `/private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/`：

- [最终汇总、源文件及产物 SHA](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/native-production-final-evidence.json>)，SHA256 `cbfc52a9b60423aac4a0d96e8868384c1f12a0cbcf3b16a1d8d27f919165f9e3`。
- [最终编译与完整链接参数](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/final-native-build-manifest.json>)、[最终污染新闻](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/final32k-exact-history-result.json>)、[最终真实 nonce](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/final32k-relative-nonce-input.json.nonce-result.json>)。
- [完整 fixture/追问记录](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/production32k-news-fixture-followup-result.json>)、[独立判分复核](</private/tmp/maichat-local-qwen35-2b-native-20261104-sl0msnz4/grading-review.json>)。原 fixture 结果 SHA 始终为 `48739e7e9b17672f632199837a647042aeef5e62aa7ece3c3d43629d5d218612`。
- 失败的 Qwen2.5-0.5B/1.5B、Qwen3-0.6B、强制路由原型和 Qwen3.5-0.8B 基线均保留，分别见共享目录的 `news-reproduction-evidence.json`、`candidate-comparison-evidence.json`、`qwen3-candidate-evidence.json`、`controlled-routing-evidence.json` 及 0.8B 目录的 `native-quality-evidence.json`。

复跑入口是 `/private/tmp/maichat-local-primary-model-20261103/build-candidate-probes.py`；不传额外对象即 clean production 编译/链接。选择 probe 参数依次为 `model_path original_fixture discovery output.json`，原 fixture 为同目录 `news-current-system-exact-known-history.json`。设置 `MAI_CANDIDATE_CONTEXT=32768 MAI_CANDIDATE_TEMPERATURE=0 MAI_CANDIDATE_SEED=0`；完整源码、命令及每次原始请求/正文/reasoning/调用均在上述 JSON 中。
