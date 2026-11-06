# 远端主分支同步与播放器重构保留

2026-11-06。用户指出截图中的旧播放器与远端重构版本不符。复现/预期：先确认当前
源码提交与 origin/main，再保留全部本地工作同步远端；不能以旧窗口样式验证新版本。

起点 main/eee43402，已拉取 origin/main/9459e7ec，快进 17 个提交。更新包含共享无边框
播放器、浮动 HUD、原生 DPI 图标、倍速/音量菜单、提问路由生命周期、Android 模型选择
与持久 outbox，以及流式文本/工具/媒体排序。282 个本地修改及新文件先保存到私有
文件备份和 Git stash，再恢复、逐处合并。检查无冲突标记、无丢失的旧文件，
`git diff --check` 通过，未新建分支、未提交或推送。

播放器使用上游 MaiVideoHud、MaiVideoSpeedSelector、MaiVideoVolumePopup 与新标题栏，
保留其菜单、图标及窗口行为。Mac HDR 系统播放后端适配新音量/静音入口、状态回显和
首帧默认长宽比；GUI 与 Agent 命令共用当前实际后端，不能读到旧 FFplay 音频快照。
新控件的文字采用全局 detail/metadata 角色，标准/大/特大都来自同一正文偏好；
倍速控件按内容留出宽度，避免 0.25× 在大字号下被固定 44px 裁切。图形图标尺寸沿用
上游外观。三端代码继续采用共享 Sarasa Fixed SC，正文仍使用用户选择的家族；
iOS 行内代码仅应用一次字号比例，避免合并时按当前字号再乘偏好。

当前正常产品构建包含无边框样式、三档文字大小、菜单位置/原生命中区域、音量键盘控制、
HLG/PQ/现场 Dolby Vision 原片的播放、暂停、倍速、seek 和结束重播回归。
菜单测试先出现 150ms 单次取样时 native popup 可见但 WindowServer 尚未注册命中区域，
改为有界观察真实命中就绪后才操作，不放宽位置或命中要求。最终定点 8 项通过，
记录 `final-player-fonts-and-controls.txt`。窗口的 native active/active Space 已观测，
仍不能把逻辑全屏状态或 SDR 截图当作物理 HDR 屏幕瞬时亮度验收。

最终 Mac 2.2.53 包完成 deep/strict 签名及独立目录、纯系统 PATH 的 --smoke，
未覆盖正在运行的 2.2.34。iOS 201 签名包已准备，真机 unavailable 未安装。
非阻塞 Session 输入的端到端结果、iOS 长历史扩展回归失败与 Windows 验证边界见
`maichat-remote-session-input-queue-20261106.md`。完整私有构建/录屏/截图和源快照在
`/private/tmp/maichat-remote-input-queue-20261106/`，原片与私有图像未入库。

补充接收端排序验收：Mac 的异步附件准备与后续纯文字现在共用会话 FIFO；Stop 不被
文件工作阻挡，登录更换和 Controller 销毁后不消费旧输入或回传旧账号结果。先前
最小失败用例的 2 项失败转为通过，最终同步整组 38 项通过（29.818 秒）。
最新包是 `/private/tmp/maichat-macos-2253-delivery-20261106/MaiChat-macos-arm64/MaiChat.app`，
重新完成签名及纯系统 PATH 的 --smoke；包内已核对新接收队列和新播放器文字样式。
