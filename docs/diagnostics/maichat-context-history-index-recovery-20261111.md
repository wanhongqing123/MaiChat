# 摘要失败后的原史索引恢复

## 用户需求与基线

用户要求：session 数据库已有原记录，模型缺信息应主动回查；可恢复的摘要失败不应直接
结束任务。基线 main 为 `739ba3b887b82e411d2cc8536ff8f8e20e767ef2`，其产品树与已发
2.2.57/f360 完全相同。只读核源码，不读取真实用户数据库、会话、剪贴板或供应商配置。

SQLite 保存 session 元数据、用户/AI 消息片段、模型返回的 reasoning、工具调用编号/
入参/实际记录的结果/错误/状态、媒体路径及 MIME、quote、待消费输入、摘要检查点/
精确引用和专业任务记录。附件文件本体另存；主 Runner 线程及模型连接没有持久化。
本次复用普通 Core、持久原史和现有 session_history，不另建数据库或恢复旧线程。

## 原行为与根因证据

Compactor 在硬预算区摘要 Protocol 失败时返回错误；Runner buildRequest 失败后离开
工具循环，finish 保存 assistant 并发 SessionError；活动 Runner 随后移除。原有工具
记录仍在数据库，但准备下一次请求的过程没有利用它们继续执行。

新增旧 API 真实 Runner/自有 store/正式 session_history/计数工具用例，原生产五源逐
raw/blob 保全。原代码正常编译后行为 RED：PID48644/exit1，main=1、summary=2，
两工具各执行一次、SessionError=1、原文读取=0。同一批首个60031字节结果可归档，
第二个小工具观察保持为最新受保护 tail；不是通过裁剪最新结果制造正例。

## 实现与守卫

- 保留 2.2.57 的软区延期。硬区正常摘要及一次格式修正仍 Protocol 失败时，具备真实
  同会话 session_history 的主请求可生成宿主确定性的部分原史索引，再沿原 Runner 继续。
- 有效旧摘要六数组保留；新增 user 原文、quote preview 和媒体元信息作为 typed 原始
  source_notes 保留，不把第三方引用或普通问题升级成 constraints/授权。
- 稳定 assistant/tool 正文留在未改写的 DB；索引提供准确 message/part/callId、角色、
  字段、工具状态及原字节数，明确不是完整语义摘要。需要省略字段时按原 ID 精确回查，
  不通过重新执行工具取得结果，不伪造成功或截图归属。
- 当前 user、最近受保护的工具/媒体 tail、活动 Pending/Running 不裁剪。候选完整请求
  包含指令、guard、引用、工具和 tail，必须按当前 usage 倍率检查硬预算并严格缩小。
- 普通 Options 默认禁用。实际有绑定 sessionAgent 和 history spec 的请求才启用。
  持久 host marker 标明 index 依赖原史回查，后续 model merge 不能抹掉每次的可信
  partial-index 说明；无回查能力既不能创建也不能复用它，应从完整原史重建。
- 最终无工具收尾在 prepare 之前去掉工具，真实 hint 也在 prepare 前加入预算；不使用
  requireFinalAnswer 布尔值猜能力。取消、网络/限流、存储、来源或检查点冲突仍失败。
- 原文和全部成功过的中间模型批次均不混入失败索引；仅由不可变原来源生成候选。
  索引恢复核完整 store 前缀/source 投影和旧检查点，再走既有 CAS。正常语义成功保持
  原 covered-source CAS 契约，不无证收紧对未覆盖 tail 的处理。

## 定点 RED/GREEN

同一 Runner 正例 GREEN（PID40604/exit0）：main=3、summary=2、两工具各一次、
SessionError=0、原文读取=1，并生成最终回答。后续用例从模型实际收到的索引解析
message/part/state/字节数后构造真实 history 读取，避免测试从 raw store 私下代喂 ID。

新增无工具 hint 边界：未修分支 PID32236/exit1，prepare 之前7679、hard7680，实际
发出的请求7777。修正后 PID44880/exit0，越界的完整最终请求没有发给模型。

另发现并修正一个内部容量误拒：宿主索引不属于模型生成摘要，不能机械套用生成摘要
上限。32768窗口中，旧候选索引5060 tokens 超过 semantic limit2048，但完整候选6248
低于 hard29696，原代码 RED（PID22076/exit1、recovered0）。普通自有 store 安装仅用于
独立测量，不把它冒称生产恢复成功。改为索引自身受 scaled inputBudget、并继续检查
完整 hard/shrink 后 GREEN（PID42768/exit0），实际完整请求6178，12KB user 全文保留。
模型生成摘要仍保留原 summaryLimit，真实必保材料超 hard 的负例不放宽。

源、exe、PID、原始 stdout/stderr 和临时自有数据证据位于
`.cache/context-history-recovery-20261111/`。最终五源逐 raw/blob 冻结于 `final-source/`，
完整 Compactor（PID21800）exit0；13目标正常重连（PID31500）exit0；必要13组 ctest
（PID49080）13/13 PASS、124.18s，其中完整 Compactor 3.50s。原日志/LastTest/receipt 和
13实际可执行文件已另存保全，`final-core-proof.json` 核源码与冻结件一致。
最终 Compactor exe SHA256：
`4ce9930248c9cb0f50488ff958bb09092ec80a2db94adb0152b8c7ccdd88e6f7`。
独立最终 spec/code review 未发现提交阻断；平台正常 App/正式包门禁在统一冻结后执行，
不以 Core 结果冒称 Windows/Mac 包或真实供应商现场已验证。

## 交付边界

本次不无限重试、不接受任意坏摘要为语义摘要、不重放既有动作、不迁移 DB、不新增
恢复按钮/跨端 wire 或供应商代理。实际模型是否正确理解并主动选择回查仍需真实供应商
现场验证；确定性 fixture 证明的是正式准备/索引/工具/数据库执行链可继续。
新修复计划随2.2.58交付，既有2.2.57 tag/正文/8资产保持不变。
