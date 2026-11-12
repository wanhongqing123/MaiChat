# 原生工具输出保留与执行恢复信息

## 需求与验收场景

用户批准先实现两项：长输出执行一次后可继续读取；工具明确提供执行事实与恢复动作。
本轮不接入 MCP/Skill，不增加服务、不改进程拓扑，不自动重新执行命令或提交供应商任务。

1. 自有命令写一次计数文件，输出超过 100 KiB 和末尾标记。
   初始预览保持有界；模型使用句柄读到末尾；重新打开 SQLite 后仍能读；计数为一。
2. 启动失败、非零退出、超时、取消与结果未知分开记录。取消/超时保留已捕获部分，
   补读结果标记不完整；不知道副作用时先核现状。
3. 已接受任务查询断网时保留原任务 ID；已确认失败/取消不再循环查询，
   已成功但产物读取失败可以恢复同一产物。日志自述的任务 ID 不能提升为可信事实。

## 时间、源码与产物

- 平台：Windows x64，MSVC 14.44，Release；2026-11-12（Asia/Shanghai）。
- 开始基线：`dabe95b854b6345dae512dbd4eb77607a3973494`，在 main 上修改。
- 本轮 C++/构建/测试变更的 34 项冻结清单：
  `.cache/tool-output-recovery-20261112/frozen-source.json`，最终逐项 SHA256 复核不变。
- 核心构建：`.cache/maiagent-core-gate/build-followup-20261105`。
  它的 CMake 包装直接 add_subdirectory 当前 `MaiAgent`，仅复用原 C TLS 库；
  Ninja header dependencies 已核实有效，新增公开结构的使用方实际重新编译并链接。
- 普通桌面构建：`MaiChat/desktop/.cache/build-qt6-ui-20261109`，完整 App/Qt 运行库检查通过。
- 候选 exe SHA256：`f7c7fbad681d7ab8b8d489ae62dc833191081571a5a1ade691b62f07c10a5d70`。
- 私有 ZIP：216968388 字节，SHA256
  `fe91cd4de975ecba0c8b37e20c86eae32f61cc172980dff8a60390a0801ee708`。
  文件名仍含构建时基线 dabe95b8；它是该基线加本轮冻结改动的候选，不能当成原提交的发布资产。
  版本仍为 2.2.58，没有 bump/tag/publish/安装。

## 根因与实现

### 长输出

旧 shell 只捕获 48 KiB，尾部在读取管道时丢失，提示模型重新执行、减少打印。
现在捕获上限为 16 MiB，初始预览保持 48 KiB。快照与预览分别存储，消息历史不反复载入全文。
输出按 session/message/part/call 验证归属，句柄不可变；已淘汰 ID 保留小墓碑防止复用。
SQLite 使用 BLOB 保字节，通过有限 substr 窗口分页，不载入全文再切片。

普通入口为 `MaiSessionStore::putToolOutput/readToolOutput` 和 `MaiAgent::readToolOutput`；
模型入口为 `tool_output_read`，共用同一分页实现。只读工具不接收 session_id 或文件路径。
每页最多 4096 源字节，合法 UTF-8 标量不拆分，非法字节单步推进并附 raw_base64 预览。
next_offset_bytes=0 只表示保留快照读完，capture_complete 独立说明捕获是否完整。

完整快照可由既有工具详情 API 读取；中断/不完整捕获继续保留原预览警告，
部分快照通过普通输出 API 或模型补读获取。源字段变化会改变详情 revision。

### 执行与恢复

新增可选 `MaiToolExecution`，与原始 output/error 分开持久化，SQLite 采用 v10→v11 加列迁移。
宿主观察在整批 tool results 之后追加，不改媒体 JSON，不打断 call/result 配对。
metadata 同时参与完整/部分历史指纹与摘要 source_fragments，压缩后仍保留恢复线索。

明确执行前拦截记录 NotStarted；执行后无法确认的失败保守记录 Unknown/InspectState。
任务接受身份只信同会话同工具的任务表或可信 typed 事实，不信模型参数或原始输出的自述。
应用退出遗留的 Running 工具恢复为结果未知，保留原输出；不再提示盲目重试。

两平台 Process 的中断检查移到持续输出也可达的位置；收尾只读取当时已到达的有限字节。
Windows 建立 Job 失败时在恢复子进程执行之前清理并报错，避免留下不受控的进程树。

