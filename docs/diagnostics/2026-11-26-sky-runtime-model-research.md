# Windows `@oai/sky` 运行时是否包含 GUI 模型：静态核对

- 时间：2026-11-26 16:42 CST（08:42 UTC）。
- 平台：本机 Windows x64；仅检查已安装文件，未启动 Computer Use、原生 helper 或模型请求。
- MaiChat 源码提交：`be9711aeac49e2be0c1c2f37069cc910ff90fc90`。
- 需求：确认 `@oai/sky` 是本地 GUI 执行接口，还是还包含独立 GUI 决策/视觉模型；区分主模型、辅助 OCR/视觉能力和操作系统执行层。
- 可复现操作：读取下列已安装运行时的包元数据、Windows JavaScript/类型声明；枚举包文件名；对两个原生可执行文件进行有限 ASCII 字符串核对；计算实际文件 SHA-256。
- 预期结果：给出可核实的调用链和证据边界，不将“未找到权重文件”当作“不存在任何模型”的证明。

## 结论

当前可读取的 `@oai/sky` 0.7.17 Windows JavaScript 实现是 GUI 观测/动作客户端与本地原生 helper 的连接层。它把 `get_window_state`、`click`、`scroll`、`set_value` 等显式动作转交给 helper，将返回的截图与 UI Automation 状态交给外层调用者；截图也会经 `nodeRepl.emitImage` 或 `codex.emitImage` 暴露给外层 Agent。没有在这些可读实现中找到“理解任务、选择 GUI 动作”的独立模型推理调用。

因此，有证据支持“外层 Agent 读取状态并决定动作，Sky 执行这些动作”的分层；没有证据支持“Sky 内置了另一个负责 GUI 决策的大模型”。但两个 Windows helper 是闭源原生产物，仅凭本次静态核对不能证明它们完全不含辅助模型、没有动态下载或没有远程推理路径。不能将此报告表述为“已证明 Sky 内部绝对没有模型”。

本次没有验证实际模型流量、服务端模型路由、任何真实 GUI 操作或 helper 运行时行为。

## 已安装产物与版本

通过 `Get-AppxPackage -Name '*Codex*'` 查询：

- AppX：`OpenAI.Codex`，`26.1120.5083.0`。
- 安装位置：`C:/Program Files/WindowsApps/OpenAI.Codex_26.1120.5083.0_x64__2p2nqsd0c76g0`。
- 已解包 CUA 运行时根目录（下文记作 `R`）：`C:/Users/18034/AppData/Local/OpenAI/Codex/runtimes/cua_node/9c26d707817e90a7`。
- `R/manifest.json`：`target=windows-x64`，`node_version=24.22.1-cua.1`，`node_binary_version=24.22.1`，`node_repl_path=bin/node_repl.exe`。
- Sky 包根目录（下文记作 `S`）：`R/bin/node_modules/@oai/sky`。
- `S/package.json`：`@oai/sky` `0.7.17`，主入口 `dist/project/cua/sky_js/src/index.js`，唯一声明依赖为 `@statsig/js-client ^3.32.6`；已安装 Statsig `3.33.3`。
- 同一运行时包括 `@oai/cua 0.2.6`、`@oai/cua-repl 0.1.3`、`@oai/browser-desktop 0.1.1`，以及 Playwright、sharp、jpeg-js、pngjs、pixelmatch、classic-level 等包。包名/依赖枚举没有发现常见 ONNX/Torch/TensorFlow 推理 SDK；这不是排除原生静态链接实现的证明。

实际文件校验：

| 相对 `S` 的文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `package.json` | 1,324 | `E2BF504D3054E3F16679A18026B28479E188E4411CF673416DF5006230CBCC7F` |
| `bin/windows/codex-computer-use.exe` | 1,548,592 | `FC0447EFB1AA5A0C527AE88C2ADA9FF7DDC398501BF451275218F1F866424BA8` |
| `bin/windows/swift/x64/codex-computer-use-swift.exe` | 56,674,608 | `591C434998764D0DD21F8B8CD558A34416944DED73762066758246C8636521FB` |

