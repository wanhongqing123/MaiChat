# 对话区上下文压缩提示

日期：2026-11-21，Windows x64；基线 `03e4f5506091be28b6b14f698a1c6ad34f1a2e27`。用户明确要求压缩时出现类似 Codex 的提示。

## 操作与预期

在生产 AgentChatPanel 中用本地固定模型生成较长历史，再正常点击发送触发真实 Core 压缩。压缩期间对话区应出现明确提示；完成后更新并收起；切到其他会话应隐藏，切回活动压缩应恢复；普通请求及检查点复用不应冒充新压缩。

本地 Codex `tui/src/chatwidget/compaction.rs` 以实际开始/完成事件显示 `Compacting context` / `Context compacted`。本批借鉴生命周期，由既有 Core ContextStatus 驱动，不猜测进度或调用新模型。

## 实现

- 在对话区底部、输入框上方创建一个提示控件，使用现有字体缩放与轻提示样式。非阻塞，不抢焦点，不重建消息页。
- 实际 Summarizing 时显示“正在压缩上下文，请稍候…”；成功就地更新为“上下文已压缩，原始记录仍可回查”，4 秒后收起。
- 失败、中断、延后、仅来源索引恢复分别展示准确文案，不将索引恢复称为完整压缩成功。
- Preparing、复用检查点与冷历史不会创建新的完成通知。只有该面板观察到实际压缩开始才宣布结果。
- 切会话、新轮次或再次压缩停止旧计时；timer 与控件由面板持有，提示不进入 ConversationState 页面缓存、数据库或模型上下文。

## 实际验证

- 原界面对应用例实际 RED 5：找不到独立压缩提示控件。
- 正常 CMake/Ninja 构建 `maichat` 与 `agent_panel_session_test` exit 0，部署/PE 门禁成功（3 binary、114 导入及 Qt 插件）。无旧手工链接包改名。
- 经 `run-tests-windows.ps1` 运行生产组件夹具：11 passed / 0 failed，8.130 秒。包括正常、Network、异常、来源索引、终态通知缺失，会话切换、空白页、跨会话正文/错误及标题隔离。
- 正常分支实际等待提示自动消失，再发送第三轮，明确检查 checkpoint reused、未推进新检查点、没有重复压缩提示。
- 仅使用自造历史与进程内固定模型；截图仅抓取该组件，已人工查看开始、完成和失败画面。没有读取屏幕其他窗口或私人聊天。
- 只读独立审查未发现 P1/P2。原有非致命 QFont point-size warning 保留，不冒称全字体环境已验证。
- 日志：`.cache/compaction-notice-red*.log`、`.cache/compaction-notice-green*.log`、`.cache/compaction-notice-green.txt`；组件截图：`.cache/compaction-notice-evidence/`。
- 候选 App SHA256：`2c950ff180cbe95c4504522a54040351a34011e2a6e16029b8fd7580be8809a1`。

## 边界

本批仅桌面展示，不改变共享 Core、压缩算法、供应商、费用或工具权限。macOS 共享此 Qt 源码但本轮未构建/运行；iOS/Android 原生界面未接入。未替换现装程序，未发布 Release，无新依赖/MCP、真实模型请求或用户 DB 操作。既有 qtbase WIP 与三份未提交源码对照文档保持原状。
