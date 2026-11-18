# 云模型 Responses 凭据部署记录

- 日期与平台：2026-11-19（Asia/Shanghai）；Mac 操作员执行服务器部署，本地 Windows Node 假凭据回归。
- 范围：现有凭据控制面增加 `api_keys.openrouter`，沿用所有有效登录账号的主模型授权；客户端直连供应商，服务不代理模型请求。
- 源码：当时 `main` HEAD `2a2c7c537bfa059f042972c11450a5b48a3e3b86` 叠加本次提交前工作区改动；最终提交尚未产生，不把后续提交 SHA 反写成本次部署来源。部署文件以以下 SHA-256 核对。

## 已部署文件与运行产物

| 服务器文件 | 部署后 SHA-256，与交付文件一致 |
| --- | --- |
| `/opt/maichat/model-credentials/index.js` | `008caa9ffdc2aa2faf1efec43eb262d2c0e126d57fd4947dcfce98aea854ab7b` |
| `/opt/maichat/releases/cloud-drive-20gb-20261117-52ac50e7234d/auth/account-model-keys.js` | `4595ee6b6ad2f2ca91044391aa9df2f85b32ac465bbb767dc1e2889c2e1a4ed9` |

Mac 部署步骤耗时 5.592 秒，完成回执 `passed:true`。只更新上述两份 JS，以及受限环境文件中获准的 GLM 新 Key 与 OpenRouter Key；DeepSeek、MiniMax 值和环境文件其余字节保持不变，文件属主与 `0600` 权限保持不变。Key 未进入命令参数、日志或本地交付文件。操作员未打开数据库，本次无数据 schema 迁移。

| 服务 | 部署后 PID | Node 与启动入口 |
| --- | --- | --- |
| Auth | `177271` | `/usr/bin/node-22`；`/opt/maichat/releases/cloud-drive-20gb-20261117-52ac50e7234d/auth/server.js` |
| Model credentials | `177269` | `/usr/bin/node`；`/opt/maichat/model-credentials/server.js` |
| Session Sync | `177273` | `/usr/bin/node-22`；`/opt/maichat/releases/prepare-image-generation-20261115-57b92599/auth/session-sync-server.js` |

Sync 的 PID 变化来自已批准的正常依赖重启；其源码、cwd、argv、systemd unit 与 drop-ins 未变，未修改其他无关服务。健康检查：Auth 200、Sync 200；无凭据访问 Auth 与模型凭据入口均为 401。旧发行器仍返回原三字段且值匹配，新发行器原三字段和 OpenRouter 均匹配。服务器受限备份位于 `/var/backups/maichat/openrouter-main-model-20261119-b73b33abd303`；本次未执行回退。

## 本地验证

服务合同先写测试：同一组 72 项首次为 61 通过、11 个预期失败，最小实现后 72/72 通过。新增覆盖旧响应缺少 OpenRouter 时归一化空串、四供应商非空判断、非法值拒绝、64 KiB 原始响应上限，以及四个最大长度 Key 的 UTF-8/JSON 转义开销。

- `services/auth/` 下 `node --test`：240/240，通过回执 `.cache/openrouter-fable52-20261118/auth-final-tests.log`。
- 仓库根目录 `node --test services/model-credentials/index.test.js services/model-credentials/server.test.js`：7/7，通过回执 `.cache/openrouter-fable52-20261118/credential-final-tests.log`。
- 最终服务回归合计 247 项，全部通过；包含本机 Auth→凭据服务 HTTP、登录门槛和 `no-store`，未调用供应商或使用真实 Key。

## 供应商探测与验证边界

- OpenRouter：两次合成 Responses 请求均 `completed`，完成 echo 工具往返；实际没有返回 reasoning item，因此没有实际签名/加密 reasoning 续接的探测证据。
- 旧 GLM 凭据：全部请求均使用同一 `/api/v1/responses` 路由。首次 `reasoning.none` 返回 `invalid_request`，第五次请求的嵌套 message 证实 `glm-5.3` 不支持关闭思考；改为 `low` 后，两型号在同一路由返回 `permission_denied`／套餐到期。Chat 可用和现金余额不等于该 Responses 路由的权限；未核对 Key 控制台类型，不推断 Key 无效。
- 新 GLM 凭据：`glm-5.3`、`glm-5.3-flash` 文本对照均 HTTP 200、`completed`，输出符合对照要求。Flash 图像探测的六位 nonce 和四种颜色全部匹配。
- `glm-5.3` 图像请求虽 HTTP 200、`completed` 且补验找到结构化对象，但 nonce 不匹配、颜色仅 1/4 正确；图像识别能力尚未由本次探测证明。

供应商探测使用合成输入，不保存原始响应、reasoning 或 Key。以上是服务部署和有限探测证据，不等于各端 App 实际运行构建的完整验收；客户端与核心回归另见主任务记录。

## 安全回执索引

指定回执位于本机 `C:/Users/18034/multi-ai-code/accounts/house-maichat/remote-im/files/p_mvzbw62t_a46370/`，以下文件均不含 Key：

- `144115251141768230-1795020858-98368685.json`：实际部署、文件哈希、运行进程及保留项。
- `144115251141768230-1795020858-22833145.json`：部署脚本独立审查，无剩余具体阻断；范围为两份 JS 与 GLM/OpenRouter 环境项，无数据迁移。
- `144115251141768230-1795019948-86117104.json`：新 GLM 文本与图像对照，记录 UTC `2026-11-18T16:29:21.678976+00:00`。
- `144115251141768230-1795019949-330079.json`：GLM-5.3 图像独立补验，记录 UTC `2026-11-18T16:34:57.138299+00:00`。
