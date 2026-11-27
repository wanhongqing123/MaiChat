# MaiAgent

Multi-AI Code 的 agent 核心，纯 C++，可移植。

目标不是把 opencode 翻译成 C++，是**把 agent 核心做成一个能被到处链进去的库**——
Qt 桌面、iOS/Android、Linux 服务器、嵌入式。

## 形状

```
include/     核心公开头（MaiAgent.h / MaiTool.h / ...）。对外只有 submit(Op) 和事件流
Agent/       会话、事件、模型、存储、路径等核心实现
AgentTools/  工具、权限路径边界及平台工具实现
cli/         maiagent-console：只链核心的交互式控制台。**可摘**
tests/       单元测试 + 一个端到端脚本
docs/        编码规范
```

文件名、类名一律大驼峰加 `Mai` 前缀，不用命名空间——**规范见
[`docs/CodingStyle.md`](docs/CodingStyle.md)，动代码前先看**。
根目录的 `.clang-format` 是它的可执行部分。

可选 Graphics 模块的对外头为 [`include/MaiGraphics.h`](include/MaiGraphics.h)，
直接暴露 OBS 的 `gs_*` 接口。启用 `MAIAGENT_BUILD_GRAPHICS=ON` 后，
支持的平台可链接 `MaiAgent::Graphics`；当前 macOS Apple Silicon 与 iOS
使用 Metal，Android 使用 GLES。MaiChat 的图片呈现和视频播放适配代码
位于 [`../MaiChat/Media/`](../MaiChat/Media/)；Graphics 模块只提供底层绘制能力。
构建、后端路径与限制见 [`Graphics/README.md`](Graphics/README.md)。

## 这不是一个服务端

产物是 **`maiagent` 这个库**。这个仓库里现在**一个 HTTP 服务端都没有**——
连能提供服务端的代码都没有。

曾经有过：`adapters/MaiHttpAdapter`（REST + SSE）加 `maiagent-bridge`，
637 行，存在的唯一理由是当时的界面是 Electron、JS 写的、调不了 C++，
只会说 HTTP。那个界面要撤，这层壳跟着撤了。

这句话不是靠文档维持的，是**构建系统盯着的**：`maiagent-console` 只链
`maiagent`，而 httplib 只剩 `mai_thirdparty_for_tests` 这一个去处。
`tests/e2e/console_e2e.py` 还会翻一遍产出的 exe，确认里面没有 httplib、
也没有那几条 REST 路由——对照组是 `MaiModelClientTests`，它**故意**链了
httplib 当假的模型服务端，免得这条检查恒为真。

Qt 桌面 / iOS / Android / 嵌入式全都和控制台一样直接链库：

```cpp
MaiAgent agent(std::move(store), std::move(model), std::move(tools));
agent.eventBus().subscribe([](const MaiEvent& e) { /* 刷界面 */ });
agent.submit(MaiSendPrompt{sessionId, "你好"});
```

摘掉那层壳的时候核心一行都没改——这就是当初要有这条边界的意思。

**别把两个 HTTP 搞混**，它们方向相反，现在只剩一个：

| | 是什么 | 干嘛用 | 现状 |
|---|---|---|---|
| libcurl | HTTP **客户端** | 核心去调大模型 | 在，摘不掉 |
| httplib | HTTP **服务端** | 以前让 Electron 连进来 | 产物里没了，只剩测试当假模型用 |

