# MaiAgent 单进程运行时优化与 Codex 对照

## 范围与基线

2026-11-21，Darwin arm64；MaiChat main 基线
`3731101d52449d59b9aa1dba1e44d2f6620cf06f`，只读参照 Codex
`ac71f246052620472c2693d56a79ad5a1304f3d5`。
用户要求按前述六项架构差异逐项优化，并明确 Agent 核心坚持单进程。

保持 UI → 进程内 MaiAgent 库 → 模型/工具的正式路径，没有添加 app-server、
Agent 服务进程、IPC、MCP 或第三方依赖。既有外部命令工具的能力边界不扩大。
AGENTS.md 与 CLAUDE.md 同步写明用户确认的单进程约束。

## 六项改动和可复现预期

| 项目 | 实现 | 验收目标 |
|---|---|---|
| 轮次职责 | MaiTurnContext 固定会话/turn/型号/强度/目录快照；TurnPhaseChanged 描述准备、压缩、采样、工具、恢复、收尾 | 阶段不创建新 turn；同轮身份一致；先保存完成身份再发收尾进度 |
| 工具调度 | 显式 ParallelReadOnly、默认独占；file_read/glob/grep 只读查询允许并行；默认 4、硬上限 8 | 独立只读任务真实重叠；独占工具隔离；所有已启动任务返回后才继续/释放轮次 |
| 声明与执行 | 元数据不授予执行权限；整批 call ID 非空且唯一；参数容器须为 JSON object；仍逐次检查型号、目录、审批、付费 | 重复/空 ID 在任何工具启动前拒绝；畸形参数不执行；新响应可重用供应商 ID |
| 模型传输 | 每供应商/凭据复用客户端；独占 curl handle 租约，最多缓存 4 个空闲连接；可确认完整 HTTP 实体时正常结束并保留连接 | 服务端观察同一 TCP 来源端口；换 key 或退出后使用新连接；凭据仍逐次从宿主读取 |
| 上下文恢复 | 仅机器码确认容量拒绝；同一采样至多一次强制压缩；必须严格缩小才重发；沿用原史、工具配对和 CAS | 本地估计未达软阈值也可恢复；摘要断流继续同一预算；仍拒绝/无法缩小时停止 |
| 执行准入 | 写入失败时禁止进入工具；明确程序写法，不将任意路径/批处理剥成白名单名称；CR/C0/NUL/变量展开保守处理；普通审批查询与 shell 工具共用实现 | `/tmp/owned/cat`、`./git status`、伪装 .cmd、CR/变量展开不会自动批准；NUL 命令不启动 |

### 并发和失败语义

保持“模型响应完整成功后才执行工具”的屏障，未开放边收流边执行副作用。
这是有意保留的恢复契约，不宣称已实现 Codex 的流中工具启动。
只有工具主动声明 ParallelReadOnly 且本次无需普通/逐次/付费审批，才允许并行。
Never 审批模式也不把修改性工具升级为并行；默认/未知工具仍独占。

并行线程只运行工具并返回结果，不修改助手消息、不发布核心进度；轮次线程按
原调用顺序合并结果和图片、逐项持久化，然后才进入独占工具、下一次采样或收尾。
Stop/新输入阻止尚未启动成员；已经启动的只读任务需响应取消并协作返回，
不强杀进程内线程。工具异常转成有真实未知执行边界的错误结果，不伪称未执行。
恢复后携带原有结果，不重跑已完成批次；这不是工具副作用的全局 exactly-once 保证。

### 连接复用和图片缓存

仅凭缓存客户端对象不能证明复用了网络连接。旧实现每次 easy_init/cleanup，
且收到终止帧立即返回 WRITE_ERROR；新实现使用独占租约保留 curl 的连接/TLS 状态。
两个 wire 共用 HTTP 实体边界记录：只有未压缩且精确 Content-Length 已读完才
正常完成传输、复用活连接；未知长度/chunked/压缩不猜边界，仍立即结束。
异常/取消的未完成传输不按成功连接处理。没有引入 WebSocket 续接或隐式换供应商。

凭据回调在每次请求执行，空/无效凭据撤销缓存，新凭据与新客户端在构造成功后
一起替换；旧在途请求保留自己的 shared_ptr，不串到另一账号。
客户端缓存生命周期延长后，预览缓存按真实文件 identity、大小、修改时间校验，
生成期间改变的来源不缓存；最多 64 项。不改变的历史大图继续复用有界预览，
同路径同尺寸改写也必须重新准备。