`S/bin/windows/swift/x64/manifest.json` 内嵌的 Swift helper 大小/哈希与实际文件不一致；本报告固定的是上表直接读取文件后计算的值。没有推断差异原因，也没有执行修复或替换。

## 可读调用链

下列源码相对目录统一为 `S/dist/project/cua/sky_js/src/`。这些分发产物大多压缩在第 1 行，因此用路径和类/方法名定位，避免复制专有实现正文。

1. `sky.js:1`：若存在可信 `nodeRepl.rpc`，导出的 `sky` 对象经 `sky_service_rpc.js` 的 `invoke` 发送 Sky 服务请求；在无 Node REPL 的场景才直接建立平台客户端。
2. `service.js:1`：可信服务创建具体平台客户端，并将允许的方法名与参数交给对应 JavaScript 方法执行。
3. `targets/windows/internal/computer_use_client.js:1`：Windows 客户端使用 `WindowsHelperTransport`；也支持可信 Node REPL 提供的 native pipe。普通窗口模式默认选择 `bin/windows/codex-computer-use.exe`。全桌面会话使用独立的 Swift helper 路径。
4. `targets/windows/internal/helper_transport.js:1`：普通 helper 由 `node:child_process` 懒启动，使用 stdin/stdout 的逐行 JSON 请求/响应；可信 native pipe 分支是本地管道，不是模型 HTTP 客户端。此处“本地传输”仅描述 JavaScript 到 helper 的连接，不表示 helper 不联网。
5. `targets/windows/internal/computer_use_client_base.js:1`：显式动作校验参数后转交 helper。`get_window_state` 返回截图数组、结构化 accessibility 状态；截图交给 `nodeRepl.emitImage` 或 `codex.emitImage`，使外层 Agent 可以观察结果。

`S/docs/skills/oai_sky_lib/windows/SKILL.md:14` 将执行机制描述为 SendInput、UI Automation 和 Windows.Graphics.Capture。其操作指南将状态读取视为昂贵的时间点快照，建议批量动作后再读取状态。

### `model` 字样的实际用途

`require_audio_model_support.js:1` 与同名 `.d.ts` 检查 `SKY_AUDIO_MODEL_ALLOWLIST`，从调用方 turn metadata 取 `model`，判断当前外层模型是否支持音频输入。`service.js:1` 在启动计算机音频录制前调用它。这是外层模型能力门控，不能据此认定 Sky 内置 GUI 模型或本地音频模型。研究没有读取实际环境变量值。

### 截图与输入契约

- `types/window2/Screenshot.d.ts:8`：`originX/originY` 为可选屏幕区域原点；`:12`、`:14` 指明 `width/height` 是 logical pixels。
- `types/window2/Click.d.ts:8`、`:10`：坐标为窗口相对坐标；`:12` 的 `screenshotId` 必须已缓存在目标窗口。JavaScript 对有限坐标取整后原样转交，没有展示 DPI/native-pixel 变换公式；无法从该 JS 推导闭源 helper 的坐标换算。
- `types/window2/Scroll.d.ts:11`：`scrollX<0` 向左、`>0` 向右；`:13`：`scrollY<0` 向上、`>0` 向下。类型/可读 JS 没有给出 delta 单位或到 Win32 wheel 单位的映射；不能将其直接断言为像素或 `WHEEL_DELTA`。
- `computer_use_client_base.js:1`：`click_count` 默认 1，要求有限、取整后 >=1；JS 没有设置上限。坐标点击与 `click_element` 是不同 helper 方法。闭源 helper 是否有更严格限制未验证。
- `types/window2/SetValue.d.ts:3`：输入为 `{window, element_index, value:string}`。
- `types/window2/PerformSecondaryAction.d.ts:3`：输入为 `{window, element_index, action:string}`；`:7` 描述动作名大小写不敏感，示例为 Raise、Scroll Up/Down/Left/Right、Expand、Collapse。JS 检查非空后转交动作字符串；实际 UIA 行为在原生端。
- `types/window2/GetWindowState.d.ts:4`：`include_text` 默认 false；`:6`：`include_screenshot` 默认 true。JS 拒绝两者同时 false。

