# 启动时好友列表无效数据弹窗

## 现场与预期

2026-11-28 用户提供 macOS 截图，并确认“启动或登录后自动出现”：IM 弹窗内容为“好友列表加载失败：网络连接失败：登录服务返回了无效数据”。已有会话列表及消息仍可见。尚未读取用户好友列表或令牌，不能据图推断腾讯 IM 连接中断。

预期：登录后正常加载账号好友列表；失败时保留已有数据，并准确区分网络传输失败、HTTP 服务错误、限流与响应格式异常。修复需解决实际请求失败原因，不能仅隐藏弹窗。

源码基线 `be27b3c26e35a44a1a768c21cee250d7843d756b`，正式 4.0.2 产物来自 `74470f39c5fc3b6c088bd87770f9bfb2c864dc42`。本地根工作区除批准 Qt overlay 和已有私有 untracked 外无他人待提交源码。截图中的实际运行二进制待核对，不直接假定来自正式包。

## 已定位调用链

`RemoteIMApplication::refreshFriendList` → `accountFriendOperations.list` → `DesktopAuthClient::fetchFriends` → `authorizedRequest("friends", "GET")` → `request` → `DesktopCurlAuthTransport::perform`。

正式地址为 `https://ichat.life/api/v1/friends`。`request` 在响应不是 JSON object 时写入“登录服务返回了无效数据”；`responseError` 除少数状态分支外会将该错误统一前缀为“网络连接失败”。因此现有文案不能给出实际 HTTP 状态或正文类型。

4.0.1 到当前提交的 `DesktopAuthClient`、`DesktopCurlAuthTransport` 和账号好友服务代码无差异；新的 session 存储切换不在此调用链上。

## 调研范围

- Mac 同行只读核 public/local 无授权响应状态、类型、大小、JSON shape，以及实际服务源码和代理路由。禁止输出响应正文、用户令牌、API Key、好友资料或线上 DB。
- 核对线上 Nginx 限流、错误页与代理映射；仓库示例的通用账号路径有 60r/m 和 burst15，不能据示例直接推断线上配置或认定限流为根因。不发压测。
- 若需要实际会话或生产修改，先明确范围；当前不重启/停止用户 App 或线上服务，不改 env、账号或数据库。

## 只读现场证据

Mac 实际运行 `/Applications/MaiChat.app` 4.0.2，exe SHA 前缀 `0e6b30cb` 与正式包相等。Auth active，health200，线上 `service.js` SHA 为 `6ad0b0c931eb96575cb7ac3962a0cc0fa1926b9af89c5432ee761361ad1353c0`，好友路由及帮助模块存在。服务器本机和正常 TLS 公网的无授权 friends/me/model-api-keys 均为 401、35 字节 JSON object；没有持续的路由映射或响应 shape 异常。

实际 Nginx 通用 `/api/v1/` 为 `limit_req zone=maichat_auth burst=15 nodelay`。最近一小时恰两条该 zone 的拒绝记录，均北京时间 02:28:46，其中一条为好友请求，最大 excess15.752，与用户启动报错时间相符。该 error log 本身没有 HTTP 状态字段，不能把历史状态推断写成已读取到的状态。

