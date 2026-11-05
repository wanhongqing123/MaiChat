# Agent 可见中间输出：源码对照、修复与验证

## 现场、复现操作与预期

- 记录时间：2026-11-05，Asia/Shanghai；本段源码核对与共享指令验证于 14:35 左右执行。
- 本机平台：Windows，PowerShell；工作分支为 `main`。
- MaiChat/MaiAgent 基线提交：`38407bf94947085a3113ce2ece1656a1e957898c`。
- 参考 Codex：`E:/OpenSource/multi-ai-code/third_party/aicli/codex`，
  提交 `2165b1da63527b66f4e491ff5e5c96d763eab973`。
- 本次源码检查没有读取用户凭据、模型目录缓存、真实会话或线上数据。
- 用户已有播放器相关未提交修改；这些改动由其他工作持有，不能归入本问题。

可复现的协议操作：以本地确定性模型回执模拟一轮任务，依次发送可见说明、工具调用、工具结果、
后续可见说明和最终回答。另覆盖同一次 Responses 响应中的两个 assistant message item，
以及只有 `response.output_item.done` 携带完整正文、没有文字 delta 的情况。

预期：可见说明立即进入正文，不以隐藏 reasoning 代替；工具执行及后续模型请求继续；
消息按原顺序保存，切换会话或恢复历史后仍可见；完整 item 回执能够补足未曾流出的正文。
长任务的阶段更新只报告真实观察和下一步，不编造比例、结果或完成状态。

产品验收操作：在正常 App 的 Agent 会话中执行已获授权、无需付费的多阶段本地工具任务，
观察工具前的简短说明、工具后的真实发现及最终结果；切换会话再返回并重启恢复，核对顺序和正文。
单个工具阻塞模型输出时，不能把未生成的新文字伪装成定时进度；应在模型恢复输出后报告真实状态。

本记录的初始判断来自静态源码核对。尚未填写的运行产物和设备结果不能视为已经复现实机问题。

## Codex 对照：可见正文与 reasoning 分流

以下路径相对上述 Codex 根目录，行号对应 `2165b1da`。

| 环节 | 证据 | 行为 |
| --- | --- | --- |
| 阶段元数据 | `codex-rs/protocol/src/models.rs:952`、`:956`、`:1036`、`:1053` | `MessagePhase::Commentary` 表示回合内的 preamble/progress narration，后续仍可有工具和输出；存于 assistant `ResponseItem::Message.phase`。 |
| assistant 与 reasoning | `codex-rs/core/src/event_mapping.rs:181`、`:193`、`:201` | assistant message 变成 `TurnItem::AgentMessage` 并保留 phase；reasoning 走独立分支。 |
| 供应商事件 | `codex-rs/codex-api/src/sse/responses.rs:354`、`:362`、`:482` | 上游 item done、文字 delta、item added 分别转成对应内部事件。 |
| item 开始 | `codex-rs/core/src/session/turn.rs:2934`、`:2993`、`:3010` | added 创建可见消息项并发 `ItemStarted`，保留 active item ID。 |
| 可见文字增量 | `codex-rs/core/src/session/turn.rs:3120`、`:3127`、`:3129` | 将文字增量归属到 active item，解析后发 `AgentMessageContentDelta`。 |
| item 完成 | `codex-rs/core/src/stream_events_utils.rs:359`、`:375`、`:385`、`:389` | done 转换完整消息项，发 `ItemCompleted`，另保存完整原始 response item；缺 started 时先补发。 |
| App Server | `codex-rs/app-server-protocol/src/protocol/event_mapping.rs:364`、`:406`、`:414`；`protocol/common.rs:1991`、`:1996` | 输出 `item/started`、`item/agentMessage/delta`、`item/completed`，完成事件含完整消息。 |
| TUI | `codex-rs/tui/src/chatwidget/protocol.rs:142`、`:148`、`:157`；`streaming.rs:445`、`:489`、`:521` | 可见增量流式显示，完整正文收尾；commentary 完成后恢复仍在工作的状态行。 |

### 消息完成、模型响应完成和工具回合完成是不同边界

- `stream_events_utils.rs:323`、`:346`、`:350`、`:356`：工具调用先保存，再创建执行 future，
  并将 `needs_follow_up` 设为 true。
