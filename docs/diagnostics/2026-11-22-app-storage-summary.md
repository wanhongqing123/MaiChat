# app_storage 默认摘要与候选分页

## 复现与预期

- 日期：2026-11-22；平台：Windows / PowerShell / MSVC。
- 源码 HEAD：`38d25b9d7a41ad29128e95289f44937c676ca452`，工作区包含其他任务的未提交修改。
- 用户现场：`app_storage` 扫描或清理 dry-run 内联 971 / 816 个候选完整路径，使本来用于判断占用和可清理大小的结果膨胀。
- 安全复现：只在 `MaiFileSystem::temporaryDirectory()` 下创建测试专属 `storage_test_*` 根目录及三个互不嵌套的目录；写入 971 个各 4 字节的缓存文件并调整为 26 小时前。
- 调用：`{"action":"scan"}` 与 `{"action":"cleanup","target":"cache","dry_run":true}`。
- 预期：默认返回候选数 971、候选字节 3884、目录统计、排除原因、清理限制、扫描是否完整及有效预览 ID；不内联候选路径。明细必须显式请求并分页。
- 该用例不请求真实清理，不访问用户 AppData、真实缓存、模型接口或网络服务；测试析构只回收其自建目录。

## 根因与职责

1. 普通核心 `MaiAgent/Agent/MaiAppStorage.cpp` 的 `MaiAppStorage::preview()` 已计算每类 `bytes/files`、`eligibleFiles/eligibleBytes`、最近文件及保护/策略排除计数；保存最多 1000 个精确候选，输出 `limited`。每类扫描最多访问 20000 个条目，触及上限时 `truncated=true`。
2. `MaiAgent/AgentTools/MaiAppStorageTool.cpp` 的 `previewJson()` 不区分摘要和明细，无条件逐项输出 `cleanup_candidates` 的绝对路径、相对路径、类别、字节和时间。`scan` 与 `cleanup` dry-run 共用该序列化路径，因此都膨胀。
3. 工具 Schema 和执行参数白名单没有摘要/明细或分页参数，未知字段会被拒绝。`preview_id` 目前只可用于真实执行。
4. 移动端桥接 `MaiChat/MobileAgentAdapter/MaiAppStorageBridge.cpp::previewOutput()` 已只返回统计，不输出候选路径。该路径说明普通预览数据足以提供摘要；本任务不修改其既有 JSON 契约。
5. 预览存储是每实例唯一的十分钟、会话/已解析根目录绑定、单次执行快照；每次重新调用 `preview()` 都替换 ID 与精确候选。明细续页不能靠重新扫描，否则会使旧审批 ID 失效且文件变更会导致跨页漂移。
6. Runner 的通用输出预算属于另一层，本任务不修改 Runner，也不借预算截断来替代业务摘要契约。

## 实现契约

- 保留 `app_storage` 名称、现有 `scan/cleanup` action、默认 dry-run 及全部现有摘要字段；默认不含 `cleanup_candidates` 明细数组。
- 显式明细使用 `detail=true`，配套 `offset`（从 0 开始）和 `limit`（默认 50，整数钳制到 1..100）。完整序列化 JSON 每页不超过 65536 字节；页内保留现有候选字段。
- 明细续页允许 `preview_id` 加分页参数；读取同一核心快照，不重扫、不刷新 ID/十分钟期限、不消耗执行资格。会话、解析后根目录、过期和工作区保护完整性校验与真实执行一致。
- 普通 API：`MaiAppStorage::readPreview(const std::string&, const MaiAppStorageContext&)` 返回已有预览副本。快照只保存在普通核心，不新增工具侧 ledger。新预览或真实执行消费后，旧 ID 的读取和执行都失效。
- 有 `preview_id` 的明细请求不能同时传 `target/scope/patterns/min_age_hours`；未显式请求明细时拒绝 `offset/limit`；真实清理仍只接受原有 `action=cleanup,dry_run=false,preview_id` 三个字段。
- `cleanup_candidate_count/bytes` 表示本预览实际允许执行的精确集合（至多 1000），`eligible_files/bytes` 仍表示已扫描范围发现的所有合格文件；`cleanup_limited` 与各类 `scan_truncated` 不可隐藏。扫描未完成时，零只是已观察到的数量，不得陈述为完整扫描确认不存在。
- 不改变真实清理三字段请求、逐次审批、保护清单、路径约束、文件身份检查及单次执行边界。

新增输出字段：

