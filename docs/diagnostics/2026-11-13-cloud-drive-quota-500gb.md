# 每用户云盘配额上调至 500GB

用户明确要求“10G太少了，一个用户500G吧”。2026-11-13 由 Mac 端唯一执行后台配置，Root 只做源码及回执复核，没有并发操作服务器。
源码审读基线 `63f4a8d53c302ef4153b5e486b5543ec3478e282`；这次没有改应用产品源码、候选或版本。

## 配置与配额语义

以既有十进制单位设置 `MAICHAT_CLOUD_DRIVE_QUOTA_BYTES=500000000000`。
持久配置为 `/etc/systemd/system/maichat-auth.service.d/95-cloud-drive-quota-500gb.conf`，root 所有、0644；
配置 SHA256 `74e4cac8ff8dbb9bd42abfc6b8384336d8cd63cb570d76deedda4fce1576cc12`。
持久值与实际 Auth 进程环境值均为 500,000,000,000；原 env 文件未写入，也未输出其它环境变量。

`cloud-drive-service.js` 的每用户配额返回值直接使用当前 service 的 quotaBytes，没有把 10GB 固化到各用户行。
上传事务以 `used_bytes + reserved_bytes + request_bytes` 对比同一配置，因此已有账号和新账号都适用。
三个客户端正式显示取服务响应 limit_bytes；原 10GB 常量仅用于初始/重置/缺字段占位，没有本地总量拦截，无需为配额重建安装包。

私有内存数据库边界通过：旧 10GB 以上仍接受上传；498GB 已用加 2GB reserved 后，再增加 1 字节返回 413。
没有创建生产测试账号、新登录或请求 OSS。会话附件配额 10GB、单视频上限 2GB 保持原设置，OSS 生命周期与现有对象未改。

## 生产回读与实际重启边界

2026-11-13T15:02:34Z，使用同版普通 GET handler、实际运行环境以及生产 SQLite readonly/query_only，
读取一个已有且有 ready 文件的活跃账号配额；设置游标使返回对象数为零，不输出账号 ID、对象名或 token。
回读 limit_bytes=500,000,000,000，reserved_bytes=0，生产库只读，OSSRequests=0。

预检漏看了 Sync 对 Auth 的 systemd 依赖。正常重启 Auth 后 Sync PID 也改变，首次 apply 封装在
“其它服务 PID 不变”的断言退出 1，stdout 为空。原 stderr 没有落盘，未补造 Traceback，也不将该封装记为全通过。
配额已实际应用；之后仅做独立回读，没有为了修正回执而再次重启或修改 unit 依赖。

2026-11-13T15:09:07Z 最终检查：

| 服务 | PID | 状态 | NRestarts | HTTP 健康 |
| --- | ---: | --- | ---: | --- |
| Auth | 160473 | active/running | 0 | 127.0.0.1:9100/health，200，0.005s |
| Sync | 160474 | active/running | 0 | 127.0.0.1:9102/health，200，0.004s |
| Media | 151848 | PID 未改变 | 未作新结论 | 未作新结论 |

实际 Sync unit 有 `Requires=maichat-auth.service`，After 包含 Auth；PartOf/BindsTo 均为空。
没有把该联动误记为 PartOf，也没有用 active 代替 HTTP 健康证据。

原回执为 remote-im 文件 `1794582396-35345976.json`、`1794582668-{84818781,63475545,72922940,78704370}.json`。
正文只记录非敏感容量与服务事实，不记录凭据、生产用户或对象信息。Mac App 退出重启与此配置授权无关，未联动执行。
