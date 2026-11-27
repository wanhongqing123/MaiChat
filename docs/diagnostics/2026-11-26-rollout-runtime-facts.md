# 模型完成项与运行事实捕获

时间：2026-11-26，Windows x64，源码基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`。
工作区包含联合存储与技能目录的既有未提交改动，本记录只说明本轮运行事实捕获边界。

复现：回环 HTTP 服务返回真实 Responses SSE，先发送 A 的 `response.output_item.done`，
再发送 B 的正文增量和一个完整 function_call item，最后发送 `response.failed`。
预期：A 的正文成为非草稿历史，B 保持草稿且不回灌模型；原始完成项完整记录，工具不执行。

修复前，原始完成项没有中立回调，编排层只有整响应成功后才清除 `modelDraft`。
这使 A 随失败尾巴一起被隔离。现在 `MaiModelOutputItem` 在真实 native item.done 的显示投影后发布，
保留完整对象和未知字段；terminal-only 成功输出在整批校验后补交，完全相同快照去重。
工具调用仍须经过整响应、整批身份及取消校验，保存原始 item 不授予执行权限。

新增 `MaiRolloutRuntimeFacts` 复用普通存储 API，记录规范输入新增项、初始/变更 developer 和
additional_tools、每轮一次 turn_context、实际 world_state 全量及后续差异。
不会把每个请求完整 A+C 输入前缀或流式 delta 追加为持久事件。图片使用路径/sourceRef，
该表示明确是规范 session 记录，不能声称等于 HTTP 请求原字节。

用量只由真实 usage 回调触发，token_usage_record 分开记录本次、当前轮和线程累计；
保留各字段来源及缺失值，不将缓存/推理子集额外加进总数。历史首个测量前是否完整未知明确标注。
新检查点在实际 prepare 后被采用时记录 compacted 的完整 replacement_history、窗口链、
源引用、最近 usage 和恢复元信息，不改写原消息。listMessages 读取后立即检查源健康，
实际读写失败阻止后续模型请求；NotSupported 只保留临时/旧自定义存储兼容。

验证：独立目录 `.cache/joint-storage-20261126/runtime-own/`，只读取 r311 依赖库，
不访问用户库、运行中的应用、真实供应商或凭据。

- 初始真实 SSE 测试 RED：原始 done/terminal-only 回调两项失败；修复后 GREEN。
- 原编排器基线 RED：A 未提交/未回灌、原始记录缺失、读失败和写失败仍发网络，共五项失败。
- 当前编排器 GREEN：完整回环 SSE、未知原始对象、加密 reasoning、正文 done 与 item.done 区分、
  用量 `77+19=96` 与 `109+5=114` 累计 `210` 而最近窗口 `114`、跨实例累计、
  单轮头部与输入去重、采用窗口的完整历史/路径引用与链式编号、两类源失败零网络均通过。
- 最终测试产物：`final/MaiRolloutRuntimeFactsTests/MaiRolloutRuntimeFactsTests.exe`，
  SHA-256 `23836ba4289d39284defdcd38f1edb7827cdb7dd0f69107fe8f18558cd44188d`，
  buildExit=0、testExit=0、0 failures。

边界：这里的编排回归使用实际网络适配器及 Memory 投影/记录捕获夹具，
没有冒充真实 SQLite 进程重启、桌面 UI、安装产物或实机验收。
真实联合源重开/索引重建及正常 CMake 全量测试由集成验证另行确认；本次没有修改 UI 的用量显示口径。

## 同日运行事实复审

复审发现旧 helper 把完整源输入 ID 集合反复附在 turn_context 与 token_usage_record，
长历史和多次请求会造成近似二次增长。新增 1000 条旧输入回归后，旧代码两项载荷大小断言失败。
现在彻底移除该持久字段与恢复逻辑，实例内只保留至多 4096 个源身份的缓存，
重启及缓存淘汰后的最终幂等由联合后端稳定源身份及载荷指纹索引保证。
1000 条历史输入的后续计量记录大小不随历史身份数增长；原输入只保留各自不可变事实。

计量累加现有 size_t 溢出风险也由 RED 用例捕获。
现在累加前检查余量，溢出字段记录 null、source=missing 及显式 error=overflow，
不会回绕成较小的“实际总量”；原始本次测量保持完整。

内部摘要请求的独立 sink 原来遗漏 usage。Compactor 现在通过可选的普通 onUsage 观察器
转发真实供应商测量；Runner 在任何摘要请求前加载既有累计状态，并将其标为 compaction。
摘要测量进入轮次与线程累计，保留最近 sampling 的实际主窗口占用；不生成虚构用户输入。
真实内部摘要流即使在测量后失败，已产生的计费事实仍保留。
观察器源失败单独阻止后续摘要/主采样，不允许格式或容量降级吞掉它。

同批 Runner 增加 prepareSkillInputs 普通回调，在过滤尚未消费的 steering 输入后，
为整次 build/compactor 重建固定同一份源用户 ID 到选中技能正文映射。
正文只经 ContextBuilder 的 UserContextProjector 加到原 USER 消息；不提升为系统指令。

复审 RED：载荷增长/溢出三项失败；摘要计量遗漏两项失败。
修复后独立 `review-green` 构建及用例为 buildExit=0、testExit=0、0 failures，
产物 SHA-256 `6cd79ef3c6390e23422126c70e3e0a7f4f778e3cc8c5dd2fa30059c6e1c45935`。
追加观察器 Protocol 源失败不可被降级吞掉的保护用例后，最终 `review-final` 同样全绿，
产物 SHA-256 `7701d1bc5072873feccf22686a0b027045416f44c0471c919384ae77e59aa298`。

## 正式联合后端与真实 SSE 的磁盘集成回归

新增 `MaiJournalRuntimeTests` 直接调用正式 makeMaiSqliteStore，而非 Memory 捕获夹具。
同一个真实回环 SSE 经 Runner 写入自有临时账号根：A 完成快照重复发送两次，
B 只有增量，随后发送未知 native item 与未执行 function_call，最后整响应失败。

实际关闭并重开后 A 仍是非草稿历史；B 不在 JSONL、不在重开后的历史及模型回放；
未知 native item 和 A 的完整未知字段与供应商原对象相同，原始 response_item 各只有一条。
恢复历史没有任何工具执行片段。随后只删除夹具目录明确拥有的 agent.db/-wal/-shm，
从原始日志重建得到相同 A，且 JSONL 字节保持原样。
最后破坏夹具自己的源正文后，正式后端源健康失败，Runner 没有发起额外网络请求。

独立 `journal-final/MaiJournalRuntimeTests` 为 buildExit=0、testExit=0、0 failures，
产物 SHA-256 `5350668be13a7289846628e43300b77173377d6cd74ebc549bd0674eae9cd565`。
该结果补充了上述真实 SQLite 重开/重建边界；桌面 UI、安装产物及实机验收仍需另行确认。

## 标准字段与轮次边界补全

turn_context 保留用户要求的 root_turn_id、disabled_plugin_ids、cwd、approval_policy、
approvals_reviewer、sandbox_policy.type、permission_profile、active_permission_profile、
model、comp_hash、collaboration_mode、multi_agent_version、realtime_active、effort、summary。
原始轮次身份、目录、型号和有效 effort 使用真实请求值；未传给核心的策略/模式等显式 null，
不从产品默认值猜造授权状态。原有 Mai 指纹元信息继续保留。

按固定上游 `d4af2452f7b5e39aadd7abd75b6e141059a4cc5d` 的 protocol.rs 核对：
开始使用 task_started；成功和普通失败均使用 task_complete，失败携带 ErrorEvent；
只有真正 Interrupted 使用 turn_aborted、reason=interrupted。
标准终态在应用 turnOutcome 源提交成功后、emitTurnOutcome 广播前追加。
应用 mai_turn_metadata 保留为完整领域事实扩展，两者引用同一原始 turnId，不重复执行任务。

字段缺失 RED 为十四项失败，补齐后 helper/Runner 测试 `schema-context-green` 为零失败，
产物 SHA-256 `ed834c71825038da07fce719569e248e610a5399b1db504feeb8d0420beb6f6b`。
新增真实 SQLite JSONL 断言确认实际失败流只有一条 task_started 和一条带真实错误的 task_complete。
最终 `schema-stable/MaiJournalRuntimeTests` 针对校验后复制到独立目录的固定 normal archive
再次为 buildExit=0、testExit=0、0 failures；archive SHA-256
`4c05321b753c3624d1d7d2b0d0c18c6ea935c30628dd7f07c59f317ea3447138`，
exe SHA-256 `b13784894258a156f91cfbdcd83af9e1c3d61e68eaf65e619719053087c20a44`。
期间另一个链接产物在进入 Runner 前即出现初始存储写失败，其相同 overlay 对象重新链接当前
archive 后通过；未凭推测修改生产代码，保留失败产物用于集成核对，最终构建须统一当前头和库。