- `session/turn.rs:2904` 将工具 future 放入 `in_flight`。
- `session/turn.rs:3069`、`:3112`、`:3115`：`ResponseEvent::Completed` 只结束本次 sampling，
  返回当前 `needs_follow_up`；`end_turn=false` 也会要求继续。
- `session/turn.rs:3280`、`:3288`、`:2565`、`:2580`：等待工具执行并记录实际工具输出。
- `session/turn.rs:578`、`:604`、`:718`：外层由模型 follow-up 或待处理输入决定继续，
  仅在不再需要继续时进入停止逻辑。commentary phase 本身不是停止条件。

### 持久化与恢复

- 完整 response item 经 `stream_events_utils.rs:93`、`:100` 进入
  `session/mod.rs:3610`、`:3724`、`:3736`，同时更新内存 history 和 rollout。
- `session/mod.rs:4710`、`thread-store/src/live_thread.rs:220`、`:261` 调用存储追加。
- `rollout/src/policy.rs:80`、`:83` 保留完整 Message；`:212`、`:215` 将 started 和文字 delta
  视为瞬态事件。`:132`、`:141`、`:250` 区分 paginated 的 canonical ItemCompleted
  和 legacy AgentMessage 事件。
- `rollout/src/recorder.rs:2107` 将记录序列化为 JSONL、写入并 flush。
- 模型上下文由 `core/src/session/rollout_reconstruction.rs:513`、`:514` 重放完整 response item。
- 新版界面历史由 `app-server-protocol/src/protocol/thread_history_projection.rs:78` 投影
  ItemCompleted；legacy 在 `protocol/thread_history.rs:523`、`:532` 恢复正文及 phase。
- TUI `chatwidget/replay.rs:299`、`:322`、`:326` 和 `streaming.rs:468` 使用完整正文恢复消息，
  不要求把历史 delta 全部重新播放。

### 进度提示词确实进入模型请求

- `codex-rs/models-manager/models.json:76`（gpt-6-astra）和 `:250`（gpt-6.1-sol）的
  `model_messages.instructions_template` 包含 Intermediate commentary：工具前先发说明，
  持续工作时不超过 60 秒没有更新。
- `models-manager/src/lib.rs:12`、`:15` 从 bundled models.json 加载模型目录。
- `core/src/session/mod.rs:797`、`:828` 的优先级是配置覆盖、恢复会话中的指令、当前模型模板。
- `prompts/src/model_instructions.rs:8`、`:9`、`:16` 返回模型模板；
  `session/turn.rs:1725`、`:1757` 放入 Prompt；`core/src/client.rs:996`、`:1004`、`:1006`
  将它加入请求前缀，角色由 `context/base_instructions.rs:31` 指定为 developer。
- 本地 fallback 在 `models-manager/src/model_info.rs:16`、`:151`、`:153` 绑定 prompt.md，
  其 `models-manager/prompt.md:173`、`:175`、`:177`、`:179` 也要求长任务更新、工具前说明。

这里证明的是本地实现和随源码附带的默认提示词。没有读取缓存，不能由此断言某次线上会话的
远程模型目录、覆盖配置或已保存旧指令与 bundled 文本完全一致。

## MaiAgent 基线的静态边界

以下行号对应 `38407bf9`，与修复后的工作区行号可能不同。

1. 常规正文已经能够流式展示和保存。`MaiAgent/Agent/MaiTurnRunner.cpp:487` 每次模型请求
   生成新的 text part，`:502` 的 onText 只处理可见正文，`:513` 的 onReasoning 单独处理草稿。
   `:770`、`:779`、`:791` 在创建及有界流式检查点保存消息。
2. 桌面历史显示全部正文 part。`MaiChat/desktop/src/agent/AgentChatPanel.cpp:2220` 遍历
   `message.parts`，`:2222`、`:2231` 将每个 MaiTextPart 加入视图；`:2232` 隐藏历史 reasoning。
   因此不能把常规中间正文不出现直接归因为界面只保留最终回答。
3. Responses 在 `MaiAgent/Agent/MaiResponsesClient.cpp:392`、`:399` 透传文字 delta，
   `:406`、`:409` 可以补发没有 delta 的 output_text.done 正文。
4. 同文件 `:420` 的 output_item.added/done 对 message/reasoning 只记协议活动，
   实际合并仅针对 function_call；不发 assistant message 边界、不保留 phase。
   所以同一次 response 中的多个 assistant message 被并入本次 requestCompletion 的 text part。
