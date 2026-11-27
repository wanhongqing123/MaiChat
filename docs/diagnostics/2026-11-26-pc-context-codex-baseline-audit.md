# PC 上下文增长：Codex 基线与补充整理策略

时间：2026-11-26，Windows x64，工作区 `E:/OpenSource/MaiChat`。
审查起点 HEAD：`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，含既有未提交改动。
参考固定为本机 `E:/OpenSource/codex` 的
`3d86e43b075adeba2193ec5c22ac92f55afd82e6`，`rust-v0.172.0`。
仅 `git show/grep`，未切换参考仓库或更改其工作区。

## 可复现需求与判断标准

连续调用工具并追加少量正文，核对准备请求的 token 估计、供应商实际 usage、
完整请求体字节以及原史字节，四者分别记录。正文和实际供应商用量不能由字节统计代替。
默认到原窗口 90% 才触发整理，输入硬限为原窗口 95% 与显式生成预留约束中的较小值。
“工具详情优先、每次约回收 20%、用户及助手正文逐字保留”叠加在这些基线机制上。
工具减到骨架而正文仍放不下时，明确失败；不能偷偷语义压缩正文来凑百分比。

本次新增确定性复现均为进程内合成模型和临时内存会话，无线上数据、供应商请求或用户数据库。

## 固定源码审查结论

| 机制 | Codex 固定版本 | 当前实现与决定 |
| --- | --- | --- |
| 触发与余量 | `protocol/src/openai_models.rs:414,577,584`：有效窗口 95%，默认自动整理上限为原窗口 90% | Compactor 保留同一基线；20% 是相对当前完整输入的回收目标，不能替换触发和硬限 |
| 最近 usage 与未覆盖增量 | `core/src/context_manager/history.rs:1193-1225`：最近服务端 usage，加该计量边界后尚未覆盖 item 的估计 | 修复 Runner 的单调倍率；使用本轮请求开始边界的真实 input 加严格前缀后新增估计，摘要/工具预算/硬限统一使用该入口 |
| 模型可见内容估计 | `history.rs:1363-1518`：文本、调用参数和媒体内容分开估计，外层 ID/JSON 元数据不作正文分词 | 修正续接载体整体 JSON 被当文本的估计；保留原始续接 wire 字节 |
| 加密推理兜底 | `history.rs:1347,1437`：base64 长度还原，扣除 650 字节，再进入约 4 bytes/token 的估计 | 使用同源公式，仍标为估计。不能断言密文为零 token、固定 token 或与长度完全无关；供应商实际 usage 可覆盖该前缀 |
| 工具输出预算 | `core/src/tools/context.rs:207,538-615`、`history.rs:735`：工具请求预算受历史策略约束，防止再截已截输出 | Runner 已有首次工具表示预算与持久固定表示；Compactor 保存每字段预算，仅在正式检查点收紧，不重新恢复省略字节 |
| 调用配对与媒体 | `history.rs:1227` 的 normalize 同时保护调用/输出配对与模型媒体支持 | Compactor 按完整轮次或稳定工具前缀切分，Pending/Running 和续接组受保护；图片逐张省略有来源，正文不顺带清空 |
| 失败及恢复边界 | `core/src/compact.rs` 在有效完成摘要后替换历史并重算用量；重试有边界 | 当前检查点使用来源校验和 CAS；取消、源变化、存储失败不推进。JSONL 原史与 window 记录继续走已有追加接口，本次未修改存储实现 |

兼容供应商的签名明文 reasoning 在本项目 Responses wire 中确实重放，因而仍估计其内容；
签名本身不计作明文。这与 Codex 针对自身 wire 不计无加密 reasoning 的规则存在适配边界，
不能靠直接删除载体或忽略全部思考来实现“对齐”。

既有补充策略保留：工具输出先于输入、错误至少保留必要预览，状态和宿主 execution 事实不裁为成功；
来源 ID 和原史精确补读能力必须存在；同一 source/frame/post-token 状态不反复整理。
没有额外一次“再砍 90%”路径。原始字节不变不代表当前请求仍含全部工具字节。
摘要收集的 1 MiB 是内存上限，摘要是否适配仍另按 token 判断；不能把 1 MiB 写成 1M token。
格式/输出预算补试共用一次补试，HTTP 413 重新分块最多五次；本次不扩大重试或主模型采样次数。
未知模型容量保持未知，不伪造 usage 或套用统一窗口。

## 修复一：摘要 413 不能绕过 token 硬限

最小复现：摘要模型只返回 `RequestBodyTooLarge`，本地确定计数器每正文/声明字节一 token。
固定窗口 8192、输入硬限 7782，分别测试原输入 7468、8768，以及含大量固定指令的 8510 token。
最后一个用例授权原史索引，但连零来源条目的最小索引也放不下。

修复前两个硬区分支返回成功的 `deferredCompaction`，把超硬限原请求交回发送层。
请求体字节适配无法替它解决 token 窗口或生成余量问题。
修复后只有 7468 安全延后；8768 和 8510 原样返回 `RequestBodyTooLarge`。
三种情况均只发一次摘要请求，无新检查点、源指纹不变。

## 修复二：续接元数据不能按明文增长

固定可见思考、助手正文和工具调用参数，只把签名扩展为 120000 字节、ID 和 transport metadata
各增加 24000 字节，并更换 JSON 空白格式。
修复前估计从 183 增到 56243 token；修复后同一可见内容均为 153 token。
显示投影即使再增加 100000 字节也不重复计量；增加真正可见正文则增加估计。
12K 与 24K 加密 reasoning 的估计由 2193 增到 4443，差 2250，符合固定版本的隐藏内容兜底公式。
没有改写、截断、解密或移除实际载体；HTTP 请求体容量仍按真实编码字节单独适配。
未知 item 或损坏载体保留保守估计，由发送层负责协议拒绝。

## Runner 增长回归的 RED/GREEN 证据

`MaiContextCompactorTests --usage-growth-anchor` 使用真实 Runner 和只读合成工具。
模型首请求本地估计 81，合成供应商实际报告 2081（固定开销 2000）；工具只返回 1200 字节。
工具后完整本地估计 564，以相同开销计为 2564，远低于 7372 触发线。
旧 Runner 却只调用一次主模型就容量失败：`main=1 summaries=0 succeeded=0`。
根因是首请求约 25.69 的比值乘到了所有后续内容。
修复后该用例完成两次主请求，不触发摘要，正常成功；第二次发送前实际请求的本地估计为
544，准备估计为 2544，固定开销只加一次。该选择器现已纳入默认完整套件。
锚点必须绑定选中模型、协议、指令/声明框架和精确保留前缀；检查点替换、重写或切换框架时失效。
不能用不受来源约束的 `actual + max(localDelta, 0)` 去掩盖输入删除、排序变化或上下文替换。

实现以真实 `inputTokens` 所覆盖的请求开始为边界，不以响应后的 `totalTokens` 为输入基线。
当且仅当来源/协议/容量/config 和旧消息、完整工具声明仍相同，使用
`measuredInputA + max(0, localInputB - localInputA)`，新助手输出和工具结果属于增量。
比较忽略仅用于宿主定位的 source message/part ID；载体字节、调用参数、消息角色和顺序必须相同。
请求覆盖边界以版本化指纹保存在原有 `token_usage_record` 中，最多 2 KiB，
不在内存或持久层重复保存整份请求；新 Runner 与重开会话只复用验证通过的覆盖前缀。
有精确 tokenizer、未知来源、缺失真实 input、框架变更时返回普通估计，不能重新乘旧倍率。
路径图片没有不可变像素保证，因此整份请求保守回退普通媒体估计；回调不做文件 I/O 或哈希。
内联续接载体则比较冻结的完整字节。

14 个真实 Runner 合成用例验证：正常追加、旧正文修改、schema 修改、来源修改、协议修改、
窗口缩小、精确 tokenizer、缺 input、未知来源、仅 source ID 改变、技能输入框架修改、
路径图片、实际输入小于粗估，以及工具预算结束后的无工具最后补答。
只有严格保留前缀及仅 source ID 改变时沿用 2000 固定开销；各失效路径逐项断言准备估计等于普通估计。
实际输入较低的用例首请求 local=1081、actual=581，下一请求 local=1544、prepared=1044，
证明不是只修正过小估计或仅调界面。目录撤下后的最后补答 local=601、prepared=601。
既有真实 usage 较高导致硬限失败的回归仍保持，不为了转绿放松容量断言。

## 新轮次及重启的实际计量基线

只修同一 Runner 仍不足：首轮 local=7095、actual=2500，下一条新用户输入使 local=7549，
旧实现会因新 Runner 丢失实际基线而做两次无必要的摘要。这在同一 SQLite 对象和真实关闭/重开
SQLite 两条路径均完成 RED 复现。

最终实现将请求开始的版本化覆盖边界与该次供应商 input 原子追加到同一 `token_usage_record`：
哈希协议版本 1、本地估计器版本 2、model/provider/wire/window/生成预留/检查点 revision、
消息数与精确顺序前缀 SHA256、工具数与完整声明顺序 SHA256、请求框架 SHA256、
原本地计数及实际 input。哈希逐字段按长度喂入，不复制、转义或持久化正文。
后续若改变本地估计器语义，必须提升 estimator version，使旧计数基线失效。
摘要用量不能生成锚点，摘要记录只携带原有的有界采样锚点；total/累计量不参与输入覆盖公式。

恢复除校验当前前缀/config 外，还核对同条记录恢复出的真实 sampling input 来源和值、
model/provider、窗口和检查点身份。锚点不能用自身的 `measured_input_tokens` 自证真实用量。
未知版本、字段类型错误、负数/越界计数、错误哈希、来源/窗口变更和检查点变化均退回普通估计；
这些可选元数据错误不会把整段有效会话判成损坏。

10 个真实 SQLite 用例验证新 Runner、真实关闭/重开、原前缀修改、未知 estimator version、
坏 SHA、负数/过大 message count、检查点不符、伪造 actual count、provider 不符。
正确的新 Runner/冷重开结果均为 `2500 + (7549 - 7095) = 2954`，摘要调用为 0；
失效条件分别回到 local=7549～7555，软区按原契约执行两次摘要尝试。
原有同轮 14 个锚点边界、20% 工具优先、正文逐字保留、413 和失败/CAS 回归全部保留。

## 构建与验证边界

证据目录 `.cache/pc-compaction-audit-20261126/`：

- `red/` 与 `green/`：413 硬预算 RED/GREEN，同一三个确定用例。
- `continuation-red/` 与 `continuation-green/`：续接估计 RED/GREEN。
- `growth-red/`：固定 usage 开销导致早触发的独立失败证据。
- `final-overlay/`：包含新增 413/续接断言的完整 CompactorTests，退出 0、`0 failure(s)`，
  运行 11.602 秒；EXE SHA256 为
  `39de1c540774ab80c3535f354b23cc29249fa31b3a2cf57b4d2b58d3b2656f1f`。
- `run-overlay.py`：复现编译与运行入口；每阶段保存 build/run 日志、退出码和 EXE SHA256。
- `native-anchor/`：使用新 ABI 的完整 114 个 Core 翻译单元构建的 archive，独立链接最新测试；
  14 用例选择器退出 0，完整套件 `0 failure(s)`、12.763 秒。
  EXE SHA256：`80f25827255e41a17a4d73090fff0b61c3a6334aa4542f85245721f4ea6759fb`；
  测试源码 SHA256：`7dcfcc19d6d8714ad71eafa775358fc8ad2e88da86198a91bbb3952783883cc8`。
  `run-native-anchor.py`、`growth.json/log`、`full.json/log` 保存可复核入口和结果。
- `persistent-red/`：新 Runner 与真实 SQLite 重开的过早整理 RED，独立选择器
  `--persistent-input-anchor`。
- `persistent-native/cohort-01/`：使用最终有界锚点 API 的全新 116 个 Core 翻译单元，
  包含正式工具目录 hooks。完整 CompactorTests、RolloutRuntimeFactsTests、
  JournalRuntimeTests 和 SessionRecordCodecTests 均退出 0。
  CompactorTests EXE SHA256：
  `8d39d0b2afabd528698df79e98f293179af8e39abd84b3b21a94e157a41c2920`。
  `run-persistent-full.py` 与 `run-persistent-final.py` 保存独立源快照和编译入口，
  build/run 日志与 source manifest 在该目录内。

本次在自有临时目录编译测试和变更对象，使用 `.cache/r311/src` 的整套一致旧头与只读
`.cache/r311/build-msvc/lib/maiagent.lib`。私有 ModelClient 源副本仅去掉新增加的 provider
虚函数尾部定义，以匹配该旧 ABI；没有覆盖共享缓存、修改 archive 或把新旧类布局混链。
后续原生验证使用 `.cache/pc-usage-20261126/full-native/cohort-01/` 中全量新编译的
Core archive 和同套冻结头，不混入旧 Core 静态库。主任务仍负责产品正常构建及桌面集成回归。
没有安装候选包、停止或替换程序；未核验用户实际运行产物，不宣称完成桌面实机验收。
