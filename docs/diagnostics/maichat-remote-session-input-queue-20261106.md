# 远端 Session 连续发送与所属端排队

2026-11-06；main / eee43402 工作区。用户要求：iOS 或其他端向远端 Session 连续发消息时，
发送端不等待当前模型任务完成，WebSocket 服务持续转发；所属端保存并排队，在安全边界
或当前轮次结束后把新消息交给模型。

复现操作：让所属端的第一条模型请求保持执行，再从远端连续发送第二、第三条消息，
不携带发送端猜测的 turnId。预期各条都及时确认“所属端已接收”，按序进入会话；
当前工具结束后即消费新输入，尚未开始的同批调用先交回模型重新判断；
不打断正在运行的工具，确认接收不等于执行完成。最初按整批消费的方案已按用户补充修改，
具体复现和验证见 `maichat-per-tool-pending-input-20261106.md`。
网络重试保留同一请求编号，不能重复插入消息；发送按钮不能因忙碌变成停止。

已定位：Desktop RemoteAgentPanel.sendOrStop 在 busy 时发送 stop；普通 owner prompt
最终调用 MaiSendPrompt，核心 rejectWhenBusy 拒绝第二条。iOS 根据远端 busy/turnId
选择 steer，缺少 turnId 会失败；本地 FIFO 还会等待前一条所属端回执。同步服务本身
没有按 busy 拦截控制命令，已经使用独立的持久信箱转发，不负责调用模型。

修改前快照及验证产物：`/private/tmp/maichat-remote-input-queue-20261106/`。
先以真实 DesktopSessionSyncHost + 无付费模型的 held-request 用例验证失败，再实现
所属端共用的输入队列，覆盖收尾竞态、幂等、附件、停止与账号/会话隔离。当前安装的
Mac 2.2.34、手机旧版均不能代表新源码效果；不停止或替换正在使用的进程。

## 2026-11-06 合并远端与正式实现

用户补充提醒播放器已重构后，执行 `git fetch origin`，将 main 从 eee43402 快进
17 个提交至 9459e7ec。先将 282 个本地修改/新文件逐个备份到
`/private/tmp/maichat-upstream-sync-20261106-1428/` 并保存 Git stash，再恢复并解决冲突。
保留上游持久 outbox、模型选择、提问生命周期、文本/工具/媒体顺序和新播放器组件；
没有用旧页面覆盖上游重构。现有本地文件没有丢失，备份和 stash 均保留，未提交或推送。

共用 C++ 普通入口为 `MaiEnqueuePrompt{MaiSendPrompt, requestId}`。SQLite v7 保存
`input_request_id` / `pending_input`，按会话和请求编号精确去重。接收期间不读取观察端
turnId；新输入先落库再发回用户消息编号。当前工具结束后，有补充便给尚未执行的同批
调用补齐明确的未执行回执，然后把输入和已有结果交给模型重新判断后续操作。
收尾阶段已关闭 steering 时，接收的新输入在原轮退出后启动下一轮。
停止会暂停所属端已存队列而保留内容；重启不自动调用付费模型，下次主动发送/原编号重试
恢复。清空或删除会话同时清除内存队列及其收据范围，不能从旧内存恢复已清历史。
普通核心入口与 `session_send_input` 模型工具共用实现；工具只向另一根会话发送文本，
不允许自发自收循环，审批遵循现有工具策略。

桌面和 Android 复用本次上游新增的持久 outbox，避免并存两套投递路径。iOS 继续使用
原 queue actor/pump。各端分开保存 relay ACK 与 owner 接收回执：relay ACK 后放行下一条，
owner 回执按请求编号独立结算，允许乱序、切换会话及迟到回执。未知送达的重试沿用
原输入编号、附件引用和所属端稳定导入路径，使用新的信箱消息编号再次送达。
Android 的附件准备也按所属会话顺序执行，不在 WebSocket 收件线程等待模型/文件任务。
Stop 保留独立入口及原轮次校验，键盘发送不会变成 Stop，也不覆盖正在编辑的新稿。

同步服务生产实现没有修改，也没有新增模型代理。服务继续转送控制消息，模型和媒体
请求仍由客户端直连。服务已有持久信箱；带副作用命令取件后的未知投递使用原输入编号
显式确认，不能假设服务会自动重放模型操作。协议见 `services/auth/README.md`。

## 验证

测试均使用本地假账号/held model，没有触发付费推理或媒体任务。

- 修改前真实 DesktopSessionSyncHost 的最小用例失败：第二条普通消息返回
  `a turn is already running for this session`。证据 `baseline-red.txt`。