核心的边界是 `MaiAgent::submit(MaiOperation)` 加一条事件流，形状抄 codex
（它的 `core/src/codex_thread.rs` 对外就 `submit` / `next_event` 两个方法）。
MaiChat 宿主可注册进程内 FFmpeg/FFprobe 工具，接口和限制见
[`docs/EmbeddedFfmpegTools.md`](docs/EmbeddedFfmpegTools.md)。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 五套单元测试
./build/bin/MaiCoreTests            # id / 会话 / 事件总线
./build/bin/MaiModelClientTests     # 流式解析 + 工具调用分片聚合
./build/bin/MaiTurnRunnerTests      # 一轮对话的完整生命周期
./build/bin/MaiToolTests            # 路径边界 + 四个内置工具
./build/bin/MaiToolLoopTests        # 工具循环：调用 -> 执行 -> 回灌 -> 再答
./build/bin/MaiPermissionTests      # 权限闸门：拦住 -> 等裁决 -> 放行或拒绝
./build/bin/MaiStoreTests           # 存储契约：内存和 SQLite 行为必须一致

# 只有 MaiModelClientTests 会起 socket——它测的就是传输和线格式。
# 其余几套直接实现 MaiModelClient 接口（tests/support/MaiFakeModelClient），
# 进程内跑，没有端口也没有 sleep。理由见 docs/CodingStyle.md 7.6。

# 端到端：自己起假模型和控制台，不需要 API key
python tests/e2e/console_e2e.py

# 交互式控制台：一个进程，不起服务端，中文输入输出都走宽字符 API
./build/bin/maiagent-console --help
./build/bin/maiagent-console                            # 空转，不接模型
./build/bin/maiagent-console --model-url http://127.0.0.1:11434/v1 --model qwen2.5
./build/bin/maiagent-console --db ./agent.db --dir E:/some/project

```

Windows 上要先进 MSVC 环境（`vcvars64.bat`）。

**构建不需要网络。** 第三方依赖（nlohmann / cpp-httplib / sqlite3 / libcurl）
全部在 `third_party/` 里，没有下载步骤——这个项目要交叉编译到嵌入式设备，
那边通常没网，而配置阶段失败比编译失败更难查。

## 进度

- [x] **M1 空转骨架** — session 增删查 + 事件流
- [x] **M2 能聊天** — Chat Completions 流式客户端 + agent loop + prompt/interrupt
- [x] **M3 能干活** — 工具注册表 + 文件创建/读取/写入/删除/查找 + 工具循环
- [x] **M4 权限闸门** — 文件变更前按审批策略询问，授权按目标路径收窄
- [x] **M5 落库** — SQLite 持久化 + 切模型 + 写失败不再无声无息
- [x] **M6 脱壳验证** — `maiagent-console` 只链 `maiagent`，HTTP 整块摘掉

## 权限闸门

`create_file` 只创建空文件，不覆盖已有文件；`create_directory` 可递归创建目录；
`delete_file` 只删除单个文件，不递归删除目录。已有的 `write` 会写入内容并覆盖文件。
这些文件变更工具都经过审批闸门；只读的 `read` / `glob` / `grep` 不问。
`read` 和 `grep` 会先把文本转成 UTF-8，编码不明确时可传 `encoding`（如 `gb18030`）。

一次授权的完整链路：

```
工具卡进入 pending 状态          message.part.updated
核心广播"有人在等授权"            permission.asked   { permissionID, partID }
        ↓  这一轮在这里阻塞（只挂住它自己那个线程）
