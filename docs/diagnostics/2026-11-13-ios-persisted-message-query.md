# iOS 本机 IM 查询与入队身份隔离

2026-11-13，基线 `583a07bc48c53759a089ae8097184730eedf2a94`。
使用正常 Simulator 构建、自有 SQLite/loopback fixture，不读取用户聊天库或调用模型。

## 复现与修复

旧正常宿主执行产生四个实际失败：其它联系人 200 条新命中遮住目标联系人旧命中；首屏没有续页；
DB 路径失效返回成功空结果；get 丢弃 hasEarlier。工具改为同一个普通持久化页 API，peer/时间/
kind/direction 在 SQL LIMIT 前过滤；7 列 DTO 不还原附件、准备媒体、合并 UI 消息或初始化 Agent。
仅覆盖当前 SDK/owner 的本机单聊，不代表云端全历史、群聊或 Agent Session 上下文。

排序、筛选及游标统一使用 `ROUND(created_at*1000)` 整数毫秒和 id 降序；正常迁移建立对应表达式索引。
游标绑定 owner/SDK/history/auth/DB 实例与全部查询条件，DB 错误、取消及换号不能成为 `[]` 或完整无匹配。
两个独立只读 worker 配合 WAL，查询不占串行历史写 actor；总期限从 native callback 进入 MainActor 前开始。

新增回归依次发现并闭合：

- 旧 callback 无限等待 Main，主线程恢复后仍交付过期成功。现在五秒原生等待有界，ARC response box
  与 abandoned flag 防止迟到 Task 发布；C 字符串先复制，任务不借用原回调栈或已释放 raw context。
- 旧 A 查询在 Main 排队期间切 B，会采用 B 身份。入队前捕获线程安全的可信身份 lease，身份发布/
  鉴权/history 变化即失效，执行、flush、DB 回传与序列化后都复核。
- 旧 Backend context 本身晚到 B 时，全局身份仍可能让它采用 B。注册 context UUID 与 lease 共用固定
  锁序，替换/释放会废旧 revision；旧 context 被拒绝，当前 B 普通 callback 仍可查询。

两种换号反例均先在旧普通源码读出自有 B 标记而真实失败，最终用零 B 读取/当前 B 正常控制闭合。
同步 C ABI 不宣称能强行抢占单次 OS I/O；native 前后取消守卫和独立期限分别有验证。

## 最终证据与边界

7 源首件 SHA `9fc95039...`，两份逐 blob/canonical 核合的相对 delta `d666e8...`、`b6dfad...`。
普通最终宿主 4 槽全通过；LocalChatHistoryStore 原完整组再次 28/0。
自有 100k：global 2.643ms、peer 1.205ms、rare 21.339ms、no-hit 39.556ms；两类 EXPLAIN
走对应表达式索引且无临时排序，持有读事务时普通写入 2.560ms。不是 iPhone 硬件耗时承诺。

正常 Release Simulator 最终成功 104.930 秒，exe SHA
`80dbc461c2ea762e04ace31c27203a9188797ee4b4639d675eab8f5a67f34871`。
9 Release MachO × 7 测试 marker 全阴性，Debug 11 镜像有相应阳性；正式查询字段与新 Native 符号存在，
对象→Core/合库成员逐字节相等、最终 Ld 消费核准，模型原 inode/长度复用，没有复制大模型。
两次 libtool 磁盘不足单列为环境失败，不倒填为功能通过。

Root 收到的原日志/manifest 及应用核对回执在 remote-im 文件目录与 `.cache/ios-message-search-*-apply-receipt-20261113.json`。
这是正常模拟器和普通 Mac C++ 消费者验证；未查询或安装真机，新版 iOS 真机交付仍等待连接恢复。