### 可核实生命周期

JavaScript 建立客户端/transport 时不立即启动 helper，首个请求才启动。窗口模式公开契约中没有 `start_capture/stop_capture`。客户端的 turn-ended handler 发送 `end_turn`；共享 transport 按引用计数关闭；transport 关闭时先发送 `close`，失败时结束子进程。

独立全桌面模式包含 `desktop_get_screenshot`、`desktop_close`、`desktop_cancel` 等原生方法名，不能将其当作普通窗口模式的生命周期。JS 不能证明 WGC frame pool/capture session 是否跨快照保持，也不能证明每次快照后已释放图形资源。

## 文件与原生字符串核对的结果和限制

- `S` 枚举到 263 个文件，共 59,210,435 字节。整个运行时 `node_modules` 的文件名检查没有找到 `.onnx/.safetensors/.gguf/.pt/.pth/.tflite/.mlmodel/.weights` 等典型独立模型文件，或 Tesseract/ONNX Runtime 等名称。
- 对两个 helper 的 ASCII 字符串检查可见 UIAutomation、SendInput 和 WinHTTP 相关符号。Rust helper 可见 Windows.Graphics.Capture 相关名称；Swift helper 可见大量 UIA 调用名称，例如 ValuePattern 和 TextPattern。
- 目标字符串 `onnx`、`tesseract`、`Windows.Media.Ocr`、`OcrEngine`、`Windows.AI`、`DirectML`、`safetensors` 未命中。Swift 二进制中的一次无边界 `gguf` 子串命中位于非文本数值字节附近，不能当作 GGUF 权重或加载器证据。
- 两个 helper 均含 `https://api.openai.com/auth` 字符串，Rust helper 附近另有 `chatgpt_account_id`；没有将它解释为推理接口或实际网络请求证据。目标 ASCII 字符串 `/v1/responses` 和 `/chat/completions` 未命中，同样不能据此排除动态构造的请求。
- Sky 的 Mac telemetry JavaScript 明确引用 Statsig 与 ChatGPT telemetry 端点；它属于已分发多平台 JS，而本次目标平台是 Windows。依赖中有网络客户端并不等于存在 GUI 推理模型，也不能声称 Sky 整体完全离线。
- 本次没有完整反汇编、导入表审计、UTF-16 全量字符串分析、运行时网络抓包或动态加载监测；没有启动 helper 验证实际行为。

所以，“未发现常见权重、SDK 或模型 URL”只能作为有限的支持性观察。闭源 helper 可能采用静态链接、嵌入资源、系统 API、动态加载或动态网络地址，静态表面检查不能排除这些情况。OCR 即使存在，也应与决定下一步 GUI 动作的模型分开讨论；本次并未证实 Sky 包含 OCR 模型。

## 安全与变更边界

未读取账号/令牌配置、运行会话、用户对话、环境变量值或带凭据的进程命令行；未安装依赖、调用计算机控制、发出模型请求或修改产品文件。仅新增本诊断记录。未完成真实流量或实际 GUI 的验收，不将静态检查表述为实机验收。
## 关于 Codex 外层审查的补充

公开 Codex 0.172.0 源码的 `codex-rs/core/src/context/node_repl_review_evidence.rs` 会按模型的
`computer_use_review_required()` 和 feature 配置选择 REPL 的多模态、纯文字或关闭审查证据模式；
`codex-rs/protocol/src/openai_models/guardian.rs` 也定义了 ComputerUse 审查 scope。
这与 Sky 本地库是否内置 GUI 决策模型是两件事。它表明客户端外围还有可配置的动作审查流程，
不能据此推断 Sky 自身打包了另一个 GUI 模型，也不能把所有辅助审查都当作主模型的界面判断。
本轮没有抓取真实请求，未确认当前用户运行配置是否启用这些流程或由哪一个模型提供审查。