界面裁决                          MaiReplyPermission{id, Approved}
核心广播裁决结果                  permission.replied { permissionID, detail }
工具卡进入 running 并真正执行      message.part.updated
```

`decision` 三选一：`approved`（就这一次）、`approved_for_session`（这个会话里相同目标路径以后别问了）、
`denied`。**认不出来的取值一律 400**，不会兜底成放行——闸门不该被一个错别字拆掉。

`GET /api/permission` 列出当前所有待裁决的请求。界面重连之后必须拉一次：
界面断开的那个窗口期里发出的 `permission.asked` 是看不到的，只靠事件流会
漏掉整整一次授权请求，那一轮就一直挂着而界面上什么都没显示。

几个刻意的选择：

- **默认不超时。** 超时自动拒绝等于替用户做了决定，而用户可能只是走开了。
  等待期间会话是"在跑"状态，界面上看得见，随时可以中断。无人值守的场景用
  `--permission-timeout <ms>`。
- **所有兜底方向都是拒绝。** 中断、超时、没接闸门、decision 解析失败——
  全部按拒绝走。兜底成允许意味着"没人点头"也能改用户的文件。
- **被拒之后同样的调用不再问第二次。** 模型被拒后经常原样重试，每次都弹框
  会把用户烦死。同一轮里相同的 (工具, 参数) 直接回同样的拒绝。
- **拒绝时告诉模型"不要重试"。** 不写这句，模型会把 12 圈全烧在同一个被拒的
  操作上，用户看到的是 agent 卡住了。

## 工具输出补读与恢复事实

`exec_command` 启动一个进程，参数为 `cmd`；等待窗口到期时返回数字 `session_id`，
后续使用 `write_stdin` 续接。默认不设整个进程的硬期限；`yield_time_ms` 只限制本次等待。
默认管道关闭标准输入；普通交互输入要求启动时指定 `tty:true`，Ctrl-C 中断原进程。
两种工具都返回统一 JSON，仅包含本次新输出、等待时间及已观察到的会话/退出事实。
输出默认预算 10000 个近似 token、策略上限 1 MiB，超出时保留头尾并标明截断前数量。
不再注册旧 `shell` / `shell_input`；历史记录仍按原始工具名称展示。

Windows 管道和 ConPTY 的前台命令正常退出不再误杀其后台服务。结果可同时包含
`exit_code` 和 `background_running:true`：前者属于启动命令，后者表示原 Job 仍拥有后台成员。
后台组继续占用每会话/全局进程额度；自然退出、显式期限、取消、清空/删除所有者或析构仍会回收。
使用服务自己的关闭命令结束正常工作流，不要因为启动命令退出就重复启动服务。

宿主提供标准会话存储时，每次调用保存至多 16 MiB 的原始输出快照。
模型从宿主执行观察或截断结果中取得 `output_id`，再调用
`tool_output_read`，每页至多 4096 个源字节。普通代码使用同一个存储实现：

```cpp
auto page = agent.readToolOutput(sessionId, {outputId, offsetBytes, 4096});
// 成功后以 page.value().nextOffsetBytes 继续；0 表示保留快照读完。
```

句柄不赋予跨会话权限。正式后端重新打开后仍可读；清空或删除会话会使快照不可查询。
保留输出的逻辑载荷配额为每会话 64 MiB、全库 256 MiB，超额淘汰最旧快照；
这不限制 SQLite 文件/WAL 或追加 JSONL 的实际磁盘大小；权威历史不因配额淘汰而改写。
存储失败或句柄已淘汰时明确报错，
不会重新执行命令来补日志。捕获达到上限、超时、取消或采集失败时标记不完整；
完整性与“是否还有下一页”是两个独立事实。Windows 沿用 UTF-8/OEM 文本解码，
POSIX 和独立存储接口保留原字节；非法 UTF-8 的模型页附带 `raw_base64`。

`MaiToolExecution` 单独记录执行状态、可确认的退出码、超时/取消、输出/任务句柄和
恢复动作。原始工具输出及媒体 JSON 不改包，模型在完整工具回执批次之后收到独立的
宿主观察。未知结果不能解释成“没有执行”：先核对当前状态；已接受的媒体任务复用
原任务 ID 查询，已确认失败/取消的任务不再盲目轮询。恢复建议本身不会自动调用工具，
权限拒绝和用户中断仍保持原来的停止语义。进程崩溃前尚未返回的 shell 输出不保证已保存。

## 落库

默认**不落盘**。`maiagent-console` 不给 `--db` 就是纯内存，进程退出什么都不留。
悄悄在用户机器上建个数据库文件不合适，要留历史就显式给路径：

```bash
./build/bin/maiagent-console --db ./agent.db
```

库里直接用是 `makeMaiSqliteStore(path)`，返回 `MaiResult`——打不开就是错误，
不会给你一个用起来处处出错的半死对象。正式实现是同一账户根下的追加 JSONL 权威原文
与可重建 SQLite 索引。传 `":memory:"` 会使用内存索引和自有临时日志目录，析构清理，
仍有文件 I/O；真正不落盘应使用 `makeMaiMemoryStore()`。

会话日志为 `sessions/YYYY/MM/DD/rollout-<UTC>-<UUIDv7>.jsonl`，外壳固定为
`timestamp`、`ordinal`、`type`、`payload`。正文、思考、原始 Responses item 与工具结果
保留在日志；索引使用偏移和长度，`item_json` 等虚拟字段通过受限回读取得原文。
普通 sqlite3 可查元数据；查询正文生成列需要正式后端注册的读取函数，不能把缺少函数
解释成正文丢失。原有 `sessions/messages/parts` 领域查询和导入扩展仍共用该后端。
旧 1..15 数据库只读迁移，验证新索引后发布，原库保留为迁移备份。

完成 item 才持久化；新流式未完成草稿只保留在内存，不承诺重启恢复。旧库已有的草稿
迁移时仍完整保留。已完成 A 后 B 尚未完成就失败，A 留在历史，B 不回灌给模型。

消息和 part 的顺序靠 `ORDER BY id`。这不是偷懒：`MaiIdGenerator` 产出的 id 是
定长的"时间戳 + 同毫秒序号 + 随机"，用的 base32 字母表在 ASCII 里递增，
所以字典序就是生成顺序。`MaiStoreTests` 里有一条用例专门盯着这个性质。

**备份注意**：必须保留整个账号的 `sessions/`，包括其中 `.journal/` 事务账本；
只拷 `agent.db` 无法备份正文。SQLite 仍使用 WAL，在线拷贝需要一致性快照，不能仅复制
主文件。源记录缺失或尾部损坏会明确失败并保留原文件，不自动截断或制造空历史。

**写失败不再无声无息**：存储的写接口返回 void（它们在流式热路径上，
每次都检查会把代码淹掉），但每轮结束时 `MaiTurnRunner` 会查一次
`lastWriteError()`，失败就把会话标成错误让界面看见。磁盘满了还假装存上了，
是用户第二天打开发现对话没了的那种 bug。

## Skills

桌面、控制台和移动宿主启用标准 `.agents/skills` 目录；库使用者通过
`MaiAgent::Options::skillsEnabled` 显式选择。扫描用户 home 和工作目录祖先，项目内遇到
Git 根停止向上；移动端 home 是宿主沙盒。每个技能为 `<name>/SKILL.md`，包含名称与
描述的 frontmatter。没有引入 YAML 或其他新依赖，无法解析的元数据明确列为发现错误。

目录只发送名称、描述和路径，正文按需读取。用户写 `$name` 或链接到具体 SKILL.md 时，
只加载明确选中的技能；重名只允许路径消歧。同一输入的技能正文固定保存到会话日志，
后续工具跟进和重启不因文件改变而改写旧输入。新输入可以读取更新后的正文。
工作目录变化只影响新输入的目录上下文；已发送基础提示保持稳定。
新用户输入会重新发现安装/移除后的目录。元数据仅在相对上一条适用输入发生变化时追加，
每输入保存小型目录摘要以跨重启去重，不在每条消息重复整个技能目录，也不改写历史正文。

用户要求从公开 GitHub 链接安装时，模型通过 `skill_install` 的 `mode=discover` 查看实际
SKILL.md 候选与 README 链接，再用 `mode=install` 选择目录及固定提交。普通宿主使用同一个
`MaiSkillInstaller`。默认安装到用户 `.agents/skills`，工作区作用域须显式选择；目录整体无覆盖发布，
保留资源、执行位和来源清单，不执行下载脚本或安装运行依赖。框架根仓库不等于某一个技能。

普通调用使用 `MaiSkillManager` 的 snapshot/find/read/readCurrent/explicitContext；
模型使用 `skill_read`，复用同一实现。技能内容是用户级上下文，不提升权限，也不自动
运行技能脚本。脚本和附带资源的读取仍遵守现有路径与工具授权规则。

初始技能目录按 Codex 0.172 的预算提供：容量未知时最多 8000 Unicode 字符；
容量已知时取窗口的 2%，显式 `maxCatalogTokens` 最多 10000 tokens。先缩短描述，
再省略整项，并明确数量；`skill_read` 的 `mode=discover` 可搜索完整元数据及分页，
不读取未选中的正文。`agents/openai.yaml` 中普通布尔形式的
`policy.allow_implicit_invocation=false` 排除隐式清单，但仍可显式选择。

## PC 工具声明与实际用量

基础文件流程按 Codex 对齐：通过 `exec_command` 使用实际系统 Shell 读取和搜索，
通过 `apply_patch` 修改文本。Windows PowerShell 启动时设置 UTF-8 输出；Unix 可执行
工具是否存在取决于实际 PATH，缺少 rg 时使用原生替代命令，不假装产品附带它。

`apply_patch` 在 OpenAI 型号使用原生自由文本/Lark 输入，在兼容型号使用相同补丁的
`patch` 字符串函数参数，执行核心共用普通 `maiApplyPatch()`。按上游顺序应用文件块，
Add 可覆盖、Move 先写目标后移除源；失败列出已应用路径，不提供整体事务回滚。
原 `file_patch` 名称只保留历史分类。工具快照 v2 显式记录本次协议升级，v1 原文不改写。

桌面账号客户端和控制台启用 `toolDiscoveryEnabled`，新会话只提供 Shell、Skills、
补丁、图像和必要原史补读等已注册的基础工具；其它能力通过 `tool_search` 的有界
元数据搜索和 `select` 选择。Schema 从下一次请求开始追加，同一次响应抢先调用会拒绝。
旧历史没有目录快照时保持原声明范围，避免追改旧前缀。普通注入客户端和库默认完整目录，
调用者可显式开启相同功能。

HTTP Responses 仍发送完整有效上下文和已选声明，保持历史消息与已保存声明字节稳定；
不把 HTTP 传输说成只发送新增内容，也不承诺供应商缓存一定命中。缓存输入仍占窗口。

状态展示使用 `MaiModelContextUsage` 和普通 `getModelContextUsage()`，窗口占用取最近
主请求的服务端 `total_tokens`，不使用会话累计或本地 tokenizer 估算。缺失总数只能由
已知 input/output 安全相加并标为推导；缺失/失效窗口不显示百分比。摘要消耗进入累计，
不能覆盖主请求占用。12000 基线只用于显示归一化，不用于压缩触发。

发送保护使用实测输入前缀加未覆盖尾部估计。最多 2 KiB 的版本化指纹和边界随用量记录
持久化，不重复保存整份请求；型号、路由、窗口、检查点或既有前缀不匹配就回退普通估计。
路径图片的像素不能仅凭路径证明不变，因此采用保守回退。此保护估计不是实际 usage。

## 文件操作

桌面宿主默认不注册独立 `file_*` 模型工具，读取/搜索通过 `exec_command`，文本修改通过
`apply_patch`；共享文件核心不删除。iOS/Android 同样不向模型注册独立 `file_*`，
改由不创建外部进程的 `exec_command` 脚本入口处理：引号、变量、顺序/短路、有界管道与重定向
调到同一普通文件核心。它不是完整 Bash，不支持外部程序、PTY、后台或命令替换，也没有
`write_stdin`。curl/FFmpeg 仍是各自独立的内嵌工具，不宣称已被该解释器调度。

原生命令开放 `ls/cat/cp/mv/mkdir/rm/touch/echo/head/tail/grep/find/stat/rmdir/read-text` 的有界子集；
以工具说明为准，不推断所有 Unix 标志均可用。`cp/mv` 无覆盖，`rm` 仅单文件，`touch`
只确保文件存在而不更新已有时间戳。目录与变量只存在于一次脚本内，进程环境不改变。
文件访问受宿主 App 容器边界约束；相册/文档的系统授权不是普通路径授权，当前尚未挂载。
外部原件不提供删除入口，App 内副本可按授权修改/清理。

`read-text --encoding E --offset N --limit N PATH` 复用普通流式读取核心，返回编码、行号、
EOF、截断与可信 `next_cursor`；后继调用传 `--cursor` 并保持路径/编码，不能同时传 offset。
长行片段可以按行号和行内 UTF-8 偏移连续拼回。根、会话、路径、编码和文件快照均核对，
源变化或游标过期时拒绝；游标不是权限。`find/grep` 同样可按 `--limit/--cursor` 续页。
`grep` 默认安全正则，`-F` 为字面串，模式需引用以免被脚本语法解释。
搜索 stdout 为数据，stderr 为完整 search_status JSON；read-text stdout 为 text_page JSON。
命令输出预算包含续页事实，不把拿不到尾部游标的截断页冒充完整结果。文本修改统一用
`apply_patch`，不另推荐 `file_edit`。库内原有工厂仍用于协议/算法回归，不代表产品公开声明。

**库代码里不出现 `std::filesystem` 和 `fstream`。** 路径是 `MaiFilePath`，
操作是 `MaiFileSystem`：Windows 走宽字符 Win32 API（`CreateFileW` /
`FindFirstFileExW` / `GetFinalPathNameByHandleW`），其它平台走 POSIX
（`open` / `opendir` / `realpath`）。

形状参考 chromium 的 `base/files`。最关键的一点是**内部存平台原生串**
（Windows 上 `wstring`，其它平台 `string`），只在跟模型打交道的边界转
UTF-8——POSIX 的文件名是任意字节序列，不保证是合法 UTF-8，统一成 UTF-8
会改写文件名。

> POSIX 那一份（`MaiFileSystemPosix.cpp`）**没有在真机上跑过**，
> 本项目目前只在 Windows 上构建。`MaiFilePathTests` 是可移植的，
> 第一次在 Linux/mac 上构建时先跑那一套。

## 三条铁律

1. **公开头 `include/` 里不出现 JSON、HTTP、Qt。** 对外 API 一律原生结构体——
   这是"能被到处链进去"的前提。

   JSON 只在实现内部的协议、工具参数、JSONL 和持久化 codec 边界使用。
   普通业务接口仍使用原生值；内部 codec 可返回不透明序列化字符串，不把 JSON 类型
   或 HTTP 类型暴露给宿主。

   分层可以直接量：

   ```bat
   dumpbin /symbols build\lib\maiagent.lib | findstr /C:httplib   :: 应为 0 条
   ```

   核心链 libcurl（HTTP **客户端**，用来调大模型），不链 cpp-httplib
   （HTTP **服务端**）。现在这条线还有更硬的一道守着：`maiagent-console`
   只链 `maiagent`，httplib 只在 `mai_thirdparty_for_tests` 里，
   漏进核心就当场链不过。

2. **构建产物只放 `MaiAgent/build/`。** 仓库根目录那个 `build/` 是
   electron-builder 在用，里面有被 git 跟踪的图标文件，别污染它。

3. **路径一律走 `MaiFilePath::fromUtf8` / `toUtf8`**（`include/MaiFilePath.h`）。
   MSVC 的 `fs::path` 把 narrow 字符串按当前 ANSI 代码页解释，中文环境是 GBK，
   而我们的路径全来自 JSON、是 UTF-8。直接 `fs::path(s)` 会让中文路径出错——
   实测是进程直接挂掉，不是返回错误。测试里写中文路径要用 `std::filesystem::u8path`。
