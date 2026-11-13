# 原生文件搜索与文本读取修复

时间：2026-11-13，Windows 11 x64。开发基线 `583a07bc48c53759a089ae8097184730eedf2a94`。
本记录的性能样本均为自建文件和本机单次测量，不是用户目录或与上游产品的速度排名。

## 现场要求与失败复现

用户反馈工具难用、每次调用等待过久，要求先读清 Codex/OpenClaw 源码，再落地完整修复。
预期操作是：按文件名定位、在指定文件或目录检索正文、读取命中位置；结果可续页、可取消，
不能把扫描/展示上限当成文件结束或全局无命中。

使用普通 `MaiToolTests` 入口和自有工作区，在旧 `MaiFileTools.cpp`
blob `d0d2c1779a04209e3030c4bd5929726bae734273` 上实际编译成功、测试失败：

1. 第 480 字节后的词未命中：旧代码先把行裁到 400 字节，再运行正则。
2. 第二行在 8MiB 后：旧读取先截取文件前缀，随后应用行偏移，误报该行不存在。
3. 260 条命中只给前 200 条，没有续页；额外 `offset` 被静默忽略。
4. 文件名定位必须猜 glob，无法直接用名称模糊查找。

四个回归产生九条失败断言，原有功能组仍执行。原始日志及输入 SHA 在
`.cache/search-tool-review-20261113/native-tools-red*`。

## 固定上游源码核对

参考仓库只读核对，没有运行其项目、安装依赖或把它们加入产品提交。

- [Codex TUI 文件搜索管理器](https://github.com/openai/codex/blob/553c764e8b99fa519e9c6eb9b657875ef0f5e1c1/codex-rs/tui/src/file_search.rs#L42)
  为同一非空 `@` 查询保留 session，更新 matcher，不为每次输入重新遍历。
- [Codex file-search](https://github.com/openai/codex/blob/553c764e8b99fa519e9c6eb9b657875ef0f5e1c1/codex-rs/file-search/src/lib.rs#L176)
  使用目录 walker、Unicode 路径列和模糊匹配。该 session 没有文件 watcher；
  [one-shot run](https://github.com/openai/codex/blob/553c764e8b99fa519e9c6eb9b657875ef0f5e1c1/codex-rs/file-search/src/lib.rs#L309)
  仍会新建遍历。不能泛称所有 Codex 搜索都会自动实时缓存。
- Codex 的这条路径是文件名定位；默认内容搜索主要依赖 exec/rg，不应误当完整原生全文工具。
- [OpenClaw read-file-page](https://github.com/openclaw/openclaw/blob/a83f74375ccaa48c3742d8dda216d603fc59cc49/src/agents/sessions/tools/read-file-page.ts#L27)
  每次调用复用一个句柄及 64KiB 窗口，实际 read 返回零才判 EOF。
- [OpenClaw 文本捕获](https://github.com/openclaw/openclaw/blob/a83f74375ccaa48c3742d8dda216d603fc59cc49/src/agents/sessions/tools/read-text-capture.ts#L48)
  按换行片段处理，预留续页信息。它每次请求仍从位置零开始；MaiChat 保留了跨调用原生字节游标，
  不把长文件续读退回从头扫描。

## 修复结构与边界

普通 `MaiFileSearch` 和 `MaiTextFileReader` 提供能力，既有三个工具只适配参数和输出。
没有增加 MCP、Skills、第二套工具名或 shell 搜索依赖。

- 文件名支持 glob/模糊名称、稳定排序、Unicode 标量匹配、include/exclude/隐藏项/忽略规则控制。
- 同根、调用范围和过滤条件在五秒内复用冻结路径索引；显式 `refresh`、invalidate 或 TTL 到期重建。
  游标保留冻结快照至空闲 120 秒，最多八个查询/64MiB；没有 watcher，也不声称实时全盘数据。
- 内容先匹配完整行再生成聚焦预览，支持字面串、受限非回溯正则、上下文、文件列表及计数。
  不支持的正则语法明确报错；保留精确文件查询及原有编码/权限/扫描预算。
- 新查询和续页均校验 scope、真实路径与返回源文件身份；续页不重新枚举根目录。
  覆盖不足、跳过文件、更多页与展示裁剪分别声明，不从部分空结果断言不存在。
- 文本读取每页只打开一次，流式定位行号；超长行按完整 UTF-8 字符续读，源文件或会话变化拒绝旧游标。
  EOF 依据实际读取，不能由 stat 大小零推出空文件。无稳定元数据的非空虚拟文件若需要跨调用续读，明确不支持。
- 等待搜索互斥量也检查取消和期限；单次系统 I/O/宿主回调仍是协作式边界，不宣称可以强行抢占。
- 可读结果后附 `[file_result]` 元数据；只返回确实保留的游标，不把 shell 的 output_id 当作未扫描尾部。

## 已观察性能与验证进度

隔离 MSVC `/O2` 自建 10002 个文件：

| 操作 | 初始原型 | 修正后样本 |
|---|---:|---:|
| 冷文件名查询 | 585ms | 661ms |
| 修改查询词 | 494ms，仍遍历 10002 项 | 9.2ms，枚举零项 |
| 下一页/重放 | 501/489ms，仍遍历 10002 项 | 0.13/0.11ms，枚举零项 |
| 16MiB 前缀后读取第二行 | 482ms | 约 12ms，一次打开 |

原型中文错误命中“你”→“一漽倠.txt”已用自有文件复现并修正。
新版页重放的 read_bytes/searched_files/visited_entries 是本次实际工作量，不复用历史统计冒充新 I/O。
旧原型快照、固定源码 SHA、测试命令和原始 NDJSON 均保留在忽略的诊断目录。

普通核心已实际编译新 API 和工具适配器，最终 11 个目标全部通过：原有核心、路径、存储、模型客户端、
Runner、工具、工具循环、权限和执行事实，以及新增 reader/search；并行回归总计 92.26 秒。
深路径上下文重复放大输出、空花括号兼容、精确文件大小和不存在文件反馈分别先失败再修正，原日志保留。
最终源 SHA/目标列表在 `.cache/search-tool-review-20261113/core-final-receipt.json`，原始测试为 `core-final-tests.log`。

Android 正常 ARM64 Release 已成功构建，实际编入新 helper 对象和 factory，包内 26 个库、七个 CPU 后端及
原完整模型/许可已核对；这只是候选构建，不等于已安装或功能实机验收。
Windows 性能测量不能代替手机硬件性能；POSIX 尚需 Mac 普通消费者核准。
没有读取真实聊天库、剪贴板或触发付费模型任务。