5. 只有 output_item.done 含完整 message 正文的回执不会在该事件立即交付，
   依赖 `:460`、`:463` 的 terminal response 完整 output 再补发。
6. 基线桌面和移动的基础 prompt 没有与 Codex 对应的通用工具前说明及阶段更新要求。
   既有部分业务/媒体工具的说明不等同于主 Agent 的通用可见进度政策。

以上是可定位的提示词与协议边界差异。没有这次现场的供应商逐项回执及对应安装产物，
不能把所有长任务沉默都归为同一个已实机证实的原因。

## 实现记录

### 已落地的共享进度指令

- 新增 `MaiAgent/include/MaiAgentProgressInstructions.h` 和
  `MaiAgent/Agent/MaiAgentProgressInstructions.cpp`，公开函数为
  `std::string maiAgentProgressInstructions()`。
- `MaiAgent/CMakeLists.txt` 将该 cpp 纳入生产核心库。
- 桌面 `AgentController.cpp` 在原 kDesktopSystemPrompt 后追加该函数返回值；
  移动 `MaiMobileAgent.cpp` 在原 kMarkdownBaseInstructions 后追加同一份文本。
- 规定工具前说明目的、长任务阶段更新不超过 60 秒、使用可见 assistant 正文、
  仅报告观察事实；不编造进度/比例/结果，不使用固定假状态，不为进度验证启动付费任务。
- 这是一段模型行为指令，不是计时器或后台生成的进度源。单个工具不能输出时，在其返回后报告事实。

### 协议和生命周期实现

- `MaiModelClient.h` 在原来的三个回调之后追加可选的可见发言开始、完成和带身份正文增量。
  身份包含本次响应内稳定的 index、供应商 ID 和 phase；推理仍使用单独的 `onReasoning`。
  phase 只描述本次流事件，不改变正文可见性，也没有在本次修复中增加数据库字段。
- Responses 按 item 和 content index 保存已交付正文。`output_text.done`、`output_item.done`
  和终止 output 快照只补发与已交付前缀一致的后缀，避免重复；没有逐字 delta 也能交付 item 正文。
  后续终止快照可以补足已经完成但正文不全的兼容 item。
- `MaiTurnRunner.cpp` 按响应内 index 选择原来的 `MaiTextPart`，不同 message 分别显示和保存。
  较早 item 在终止快照中补尾时仍写回原片段，不会误加到最后一段；完成发言不结束工具循环。
- Chat Completions 和本地模型可以继续只提供 `onText`，保持原来每次响应一个正文片段的行为。
  无需改造工具执行并发，也没有添加固定文案或后台伪造进度。
- 移动端按原始 parts 顺序显示连续工具组，首个工具 part.id 保持折叠身份。
  iOS 的 capture 图片保持单个工具详情和卡片，媒体去重后的归属顺序与播放列表一致；
  Android 正文继续按自身 part.id 更新，工具组不再跨越中间正文或媒体。

### 确定性回归

- `MaiModelClientTests` 在修复前新增 item.done 正文补足及两条 message 独立片段用例，
  首轮执行退出码 1，分别失败于正文 `ABCDone` 和片段数应为 2。
- 只读评审进一步发现两条终止快照边界：空 item.done 不能屏蔽后续完整正文；
  两个未收到 item.done 的 message 在 terminal 补尾时必须各归原片段。
  新增同一用例的两个变体再次执行退出码 1，两个 `actual == expected` 均按预期失败。
- `MaiTurnRunnerTests` 新增 `A→工具X→B→工具Y→C`，工具在 X/Y 处阻塞时检查事件正文已交付、
  存储包含对应正文与工具；完成后重新打开实际临时 SQLite 文件检查完整顺序。
  该用例保护已存在的常规工具前后正文能力，不把原来正确的行为写成现场根因。

## 已执行验证

本机源码契约入口：

```powershell
python MaiAgent/tests/contract/agent_progress_instructions_test.py -v
```

- RED：先新增契约检查，再运行。退出码 1；3 个测试，4 处失败：共享声明/实现缺失，
  桌面与移动均未接入共享函数。没有使用临时空实现作为失败替身。
