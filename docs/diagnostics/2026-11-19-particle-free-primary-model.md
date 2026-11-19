# 免费主模型 Particle-1 接入

## 操作与期望

基线 `1ea61ad16828dec2394ef34618673b066ed9112e`，产品缓存路由代码等于 `2dd34dc0f218eccb26154792ae1c558a2abcac54`。用户要求比较能力较强的免费模型，并在现有主模型菜单接入一个，以降低 Fable 使用费用。

创作中心原主模型菜单应新增“Particle-1（免费）”；手动选择后沿现有账号的 OpenRouter 凭据和 Responses 路径运行。已有会话、默认型号和 Fable 五档设置保持原选择；切型号时沿现有规则把不兼容的继承档位归为合法高档，显式非法组合仍拒绝。新型号应接受文本、图片和工具调用，不能只在菜单增加名称。

免费指模型端点的 prompt/completion 单价为零，仍有调用配额；联网检索及其他外部工具按原服务规则计费。不可用时明确失败，不自动切换付费型号。

## 选型依据

优先选 `particle-ai/particle-1:free`：公开目录当前 prompt/completion 均为 0，无其他非零价格项或到期日；图文输入、工具调用、1,048,576 上下文和 131,072 输出上限符合现有 Agent 需求。[OpenRouter 型号页](https://openrouter.ai/particle-ai/particle-1:free)、[Particle 模型文档](https://docs.particle.ai/models)

Particle 一手文档明确实际思考三档为 low/high/max，其他部分值映射为这三档。产品保留真实三档，不把接受别名误呈现为更强的独立档位。[思考档位](https://docs.particle.ai/reasoning)

该模型面向 Agent 编程；厂商报告四项任务用更少思考和工具调用 token，成功率接近其基础模型。这里只作为任务方向与接入适配依据，不宣称已独立证明它在所有免费模型中最强。[发布评测](https://particle.ai/blog/introducing-particle-1)

其他候选取舍：Inkling 免费目录明确于 2026-11-19 到期；ERNIE 当前只接受 high 且文本输入；Gemma/Nemotron 部分候选未明确 effort 枚举。没有将未明确字段猜成与现三档兼容，也不承诺免费端点无限、永久或具备标准付费服务的隐私保证。

## 共同合同

- Provider 为 `openrouter`，精确型号保留 `:free` 后缀；同名收费端点不是回退路径。
- 沿现有 `openrouter_api_key` 当前账号回调和后台受限环境字段，不新增密钥入口、授权范围、库、MCP、代理或服务部署。
- 免费请求附带 `provider.max_price` 的 prompt/completion/request/image 全零约束、`allow_fallbacks:false` 和 `plugins:[]`；失败不改型号重发。[OpenRouter 价格过滤](https://openrouter.ai/docs/guides/routing/provider-selection#max-price)
- OpenRouter 两型号都保留合法会话路由标识；Claude/Fable 专用的 `cache_control:ephemeral` 不发送给 Particle。
- 共享目录、CAPI/Qt/Android 路由与宿主白名单、图像门禁同步接入。旧 ProviderWeb 普通库仍严格 GLM/DeepSeek 白名单，产品通用 GLM 搜索/Reader 不增加另一套执行核心。
- 前缀、固定指令、工具说明排序、usage/数据库方案仍不在本次改造范围。

## 验证分层

旧正常 iOS 菜单缺免费入口已实际 RED，13.006 秒；宿主入口及合成 owner 确认用例先阶段 GREEN2/0，14.921 秒。这不表示旧 Core 已支持免费型号。

先以自有本地服务验证型号/凭据/字段/无付费回退，再由唯一消费者最多两次精确免费端点调用检验自造 PNG 与内存 echo 工具回合。目录价格和服务端零价约束均应核对；不执行任何外部工具，不保存密钥、响应正文、思考或签名，费用缺失保持未知。

## 已完成的受限免费验证

公开完整目录读取出现截断和网络失败时，执行尚未到凭据或模型阶段。随后使用本任务已从官方公开 API 下载、未超过 24 小时且价格全零的精确型号记录作前置资料，实际请求仍由服务端四项零价约束保护。

真实模型 operation 合计严格两次，均精确 `:free`，low、输出上限 1024、`store=false`、插件空、不重试或转型号，不使用用户内容：

- 第一次 HTTP 429，顶层提示供应商错误；没有任何输出、费用字段缺失，未记为已知零账单，也未派发工具结果跟进。
- 充分等待后仅用剩余一次额度，HTTP 200 / `response.completed`；返回 reasoning 和 function_call，自造六位数字与四种颜色的工具参数完全匹配，供应商该次 `usage.cost` 数值为 0。

预算到此停止，没有执行任何外部工具。真实验证达到图文识别与单回合函数调用，不宣称已实测完整工具结果回灌回合、所有任务质量或无限免费；429 可用性边界保留。密钥只在内存，响应正文、思考和签名未保存。

共享旧正常 Core 同一最终 test 实际 RED26，新增实现后 GREEN0；包含精确目录/官方容量、空及非空会话 ID 的零价与插件守卫、旧供应商请求、429/402 不回退或读取付费路线。完整普通 ModelClient 和其余十一套 Core 回归共十二套均通过，总运行 49.145 秒；可选 TLS 宏和 Windows live 探针未开启，不冒称已验证该矩阵。

## 宿主与界面回归

Mac 按原同 ABI 基线仅重编 Core3/agent3 六个 TU，编译均为 0；新 Core90/agent36 归档成员逐字节核等，302 个头文件与基线保持相同，完整普通 App 链接 0/0.916 秒。没有运行或签装桌面 App，不以此代替 Windows 验收。

Android 同一私有普通 API 检查旧源 RED7/17、新源 GREEN0/23，另有既有 JVM 11/0。正常 Gradle `compileDebugJavaWithJavac` 目标在现有 Java11 下通过，0/9.966 秒，实际产品 Controller/ActionMenu 等 class 已生成；Java17/21 的旧 AGP jlink 环境失败另留证据，没有更改构建配置或 SDK 绕过。双 ABI native 尚未计为通过。

iOS 最终普通构建 0/11.772 秒、严格签名 0；四个变动 native 对象到 Core/Mobile 和 App 合并归档逐字节核等。普通菜单和合成远端 owner 用例通过，免费文字三档请求、切型号归一与重启保留通过。

图片验证须分开记录：普通导入/附件接纳通过后，显式受限 DEBUG 按钮调用正式 `sendMessage`/outbox 队列，取得第四次实际本地 `input_image` 请求、回复及持久化；不是直接 HTTP 或伪响应。普通键盘图片发送在自有夹具 R7 中出现 `Send Disabled`，未证明原因也未修复共享 Composer，不能把辅助用例当作该普通键盘路径验收。测试初期的滚动/配置确认等待及诊断进程中止另留记录，没有修改生产输入框或降低业务断言。

上述模拟器使用自有目录、合成 Key 和本地服务，不读取用户会话或触发收费模型。候选后继版本为 3.0.1(232)，签名真机产物、安装及进程以后续实际回执为准；此前 231 包仅 ready、设备连接 4016 的边界保留，不冒称已安装。
