# MaiChat 模型凭据服务

本进程只保管并签发模型凭据，不处理账号登录、OSS 文件或素材登记。
它仅监听本机 `127.0.0.1:9200`，不配置公网 Nginx 路由。账号服务验证
有效会话后，携带只在两个服务进程中保存的令牌调用：

- `POST /account-model-keys`，`{"action":"account_models"}`：返回 GLM、DeepSeek、MiniMax、OpenRouter Key。
- `POST /temporary-credentials`，`{"action":"ark_seedance"}`：返回五分钟的 Seedance/Seedream Key。
- `POST /temporary-credentials`，`{"action":"bailian_video"}`：返回五分钟的百炼 Key 与 Workspace ID。

App 只调用账号服务的公开接口；取得凭据后直接请求模型供应商。
本进程不转发模型请求、流式响应或素材字节。环境变量见
`deploy/model-credentials.env.example`；真实密钥只保存在服务器受限环境文件中。

主模型响应为 `{"api_keys":{"glm":"…","deepseek":"…","minimax":"…","openrouter":"…"}}`。
四个字段始终为字符串，未配置的供应商返回空字符串；四者均为空时返回 503。
OpenRouter 沿用所有有效登录账号可领取主模型凭据的既有授权范围，不增加独立白名单。
账号服务兼容旧进程缺少 `openrouter` 的响应，将其视为空字符串；不能用 `null` 或对象
代替字符串。供应商地址、型号和 Responses 协议由客户端配置，本服务只下发凭据。

本地假凭据回归从仓库根目录运行，不读取部署环境文件、不调用模型供应商：

```sh
node --test services/model-credentials/index.test.js services/model-credentials/server.test.js services/auth/account-model-keys.test.js services/auth/service.test.js services/auth/server.test.js
```

上线顺序与回退：先更新账号服务的兼容解析，再更新本凭据服务，保留既有服务令牌及
GLM、DeepSeek、MiniMax 字段及未获授权替换的原值。本轮用户另提供并验证了新 GLM Key，
因此操作员可在服务器原有受限 `/etc/maichat/model-credentials.env` 中原位替换
`MAICHAT_GLM_API_KEY`，并配置 `MAICHAT_OPENROUTER_API_KEY`；DeepSeek、MiniMax 及其他字段
保持原字节。持有 Key 的操作员保持文件
权限 `0600` 并重启本服务；不把 Key 传到源码、命令参数、日志或部署产物。
核验账号服务 `/health`、无会话 401、有效会话下四字段的类型及是否为空即可，不输出凭据响应正文，
也不需要发送付费模型请求。无需数据库迁移、Nginx 新路由或模型代理。
如需回退，操作员原位恢复受限配置及前一服务版本，恢复明确备份的供应商值；缺少新字段
由兼容账号服务转换为空字符串，新客户端应将 OpenRouter 标记为未配置。