- GREEN：新增共享实现与两宿主接入后，同一命令退出码 0，3 个测试全部通过。
- 检查覆盖两个实际宿主的 include 和 baseInstructions 赋值、公开声明与 CMake 生产源接入，
  以及正文/真实事实/时间间隔/付费边界的提示词约束。它不宣称模型必然遵从，也不替代实际 App 验收。
- 新增 h/cpp 已使用本机 LLVM clang-format 按仓库配置格式化。
- 仅编译新增 cpp：MSVC 14.44，C++17、`/EHsc /utf-8 /W4 /c`，退出码 0，
  没有警告；对象文件在 `.cache/agent-progress-policy-check-20261105/`。
  执行命令如下，没有链接或启动 App：

```powershell
& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64 >nul && cl.exe /nologo /std:c++17 /EHsc /utf-8 /W4 /c "E:\OpenSource\MaiChat\MaiAgent\Agent\MaiAgentProgressInstructions.cpp" /I"E:\OpenSource\MaiChat\MaiAgent\include" /Fo"E:\OpenSource\MaiChat\.cache\agent-progress-policy-check-20261105\MaiAgentProgressInstructions.obj"'
```

- `git diff --check` 未发现空白错误；原播放器测试文件的 CRLF 提醒属于已有工作区内容。
- 本代理没有构建或启动 Desktop/Android，没有停止或替换任何用户进程，没有创建提交。

移动端独立回归入口：

```powershell
python MaiChat/ios/MaiChat/scripts/tests/test_assistant_part_order.py
```

初次结构回归 3 项 RED；实现并补 capture 媒体边界/播放列表回归后共 6 项 GREEN。
该命令同时使用本机 Android Studio JDK 编译并执行无 Android 依赖的 Java 分组测试。
iOS 部分只是源码结构检查，不代表 Swift 编译或实际界面验收。

核心门禁使用独立缓存 `.cache/maiagent-core-gate/build`，从当前 `MaiAgent/` 源码重新编译。
没有复用旧 Agent C++ 对象；只复用当前 checkout 已有的 C TLS/其他第三方依赖。
本机 MSVC 的 Ninja include 前缀曾漏记公开头依赖，因此本次先按编译规则清理核心、
测试支持库和八个测试的全部 C++ 对象，再构建与执行：

```powershell
$coreTests = @('MaiCoreTests','MaiFilePathTests','MaiStoreTests','MaiModelClientTests',
    'MaiTurnRunnerTests','MaiToolTests','MaiToolLoopTests','MaiPermissionTests')
$coreRules = @('CXX_COMPILER__maiagent_unscanned_Release',
    'CXX_COMPILER__maiagent_test_support_unscanned_Release') +
    @($coreTests | ForEach-Object { 'CXX_COMPILER__' + $_ + '_unscanned_Release' })
ninja -C .cache/maiagent-core-gate/build -t clean -r $coreRules
cmake --build .cache/maiagent-core-gate/build --parallel 6 --target $coreTests
ctest --test-dir .cache/maiagent-core-gate/build -R '^(MaiCoreTests|MaiFilePathTests|MaiStoreTests|MaiModelClientTests|MaiTurnRunnerTests|MaiToolTests|MaiToolLoopTests|MaiPermissionTests)$' --output-on-failure
```

2026-11-05 Windows/MSVC 14.44：构建退出码 0；八套测试 8/8 通过，ctest 退出码 0，
总计 110.81 秒。`MaiModelClientTests` 28.83 秒，`MaiTurnRunnerTests` 31.90 秒。
其中包含上述两轮先 RED 后 GREEN 的协议用例、工具阻塞时可见正文和 SQLite 关闭重开。
测试只连接本地假模型，没有调用真实付费供应商。

基线源码提交为 `38407bf94947085a3113ce2ece1656a1e957898c`，加本次未提交修改。
关键源码与八个实际测试 exe 的 SHA-256 保存在
`.cache/maiagent-core-gate/intermediate-provenance.json`；构建及测试完整输出为
`intermediate-final-build.log`、`intermediate-final-tests.log`，都在同一门禁目录。

## 交付前需由统一验证补齐