## 失败对照与修正

所有下面的 RED 均为实际可运行构建上的功能断言失败，编译阻断另列。

| 用例 | RED | 修正后 |
|---|---|---|
| 长输出计数为一，但缺少补读入口 | 唯一 tool_output_read 指引断言失败 | 分页读全、尾标记及计数为一通过 |
| 普通错误没有独立恢复观察 | 1 FAIL | 原 JSON 不变、批次后独立事实通过 |
| 两标准后端没有独立输出存储 | 119 FAIL（NotSupported） | 所有权、配额、重开、迁移和事务故障用例通过 |
| 任意 shell/file_read 输出伪造 task_id | 6 FAIL | 不再升级为可信接受身份 |
| 已确认失败任务仍 QueryStatus | 2 FAIL | Failed/Canceled/Completed 与查询未知分开 |
| 中断快照错误声明完整 | timeout/cancel 两个完整性断言失败 | capture_complete=false |
| Windows 首个 UTF-8 标量就被小预算截断 | 1 FAIL | 3/0/1 字节上限返回正确完整前缀 |
| 补读工具取消缺少 canceled 事实 | 1 FAIL | Canceled、canceled=true、started=false 一致 |

夹具观察问题单列：Windows echo 的尾空格使固定字节计数不成立，改为统计真实 run 标记次数；
取消的 MaiToolResult 沿既有 failure 契约把预览放在 error().message()，原 output() 断言使用了错误入口，
实际快照未丢。自有子进程后来以写出后的 ready 标记同步取消，避免启动时序影响观察。
独立旧库夹具编译脚本曾误改源文件名导致 C1083，不作为功能 RED。

原 Store/Core 用例要求恢复时把 output 覆盖成错误文字，已按新契约改为严格验证保留原输出、
单独 error 和 Unknown 事实；原消息数量断言也严格核对新增观察的位置和 call_id，没有放宽为下界。

## 最终验证

| 检查 | 实际结果 |
|---|---|
| 14 套核心 CTest，含原八套及 Shell/OutputStore/OutputRead/Execution/ContextBuilder/Compactor | 14/14 PASS，exit 0，288.74s |
| console_e2e.py，真实控制台与本地假模型 | 全部检查通过，exit 0；落库及 hard-kill 重开覆盖 |
| 普通 Windows maichat 构建 | exit 0，windeployqt 与 3 binaries/114 PE imports 检查通过 |
| staging 与实际 ZIP 解压目录 | 78 文件逐 SHA256 相等 |
| staging 普通 --smoke | PID 48796，exit 0，2.196s |
| ZIP 普通 --smoke | PID 20776，exit 0；exe SHA 与 staging/构建产物一致 |
| 规格/进程/存储独立审查 | 发现项均有定点 RED→GREEN，最终无未处理 must-fix |

两个 smoke 都用裸系统 PATH，实际读回各自 exe 路径、版本、PID 和包内 Qt 模块位置。
普通 smoke 跳过登录和账号恢复；Qt 可在正常 known-folder 日志追加启动头，未读/复制原账号日志。
完整原日志、源码和可执行文件清单保留在本轮私有 `.cache/tool-output-recovery-20261112/`。

## 边界

- 未使用付费模型/媒体任务，未查询或导出真实会话数据库，未替换现装程序或发布资产。
- 输出在命令返回时一次保存；宿主进程崩溃前尚未返回的 shell 输出不保证已持久化。
- 16/64/256 MiB 分别限制单条、单会话、全库的可读逻辑载荷，不限制 SQLite 文件/WAL 的物理大小。
  捕获超限、保存失败或句柄被淘汰会明确暴露不可恢复边界，不重执行原命令。
- Windows 延续既有 UTF-8/OEM 文本归一化，不声称保留终端原编码；POSIX/直接 BLOB 接口保原字节。
- ce74 的原始运行验证批次为 Windows；POSIX 源码已审阅，本机 WSL 不可用，
  该批次未做 Mac/iOS/Android 构建或实机验收。后续 iOS 普通构建发现并修正移动注册遗漏，
  Simulator 同例及签名 build206 已就绪，真机安装因设备断开尚未完成，
  见 [iOS build206 交付记录](maichat-ios-build206-20261112.md)。
- 通用恢复建议供模型决策，没有新增自动重试执行器；既有权限拒绝、付费提交审批和用户停止语义保留。
