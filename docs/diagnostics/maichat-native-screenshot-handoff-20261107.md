# 创作中心原生截图与同文件交付排障记录（阶段 A）

本文记录阶段 A：修复现有截图的同文件交付和能力宣告。用户随后新增阶段 B 要求：常驻屏幕采集，模型侧只保留屏幕采集并移除窗口采集工具。本文的窗口截图 fixture 仅是阶段 A 正确性证据，不能视为常驻采集或 screen-only 设计已经实现、验收。

## 现场需求与复现操作

- 时间：2026-11-07，Asia/Shanghai（UTC+08:00）；本轮 Windows 日志主要覆盖 10:10 至 10:27。
- 用户明确使用桌面创作中心的 DeepSeek 云端模型，反馈 Agent 每次自行编写 shell/C# 截图程序，并表示无法查看截图。
- 操作：进入创作中心会话，要求截取指定窗口、查看画面并在当前回复展示该截图。
- 预期：调用原生 `screenshot` 或 `desktop_computer.observe`，将截图像素交给当前模型，并通过 `agent_send_media` 展示同一份文件；不重新编写脚本截屏。

主任务未取得现场原始供应商请求，因此不把当前源码默认值 `deepseek-flash` 当作现场请求实际 API 模型 ID 的证明。用户已经明确使用 DeepSeek，不能把本次问题归因于 `glm-5.3`。

## 源码、构建与运行身份

| 对象 | 本轮身份与边界 |
| --- | --- |
| 源码基线 | `main`，`9cd8726778530d6652b00eb8be91a521639ce995`，叠加本轮暂存实现和测试；本文写入时尚未取得最终提交 SHA |
| Windows Desktop 构建目录 | `E:/OpenSource/MaiChat/MaiChat/desktop/.cache/build-release-62111144`；本轮相关目标日志为 `final-desktop-build.txt` |
| Windows 测试环境 | Windows x64，Qt/QtTest 5.15.2，Windows QPA；构建日志使用 MSVC 14.44.35207 工具链 |
| Core 构建 | `E:/OpenSource/MaiChat/.cache/maiagent-core-gate/build-followup-20261105`，VS2022 / MSVC 14.44；`core-green-build.txt` 记录截图工具与模型客户端目标构建，`core-required-build.txt` 记录另外九组目标链接，最终回执为 `core-required-results.json` |
| 实际运行产物 | 测试运行上述构建目录中的独立测试 exe；正式应用相关构建完成了 runtime PE 依赖检查，`product-integration-build.txt` 记录 3 binaries、119 PE imports 和 Qt plugins 验证 |
| 现装应用 | 主任务核对仍为 `D:/Program Files/MaiChat` 下的 MaiChat，PID 584；本轮未停止、替换或安装该应用 |
| 日志与证据根目录 | `.cache/screenshot-handoff-20261107/`，以下日志名均相对此目录 |

Windows 运行验证对应本轮构建的宿主测试及真实原生捕获链，不是现装 PID 584 已经更新的证明。没有发布、安装或覆盖公开版本。

## 定位证据与根因边界

### 同一截图无法直接交付

原 `MaiAgent/AgentTools/MaiScreenshotTool.cpp` 将 PNG 保存到系统临时目录，但工具文本只返回捕获对象和尺寸。实际路径只存在于内部 `MaiToolImage` 附件。

`MaiAgent/Agent/MaiContextBuilder.cpp` 原样回灌工具文本，图片观察消息仅写 `Image returned by the preceding tool.`，没有另向模型暴露文件路径。`agent_send_media` 的既有契约要求工作区相对路径，并使用 `maiResolvePathWithinRoot` 拒绝越界。因此，即使只把临时文件绝对路径追加到文本，仍不能直接交付。

`desktop_computer.observe` 原来已经把窗口 PNG 保存到工作区 `computer/`，但输出 JSON 同样缺少引用该文件的路径。现场助手曾表示“内置截图只能内嵌给模型，没有可引用文件，因此需要自己截图再发送”，与上述交付缺口吻合。

这是同文件交付受阻的确定证据，不能据此断言原生截图像素没有回灌。原来已有 `successWithImages` → 持久化图片片段 → ContextBuilder 图片观察 → 供应商多模态请求的正常链路。反过来，shell 输出一个图片文件路径，也不会自动把那个文件变成模型图片输入；仍需实际图片附件或 `view_image`。

