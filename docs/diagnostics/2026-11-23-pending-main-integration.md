# 积压开发改动的主线整合

2026-11-23，用户明确要求将本地积压修改提交并推送到远端。此次更新源码，不重新发布安装包或移动已有发布标签。

## 输入与备份

- 原主工作区 `main` 为 `38d25b9d7a41ad29128e95289f44937c676ca452`，包含三个尚未进入远端的本地提交，涉及轮次终态、原始要求保护、上下文状态显示。
- 远端主线为 `1c5ad48f6cbb8eceb2050a918c430727fa140c8b`，包含公开 3.2.0 产品 `564dcb1f4a5f82bf1ca1e467d717b76ddd23aa42` 及交付文档。
- 原工作区有 79 项跟踪变动、79 个未跟踪文件（短列表中目录折叠为 52 项）；连同 Qt 两项源码变动共备份 159 项。私有 ZIP 为 1,910,620 字节，SHA256 `e49e3ca1c22756ce777a8c2fca6d7f3d0f50e4e9c2ae8d434f8d813b3574761f`，位于 `.cache/pending-integration-20261123/`，未提交备份或用户运行数据。
- 隔离主线副本使用原 CMAKE_HOME、SDK 和依赖正常构建，避免先改写主工作区。最终合并提交保留真实远端和原本地 HEAD 为父提交，不把临时输入快照作为发布历史。

## 功能范围

文件 move/copy/stat/delete-directory、精确搜索与文本统计；curl 的重定向、请求体文件、multipart 和 Cookie；Shell 会话、stdin/增量读取、停止及解释器环境适配；旧进程四件套退役；浏览器遮挡观察；存储默认汇总与分页；压缩存档读取；截图运行态诊断；统一工具输出预算与补读，以及相关测试、诊断、打包许可声明。

已发布的 OCR、工具分类、请求追加、渐进压缩、跨设备队列/图片及 413 修复均保留。Codex Shell 目录只有现有移植代码的 Apache-2.0 LICENSE/NOTICE，未接入新依赖库或 MCP。

## 合并与兼容修复

一次三方整合出现 18 个文件冲突，逐模块保留两边行为。公开事件枚举的已有顺序不改，新本地事件追加；Runner 统一使用现有 MaiTurnContext 并保留重试、413、终态与上下文状态。

统一工具预算原先会在追加结果时重分旧结果，违反已发布追加合同。新执行片段使用明确版本，旧片段保持原文；最终待发表示按精确来源和型号持久固定，候选不写库，SQLite v15 独立保存并与消息/会话清理联动。未知配额用尽停止而不改旧结果；完整原文及可用补读仍保留。

原始用户要求层的预算现在写入检查点，读取相同检查点不会随新尾部重新裁剪；只有成功推进检查点时调整。

## Windows 验证

- 普通 `maichat` 完整依赖构建、部署、PE 检查通过。
- 20 套 Core：BuilderImage、Compactor、ToolLoop、TurnRunner、InputQueue、Runtime、Store、Tool、Shell、FilePath、FileSearch、TextFileReader、FileToolSchema、CurlEmbeddedCli、AppStorage、ToolOutputRead、CheckpointRead、CheckpointTool、ScreenshotAvailability、RequestBodyRecovery，最终结果均为退出 0。
- 固定投影专项覆盖 Memory/SQLite、四个大输出后追加第五个、后继轮次和 SQLite 重开、旧原文兼容、未知配额耗尽。Store 专项还覆盖并发首写获胜、NUL 字节、来源隔离、原始快照淘汰和清理。
- Qt 选定会话 17 passed / 0 failed，远端分类 14/0，远端图片 MIME 7/0，目录测试退出 0；真实自有浏览器窗口遮挡后 visibility=hidden，DOM/type/PNG 流程退出 0。
- 初轮独立测试工程缺正常闭包/include，以及过长临时路径触发 Windows 路径边界；修正工程和自有短 TEMP 后通过。Store 新测试错误假定片段顺序导致 bad_variant_access，改为按稳定 ID 核对后完整通过；容量错误测试改用正式终态 API，避免隐藏草稿被可见聊天列表遗漏。保留原失败记录，没有放宽功能断言。
- Qt SDK 工作树的两项修改与已跟踪 `windows-packet-surrogate-order.patch` 逐字相同，SHA256 `a66f64b74b19c5b10e2a0f6892451bd8617d05be18429c401174d8ecb97e67de`。不新增或推送 Qt 上游提交。

开发 ZIP 的 staging 与实际解压裸 PATH 隔离启动回执随本地验证目录保存；它是未提交整合候选验收件，不冒充公开 3.2.0 包。未安装或停止用户程序、未修改系统剪贴板、未读取用户会话 DB、未调用真实模型/SSH。

Mac/POSIX 原生 Shell、macOS 截图/TCC、移动端 UI 和此前 Windows 浏览器最小化截图超时不在本次实机验收结论内。未运行全体平台或全部 UI 套件，不把定点通过描述为全平台全绿。
