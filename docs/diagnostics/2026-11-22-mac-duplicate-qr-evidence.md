# Mac 同一二维码重复展示：只读现场证据

## 操作和预期

在创作中心当前 Mac 会话中请求二维码。一次生成的同一文件应只展示一份，工具调用记录保持完整。现场截图显示两张相同图片。此次只读核对，不执行 SSH、模型请求、二维码解码或用户程序重启。

## 运行产物

2026-11-22，macOS，运行 PID 438，`/Applications/MaiChat.app`，版本 3.1.1。可执行文件 SHA256 为 `f5144cc2e3f12e2dd72a9b70e3c94d1c719c400014263bc22a7a05c45a5a68cb`，与本机留存的公开 v3.1.1 发布 manifest 完全相等，对应源码 `af01f9ca2cf8fc7aa0a304f1cbc1e00b0c4da5c1`。不是当前 main 的 Opus 接入构建。

## 定点存储证据

只读连接实际进程所用 Agent SQLite，设置 query_only，仅输出目标轮次 ID、类型、计数和文件哈希。未导出正文、工具参数、完整命令或连接凭据。

- session：`ses_01M5WJ12RZ00020MJ8Q24`
- assistant message：`msg_01M80ZA3Q0003HTTN7SG1`
- QR 工具 part：`prt_01M80ZAJWW001D98DRHVJ`，14:33:02.620 +08，一次。
- 普通 image part：`prt_01M80ZAJXB001HFJQYPN3`，14:33:02.635 +08，仅一份。
- 随后的 `agent_send_media` part：`prt_01M80ZANVZ000EF2Q2AT0`，14:33:05.663 +08，完成状态，delivery 为 current_ai_session。
- 图片 part 的路径与显式发送工具的输入及输出路径，结合原 session.directory 解析后，canonical 文件相同。文件 SHA256：`7feeb16c8589aa9e071fe2cff71d3c9c4a5c87db373d5aa76e5fb7a38c14e6f7`。
- 此轮工具实际为 shell、QR 生成、显式媒体发送；没有 view_image。正文 Markdown 图片引用为零。

## 根因调用链

按发布源码 af01 对照，AgentChatPanel 的 appendStoredImagePart 与 appendAgentMediaCard 都调用 appendResolvedMediaCard。两条路径以各自 partId 加 -agent-media 作为行身份；当前只防同一个 part 重复插入，没有合并同一消息中不同 part 对同一文件的展示。

因此不是两次二维码生成，也不是数据库含两个 image part：普通 typed image 和显式媒体发送分别提供一份展示来源。显式媒体工具完成后，其工具卡被图片卡取代，所以截图前面的“工具调用 2 次”仅属于 shell + QR 工具组，不代表本轮只有两个工具 part。

历史重放同样分别处理 image 和完成的 agent_send_media，静态路径仍会产生两个不同的行 key。这是源码推论；本次未切换实际用户会话或重启程序，不宣称已现场验证重开后的表现。行 key 来自源码和持久化 ID，未直接探查运行 UI 内部对象。

## 最小修复建议及边界

先给已有 owned UI 用例补上“真实 QR 生成后，对同一文件调用 agent_send_media”的路径，再在同一 assistant message 内合并这两种展示来源，保留显式发送的 caption、交互动作和所有持久化工具记录。避免全局按图片哈希去重，也不删除历史 part。回归应覆盖重复通知、会话切换、面板重建，以及不同图片和不同消息仍分别展示。

当天日志未记录目标 message/part/call ID，不能据此数历史 emit 次数；持久化双来源已经足以解释两张图片，不需要假设事件重复。此轮没有修改生产代码或推进 main，修复和原场景回归尚未执行。

## 后续候选补丁的私有 Mac 验证（2026-11-22）

收到 Root 的 QR-only 两 UI 文件补丁，5501B，SHA256 `19aff6fc80af38316539b7c1e5c314eec4ad4778d5a0032069c304567b50a7b8`。独立限定审查未发现 P1/P2：索引随 ConversationState 和页面一起保留；同消息的未变化 canonical 文件复用首卡及动作；caption 合并去重；不跨消息或按图像内容全局合并，video/audio 不变。

在既有私有正常 Release 构建目录消费一次该补丁。主工作区生产文件和 HEAD 936e7e5dd 未动。私有 baseline 的 Core、desktop、Media、Network 编译树先核与 936 一致；之后生产 delta 仅这两个 UI 文件，另给测试补两个 slot 和 owned 片段。

用例移植时发现两个夹具问题：waitIdle 不保证 Qt 已处理交付通知；Mac QTemporaryDir 的 /var 与 /private/var canonical 别名使字符串相对路径越出工作区。只在私有测试修正等待 caption 后再计数，并把 canonical 临时根传给假模型和 session，不放宽真实路径边界。r1-r3 的这些失败保留，不计二维码重复 RED。

修正夹具后的旧逻辑 r4 明确 actual=2、expected=1，exit1，0.771s，已查看两张图片和 caption 的 owned 截图。应用原补丁后正常 maichat + agent_panel_session_test 构建、链接 exit0。三个 QR data case 全部通过（含 setup/cleanup 为 5/0，2.613s）：仅生成1张、同文件生成后显式发送1张、同字节不同文件2张。覆盖重复事件、会话切回、冷面板、caption、后继消息重发、原 part/工具输入输出/ID 保留、真实图片预览。

已查看 owned live/restored 截图，同文件只有一张且 caption 可见，不同文件仍为两张。另有媒体消息边界回归通过，两个缓存失效回归通过（setup/cleanup 合计4/0，0.781s）。

额外的既有 sentImagesOpenNormalPreviewLiveAndAfterRestore 在独立测试进程的 QDialog::exec 模态预览超过90秒未闭；sample 记录 open lambda -> MessageImageLoader::deliver -> QDialog::exec，只终止自有 PID65202（SIGTERM/143）。此项未通过，未证明与本补丁的因果，不称整个 Mac UI 套全绿，也不扩大生产修复。新增 QR 用例自身的图片预览通过单独保留。

原 Mac 现装 PID438 和可执行文件哈希保持，未安装、未更换用户程序、未操作现场会话、未请求 SSH/真实模型。本次没有 commit/push，补丁及 Mac safe receipt 已回传给唯一共享写源端串行收口。

## 用户后续发布授权

用户随后明确要求与 house-maichat 一起提交、推送并发布新版本。Parent 在 main 936e7e5dd 上聚合已核的 QR-only 两文件修改、自有测试和此诊断，桌面版升为 3.1.2；只对新增 C++ hunk 做既有 clang-format 格式化，不追加业务逻辑。发布源码也包含此前已在 main 的 Opus 接入及单进程运行恢复改进，不混入 Windows 本地尚未聚合的三项 context/turn 功能。Windows 与 Mac 之后按相同冻结提交正常重建及验包，公开发布结果另记，不把此段准备工作冒作已交付。
