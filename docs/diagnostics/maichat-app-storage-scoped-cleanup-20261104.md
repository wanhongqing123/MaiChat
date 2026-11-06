# app_storage：有界预览与工作区中间文件清理

日期：2026-11-04。定位起点提交：`eee43402aff22caeb4b0c2688e381ced569f7e63`。
本记录只涉及实现和隔离测试，未对用户工作区执行扫描或删除，也未访问用户相册、账号数据库。

## 复现与根因

在新建、自有临时目录内建立三个互不嵌套的 temporary/cache/workspace，写入八字节
`temporary/qa-model/public-fixture.gguf`。调用原工具 `{"action":"scan"}`：
temporary.files=1，cleanup_candidate_count=0。仅将同一个文件的修改时间设为 26 小时前，
再次扫描得到候选数 1。递归枚举正常；原因是原实现只接受至少 24 小时前修改的文件。
原响应却将此条件标为 `cleanup_max_age_hours`，且只列前 20 个样例；工作区一律不能清理。

证据保存在 `/private/tmp/maichat-app-storage-20261104-dnxfvndu/age-repro.json`，包括原源码副本、
独立编译链接命令及结果。该复现未读取或删除实际 QA GGUF；其所有权、活动状态及年龄不能从
“临时目录中的模型文件”推断。因此新默认策略另行保护未明确指定范围的 GGUF/ONNX 文件。

## 共用接口与选择规则

普通入口为 `MaiAppStorage::preview/cleanup`；`app_storage` 仅负责参数、现有媒体收据及审批适配。
宿主提供 App 自有且互不嵌套的三个根目录，不能传入系统共享临时目录。旧 `action=scan` 默认仍
只读统计三类目录，工作区文件不进入该预览。临时/缓存默认保留最近 24 小时文件。

工作区必须显式选择 `target=workspace`，并给非空 scope 或有界 patterns。允许的目录族只有：

- `frames_<id>/`（scope 可用 `frames_*` 选这些目录）、`wmframes/`。
- `screenshots/`、`search/`、`browser/`、`.maiagent/model-previews/`。

这些是明确命名的中间文件目录规则，不是基于“所有图片均为成品”或“所有文件均可再生”的推断。
根目录下仅以 `frames_` 开头的成品文件不属于目录族。其余位置的原图、文档和最终媒体不参与
工作区清理。任意层级的 `gallery-*`、已记录交付的媒体、消息附件及宿主保护路径继续保留。
工具从同工作区的根会话及子 Agent 历史读取这些收据；预览和执行缺少会话宿主时均拒绝工作区
清理。普通调用者须提供完整保护清单并显式声明 `protectionComplete`，未知不等于无须保护。

scope 和 patterns 均相对所选根目录；模式只支持单段内 `*`，不跨 `/`，不支持 `**`、`?`、
绝对路径、空范围、`.`、`..`、反斜杠或根目录通配清空。枚举不跟随链接，执行复核每级路径及
解析后的根目录。`min_age_hours` 默认为 24，范围 0..8760；零小时也必须先预览并明确有界范围，
不能对整个临时目录使用。明确选择 QA 权重前，调用者仍须核实所有权及是否仍在使用。

例如，仅预览截图中间目录：

```json
{"action":"cleanup","target":"workspace","scope":"screenshots","min_age_hours":24,"dry_run":true}
```

预览返回 target/scope/patterns、准确的 workspace_cleanup_allowed、最小年龄、年龄和保护排除数量，
以及最多 1000 项的完整候选路径、大小、修改时间、合计数量和字节。超出上限会明确标记，不会在
执行时补入。默认 `dry_run=true`，不触发删除审批。只允许这样执行已确认的预览：

```json
{"action":"cleanup","dry_run":false,"preview_id":"<confirmed preview ID>"}
```

执行仍遵从现有 Agent 审批策略，预览绑定当前服务实例、会话及解析后的全部根目录，十分钟过期、
单次使用。不能同时提交新 scope/target/patterns 来扩大集合。新文件不加入；大小、平台文件身份、
高精度 mtime/ctime 变化、已交付或链接替换的项跳过。绝不删除目录。
此服务不是对不受控并发写入者的文件锁；宿主不得在最终复核和删除之间同时替换同一路径。

## 验证与边界

`MaiAppStorageToolTests` 只创建并删除自身唯一命名的临时测试树，覆盖普通和工具入口：
年龄排除、默认模型保护、超过 20 项的精确清单、1000 项上限、默认只读、逐次审批分类、工作区
允许目录、相册/成品保护、根/会话绑定、同尺寸修改及替换、符号链接、取消和预览后新文件。
另覆盖子会话交付收据、预览后新交付保护，以及执行时宿主丢失不得放行。

隔离红绿对照使用相同测试与普通服务，只替换原/新工具对象；原工具出现明确失败，新工具通过。
正式 CMake 构建在 `/private/tmp/maichat-pdf-core-gate` 成功（65.95 秒），随后 15/15 注册测试通过
（97.37 秒）：Core8、SendMedia/ViewImage/ContextBuilderImage/DeepSeek/LlamaModelClient/LlamaTools
六项既有定点回归及 AppStorage。真实模型环境变量未启用，不发供应商模型请求。
新构建的 `maiagent-console` 通过 `console_e2e.py` 全部检查（7.31 秒），只使用该脚本自己的临时
工作区、SQLite 和 localhost 假模型。九项存储源码/测试/CMake 输入在构建和测试前后摘要一致。

同目录 `normal-final-evidence.json`、`source-freeze.json`、`normal-results.xml`、`normal-ctest.log`
及 `console-e2e.log` 保存源码、产物和命令绑定。核心 archive SHA-256 为
`cc3af3116d92db96d4d15ef4908e23c27b0122bc78f76414b354b7372d7fb705`，AppStorage 测试二进制为
`d6d310684c005ef5c62ed430859e3e4aad43b750079a2ceade7441d889cc613a`。

macOS 的原生测试不能替代 iOS/Android/Windows 安装产物验收；Windows 快照实现已写入但未在
Windows 运行。本任务未执行 App、设备或用户存储操作，也未提交或合并代码。