[Nginx 官方模块规范](https://nginx.org/en/docs/http/ngx_http_limit_req_module.html#limit_req_status) 给出的默认限流拒绝状态为 503；完整线上覆盖配置与同二进制隔离复现由 Mac 继续核验，当前不对公网发送突发测试。

Root Windows 普通系统 curl、Mac 系统 curl 的此域名直接探测均存在 HTTP 前 TLS 失败；这不是接口无效 JSON 的证据。Root 使用正式 4.0.2 的原生 Auth transport 和正常运行库后，三个无授权正式 GET 均为 401/35B/JSON object。未关闭证书验证，未发送真实账号凭据。

## 最小客户端复现

Root 用冻结的正式 `DesktopAuthClient` 和 libcurl 账号传输链接自有测试 TU，在 127.0.0.1 合成服务中通过公开 `adoptApprovedSession` 注入纯合成会话，`useSecureStore=false`。没有读用户凭据、安全存储或好友内容，也没有向生产服务发送这些合成令牌。

| 合成好友响应 | 实际 4.0.2 客户端结果 |
| --- | --- |
| 503 HTML | 与截图一致：网络连接失败、登录服务返回了无效数据 |
| 429 JSON | 明确请求太频繁，不出现网络失败或无效 JSON |
| 200 JSON `{friends:[]}` | 成功，空好友列表 |

三种分支同一次探测通过，证据为 `.cache/friends-auth-invalid-json-20261128/FriendsErrorFixture-safe.json`。只读现场证据为 Mac 供件 JSON，未保留响应正文、IP、令牌或用户资料。

早期裸诊断 exe 因未使用完整运行库发生入口点加载错误；复制到自有正式运行库副本后正常执行。它不是产品加载失败，不计作业务 RED。

## 修复准备

使用服务器已有 Nginx 做自有 loopback fixture。完整展开线上 includes 后，已确认实际 zone 为 `60r/m`；通用路径无继承的 status/error_page/proxy_intercept_errors 覆盖。云盘独立 429 和默认 server 的错误页不属于该通用路径继承。

| 隔离场景 | 结果 |
| --- | --- |
| 原 burst15、20 个合成并发请求 | 16×200 JSON、4×503 HTML（197B） |
| 新 burst60、20 个合成并发请求 | 20×200 JSON |
| 新 burst60、独立重置后 80 个请求 | 61×200、19×429 JSON |

429 的 `RATE_LIMITED`、`Retry-After: 1`、`Cache-Control: no-store` 精等；合成 Authorization 转发正确，上游业务自己的 429 未被接管。完整候选 `nginx -t` 通过。

最窄变更只涉及 `/etc/nginx/conf.d/ichat.conf` 的 burst15→60、显式429及 internal 命名JSON处理。稳态rate/key/zone、鉴权、转发、body限制、WS、云盘和其他location保持。仓库部署示例与 README 同步维护。

两个夹具问题另行保留：展开 include 后文本替换的前置唯一性检查失败，改为先在文件映射覆盖候选再展开；合成 Python 后端默认 backlog5 容量不足，改为 owned backlog256 后保持同20/80请求及断言全部通过。未用这些夹具失败冒充产品RED，也未向生产发送突发测试。

## 实际部署与回归

2026-11-28 北京时间 02:57:08–02:57:10，Mac 唯一运维窗口完成窄修复。原文件4316B，SHA256 `2a51efd39f0b68aa593c758bbb634543429e3c5fa7da9cb91e5075c00da86518`；post4670B，SHA256 `31aab4e4aa11f6cec8f068a1752ff27bde8fceff9ead53716fd0ec6af042ff46`，owner/group/mode保持。

真实完整 `nginx -t` 通过，仅一次 `systemctl reload nginx`，master27457保持，新workers200999/201000接管；旧worker正常排空，未强杀，不宣称已全部退出或共享计数重置。Auth199507、Sync199508、Model199544保持原PID，四服务active。

受限原config、metadata与回退脚本仅保存在服务器 `/var/backups/maichat/nginx-friends-burst-20261127T185709Z`，未下载完整生产配置/凭据。未读写DB、用户Token或env值，相关源码与env stat metadata保持；未停止用户App、访问真实好友列表、调用模型或改变发布tag/附件。

部署前后低频回归：Auth本机health200 JSON、公开首页200，friends/me/model-api-keys均401、35B JSON object。Root另用正式4.0.2 Windows原生传输独立重跑同三无凭据接口，均通过。线上未做20/80突发或授权好友请求；历史用户HTTP状态由配置与隔离复现推断，不冒充已读取到的历史状态码。

安全部署回执3967B，SHA256 `54df31148d9988e6f1d6344cbbe2b87f6fc030fdca217096cedf1bf8465f94a3`，Root核验post摘要、服务PID/健康与一次reload；写窗口关闭。现有4.0.2无需重装，关闭当前弹窗后重新打开应用可重新加载。尚未代用户做真实好友列表UI验收。
