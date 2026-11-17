# 摘要超预算后继续执行

2026-11-15，Windows 与 macOS 普通 Core 构建；修复前基线 `fa3e852a`。用户原报错为 `Context summary exceeded its token budget; original history is preserved.`，任务停止。

合法 JSON 摘要超过预算时原代码没有尺寸重试。硬预算区的备用索引又完整复制早期用户原文，较大的历史导入和工具结果使完整请求再次超限，最后返回原错误；“原历史保留”没有使 Runner 继续执行。

修复让格式错误与合法超大摘要共用最多两次总尝试，只从原始输入重建请求，不回灌被拒响应，不降低生成/推理预留。完整用户原文索引优先；仅容量失败时改为有界来源元数据，保留有效旧摘要的六个数组、当前用户输入及保护尾部。原史仍在数据库，由已注册且本次实际声明的 `session_history` 精确回查。

实际 Runner 对照使用 48 KB 旧用户输入、58 KB 工具结果和短当前请求：原实现 1 次摘要、0 次主模型响应、1 次错误；修复后 2 次摘要、2 次主模型响应、0 次错误，模型通过真实注册工具取回原 user/tool 字节后保存正常回答。源指纹不变，没有重放已执行工具。参考了本机 Codex `compact.rs` 的有界重试、保留近期输入及独立原史设计。

取消、网络、存储、来源或 CAS 冲突不被当作容量失败吞掉；当前指令及必须保留的观察本身已经超过模型容量时，仍明确返回 `InvalidInput`。没有精确历史工具能力的宿主和禁止工具的收尾不能伪称具备回查能力。

macOS 普通八套通过；Root 独立核对三个最终 blob 并在 Windows 普通库跑完 Core 12 套及 console E2E，全部退出 0，其中完整 Compactor 测试 4.179 秒。未调用真实供应商或用户数据库；发布与现装验证另行记录。


<!-- Preserved local validation record before source synchronization. -->

# Context summary budget recovery

Root handed over the exact test-only failure on 2026-11-15. Three production preimages match its SHA256 manifest. The same normal Apple Core target compiles/links successfully, then fails with one 1500-token valid JSON summary against a 1024-token accepted budget: main calls zero, one error, four failed assertions. It cannot recover because the original host index copies the 48k older user text back into a 13312-token request.

The compactor now shares at most two total model attempts between malformed schema and valid oversized summaries. Retries rebuild only the original request, do not feed back the rejected response, and keep generation/reasoning reserves. Stream/tool/ambiguity/truncation/network/cancellation failures keep their original behavior.

On hard capacity failure, the original full user-text index is tried first. Only a measured capacity failure before storage may retry a bounded host-metadata index. Valid previous six arrays, actual current input, protected tool tail, original DB records and source/checkpoint CAS are retained. Metadata entries record IDs/fields/byte lengths, mark incomplete indexes and direct session_history retrieval; they do not claim semantic summary or execution success. Storage/conflict/cancellation failures are returned without fallback. Genuinely oversized protected current input is a separate InvalidInput failure.

The actual Runner test now produces exactly two summaries, two main calls, zero errors and two real registered session_history invocations. Those return the exact persisted original user constraint and tool-result marker; only then is the answer stored. Original records/fingerprints remain unchanged. Additional tests cover format/oversize mixtures sharing the same two-attempt budget, unchanged reasoning reserves, bounded hundreds-of-record metadata, exact omitted-original reads and all failure guards.

Normal same-library eight suites all exit zero: Compactor, Core, Store, InputQueue, ToolOutputStore, TurnRunner, ContextBuilderImage and SubAgent. Original RED and intermediate failed full-suite logs remain separate. The first full suite exposed an early protected-tail check invalidating a still-fitting cached-image request; the check is now limited to hard overflow, and the original image assertions pass unchanged. The current-overflow test explicitly expects InvalidInput and zero summary requests rather than old Protocol.

Only MaiContextCompactor.cpp behavior, MaiContextCompactor.h contract comments and original CompactorTests changed. No Runner/Event/Origin/History/Qt production source was edited. Actual local Codex compact.rs was read for retry/oldest-history reduction and recent-user preservation. Tests use owned stores/Fake models/loopback only, with opt-in live environment removed and outbound provider networking denied.
