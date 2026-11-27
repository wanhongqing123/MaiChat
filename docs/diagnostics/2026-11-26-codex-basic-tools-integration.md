# Codex 基础工具与文件操作对齐验收

## 需求和复现

用户要求基础工具直接对齐 Codex，重点解决文件操作经常失败；不另造多文件事务、不增加命令行依赖。本轮参考固定为本机 `E:/OpenSource/codex` 的 `rust-v0.172.0`，提交 `3d86e43b075adeba2193ec5c22ac92f55afd82e6`。只读 `git show` 获取该版本，不使用其工作区未提交修改。

可复现操作及预期：

1. 用 `exec_command` 在普通临时目录读取、搜索文件；默认 Windows PowerShell 在进程尚未退出时输出中文，应直接得到 UTF-8 文本。
2. 用正式的 `apply_patch` 修改含重复上下文的文件、追加文件末尾、匹配 EOF、处理 Unicode 标点和不同换行；匹配位置和文件结果与固定上游相同。
3. 一份补丁按顺序修改多个文件，后续路径失败时，如实列出前面已成功修改的文件。上游不提供整批回滚，本实现也不承诺整批回滚。
4. OpenAI 原生自由文本工具声明、流式输入、完整成功后的执行及结果回传均能配对；兼容供应商使用相同核心的 `patch` 字符串函数入口。不完整、冲突、取消和未声明调用不能执行。
5. 冻结工具快照在 SQLite/JSONL 重开后保持原生输入格式；原 v1 快照保留，显式追加 v2 工具配置，而不是改写旧会话原文。

## 定位和修复

- 工具注册为 `file_patch`，而用户和系统指令使用 `apply_patch`；同时旧指令要求优先使用全部 `file_*` 工具，与实际小型默认工具目录不一致。现在正式入口为 `apply_patch`，旧名称仅用于历史显示，不注册活动别名。读取、搜索、进程交互优先走当前系统 Shell；不假设包内存在 rg、sed、Python 或同名 apply_patch 可执行文件。
- 旧补丁逻辑忽略 `@@ context`，可能修改重复片段的第一处；纯新增块写在开头；EOF 匹配允许退到中间。按固定上游修正定位、搜索次序、尾部追加、覆盖和移动行为，并保留未修改上下文的原始字节及换行。普通调用和模型工具适配共用核心。
- 默认 Windows PowerShell 的运行中输出仍可能是 CP936；旧完成态转换不能修正已发出的中间输出。按上游为两种 PowerShell 启动模式添加 UTF-8 输出前缀；CMD/Bash 不套用该前缀。
- Responses 接入原生 custom/Lark 工具格式，完整缓冲并校验输入。原生调用/输出、无活动 tools 的历史回放及丢弃 continuation 后的回放均保持调用配对。工具目录、估算和指纹同步包含 grammar；估算锚点版本升至 3。
- 附加文件能力修复末尾独立 CR 丢失、ignore 读取未计入总预算、显式 UTF-16 端序与 BOM 冲突被当成搜索无命中。细节见 `2026-11-26-basic-file-read-search.md`。

## 同源构建和验证

- 时间：2026-11-26；Windows x64，正常桌面 Release 和普通 Core/Qt 测试目标。
- 源码：`main` 基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加当前工作区。未提交、未推送、未发布。保留其他平台及其他任务的工作区修改。
- 编译目录：`.cache/r311/src`、`.cache/r311/build-msvc`。`final-normal-source-manifest.json` 的 160 件输入逐 SHA-256 核对原工作区和实际编译副本；本轮包含 `MaiToolSpec` ABI 变化，正常依赖消费者重新编译。
- 补丁专用：35 个实文件用例通过；直接使用固定 Rust 源码的 275 个差分用例全部匹配。原生 custom 协议的 180 项检查通过；非真实供应商调用。
- 最终正常 Core：41 套全部 exit 0，包含文件读写、补丁、搜索、Shell/PTY、Responses、JSONL、目录恢复、上下文与权限回归。
- 最终桌面：6 个目标全部 exit 0。控制台端到端 exit 0，新增直接使用正式 `apply_patch`、权限批准后核对真实文件字节及工具结果回传的流程。
- 桌面首次运行有一处测试在 Core 完成后、GUI 队列终态到达前立即断言；改为在同一最终条件上有界等待，完整重跑 Panel 通过，其他五个已通过目标保留。控制台首次硬终止用例仅等可见流式文本，未等 completed item 持久化确认；改为等待 SessionIdle 提示再终止，完整端到端通过。两处仅修改测试等待边界；原失败日志保留，没有扩大产品草稿持久化范围或放宽最终断言。
- 便携包正常 staging 校验 3 个程序、114 个 PE 导入及 Qt 插件。ZIP 由一个生产器生成，验证全部成员 CRC，再实际解压；stage、ZIP 副本及交付副本在隔离 profile、bare PATH 下 `--smoke` 均 exit 0。

证据目录为 `.cache/basic-tools-20261126/`：正常构建 `normal-build-fixed.log`、`package-stage.log`，核心 `normal-core/results.json`，桌面 `final-qt-results.json`，控制台 `normal-console/receipt.json`，源输入 `final-normal-source-manifest.json`，交付 `local-handoff.json`。首次测试失败分别保留在 `qt-first-results.json`、`console-first-fail.log`。

## 实际交付与边界

- 程序：`C:/Users/18034/Downloads/MaiChat-local-20261126-codex-basic-tools/maichat.exe`。
- ZIP：`C:/Users/18034/Downloads/MaiChat-Windows-local-20261126-codex-basic-tools.zip`，264475045 字节。
- ZIP SHA-256：`d16fff0ec23f1e755e7c44769883dfb66e0090c8762f8e05a81a650ef7f5fafa`。
- 三份 EXE SHA-256：`f503328ef15c670334c69f93dc22de44fd1bfbe66f2f31bb3811d5c4c424213d`。
- 包内 `local-source-manifest.json` 和 `LOCAL-EXPERIENCE.txt` 说明精确源码输入及体验边界。

核验时用户仍在运行旧目录 `Downloads/MaiChat-local-20261126/maichat.exe`。本轮没有停止该进程、替换现装、安装或访问用户真实会话数据库。需用户退出旧版再启动新目录程序后才能体验新实现。

本轮仅验证 Windows 正常构建和隔离自有数据，未声称真实供应商、用户账号迁移或 Mac/Windows 双机验收。OpenAI 后台 Key 部署仍待可用部署入口，不因本包宣称上线。不支持 Codex 远程 `environment_id` 路由，遇到时在文件副作用前明确拒绝。普通 file 扩展仍可发现使用；这是附加能力，不要求模型绕开系统 Shell。
