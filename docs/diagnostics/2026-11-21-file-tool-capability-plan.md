# 内置文件工具补全：复现、契约建议与验收计划

日期：2026-11-21。主机：Windows。本地 main：
`38d25b9d7a41ad29128e95289f44937c676ca452`。
本任务独立于暂停媒体提示词精简；当前仅核查和方案，未改生产代码或测试。

## 可复现操作与预期结果

验收素材全部在自有临时目录：7 个 `.cpp`、2 个其它文件；3 个普通文件含指定字面串，
各含 2、1、3 行命中，另有一个排除目录。不得读取用户文件或运行 shell 完成被测操作。

| 操作 | 预期 |
| --- | --- |
| `file_glob` 指定该目录、`**/*.cpp`、`limit:3` | 当前页 count=3，完整过滤后快照总数=7，精确；有下一页不代表总数未知 |
| 普通搜索 API 将 maxEntries 降到不足以遍历素材 | 总数未知，只报告已确认下界和原因，不以页面条数冒充总数 |
| grep 的 lines/files/count 三模式 | 分清命中行数、命中文件数、返回记录数；分页重放不重复累加 |
| 单次聚合搜索，输出额度只允许 1 条明细 | 完整扫描时仍报告完整总数；扫描受文件/字节/时间预算限制则报告未知及下界 |
| 取消、解码失败、拒绝访问、文件变化或缓存失效 | 不报告精确总数；返回现有错误/覆盖范围原因 |
| `file_grep` 的 output_mode=content | 错误只指明 output_mode 枚举错误；pattern 仍是唯一必填参数 |
| `file_move` 普通文件到不存在目标（同文件系统） | 一次完成重命名，目标内容不变，源消失；已有目标拒绝且不覆盖 |
| `file_copy` 普通文件到不存在目标 | 一次完成字节复制，源保留；空文件和非 ASCII 路径也通过 |
| move/copy 已有目标、同一路径、目录、链接、越界、磁盘失败 | 明确失败，不静默覆盖、跟随链接或销毁源；不回报虚假成功 |
| `file_delete_directory` 删除自有空目录 | 单独审批后成功；非空且未声明 recursive 时失败且不改内容 |
| 同工具 `recursive:true` 删除非空自有目录 | 不复用空目录审批；递归范围、链接和取消边界明确；中途失败如实报告已有变更 |
| `file_stat` 自有文件与目录 | 类型、大小适用性、Unix 修改时间及单位明确，缺失/无权不伪装为零 |

## 搜索总数建议

- 现有 count 保留为本页记录数。新增精确总数采用可空 total_matches 和
  total_matches_exact；未知时 total_matches=null，以 total_matches_lower_bound 报已证实下界。
- 统计单位必须明确：glob 为文件；grep files 为不同文件；grep lines/count 为匹配行，
  不统计同一行内的出现次数。另列 total_matching_files，回答符号在几个文件中出现。
- 精确性限定于本次过滤条件和冻结快照，不表示整个磁盘或查询后实时状态。
- glob 现有实现先收集匹配名单再分页，可以在完整快照上直接给精确总数。
  grep 现有实现按页继续扫描，不能将目录枚举完成误当内容搜索完成。
- 建议为 grep 增加显式单次聚合选项，扫描至现有预算耗尽或完成，明细输出分页不阻止
  聚合。普通分页路径不强制额外扫描全仓库；未扫完就保持未知，避免改变其首屏耗时。
- 排除规则是查询范围，不是错误；范围内无法读取/解码/变化等未覆盖内容影响精确性。
  Files 模式可在每文件首个命中后结束，不能因此推断全部命中行数。

## 写操作与底层 API 边界

- move/copy/delete-directory 均 requiresApproval=true；移动绑定源与目标，复制绑定
  读取源与写入目标，目录删除绑定规范化目标及 recursive 模式。缺参数不退化为宽泛授权。
- 默认不覆盖、不自动创建目标父目录；首版 move 明确只支持同文件系统普通文件，
  跨卷报不支持，不暗中变成复制后删除。目录移动/复制不伪装成普通文件操作。
- `publishNewFile` 契约仅支持同文件系统、不覆盖的新文件发布；Windows 用 MoveFileExW，
  POSIX 用 link/unlink；它不是完整通用 mv。工具必须先验证普通文件与路径访问权限。
- `removeRecursively` 返回 void，注释明确是测试清理且不报错误；不能直接包装为生产删除
  工具。应新增有错误契约的普通目录删除 API，再由工具复用。
