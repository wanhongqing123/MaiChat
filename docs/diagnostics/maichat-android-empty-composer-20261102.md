# Android 空输入框长按与消息空白收键盘

- 日期：2026-11-02，现场复现时间约 23:28–23:29（Asia/Shanghai）。
- 源码基线：`main`，`19979472`。
- 运行平台：OPPO PKD130，Android 15，正常包 `com.kongshang.maichat`，Release 2.2.6（238）。安装与签名由主任务另行核对；本记录不保存账号、令牌或服务凭据。
- 操作边界：保留已登录账号和数据，复现仅点击、长按和查看画面，没有发送文字、语音或模型请求。

## 复现步骤与预期

1. 从消息页进入已有私聊，确认输入框为空。
2. 在编辑框内保持手指约 900ms。
   - 预期：进入语音转文字，提示正在听；上滑后松手应取消。
   - 实际：只出现系统“粘贴”菜单，未进入录音。提示仍为“输入要发送给当前联系人的消息...”，占两行。
3. 点击输入框弹出键盘，再点击消息列表下方的白色空白区域。
   - 预期：收起键盘，保存草稿；消息链接、复制与滚动仍可用。
   - 实际：键盘保持显示。
4. 切到创作中心，对空输入框长按。
   - 实际：虽提示“可按住转文字”，仍只出现系统粘贴菜单。

现场截图保存在忽略的本地目录 `artifacts/composer/`：`im.png`、`im-long.png`、`im-blank.png`、`longpress.png`、`ime.png`、`blank-ime.png`。图片包含当前聊天画面，不提交仓库。

## 根因证据

- `MainActivity.composer()` 只给语音模式的独立按钮绑定 `handleVoiceTouch`；文字框的 `OnTouchListener` 仅恢复键盘定位开关，始终返回 `false`。空编辑器长按完全进入 Android 原生文字编辑路径。
- `AIAssistantPanel` 同样只给隐藏的 `voiceHoldButton` 绑定 `voiceTouch`，其可见编辑器没有长按录音绑定。
- 普通聊天消息列表没有空白点击隐藏键盘的事件入口。创作中心的监听绑在 `ListView` 的 `OnTouchListener`，子视图消费事件时父列表监听不会收到完整手势。
- 普通聊天 `startVoiceRecording()` 使用不带 ASR ticket provider 的录音入口，且 `speechRecognizer` 初始化为 `null`，所以这一入口只能回退语音消息。不能只补提示文案来声称转写已接通。
- 对照 iOS `ChatView.handleVoicePressChanged/Ended`：空编辑器长按使用实时转写，默认松手发送文字；独立语音模式使用直接语音；上滑取消不发送。

## 修复与验证

实现：

- `ComposerVoiceInput` 复用 Android 原生长按定时器，只有空草稿长按开始实时转写。短按继续打开键盘；非空草稿仍可原生选字；按住时使用屏幕坐标判断上滑取消，键盘收起导致编辑框移动不改变手势原点。
- 普通聊天空编辑器领取账号临时 ASR ticket，并通过已有客户端实时流直连供应商。独立语音按钮仍发语音；转写失败不隐式发送语音。取消、账号代际变化后丢弃结果。
- 创作中心可见编辑器接入相同手势及已有录音器，录音时在原输入区域显示“正在听／松开取消／实时文字”。输入控件保持挂载，手势不因反馈层出现而中断。
- `ChatKeyboardDismissal` 在消息容器分发层观察空白短按，并继续向子控件分发事件；点击链接、长按复制、拖动滚动不触发收键盘。

本地事件回归：

