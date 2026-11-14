# Session 单张临时图片重新发布的 Auth 控制面

- 时间：2026-11-15，Asia/Shanghai；平台：Windows、Node v24.14.0。
- 基线源码：`38480181`，本次基于其工作区增量；最终三个生产文件的 SHA-256 见私有验证收据。
- 目标：用户显式要求某一个消息/图片片段重新加载时，仅已 ACK 的当前所属设备下一代 pending 能申请新对象。账号服务只处理授权、元数据和签名控制，不代理媒体字节或模型请求。

复现：为同一个 owner/Session/message/part 创建代 0 临时图片并固定到期；到期后确认旧 init 返回 410。随后由 Sync ACK 合法下一代 pending，再用原 upload body 初始化。旧实现始终先取代 0 过期回执，无法进入下一代授权；loopback 适配器还把带 generation 的授权压成布尔值。

修复边界：

1. Auth 固定 loopback 适配器发送 `X-MaiChat-Image-Generation: 1`，要求 create HTTP 响应显式包含安全整数 generation（含 0）。缺代际的旧 `{ok:true}` 不得授权新分配；只有可信进程内普通 API 的既定布尔 `true` 回调兼容代 0。公网 body 和 header 不能覆盖服务端授权代际或 capability。
2. `session_image_publications` 保留代 0 的原始表与回执；只增加 `session_image_publication_generations`。新表以完整所属身份/代际建主键，并对账号、名额、Session、消息、片段和代际增加唯一约束，重登录不生成同一代的第二个对象。
3. 新对象仅按受信 Sync grant 预留；每代固定 24 小时、并发初始化幂等，同代过期仍 410。Sync 拒绝或不可用时，兼容路径只返回已存在的回执，不分配新对象。
4. 未完成 Multipart 根据 attachment ID 查询其服务端映射代，在签名、列表与完成边界重新核对当前 create grant。已完成的原有幂等完成响应不延长生命周期。
5. 普通到期清理保留两表回执。真实房间删除及账号删除沿用既有授权、OSS 缺失核验和 24 小时保护期规则；续期不删除旧行来绕过幂等。

正常调用关系已经用本机 Auth 与 Sync HTTP 串联验证：首次 pending→代 0 上传/完成/ready→服务端固定到期→合法下一代 pending 的 ACK→typed grant→唯一新对象→新 ready 与所属端离线读取；旧图仍 410、原回执字段不变。另以没有 capability 的真实旧布尔适配行为，验证新登录、没有代 0 回执、代 1 pending 的场景无法新建代 0。共享原生七字段请求没有加入 generation。

迁移/回滚：测试从旧表数据迁移，检查原表 SQL 和旧回执完全保留、重开数据库后新表仍在、同一代跨登录重复插入被拒，以及旧版本只访问代 0 表的读写仍可工作。热回滚旧 Auth 时，新的 Sync 通过 capability 门禁对代大于 0 的请求返回 409。另经 HTTP 切换到基线旧 Sync 的真实代际盲 create 授权分支，复现了 g1 已 ACK 但 Auth 尚无回执时误建 g0 的窗口；修复后固定标记适配器因缺少 typed generation 拒绝分配，startUpload、g0 和 g1 新行均为 0。新 Auth 遇旧 Sync 仅降新分配可用性，已有回执仍可幂等返回。切换时应配对 Auth/Sync 版本，不删除新映射表或假称跨二进制全兼容。

离线验证记录位于 `.cache/android-image-republication-20261115/`：

- `auth-generation-red.log`：六个关键行为原实现均失败；`auth-generation-green.log`：对应关键授权、迁移和幂等用例通过。
- `auth-rollback-red.log` / `auth-rollback-green.log`：capability、跨登录降代以及唯一映射约束的红绿记录。
- `auth-renewal-integration.log`：三条本机正常 HTTP/Fake OSS 链路通过。
- `auth-old-sync-red.log` / `auth-old-sync-green.log`：真实旧授权分支的 HTTP 回滚窗口及严格 typed grant 的红绿记录。
- `auth-full-node.log`：首次全量 219 项中，218 通过，唯一失败是既存云盘迁移测试在 Windows 先删临时目录、后关 SQLite 的 EPERM；该 fixture 在 `38480181` 已有同样 hook 顺序，单例复查也失败。
- `auth-folder-migration-green.log`：仅合并清理闭包，先关闭可能打开的数据库再删除目录；业务断言未改变。最终全量结果与源码哈希另见验证收据。

未执行部署、进程重启、线上数据库迁移、真实 OSS 或密钥操作；这些离线测试不代表生产服务已经更新，也不代替客户端真机的单张图片重新加载验收。
