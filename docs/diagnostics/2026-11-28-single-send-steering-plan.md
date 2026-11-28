# 单发送入口与即时补充输入

> 执行方式：按 writing-plans、test-driven-development 与 subagent-driven-development 组织，直接在 main 开发；用户已确认下述简化交互，不再要求二次审批。

**目标：** 保持现有单发送按钮；空闲开始任务，忙碌输入补充当前任务，由共享核心在正确边界合入。

**架构：** `MaiAgent` 持有轮次取消和独立的单次采样取消注册；输入先落库后只中断采样。`MaiTurnRunner` 等同步 stream 返回后保留已完成原生 item、隔离草稿/未准入调用，消费原有 FIFO 输入并重新构造请求；工具仍使用真正任务取消信号。

**技术栈：** 既有 C++17、atomic/shared_ptr、Runtime→steeringMutex 锁序、Responses 与本地模型的现有 stream 取消契约；不新增库。

## 已确认的范围

用户明确拒绝增加按钮、Tab或长按学习成本，并同意“只正常发送，空闲新任务、执行中补充，系统暂存并在交接点交给模型”。本次不暴露明确下一轮模式，也不把队列内容交给模型猜分类。

普通已执行工具保留真实结果交接点；不把新输入接到 Shell 的 task cancel，避免误杀命令。没有独立 sleep 工具，shell sleep 是进程；本次不为等待工具引入新的 yield 协议。现有 Retry-After 截止约束保持。

## 任务与验收

- [x] 测试先行：`MaiAgent/tests/MaiInputQueueTests.cpp` 的被阻塞真实 Runtime+fake stream 不手动 release，分别验证 durable enqueue 与显式 steer 会触发单次采样取消。原实现两项 `CHECK(canceled)` RED，日志在 `.cache/steer-input-20261128/tests/`；生产实现/头摘要未变。
- [x] Runtime 注册：`MaiAgent/include/MaiTurnRunner.h` 增加内部依赖回调：

  ```cpp
  std::function<void(const std::shared_ptr<std::atomic_bool>&)> beginSampling;
  std::function<bool(const std::shared_ptr<std::atomic_bool>&)> endSampling;
  ```

  `ActiveTurn` 保存当前注册；begin/end 与输入接受同用 steeringMutex。begin 初始化于任务取消或已有 pending 输入，end 原子捕获该请求的取消并仅撤销同一注册。输入落库、回执成功后置该信号；Stop/析构置真正任务取消及该注册，统一 Runtime→steering 锁序。
- [x] Runner 接收：`MaiAgent/Agent/MaiTurnRunner.cpp` 在 emit Sampling 前注册；仅 stream 接收请求信号，其他上下文/工具仍接任务信号。正常返回和异常都撤销注册。注册期间收到补充导致的 Canceled 不发布 TurnAborted，不消耗错误重试预算；等旧 stream 退出后重新消费输入与请求。
- [x] 输出安全：先归一化取消，再决定草稿晋升。已完成 item 及已收到 usage 保留；旧调用不执行，已发布但不再准入的 continuation 载体撤销。协议与存储错误不得被补充输入吞掉；用户 Stop 优先。
- [x] 核心回归：生成中补充/多条FIFO/同轮终态、Stop及析构、运行工具不被中断、旧未启动调用/不完整参数不执行、完成item与草稿隔离、收尾竞态、不同会话隔离。明确测试屏障，不通过手动放开旧 sampling 伪造通过。
- [x] 正常构建验证：MaiInputQueueTests 与相关 TurnRunner/Runtime/ToolLoop/Responses/存储测试，以及规定八套核心；触及控制台行为时重跑 console e2e。正常 Windows App 与移动 JNI 重新编译；实际独立候选 staging/ZIP 裸 PATH smoke 通过，不动用户现装程序或已发布4.0.2原包。Header 后续聚合另跑桌面定点与候选启动，不重复无关 Core 套件。
- [x] 独立规格与代码审查、更新对照文档/诊断；未实机平台说明边界。源码提交与推送以 Git 记录为准，未重新发布正式包。