### DeepSeek 图像能力与模型宣告

主任务核对 DeepSeek 官方 [Vision](https://api-docs.deepseek.com/guides/vision/) 和 [Responses API](https://api-docs.deepseek.com/guides/responses_api/)：当前 `deepseek-flash` 支持图像输入、Base64 data URL 及 Responses `input_image`，不要求先上传为公网图片 URL。本轮没有给 DeepSeek 增加禁用规则。

另有独立的模型宣告不一致：`view_image` 已对 `glm-5.3` 与 `local:*` 隐藏，但 `screenshot` 仍向它们宣告，执行时才部分拒绝。该边界需要修正，但它不是用户明确使用 DeepSeek 时的根因。

### 输出目录边界与取消

测试还发现两个必须在捕获前处理的边界：截图目录可能是指向工作区外的链接；`desktop_computer.observe` 创建输出目录失败或已经取消时，不应继续捕获、写图或回传成功。

Windows 当前 `MaiFileSystem::resolve` 对不存在的最终文件使用词法归一化，不能只检查一个尚未创建的 `screenshots/<id>.png`。本轮先独立解析截图目录，再在目录创建、捕获和写入阶段复核目录与最终路径，沿用既有路径保护接口，没有修改公共文件系统 API。

## 修复行为

- `screenshot` 保存到会话工作区 `screenshots/`，返回 JSON：`image_path`、`type=image`、`mime_type=image/png`、`width`、`height`、`mode`。`image_path` 为工作区相对路径，图片附件仍引用该文件的绝对路径。
- 工具说明及桌面系统提示明确原生截图优先、直接检查返回图片，并将返回的 `image_path` 作为 `agent_send_media.file_path`，复用同一文件。
- `screenshot` 的模型可用性与 `view_image` 一致：`glm-5.3`、`local:*` 不宣告，直接执行也返回不支持；DeepSeek 保持可用。
- 截图前检查工作区、目录及输出路径；缺失工作区、目录失败、目录链接越界和取消均返回对应错误。
- `DesktopComputerController::observe` 返回同一窗口 PNG 的相对 `image_path`、`mime_type`，补充目录校验和同步取消回调；普通调用与工具调用继续复用相同核心实现，回调不保存在会话状态中。
- 保持 `agent_send_media` 的工作区保护，未增加接受任意临时绝对路径的旁路；普通原生截图 API 不变。

## 真实 RED 与 GREEN

### RED：先验证新增断言能抓住缺陷

| 日志 | 实际失败 |
| --- | --- |
| `core-screenshot-contract-red.txt` | 7 项失败：模型可用性/直接执行 3 项，注册宣告 2 项，工作区缺失 1 项，目录链接越界 1 项；主任务记录 exit 1 |
| `core-screenshot-owned-red.txt` | 上述 7 项之外，实际自有窗口截图回执 `output.is_object()` 再失败 1 项；主任务记录 exit 1 |
| `desktop-contracts-red.txt` | 1 项失败：窗口观察回执缺少相对图片路径 |
| `desktop-boundary-red.txt` | 3 项失败：目录链接越界仍捕获，且在外部目录写入文件 |
| `desktop-review-red.txt` | 5 项失败：输出目录失败仍捕获，以及取消后未返回 Canceled、仍附图/落图 |
| `controller-model-red.txt` | 3 行文本模型截图宣告失败；`deepseek-cloud-image-capable` 当时已经 PASS，不能把 DeepSeek 禁用作为修复 |

### GREEN：Core 与本地 HTTP 回灌

| 项目 | 已核对结果与范围 |
| --- | --- |
| `MaiScreenshotToolTests --request-validation-only` | `core-screenshot-contract-green.txt`：`screenshot tool tests passed`，主任务核对 exit 0；未触发实际屏幕捕获 |
| `MaiScreenshotToolTests --owned-window-only` | `core-screenshot-owned-green.txt`：同样通过，主任务核对 exit 0；只截测试进程自建窗口，排除整屏捕获 |
| 截图同文件契约 | 临时中文空格工作区；核对 JSON 相对路径、附件绝对路径、PNG 实际尺寸，并调用真实 `makeMaiAgentSendMediaTool` 使用同一相对路径交付成功；两个截图 GREEN 日志没有目录链接跳过提示 |
| `MaiModelClientTests` 全套 | `core-model-images-green.txt`：`llm tests passed`，主任务核对 exit 0；包含本轮真实 `view_image` → Agent 工具循环 → 两次本地 HTTP 请求测试，两种协议均执行 |
| HTTP 字节断言 | 第一请求没有图片；第二请求核对调用/回执配对及图片观察顺序，分别检查 Chat Completions `image_url` 与 Responses `input_image`，解码实际 data URL 并逐字节比较独立构造的有效 PNG |

HTTP 测试通过 `FakeServer` 的真实 POST handler 保存每轮请求体，严格要求两轮请求。输入文件在 Agent 退出后删除；服务器停止并 join，网络请求设置超时，工具循环有上限。独立只读复核确认 PNG 的 IHDR/IDAT/IEND CRC 及解压数据有效。

这证明真实工具、Agent 和序列化后的图片字节能够连通，不是 DeepSeek 在线识别准确率测试，也不代表已经运行了付费模型。

### Core 十组 required gates

最终 `core-required-results.json` 已完整写入，**10/10 全部 exit 0**。`MaiModelClientTests` 在最终 Core 修改后提前完整实跑，同一最终 Core 输入的另外九组随后在 required gate 中执行；没有将未执行的组计为通过。

| 测试 | 退出码 |
| --- | --- |
| `MaiModelClientTests` | 0，`core-model-images-green.txt`，含两种本地 HTTP 图片协议 |
| `MaiCoreTests` | 0 |
| `MaiFilePathTests` | 0 |
| `MaiStoreTests` | 0 |
| `MaiTurnRunnerTests` | 0 |
| `MaiToolTests` | 0 |
| `MaiToolLoopTests` | 0 |
| `MaiPermissionTests` | 0 |
| `MaiQuestionTests` | 0 |
| `MaiInputQueueTests` | 0 |

其余九组日志为各测试名对应的 `.txt`；回执逐项记录实际日志及执行方式。

### Windows 创作中心宿主与原生截图

`product-native-result.txt` 记录 `DesktopScreenshotIntegrationTest`：**3 PASS、0 FAIL、0 SKIP**，808 ms。这里的 3 项是 setup、`nativeCaptureReturnsToCreationCenter` 一个测试槽和 cleanup，不是三个独立业务场景。

该用例使用生产 `AgentController`、`AgentChatPanel`、原生 `screenshot`、WGC 捕获和 `agent_send_media`，只创建并截取自有隔离窗口。模型客户端是本地脚本，名称设为 `deepseek-flash` 用于验证实际请求模型和工具宣告；没有向 DeepSeek 发送模型请求，没有计费，没有调用 shell，也没有向其他应用注入系统键鼠。

最终回归的 `final-product-native/screenshot-transcript.json` 记录：

- backend：`production_native_window_screenshot`；3 次脚本模型请求。
- 第一工具为 `screenshot mode=window`，回执 `image_path=screenshots/<id>.png`，480 × 320。
- 模型图片附件与已保存原生 PNG 为同文件；PNG 为 6325 bytes。
- 第二工具 `agent_send_media` 使用同一个 `file_path`，交付回执 `delivery=current_ai_session`，类型、路径及 6325 bytes 一致。
- 创作中心实际显示图片媒体卡片；主任务已查看本地保存的原生窗口图与交付界面图。

证据保存在 `final-product-native/native-owned-window.png`、`creation-center-delivered-image.png`、`owned-fixture-widget.png` 和上述 transcript。先前一次相同用例的证据另存于 `product-native/`，不要混用两次捕获的文件尺寸和哈希。

### Desktop 受影响回归

`final-desktop-regression.txt` 已结束：**8/8 PASS，0 SKIP，总耗时 67.20 s**。

| 测试 | 结果 |
| --- | --- |
| `desktop_session_sync_host_test` | PASS |
| `desktop_computer_contracts` | PASS |
| `desktop_computer_agent_integration` | PASS，指定确定性脚本槽，无系统输入 |
| `desktop_screenshot_integration` | PASS，真实自有窗口原生截图 |
| `agent_controller_test` | PASS，含 DeepSeek 与文本模型宣告行 |
| `agent_panel_session_test` | PASS |
| `main_window_layout_test` | PASS |
| `ui_zoom_test` | PASS |

### macOS 同伴反馈

主任务已读取同伴对 72434-byte review patch 的最终 Markdown 报告、执行回执和源码 blob 清单，逐项核对 10/10 源文件与本机暂存输入一致。以下均区分为同伴执行结果，本文没有在 Windows 上重新执行 Mac 构建或测试：

- 7 个 Desktop 与 2 个 Core 目标编译通过。
- contracts 与 controller 的 CTest **2/2 PASS**；Controller 的 QtTest 汇总为 **22 PASS**。
- `MaiScreenshotToolTests --request-validation-only` 退出 0。
- `MaiModelClientTests` 全套退出 0，用时 72.28 s。
- 新 `DesktopScreenshotIntegrationTest` 在 Cocoa 与 ScreenCaptureKit 下实际截取自有窗口，**3 PASS、0 FAIL、0 SKIP**，截图 960 × 640；仍是 setup、一个业务槽、cleanup，不是三个独立业务场景。
- 同伴与主任务均已分别查看原生截图及交付画面：仅包含自有橙蓝 nonce 夹具与创作中心图片卡片，没有用户桌面画面。
- 未运行系统键鼠 native 验收，现装 Mac PID 96951 未替换。

这些结果补充阶段 A 的 Mac 编译、HTTP 回灌与真实原生窗口截图证据；不证明 DeepSeek 在线识别、常驻屏幕采集、screen-only 工具设计或系统键鼠操作已经验收。

## 尚待完成的验证与交付边界

- Core required gates：10/10 全部 exit 0，已核对完整 `core-required-results.json`；Desktop 8/8 已结束。
- macOS：最终报告、回执及源码身份已核对；9 个实际编译产物由同伴按 SHA256 归档。主程序 SHA256 为 `f3cee10c84aaa32b40e15317fb516d6effb17bfdd43bf1d42b994fe2265ca598`，证据根目录为 `/private/tmp/maichat-screenshot-handoff-mac-20261107-2979588d`。
- DeepSeek 云端视觉：尚未使用用户现场同一原始请求进行真实识别复现；当前证据覆盖工具文件交付、原生捕获和本地 HTTP 图片字节。用户实际使用模型/API ID 仍需原请求证据核对。
- 系统键鼠控制：本轮截图通过不补齐此前原生输入验收；该项仍 pending，详见 `maichat-desktop-computer-control-20261107.md`。
- 现装应用、安装包与发布：Windows PID 584、Mac PID 96951 均未替换，未安装、未发布；不能把源码和构建宿主测试通过描述为现装应用已经修好。
- 阶段 B：常驻屏幕采集及模型 screen-only 需求需要另行实现和验收，阶段 A 的自有窗口截图回归不覆盖此需求。

## 最终提交与包引用回填

| 引用 | 当前记录 / 待补 |
| --- | --- |
| 最终实现提交 SHA / 产品 Core tree | 待主任务提交后填写；基线为 `9cd8726778530d6652b00eb8be91a521639ce995` + 本轮暂存 overlay |
| Mac review patch 快照 | `.cache/screenshot-handoff-20261107/native-screenshot-handoff-review.patch`，72434 bytes，SHA256 `2979588d8a6ef9bb76ffbd1e1c88bcc7a2d5c493e4a29103147a18fd33b2e10f`；它是编译评审输入，不能替代最终提交身份 |
| Core 十组最终回执 | `.cache/screenshot-handoff-20261107/core-required-results.json`，10/10 exit 0 |
| Mac 产物、日志与通过边界 | 已核最终报告、10/10 源码 blob；同伴 9 个产物已按哈希归档；原生 PNG 和图片卡片已由主任务另行查看 |
| Windows 最终程序 SHA256 / staging / ZIP | 构建主程序 SHA256 `02e1bfa357906f36354415acf430c6685fae83d825b2fc4956758ab63c2989af`；本阶段不另出包，继续用户要求的常驻、screen-only 改造后统一验证交付物 |
| 实际安装与运行进程 | 未安装；Windows 现装 PID 584、Mac 现装 PID 96951 保持原状，后续如更新需另核对运行路径与产物身份 |

阶段 A 冻结产品 tree 为 `5e9c08602ce3a0d7bd28a07d73b8882d3128e11b`，Core tree 为 `c45f53053fa80c0eec80852640aed03484a83e14`。本记录随实现提交保存；后续阶段 B 的屏幕专用工具和常驻采集需要自己的源码、构建与实机回归记录。

阶段 A 按上述证据冻结，供主任务提交。阶段 B 的常驻屏幕采集与模型 screen-only 改动尚未开始实现。