- 用本机 JDK 17、缓存 Robolectric 4.12.2 与 SDK 34 运行 `ComposerVoiceInputTest`（6 例）、`ChatKeyboardDismissalTest`（3 例）及 `ComposerVoiceResultGuardTest`（4 例），不连接账号或 ASR 服务。
- 修复前同一测试运行得到 8 个断言失败，分别提示缺少空编辑器长按绑定和消息空白短按收键盘容器。
- 最初 8 例通过后，补充“录音编辑器离开窗口”得到 1 个功能失败；加入离窗取消后通过。此边界覆盖旋转或页面重建未向旧编辑器交付 `ACTION_CANCEL` 的情况。
- 独立审阅发现创作中心松手等待 final 期间，用户可编辑或主动提交新请求；旧 final 可能覆盖新稿，或在 busy 状态进入 `send()` 的停止分支。`ComposerVoiceResultGuard` 按编辑和提交代际保护结果，ASR 自有 partial 不增加代际；写入 final 前额外核对可提交状态。4 个新增回归先失败，接线后通过。
- 最终本地运行结果为 `OK (13 tests)`，覆盖长按开始、正常松手、上滑取消、系统取消、离窗取消、已有草稿、短按、滑动、空白点击、点击控件传递、迟到 final 与新编辑／提交隔离。
- 本地复现命令：`artifacts/composer/run-robolectric.ps1`（忽略的环境脚本）；标准 Gradle 入口为 `:app:testDebugUnitTest --tests com.kongshang.maichat.ComposerVoiceInputTest --tests com.kongshang.maichat.ChatKeyboardDismissalTest`。

边界：待主任务统一完成 App 编译、签名安装、修复包真机按住后上滑取消和收键盘检查。当前尚未宣称修复包真机验收通过，也没有验证实际文字／语音发送或发起模型任务。

## 真机检查步骤与安全脚本

设备操作由主任务串行持有。检查仅在已打开的空编辑器执行，保持既有账号、权限和数据：

1. 核对修复 APK 构建／安装版本、签名与运行进程，打开已有私聊，查看提示为“可按住转文字”。
2. 短按空编辑器，确认能正常打开键盘；点消息下方无文字／链接的白色区域，确认键盘收起。再短按编辑器确认焦点可恢复。
3. 确认麦克风已获授权后，保持按下超过 600ms，检查录音反馈出现；先向上移动超过 100dp，再在上方松开。确认反馈消失、草稿仍为空、无新增消息。
4. 创作中心重复相同操作；确认“正在听／松开取消”反馈出现在原输入位置，松开后不发送、不新增模型任务。
5. 已有非空草稿只检查原生选字／复制；切回会话后草稿应保持。真实账号若只有一个联系人，双 peer 草稿隔离列为未实测，不新增联系人或发送消息来补场景。

首次麦克风权限流程：当前真机若已有授权，不撤销权限来制造首装条件。未授权环境下首次长按只弹出系统授权，不自动录音；授权或拒绝后回到空编辑器。授权成功需重新长按才能开始，拒绝不产生消息。此路径需有对应环境才可记录为实测。

本地可复用脚本 `artifacts/composer/verify-on-device.ps1`（不提交）默认仅截图；`-Mode HoldCancel -EmptyEditorConfirmed` 会查找唯一空输入框、核对前台包与既有麦克风授权。居中三态版使用左上各 200dp 的取消兜底，且横移不足 130dp 时不注入手势；`finally` 中始终在该左上取消坐标发 `UP`。三态版中同 X 垂直上移仍是 SEND，严禁再用旧的同 X 上滑验收脚本。输出仅本地截图／UI 层级，不输出聊天内容或账号凭据。

## 2026-11-03：真机转写失败定位与普通协议收敛

修复手势后的 Release `2.2.7 (239)` 出现“语音转文字失败”。原位安装后当前账号仍已登录，未更改用户数据。

