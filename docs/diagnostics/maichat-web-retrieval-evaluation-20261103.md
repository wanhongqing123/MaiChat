# App 联网检索接入评估（2026-11-03）

## 现场操作与预期

用户在 iPhone 创作中心查询资料时，直接访问搜索结果页或正文，遇到 HTTP 403、
验证码、登录墙、空 DOM、降级搜索页和 HTTP 429。预期 Agent 有独立的结构化搜索
及 Markdown 正文工具，保留查询算子，明确报告失败，不把空内容或挑战页当成结果。
这次消息要求先确认现状和改动层次；替代之前外置 `dsweb` CLI 的方案。

## 源码证据

检查基线为 main `99594888` 加尚未提交的美颜、图片预览和任务补充修复。
检索评估未引入检索产品代码；原修复工作独立继续。

- `MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:525` 注册 `registerMaiCurlTools`。
  `MaiAgent/AgentTools/MaiCurlTools.cpp` 只注册进程内 `curl` 工具。
- `MaiAgent/AgentTools/MaiCurlCliTool.cpp:400` 将状态、响应头、正文交给模型；
  `text_only` 只对 HTML 去标签。未指定下载路径时，HTTP 403 或 HTTP 200 挑战页
  都可能是成功执行的工具结果，调用方仍需自行判断内容有效性。
- `MaiAgent/AgentTools/MaiWebFetchTool.cpp` 中的 `curl_fetch` 是已有实现，但当前
  移动端注册路径不使用该工厂，不能把它算作 iPhone 已启用的独立检索工具。
- `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` 注册 `mobile_browser`，iOS 通过
  `AIMobileBrowserTools.swift` 中的 WKWebView 在当前执行设备访问网页。
- `MaiAgent/Agent/MaiResponsesClient.cpp:251` 发送的工具都是 function 类型。
  对 `web_search_call` 事件的解析并不等于主动配置了供应商内置搜索。
- 未发现独立的 `web_search`、结构化搜索供应商配置或默认 Bing 搜索路径。
  模型用通用网络工具请求 Bing 页面，与产品预设 Bing 后端不同。
- 账号凭据已有下发路径：`services/model-credentials/index.js` 从服务部署环境的
  `MAICHAT_DEEPSEEK_API_KEY` 获取凭据，经账号接口供客户端使用。iOS 在
  `AICloudCredentialStore` 中按账号接收并在退出时清理。

结论：已有通用联网和交互浏览器，需要新增正式搜索能力，同时补全正文抓取契约；
不能只按“已有搜索引擎效果不好”处理。远程会话的工具在所属执行设备运行，其出口
不能仅凭 iPhone 是操作端来判断。各站失败现象也不足以单独证明统一的 IP 声誉根因。

## 供应商文档验证

本机实际 GET 以下官方文档均返回 HTTP 200、`text/markdown; charset=utf-8`：

- <https://api-docs.deepseek.com/guides/search.md>
- <https://api-docs.deepseek.com/api/search.md>
- <https://api-docs.deepseek.com/api/fetch.md>

指南 SHA-256：`e0016ef337815a28ac1020a93009eec0ef710e403cce594c3f595aff2612d24c`。
文档说明 Search/Fetch 使用 Bearer 认证，分别调用 `/v1/tools/search` 和
`/v1/tools/fetch`，搜索支持用户列出的查询算子，正文返回 Markdown。
Fetch 仍受目标站可用性及访问策略限制；其 HTTP 404 可以合并表示目标站错误、
站点策略、类型不支持或无法提取正文，因此不能承诺供应商总能区分验证码与登录墙。

仅检查当前工具进程环境变量 `DEEPSEEK_API_KEY` 是否非空，结果为**未配置**。
没有查找配置文件、Keychain 或其他密钥，也未输出密钥。此结果不代表 App 的账号
凭据服务未配置。尚未向收费 API 发起真实搜索，无法评价实际召回质量或服务端出口。

## 建议接入边界

1. 共享 C++ 层提供独立普通接口及 `web_search`、`web_fetch` 工具包装，复用同一
   供应商客户端；iOS、Android 和 Desktop 仅接账号凭据、取消与界面通知。
2. 优先评估客户端直连 DeepSeek API，由供应商服务器搜索/抓取目标网站。
   该方案改变目标站抓取出口，不等于将设备到 API 的出口也改成 MaiChat 服务器。
   若后续实测证明设备到 API 本身也不可达，再单独评估 MaiChat 服务端检索服务。
   主聊天和媒体模型仍遵守客户端直连供应商约定。
3. 查询算子原样发送，返回标题、URL、摘要、站点、日期、相关度、供应商权威度；
   未提供的字段保留未知，不推断发布时间或把供应商评分当成事实核验。
4. 返回明确的 `stage/code/http_status/retryable/attempts`。区分传输失败、API
   拒绝、目标站已知拒绝、空正文和供应商未披露的抓取失败。不猜测隐藏原因。
   可重试错误每次请求至多自动重试一次；余额、认证、请求错误不盲目重试。
5. 文档站的 Markdown、llms.txt 和 sitemap 属于可用来源线索，按站点实际声明使用；
   不对任意 URL 机械追加 `.md`。登录/交互任务继续由 `mobile_browser` 承担。
6. 新工具不需要弹出网页或新增可点击功能卡片。检索结果和来源回到原会话；
   测试覆盖 `site:` 等算子保留、空结果、错误分类、重试上限、取消和日志脱敏。

## 验证边界与后续

本次完成源码路径和官方接口文档核对，未实现或发布检索工具，未宣称 API 已打通。
真实联调需通过既有凭据渠道取得可用权限，然后执行用户授权的三条结果搜索与正文
抓取，记录字段、算子约束和具体 HTTP 状态。不得从现有用户数据中另行翻找密钥。
美颜修复、任务补充、图片预览和随后真机安装仍是保留的进行中工作。