| 对象 | 源码/产物/进程对应信息 | 结果 |
| --- | --- | --- |
| 核心协议、工具循环与持久化测试 | HEAD `38407bf9` + 未提交改动；独立 core gate 及源码/实际 exe 哈希如上 | 八套 8/8 通过 |
| Desktop 正常 App | 待记录构建目录、部署产物、进程路径/PID、截图或录屏 | 未在本代理实机验收 |
| Android 正常 App | OPPO PKD130，2.2.30(267)，最终 APK 与拉回安装包 SHA 完全相同；详情如下 | 正常启动/历史呈现/菜单/私有模型完整性通过；未触发供应商正文 |
| iOS | 待记录设备连接和构建条件 | 未验证 |
| 会话切换、重启恢复及取消 | 核心已验证阻塞/SQLite 重开及既有取消用例；产品界面路径待 root 统一记录 | 核心通过，App 未验 |

最终结论应依据上表真实结果填写；共享 prompt 契约通过不能扩写成完整产品验收通过。

## 桌面可见顺序的实际回归

统一构建前的 Qt 定点回归进一步证实：桌面正文片段本身没有丢失，但工具组原本按单一
`messageId` 聚合，`A→工具 X→B→工具 Y→C` 的 Y 会被并回 X，视觉上移到 B 前。
`intermediateProgressStaysVisibleAcrossToolsAndHistoryRestore` 首次实跑在
`firstGroup != secondGroup` 断言失败（`.cache/desktop-player-style-20261105/agent-visible-red-cdb.log`）。

现按源 parts 中连续工具的首 part.id 建组，正文、独立媒体/PDF 断组，reasoning 不作为正文断组；
独立索引把迟到状态和子 Agent 授权回填原组，摘要只统计本组。创建组直接复用调用方持有的
`MaiMessage`，避免长历史逐卡重新读取数据库。

真实 Core→Controller→Panel 使用本地模型替身与两个门控只读工具：X 阻塞时 A 已可见；
Y 阻塞时 A/X/B/Y 按序；完成 C 后切换会话、销毁 controller、重开临时 SQLite，顺序仍恢复。
两个新增定点用例已通过，包括相邻 X/Y 保持同组、回执独立。相应截图在
`.cache/desktop-player-style-20261105/agent-progress-green/`，仅包含测试面板的自建数据；
已亲看工具仍运行时 A、B 分别位于正确位置。最终桌面套件结果另行记录。

## Android 最终交付（2026-11-05）

### 构建输入与普通 Release

源码基线仍为 `38407bf94947085a3113ce2ece1656a1e957898c`，加当前未提交的生产改动。
共享 `MaiFfplayEntry.c/.h` 最终冻结后再次构建；12 个关键源码输入的完整 SHA-256 记录在
`.cache/android-intermediate-output-20261105/final-source-fingerprint.json`，安装前后均重新比对一致。
没有自行改版本、包名或产品签名。

本机 JDK 为 `C:/Users/18034/.jdks/jbr-17.0.14`，SDK 为
`C:/Users/18034/AppData/Local/Android/Sdk`，NDK `27.3.13750724`、CMake `3.22.1`。
现有本机 init script 固定 ARM64，并通过已核对的 seed hook 复用第三方 OpenSSL/FFmpeg
导出与头文件；没有复用旧 App、MaiAgent、共享适配器或 JNI 编译对象。

在 `MaiChat/android/MaiChat` 执行：

```powershell
$env:JAVA_HOME = 'C:/Users/18034/.jdks/jbr-17.0.14'
$env:ANDROID_HOME = 'C:/Users/18034/AppData/Local/Android/Sdk'
./gradlew.bat --offline --no-daemon `
  --init-script E:/OpenSource/MaiChat/.cache/android-install-19979472/local-build.init.gradle `
  '-PmaichatPythonExecutable=D:/Python314/python.exe' testDebugUnitTest assembleRelease
```

构建前用既有 native cache `RelWithDebInfo/3k666l63/arm64-v8a` 的以下规则清理，
本轮共清理 112 个编译产物，避免 `MaiStreamSink` 公开结构变化后混入旧 ABI 对象：

```text
CXX_COMPILER__maiagent_RelWithDebInfo
CXX_COMPILER__maichat_mobile_RelWithDebInfo
CXX_COMPILER__maichat_agent_RelWithDebInfo
C_COMPILER__maichat_ffplay_hosted_RelWithDebInfo
```

