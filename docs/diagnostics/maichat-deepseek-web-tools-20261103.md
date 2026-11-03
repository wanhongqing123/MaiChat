# DeepSeek联网检索工具接入（2026-11-03）

基线main `2c823e74`，最终源以本记录所在提交为准。用户要求搜索不再抓取Bing等搜索HTML，
curl用于文件传输、API/自建后台交互及连通性诊断；公共检索独立为web_search/web_fetch。

## 复现与预期

在正常本机会话请求 `site:example.com "exact phrase" lang:zh after:2026` 搜索：
query操作符原样进入供应商Search JSON，返回title/url/snippet/summary/site/date/authority，
未知元数据保留null或authority=-1；无浏览器弹窗/搜索引擎HTML。指定URL取正文走Fetch，
返回Markdown或明确失败。403/404/402不盲重试，临时错误同次调用最多自动再试一次。

## 实现

普通接口 `MaiDeepSeekWebClient` 与两个Agent工具复用同一个客户端。生产仅允许
`https://api.deepseek.com/v1/tools/search` 和 `/v1/tools/fetch`，不跟随API重定向。
客户端直连供应商API；供应商从其服务端检索/抓取，不新增MaiChat模型代理服务。
凭据每次从现有账号控制面缓存读取，iOS/Android私有host回调和Desktop provider均不进入
工具参数、返回值、日志或仓库。CLI环境无Key不等于App凭据未配置，本批未翻找系统密钥。

Search限制query/原问题128个Unicode字符、count1..50，保留site/lang/日期语法；Fetch限制
HTTP(S)、禁止URL用户名密码、8192字节URL，正文最多64000码点并在UTF-8边界截断。
网络正文上限2MiB，默认30秒超时；取消传递libcurl。仅临时网络错误及408/429/500/502/503/504
最多重试一次。API错误输出稳定code、reason、http_status、provider_api阶段、attempts和retryable。
404只说明供应商无可提取正文，不能虚构确切origin状态或分清所有登录墙/验证码原因。
200空正文、识别到的挑战/登录页或未提取的原始HTML也算失败，不伪装成功。

工具schema拒绝未知字段/类型和整数溢出；工具显式标记供应商按成功非空请求计费，沿现有
requiresPaidApproval审批规则，不为UI验收触发真实计费请求。curl原HTTP功能保持，description
说明其传输/API/诊断用途并引导搜索用web_search、文章提取用web_fetch。

## 主要证据

官方协议直接取自下列一手Markdown（本机HTTP200，未提供真实Key）：
- https://api-docs.deepseek.com/guides/search.md
- https://api-docs.deepseek.com/api/search.md
- https://api-docs.deepseek.com/api/fetch.md

原协议副本及SHA：`/private/tmp/maichat-web-retrieval-evaluation-20261103/docs-receipt.json`。
测试使用真实本地HTTP/TLS服务和临时内存私钥，仅public CA进入临时文件，不外发请求。
首轮真实发现并修复：authority UINT64_MAX/整数回绕；空原问题仍发请求；带空白/大写的
原始HTML误判成功；非法UTF-8 URL在读Key后JSON编码抛异常。保留红日志，没有放宽断言。

最终DeepSeek专测0失败，含Unicode128/129、完整操作符、null未知元数据、空results、
严格参数/溢出、空/验证码/登录/HTML分类、2MiB边界、重试上限、取消、Key轮换，以及
真实TLS信任/hostname失败和HTTPS302不向HTTP目标转发任何请求/Key。
同一源码最终核心八套重新链接，8/8通过（91.80秒），18385个源文件/配置前后SHA不变。
回执 `/private/tmp/maichat-deepseek-web-tests-20261103/final-evidence.json`。

## 验证边界

未对真实供应商Search/Fetch执行收费调用，不宣称已验证所有目标网站的召回或反爬绕过能力。
没有改服务端部署/凭据下发策略；真实App需有效账号下发DeepSeek Key，首次工具调用依既有
计费确认执行。iOS/Android/Desktop正常构建与安装收据由主批次记录，不能拿本地TLS测试
替代真实服务与用户数据验收。
