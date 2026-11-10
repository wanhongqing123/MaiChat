# Windows console e2e：轮次边界与假 HTTP 响应长度

## 环境与目标

- 时间：2026-11-10，Asia/Shanghai。
- 平台：Windows，PowerShell，Python 本地 `ThreadingHTTPServer`。
- 源码基线：`cecf8489f3d922153699ee1d66422b8860669d26`，执行产物另含 v7d 的 31 源冻结 `da292832a59f11057f9dc63323907848df810c07e460ff1f826c40c3e537a6b6`。本子任务只改 e2e fixture，未改 C++ Core、CMake 或编译产物。
- 实际执行：`.cache/maiagent-core-gate/build-followup-20261105/bin/maiagent-console.exe`。
- 执行文件 SHA256：`b4c49563c447bd080c3c766faf3fdffd1d36133ebaeafc0ffddd0043027f03a9`。
- 原 fixture SHA256：`81ed918e11272222e9f776703ecefab4dbffdc44605963e8f3a3d28849b9a552`；原文件保存在 `.cache/context-compaction-20261110/console_e2e.pre-idle-sync.py`。

复现操作是通过 stdin 顺序驱动纯聊天、授权写文件、历史累积和再次写文件；预期每轮精确回答出现，并在本轮 `SessionIdle` 后提交下一轮。所有请求只到 owned loopback fake，数据库和文件均在本次新建临时目录内。

## 观察与定位

原失败记录 `.cache/context-compaction-20261110/windows-console-e2e-v7d.txt` 保留不动，其中 `third turn answered`、`second write finished` 失败，而后续文件及历史检查通过。独占重跑有时全部通过，因此不能仅凭该日志断言 Busy 或产品压缩回归。

### 1. 正文结束不是会话结束

`MaiTurnRunner::requestModel` 在 `model->stream` 内广播正文增量；之后才完成流、落库和 `finish`。`MaiAgent::Runtime::retire` 移除 active 再发 `SessionIdle`，控制台收到后输出提示符。原 fixture 在首轮正文和 note 收尾正文出现后立即继续，尚未等对应 Idle。

只把 fake 的 note 最后正文至 `[DONE]` 间隔改成 300ms，就观察到原 fixture 在正文后提前提交第三请求，并收到 `a turn is already running`。`third turn answered` 随后失败。加上启动提示符基线、首聊天与 note 轮的 Idle 屏障后，同一受控时序全部通过。此证明 fixture 存在独立同步缺陷，不宣称它单独解释了原日志中的两项失败。

证据：`console-e2e-v7d-held-original.txt/.jsonl`、`console-e2e-v7d-framed-held-fixed.txt/.jsonl`，均在上述 cache 目录。

### 2. 其余失败是 HTTP 接收错误

只增加 Idle 屏障后仍捕获到一次失败。失败时的原始屏幕不是缺少字面匹配：`Third one done.` 只显示 `T`，随后明确出现 `[error] curl: Failure when receiving data from the peer`，再进入 Idle。见 `windows-console-e2e-v7d-idle-sync-final-3.txt`。

独立 cache probe 每轮都等 Idle，不重置剧本：原先 HTTP/1.0 依赖连接关闭定界的 handler 在第 31 轮只显示 `f` 后发生相同接收错误；完全相同的逐字 SSE 内容只增加准确 `Content-Length` 后，100/100 轮通过。本次假服务 handler 修复后再次 100/100 通过。未增加等待时间、重试或忽略错误。

证据：`console-http-fixture-probe.py`、`console-http-fixture-unframed.txt`、`console-http-fixture-framed.txt`、`console-http-fixture-current-fixed.txt`。该对照定位到 fixture 的响应完成定界；未抓包，不进一步声称具体 TCP 关闭原因。

## 修改与验证

唯一产品树代码修改为 `MaiAgent/tests/e2e/console_e2e.py`：

- 给 fake SSE 响应声明精确字节长度，保留逐字符帧、flush、流式延迟和 interrupt 行为。
- 在首轮发送前建立启动提示符基线；首聊天与 note 轮完成后先等 Idle。
- 仅失败时输出 console tail，保留原业务断言以便下次定位。

验证均顺序执行，未与 Core ctest 并发：

- 离线四类响应（多字节文本、工具、思考、空答）：旧新 SSE body 与每次 write 的字节完全一致；新 header 等于实际 body 长度。`console-http-framing-bytes.py`：4/4 PASS。
- 当前 handler 独占 100 轮：PASS。
- 相同 300ms 收尾延迟的完整 e2e：exit 0。
- 未插诊断的正式 `python MaiAgent/tests/e2e/console_e2e.py`：exit 0，完整记录 `windows-console-e2e-v7d-framed-idle-sync.txt`。
- Root 独立复跑正式脚本：`windows-console-e2e-v7d-final-root.txt` exit 0、全部断言通过；之后仅将两行注释改成已实证的“接收错误”，执行代码未再变化。

临时目录与本次创建的进程由脚本清理；未访问用户数据库、GUI、线上服务或付费模型。这里验收的是所列 Windows console 构建与 fixture，不代替候选安装包实机验收。