- 目录默认只删空目录；recursive:true 才递归且与空目录删除分开授权。不跟随符号链接/
  junction，不允许删除文件系统根、会话工作根或其祖先；所有目标受宿主授权边界限制。
  审批后执行前重新核实路径与身份，不承诺递归删除可事务回滚。
- copy 新增 MaiFileSystem 普通入口，不用 std::filesystem/fstream。新文件发布前避免暴露
  不完整目标；取消/失败清理自身临时产物，原文件保持。
- stat 是只读，无写审批；目录大小不伪称递归总大小，也不额外遍历整目录。

## 验证计划与目前边界

用户当前要求先回答三个设计问题；生产实现待设计结论。之后按同素材先 RED 后 GREEN，
验证普通 API 与正式工具注册入口共用实现，以及实际主模型请求中工具 schema/审批行为。
Windows 可做普通 Core/工具/目标 App 验收；Mac/iOS/Android 未运行时必须明确列为未验证，
不能用共享 C++ 的 Windows 结果代替其它平台实际运行。

已静态核查 MaiFileSearch、MaiFileTools、Windows/POSIX MaiFileSystem 实现及 Chromium
file_util 接口说明；尚未执行本轮运行时用例。原 main、Qt 改动和其它未跟踪文档保持。

## 第三轮补充：file_read 规模与预读

### 已核查事实

- MaiTextFileReader::read 已在同一打开句柄上调用 snapshot()，结果携带
  MaiRegularFileInfo.bytes，并在返回前 validate；MaiTextFilePage.snapshot 已向工具返回。
  因此 total_bytes 可复用 page.snapshot.bytes，不需另调 fileSize，不新增打开或独立路径查询。
- 对普通磁盘文件，该值是文件原始字节大小，不是解码后的 UTF-8 长度，也不是本次 read_bytes。
  现有实现明确支持 size=0 但实际有内容的虚拟文件；对此不能输出精确零字节。
  建议 total_bytes=null、total_bytes_exact=false，另保留实际 read_bytes；空磁盘文件仍为精确0。
- 实际预读发生在 MaiTextFileBuffer::ensure：每次 readAt 固定请求 64KiB。
  max_bytes 控制工具渲染输出预算，工具没有把它赋给 maxScanBytes；reader 扫描预算仍默认32MiB。
  所以 41978B 小文件首读会整份进入64KiB缓冲，不是一次实际请求了1MiB。
- 现有测试 test_one_page_reuses_one_handle_and_reads_real_eof 明确断言 limit=1 时读取64KiB，
  是需要同步改为新目标的性能契约，而非产品必然依赖此预读量。

### 调整原则

减少缓冲预读与减少扫描预算是不同的修改。优先让小 limit 使用小块预读，按需追加，
达到目标行前仍允许高效跳过大前缀；不直接用 limit*估算行长切断原有扫描预算。
保留 max_bytes 输出上限、原始字节/行内UTF-8双偏移、字符边界、目标行和快照身份。
长行仍可输出连续片段并返回 next_cursor，不把预算耗尽冒充 eof。

自动编码探测目前使用首次缓冲前缀，调整该前缀可能改变旧编码判定结果。
必须专门验证 ASCII 前缀之后出现旧编码字节的情况；不能因性能修改静默更换已保存游标编码。
若需要改变探测策略，明确记录边界；显式 encoding 的语义保持。

### 新增可复现操作与预期

1. 自有约42KiB短行文件及16MiB文件，分别 limit=1/500。第一行相同；小limit实际read_bytes
   显著降低而非仅输出减少，仍只打开一次句柄；total_bytes与写入原始bytes精等。
2. 分别覆盖UTF-8中文/emoji、UTF-16、CRLF、无末尾换行、空文件、超长行和旧编码；
   反复沿游标拼接应与原文精等，无半字符、重复或遗漏。
3. offset跳过大前缀后取一行，与旧语义一致；小预读不导致无进展游标或无限循环。
4. 续读时改变limit/max_bytes，位置与原快照继续有效；改文件或改路径仍拒绝旧游标。
5. 真实EOF仍由底层读取确认，不以total_bytes推导；虚拟size-zero路径单独列平台边界。

统一建议顺序：先统计与常用文件操作，再读取规模/预读；精确报错随工具参数修改处理。
媒体提示词任务独立，用户先前要求的历史说明与决策边界不由本轮读取需求隐式替换。

## 实施与验证（2026-11-21，Windows，main 工作区）

以上为实施前形成的复现与契约计划；当前已按用户后续授权实现第2/3批。
源码基线仍为 `38d25b9d7a41ad29128e95289f44937c676ca452`，本轮未提交。
独立普通核心缓存为 `MaiAgent/.cache/build-file-batch2-20261121`，MSVC 19.44、Release、Ninja。
仅复用来源相同的现有 OpenSSL 中性 SDK；没有新增第三方库，没有运行旧桌面缓存 Ninja。

