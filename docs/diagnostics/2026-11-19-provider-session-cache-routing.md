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
