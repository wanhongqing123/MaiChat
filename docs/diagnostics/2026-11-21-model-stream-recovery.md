# MaiAgent 模型断流恢复

## 需求、复现与预期

2026-11-21，macOS arm64，MaiChat `main` 基线
`117b48a18be7367787918ee97d3c0990869e9a1f`。用户要求按 Codex 的 turn 级恢复思路，
打通 MaiChat 模型断流重试。本记录与实现同次提交，未生成发布包。
提交前已快进纳入远端 `cdf82acffde9565ae4cec88e4ca37db75d76bbef` 的发布文档更新，
该提交未改变本次验收的程序源码。修复提交可用
`git log -1 --format=%H -- MaiAgent/Agent/MaiModelRetry.cpp` 精确查询。

可复现步骤：启动只监听本机的 Responses SSE 假服务，第一次响应输出
`FAILED_ATTEMPT_PRIVATE_DRAFT` 后正常关闭 HTTP，但不发送 `response.completed`；
第二次响应返回完整 `RECOVERED_FINAL`。通过真实 `MaiAgent::submit(MaiSendPrompt)` 发起。
预期在原会话、原 turn 内恢复，最终完成一次正文提交；第一次的半截文本仍可回查，
但不能进入下一次模型请求；失败响应不能启动工具。

修改前在独立 Release 构建运行新增失败用例，退出 1：HTTP 仅请求一次、
出现 SessionError、没有成功正文提交，也没有最终回答。修改后相同 EOF 场景通过。
后续扩展到 Chat Completions、输出后停滞、流内 server_error 等场景。

## 原因证据

- 两个 HTTP 适配器仅在部分传输/5xx 错误且 `!sawOutput` 时内部重试。
  传输/模型进度超时、缺终止帧提前返回，不经过同一恢复分支。
- 429 只分类为 RateLimited；Agent 没有读取结构化建议、重建历史的恢复循环。
- TurnRunner 收到模型错误后直接收尾；草稿保留，但没有持久化的未确认身份，
  以后构造模型上下文或摘要时可能把半截正文当作普通历史。
- 本实现中普通工具在模型响应完整成功后顺序执行。不能通过重新启动工具循环，
  或把失败响应中捕获的调用直接执行，来模拟网络恢复。

## 实现与约束

1. `MaiError` 增加 typed `MaiRetryAdvice`。HTTP 适配器基于传输事实、状态码、
   明确协议错误码给出恢复建议，不从人类错误文案猜测。`MaiModelRetry` 共用退避、
   Retry-After 解析与取消等待；无新增第三方依赖。
2. `MaiModelRequest::callerManagesRetries` 指定唯一恢复所有者。
   Agent 的主采样、工具后继、无工具补答、压缩摘要由 TurnRunner 统一计数；
   普通直接 stream 调用保持只恢复无输出请求的契约，避免两层预算相乘。
3. 每次恢复重建上下文，消费已接受的补充输入，带回原工具调用和结果。
   退避期间检查取消/补充；消费补充不会重置或绕过服务方等待截止时间。
   等待剩余毫秒向上取整，避免提前结束；恢复期间的新指令同时清除旧计划的工具重复计数。
4. 每个模型采样的文本/推理从首次落库起标为 `modelDraft`，成功持久化后确认。
   失败/取消草稿保留在本地，直接上下文和摘要来源均排除。
   SQLite 仅对 text/reasoning 的原空闲 state 列保存此位，不迁移 schema；
   tool 的状态及 kind=2 恢复查询不变。false 保留旧检查点哈希，true 纳入来源指纹。
   没有元数据的既有历史不会被事后猜测为失败草稿。
5. 失败响应的调用不派发，恢复层不重新执行旧工具批次。
   这不是全局 exactly-once 承诺：新的成功模型响应仍可能请求一个新的工具调用，
   按现有工具/审批规则执行。
6. 成功持久化后才发布 MessageTextCommitted。重连使用独立 ModelRequestRetrying，
   不重复发布 turnStarted，不提前 idle，不伪造模型正文。
   Console 仅作为同一核心库的独立可选入口显示该状态；MaiChat App 不会启动它，
   重试在原 App 进程的工作线程内运行。