## 计量边界

定制 Codex 有 upstream drain/计费归属等待，当前 MaiChat 的同步 curl stream 没有等价 owner。最小安全边界是旧 stream 返回后才启动后继请求；只保留供应商实际返回 usage，未收到不能造零或宣称计费完全对齐。此边界在实现和验收记录中明确保留。

## 实施与验证记录

Root 基准 `6adfe4c02`，Windows 正常 MSVC lane 构建共享 Core、桌面 App 和控制台；
Android arm64-v8a 正常 JNI 编译通过，未安装到设备，不声明移动端新功能实机验收。
12 套最终 Core/Controller 回归通过，日志与机器回执为
`.cache/steer-input-20261128/final-core-02-{build,test}.log` 和 receipt，测试总耗时 69.89 秒。
正常 App/CLI 构建与 console e2e 通过；全程无供应商请求。

新增测试使用真实 Runtime/存储及可控模型屏障，确认取消信号来自接收输入，
不靠手动放开旧 stream。已完成原生 item/未知字段由自有 rollout 回放核对，
未完成正文/思考仍为草稿，旧未准入调用不执行。Stop 及 steer 后 Stop 保留暂停队列，
运行中的普通工具不收到采样取消。最终无工具回答和空响应 fallback 期间仍可补充。

独立审查发现并修正一次错误来源混淆：存储返回 Canceled 不能被当作普通 steer 重试；
同一测试先两项失败、修复后通过。空响应收尾提前封闭接收先九项失败、修复后通过。
两组红绿证据在 `audit-tests/`；原始输入取消红绿在 `tests/`，旧测试观察草稿/完成项的
夹具问题已按真实生命周期修正，没有禁用事件回调的慢操作守卫。

构建验证另发现基础设施问题：CMake 缓存 CP936 include 前缀，后续 cl 输出 UTF-8，
旧 normalizer 只识别完全相同字节，导致 Ninja 对两个 Core 与四个栈分配消费者漏记头依赖。
这造成中途 ABI 混编的四套崩溃，不能算产品回归通过。修复仅对前缀严格转换 UTF-8/系统 ACP
等价表示，路径/诊断原字节与编译退出码保持；真实 C/C++ 头文件变更用例先 `#deps 0`
再成功自动重编。六消费者重编后均记录 `MaiTurnRunner.h` 依赖，最终 12 套全通过。
正常注册测试 `msvc_include_dependencies_test` 也通过，未改生成 Ninja、系统语言或 SDK。

独立 Windows 候选按正常 package 脚本生成，经 PE 依赖门禁后分别从 staging 和实际 ZIP
解压目录启动 `--smoke`，裸 PATH、独立 profile、实际 exe/PID/Qt 模块路径精核。
早期候选脚本两次路径假设错误分别是 PowerShell Join-Path 不接绝对 OutDir、ZIP 为平铺根目录，
均保留原失败；修正为相对 OutDir 和实际根目录后两 copy 成功，不把脚本错误称作产品错误。
Header 合并后候选重新生成并单列最新回执。公开 4.0.2 包和用户现装未替换。

最终 Header 聚合候选回执为 `candidate-windows-03/receipt.json`，staging/实际 ZIP
两次裸 PATH smoke 退出码均为 0；正常构建 exe 与两 copy SHA256 同为
`7d9097e7978bd4f737bce056bfd70c4e72dd2f4921f795a1c3d2e1b0aa682289`。
`final-source-build-inventory.json` 逐字节确认 Root 的全部 14 个修改源码/测试/构建脚本
与正常构建输入相同。私有构建 checkout 基准仍为 74470f39，叠加这些已核输入；
Root 基准 6adfe4c02 相对 74470f39 在 MaiAgent/MaiChat 产品树无差异。
因此这是有源码摘要约束的独立候选验证，不称为干净 final commit 的正式发布构建。