### 已实现

- glob 保留页 count，新增可空 total_matches、exact 标记、下界和单位；完整冻结名单即使有下一页也精确。
- grep 的 lines/count 总数单位为命中行，files 为不同文件；另报 total_matching_files。
  增量分页未完成为 null，缓存页重放不重复累加。summary_only=true 显式只聚合，仍受原预算限制并可续页。
  聚合结果独立显示统计句子，正命中不再被写成“没有匹配”。output_mode 错误只指明该字段。
- MaiFileSystem 普通 stat/moveFile/copyFile/deleteDirectory 与4个正式工具共用核心。
  move/copy 不覆盖、不创建目标父目录、拒绝目录/链接；move 不做跨卷复制删除。
  copy 私有暂存后发布；取消或失败清理自身暂存文件。stat 目录大小为 null，时间为 Unix 秒。
- 删除默认只删空目录；recursive 单独授权，拒绝会话/文件系统根和祖先、链接/junction。
  审批快照按调用/轮次隔离，执行前复核身份，轮次结束清理未消费快照。
  不淘汰仍待执行的审批后回退为当前身份；完全访问模式可独立执行。
  中途错误/取消返回真实删除数，非事务操作不承诺回滚。
- Windows UNC 共享根（含扩展 UNC）在任何解析/枚举前纯词法拒绝；POSIX 末段链接检查先移除尾斜杠。
- file_read total_bytes 复用同句柄 page.snapshot；size-zero 但读到内容时 null/false。
  首块固定4KiB，小页按需增长，大页/大offset后续恢复64KiB；扫描预算仍为32MiB。
  自动编码统一按4KiB窗口选择，ASCII前缀之后的旧编码歧义明确报错并要求显式 encoding；游标保留原编码。
- 移动适配层 `MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:767` 已调用中央 registerMaiFileTools，新增能力随之注册。

### RED / GREEN 证据

正式 MaiToolTests 旧实现17条失败、MaiTextFileReaderTests旧实现1条失败（limit=1仍读64KiB），随后全部转绿。
复核阶段另有：聚合错误文案2条RED、UNC根保护6条RED、审批身份容量回退及完全访问隔离RED，均用相同用例转绿。
测试均操作自建临时文件，没有用户数据、真实模型或付费供应商。

- 8套核心回归：MaiCoreTests、MaiStoreTests、MaiInputQueueTests、MaiTurnRunnerTests、
  MaiSessionRenameTests、MaiFilePathTests、MaiToolTests、MaiContextCompactorTests，全为 exit=0。
- 专项 MaiFileSearchTests、MaiTextFileReaderTests、MaiFileToolSchemaTests 均通过；Schema 使用正式 Qwen XML grammar/parser
  验证新增4个工具及 summary_only 参数，不加载模型。
- 普通 API 覆盖二进制复制、无覆盖、取消、空文件、非ASCII路径、身份变化、递归范围拒绝、部分删除精确计数、
  链接及尾斜杠不跟随；工具入口覆盖注册、单位/分页/预算/重放及源和目标审批范围。
- 41978B 素材：limit=1 实际读取4096B，limit=500读取41978B，total_bytes=41978，单句柄。
  16MiB前缀定位实测18.808ms、258次系统读取、1次打开，保留有效尾行。
- 实际 C:临时目录到 E:独立缓存：move 返回 NotSupported、源保留且目标不存在；copy 字节精等；自有文件和空目录已清理。

最初独立缓存的本地化 /showIncludes 前缀被 CMake 错解，导致旧搜索测试对象与新公开结构布局不一致。
已强制重编相关消费者；后续 Search/Reader/Tool/Path/Schema 运行均通过。该缓存问题不作为产品缺陷结论。

### 产物、日志与验收边界

构建、RED/GREEN 日志及源/产物 SHA256 回执位于独立缓存；八套逐项日志为 `<TestName>-final.log`，
另有 `tool-red.log`、`summary-red.log`、`policy-red.log`、`unc-red.log` 和 `file-capability-receipt.json`。
Windows 普通 API/正式工具入口和真实编译产物已运行；桌面实际 App 主模型清单/运行进程验收由主任务另记。
macOS/iOS/Android/Linux 未运行；WSL 未安装，POSIX 修改仅源码审查，虚拟 size-zero 文件 Linux 分支未在本机执行。
目录删除防护不构成针对恶意并发改名者的完整句柄沙箱；普通调用方仍负责父目录与宿主授权范围。
