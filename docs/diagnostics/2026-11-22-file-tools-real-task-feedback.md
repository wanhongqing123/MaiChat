# 文件工具真实任务回执修正：复现与验收

日期：2026-11-22；平台：Windows；工作分支 main，保留既有 WIP、不提交。
本轮首先使用正式 Core 工具在实际 `E:/OpenSource/MaiChat` 源码仓库复现；不访问 App DB、用户日志或密钥。

## 修改前复现计划

1. `file_glob(path=MaiAgent,pattern=*,refresh=true)`：记录当前语义、枚举/排名耗时、页与总数，不输出文件正文。
2. `file_grep(path=.,pattern=MaiFileSearch,mode=literal,limit=1,refresh=true)`：记录枚举与实际内容搜索阶段；零工作量不能被解释成无命中。
3. 读取实际头文件前40行及源码前18行：请求行已完整返回时，应区分窗口完成与文件 EOF，不能无条件报 output_truncated。
4. 自建最小回归覆盖即时子项列举、third_party/vendor/build 默认排除及显式 include_ignored、缓存重放的工作量零和未搜索的区别。
5. 读取自有大文件前5行、长行片段/扫描中断和游标续读，核对输出截断、更多内容和请求窗口完成各自含义。

已有普通核心缓存：`MaiAgent/.cache/build-file-batch2-20261121`。新增日志统一 `batch7*`，不覆盖历史验收回执。
本节为动生产代码前写下的可复现操作与预期；复现结果、根因及后续验证追加在后文。

## 实际仓库修改前观察

使用当时正式工具、独立实际 Core Release 产物运行 `MaiToolTests --batch7-repo-probe E:/OpenSource/MaiChat`。
仅记录 `[file_result]` 元数据、工作量和耗时；没有把源文件正文或匹配行写入探针日志。

- MaiAgent 的 `pattern=*`：5012ms，count=0，visited_entries=0，partial=true，唯一回报为 time_budget_ranking。
- 整仓 literal grep `MaiFileSearch`：5002ms，count=0，searched_files=0，read_bytes=0，visited_entries=14683，time_budget。
- 实际头文件前40行、源码前18行：返回数量与请求相等，last_line_complete=true，eof=false，均读取4096B，
  却 complete=false、工具/执行 output_truncated=true。

日志：独立缓存 `batch7-repository-before.jsonl`；初始 owned 回归29条断言失败，记录在 `batch7-tool-red.log`。
默认排除的实际实现本来已有字面目录 build，缺少 vendor/third_party 及 build-*、cmake-build-* 等变体。
实际 .gitignore 不排除所有第三方源码；根规则读取范围仍按既有搜索根及子目录，不读取搜索根以外父级规则。

## 根因与实现

1. `*` 无斜杠的既有含义是递归匹配 basename，先建立全树索引再匹配；收集耗尽预算后，排名返回时丢掉了已采集的工作量。
   现在保留真实枚举 observations；没有把 * 静默改为非递归。
2. 新增 `recursive:false` 浅层模式，普通 `MaiFileNameSearchOptions` 与正式 file_glob 共用核心。
   只枚举直接子项，不进入子目录；返回普通文件和目录及 isDirectory/type，count_unit=entries。
   递归模式仍只返回文件、count_unit=files。total_matching_files 始终只计普通文件。
   浅层目录同样绑定原生身份，分页重放后目录变化会拒绝旧游标。
3. 默认递归范围剪枝 third_party/third-party/vendor/vendors、常见构建变体；先作廉价名称过滤，避免逐个解析第三方文件身份。
   实际整仓还含 Qt 上游子仓库，不能硬编码 Qt 名称：默认过滤嵌套 Git 仓库/子模块（只检查 .git 标记是否存在），
   检查发生在真实路径边界及宿主授权之后。搜索根本身不被其名字/仓库身份排除。
   `include_ignored:true` 关闭默认排除和 gitignore；也可显式指定子仓库为搜索根。
   浅层模式保留这些目录的入口，仍遵守显式 exclude/gitignore/hidden 设置。