最终 Gradle 构建 1 分 17 秒，退出码 0。当前 JVM 报告为 96 suites、405 tests，
失败/错误/跳过均 0；Java 源变更后在前一阶段真实执行，本次共享 native 收尾构建复用该结果，
不把 UP-TO-DATE 写成重新执行。完整输出为新缓存目录的 `final-gradle-build.log`。

准备过程中两项本机问题都在安装前拦住：未加载历史 init hook 会选中缺 OpenSSL 导出的新缓存；
`android.injected.build.abi` 会生成 testOnly 包。已使用上述普通入口解决，未将 testOnly 包安装到手机。

### 包门禁与实际安装身份

最终签名 APK 位于：

```text
.cache/android-intermediate-output-20261105/MaiChat-Android-v2.2.30-intermediate-final.apk
```

- 大小 `1,527,245,059` 字节；版本 `2.2.30(267)`，包名 `com.kongshang.maichat`，非 testOnly。
- APK SHA-256：`7e1e73c5620f54eba20df860854ab5e8094231461fc4687724e9de071b4a6235`。
- 签名证书 SHA-256 与现装相同：
  `87704a9ff5f7394186ca146b6303485abdc8fc594a4f8d2da9aaddc87ea00ea5`。
- ARM64 库严格为 26 个，包含原有 7 个 CPU MODULE，无 NDK `libz.so` stub；
  APK 内每个库与实际 stripped 产物哈希对应，关键库 BuildID 与 native obj 对应。
- `libmaichat_agent.so` BuildID：`2df669e1976ba4a333119c6fab19ce26e4336428`；
  `libmaichat_ffplay_hosted.so` BuildID：`5cba7dd91b723299e72207dd26101ceb5671ce4e`。
- GGUF 未压缩、完整 `1,396,198,496` 字节，SHA-256 为固定
  `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`；manifest、许可与模型卡也逐项核对。

手机为 OPPO PKD130 / Android 15，serial `89VCD65LEYTGROW8`。
使用 `adb install -r --no-incremental` 同包原位安装成功，没有卸载正常 App 或清数据。
随后从 `pm path` 给出的新 codePath 拉回实际 `base.apk`，完整 SHA-256 与上述最终签名包完全相同。
首次安装时间仍为 `2026-11-02 23:16:39`，本次更新时间为 `2026-11-05 15:20:18`。
安装验证见 `installed-verification.json`、`final-install.log` 和 `verify-installed-final.log`。

### 目标手机上的检查与模型数据保护

已获授权沿用既有、同证书、无 Launcher 的单项完整性测试 APK，仅执行
`MaiBundledPrimaryModelInstrumentedTest#preparedModelMatchesThePinnedIosResourceInsideThePrivateAppRoot`。
没有添加另一个 MaiChat 产品包、读取账号/聊天文件或运行推理。该探针 **1/1 通过，7.15 秒**；
独立校验现有私有模型的文件身份、字节数与完整 SHA，与安装前的收执一致，`inference_exercised=false`。
本轮测试包 `com.kongshang.maichat.test` 已移除，正常 App 保留。

随后启动正常 `MainActivity`，冷启动 `TotalTime=1044ms`，检查时正常进程 PID `13780`，
`/proc/13780/cmdline` 为 `com.kongshang.maichat`，与新安装 codePath 对应。
亲看 `final-launch.png`、`final-creation.png`、`final-settings.png`：启动页正常；
原有自建 QA 会话和文件工具结果仍在，工具组后跟原有最终正文；更多菜单保持
**云端模型 / 标准 / 请求批准**。没有改模型来源、强度或权限，没有清理会话或输入草稿。
检查后已通过普通导航回到消息页。截图仅保留在本机缓存，不作为公开发布素材。

本轮没有发送云端模型请求、执行本地推理或调用付费模型，因此没有在目标手机重新触发
“模型中间正文→工具→后续正文”的新供应商流。手机检查证明新 APK 安装、启动、既有历史展示与配置保留；
多段中间正文及快照兼容由前述本地假模型协议、持久化和分组回归覆盖，不能扩大成真实供应商行为验收。

本机结果文件均在 `.cache/android-intermediate-output-20261105/`：
`final-device-result.json`、`package-verification.json`、`after-private-model-integrity.json`、
`final-model-integrity-probe.log`、`final-remove-probe.log`、`final-normal-launch.log`。
