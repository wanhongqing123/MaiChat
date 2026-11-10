# Remote Session 临时图片服务端验证记录

## 现场操作与预期

2026-11-10，Windows PowerShell；基线源码提交为
`28977e7a42e5bbb680505e5aa7c8aef9ebe0b240`，直接在 main 工作区开发，保留既有 computer-use 修改。
现场问题是远端 Session 中助手的文字—截图—文字只能显示“图片在所属设备，暂不可跨设备预览”。
用户最后确定的语义是新图片首次发布上传一次至临时 OSS，随后查看无需所属设备在线；
到期不重传，本地完整缓存可继续显示，未缓存显示“图片已过期”。禁止全历史自动上传。

本记录仅覆盖服务端实现和本地验证；没有安装或运行目标客户端构建，没有实机 UI 验收，
没有访问线上账号、OSS、RAM、凭据或实际服务配置，没有部署或更改线上生命周期规则。

## 根因证据

- `DesktopSessionSyncHost::serializePart` 原来只发布图片的安全文件名、MIME 与 `remotePartId`，
  故意不公开本机路径。已有跨设备历史没有可直接下载的图片对象引用。
- 现有 `session_attachments` 只为未完成上传设置七天期限，ready 对象没有临时保留期限。
- 原附件 `roomKey` 只验证 owner slot/login/Session 的格式，未在 Sync DB 验证具体消息和 part。
- Sync 的旧附件保留逻辑只照顾 user 消息；助手后续文字快照需要保留同位置图片的独立发布回执。

## 实现

已有会话附件 Multipart 控制 API 新增 `purpose:temporary_session_image` 与明确的
owner slot/login、Session/message/part 身份；对象使用独立的 `session-temp-images/` 前缀。
账号服务在首次预留时分配固定 24 小时 `expires_at`，保留幂等发布身份；重复初始化仍返回同一
Multipart 身份和原期限，expired 身份不能创建新对象。上传和下载字节继续由客户端直连 OSS。

所属设备必须先成功发布 pending 图片，账号服务通过固定 loopback、当前 Bearer 请求 Sync
的 `authorize-attachment` 控制路由，在它自己的数据库中核对精确消息/part。
ready 读取再次核对引用，不以所属设备在线状态为条件。两个消息角色共用相同 `remoteImage`。
后续文字快照保留原图引用与顺序；对象编号、属性和期限不能替换。

签名读取最多 60 秒，分片 PUT 最多 300 秒，都进一步截短到原始期限；
签名过程中到期也返回 `410/image_expired`，不会发出零秒链接。完成上传跨过期限会删除迟到对象，
并保留原有 24 小时完成竞态保护和 revision。普通过期保留小型身份墓碑，防止重建；
明确删除房间后，在相应 OSS 对象核实消失后才清除该房间的发布身份。
清理前通过同一 Sync 授权路由核对当前所属登录，以及会话目录和房间消息均已删除；
仍存在 pending part 的房间不能借附件清理来重置原身份。该场景已用真实本机 HTTP 链路复现并回归。

沿用启动时和每小时清理队列，核对 exact-key Multipart 已空、DELETE 成功且 HEAD 不存在后
才释放对象元数据和配额。独立前缀的 RAM 与生命周期工具已补齐；生命周期分别配置完成对象
一天到期、未完成 Multipart 一天中止，并备份、检测配置漂移、合并和读回整个规则列表。
业务过期、签名期限和最终物理删除是三个独立事实，物理清理允许异步重试。

Root 复核还发现生命周期工具没有复核 Bucket 版本控制；仅删除当前对象不能证明历史版本
已清除。新增同例先得到 7PASS/2FAIL，再在首次读取和备份后的写入前检查 pinned SDK 的
`versionStatus`。Enabled、Suspended 或无法确认的响应均拒绝写规则，不改变桶的版本配置。
修正后的 signer/原生命周期/临时生命周期合计 27PASS/0FAIL；日志为
`.cache/remote-session-temp-images-20261110/lifecycle-version-{red,green-all}.txt`。

## 验证过程与边界

本地使用锁定的 npm 依赖，Node `v24.14.0`；账号服务原生 `better-sqlite3` 已重建。
签名服务安装使用相同版本及 integrity 的缓存依赖，仓库 package-lock 未修改。

新增测试先观察失败，再实现并用相同断言通过：固定期限/完整身份/幂等、拒绝路径与调用方期限、
账号和 part 隔离、过期拒绝读取、删除失败保留记录、迟到完成、签名期间到期、房间清理后的身份回收。
Sync 测试覆盖用户和助手、错误身份、所属端离线/重新登录、助手文字更新和对象不可替换。
本机真实 Auth/Sync HTTP 测试覆盖 pending 发布前拒绝上传、ready 精确授权、所属端离线读取和删房后拒绝。

执行命令及最新结果见本地
`.cache/remote-session-temp-images-20261110/verification.md`。
全服务测试中发现的 Windows 现存清理问题需要单独记录：云盘目录迁移测试先执行删除目录 hook，
后关闭 SQLite，导致 Windows `EPERM`；素材 OSS smoke 测试对目录 fsync 返回 Windows `EPERM`。
这些原有测试/脚本未纳入本次修改。真实 OSS 的私有权限、签名行为、最终清理和三端 UI 仍需部署后验收。
