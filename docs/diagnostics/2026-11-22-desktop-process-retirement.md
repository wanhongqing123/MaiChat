# 第十一批：进程工具收口与运行版本边界

## 操作与预期

用户要求在 shell/session 替代路径可用后删除 desktop_process_start/read/write/stop。
这是第六批同一决策的再次确认，不创建第二套进程管理器，也不先删掉仍需的交互能力。
本次在 Root main `38d25b9d` 的既有工作区检查实现，保留原三项已提交功能、Qt 与文档 WIP。

核对内置工具注册与 StarterCatalog：正式执行入口应只有 `shell` 与 `shell_input`。
短命令直接结束；长命令让出后返回 session_id；后继轮次可补读、写 stdin、显式终止。
旧工具实现、注册、菜单和测试构建条目都应移除。移动界面文案保持。

## 源码与验证

第六批工作区已包含 MaiShellSessionManager、两个工具及 owner 清理；此次没有重复实现。
DesktopProcessTools.cpp/.h、DesktopProcessToolsTest.cpp 在工作树中已删除；
AgentController.cpp、StarterCatalog.cpp 和 desktop/CMakeLists.txt 没有旧执行器注册/源条目。
工具组对旧名字的识别仅供持久化历史显示，不注册兼容执行别名。

最新同源 MaiShellToolTests 47 项全部通过；跨 Agent 轮次的真实 stdin/授权路径与 QR 对照
合计 QtTest 6/0；StarterCatalog 129 个检查、0 失败，核对正式两个工具及旧名字不再进入目录。
日志位于 `.cache/post312-validation/{shell-final.log,shell-ui.log,catalog-launcher.log}`。
这些用例只运行自有命令/子进程，不调用供应商、不改现装、不执行原用户 PID 73253。

解释器选择、login 与 Shell 原生状态规则沿已核 Codex 合同；不默认启用 pipefail，
保留原 command 参数。PTY 仍按用户明确决定不做。Native Mac/POSIX shell 会话在本轮
没有重新实测，不把 Windows 的 Git Bash/PowerShell/cmd 证据代替原生 Mac。

## 为什么用户仍能看到旧四件套

公共 3.1.2 的冻结源码是 `f47b912c`，它不含 Root 未合入的 Shell/session 工作区。
所以该版本及仍运行的旧 App 中缺少 shell_input、仍有 desktop_process_* 的观察成立。
“源码改造已完成”不能解释为“现装工具清单已经更新”。当前两个工具与删除项仍待独立源码
聚合和后继版本交付，不能只在公共旧版本删掉四个入口造成能力空白，也不能移动 3.1.2 tag。