- 首轮 PID `28715`：`RECORD_AUDIO` 已授权，appops 记录前台允许并实际录音约 2.151 秒；不能将失败归因于权限。
- 原有阶段诊断只写私有 `files/diagnostics/ui.log`，正常 Release 不能通过 `run-as` 读取。增加固定 ASR 阶段、数字错误码与异常类名的 logcat 记录；统一正则白名单禁止 URL、签名、token、原始异常 message、识别文本或响应原文进入记录。
- 日志版原位安装后 PID `32490`，2026-11-03 00:05 左右，在创作中心空编辑器持续按住 4 秒，再向上移动 508px 后松开取消。得到：`asr-recorder-started → asr-ticket-ready → asr-start → asr-service-failed code=4004`。握手未进入 ready，音频发送泵未启动。没有新增消息或模型任务。
- 决定性根因：Android `RealtimeAsrCredential` 仍选择 `16k_zh_en_2.0 + result_mod=1`，使用大模型 `sentences` 解析；此前 iOS 现场已因相同 4004 由用户选择普通 `16k_zh`，并记录普通引擎拒绝 `result_mod=1`。见 `maichat-asr-v2-sts-20261030.md` 末两节。此次属于 Android 未跟随已确定的正式引擎与协议收敛。

本次修复采用单一普通协议：

