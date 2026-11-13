# Desktop 持久化消息查询回归与验证

日期：2026-11-13。平台：Windows 11、MSVC 14.44、Qt 6.12.0、仓库内 SQLite 3.49.1。
源码基线：`583a07bc48c53759a089ae8097184730eedf2a94`；本文修复与回归尚未提交。

## 可复现操作与预期

所有数据均由测试在 `QTemporaryDir` 内创建，使用 `FakeRemoteIMClient`；未读取真实账号数据库、查询实际账号、访问线上接口或操作剪贴板。

1. 在私有 SQLite 中保存同一 peer 的 65 条消息，最早一条包含唯一文本 `UNIQUE_NEEDLE`。
2. 构造 `RemoteIMApplication`，确认内存仅有一条会话预览。
3. 调用 `maichat_search_messages`，应找到最早消息；查询不得把历史灌入 `ChatState`。
4. 调用 `maichat_get_messages`，按倒序游标逐页读完，应得到全部 65 个不重复 ID。
5. 打开会话，确认内存加载 40 条，再重复查询，结果与第 3、4 步一致。

原实现的实测 RED：在仅加载 1 条和已加载 40 条时，搜索均返回 0 条，预期为 1 条。
`MaiChatHostTools.cpp` 原搜索只遍历 `ChatState::forEachMessageWith`；应用启动预览为 1 条，打开会话页为 40 条。
因此“当前内存没有命中”无法证明持久化历史没有命中，更无法据此推断用户清空过历史。

## 最终查询契约

`maichat_search_messages` 与 `maichat_get_messages` 均通过普通 `queryLocalMessages` API 读取活动账号的本地数据库，不读取内存消息、不提供内存回退。
无可用数据库、取消、账号或连接代际变化均显式报错。

- 排序为 `(created_at_ms DESC, id DESC)`；游标严格读取小于上一页末尾键的记录。
- `since_ms`、`until_ms` 均包含端点；可传精确整数或十进制字符串。
- 默认 `limit=50`，范围为 1–200；未知字段、非法筛选或游标报错。
- 搜索要求 `query`，可选 `peer_id`，返回 `matches`；普通读取要求 `peer_id`，禁止 `query`，返回 `messages`。
- 共用 `kind`、`direction`、时间、数量和游标筛选。游标绑定数据库实例、查询条件和账号连接代际。
- 结果声明 `coverage=local_persisted`、`local_available=true`、`cloud_history_complete=false`、`snapshot=false`。
- `no_matches` 只用于完整执行后、首页确实为空的本地数据库查询。错误或后续空页不会生成全局无命中结论。

每页反映当前数据库；游标并非保留删除前记录或时间修正前记录的快照。查询范围为本地已保存的私聊消息，不代表已下载腾讯或云端全部历史。

## 实现与并发边界

查询使用独立只读 SQLite 连接、参数绑定和 `LIMIT+1`，只读取已公开的消息字段；不读取附件本地路径或审批令牌。
`MessageSearch::PreparedQuery` 只缓存查询的 trim、分词、折叠大小写与拼音资格判断，原有匹配等级、子序列和拼音规则由原测试守护。

单独增加只读连接仍不足以保证写入安全：原 rollback journal 下，长查询会持有 SHARED 锁，UI 线程的消息落库可能等待后失败。
合成 RED 已观测到活动读事务期间 `insertMessageIfAbsent` 返回 false；写侧原有 busy timeout 为 5000ms。
改为验证 WAL 后打开数据库，使查询读事务不阻塞新消息写入。原依赖 SHARED 锁制造 COMMIT 失败的测试改为 WAL 并发写入成功测试；另保留独立写锁超时与业务事务回滚断言。

跨 peer 倒序查询原计划为 `SCAN messages; USE TEMP B-TREE FOR ORDER BY`。
新增 `idx_messages_page(created_at,id)`，保留 `idx_messages_peer_page(peer,created_at,id)`，使全局与单 peer 查询均可按索引顺序读取。

普通 API 使用独立的两线程池，最多四个未完成请求，不依赖头像或媒体任务使用的全局线程池。
固定 5 秒预算包含排队时间；SQLite progress/busy handler 检查预算与取消。
预算用尽返回 `query_timeout`，超出排队上限返回 `query_busy`，不会伪造空结果。
工具在最终发布前再次检查本轮取消标志，避免 10ms 轮询间隙返回成功或 `no_matches`。

## 已执行验证