| 字段 | 含义 |
|---|---|
| 各类别 `scan_error_count` | 枚举或普通文件元信息读取失败次数；错误也令 `scan_truncated=true` |
| 各类别 `excluded_protected_bytes`, `excluded_outside_policy_bytes` | 与对应文件计数同一互斥分类的内容字节总量；普通结构体追加 `excludedProtectedBytes/excludedPolicyBytes`，扫描不完整时也只代表已观察下界 |
| 各类别 `totals_exact` | 该类别未触发条目预算且没有扫描错误 |
| `scan_complete`, `eligible_totals_exact` | 所选类别扫描是否完整，合格文件统计是否为精确总量 |
| `cleanup_candidates_complete` | 扫描完整且没有触发 1000 候选上限 |
| `detail_offset`, `detail_limit` | 当前保存集合中的起点及钳制后的条数上限 |
| `detail_total_count`, `detail_total_bytes` | 精确的已保存执行集合总项数和文件内容字节总量，与原 `cleanup_candidate_count/bytes` 相同 |
| `detail_returned_count`, `detail_returned_bytes` | 本页候选项数和文件内容字节总量；后者不是序列化 JSON 大小 |
| `has_more`, `next_offset` | 是否还有保存候选；有则为实际前进后的索引，无则 `next_offset=null` |

即使单项也无法装入序列化预算时，返回固定 `InvalidInput` 文案 `storage detail entry exceeds the 65536-byte page budget`，不返回会反复停留在同一游标的空页。

Windows 全套测试还暴露了旧的多段 scope 问题：`root.append(fromUtf8(".maiagent/model-previews"))` 保留内部 `/`，解析结果使用原生反斜线，导致安全检查的原始路径相等比较失败。用 HEAD `MaiAppStorage.cpp` 加当前文件系统实现/头文件和运行环境独立编译的对照程序，同样表现为 `screenshots` 成功、`.maiagent/model-previews` 失败；该对照不代表历史已发布二进制。日志为 `batch9-storage-scope-baseline.log`。

经主任务确认后，仅把 scope 路径改为逐组件 `MaiFilePath::append()`，统一为原生分隔符。逐级链接拒绝、解析相等比较和根目录逐段边界检查全部保留；没有修改文件系统公共实现。原多段允许目录、`..` 拒绝用例及已有 POSIX 链接拒绝用例保持在测试中。

## 验证记录

使用已有独占缓存 `MaiAgent/.cache/build-file-batch2-20261121`（CMakeCache 直接位于该目录；Ninja / Release）构建 `MaiAppStorageToolTests`。构建成功，耗时约 7.5 秒；没有修改该缓存配置或重建第三方库。日志：同目录 `batch9-storage-red-build.log`。

运行 `bin/MaiAppStorageToolTests.exe --summary-only`，退出码 1。日志：同目录 `batch9-storage-red.log`。

| 请求 | JSON 字节数 | 候选数 | 内联候选数 | 摘要应有候选字节 |
|---|---:|---:|---:|---:|
| scan 默认 | 221195 | 971 | 971 | 3884 |
| cleanup dry_run | 220731 | 971 | 971 | 3884 |

仅“默认不输出路径数组”和“小于 4096 字节”的两条断言分别失败，共 4 条。候选计数、候选字节、目录字节/文件数、合格字节/文件数、预览 ID、dry-run 和未触发限制/截断均通过。这是预期行为缺失导致的 RED，而非构建或样例错误。

RED 执行产物 SHA-256：`F3E852A1E6DC3CABF9D58E6B15A771E35CEA395B1405851566BF9F26AECF0826`。

批准完整契约后，先补充分页/精确零值用例，确认旧生产实现仍有 17 条预期失败（`batch9-storage-contract-red.log`），再修改普通核心、头文件和工具适配。第一轮完整测试通过记录在 `batch9-storage-green-full.log`。

独立审查随后发现保护/策略排除虽有计数，仍缺用户要求的字节总量。先补充七文件自建分类样例，旧实现稳定 RED 13 条缺字节字段断言（`batch9-storage-excluded-bytes-red.log`）；随后只追加普通摘要字段、匹配分类分支累加及工具 JSON 字段，移动桥接继续使用其现有摘要子集。最终完整 `MaiAppStorageToolTests` 退出码 0，0 failures；日志为 `batch9-storage-excluded-bytes-green-full.log`。本轮最终结果如下：

| 请求/场景 | 修复后结果 |
|---|---|
| 971 候选 scan 默认 | 1372 字节，内联路径 0，精确保存候选 971 / 3884 字节 |
| 971 候选 cleanup dry_run | 700 字节，内联路径 0，精确保存候选 971 / 3884 字节 |
| 同一 ID 遍历 971 项 | 无重复、无遗漏；后创建的文件不会进入快照；会话不匹配被拒绝 |
| 110 个长 Unicode 文件名，limit=100 | 首页面 65217 字节 / 96 项，触及序列化预算后以实际索引继续 |
| 空目录 | 候选/文件为 0，扫描完整、总量精确 |
| 20001 个测试自建目录 | 已观察到文件/候选为 0，但扫描不完整、总量不精确、错误计数 0 |
| 1002 个合格文件 | 保存候选精确 1000，合格总量精确 1002，`cleanup_limited=true`、`cleanup_candidates_complete=false` |
| 普通读取与真实清理 | 读取不会消耗 ID；其后只删除原预览内未变化、未新受保护的自建文件；消费后不能再读取 |
| 工具明细与真实清理 | 分页后原执行 ID 仍有效；新交付收据导致相应文件跳过；真实清理带分页字段被拒绝且不消耗预览 |
| 七文件分类字节闭合 | 合格 5 + 最近 6 + 保护 26（默认模型 8、gallery 7、交付收据 11）+ 策略排除 19 = 总量 56；普通入口、普通快照读取、工具默认摘要和工具保存页均验证排除字节 |