### 权限边界

新增的 shell 普通查询 maiShellCommandRequiresApproval 与工具 requiresApproval
共用分类。它是执行前策略，不能替代系统沙箱、可执行文件内容验证或可信 PATH。
没有将工作目录称为沙箱，没有改动整个 App 的系统权限、付费审批或安装产物。

## 验证环境与命令

复用 `.cache/model-stream-recovery-20261121`：Release、Apple C++、
MAI_ENABLE_QUIC=OFF；测试模型仅监听本机，不使用真实账号/模型 Key、不触发付费任务。

```sh
cmake --build .cache/model-stream-recovery-20261121 --target \
 MaiRuntimeTests MaiCoreTests MaiInputQueueTests MaiStoreTests MaiToolExecutionTests \
 MaiModelClientTests MaiTurnRunnerTests MaiToolTests MaiFileSearchTests MaiPermissionTests \
 MaiContextCompactorTests MaiToolLoopTests MaiShellToolTests MaiContextBuilderImageTests \
 maiagent-console -j 3
ctest --test-dir .cache/model-stream-recovery-20261121 --output-on-failure \
 -R '^(MaiRuntimeTests|MaiCoreTests|MaiInputQueueTests|MaiStoreTests|MaiToolExecutionTests|MaiModelClientTests|MaiTurnRunnerTests|MaiToolTests|MaiFileSearchTests|MaiPermissionTests|MaiContextCompactorTests|MaiToolLoopTests|MaiShellToolTests|MaiContextBuilderImageTests)$' -j 3
MAI_CONSOLE_BIN="$PWD/.cache/model-stream-recovery-20261121/bin/maiagent-console" \
 python3 MaiAgent/tests/e2e/console_e2e.py
```

专项验证使用同步屏障而不是“创建线程数量”判断重叠，并检查结果配对顺序、独占
准入、取消排空、并行异常、工具完成后断流不重跑、阶段所属、受保护输入不循环。
容量测试含正常恢复、第二次仍拒绝、摘要断流后重试三种路径。
工具占位事件触发宿主撤销可用性时，启动前重新准入，两个调用均未执行且不崩溃。
HTTP 测试由假服务器记录真实 TCP 来源端口，核对复用、换凭据、退出后的重建。

负对照 `.cache/.../bin/MaiRuntimeTests --serial-control` 将并行上限设为 1，
相同“重叠且峰值为 2”断言失败并退出 1；正常上限 2 时通过。
新增缓存优化的早期整套回归曾发现历史大图 prepared 次数从预期 4 变多；
证据定位到残留的每请求 clear，已删除并保留“未变命中/改写失效”两条断言。

本机证据路径：
- `/tmp/maichat-runtime-full-tests.log`：首次完整回归及缓存失败证据。
- `/tmp/maichat-runtime-delivery-serial-control.log`：预期为红的串行负对照。
- 最终构建、测试、CLI 结果补充在下文。

## 最终验收与提交定位

- 最终构建：`/tmp/maichat-runtime-delivery-build.log`，退出 0。
- 全部 14 套回归：`/tmp/maichat-runtime-delivery-tests.log`，14/14 通过，40.52s。
- CLI：`/tmp/maichat-runtime-delivery-e2e.log`，72 项检查全部通过。
- 定向图片缓存：`/tmp/maichat-runtime-image-preview.log`，退出 0。
- 32 个改动 C++ 文件的完整格式检查通过；FileTools 只校验改动的三个声明，避免格式噪音。
  `git diff --check` 和 AGENTS/CLAUDE 一致性检查通过。
- 实际 CLI SHA-256：`4ac4e773f0d687f6aa7c9baff4113fb7995e353fdd24c9881510dc229e2cfac3`。
- 修复提交与本文同次保存，可用
  `git log -1 --format=%H -- MaiAgent/Agent/MaiToolCallValidation.cpp` 精确查询。

验证是在该次实现源码上完成；提交/推送源码不代表已更新用户安装的 App。

## 未验证边界

本次验收是 macOS 上新构建核心与 CLI 进程，未替换或重启已安装的 MaiChat。
iOS、Android、Windows、桌面 GUI 正常打包/实机、HTTP/3、真实模型账号及真实 IM
未验证。没有将本地测试通过表述为已安装 App 已更新，也没有发布新安装包。
