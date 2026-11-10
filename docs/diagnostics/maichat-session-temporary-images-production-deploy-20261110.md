# Session 临时图片后台部署记录

## 目标与冻结输入

2026-11-10，用户明确要求推进后台实际部署。预期是新增 Desktop 所属端图片首次上传一次，
同账号远端凭发布引用读取；所属端离线后仍可读取；24 小时业务到期不重传。
本地完整缓存可继续显示，未缓存图片明确过期。客户端直接传 OSS，后台只处理控制元数据。

服务源码冻结为 `8cde6a87418c84691598b567f49a68255880bd10`。本次没有版本、Tag、
GitHub Release、旧候选包重打或现场客户端安装。上下文压缩工作是独立后续输入。
部署仅通过 Mac 既有受信任 SSH 凭据执行 Root 冻结脚本；没有复制私钥或增改 SSH 授权。

目标经公共 DNS、SSH 主机指纹、实际 Nginx/三个服务目录交叉核对。旧 SSH 别名指向其它项目，
已排除，没有向其写入。目标运行 Node `v22.23.0`，`/usr/bin/node` 实际为 `/usr/bin/node-22`。
Auth、Sync、Media 原目录不同，不按单一旧目录覆盖；原实际 PID 分别为 122880、122881、111444。

## 已执行与原始失败

1. 公网 TLS 开启，无凭据 Auth、Sync、附件列表/播放接口均拒绝访问。
2. 初始 stage 脚本误检查 Media `/health`，得到 404；Auth/Sync 同路径均 200。
   失败发生在 backup/release 创建之前，无服务、DB、RAM、OSS 变更。
3. 修正版改查 Media `POST /cloud-drive/read` 的精确 403/Internal authorization required，
   不把任意 404 当服务通过。随后 Auth/Sync 两份 SQLite 在线备份通过完整性检查，
   备份和原配置仅留服务器 root 私有目录。
4. 首次 Linux Auth 全套 195 项，190 PASS/5 FAIL。四项是部署包 `media/` 与测试引用
   `oss-media-signer/` 的目录映射；一项是 WebSocket 测试结束后关闭 DB 的异步清理竞态。
   原日志保全，不称全绿。服务仍保持原 PID。
5. 新 release 内建立 `oss-media-signer -> media` 别名，生产源码和测试断言不改。
   media 安装受锁文件内网 npm 地址超时阻断，尚未进入下一轮测试。
6. 在本机已有 npm 内容缓存中找到全部 89 个原锁定依赖包，逐项 SHA512 与原 SRI 一致。
   独立空缓存/精确 package 与 lock 的实际离线安装成功，89 包、exit 0、913ms。
   供件恢复仅提供内容寻址缓存，不改 registry、代理、版本或锁文件。

## 精确部署供件

| 供件 | SHA256 |
| --- | --- |
| 110 源文件归档 `session-temp-images-8cde6a87.tgz`（269549 B） | `7f9609445768dc7e35493aa21392961b2b369efafad412dbea218e79b1e7cfd3` |
| stage `phase1-v2.py` | `b065ea071b70f35906428ad6f1314129137772309993bb27d869d97e8d08951b` |
| 首次续跑 `phase1-resume-v3.py` | `8284611bdf3392db74681314edf16429b369f9cfedfdca1724cdf81d3d492ec5` |
| 离线缓存（2712284 B） | `3893d1cba4eb5b4bad59b092f159cd1c29bde807e5207809a9d3594d99af294f` |
| 离线续跑 `phase1-resume-v4.py` | `6d8d5bad74de473404f6de3c4a7931e7210a87dd65870dfc309a403e70158332` |
| RAM/生命周期 `storage-phase.py` | `39ea234a61e20a12639ee486801b378e85b327bb5253e37e3ef0ca98be40aa0a` |
| 实际 CLI 路径适配 `storage-phase-v2.py` | `547ed8b5fc58c691707fdc4019d573c57122411adb610086a84d87b5e8ef2f97` |
| 完整仓库布局验证 `validation-v5.py` | `be7403ce9f036801607d334ed7c45139e066a55adba29ef8c7f4423bcddd9135` |
| 独立测试夹具包 `validation-fixtures-v5.tgz`（5638 B） | `d7d6124b5bef3e71191abae92c09ff351bdef95cdca467cc1ec1ef66242d4108` |
| 三服务切换/回滚 `service-phase.py` | `1437d52295a5e1b16ab223fcfbf54cf734588440ad8a71d139dda2707cf83d04` |
| 真实链路 canary `canary.js` | `52454701ed941172a0e64563661b97b345d2a1b543134e9e7cfeae4908fa9859` |

原服务配置、SQLite 备份、完整 npm 日志只保留 ECS root 私有目录，不复制用户数据库或凭据至测试产物。
脚本的本机副本与无敏感测试证据在 `.cache/session-temp-images-deploy-20261110/`。

## 上线与验收状态

**实际部署完成，最后只读核验时间为 2026-11-10 01:55:17 UTC。**

