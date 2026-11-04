# 本地主模型首轮工具声明按需发现

## 复现与预期

2026-11-04，用户在 iPhone 14 Pro Max 上以官方 Qwen2.5-0.5B Q4_K_M 本地主模型发送
“你好”，实际完成耗时 62 秒。预期是正式 Agent 的首轮短问能及时开始真实回答，同时保留完整
system、用户历史、工具能力、原有审批与执行回路。不能用固定问候、输入裁剪或隐藏耗时通过。

原长上下文证据见 `maichat-local-primary-prefill-20261104.md`。本轮宿主为 macOS 27.2 arm64，
源码 HEAD 为 `387683b937edb734e6af549421c8100b044b8b68` 加本轮未提交改动。未操作真实设备、
用户数据库、账号、密钥或付费接口；本记录的 native 时间不代表 iPhone 或 Simulator 验收。

## 定位证据

正式 Mobile C ABI 捕获的完整目录有 119 个工具、约 73 KiB 原始声明。原模板渲染为
97047 字节、23457 个输入 token。独立 native 探针使用同一个真实官方 GGUF、完整捕获的
system/history/tools 和实际 MTL 权重，测得 model load 0.264 秒、context 0.041 秒、完整
prefill 13.653 秒。耗时主要在目录对应的长上下文计算，不能归为每次重新读取权重。

保持相同完整输入后，Metal microbatch 512/1024/2048/4096 分别为
13.653/13.523/13.457/13.555 秒；Q8 KV 的 512 批次为 14.163 秒。这些差异不足以支持
修改生产 batch/KV 参数。发现优化没有修改原 runtime 的 KV 清理；后续实际 Simulator
数值故障需要补充 GPU 能力闸门，详见下节。

## 实现

- 新 `MaiLlamaToolCatalog` 提供普通按精确名称查询及 JSON 工具调用入口，共用同一实现。
  目录保留每个当前可用工具的原名；原始 description/schema 逐字保留，未做摘要、类别筛选或改写。
- 本地主模型先仅声明真实只读 `tool_search`，其 schema 枚举完整当前工具名目录。模型可以直接
  回答，也可以选择本次需要的工具。普通 catalog 查询成功后，同一真实模型用原始完整
  system/history 和选中的原始声明再次推理；真实操作仍交原 `MaiTurnRunner` 审批、执行、回灌。
- 内部声明选择不追加为用户或工具历史，不伪造操作结果。早期原型曾把 metadata 作为工具结果
  回灌，0.5B 模型随后只解释声明而没有操作，故该红原型未进入生产。
- 每个逻辑 stream 最多一次发现批次和一次实际任务推理。未知名字、非法/未完成参数、混合批次、
  取消或推理失败明确返回错误，不回退到缓慢的完整声明模式，不交付半批操作。
- 原始请求的 2 MiB 字节边界仍合计检查全部历史与全部目录。每次调用的选择状态只保存在栈上，
  不跨客户端、账号或会话保存。正文/思考仍同步增量交付；时间包含真实声明选择与任务两次推理。

## 真实模型结果与边界

生产工厂处理完整 119 工具、相同完整 system/history、真实“你好”，首次正文为 **0.991854 秒**，
随后按测试约定主动取消。实际 MTL 权重为 391859712 字节。后续 prefill 合作取消返回 Canceled，
普通 generate 仍切换到真实 CPU 配置，主模型恢复实际 MTL，新驻留代数与旧契约保持一致。

使用同一生产发现包装器与真实 `MaiAgent`，分别在三个独立进程/合成会话创建不可预知的随机
文件 nonce。119 项目录均保留；只有真实 `file_read` 可以操作合成文件，其他工具在探针中
明确禁止执行。三轮均真实发现 file_read、实际读取文件，并按调用 ID 将原始结果回灌；没有
从提示词预置 nonce。三轮共耗时 4.015、3.841、3.813 秒，每轮为 3 次真实模型推理、1 次
metadata 发现、1 次真实文件读取。

最终答案的严格 nonce 复制断言为 **2/3 通过**。第三轮 0.5B 模型把带行号的工具结果抄错，
没有削短 nonce、修正答案或放宽断言。

进一步捕获该轮真实读取后的完整 3 条历史，同一 GGUF/seed 直接重放，只改变工具声明：

| 相同 post-read 历史 | 返回时间 | 真实模型答案 |
| --- | ---: | --- |
| 按需目录声明 | 0.972 秒 | 把 nonce 抄为 `1.01M6J440VE001DQ7PT4V2` |
| 原完整 119 schemas | 14.051 秒 | 错答文件第一行是 `1` |

两轮的真实工具结果完全相同，45 字节：
`1\tREAL_DISCOVERY_NONCE_01M6J440VE001DQ7PT4V2\n`，SHA256 为
`66684a80b087d996824ce501d18c34e12438ba960e183ee166c94bfb19b9b9ac`。
这给出既有小模型理解行号/复制精度的能力边界，不能报告为完整随机 nonce 验收全绿。

## 产物与验证

产物目录：`/private/tmp/maichat-local-primary-model-20261103`。

- `discovery-production-freeze.json`：逐文件源码和 native 库哈希；9 项源码集合 SHA256 为
  `263a1e6de56124229f6089992f2b8711c5e32f584763f69b32e796a2c69aea75`。