- 签名参数选择 `16k_zh` 并去除 `result_mod`；STS 领取／内存缓存、临时 token 固定请求头和客户端直连路径保留。
- 按普通协议的 `result.index / slice_type(0,1,2) / voice_text_str` 逐段替换并按序合并。稳定段拒绝迟到 partial，最终 `final=1` 没有 result 时保留已有全文，取消不提交。
- 删除正式解析器的大模型 `sentences` 分支，不维护两套失败回退协议；本地 WSS 测试样例同步为普通 `result`。
- 用公开合成凭据和独立 .NET HMACSHA1 计算签名预期值。普通签名及分段解析测试在旧实现下得到 8 个断言失败，修复后与输入交互测试合计 `OK (26 tests)`。
- 普通协议字段、同 `/asr/v2` 地址、音频节奏与错误码已核对 [腾讯普通实时 WebSocket 官方文档](https://cloud.tencent.com/document/product/1093/48982)。文档中的路径名称不代表客户端应选择大模型 V2 引擎或 `result_mod`。

普通协议修复包真机回归（2026-11-03 约 00:10–00:12）：

- 主任务完成完整单元测试、AndroidTest 编译和 Release 构建，并同签原位安装 `2.2.7 (239)`；本轮运行 PID `2540`。构建和安装产物核验由主任务单独记录。
- 私聊空编辑器持续按住：实际日志为 `asr-recorder-started → asr-ticket-ready → asr-ready → asr-audio-sent samples=1024 → asr-partial`（多次，最多 17 字符）→ `asr-recording-cancelled`。屏幕确实显示实时识别文字；不在日志或本记录复述识别内容。手势在原点上方 490px 处松开，取消后恢复空编辑器。截图 `artifacts/composer/im-standard-holding.png` 与 `im-standard-cancelled.png`。
- 私聊键盘：仅输入执行者自己的临时 ASCII 草稿，点击消息空白后 `mInputShown=false`、编辑器 `focused=false`，草稿完整保留。随后重新聚焦并仅删除该临时稿，回读确认恢复空框提示；未按发送或 Enter。截图 `artifacts/composer/draft-blank.png`。
- 创作中心空欢迎会话持续按住：屏幕显示实时识别文字，向上 508px 后松开取消，回读编辑器为空，欢迎页无新增会话消息。截图 `artifacts/composer/assistant-standard-holding.png` 与 `assistant-standard-cancelled.png`。
- 期间用户也在手持设备操作，私聊日志出现两次另行正常结束的识别。检测并发后停止所有 ADB 触摸并交回设备；不将用户操作归入自动验收，也不继续做发送测试。本轮执行者的两个录音手势都以取消结束。

边界：已验证真实麦克风、普通 ASR 握手、音频发送、实时识别、取消以及私聊键盘／草稿。没有由执行者验证自动发送、修改后发送、双 peer 或首次未授权环境。用户随后要求居中转写浮层以及取消／修改两个动作，属于后续交互调整；以上截图仍是旧输入区域反馈形态，不能作为新浮层视觉验收。

## 居中三态浮层接线

- IM 和创作中心转文字均使用 `VoiceTranscriptionOverlay`，旧底部实时条／语音按钮动态文字不与其同时显示。直接语音模式保留原有语音消息路径。
- 空编辑器在 350ms 开始，手势只传原始 start/move/end 坐标，目标选择由共用组件执行。系统取消、多指中断或编辑器离窗均取消录音；松手后离窗也失效在途 final。
- SEND 和 EDIT 松手后显示 finishing，等真实非空 final 才更新文字，再停留 240ms。EDIT 只填入草稿并打开键盘，不调用发送；CANCEL 清除本轮 partial 并终止录音，不清用户另行输入的草稿。
- 编辑／主动提交／账号或会话变化／离页会使旧 final 和浮层失效；240ms 延迟回调再次核对录音身份、编辑代际和页面状态，不能覆盖新草稿或操作新请求。
- `VoiceTranscriptionResultTest` 5 项先失败后通过，覆盖等待真实 final、EDIT 永不返回 SEND、取消、空 final、失效与一次交付；手势新增 raw 坐标、350ms 和松手后离窗等回归。
- 定点测试实际复现：350ms 自定义开始后在 400ms 松手，原生 long-click 仍在稍后发生一次。`cancelLongPress()` 与按压状态清理修复后该用例通过，避免晚到粘贴菜单。最新本地汇总 `OK (34 tests)`，两个宿主独立 `javac` 退出 0。

### 居中三态版最终真机结果

2026-11-03 约 00:31–00:33，主任务完成 223 项单元测试与 Release 构建、同签原位安装后冷启动，运行 PID `5787`。构建源码和 APK 哈希由主任务交付记录核对。

- 私聊空框持续按住，浮层位于屏幕中央，下方叉号／铅笔分区可见，输入区域没有第二条转写文字；移动到左叉中心 `(216,1220)` 时叉号变红，标题变为“松手取消”。随后在左上取消区域松开，浮层退出、草稿仍空。
- 创作中心空欢迎会话按住，显示相同居中浮层；左叉中心 `(216,1088)` 命中后同样变红，取消后空框和欢迎会话保持。
- 创作中心再次按住，主任务在 PC 扬声器播放一次固定中文测试短句。右铅笔中心 `(504,1088)` 命中后呈蓝色，并显示“松手编辑”；在该点松开后浮层保留“正在准备编辑…”。
- 实际日志收到 `asr-end-sent samples=378880 → asr-server-final chars=46`。final 后编辑器回填 46 字符、`focused=true`、`mInputShown=true`；欢迎页无新增消息，未提交模型请求。记录只保存字符数，不复述环境拾音或识别正文。
- 清理前再次回读编辑器，确认内容仍逐字等于本轮生成的草稿后，仅删除该草稿并收起键盘；回读确认空框提示和欢迎会话均恢复。未清除用户原有草稿／消息，未登出或清数据。
- 现场截图：`artifacts/composer/final-im-holding.png`、`final-im-cancel-target.png`、`final-im-cancelled.png`、`final-ai-holding.png`、`final-ai-cancel-target.png`、`final-ai-cancelled.png`、`final-ai-edit-target.png`、`final-ai-finishing.png`、`final-ai-edited.png`、`final-ai-restored.png`。脱敏阶段记录：`final-asr-safe.log`。全部为忽略的本地产物，不提交识别正文。
- 检查完成即交回设备，执行者停止所有 ADB 操作。

当前边界：两入口居中视觉、取消，以及真实最终结果的 EDIT 回填／键盘已通过真机检查。SEND 自动发送没有由执行者触发；其分支与迟到结果、取消／编辑隔离由定向单元测试覆盖。首次麦克风未授权和双 peer 场景仍未在本机制造或实测。