离线恢复成功后，原 release 上的测试仍得到 WebSocket 5/1、Auth 204/1、Media 72/1。
WebSocket 缺的是服务端断连完成屏障，HTTP close 不等待 upgraded WebSocket 回调；Media
缺的是原仓库 PNG 夹具路径。为避免改生产冻结输入，最后验证复制全部 110 个原文件到 BACKUP
下独立的完整仓库布局，仅替换测试文件 teardown 和提供原 PNG，不改生产代码和原断言。
测试文件新 SHA256 为 `eeb71b43f24c389f165364d7dcc161bdd6b4999f91bddd5ae4827c24d1b0b78b`。
完整 Linux 验证得到 **WebSocket 5/0/0、Auth 204/0/0、Media 73/0/0**。
三个服务用户在实际 release 上 import 成功；部署前后生产 110/110 文件仍精确等于 `8cde6a87`。

存储 inspect 首次因旧 CLI 路径是 symlink 拒绝，在核定实际 `/usr/bin/aliyun` 的 root 权限、
SHA256、版本 `3.0.282` 后使用新版冻结脚本；没有改变系统路径。01:46:00—01:46:17 UTC
实际更新 RAM：reviewed legacy 完成 persistent、temporary 两级增量，完整读回最终策略 SHA256
`84fc9bcd960405248239cc49bffcbd6f0529d44a2f763f1f32e616c33327b068`。
生命周期原 1 条规则保留，新增专用规则后共 2 条；再次独立读取确认 already-configured，
Bucket 从未启用版本控制。完整备份在服务器私有 `storage-czi8e3l7` 目录。没有创建或轮换密钥。

01:51:26—01:51:30 UTC 实际切换成功：

| 服务 | 实际 PID | 新 release 子目录 |
| --- | --- | --- |
| Media | 151848 | `media` |
| Auth | 151857 | `auth` |
| Sync | 151867 | `auth` |

三个进程的 cwd/script 均在 `session-temp-images-20261110-8cde6a87`；User、EnvironmentFiles
和剔除 invocation 元数据后的完整配置 SHA 与原进程逐项一致。Node22 保留。无回滚或 DB 恢复。
服务就绪检查通过，默认 TLS 校验下公网三个未登录 GET 均为 401。

01:53:07—01:54:13 UTC 真实 canary exit 0：

- 自建 phone-less 账号、合成 PNG，pending → 首次初始化 → 直接 OSS PUT 200 → 完成 200 → ready。
- 固定期限、重复 init/ready 身份、精确消息 part 顺序、活动房间拒绝清理均通过。
- 实际等待 46504ms，catalog 确认所属端离线后，同账号 viewer GET 200；跨账号访问拒绝。
- 上传与下载 SHA256 均为 `ee77cbc4d58e7c1ddce76e1757b4dc60f9a155014f62a66e2a1b1113f683d83e`。
- 房间、附件元数据已清理，OSS HEAD 确认对象不存在，exact-key Multipart 列表为空；
  测试登录已退出，两个自建账号已停用。没有删除真实账号或访问真实用户历史。
- recovery journal 始终只在服务器 root 私有目录，没有下载或输出。

01:55:17 UTC 最后只读核验 PID、cwd、配置及生产源 110/110 未变，Mac 外部 TLS 三 GET 仍 401。
小证据包 27046 B/SHA256 `0edcd5b96f0c7c7f4631b9c6c5be63f71cfb46dc54679ed5c8f2abebc21cffa7`
含 30 份安全原件；Root 复算其中 manifest 列出的 29/29 文件 SHA、归档 CRC 无误。
不包含 env、DB 备份、recovery journal 或凭据。

Root Windows 的额外独立公网探针在本机既有代理和直连路径均遇到 TLS handshake/reset（curl 35），
没有收到 HTTP 状态，原失败保留，不将其冒充 401 或证书缺失证据。Mac 外部默认 TLS 与服务实际
HTTPS canary 均已通过；没有因此禁用证书校验或更改服务器 TLS。

存储脚本离线 Python 11/11、Node adapter 7/7 通过；切换脚本 4/4 通过；canary 18/18 通过。
Root 独立复跑 Python 11/11、切换 4/4 和 canary 18/18。
存储 apply 先备份、完整读回，保留其它前缀和生命周期规则；拒绝版本控制和未知策略漂移。
服务只新增独立 drop-in，保留环境、用户、Node runtime。失败时三个服务一起回滚，不用旧 DB 覆盖新写入。
真实 canary 只操作自建账号和 PNG，已验证上传/读取 SHA、同账号权限、所属端离线读取、跨账号拒绝、
精确对象与 Multipart 清理。24 小时实际到期及 OSS 异步生命周期物理删除另计，不能由快速验收冒称观察到。
现场客户端仍未更新；旧历史不补传，iOS 作为图片所属端首次发布仍未接入。iOS 新接收界面已有
Simulator UI/unsigned Release 构建证据，本次不冒充现场手机已安装或实机端到端通过。
