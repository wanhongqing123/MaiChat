# Session 模型与供应商原生检索

基线：`0801974d58e5955af0744a4a00b2718e8c65eed4`，2026-11-10。
按用户已明确的会话隔离、四型号、思考强度与 GLM 原生搜索要求实现。
已交付的 2.2.55 候选及线上服务保持其原冻结源。

## 操作与预期

在创作中心已有更多菜单中展开“主模型”，选择 `glm-5.3`、`glm-5.3-flash`、
`deepseek-flash` 或 `deepseek-v4-pro`。仍在原面板选择思考强度 low/high/max。
修改只写当前目标会话；切换到另一会话、回到原会话、重启后，各自值和实际请求保持一致。
新会话默认保持 deepseek-flash/high，不从刚离开的会话继承。旧空值沿固定初始化默认解析。
目标会话忙碌时拒绝修改，另一个会话仍可独立选择；已经运行的模型请求和工具上下文保持原值。

正文、附件与输入草稿在取消、失配、权限不足或保存失败时保留。型号/强度显示只在持久化
或 owner ACK 成功后更新；HTTP 入队成功不当作远程选择成功。

## 状态与存储

复用 `MaiSession.model` 为唯一型号来源。发送、steering、后台队列不得用全局 UI 值覆写它。
Core 增加 Session reasoningEffort 与 SQLite v10 单列迁移，更新字段追加末尾以保留原聚合
初始化语义；未提供与显式继承的区别由普通 mutation 契约明确。非法组合整次失败。
原始消息和检查点不因切模型改写；压缩容量按本次真实模型重新评估。

平台 sidecar/全局 settings 仅作初始化配置，不再保存当前会话的选择。相较平台各建一个
session map，此方案复用现有持久化、事件与迁移；相较继续改全局 configure，它不需要重建
整个 Agent，也不会阻断其他活动会话。

## 模型能力与实际路由

| ID | 供应商 | 产品中的视觉输入 |
| --- | --- | --- |
| glm-5.3 | GLM | 仅文本 |
| glm-5.3-flash | GLM | 可接图片 |
| deepseek-flash | DeepSeek | 可接图片 |
| deepseek-v4-pro | DeepSeek | 仅文本 |

GLM 主聊使用官方 Chat Completions 路由，DeepSeek 沿 Responses；混合客户端提供按型号
查询 wire 的真实入口，Mixed 不能被当成可发送协议。配置只能来自宿主；模型参数不能指定
endpoint、Key 或代理。每次请求按已冻结的 request.model 读取对应账号 provider Key，不跨
供应商兜底。缺凭据明确失败，不读另一个 provider 的 Key。

四型号的 low/high/max 按供应商实际协议发送；GLM thinking 不能发送 disabled。
文本模型不能被当作视觉模型；图片/截图能力错误必须明确，保留草稿，不静默换型号。
既有本地模型配置路径保留，不借开放四个云模型移除本地能力。

官方依据（2026-11-10 已读取）：

- [DeepSeek 模型元数据](https://api-docs.deepseek.com/api/list-models/)：四型号中的两个 DeepSeek ID、容量、模态与 effort。
- [GLM-5.3](https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3)：仅文本、thinking/effort、Chat/Responses 端点及 Coding Plan 协议限制。
- [GLM-5.3-Flash](https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash)：多模态与文本参数的对应关系。

## 普通接口、工具与跨设备

Core GLM 检索同时提供普通客户端与 `web_search` 工具，共用实现。工具按
`MaiToolContext.model` 分派供应商，不读随后变化的 UI 全局值。GLM 失败不自动重试到
DeepSeek；没有等价供应商能力时明确返回不支持，不虚构原生 fetch。

Mobile JSON 普通入口 `set_model` 接受 session/model/reasoningEffort；成功返回
ok/session/model/reasoningEffort/wire。snapshot 的每个 sessions[] 也返回这些配置，
selectedModel/selectedReasoningEffort/selectedWire 指向实际选中会话。通用 JNI 继续透传 JSON。

远程会话沿既有 owner slot/login/session 与 commandId/requestId 通道使用 `get_session`
和 `set_model`，后者只接受 model/reasoningEffort。成功回执包含同样的 session 配置。
显示状态按 owner tuple 隔离，catalog 刷新不覆盖已确认值；旧登录、旧会话和晚到回执不能
写入当前会话。服务的 presence/catalog 仅用于发现，不作为模型配置数据库。

账号服务只下发 provider 凭据，没有本轮四型号的白名单；本轮无需新增服务 schema、
模型代理、线上配置或部署。主聊和检索继续由客户端直连供应商。

## 分工与验收

- Root：Desktop Controller/Panel/MainWindow、远程 Host/RemotePanel 与对应测试；共享 CMake 与统一集成。
- Mac：Core 路由/能力/GLM 检索/Session effort 存储、共享 Mobile Adapter、iOS 与对应测试；先冻结精确头和 JSON 契约。
- Android：共享契约稳定后，仅薄接菜单、Session 恢复、队列和媒体门禁；不另写搜索 HTTP/JNI。

先以四会话不同型号/强度和迟到凭据回调建立行为 RED，再实现；验证 SQLite 重启、忙碌
隔离、provider Key/端点/wire、两种 HTTP 实际请求与原始图片字节。远程只在 owner ACK 后
更新，覆盖拒绝、离线、切会话及旧登录。UI 采用自有 fixture，亲看菜单与切换画面。
不为界面验证发起付费模型请求，不读取用户数据库，不替换现装客户端。