7. 默认两次额外恢复；初始 250ms 指数退避、上限 60s；有效 Retry-After 可延长。
   支持连接/接收/发送错误、408/429/5xx、超时、缺终止帧和明确临时流错误码。
   401/403、402、已知硬额度、输出长度/内容过滤、未知协议错误不自动恢复；
   不更换模型、供应商或免费/付费路由。持久化错误不按网络错误重试。

这是从最后确认历史重新采样，不是逐字节续传。原始私有 Responses 续接状态
只在完整成功后回放，不用半截可见推理伪造签名或加密状态。

## 构建与验收

独立目录：`.cache/model-stream-recovery-20261121`，Darwin arm64、Apple C++、
Release、`MAI_ENABLE_QUIC=OFF`，仅仓库已供应的依赖。

```sh
cmake -S MaiAgent -B .cache/model-stream-recovery-20261121 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMAI_ENABLE_QUIC=OFF
cmake --build .cache/model-stream-recovery-20261121 --target \
  MaiModelClientTests MaiToolLoopTests MaiStoreTests MaiContextCompactorTests \
  maiagent-console MaiCoreTests MaiTurnRunnerTests MaiInputQueueTests \
  MaiToolExecutionTests MaiContextBuilderImageTests -j 3
ctest --test-dir .cache/model-stream-recovery-20261121 --output-on-failure \
  -R '^(MaiModelClientTests|MaiToolLoopTests|MaiStoreTests|MaiContextCompactorTests|MaiCoreTests|MaiTurnRunnerTests|MaiInputQueueTests|MaiToolExecutionTests|MaiContextBuilderImageTests)$' -j 3
MAI_CONSOLE_BIN="$PWD/.cache/model-stream-recovery-20261121/bin/maiagent-console" \
  python3 MaiAgent/tests/e2e/console_e2e.py
```

新回归覆盖真实 HTTP 两种 wire 的 EOF、半截正文后超时、流内临时错误、429 恢复、
503 耗尽、硬额度/鉴权/402 不重试；工具执行后断流时保留原 call/result、旧工具只执行一次、
失败调用不执行；单 turn 生命周期/提交事件、取消、补充输入、等待期限、SQLite 重开、
上下文和摘要隔离、摘要断流传回同一预算。已有免费主模型测试继续验证不读付费凭据、
不访问付费路由。原先“超时/429 绝不重试”的断言按新的显式预算更新，永久错误仍不重试。

测试日志保存在本机以下位置；red-build 为修改前测试构建日志，
红测试的四项失败观察记录在本文“需求、复现与预期”中：

- `/tmp/maichat-model-recovery-red-build-20261121.log`
- `/tmp/maichat-model-recovery-input-build-20261121.log`
- `/tmp/maichat-model-recovery-ctest-delivery-20261121.log`
- `/tmp/maichat-model-recovery-e2e-delivery-20261121.log`

最终验收：上述九套单元测试 **9/9 通过**（40.30s），
CLI 端到端 **all checks passed**。新增工具重规划用例也在最终工具循环套件中通过。
23 个改动 C++ 文件的 clang-format dry-run 通过，`git diff --check` 通过。
验证采用本次提交所含源码编译的产物；提交及推送源码不代表已更新安装的 App，
本次未打发布包。

实际 CLI 产物 SHA-256：
`90a91fa1d369ee7005fe72d594862199fab8349c04f618c3463ffee3fc74c3c9`。
该程序直接静态链接新构建的 `maiagent` 核心，未调用用户已安装的 MaiChat。

## 未验证边界

本次实际执行的是本机新构建的核心测试与 CLI 进程，未替换/重启已安装的 MaiChat。
未做 iOS/Android/Windows 或桌面 GUI 打包、真机交互、HTTP/3、真实账号/真实模型服务验收；
未发生付费请求。测试通过不能推断用户正在运行的 App 已更新。
已有 UI/IM 消费者的协议不变；CLI 显示具体重连次数，其他宿主沿用现有活动轮次显示，
未新增各端重连文案界面。未自动修改、停止或删除用户应用/数据/缓存。
