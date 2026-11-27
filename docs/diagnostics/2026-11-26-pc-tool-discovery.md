# PC 模型工具声明按需加载

日期：2026-11-26。平台：Windows x64 / MSVC 14.44。源码基线：
`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，叠加当前工作区既有改动与本次修改。

## 复现与预期

1. 产品宿主设置 `MaiAgent::Options::toolDiscoveryEnabled = true`，新建无历史会话。
2. 捕获首次实际模型请求的 `tools`：只包含已公开注册且当前型号支持的基础工具。
3. 请求 `tool_search` 查询其他能力：只返回有界名称/描述，不返回参数 Schema 或技能正文。
4. 以 `select` 选择精确名称：完整声明追加到下一次请求；同一响应中抢先调用该名称仍被拒绝。
5. 后续轮次、切换型号再切回、关闭并重新打开持久存储：已加载声明的字节与顺序保持固定。
6. 无快照但已有历史的旧会话：保留完整声明；不宣称旧会话头部自动缩小。

原路径 `MaiTurnRunner::buildRequest` 每次直接使用 `registry.specs(model)`，
因此所有工具的描述和 Schema 都进入每次模型请求；只增加搜索工具不会减少这些声明。

## 实现与边界

普通 `MaiModelToolCatalog` 核心同时供 `MaiAgent::searchTools` 与 `tool_search` 使用。
基础集合按现有注册表实际能力选择：`exec_command`、`write_stdin`、`skill_read`、
`tool_search`、`apply_patch`、`view_image`、`session_history`、`tool_output_read`。
没有注册或当前型号不支持的项不虚构；两种来源补读工具始终列在自举候选中。

查询最多 4096 字节，每页 1..10 项，描述最多 512 UTF-8 字节，每次最多选择 5 个名称；
模型结果另限 64 KiB。普通注册表元数据接口不构造 Schema。只有显式选择才调用真实
`specForModel` 并追加完整声明，不把全部目录字符串拼入指令或上下文。

快照使用现有会话上下文的 insert-if-absent：不可变会话模式、按型号隔离的基础声明，
以及按数字序号追加的不可变选择批次。冷启动只重建一次，后续请求使用内存快照；
不按每次请求扫描 JSONL。每型号最多 1024 个追加批次。所有新增存储键使用现有接口，
没有数据库 Schema 或存储虚方法变更。旧自定义后端若明确不支持会话上下文，
仅保证当前 Agent 生命周期内固定，不声称跨进程保存。

新会话在 `MaiCreateSession` 成功保存后、发布 `SessionCreated` 前固定 bounded 模式。
不能等到首次模型请求才按尾部消息数量判断，否则技能扫描期间到达的 pending 补充
会把全新会话误判为旧历史。保守历史判断仅用于没有模式标记的既存会话。
模式保存失败不更新准备缓存，也不发布成功创建事件；已有不可变历史模式不被覆盖。

执行准入使用实际发送请求冻结的名称集合；串行、并行及审批查找均复用同一检查。
目录中加载过但本次没有声明、内部、未知或当前型号不支持的工具均不能执行。
原有文件边界、审批和能力判断保持生效。库默认关闭此功能，保持原公开工具行为。

## 验证记录

目录核心的首次 RED 对照出现 15 条预期失败，覆盖初始声明、搜索、按需加载、
跨会话/型号隔离、恢复及旧前缀。随后核心 GREEN。

`.cache/pc-tool-discovery-20261126/fresh` 从 116 个当前核心翻译单元完整编译，
没有用旧核心对象链接新增 Options/Dependencies 布局。该构建的
`MaiModelToolCatalogTests`、`MaiToolDiscoveryRuntimeTests`、`MaiTurnRunnerTests`、
`MaiSkillRuntimeTests` 均通过；目录测试包含 SQLite 真正关闭/重开、工具参数校验，
运行时测试包含同一响应提前调用、后续请求准入、内部工具隔离与库默认行为。
回执位于 `fresh/test-cohort-02/receipts.json`。

补充的新会话 pending 输入用例先验证 3 条预期 RED，再修改创建路径；
注入会话上下文保存失败确认不会标记准备完成，旧会话模式仍保留完整声明。
`.cache/pc-tool-discovery-20261126/delta-marker` 使用同一 Options/Runner 布局的私有
核心归档，仅重编译修改的 `MaiAgent.cpp` 与目录核心。原生依赖复制到私有目录，
逐个核对复制前/后 SHA256，避免与主任务正常构建竞争库文件。目录及真实 Agent
运行时测试均再次通过，包含最后补充的超限保存失败与 Schema 变化恢复断言。

该原生配置真实内置目录统计：完整 42 项 / 25221 字节，初始 5 项 / 6003 字节，
约减少 76.2%。此配置没有启用技能目录，也不虚构未注册的基础能力。
字节只统计工具名称/描述/Schema 字段，不代表供应商 usage 或最终 wire 请求字节。
Desktop 宿主额外工具的实际数量、运行产物和 UI 实机验收由主任务记录；
本子任务不宣称完成 GUI 实机验收。