- `prefill-batch-*.log`：相同完整输入的 batch/KV 对照。
- `discovery-full-catalog-nihao.log`：生产工厂真实首字、实际 GPU、取消与 CPU/GPU 隔离。
- `discovery-agent-production-1/2/3.log`：三次独立 nonce 的严格结果，第三次红日志保留。
- `discovery-post-read-exact-history.json`、`discovery-agent-post-read-exact-replay.log`：
  相同完整历史与相同真实工具结果的直接复制对照。
- `discovery-catalog-and-boundaries-final.log`：普通 API 与工具 JSON 入口、完整历史/原始声明、
  流式、取消、未发现操作、混合/半批参数等定点验证。
- `discovery-action-batch-red.log`：只在临时诊断源码移除最终批次校验后，安全边界断言确实变红；
  生产源码未改动。catalog 重复 JSON 键校验也完成独立红→绿对照。

最终 Core8 结果另见 `discovery-core8-final.log` 和 `discovery-core8-result.json`。
正常 App 构建 182 的安装、运行进程与真实 iPhone/Simulator 首问表现由根任务继续验收。

## App 182 实际 Simulator 数值故障与 183 修复

根任务在正常 App 182 的同一 iOS 26.5 Simulator 上，原封不动执行英文请求
`Reply with one short sentence: Hello from Qwen.`，原 360 秒窗口内始终未结束，正文产生大量
`!`。没有改短测试、替换问题或延长窗口。现场 `primary-ui-model-state` 为
gpu-selected=true、gpu-bytes=286208、backend=MTL、qwen-gpu-ops-supported=false。
这说明只有少量简单层权重卸载，并不能证明关键矩阵/注意力可在该设备执行。

`/private/tmp/maichat-inline182-primary-wait-sample-20261104.txt` 中全部 mai-turn 样本在
`sampleChat -> llama_decode`，主要为 Simulator Metal 搬运/同步。对冻结 App 182 dylib
反汇编，wrapper 偏移 +1516 对应第一次声明选择推理的虚拟 stream 调用返回地址，不是第二次
任务推理，也不是持续 prefill。macOS 同英文请求正常自然 EOG，用时 1.321 秒、1 次推理、0 工具。

独立数值探针链接 App 相同 Simulator vendor archives，使用相同官方 GGUF、相同 3035 个
完整 system/history/目录 token，按生产上下文 32768、batch 512、线程 2 运行。只取首 logits
和前 8 个诊断 token，不把诊断前缀当作完整用户回答，不改变任何生产输出预算。
输入来自正式 Mobile C ABI 的 119 工具目录基线，英文问题与 App 182 失败用例逐字一致；
可选 App 工具接线可能增加目录项，因此此探针不冒称捕获了运行中 App 的完整请求。

| 相同输入的 Simulator 后端 | 完整 prefill | 首 logits | 8-token 诊断前缀 |
| --- | ---: | --- | --- |
| 原部分 Metal | 23.629 秒 | 151936 项全部为 0 | `!!!!!!!!` |
| 显式无 GPU 设备的纯 CPU | 19.821 秒 | 151936 项均非 0，范围 -11.471 至 19.610 | `Hello! How can I assist you today` |

CPU 首选 token 为 9707（Hello）、logit 19.610199；原部分 Metal 每步 greedy 都选择 token 0
（`!`）。该对照独立复现 App 实际文本，证明是该 Simulator 部分 Metal 计算的数值故障，不能
用输出上限、重复惩罚或修改问句处理。

修复只改设备选择：Metal 须同时支持合法 Q4_K 矩阵乘法与 F16 KV Flash Attention，并分别
查询 batch 1/512。元数据形状使用官方 0.5B 的 FFN 行宽 4864、输出维度 896、headDim 64、
14 个 query heads 与 2 个 KV heads；仅分配元数据，不分配张量数据或运行 kernel。
能力不足时 n_gpu_layers=0、offload_kqv=false、op_offload=false，并传显式空设备列表。
不能继续传 devices=nullptr，因为固定上游会将其解释为自动枚举 GPU，而不是禁止 GPU。

补闸门后 macOS 正向仍使用完整实际 MTL 权重 391859712 字节，完整目录“你好”首字 1.171 秒，
CPU generate 隔离与取消通过。生产输出预算、采样器、模板、历史与 KV 清理没有修改。

后续产物位于同一证据目录：

- `sim182-numerical-{cpu,metal}.log/.logits`、`sim182-numerical-logits-evidence.json`：
  同输入的数值红→绿、全部 logits 的计数、范围和哈希。
- `sim182-exact-english.tokens`、`sim182-numerical-commands.json`：完整 token 输入及编译命令。
- `sim182-discovery-wrapper-disassembly.txt`：实际 App 182 第一阶段调用地址映射。
- `sim-metal-gate-native-positive.log`：能力合格设备仍使用实际 MTL 的正向回归。
- `sim-metal-gate-production-freeze.json`：App 183 冻结的 10 项源码；集合 SHA256 为
  `b3eb0dd454fec47a43034802cd87d6665277a87f18be956e51a4298a85a8628d`。
- `sim-metal-gate-core8-plus-local-final.log`、`sim-metal-gate-core8-plus-local-result.json`：
  闸门版本 Core8 与 4 套本地推理/目录边界测试。

正常 App 183 的实际安装、运行进程、首轮英文和多轮交互验收仍由根任务完成，不能把独立
CPU 诊断探针冒称为正常 App 已验收。
