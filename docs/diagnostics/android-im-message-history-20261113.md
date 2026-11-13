# Android IM 本机持久化消息查询

记录时间：2026-11-13；源码基线：`583a07bc48c53759a089ae8097184730eedf2a94`。

复现使用 Windows 上的 Robolectric SDK 34 与独立合成 SQLite 数据库，没有访问真实账号、用户消息、模型或网络服务。本记录不代表 Android 真机、模拟器正常 App 或安装产物验收；该次验证只运行 Gradle 单元测试，没有生成第二个 APK、安装或启动 App。

## 操作与预期

1. 向当前 owner 的数据库写入目标会话的一条旧消息，再向另一会话写入 240 条更新的同词消息。调用 `maichat_search_messages(query, peer_id=目标会话, limit=1)`，应返回目标消息。
2. 数据库保存一个从未打开、页面缓存为空的保留会话。调用 `maichat_get_messages(peer_id, limit=2)`，应返回数据库的最新两条。三条同时间消息 `a/b/c` 必须按 `c/b/a` 分页，不重复或漏项。
3. 使用 `kind/direction/since_ms/until_ms` 筛选；时间边界包含端点，毫秒支持整数及十进制整数字符串。更改游标筛选、登录代际、缺失数据库、表查询失败、取消均返回错误。空的后续页不能声称全局 `no_matches`。
4. 网络任务占用原 IO executor 时，历史查询仍可完成，主线程 heartbeat 可执行；取消排队请求只回调一次并移除队列任务。结果不合并进页面，不改变未读或输入草稿。

## 根因证据及修复

原 `MaiChatIMTools.hostGetMessages` 读取 `chatState().messagesWith(peer)` 页面窗口；原搜索先调用不含 peer 筛选的数据库 `LIMIT`，再在 Java 过滤 peer。`red-unit.log` 记录 16 个测试中 6 个 IM 回归失败，其余 10 个云盘测试通过。目标会话遗漏、未打开会话为空、筛选被忽略、无游标和账号切换后仍成功均被复现。

新 `RemoteIMMessageHistory` 提供普通类型化查询接口。两个 Agent 工具复用 `AndroidChatHistoryStore.queryMessages`，全部条件与 `(created_at,id)` 降序 keyset 边界在 SQL 的 `LIMIT` 前完成。保留原 `instr(lower(body),lower(?))` 匹配语义，`%/_` 是普通字符。查询仅覆盖当前账号单聊表，不覆盖群聊和 Agent 会话数据库，也不保证云端历史完整或跨页快照。

Session 在主线程捕获 owner、账号与鉴权代际，在独立单线程 executor 查询，再在主线程校验后交付。队列上限 8；查询总预算 5 秒，从主线程移交前开始，由独立单线程 daemon scheduler 触发取消，不依赖主线程计时器。取消同步中断 SQLite、移除排队任务，结束时清除 timer；主线程恢复后仍只交付一次并复核账号。工具等待上限 7 秒。游标是当前数据库实例内的随机标识，最多保存 512 个，绑定 owner、登录代际、数据库 handle/epoch 和规范化筛选；关闭或重新打开数据库即失效。数据库缺失或异常没有内存回退。

查询直接投影 `id/peer_id/sender_id/text/kind/direction/created_at_ms` 七列至不可变 `Row`，毫秒以十进制字符串返回。SQL 的 kind 过滤与结果使用同一个表达式，不调用普通 UI 的附件路径还原、审批/引用解码或图片准备；旧绝对路径、越界附件路径不会让 `kind=image` 结果变为 `text`。普通 UI 消息加载接口保持原有行为。

性能回归先发现 WAL 未启用，跨会话查询计划使用 `messages_remote_idx(owner_id)` 加临时排序。`perf-red.log` / `perf-red.xml` 记录这两项失败。随后启用 WAL，并以 schema 8 → 9 的增量迁移添加 `messages_owner_order_idx(owner_id,created_at DESC,id DESC)`，保留现有会话索引；查询执行期间不持有 helper 的 Java monitor。

## 验证结果与边界

独立审查发现旧查询可能在 helper 关闭/重开后向新注册表发布游标。新增确定性测试先捕获旧 handle 并阻塞注册，再关闭/重开测试库；`epoch-red.log` 记录旧代码错误返回成功。修复以短锁核对 handle/epoch，不锁整个扫描；数据库代际同时绑定结果页并在 Session 回调前复核。

最终离线 `testDebugUnitTest` 35 项通过：14 项工具历史、4 项性能/并发重开/迁移、原消息搜索 1 项、本地历史 6 项、云盘对话框与目录 10 项。包括 200 条图片命中不初始化 Agent/物化媒体、七字段类型、旧路径/损坏的非查询元数据，以及主线程持续阻塞 5.3 秒时请求仍已被独立期限取消并从队列移除。上述新增用例均先在旧实现失败。完整原始日志、JUnit XML 与七个相关源码/测试文件 SHA-256 在 `.cache/android-message-search-20261113/dto-deadline-green-receipt.json`；日志为 `dto-deadline-green-unit.log`。早期 28/30/32/34 项记录及各次失败分别保留，没有覆盖为最终通过。

| 合成记录数 | 指定会话 | 跨会话常见词 | 罕见词 | 无命中 | 下一页 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 4 ms | 4 ms | 5 ms | 3 ms | 2 ms |
| 100,000 | 2 ms | 1 ms | 46 ms | 44 ms | 2 ms |

100,000 条数据中，翻到约 90% 历史位置的旧游标页耗时 45 ms；持续扫描同时的写入耗时 2 ms；运行中取消返回 46 ms，预先取消 0 ms。全局查询计划使用新 owner/order 索引，会话查询使用原 conversation 索引，均没有临时排序。以上是当前 Windows 测试引擎的观测值，不是手机硬件性能承诺。

共享移动 C++ 工具已补完整筛选/续页 schema 和调用宿主前后取消守卫；Mac 复用同一测试 object、普通旧/新 Mobile 库，实际 RED→GREEN，Core 库未重建。同步 JNI/C ABI 尚无即时原生 Stop→SQLite interrupt 通道，因此取消结果的可靠性与宿主独立期限均有验证，但不能称为跨桥立即抢占。正常 App 的构建、单次正式更新及设备验收由主任务统一进行。