隔离构建位于 `.cache/message-search-20261113/isolated-build`，使用与 Desktop 相同的 C++17，仅编译实际存储、模型、匹配器和对应测试，使用仓库 SQLite，未触碰主构建缓存。

- `LocalMessageDatabaseTest`：36 通过、0 失败。
- `MessageSearchTest`：21 通过、0 失败。
- 两者退出码均为 0。
- 构建与测试日志：`.cache/message-search-20261113/build-isolated-green.log`、`database-green.txt`、`matcher-green.txt`。
- 先前失败证据：`red.txt`、`performance-red.txt`。
- 使用主构建的实际 Ninja 编译参数，`RemoteIMApplication.cpp`、`MaiChatHostTools.cpp` 与 `MaiChatHostToolsTest.cpp` 的独立 MSVC `/Zs` 语法检查均退出 0；测试 MOC 在私有目录生成，未写主缓存对象或 PDB。这不等同于完整链接和执行 Host/App 集成测试。

10k/100k 合成历史均分布于 100 个 peer，包含普通文本、中文拼音匹配文本和一条较早的唯一命中。
以下是同一 Windows 环境中，数据生成后执行的样本；不是实际账号或冷磁盘性能保证。毫秒为整数，0 表示不足 1ms。

| 操作 | 10k 条 | 100k 条 |
| --- | ---: | ---: |
| 单 peer 较早的稀有命中 | 0ms | 3ms |
| 跨 peer 较早的稀有命中 | 3ms | 34ms |
| 跨 peer 拼音无命中全扫描 | 2ms | 35ms |
| 拼音命中第一页 | 0ms | 0ms |
| 连续三页倒序游标，每页 17 条 | 各 0ms | 各 0ms |

实际 `EXPLAIN QUERY PLAN`：单 peer 使用 `idx_messages_peer_page`，跨 peer 使用 `idx_messages_page`，均没有临时排序树。
活动读事务下的新消息写入为 0ms。
100k 数据上，1ms 预算在 1ms 返回 `query_timeout`；执行中的取消在不足 1ms 返回 `canceled`，两者均不返回 `no_matches`。
工作线程查询期间，主事件循环得到 3 次心跳，最大间隔 15ms，总观察 60ms。

## 上一轮主构建与集成回归（UI 移交预算修复前）

统一构建已在正常缓存 `MaiChat/desktop/.cache/build-qt6-ui-20261109` 完成，构建命令为：

```text
cmake --build E:/OpenSource/MaiChat/MaiChat/desktop/.cache/build-qt6-ui-20261109 --target maichat maichat_host_tools_test local_message_database_test remote_im_application_test message_search_test agent_panel_session_test --parallel 4
```

退出码为 0。`maichat` 的 POST_BUILD 已完成 `windeployqt` 与 PE 检查，报告 3 个二进制、114 个 PE imports 与 Qt plugins 验证通过。
源码 HEAD 与逐文件 SHA256 记录在 `composite-source-manifest.json`；构建和测试后再核对，清单内源码未变化。
生成的六个 EXE 的 SHA256 记录在 `composite-binary-manifest.json`。这些清单及以下原始日志均位于 `.cache/message-search-20261113/`。

| 正常构建运行项 | 结果 | 原始日志 |
| --- | --- | --- |
| 完整 `LocalMessageDatabaseTest` | 36 通过，0 失败 | `composite-local_message_database_test.txt` |
| 完整 `MaiChatHostToolsTest` | 18 通过，0 失败 | `composite-maichat_host_tools_test.txt` |
| 完整 `RemoteIMApplicationTest` | 44 通过，0 失败 | `composite-remote_im_application_test.txt` |
| 完整 `MessageSearchTest` | 21 通过，0 失败 | `composite-message_search_test.txt` |
| 欢迎页及先前 MIME、布局相关槽位 | 18 通过，0 失败 | `composite-agent_panel_session_test.txt` |

以上进程均退出 0、无跳过。Host/App 实际链接后的回归覆盖：65 条完整数据库分页与 1/40 条 UI 窗口隔离、专用线程池不受全局线程池阻塞、发布前取消、账号和连接代际边界。
正常构建的 100k 稀有全局查询为 35ms、无命中查询为 36ms；专用查询在全局线程池被阻塞时仍完成，测试观察 50ms（包含 QTRY 轮询粒度）。

