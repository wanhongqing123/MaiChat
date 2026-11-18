# Fable 五档、通用检索与 402 安全诊断：共享及 Windows 验证

基线 `fcf0c3d8d5e014126dddcffaf364c0939f6e44ef`，2026-11-19 在 Windows
正常源码和构建目录验证本次增量。用户要求 Fable 的完整五档，同时指出该会话缺少
通用检索、OpenRouter 出现 402。后台凭据、默认模型和公开 release 均不因本次改动调整。

## 操作及实现

- 创作中心选择 Fable，再从原菜单选择低、中、高、超高、最高；逐一对应
  `low/medium/high/xhigh/max`。其他型号保留低、高、最高。
- 保存、重启或读取远端配置后保持同一会话的选择。仅切换型号时，Fable 的中/超高
  在不支持的目标型号下原子归一为高；显式请求非法组合仍失败，不能同时改掉标题或型号。
- 共享目录新增普通按型号查询、验证及已知值归一函数；旧单参三档验证合同保持不变。
  空值保留继承，任意未知字符串不归一为高。没有新增 struct 字段或数据库迁移。
- CAPI choice、snapshot/remote_snapshot 顶层及 sessions 项增加同名
  `supportedReasoningEfforts` 数组；继承默认和已知历史值按实际型号投影，查询不写库。
- 产品两个注册点直接使用现有 GLM Search/Reader。其描述、输出明确署名 GLM，
  search 仍限 70 Unicode 字符，独立默认路径保持 `/api/paas/v4`；
  不借用主模型的 `/api/v1` 地址或 DeepSeek 凭据，不新增浏览器旁路或 MCP。

## 同例失败与回归

证据在 `.cache/openrouter-fable52-20261118/`：

| 边界 | 修改前实际失败 | 修改后 |
| --- | --- | --- |
| 五档 wire 与其他型号严格拒绝 | 58 个断言 | 原值发送、非法值零 HTTP；通过 |
| Core 会话/Runner | 76/60 个断言 | 定点均零失败，SQLite 重开、busy、原子更新覆盖 |
| Windows Controller | 10 项业务失败 | 路由/持久化/五档/检索合计 Qt 35/0 |
| Panel / 远端配置 | 各 1 项失败 | Qt 4/0、3/0 |
| 实际 MainWindow 点击 | 中/超高各失败一次 | 旧宿主三档门禁移除，同例 Qt 4/0 |
| 产品检索注册 | Fable/DeepSeek 共 3 项失败 | 五云型号的 GLM schema/描述、GLM Key 隔离均通过 |
| 未来 402 完整响应分类 | 22 个断言 | 已知分类、未知/其他供应商、等待秒数边界均通过 |
| HTTP 402 头后半正文/RST | 接收重置 oracle 成立，但请求次数大于 1 | 已知 402 不再沿传输错误自动重试，同例通过 |

Core、InputQueue、SessionRename、FilePath、Store、ModelClient、Runner、Tool、
ContextBuilder、Compactor 十套回归全部通过（47.03 秒）；最后 RST guard 后完整
ModelClient 再次通过。普通原生网页测试、TLS/重定向凭据边界及控制台端到端全部通过。
CLI 端到端包含工具、权限、会话恢复和被终止进程已落库内容的恢复。

Windows 正常 `maichat` 构建后部署门禁检查三产物、114 个 PE 导入及 Qt 插件通过。
MainWindow 用例已实际导航至创作中心，再打开五档菜单点击并读回真实 Controller/存储。
亲看 `fable-effort-visible-*-window.png` 和 `*-menu.png`：五项完整可见、未截断。
这是测试隔离窗口，字体启动环境不等同于用户现装 App；未修改用户的字体设置或现装进程。

Android Java 六套最初 30 项中 10 项失败，修复后 41/0；后补云/本地 ACK wire
负控，旧代码再现 2/2 失败，修复后六套共 43/0；同一 Qt 两反例从失败到 4/0。
回执必须按目标型号匹配 wire，五云仅 Responses，
本地仅 local，不把普通库的 Chat 能力误当产品远端能力。
Android 最终双 ARM ABI 已实际正常编译（37 秒）；未宣称 Android 真机 UI 或安装验收。

Apple CAPI 同一最终测试旧源 23/43 失败，新源 107/0，供应商 HTTP 为 0。
正常 iOS 模拟器三业务 3/0，包括迟到历史页不能覆盖新选择、跨 owner 的空默认值不能
借用前一 owner。完整 Apple 与真实普通检索证据见
`2026-11-19-ios-model-efforts-web-retrieval.md`；230 真机安装需独立回执，当前不预称完成。

## 402 边界

保留原供应商文本，只附加白名单分类和 `0..86400` 的数字 Retry-After 秒数；
任意原始 metadata、头值和 HTTP-date 不附加。已知短暂 in-flight 竞争映射
RateLimited，单请求估价过大单列，缺失或不匹配的 metadata 不推断。
既不修改输出上限或目录容量，也不增加自动重试、充值或换 Key。

当前普通请求省略输出上限，目录 128000 仅用于本地预算，不能说 App 显式申请了该值。
旧现场没有保存实际分类，免费 Key 状态查询也不能重建当时并发预算。
本次只交付未来可诊断的错误和重试门禁，不宣称供应商额度限制已消失。
详见 `2026-11-19-ios-openrouter-402-observation.md`。

Mac 普通消费者在同 ABI 档案上更新最终 v2 八对象后，v3 再独立编译 RemotePanel
0/4.867 秒，36 个 agent 成员逐等（1 新、35 复用），完整 App link 0/0.463 秒。
这不是本轮全部对象重编，且没有运行、签包或替换 Mac 现装。
