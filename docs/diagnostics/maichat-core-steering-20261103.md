# 运行中补充当前任务的共享核心边界

日期：2026-11-03。源码基线：`9959488832f2d6f1ab8831fd6b0029f68180ac01` 加本轮未提交改动。复现来源：iOS 创作中心美颜持续工具循环，用户输入“别再反复调参，先把当前结果给我”，原 composer 因 busy 禁止发送。用户明确选择补充当前任务，保留正在执行的工具。

预期操作：开始任务，工具执行中提交补充。当前工具以及同批其余工具正常完成；下一次模型请求包含全部真实工具回执及补充用户消息，模型据此调整后续工作。普通 `MaiSendPrompt` 的忙拒绝保持原有行为。

## 普通 API 与移动桥

`MaiAgent::submit(MaiSteerPrompt{sessionId, turnId, requestId, text, images, videos, quotedMessageId})`。`activeTurnId(sessionId)` 是整轮最初的 assistant 消息 ID，assistant 分段后保持不变。idle 返回空。

移动 C ABI 与既有 JNI 使用同一 `maiMobileAgentRequest` JSON 通道：

```json
{"op":"steer","session":"ses_...","turnId":"msg_...","requestId":"client-uuid","text":"Show the current result first"}
```

图片、视频和 `quoteMessageId` 可选，形状同 `send`。本机 snapshot 的顶层 `turnId` 属于选中会话，`sessions` 各项也带对应 `turnId`；`remote_snapshot` 带目标会话的 `turnId`。成功返回 `{ "ok": true, "id": "msg_..." }`，ID 指已落库的 user 消息。宿主捕获目标账号句柄、session 与 turnId；拒绝时保留草稿，不自动回退 send。

输入要求：requestId 为 1..128 字节可见 ASCII；文本最多 64 KiB；图片与视频各最多 10 项；每轮最多接受 64 条补充。空输入及失配 turnId 为 InvalidInput，idle 为 NotFound，stopping 为 Canceled，已封闭输入的收尾为 Busy。相同 session + turnId + requestId + payload 重试返回同一 user ID；同请求 ID 修改内容拒绝。活动轮保留其全部接收凭据，Agent 实例另保留最近 128 份凭据以处理结束后的重试，凭据不跨账号 Agent 实例或进程重启。

## 顺序与生命周期

补充先以普通 user 消息持久化，再广播。运行状态把未消费 user ID 从当前模型历史过滤，在完整工具批次之后才原子完成旧 assistant 段、创建新段并消费。历史顺序因此是 assistant 的 tool_calls、全部 tool results、补充 user、后续 assistant；不会在工具调用与回执之间插入新用户。

全部分段仍使用同一个 TurnRunner、取消标志、权限拒绝记录和迭代预算，整个轮次只完成一次。模型返回无工具的末尾流也经过原子 drain-or-close；已接受输入会触发后续模型读取，已进入最终无工具收尾才来的新输入会被拒绝。预算耗尽的最后请求先封闭输入，再读取此前已接受的补充。补充本身不置 cancel，不撤回工具。停止或模型失败保留已接受历史，不自动另启一轮。

专业任务在本轮开始捕获既存历史身份，临时回复仍位于全部既存历史之后、本轮 assistant 分段与新补充之前。这样重试复用较旧通知 ID 时，不会把既存的新用户消息错移到回复后面。通知确认仍归属原先预留的专业任务消息，不随 assistant 分段更换任务归属。

## 回归证据与验证边界

`MaiToolLoopTests` 使用本地假模型与条件变量工具闸门，无供应商网络或付费调用。新增用例核对完整双工具批次回执顺序、下一次请求真实读取补充、当前工具 cancel=false、后续工具位于新的 assistant 分段、先落库再广播、幂等与冲突、idle/错误会话/错误轮次/stop 边界，以及最后一次普通模型流中接受输入后仍读取的竞态。

新增四个 ToolLoop 回归全部通过；其中专业任务用例同时覆盖旧 notice ID、较新的既存用户历史、工具执行中补充、后续模型顺序与通知完成归属。

最终八套 Core 测试已按改后的源码重新构建，8/8 exit 0，最多两套并行，共 103.626 秒。每项日志、时间、源码 SHA-256 见 `/private/tmp/maichat-steer-core-final-20261103-results.json`；构建日志为 `/private/tmp/maichat-steer-core-eight-final-build-20261103.log`。八项依次为 MaiCoreTests、MaiModelClientTests、MaiTurnRunnerTests、MaiToolTests、MaiToolLoopTests、MaiPermissionTests、MaiStoreTests、MaiFilePathTests。构建仅有已有的静态库重复链接警告。

新增标准 CMake 测试入口 `MaiMobileSteeringTests`，由既有 `MAICHAT_MOBILE_TESTS=ON` 控制。独立 macOS arm64 harness 编译实际 `MaiMobileAgent.cpp`、HostTools 和共享工具，复用本机静态依赖；没有替换产品 dispatch 或核心实现。模型只连 `127.0.0.1`，宿主工具是条件变量闸门，资产能力探测明确返回不可用，不配置真实账号或供应商凭据。此测试验证 C ABI 接受后 SQLite snapshot 中恰有一条真实 user、幂等、冲突拒绝、send Busy 不变、稳定 turnId、工具完成前无后续模型请求、完成后 wire 中 assistant/tool/user 的精确顺序和 idle 拒绝。最终 1/1 通过，1.30 秒：`/private/tmp/maichat-steer-mobile-native-final-test-20261103.log`。

native harness 可重复执行：`cmake --build /private/tmp/maichat-steer-mobile-native-20261103/build --target MaiMobileSteeringTests -j2`，然后 `ctest --test-dir /private/tmp/maichat-steer-mobile-native-20261103/build -R '^MaiMobileSteeringTests$' --output-on-failure`。构建配置与依赖路径保存在该目录的 `CMakeLists.txt`；产品目录的标准测试 target 也已列入同一源文件。

原有完整 `MaiMobileAgentTests.cpp` 也增加 C ABI 流中补充回归，native `-fsyntax-only` 已通过；该完整平台测试本轮未运行。`git diff --check` 与改动核心文件的 clang-format 检查通过。此记录只涵盖共享核心与 C ABI，iOS composer、远端控制链及实际安装产物由对应平台验收记录覆盖；未操作设备、安装或提交。
