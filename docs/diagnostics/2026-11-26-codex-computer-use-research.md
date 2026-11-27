# Codex Computer Use 调研与 MaiChat 对照

日期：2026-11-26。任务是只读调研 Codex 的桌面控制工具、模型交互、观察数据和权限，以及当前 MaiChat 的差异。本轮没有控制用户桌面、截图、启动应用、修改剪贴板、发送模型请求或修改产品代码，也没有新增依赖。

## 结论

Computer Use 是模型操作图形界面的能力，包括选择应用窗口、观察截图或无障碍元素、点击、输入、滚动和再次观察。它与执行命令行的 Shell 分工不同。

本机 OpenAI Windows 插件的具体入口是持久 `node_repl` JavaScript 会话中的 `@oai/sky`，不是一个必然名为 `computer` 的基础工具。Responses API 另有 `computer` 工具及 `computer_call`/`computer_call_output` 协议；不能据此推断 Codex 客户端使用同一种模型工具协议。

MaiChat 已有同类普通 C++ 核心及 `desktop_computer` 适配器，基本观察、输入、取消和快照绑定并非空缺。后续对齐应集中于观察内容、应用授权和模型调用流程，而不是重新实现一套桌面输入后端。

## 证据及版本边界

1. 本机公开 Codex 源码：`E:/OpenSource/codex`，固定提交 `3d86e43b075adeba2193ec5c22ac92f55afd82e6`；该提交 `codex-rs/Cargo.toml` 的版本为 `0.172.0`。通过 `git show` 只读检查，没有切换其工作区。
2. 本机已安装的官方 Windows 插件：`C:/Users/18034/.codex/plugins/cache/openai-bundled/computer-use/26.1007.71326/`。读取其 `skills/computer-use/SKILL.md`、`docs/api.md`、`docs/guidance.md`、`docs/confirmations.md` 和 `.codex-plugin/plugin.json`。这是该安装版本的接口事实，不代表所有新版客户端采用完全相同的名字或参数。
3. 当前官方产品文档：原 Codex 页面现重定向到 [Computer Use](https://learn.chatgpt.com/docs/computer-use)。其中说明 macOS/Windows 的应用权限、Windows 前台使用和 macOS 后台使用差异。当前说明与 0.172 的配置字段有版本差异，不能混用配置键。
4. [Responses Computer Use](https://developers.openai.com/api/docs/guides/tools-computer-use) 与[集成示例](https://developers.openai.com/api/docs/guides/tools-computer-use-integration)用于核对 API 协议，而不是充当 Codex 私有桌面后端的源码。
5. MaiChat：`main` 基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加现有工作区；重点阅读 `MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp`、`DesktopComputerPlatform.h`、`DesktopComputerBackend.h` 和 Windows/macOS 后端。本轮是源码对照，未重新做实机验收。

插件清单将 license 标为 `Proprietary`，其仓库地址指向 OpenAI 内部项目。公开 Codex 仓库能查到配置、审查策略及活动展示代码，不能因此声称拿到了完整 `@oai/sky` 原生后端实现。遮挡捕获等能力在本次仅得到插件文档说明，未实际运行验证。

## 本机 Windows 插件的实际调用方式

链路为：模型调用持久 JavaScript 工具 → `@oai/sky` 提供窗口操作 → Windows 原生能力执行 → 文本/图像结果回到模型。

`SKILL.md` 明确说明使用 SendInput、UI Automation 和 Windows.Graphics.Capture；文档称窗口遮挡时仍可捕获截图。截图可读不等于输入能在后台执行：当前官方文档明确 Windows 输入在活动桌面前台进行。

API 分为三组：

| 用途 | 本机插件接口 |
| --- | --- |
| 发现和绑定 | `list_apps`、`list_windows`、`get_window`、`launch_app`、`activate_window` |
| 观察 | `get_window_state`，分别控制 `include_screenshot`、`include_text` |
| 操作 | `click`、`press_key`、`type_text`、`scroll`、`set_value`、`drag`、`perform_secondary_action` |

窗口对象携带 `app`、不透明 `id` 和可选标题。必须从真实返回结果选择目标；有歧义时不猜窗口。窗口状态包含窗口对象、有限截图列表和可选无障碍状态。

无障碍状态除元素树外，还提供 `focused_element`、`selected_text`、`selected_elements`、`document_text`。它使模型可以直接读取当前选择或文档内容，不必总依赖 OCR 或遍历所有控件。

坐标操作使用窗口相对位置，并可携带该窗口最新观察返回的 `screenshotId`；元素操作使用最新无障碍树的 `element_index`。两者不是可永久复用的目标标识。

## 观察—操作闭环及上下文开销

插件 guidance 对依赖界面状态的动作要求：先观察并让模型检查结果，再执行一个动作并立即刷新。界面变化、调用交错或重试后，重新获取元素索引和截图依据。

- 默认观察返回截图，不返回无障碍文字；需要元素时可只返回文字。只有下一步确有需要时才同时获取两份。
- 截图由运行时自动展示；不为了让模型看图而再次打印 Base64、解码、保存再重复展示。
- 窗口对象保留在持久 JavaScript 状态中，减少反复发现；这不代表图像或历史输入不占模型上下文。
- 截图和无障碍树是昂贵的时点观察。刷新服务于下一步决策或验证，不应盲目高频截图，也不能因为省 token 而复用失效元素。
- 输入或随后刷新出错时，可能已经产生部分副作用。先观察实际状态，再决定是否重试，不能自动重复点击或输入。

这些证据能支持减少重复观察和重复图像输出；不能支持“Codex 自动只发送最后一张图”“截图不算 token”或“Computer Use 固定只发送七个工具”等结论。本次没有请求抓包或真实 usage 计量，不给出节省比例。

注意这个 Windows 插件的观察/单动作约定与 API 原生 `computer_call.actions` 有序批处理不是同一层契约。API 文档的 `status: completed` 仅表示调用已生成完，操作仍需客户端执行；不能将它当成应用任务已完成。

## 权限与生命周期

公开 0.172 源码的 `codex-rs/config/src/computer_use.rs` 包含 `default_app_access`、macOS `bundle_ids`、Windows `aumids` 及按 publisher/product/binary 的应用规则。当前在线文档出现 `always_allowed_app_ids` 等更新配置，迁移时需固定版本核对，不直接拼到旧格式中。

操作系统权限、允许控制哪个应用、具体敏感动作审查是不同层次。文件/Shell 的沙盒授权也不能代替桌面应用授权。插件 guidance 另有禁止操作对象及逐动作确认策略；其具体限制需作为该版本策略读取，不能只截取较宽松的确认表。

插件的 Interrupt、Stop、SubagentStop hooks 通知 `turn_ended`；guidance 要求任务结束或用户停止后不再发输入。公开源码还有按模型策略启用的 REPL 文本/图像审查证据模式及 Computer Activity 历史分组。这说明停止、审查证据和回放是正式流程的一部分，而不只是发鼠标键盘事件。

这些是参考实现的机制事实。MaiChat 按仓库约定保留进程内 C++ 核心，不引入 MCP、app-server、额外 Agent 进程或新 JavaScript 依赖。插件的传输与部署方式不列为产品对齐缺口。

## MaiChat 当前能力与差异

| 项目 | 当前 MaiChat 源码观察 | 判断 |
| --- | --- | --- |
| 普通核心与模型入口 | `DesktopComputerController`/native backend + `desktop_computer` | 已有共享核心，继续复用 |
| 窗口、显示器发现 | `list`/`list_displays`，真实 window_id 和 PID | 已有；不等同于插件的已安装应用发现 |
| 截图/元素分开请求 | window observe 支持 `include_image`、`include_elements`；窗口图片默认关闭 | 已有按需观察，默认语义不同 |
| 元素、坐标绑定 | session/target 的 `snapshot_id`，元素原生身份重新校验，突变消费快照 | 已有，不能说只会盲点坐标 |
| 当前焦点和文档信息 | 有非密码焦点文本 readback；元素输出主要是 role/label/geometry/automation_id | 缺少插件同等的 selected_text/selected_elements/document_text 结构化观察 |
| 直接元素操作 | 元素定位支持点击；replace_text 是聚焦后全选输入 | 不等同于 `set_value(element_index)` 或元素次级动作 |
| 坐标 | 显示器/窗口 0..1 比例，输出原生 bounds 与图片大小 | 与 sky 窗口相对坐标接口不同；对齐语义不应只改参数名 |
| 应用授权 | 此工具按动作走 Agent approval，并有 OS/焦点边界 | 此入口未见与 Codex 按应用持久 allow/deny 同等的策略结构 |
| 输入释放 | 自有按键/鼠标账本、取消/回合结束释放、空闲回收、失败事实回执 | 已有，后续改动必须保留 |
| 应用发现与启动 | `desktop_computer` 没有 `list_apps`/`launch_app` 同等接口 | 是该入口差异；不因此恢复已删除的旧 process_start 工具 |
| 原生 Responses computer 协议 | 当前桌面入口是普通函数工具；检查的 Responses encoder/model 类型未发现 computer_call 处理 | 独立 API 接入任务，不冒称已支持，也不是模仿客户端 sky 必须走的路线 |

建议后续顺序：先补齐选中文本/焦点/文档等观察事实及元素动作语义，再完善应用级授权和停止后续输入的验收；同时收紧 Computer Use skill 的实际工作流程与按需图像输出。当前长工具说明是否实际导致额外 token，需要针对最终声明/请求计量，不能仅凭源码长度下结论。

是否接入 API 原生 `computer` 应另行按目标模型和供应商能力决定。研究 Codex 并不自动要求替换现有跨供应商入口，也不意味着获准增加桌面运行时依赖。

## 后续验收操作建议（尚未执行）

1. 自有测试窗口放两个同名控件：观察后点击元素，移动/销毁原控件后拒绝复用旧元素。
2. 原生编辑控件选中一段中英文：只请求文本观察，验证焦点、选中内容和文档正文的来源和截断标记。
3. 窗口缩放、DPI 变化、多显示器与弹窗：核对截图 ID、图片尺寸及接收窗口，过期依据必须重新观察。
4. 拖动中取消/任务结束：释放本任务持有输入，并确认没有后续注入。
5. 分别请求图像、文字和二者：记录实际请求图像数、序列化字节和服务端 usage，量化重复输出；保留真实会话正文。

本报告只完成调研，不将上述未跑操作写成已通过测试。没有改动本轮已交付的基础工具包。
