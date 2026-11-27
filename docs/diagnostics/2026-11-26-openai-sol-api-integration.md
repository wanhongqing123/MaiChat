# GPT-6.1 Sol API 接入与本地体验包

## 用户要求与操作

将 `gpt-6.1-sol` 接入现有主聊天，沿用后台受限配置保存 Key、账号接口下发、客户端直连供应商的方式。
授权范围与其他已配置供应商一致；不增加个人 Key 页面，不建立模型代理。
用户随后要求先提供最近功能的 Windows 本地体验包，该包已优先交付，未覆盖运行中的程序。

操作预期：创作中心主模型菜单选择 GPT-6.1 Sol，五档强度分别保存，切换会话或远端确认后原配置不丢。
OpenAI-only 凭据应可使用；未配置或非法凭据明确失败，不能拿 GLM/DeepSeek/OpenRouter Key 代替。

源码基线为 `be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加原有 WIP 和本任务未提交修改。
没有提交、推送或创建 Release。

## 模型及协议

依据 [官方型号页](https://developers.openai.com/api/docs/models/gpt-6.1-sol) 与
[推理指南](https://developers.openai.com/api/docs/guides/reasoning)：

- 精确型号 `gpt-6.1-sol`，供应商 `openai`，地址 `https://api.openai.com/v1`，唯一网络协议为 Responses。
- 上下文窗口 1,050,000，最大输出 128,000，支持图像输入。
- 推理档位 low、medium、high、xhigh、max；省略档位继承供应商默认，不接受 none/minimal。
- 无状态 `store:false` 请求保留原生 response output；显式请求兼容的 `reasoning.encrypted_content`，按会话保持缓存身份。
  官方说明当前无状态请求默认提供 encrypted content；显式 include 不代表每次一定返回 reasoning item。
- Codex 产品默认窗口与 API 上限不同。相同公开型号不等于相同系统指令、工具目录、推理设置或服务端会话能力。

既有会话与未来默认模型保持原配置，不因新增型号自动切换。

## 凭据与后台状态

代码新增可选 `MAICHAT_OPENAI_API_KEY` 和 `api_keys.openai`。旧响应缺少字段按未配置处理；显式 null、错误类型、控制字符或过长值拒绝。
保留原四家值、登录门槛、内部服务令牌、回环监听和 no-store；OpenAI-only 集合同样有效。
五个 4096 字符的最大三字节 UTF-8 Key 共 61,518 字节，仍在 64 KiB 原响应限制内。

真实 Key 仅从本会话受控输入读取到内存，通过 stdin 交给验证进程，没有放入命令参数、源码、明文测试文件或日志。
实际 Key 排除检查已覆盖本任务文本产物和跟踪文件差异；后台准备包只含 JS、说明及空环境示例，不含任何真实 Key。

**后台实际部署尚未执行。** 本机已配置 SSH 身份认证未通过，当前没有可用的部署入口；已向用户请求现有可用 SSH 配置名或运维入口。
未改服务器配置、其他供应商 Key、数据库、Nginx 或生产服务进程。
待部署文件与散列在 `.cache/openai-sol-20261125/backend-ready-manifest.json`，安全准备包为 `backend-ready-no-secrets.zip`。

## 实测与验证

证据目录 `.cache/openai-sol-20261125/`：

- Core：原始 RED 37 项；193 项新回环断言通过，已有 ModelClient 全套通过。
- 正常 Windows App、CLI 和四个 Qt 定点目标构建/链接通过；产品 PE/Qt 部署检查通过。
- 正常 Core 静态库：Sol 193 项通过，ModelClient 全套通过（34.920s）。
- 正常 Qt：Controller、Panel、Login、远端配置定点全部通过，共 47 项；亲看型号菜单和五档强度截图，无裁剪。
- Android Java 47 项通过；iOS 13 项源码检查通过，未做 iOS 实机/模拟器运行。
- 移动 C++ 适配器和测试对象编译通过；Windows 自有链接仍缺既有 `makeMaiOpenSslCliTool`、`registerPlatformPhotoAlbumTools`，未使用桩替代，未宣称移动 C ABI 运行通过。
- Root 独立重跑：凭据服务 10/10、账号服务 244/244，Node v24.14.0。
- 独立源码审查无具体 P1/P2；不把源码审查代替供应商、部署或实机验收。

### 真实 OpenAI 小窗口探测

GET 型号元数据 HTTP 200；随后用实际 C++ Responses 客户端完成两个有界请求。
合成工具 `owned_echo` 只回传固定测试文本，不执行命令、不访问文件、不上传用户历史。
每请求最大输出 256，推理 low，不自动重试；没有进行长上下文或收费工具压力测试。

| 请求 | 输入 | 输出 | total（供应商） | 观察 |
| --- | ---: | ---: | ---: | --- |
| 1 | 77 | 19 | 96 | 一次测试工具调用，原生 continuation 已收到 |
| 2 | 109 | 5 | 114 | 续接工具结果并返回 OK，无重复工具调用 |

累计请求 total 为 210；第二次请求 total 为 114，两者不能混作当前上下文占用。
探测不证明长前缀缓存命中，也未保留/打印 reasoning 或 encrypted payload；加密续接结构由合成测试覆盖，不能反推此次线上一定返回了 reasoning item。
安全回执为 `openai-access-result.json`、`openai-live-result.json`。

### 新客户端私有候选

正常 staging 和实际 ZIP 解压目录分别以裸 PATH 执行 smoke，均退出 0。
App SHA256 `bc2a2399931dba9580e91a3384adaa32246492176206e41e9778d450cdf70cba`。
ZIP 261413550 bytes，SHA256 `33e3d929378a6beb2924adc0494465d0900c3478ba49696da93dccba9ce79e60`。
此候选在 `.cache/openai-sol-20261125/candidate/`，未替换用户正在体验的目录；后台尚未部署，不能称为上线完成。

## 已交付的优先体验包

用户先拿到的稳定包：`C:\Users\18034\Downloads\MaiChat-local-20261126\maichat.exe`，同目录已放 ZIP 和体验说明。
它包含 Responses 改造、新命令工具、PTY、输出补读、侧栏滚动条和已有上下文改进，不包含尚在进行时的 OpenAI 接入。
App SHA256 `3f8e32468e80a10be474f85211710b3e7020e9f53380f27c4903304d77557f8d`；ZIP SHA256 `8175600cbfcc3ce54fd79fff37123792fc4f75b68154492f929dab6c5bf91604`。
实际交付目录独立 smoke 退出 0；后续观察用户已从该目录启动 PID 67168（2026-11-26 00:04:19），未停止、覆盖或重启该进程。

当前 token/窗口 UI 的真实 usage 统一接入、Codex 式增长计量、联合存储正式切换尚未完成，未将它们标为此体验包的完成功能。