最终存储测试产物 SHA-256：`BDDD38715CA9AB8BA836C4C9A47C6382202F16E09D5E1D443AA0BDB4985AEE35`。

补充排除字节后，另用同一缓存的当前 `maiagent.lib` 再次手工编译/链接现有 `MaiAppStorageBridgeTests.cpp` 与未修改的移动桥接实现，结果 `PASS (0 failures)`。日志 `batch9-storage-excluded-bytes-bridge.log`；产物 `Batch9AppStorageBridgeTests.exe` SHA-256：`012615E1258C705708CC5FE9FE99BB56E35D1E088D1D396BFEC95F888C5B63F7`。这验证 Windows 上普通 C ABI 的既有摘要接口兼容，不代表 iOS/Android 实机验收。

补充排除字节前，按仓库要求额外构建并运行过八套核心回归（同一专属缓存，日志 `batch9-storage-core-final-build.log`、`batch9-storage-core-tests.log`）：`MaiCoreTests/MaiInputQueueTests/MaiSessionRenameTests/MaiFilePathTests/MaiStoreTests/MaiToolTests` 六套通过；当时 `MaiTurnRunnerTests` 和 `MaiContextCompactorTests` 未通过。Runner 中 2123/2129 等断言仍假定默认存储返回超过 100KB 且内联 971 路径，并继而断言通用输出投影；本次摘要契约使这些旧样例假设失效，已交由其所有者处理独立的大输出样例。另有 Runner 2594/2595 的行输出预算断言，以及 Compactor 44 条已有输出/恢复场景断言失败，未在本任务中定位或更改。该结果是当时工作区的检查记录，不代表主任务后续集成后的状态，也不能据此宣称全仓回归通过。未修改相关脏文件。

同一缓存构建 `maiagent-console` 与 `MaiModelClientTests` 后，以 `MAI_CONSOLE_BIN` 明确指定该新控制台，运行 `MaiAgent/tests/e2e/console_e2e.py`，退出码 0：`console end-to-end: all checks passed`。补充排除字节后重新链接控制台并再次运行，也于 2026-11-22 17:31 返回 0（最终日志 `batch9-storage-excluded-bytes-console-build.log`、`batch9-storage-excluded-bytes-console-e2e.log`）。脚本使用自建临时目录/数据库和 `127.0.0.1` 本机假模型，无真实供应商请求。

边界：执行产物是当前工作区源码构建的独立测试进程，不是已安装 MaiChat；未运行真实用户 AppData 清理、供应商请求、SSH、桌面安装包或移动端实机。扫描错误回调及单个超大条目固定错误分支已静态核对，未通过危险 ACL 更改或超长真实用户路径制造运行时失败。普通遍历预算不完整状态已由专属样例运行验证。

## 主任务最终集成

为保留旧聚合初始化顺序，主任务将新 `scanErrorCount` 放在原 `truncated` 之后，
新排除字节字段仍在尾部；二进制消费者均按新头文件重建。当前头文件 SHA256
`471A8BFB9ED94E6BB6CA8730E2D500B4EA3912451B3E5AB067CE3AE871C63BAD`。
随后移动 C ABI 测试重新编译/链接通过，控制台重新链接后的自有 loopback E2E 全通过。
最新专属收据 SHA256 为 `FE3E205A88ACAAE63BF01F43F9A80485466E0B321B0EFCFD0A304D8EB082E16F`，
保留了前述各轮产物的时间点身份。

旧 Runner 大输出夹具现改为独立的自有 verbose 工具，而非要求存储默认回到 971 条路径。
聚合已知/未知窗口与 Compactor 压力来源也经明确因果定位修正，原始来源、错误、
CAS、单次执行及补读断言保留。最终 Core 八套加存储及纯截图可用性 10/10 通过，
8.66 秒；见 `2026-11-22-core-budget-fixture-integration.md`。因此前述 6/8 只是历史记录。
Windows 普通 App 新依赖重编、开发 staging 与真实 ZIP 解压副本的裸 PATH smoke 均通过；
未安装、提交或发布这些改动，公共 3.1.2 不含此摘要功能。
