# 既有会话身份与供应商缓存路由

## 操作与期望

基线为 `77b7f0a451f3da8381c630bbb8fe9c580c9db687`。用户要求先补现有会话 ID 透传和厂商明确支持的缓存参数；固定资料前缀、指令内容、工具排序与历史整理留待后续讨论，本轮不改。

用自有会话 A 连续请求、工具跟进及摘要时，供应商路由标识应保持 A；会话 B 与子会话必须使用各自既有 ID。普通无会话调用保持空值，不生成新身份，也不把路径、用户号、凭据或正文当作替代 ID。

## 协议范围

- OpenRouter 的已注册型号：非空会话 ID 映射到顶层 `session_id`，并添加顶层 `cache_control: {"type":"ephemeral"}`，不指定长期 TTL。空 ID 的旧普通调用不增加这些字段。[官方 Responses](https://openrouter.ai/docs/api/api-reference/responses/create-a-response)
- GLM 的已注册型号：非空 ID 只映射到 `prompt_cache_key`，不照搬 OpenRouter 字段。[官方创建接口](https://docs.bigmodel.cn/api-reference/response/%E5%88%9B%E5%BB%BA-response)
- DeepSeek 的上下文缓存自动管理，Responses 明确不支持 `prompt_cache_key` / `prompt_cache_retention`。其 `user` 涉及终端用户及隔离语义，不把会话路由 ID 填进该字段。[官方 Responses](https://api-docs.deepseek.com/guides/responses_api/)
- 未注册供应商和本地模型不发送上述供应商缓存字段。继续保留 `store=false`、完整有效历史、原型号、原思考档位和原输出预算。

缓存 ID 不是鉴权或安全隔离边界，也不保证命中；实际节省须以后依据供应商真实用量验证。本轮不新增付费探针。

## 实现约束

`MaiModelRequest` 末尾追加默认空的 `sessionId`，合法非空值为最多 256 字节的可见 ASCII，不含空格与控制字符。它只作为请求元数据，不写进提示正文、工具声明或 token 估算。

Runner 从自身 `mSessionId` 赋值。Compactor 的 `prepare` 使用其已有会话参数填补空值，非空不一致时拒绝，且不派发请求；逐字段新建摘要请求时复制原 ID，后续值复制与重试自然保持。子 agent 不借用父会话或共同工作目录的身份。

追加字符串会改变 C++ DTO 布局；全部实际构造、复制、销毁或传递该请求的 Core、CAPI 和宿主消费者须同源重编，禁止混用旧 archive。无新依赖、MCP、服务部署、凭据或数据库迁移，不扩展计费持久化和 UI。

## 验证计划

先在仅增加字段、保留旧生产实现的同一新 ABI 上，运行 wire、Runner、Compactor 的正常本地消费者取得行为 RED；再逐项回归。覆盖不同厂商字段隔离、空值/非法值、原请求内容不变、同/不同/子会话、摘要分块与格式重试，以及估算不受元数据影响。共享公共用例只跑一次，Apple 执行本端消费者编译与普通请求接通；未发真实供应商请求，不宣称实测缓存命中率或节省金额。

实际新 ABI 失败对照：标准 own-target clean 后，Core 90 个对象、support 1 个对象和三组 test 对象全新重编并链接成功，vendor/SDK 未变。`--cache-session` 的 ModelClient/Runner/Compactor 分别 RED309/10/14；编译失败未计为业务失败。

同一套最终测试、同一新增字段布局消费实现后，三 selector 均 GREEN0，未通过修改断言获得绿色。完整 Core 十套随后 0/49.29 秒（其中 ModelClient 34.08 秒）。独立限定源审确认元数据不进入提示词或估算，原 input/instructions/tools 顺序和续接载体保留。

Apple 正常模拟器 Core/CAPI/App 构建 0/72.792 秒。实际依赖记录确认 76 个新 Header 消费者对象已重编，9 项关键对象到 Core/Mobile 和 App 合并归档逐字节核等；新请求布局 `sizeof=184`，没有以旧 Request 对象混链。普通 C API fresh test、普通链接及 CLI spawn 均为 0，65 项检查/0 失败，五次自有回环 HTTP 验证 OpenRouter A 两轮与 B 新会话、GLM `prompt_cache_key` 对应实际 C API ID、DeepSeek 不添加上述参数。

Mac 普通消费者共 230 个产品对象全部新编译，旧产品对象复用为 0；fresh MOC 133 项、RCC 2 项重新生成。七个新产品 archive 的 224 成员与 6 个 App 对象逐字节核等，90 个实际使用新 Header 的 TU 没有旧头/PCH 混入。完整普通 App 链接 0/0.638 秒，候选 exe 158561144 字节，SHA256 `050b84e33ca4e8387e3e29a4d1894608b2d0fb33a10f7c771e38a6cb10572875`；未运行或签装 Mac App。

Windows 标准 own-target clean 后新 ABI 产品构建及三 binary/114 PE/plugin 门禁为 0，普通 Controller28/Panel3/Remote4/Window4 用例均通过。Android 两 ARM ABI 的正常 native 新 Header 重编/链接 0/42 秒，未安装设备。

这些只验证本端的请求参数和身份链路，不是供应商真实缓存命中或节省金额证明。候选 231 版本只改 app build number，尚未据此声称手机已更新；旧横屏及其他 UI 场景未重复运行，不计入本轮通过。

## 已推送实现与 Windows 候选

实现提交 `2dd34dc0f218eccb26154792ae1c558a2abcac54` 已正常推送并独立核对远端 main。
同源新 ABI 私有候选 `MaiChat-win64-v3.0.1-20261119-2dd34dc0.zip` 为 242393512 字节，
SHA256 `bb6b193bd519b6421e8bb6d0b9e732cd7b41b6ffdb4ff4f40f3671ef0728d892`；
exe SHA256 为 `88bba07f9cb522857f6b171021b04439730d9aed89e180fe6033fe9f2cc7e425`。
staging 和实际 ZIP 提取目录各运行一次 `--smoke`，退出均为 0（1.793/4.272 秒）；
PATH 只有 Windows/System32，Qt 环境已清除。这是启动与依赖验证，不是供应商命中实测。
用户现装 PID 59396 的 `D:\Program Files\MaiChat\maichat.exe` 未停止或替换，候选未发布上传。

iOS 同源正常签名 Release 231 已就绪；安装前可信连接检查返回 CoreDevice 4016，
未派发安装或启动，最新独立已装版本仍是 230。连接与解锁请求已在 Apple 端等待用户回复，
具体构建和阻塞边界见 `2026-11-19-ios231-cache-package-delivery.md`。

## 2026-11-22：低命中率日志访问与源码排查

用户提供 Activity 的低缓存命中率截图，要求分析 `https://openrouter.ai/logs`。
独立网页工具打开该 URL 实际重定向至登录页；后续按进程识别同机已登录的 Chrome，
激活原窗口后使用现有辅助功能接口读取统计页。未更改浏览器脚本权限，未提取 Cookie
或账户令牌，不需要用户导出 CSV。下面源码风险与历史账单根因分开，不能仅由汇总
命中率认定某条代码路径在生产请求中实际触发。

审查本地 HEAD `0875e487dd5de0f652ae5d25f0cf49d7baf0940d`：

- `MaiTurnRunner::buildRequest` 使用持久会话 ID；`MaiResponsesClient::buildBody`
  对已注册 OpenRouter 型号发送 `session_id`，Fable 5.2 另发送顶层
  `cache_control: {"type":"ephemeral"}`。没有指定 1 小时 TTL。
- 按[OpenRouter 缓存文档](https://openrouter.ai/docs/guides/best-practices/prompt-caching)，
  该参数默认 5 分钟。按[Anthropic 文档](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)，
  TTL 从读写请求开始计时，生成耗时也占用有效期。`session_id` 可稳定路由，不保证命中。
- `MaiContextBuilder.cpp` 中，用户图片转为历史后会改写 `Current/Earlier` 附件说明，
  并不再发送旧图像像素；工具旧图亦被过滤。`omittedImages` 非零时，包含数量的
  `Omitted N older image observations` 提示被插到消息最前面。该计数变化会改变
  请求前缀，有破坏旧缓存条目的风险。上述路径亦存在于已交付 233 的产品提交
  `67f0d37d`，但未据此断言用户这次日志运行了哪一版本或实际触发了该路径。
- 当前移动端基础指令在配置时建立；没有在已审查的主请求构造路径中发现每次采样
  自动追加当前时间的代码，不把时间戳变化猜作已确认原因。

下一步须对照相邻请求的缓存写入/读取、间隔、模型/提供商与会话身份：读写均为零
与反复写入但不读取是不同现象；过期、提供商变化和前缀变化需要分别用事实验证。
未读取 Key、手机数据库或私人正文，未发新模型请求，未修改既有缓存参数、TTL、
历史投影或工具顺序。


### 已登录后台核对结果

原浏览器的当前时间范围内，收费 Fable 记录均发生在稳定会话/缓存参数交付之前。
缓存功能提交 `2dd34dc0f` 的时间为 2026-11-19 09:59:05 +08:00；包含它的
233 安装回执时间为当天 22:08 +08:00（见对应交付记录）。账单里该功能交付后
没有新的 Fable 样本，不能据历史低命中率评价新版本的实际缓存效果。

在最新收费记录与一条连续工具轮次记录中，原生缓存读取均为零、session 缺失，
收费与普通输入/输出单价相符。整体费用主要来自大量输入，而最新记录亦有大量内部
思考 tokens；未保存原始参数，因此不能凭用量反推具体思考档位。后台明确未启用
I/O logging，不能逐字对比当时的 prompt 或工具前缀；未打开该设置。

本记录不保存账户账单逐行数据、原始 JSON、请求 ID、用户标识、密钥或聊天正文。
旧图片投影改变前缀仍是独立的后续风险，不冒称它已被证实为该批历史费用的根因。

## 2026-11-22：Opus 5.5 主模型接入与手机 234

用户另要求接入 `anthropic/claude-opus-5.5`，源码基线为 `0875e487d`。
按 [OpenRouter 官方型号](https://openrouter.ai/anthropic/claude-opus-5.5) 与公开模型目录
核对：Responses、1,000,000 输入窗口、128,000 最大输出，支持图像/工具及
low/medium/high/xhigh/max 五档。图像能力为官方声明，没有触发真实视觉探针。

普通 `MaiModelClient`、共享能力/容量、Desktop 与 Mobile 的正式路由、iOS/Android
设置和本地/远端选择菜单同步增加精确 ID；普通会话 ID 与 Claude 自动缓存提示沿用
现有协议。复用账号的 OpenRouter 凭据，不借 GLM/DeepSeek Key；不改变现有默认、
用户选择、输出预算、免费 Particle 的零价约束或服务 env。仅公共头注释改变，
没有新增结构布局、协议字段、第三方库、模型代理或模型专用的第二执行核心。

### 验证

- 正常 Core target：新用例编译成功后业务 RED27；目录/请求实现接通后同例 GREEN。
  新模型五档真实 loopback JSON、session/cache hints、普通 tools 及非法档位零 HTTP
  均通过。完整九套 Core 9/9，40.03 秒。
- 普通 Mobile C ABI 的 `--opus-55` 与既有 `--model-efforts` 各 107 检查/0 失败，
  配置、create/set/get、snapshot/remote snapshot、重开与跨型号归一均零供应商 HTTP。
  沿实际 simulator producer cache 正常构建已有 target，执行后恢复 tests-off 构建选项。
- Android 生产 Java 正常编译，设置/菜单/ACK/媒体四组 38/0。编译继续使用项目的
  Java 11，Robolectric SDK34 在现有 JDK17 测试进程运行；只使用私有 init script，
  未改 Gradle 产品配置或安装依赖。当前无连接的 Android 设备/模拟器，不冒称实装通过。
- iOS 专用 iPhone 14 Pro/iOS26.5 模拟器普通 App：Fable 回归和 Opus 普通界面/本地
  HTTP 接通两例均通过，正常 xcodebuild exit0/159.680 秒。Opus 选择、五档、medium
  回复与实际请求中的 model/effort/session/cache、供应商隔离均断言通过。
  新行增加后，XCTest 将部分露出的行误判为 hittable；已亲看录屏/截图，测试改为
  在同一普通菜单内滚动到整行可见，慢拖后停留以避免惯性越过目标。保留失败记录，
  未删减五档/请求断言或修改产品 UI 行为来放行。
- Mac 正常 Release App 与两个既有消费者 target 构建通过；凭据/协议矩阵 22 数据行
  （含 Opus 五档）及 setup/cleanup 共24/0，普通菜单点击3/0；菜单截图已查看。
  原14.4 SDK缺生成头，按相同版本、相同公开头字节与运行库版本补全私有 include
  overlay；未替换运行库、改变版本或伪造 load commands，未替换 Mac 现装。

### 234 正常设备交付

App build number 两处从233改为234；营销版本与测试目标版本不变。沿原 workspace、
scheme、Release 和已使用的开发团队正常构建，exit0/185.144秒；深度 strict 验签0。
候选可执行文件95,651,376字节，SHA256
`3dc71ac62789ed90cb688d5df700b5530c7bbb4b5b766a06c5055e5dd3d6cbad`。
初次连接4016如实保留；用户确认连接后独立 metadata 原为233，同 bundle 正常更新
exit0/59.207秒。随后独立 metadata 为3.0.1(234)，无附加参数启动0，独立进程查询
只找到一个匹配新 bundle 的 PID86827。

没有卸载/清空、重新配对或更改 TCC，没有比较用户数据字节或读回设备 exe。
未为本轮发真实付费模型、图像或签名续接探针；目标机普通启动不等于私人会话任务验收。
Desktop 公开 Release/tag 本轮未动，当前公开3.1.1仍固定其原发行源码。
完整本地验证回执位于 `.cache/opus55-20261122/`；账户原始日志不作为测试产物保存。
