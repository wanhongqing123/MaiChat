# iOS Session 绑定与历史导入：2026-11-14

## 需求与操作

在已接受好友的私聊中打开加号，选择“绑定会话”，从动态本机会话列表选择并确认。先把本机持久化的该好友双向聊天记录作为历史上下文导入；导入不执行模型、不逐条回放回复。完成后，新 Human 单聊消息沿原精确 turn/assistant 收据自动回发 Machine 消息。群聊、控制帧和 Machine 消息不进入 live 执行。

删除独立“AI 帮我回复”建议链。绑定图标复用原紫色 sparkles；选择页复用 App 的 RemoteIMStyle/AppTypography、蓝白面板和主按钮。移动欢迎页仍保留卡片。

## 源码和交付边界

本轮在 `d2d4ffb33eb338244240831c5cf3fc7fffe9dd0c` 上消费 Root 精确清单，保留原 docs/index；最终统一 commit 尚待协调端冻结。不可把 base commit 写成已包含全部 overlay。

- 旧 live-only Swift v4、Native14/Security 产品于真机从 206 保数据覆盖到 **2.2.4/207**；install exit0/12.380s、普通 launch exit0、PID58659，并读回进程 exe 与装后 bundle URL 一致。
- 署名构建 exe SHA256 为 `e2efd4b5b7bafc740247bddf835e889eba94f31e8d503fdcd85fb96a89ca36b8`；这是本机构建字节，不是从设备容器重新读出的字节。
- 207 构建的 iOS/Core/Mobile/SessionIM 树分别为 `d02e5f5055bb92b97b4a3ffd5b682429ff69434c`、`dc3f528c751e9f423fcb59f5210498552f920d5d`、`aca0d657356ceff519d3d4e47d9067abc7cd2c23`、`e7c07f83d503e1933ad113934f0761b41a549aa7`。
- 207 不含后来新增的完整历史导入和现场红字修正。以下 208 才包含这次完整范围；Simulator 功能回归与真机安装/运行读回分别记录。
- 没有停止/替换现装 Mac App，没有新生产账号、真实双账号消息测试或付费模型请求；GitHub 发布继续等待最终组合源与平台供件。

## 现场红字与样式

用户在 207 的绑定页看到列表已加载但底部仍显示“模型配置正在准备，请稍后重试”。同一 backend 的20个普通并发 open 实际得到19个 busy 错误；成功 refresh 后旧提示仍在，同例 RED。修正为同账号共享一次准备结果；会话列表仍每次读 Native 当前 projection，不缓存成固定列表。恢复只清旧读取错误，Native warning 和真实操作失败分别保留。普通同例 GREEN，并验证新建会话会更新列表。

旧入口普通 UIKit RED 后，新入口/选择/取消/解绑/草稿保持均在正常 Debug App 回归。选择页后来按用户要求改用现有群聊创建页的 App tokens；仅图标保留紫色。所有图像均来自自有 Simulator 页面，不拿 Windows/Mac 图代替。

## 持久化与 bootstrap 验证

SDK 原入口先登记身份/ingress 序列和不可变绑定 epoch，随后才解析/排队。原元数据经普通 SQLite 写 actor 同事务确认成员并插入后，才允许媒体下载；ready 保留原 DB insert 收据。真实 remote ID 与本地 UUID 分列，不拿 media UUID 冒充 SDK ID。重复旧 DB 行通过 persisted_input(false) 正常退出 live 槽。

共享 C++ reader 在 preparation worker 上使用固定 WAL 只读快照跨页。所有 normal projection 的 one-shot history_tasks 在统一 accept 中接取；whole task 不改写，progress 与 import_id 分离。EOF 后关闭 reader，Core idle 后提交并释放同 epoch live 屏障。临时2-reader背压重试有界，SQL/取消/身份错误不返回成功空历史。

已实际运行的自有范围：

