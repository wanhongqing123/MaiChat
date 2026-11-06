# 远端会话工具组展示

## 复现与预期

- 日期：2026-11-07，基线 `ace8651ee6c072b9ad7e438093592f70e95b1a0b`。
- 用户截图中远端工具摘要是普通黑色正文：“工具调用 2 次 · shell · running/completed”，与本机工具组外观和状态用语不一致。
- 隔离复现场景：远端 assistant 正文 A → 连续两个工具 → 正文 B → 一个工具 → 正文 C；推送运行/完成状态、重新请求历史并加载较早消息。
- 预期：连续工具在调用原位置成组；使用本机相同的灰色 Metadata 折叠按钮、图标和中文聚合状态；正文、问答、媒体分隔工具组。更新与历史插入保留现有控件和展开状态。

## 根因与边界

- `RemoteAgentPanel::messageText` 将整条消息中的工具计数、最后工具和原始 state 拼成 Markdown 正文，并附加在全部正文之后。
- 本机 `AgentChatPanel::ToolGroupCard` 已有独立的 Metadata 折叠控件及中文状态聚合；远端没有复用该呈现。
- `DesktopSessionSyncHost::serializePart` 对普通工具只同步名称和状态。展开区域只能使用这些真实字段，不扩展到参数、输出、失败原因或授权操作。
- 保留 `questionDisplayTimeline` 的片段时序与独立问答投影；不更改 Core、同步服务或移动端协议。

## 验证

- Windows 10 / Qt 5.15.2 / MSVC 2019，构建目录 `MaiChat/desktop/.cache/build-release-62111144`；未停止或替换用户已安装的程序。
- 首次原生失败用例：预期两个独立工具组，旧实现实际为零。修复后验证 Metadata 字号和灰色、中文状态、折叠展开、正文间顺序、状态更新与分页保留同一控件及展开选择。
- 审查补充的三个用例在修复前均失败：追加同消息提问丢失首正文 ID；晚到子任务提问使后续正文 ID 嵌套并丢失选区；合并工具组后旧控件仍未立即隐藏。现统一使用原始 source message / part 身份，缺 ID 的历史数据保留独立片段序号，并在移除工具组时立即隐藏。
- 显式 `id: ""` 的旧快照另实测缺行（6 行，预期 7 行），已按缺 ID 使用序号兜底，避免多个正文段相互覆盖。
- `desktop_session_sync_host_test` 整组 53 PASS、0 FAIL、0 SKIP，覆盖上述场景及原有问答、同步、分页和普通输入路径。本机 Agent、Controller、MainWindow、UiZoom 四组 CTest 通过。
- 完整桌面构建通过；构建运行目录 PE 119 项依赖闭包通过。交付前另核对提交后的 staging / 实际 ZIP 解压目录、四个 Portrait 模型及裸 PATH smoke，记录放在以下私有验收目录。
- 私有证据目录 `.cache/remote-tool-style-20261107/`：`red.txt`、`append-red.txt`、`empty-id-red.txt`、`full-sync-green.txt`、`related-regression.txt`、`final-build.log` 及打包验证日志。
- 原生控件截图 `native-ui/remote-tool-groups-collapsed.png`、`remote-tool-groups-expanded.png`、`remote-tool-groups-updated.png` 来自隔离测试会话；未把测试记录写入用户会话。

## 交付边界

- 远端与本机复用 `AgentToolGroupWidget`；本机参数、输出和授权控件仍由本机 ToolCard 管理，远端展开区域只显示已有同步字段。
- 本次没有 Core、服务端和移动端修改。Windows 原生验证不代替 Mac 实机验收；Mac 同伴另行处理播放器动画。
- 公开 `v2.2.52` 仍指向 `d71652124c3ef6b50ed8cfcf9292579d1c1a771e`；本次源码提交和私有验收包不覆盖已发布资产，也未执行安装器。