欢迎页仅运行指定的自建模型与普通 MIME 输入槽位，没有运行接触系统剪贴板的其他测试。
`composite-ui/welcome-tool-list-1100.png`、`welcome-tool-list-700.png`、`welcome-tool-list-550.png` 已截图并人工查看；这三个数是逻辑宽度，Windows 150% DPI 的实际图片宽度为 1650、1050、825。
三个宽度均无水平滚动；逐行测试检查了 19 类工具的文案、换行和点击只追加当前会话草稿。
已有 `emptySessionShowsWelcomeAndFirstMessageHidesIt` 槽位输出一次非致命 `QFont::setPointSize(-1)` 警告，新增欢迎页槽位未输出该警告；原始日志保留此记录。

## 后续复审：主线程移交也纳入预算

独立复审发现，上述主构建虽然验证了工作线程 SQL 和线程池预算，但工具仍通过通用 `BlockingQueuedConnection` 等待主线程，五秒计时到 `queryLocalMessages` 才开始。
主线程繁忙时，这段等待没有预算，工具的取消轮询也无法启动；不能据上一轮结果宣称端到端五秒界限。

新回归主动阻塞主线程事件队列，在工具线程分别调用 search/get，并测试取消、五秒预算和账号代际撤销。
旧实现六种情况均实测 RED：取消和账号撤销约 0.6 秒后仍在等待，预算情况约 5.63 秒后仍在等待；放开队列后各出现两次数据库路径的身份读取。
测试先排空旧实现的队列再断言失败，避免失败用例本身留下等待主线程的 future。

现在仅这两个只读查询使用排队调用和自有完成状态；通用写操作的 `onApplicationThread` 未修改。
五秒绝对预算在交给 UI 之前创建，原样传至普通 App API、线程池和 SQLite；工具线程的可取消条件等待不依赖 UI 处理事件。
排队函数和迟到的 SQL 完成函数只持有自有 request、共享取消标志、预算与账号代际租约，不捕获原生 `MaiToolContext` 或其取消裸指针。
工具提前返回时标记 abandoned；UI 之后恢复也不进入过期请求的账号或数据库路径。

`.cache/message-query-handoff-20261113/` 保存独立构建与回归：

- `handoff-red.txt`：3 通过、6 失败，退出 6。
- `handoff-green-final.txt`：完整 Host 套件 25 通过、0 失败、无跳过，退出 0。
- search/get 取消分别在 107/113ms 返回（包含触发取消前刻意阻塞的 100ms）；账号撤销为 129/115ms；预算为 5010/5001ms，包含线程调度精度。
- 六种情况均在主线程仍被阻塞时返回；随后释放队列，身份读取次数均为 0。
- 原生取消标志的内存由工具线程在返回时立即释放，再排空 UI 队列；预取消和普通 API 已过期预算对照均不进入数据库路径。
- `frozen-source-manifest.json` 保存本轮四个源文件与独立 EXE 的 SHA256；`private-green-build.log` 保存编译链接日志。

该独立 EXE 重新编译本轮 Host、App、Qt MOC 与测试对象，读取上一轮依赖库，不写主 Qt 缓存。
本轮源码已冻结并把 `RemoteIMApplication.cpp/.h` 所有权交给后续功能生产者；本轮修复及后续功能尚须统一正常构建验证，上一轮产品 EXE 的清单不能替代这一轮。

最终联合普通构建已闭合上述待验证项：原生客户端 Release `maichat`、部署脚本和 PE 依赖检查通过，
同源 Host 25/0、DB 36/0、App 44/0、matcher 21/0；新绑定与原有安全 UI cohort 合计 255/0。
后续机器发送/媒体准备的附加代码没有改查询主体，独立评审以扣除附加段的方式逐 SHA 核同冻结的 App CPP/header。
最终常规 EXE SHA `1fdc7aa7c796cdec1dc85ce4a205ecb9176b62d4ecdf1b4c7a065615a37591e3`；
原始 source/command/binary 清单在 `.cache/session-im-bridge-20261113/verification/`。

## 验证边界

未启动或停止用户的主 App，也未裸跑 `build/bin/maichat.exe`；检查时没有运行中的 `maichat` 进程。
完整 staging 与实际 ZIP 解压目录的 `--smoke` 仍须等待主流程的打包产物就绪，不能据组件测试宣称安装产物和真实账号流程已经验收。
未对真实账号库执行 WAL 或索引迁移。macOS、iOS、Android 的实现及设备验收由对应平台验证单独记录。