- 普通 Core Memory/SQLite staged context、原生 coordinator19/0、kernel15/0、snapshot13/0、BindingTool 和完整新 wrapper CABI selector均通过。
- wrapper 初次失败是测试夹具8列与正式 Desktop schema 不符；私有同库 probe 明确 `no such column: caption_above`。只补测试 DDL 后同 target/selector通过，正式 reader 门禁未放宽。
- 普通 Swift metadata/fence三个 unit 通过：DB成员确认跨 UI 重建、缺失媒体/无假remote ID、捕获序列和换登录代际。
- 自有205条双向同毫秒历史跨页、绑定前 raw 元数据 fence、导入中 live 重叠：导入206条时模型调用0，之后一条 live 恰有1次 loopback 模型请求且能读取历史首尾。
- 增加数据库旧 SDK 重放与32-slot溢出后同入口通过：旧行不重复执行，第33条明确使整次 bootstrap 失败，不假称 ready。模型请求均为本机脚本服务，非真实网络 E2E。
- 实际 TencentIMClient raw 回调＋自有 SDK 消息对象验证普通 DB commit 先于 getUrl；故意失败的媒体仍有历史行，同 SDK replay 的 DB收据为 false 且不重复下载。没有调用真实 SDK init/login。

导入保留全部本地 raw 记录；下一条真实请求仍遵守正常模型上下文预算，不承诺任意长度历史能塞入单次模型请求。没有查询真实聊天数据库。

## 最终持久化与共享消费者

独立审查发现：旧 fence 在失败发生后才 capture 时，把失败次数当作新基线，可能漏掉尚未补齐的消息。自有 SQLite 第二连接持有写事务，使真实 writer 失败，再 capture，得到实际 RED。修复后按同一来源消息保留失败债务；其它消息成功或同代际再次 activate 不消债，只有原消息真正写成功或来源代际明确失效才解除。原 SQLite 解锁重试、其它来源与代际三个定点用例 GREEN，正常 App 也消费此源。

最后的 Apple 必要验证使用同一普通库与既有缓存：

- 生产 snapshot reader 15/0，包含 iOS 旧 NULL/空 peer 行；自有 20,000 行分页实际 SQLite VM 步数 465,850，用时 93ms。这是测试主机与自有库结果，不当作手机性能结论。
- 普通 Native coordinator 23/0，包含终页到达前过期、等待 idle 期间过期、已授权真实 Store commit 期间过期三个边界。已经开始的 SQL 提交可能完成，但过期绑定不被标成 ready，也不发送后续回复。
- Core Memory/SQLite context、450 条重复绑定/取消/重启清理及真实 CABI selector 通过；旧副本按每批不超过 200 条回收，原 IM DB 行保留。宿主在无绑定但 history_cleanup_pending 为 true 时仍继续普通 pump。
- 维护版正常 Debug App 五槽 0 失败；最终 reader/expiry 源再跑历史导入与实际 raw SDK 两槽，build exit0/18.272s、test exit0/17.133s。没有扩大到真实 SDK 登录或聊天数据库。
- 一次测试启动因已有结果目录退出 64，未运行 case；保留原错后，同一已编译 App 换唯一结果目录运行五槽通过，没有把目录冲突计作功能 RED。

## 真机 208 交付

装前重新读回真机实际为 2.2.4/207，选择更大的 build override 208，版本源文件未改。复用原 device-derived normal generic Release 缓存及现有模型；旧 207 App 用同卷 clone 保全。

- 正常 Release 首轮构建原日志为 BUILD SUCCEEDED，但私有回执脚本提前读取未写完的树元数据，写 JSON 时 KeyError，包装退出 1 原样保留。修正脚本后同源普通增量确认 exit0/18.128s，未重跑功能测试。
- deep/strict 签名、未过期且包含目标设备的 profile、10 项升级 entitlement 均核合；20 个实际 normal Native 对象与合库成员及最终 Ld 核同，9 个 MachO 的 Release 测试入口负检和 Debug 阳性对照通过。7 个原模型资源 inode/bytes 复用，没有复制大模型。
- 本地署名构建 exe SHA256：`706435d891b0ee1e20386b184007c1a6868f645cc07271cab8521e1955846ca4`。
- 原包保数据覆盖 install exit0/12.448s；装后读回 **2.2.4/208**，普通 launch exit0/0.468s、PID58926；3 秒后进程 exe 与装后 bundle URL 一致。没有卸载、清数据、读出设备容器字节或查询设备 DB。