- 当前生产 C++ 库对应的 8 套套件全部通过；含 80 条忙碌输入顺序、相同文字不同点击、
  原编号重试/内容冲突、收尾竞态、停止/SQLite 重开、清历史和模型工具入口。
  `core-suites-upstream-results.json`；console E2E 全部通过。
- Qt DesktopSessionSyncHost/Coordinator/RemoteAgentPanel：38 项通过。验证独立 Stop、
  不等前条回执连续发送、收据归属、迟到回执不改新任务、重启不盲发及原编号恢复。
- 同步服务 24 项通过，包含所属端 busy 且 turnId 为空、暂扣全部 owner 回执时三条
  消息已全部转发的 WebSocket 用例。`service-tests-upstream.log`。
- iOS 正常 App：独立 iPhone 16 Pro Max / iOS 26.5 模拟器，
  `testRemoteInputsPipelineWhileOwnerBusyAndReceiptHeld` 通过，17.923 秒。
  保持 fake Mac busy、turnId 为空并扣住回执，发送 A/B/A，服务收到三个独立编号、
  不含 turnId。放行回执后显示三条正式历史。录屏 `ios-ui7.mp4` 和五阶段帧、两个
  xcresult 截图已经查看，输入框和键盘始终可用。
- iOS actor 16 项通过；合并后的字体、代码字体实际字宽与 HTML br 五项定点检查通过。
  最初 UI 用例在加载遮罩未消失时点击底栏，已改成观察真实页面可交互后再点击；
  失败录屏保留，不计为功能通过。假账号的腾讯 IM 70402 横幅来自假 userSig，
  不是 Session 同步错误，没有据此修改生产 IM 错误处理。
- Android：独立 API 37 / arm64 / 16K 页模拟器 emulator-5582，正常 MainActivity，
  独立 `.isolated` 本地鉴权包。相同三输入场景通过，首轮 8.051 秒；273 最终包严格等待三条正式历史绘制后通过，7.429 秒。
  不以 worker 队列变空代替页面已刷新。原 emulator-5580
  和生产应用数据未被修改。Android 本地鉴权调试开关同时选择本地 WSS，仍严格验证
  TLS；测试只替换假账号私有根目录的证书文件，结束恢复，不改系统证书。

## 安装与边界

以下 2.2.53 / 201 为上一次验收的冻结产物。后续逐工具检查已生成 Mac 2.2.54、iOS 202；
202 已原地安装真机并启动，最新产物及验证边界见 `maichat-per-tool-pending-input-20261106.md`。

Mac 2.2.53 便携包位于
`/private/tmp/maichat-macos-2253-delivery-20261106/MaiChat-macos-arm64/MaiChat.app`，
深度/严格签名及清除开发 Qt/DYLD 搜索路径的 `--smoke` 通过。播放器采用 9459e7ec
的新外观，HDR/云盘能力和本次队列均已合并。当前 `/Applications/MaiChat.app` 仍是
2.2.34、PID 43461，未停止或替换；真实所属端也须升级，旧所属端不能实现新接收语义。

iOS 2.2.4 (201) 已构建并签名冻结到 QA 目录的 `device201-final/MaiChat.app`。
CoreDevice 最新查询真机 iPhone 为 unavailable，尚未安装或作真机验收。
Android 正常 App 已在独立模拟器验证；Windows 尚未构建/实机验证，不能替代其验收。

扩展运行 iOS MarkdownPresentationTests 时还有历史滚动/长列表的 10 个断言未通过，
集中于 3 个用例；对应生产 MarkdownPresentation.swift 与拉取前的备份逐字节一致。
尚未在正常 App 中复现或定位，不能把整组扩展回归报告为通过。字体包在无 UIApplication
的独立单测中需要自行注册，修正测试宿主后真实字宽检查通过，没有放宽字宽断言。
供应商 120 秒无流数据、远端真实网络断开恢复及 Mac 物理 HDR 全屏瞬时亮度不属于本次
队列验证结论；有关 HDR 窗口显示和全屏边界见对应诊断记录。

最后补齐 Mac 接收阶段的附件 FIFO：先用精确重入用例证明旧路径中第二条文字在
第一条附件准备前落入 Core，2 个用例均失败（117ms）。修复后，附件失败再放行文字、
Stop 绕过准备并保留发送端内容、登录变化不消费旧输入，以及 Controller 在准备期间
销毁的回归均通过，整套 38 项通过。账号更换会失效旧文件回调；附件装饰通过持久
用户 ID 精读，不再取“最近两条”。最终 Mac 包包含这一版接收队列。