4. 默认过滤范围明确写入工具说明及有界元数据 default_excludes、excluded_nested_repositories、
   excluded_nested_repository_paths；路径最多8条、各256 UTF-8字节，并报告省略数。
   这些是查询范围，不作为读取失败，不置 partial；分页及缓存保留同一快照的范围统计。
5. 内容搜索状态 content_search_started/not_searched 独立于本次 I/O 工作量，随查询/缓存页保存。
   缓存重放 searched_files=0 但已搜索过时 not_searched=false；未进入内容搜索不说“没有匹配”。
   count 继续表示本页行数，不以0冒充整库零命中；精确总数仍遵循既有可空总数/下界约定。
6. file_read 的 complete/requested_range_complete 表示请求窗口完成；eof 独立表示观察到真实文件末尾，has_more 与之分开。
   只有输出/扫描/时间预算或未完整的行造成请求未完成时才标 output_truncated；完整请求窗口的 next_cursor 是可选读更多，
   不再给“Output truncated”或强制恢复提示。普通 reader 的 I/O、解码、扫描预算、原始/UTF-8游标及身份验证未变。

## 同一步骤修改后实测

本机同一正式探针、同一仓库路径，最终元数据在 `batch7-repository-final.jsonl`：

| 操作 | 修改后观察 |
| --- | --- |
| MaiAgent `pattern=*`（保留递归默认） | 71ms，页186条，冻结名单总数568且精确，visited609，无 issues；有下一页而非覆盖不完整 |
| MaiAgent `pattern=* recursive:false` | 2ms，15条直接子项（4文件、11目录），complete=true，visited19 |
| 整仓 literal grep `MaiFileSearch limit=1` | 466ms，页1条，searched17，visited3894，not_searched=false，partial=false，无 issues |
| 实际源码前40/18行 | 请求窗口完成，eof=false/has_more=true，工具与执行截断标记false，各实际读取4096B |
| 自有209MiB普通文件前5行 | total_bytes=219152384，实际读取4096B，完整5行，请求完成，eof=false，截断false |

整仓默认范围元数据明确列出本次排除的5个嵌套仓库：reference/codex、reference/openclaw、
MaiChat/desktop/qt/qtbase、MaiChat/desktop/qt/qtimageformats、MaiChat/desktop/qt/qtsvg。
它们不是整个磁盘都无命中的隐式例外，用户可 include_ignored 或单独 root 检索。
耗时为本机当次观测，前后范围按新默认过滤约定变化，不承诺其它磁盘/权限/机器同等耗时。

## 验证与边界

- MaiToolTests、MaiFileSearchTests、MaiTextFileReaderTests、MaiFileToolSchemaTests 全部通过。
  覆盖 shallow 类型/不下钻、目录身份变化、默认排除与 opt-in/显式子根、预算前置工作量、未搜索阶段和缓存重放、
  40/18/5行窗口、超长行真实截断、209MiB读取规模，以及既有UTF-8/UTF-16/旧编码/大偏移/游标用例。
- Qwen 正式 grammar/parser 验证递归布尔参数；没有真实模型、供应商费用或网络调用。
- Core/Store/InputQueue/SessionRename/FilePath/ContextCompactor 通过；本轮同缓存大门中 TurnRunner 的失败来自并行新增中央模型投影回归，
  实现由主任务负责，不修改 Runner/ToolOutput 或其测试。完整任务总体验收由主任务汇总。
- 主共享头已追加 recursive、条目目录类型、内容阶段及范围统计，需对实际消费者全量重编；独立缓存已重编相关目标。
- Windows 普通核心/正式工具及实际仓库已运行；macOS/Linux/iOS/Android 未运行，不用本机结果代替其它平台实机验收。

本任务未停止用户 App、未访问 App DB/用户日志；未新建分支或提交；其它并行 WIP 保留。