本轮基于 d2d4 加精确 overlay；下列产品树已由协调端逐项核同，待统一源码提交。构建 exe SHA 不冒称从设备重新读出的字节 SHA。

| 产品 | 树 SHA |
| --- | --- |
| iOS | `3aa5c00502a3110fc999938259a456e6d06a5cee` |
| Core | `37e6783a52494fe76fef9df981af92290ca5fa3b` |
| Mobile adapter | `f4ee11b6bf7571b778b890b36c92b157c8cc3827` |
| History | `8a23692d013d86d4f45516aa6cad7aad3e48b18d` |
| SessionIM | `f3d3bff0cd3c12c1bb2e8ea3075df622cc233036` |

以上覆盖自有数据功能回归及真机正常安装/启动；真实双账号、真实模型消息往返和用户实际好友历史内容未操作，继续保留现场 E2E 未验边界。Mac 现装程序未退出或替换；GitHub 桌面发布仍待最终组合版本。


## 最后维护修正与真机 209

对已装 208 的正常库追加真实 SQLite 写锁定点验证：在隐藏暂存 200 条后，独立连接持有写锁 6500ms，超过既有 busy_timeout。旧 Native 的 reap 先 swap 掉待 abort，失败后没有保留任务，出现隐藏行仍 200 而 cleanup_pending 为 false 的实际 RED。最后窄修保留同一待撤销任务、每普通请求最多尝试一次、1 秒 backoff；NotFound/Canceled 终结撤销步骤，但仍安排同 Session 的有界 purge。维护 bool 包含待 abort 队列。

Apple 普通 Native 同 24 组通过，原锁例读到 unbind5290ms、first_rows200、pending_honest1、final_rows0；原 IM 450 条和模型调用0也通过。真实 wrapper selector 通过，没有改变 Core、reader 或公开 CABI 字段。

Swift 原 warning 槽实际 RED：维护重试被显示为“消息未提交”；只将 history_cleanup_failed 分类为本地固定维护日志，pending=false 重置该维护类别，真实 live 告警仍显示并保留。同一测试字节 RED1→GREEN1，最终普通新 Native App 再跑该槽0失败，没有重跑其它 iOS 组。日志不含账号、消息内容或对象名；测试入口仍仅 Debug。

2026-11-14 最终交付：装前实际读回 2.2.4/208，选择 override209，版本源不改；复用原 normal generic Release cache，build exit0/108.689s。strict 签名/profile/10项升级 entitlement、20个真实 normal Native 对象与合库/Ld、9个 Release MachO 的测试入口负检、7个模型 inode/bytes 复用均核合。

- 保数据覆盖 install exit0/11.424s，装后版本 **2.2.4/209**。
- 普通 launch exit0/0.563s、PID59100；3秒后进程 exe 与装后 bundle URL 一致。
- 本地署名构建 exe SHA256：`e4d331ccfab09cafdda4d1a7e8ef7c265fcb45ae38497970eb9143b9b395704b`；没有从设备容器重新读取字节，没有查询设备 DB。
- 实际 build source 为 d2d4 加精确 overlay；5个产品树与最终组合提交 `32a606d307636be06a715ada7f64ae7d0ff49761` 逐项相同，随后安全 ff，不为相同 iOS 产品树重建。
- 最终 iOS tree `3425d8abf24fc07ab2c4ed978c14503cf80c43af`，Mobile tree `a16a042dc839402940c95e64281ac8aacb47dd9b`；Core/History/SessionIM 仍为上表值。

208 与其原验证记录保留为前一实际安装，不倒填为含最终维护修正。真实双账号/模型网络 E2E 仍未操作，Mac 现装仍未退出或替换。
