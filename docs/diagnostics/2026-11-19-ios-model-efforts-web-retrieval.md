# iOS 型号思考档位与通用检索补齐

## 操作与期望

源码起点为 `fcf0c3d8d5e014126dddcffaf364c0939f6e44ef`；真机已安装 3.0.1(229)，该版本不包含本轮补充。

1. 创作中心选择 Fable，再打开原思考菜单，应提供低、中、高、超高、最高。设置应在会话读取、重启和远端确认后保持一致。从 Fable 的中或超高切到其他型号时，继承值归一为高；显式请求不支持的型号/档位组合仍应失败。
2. 使用 Fable 的会话请求最新信息时，应能发现 `web_search` 与 `web_fetch`。前者返回标题、链接、摘要，后者返回提取正文；不以浏览器搜索或 curl 抓搜索结果页替代。

## 旧生产失败对照

- 正常 iOS 模拟器 App 的 Fable 菜单只有 3 个按钮，5 档断言实际失败；用例 10.535 秒，构建及运行 44.442 秒。失败前 Model/View 与起点源码逐字节一致，仅新增测试。
- 普通 Apple native C ABI consumer 的 `--model-efforts` 最小 selector：Fable high 正控成功；medium 和 xhigh 的 configure 均返回 `invalid_input`。结果 exit 1，失败 2/检查 2，供应商 HTTP 请求 0；运行 0.580 秒。
- C ABI 对照使用 fresh 编译的起点 CAPI 与测试对象，分别 4.717/3.840 秒、链接 0.285 秒，均 exit 0。复用此前完整构建并验证的同 ABI Apple Core/vendor 库，经 `simctl spawn` 运行；没有新建测试 target、重编 FFmpeg 或安装 App。
- 产品只有 Desktop AgentController、Mobile CAPI 两处检索注册，均经过 `registerMaiProviderWebTools`。其型号选择明确只支持 GLM、DeepSeek，因此 Fable 的工具声明被过滤；不存在另一个已注册的 GLM 搜索入口。

## 实现边界

共享 owner 维护按型号验证/归一的 Core helper、CAPI 投影与产品检索注册，本端维护 Swift 设置、菜单和远端 ACK。Fable 五档为 `low/medium/high/xhigh/max`，其他型号保留原三档；空值保留继承语义。只有继承/切型号路径归一，显式非法输入不能被吞成 high。

`supportedReasoningEfforts` 是 choice、snapshot/remote_snapshot 顶层和 sessions 项内的可选 JSON 数组，不是 C ABI struct 增量。数组不含空值，缺字段时按实际型号兼容旧宿主。

产品通用检索复用已有 `MaiGlmWebClient` 与 `registerMaiGlmWebTools`，明确署名 GLM，与主模型解耦；独立保留 `/api/paas/v4` 搜索/Reader 端点，不沿主模型 Responses 的 `/api/v1` 改写。不新增执行核心、库、MCP、模型代理或其他供应商凭据回退。旧 ProviderWeb 普通库未知型号拒绝的合同保持。

## 当前验证状态

同一最终 C ABI 测试源码在 shared10 v1 正常 Apple consumer 上已通过：0 失败/107 检查，供应商 HTTP 请求 0，运行 0.633 秒。旧源因入口失败跳过依赖步骤，所以只展开 43 项；不是通过修改断言获得绿色。

首轮共享 v1 普通 App 3 项用例通过，192.455 秒，包含五档本地 Responses 请求和 medium/xhigh 切 DeepSeek 的持久 high。独立审查随后找到旧远端历史页的 default 绕过选择守卫，会覆盖新 Fable 的继承显示；同例已实际失败，20.240 秒。

最终 Swift 与 shared v1→v2 的普通模拟器 App 为 3.0.1(230)，3/0 通过，192.414 秒（14.580/172.121/5.713），构建及运行共 308.531 秒；未知默认值、旧页迟到、跨 owner 缺省/null 不继承前一 owner 等反例均已回归。Core 四个关键对象及 CAPI 对象到静态库和 App 合并库逐字节核等。父级另亲看菜单截图；菜单可滚动，截图时低档可在当前视口上方，五档已逐项选择并核对本地请求。旧横屏问题未重测，不计入本轮通过。

最终五文件供件为 55652 字节，SHA256 `f3889ca4b609b25689aea3c55cfa3a6c59ad4952d2f8e801797f17f9258e960f`。四件以完整 fcf0 源为 before；测试以已交付的 canonical fixture blob `ce755e3a0a58a209002866d8cf0572071f9c8f84` 为 before，不重复叠加该旧增量。后继正常真机 Release 构建、安装与进程回读另记，当前不把模拟器 230 当作已安装手机 230。

真实检索接通限定为最终源码冻结后一次固定公开 query（count 1），成功后再一次 example.com Reader（max 256）；search 最终失败即停。普通 Core 每项固有至多一次瞬态重试，无法从 options 关闭；协调后的预算是至多 2 个 operation、4 次 HTTP，不为探针修改生产重试策略。回执分别记录 operation、actualAttempts 和可证明的 HTTP 次数；普通接口未暴露的成功 HTTP 数值不伪造为 200。只保存状态、形状、数量和正文非空布尔，不保存密钥、原始正文或用户内容，不触发主模型请求。

2026-11-19 02:32（UTC+8），shared v1→v2 最终生产源、普通 API 对象归档、CLI 和证书哈希守卫通过后，由唯一消费者完成真实普通 API 接通，exit 0：

- Search：1 operation、1 attempt、1 条结果，标题/链接形状及摘要非空布尔均为 true。
- Reader：只在 Search 成功后执行，1 operation、1 attempt，标题/正文非空布尔为 true，未截断。
- 合计 2 operations、2 attempts，未实际重试。原接口不暴露成功原始 HTTP 数值，回执保留 unknown 与 2 次上界，不伪造精确状态。真实主模型调用为 0；到此停止，不再重复接通。

凭据只由既有 strict-host SSH 读取精确受限字段，在内存交给普通 C++ API 的 stdin；没有保存凭据值、原始供应商正文或用户内容。正常 App 的工具注册验收与上述普通 API 接通分开记录。
