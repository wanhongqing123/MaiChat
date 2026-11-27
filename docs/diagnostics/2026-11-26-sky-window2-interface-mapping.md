# @oai/sky window2 接口逐项对照

日期：2026-11-26。用户要求明确 sky 向模型开放哪些操作，并使 MaiChat 操作尽量对齐。本轮核对接口，未新增产品功能或依赖，也未操作桌面。

依据：本机官方 Windows 插件 `C:/Users/18034/.codex/plugins/cache/openai-bundled/computer-use/26.1007.71326/docs/api.md`，结合相邻 `guidance.md`。这是该版本 Windows `window2` 接口，不推定为 macOS 或所有新版插件的完整协议。

## 模型看见的入口

该插件通过持久 JavaScript 工具提供 `@oai/sky`。API 参考列出以下 **13 个可调用方法**；`target: "windows"` 是属性，不是第 14 个操作。这不表示模型请求必然包含 13 份独立 function tool schema：这些是模型通过 JavaScript 入口可调用的方法。

当前 MaiChat 用一个客户端执行的 `computer` 函数工具分派 action。对齐应包含行为、参数、返回事实和失效边界，而不只是把 action 改成一样的名字。本文只读对照，不决定必须拆成 13 个工具或引入 JavaScript。

## 13 个操作与当前映射

| sky 方法 | 对模型提供的能力 | MaiChat 当前入口 | 差异 |
| --- | --- | --- | --- |
| `list_windows()` | 列出可操作窗口 | `computer(action=list)` | 已有窗口发现；返回对象结构不同 |
| `get_window({id,app?})` | 用已有窗口身份恢复绑定 | `observe` 按 window_id 重新选择和观察 | 没有独立的轻量窗口对象恢复接口 |
| `list_apps()` | 已安装应用及其当前窗口 | 无同等接口 | 只列运行窗口不等于应用目录 |
| `launch_app({app})` | 按应用标识或明确 exe 路径启动 | 无同等 computer action | 不能把 Shell 启进程冒充窗口应用启动契约 |
| `get_window_state(...)` | 获取窗口截图、无障碍状态 | `observe(target=window)` | 已有按需图片/元素；默认值和观察字段不同 |
| `click(...)` | 坐标或元素点击，指定按钮和次数 | `click/double_click/right_click` | 已有；窗口坐标、截图绑定、任意 click_count 与当前参数不完全相同 |
| `press_key(...)` | 窗口内按键或组合键 | `key` | 已有；键名是两套解析规则，不能直接声称逐字兼容 |
| `type_text(...)` | 向窗口当前焦点输入字面文本 | `type` | 已有 Unicode 文本输入；仍需确认真实编辑焦点 |
| `scroll(...)` | 从窗口指定位置执行水平/垂直滚动 | `scroll` | 已有单轴行数；方向、单位和双轴参数不同 |
| `set_value(...)` | 替换指定可编辑元素的值 | 无等价元素接口；只有 `replace_text` | 聚焦后全选输入不等于按元素设置值 |
| `drag(...)` | 从窗口起点拖到终点 | `drag` | 已有；坐标单位、截图关联和持续时间参数不同 |
| `perform_secondary_action(...)` | 执行元素报告的次级动作 | 无同等接口 | 普通右键不能替代 Expand/Collapse 等元素动作 |
| `activate_window(...)` | 显式激活目标窗口 | `focus` | 有同类能力；sky 文档还规定输入方法自动激活其目标 |

当前 MaiChat 对照位置：`MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp`、`DesktopComputerTools.h`、`DesktopComputerPlatform.h`。正式工具名为 computer，不含旧活动别名 desktop_computer。

## 参数与返回事实中的关键差异

### 窗口与应用

sky 的 Window 对象包含 `app`、不透明 `id`、可选 `title`，动作携带目标 Window。应用列表还包含 displayName/id、可选运行和最近使用信息，以及其 windows。文档中的可选值不能在 MaiChat 后端取不到时凭空填成已知。

MaiChat 当前把 session、window_id、snapshot_id 等传给共享核心，并额外检查 PID、几何和输入接收者。已有安全绑定应保留；对齐输出对象并不要求移除这些检查。

### 窗口观察

sky 默认 `include_screenshot=true`、`include_text=false`；MaiChat 模型入口默认不带窗口图片，由 include_image=true 显式开启。普通 C++ observe 的默认参数又与模型适配器不同，不能混为一个默认行为。

sky 的 WindowState 返回：

- `window`：当前窗口对象。
- `accessibility`：未请求时可为 null；请求后包含树及可选 focused_element、selected_text、selected_elements、document_text。
- `screenshots`：有限截图列表，覆盖窗口及相关瞬态界面；每张有 id/url/zIndex，可选尺寸和 origin。

MaiChat 主要返回有界元素列表、provider/覆盖诊断及可选单张窗口图片，尚无同等的选中内容/文档正文结构。单张图加单个 snapshot_id 也不能直接冒充 sky 的多张截图 id/zIndex/origin 契约。

### 点击、拖动和滚动

sky 用窗口相对 x/y 坐标；截图尺寸在文档中注明逻辑像素。坐标动作可携带最新观察对应的 screenshotId；按元素点击使用最新树的 element_index。窗口变化和重试后必须重新观察。

MaiChat 用目标 bounds 的 0..1 比例坐标，element_id 是当前快照中的不透明身份。保留严格绑定的同时若提供 sky 式入口，需要明确坐标转换，不能只重命名字段或假设 PNG 像素等于输入坐标。

sky 的 scrollX/scrollY 是滚动 delta：负 X 向左，正 X 向右，负 Y 向上，正 Y 向下。MaiChat 的 lines 正值在垂直轴代表向上，且一次选择一个 axis。这里既有方向差异也有单位差异，不能把数字直接透传。

`perform_secondary_action` 的动作标签来自当前元素观察，如 Raise、Scroll Up/Down、Expand、Collapse；不是任意字符串指令，也不意味着每个原生元素都支持这些动作。

## 对齐顺序建议

1. 先补窗口对象、窗口状态和焦点/选择/文档等观察事实，使模型能决定使用图像还是文字；所有缺失、截断和失效必须真实表示。
2. 补指定元素 set_value 与次级动作，复用现有 UIA/AX 引用与执行核心；实际不支持时明确拒绝，不能用一串盲键伪造成功。
3. 核对窗口相对坐标、滚动单位/符号、键名及点击次数；保留现有 snapshot/PID/取消和自有输入释放保证。
4. 应用发现/启动与应用级授权共同设计，不恢复已删除的桌面进程管理旧工具作为第二条正式路径。

本次未实施这些新增能力。先前的 computer 改名及提示修正仍是单独一轮已验证改动；本报告不将“有相近动作”写成“已经与 sky 全面对齐”。
