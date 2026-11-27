# 移动端原生文件工具与进程内命令环境

日期：2026-11-26。工作区基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，包含既有未提交修改。

## 用户需求与复现操作

桌面默认用 exec_command/write_stdin 和 apply_patch，移动端没有外部 Shell，仍必须保留完整
file_glob/file_read/file_grep/file_edit 及创建、复制、移动、删除等原生工具。
用户进一步明确需要命令行式的进程内脚本环境，能执行 ls/cat/mv 等命令，并覆盖系统授权的
相册、文档。现有 curl/FFmpeg 为各自改造的进程内入口，不是已有 Shell 仿真环境。

基本操作：在 App 工作区执行 `NAME='file a.txt'; echo hello > "$NAME"; cat "$NAME" | grep -F hello`，
应返回真实输出与状态；变量和目录只作用于本次调用，App 进程工作目录/环境不变。
脚本错误、取消和越界不能被当成执行成功，管道不能消费被静默裁剪的中间值。

## 实现分层

- 原生 file_* 工具保留在 MaiAgent 核心，iOS/Android 共用同一份实现，不包装系统 Shell。
- Desktop builtin 注册选择不暴露 file_*，保留 apply_patch。目录版本更新为 v5，旧 v1..v4
  记录不修改；新快照不能继续钉住已移除工具。普通库/CLI 默认工具注册保持完整。
- MaiCommandScript 只解析和调度，通过中立回调访问命令和文件；不直接操作磁盘、启动进程或退出线程。
- MaiFileCommands 提供共享原生文件命令，复用路径、文件系统、搜索和追加组件。
- 移动 exec_command 是薄工具，使用会话工作区和 App 容器边界，不返回进程 ID/PTY/session_id，
  不注册 write_stdin。curl/ffmpeg/ffprobe 暂时仍是各自独立工具，不宣称已接入命令解释器。

这不是完整 Bash 或 POSIX Shell，也不仿真整套移动操作系统。首批语法为引号、变量与赋值、
换行/分号、&&/||、有界顺序管道和 <、>、>>。未实现的后台、子 Shell、命令替换、控制流、
here-doc、通配符展开和 fd 重定向明确拒绝。多命令脚本不是事务，已成功操作不自动回滚。

## 已证实的编辑问题

共享 file_edit 原实现存在：replace_all 错类型可抛异常；未知字段及 NUL 路径未拒绝；
取消后仍可能继续写；替换扩张没有最终大小限制；大 new_string 被全部回发；摘要切断 UTF-8；
二进制/UTF-16 可能被按字节误编辑。

定点修复保持唯一匹配/显式全替换，预计算最终 8MiB 上限，检查取消，摘要片段限制 480 字节并
保持 UTF-8 边界。只编辑无 NUL 的 UTF-8（保留 BOM），不静默转码。UTF-16/旧编码仍可通过
file_read 读取，但不能据此宣称 file_edit 已支持原编码写回。

独立真实文件回归 RED 27 处失败 → GREEN exit 0，记录在
`.cache/playwright-skill-20261126/edit-hardening/{red,green-final,source-receipt.json}`。

Desktop 注册定点测试先记录 RED：Controller 仍发现 file_read，Panel 请求仍含 file_read；
原始结果在 `.cache/platform-file-tools-20261126/profile-red`。不能删除移动实现来满足桌面断言。

## 授权资源边界

App 容器内普通路径可走原生文件 API。iOS 相册资产/安全作用域文档、Android MediaStore/SAF
资源可能不是 POSIX 文件路径，必须由平台保留授权令牌与生命周期，再经同一命令宿主抽象访问。
不能把导入缓存冒充原目录原地编辑，也不能把单项照片读取授权扩大成整目录写入/删除。
当前命令原生文件宿主的 accessRoot 只表达普通目录边界，不能据此宣称相册或 SAF 挂载已完成。

平台约束依据：[Apple 文件和目录](https://developer.apple.com/documentation/technologyoverviews/files-and-directories)、
[Android 应用专用文件](https://developer.android.com/training/data-storage/app-specific)。
这些资料支持文件访问受 App 权限/容器约束，不单独构成产品已接通授权资源的验收证据。

用户明确限定删除只在 App。当前命令宿主 App 根不会因照片或文档读取授权扩大，rm/mv/重定向
也不能取得原件删除或覆盖权限。既有 Android MediaStore 失败清理仅回滚本次新建副本，
不是删除其他原件的入口，不申请原件删除权限；本轮没有修改这套事务回滚。

## 最终验证与实际边界

- 正常 Windows App build 0，3 个二进制/116 个 PE 导入检查通过。
- 9 个 Core 目标最终全 0：CommandScript、FileCommand、InProcessShellTool、Tool、EditTool、
  FileSearch、TextFileReader、ModelToolCatalog、ToolDiscoveryRuntime。
- Controller 76/0、Panel 138/0、Starter 全 0；欢迎页文案修正后重跑欢迎页及默认工具声明定点，
  核对新截图与实际工具一致。界面使用原来组件，没有新增工具选择流程。
- FileSearch 首轮夹具失败 4 处：过长临时根与 140 字符文件名叠加，旧 ofstream 静默创建失败；
  改用原生文件 API 并核对创建返回值后，同用例 0。产品搜索实现未改，原 FAIL 日志保留。
- Android NDK 28：ARM64/x86_64 API26 下 4 个共享实现及移动适配层均通过语法编译，
  不代表 APK 链接、安装或手机实机通过。iOS 未编译、未实机验证。
- 只读代码审查未发现新增 P1/P2。极小 stderr 预算缺口已由独立 RED→GREEN 修复，正常构建纳入。

证据根 `.cache/platform-file-tools-20261126`：`final-core-results.json`、`qt-final/results.json`、
`profile-green`、`welcome-corrected`、`android-syntax.json`、`android-mobile-adapter-final-syntax.json`。
204 个源输入的精确 manifest 与最终本地包回执位于同目录。未操作现装、用户数据、真实相册或文档。

本地 Windows 交付：`C:/Users/18034/Downloads/MaiChat-local-20261126-platform-file-tools/maichat.exe`。
ZIP 264,595,344 字节，SHA-256 `c852cf532b44c5597881c4b6b463fec153eef23c93181f1a320d1540f979d32c`；
staging/实际 ZIP 解压/Downloads 三份裸 PATH `--smoke` 均 0，EXE SHA-256 均为
`8fe7550405afc6a3f83cdae05a87acec8b2877455aea2580ff764d0547cc9421`。
这是桌面包，不是移动 App 包。当前现装进程 PID 27200 位于 `D:/Program Files/MaiChat/maichat.exe`，
本次未停止或替换；产品/文档均未提交、推送或发布。
