# Sky Computer 方法实现与 Windows 本地交付

## 需求与实际范围

用户要求按本机 `@oai/sky` 暴露的方法全部对齐，并仔细确认该库是否内置 GUI 模型。
参考为已安装官方 Windows 插件 `computer-use/26.1007.71326` 的 window2 文档与实际
`cua_node/9c26d707817e90a7` 中 `@oai/sky 0.7.17` 的可读 JS/类型声明。
本轮实现方法接口及其普通调用、模型适配和必要原生能力，不导入专有 SDK 或其执行协议。

正式 `computer` 工具现在使用 `{method,parameters}`，提供以下 13 个方法：
`list_windows`、`get_window`、`list_apps`、`launch_app`、`get_window_state`、`click`、
`press_key`、`type_text`、`scroll`、`set_value`、`drag`、`perform_secondary_action`、
`activate_window`。每个方法的参数保留 Sky 命名，次级动作的 `action` 留在 parameters 内，
避免把方法选择器与原生动作名混成一个字段。

原有高级显示器、按住键鼠、原始事件、触摸及笔等能力保留在 `computer_input`，不提供
重复的基本 Sky 操作。两个工具复用同一 Controller、观察缓存、原生捕获/输入后端和输入所有权。
普通 C++ 调用不经过 JSON 工具协议。没有新增 MCP、Agent 服务进程、JavaScript 运行时或第三方库。

## 可复现操作与预期

1. 列出应用/窗口，恢复真实返回的 `{app,id,title?}`；不以模型猜测或窗口名称代替进程/应用身份。
2. `get_window_state` 缺省返回图像；明确 text-only 时只请求原生文本，不增加截图请求或图像结果。
   同时关闭两个观察项应失败。
3. 主模型从当前树读取 element_index，或将当前图像像素换算成窗口逻辑坐标再操作；旧截图、旧索引、
   错误应用、窗口/PID/DPI/原生元素变化必须拒绝。
4. `set_value` 通过 UIA ValuePattern / AXValue 修改指定可编辑元素，次级动作来自实际提供者；
   不支持时返回 NotSupported，不偷偷改成全选输入或盲按键。
5. 鼠标键盘/双轴滚动/拖动复用既有输入核心；取消、失败或任务结束只清理自身输入，不能释放新任务或其他控制器的按住状态。
6. 真实 Agent 会话中用中文要求编辑自有控件，完成 17 次工具调用、7 次效果及最终读回；另用 8 次
   canonical 调用验证 4 张实际生成的自有 PNG 图像经过 Agent/ContextBuilder 传入模型。

## 实现和关键边界

- 增加 native application 目录与启动、窗口规范身份、可选焦点/选择/文档事实、元素深度和次级动作。
  Windows 使用系统应用目录/注册信息、COM/UIA/Shell；macOS 使用 AppKit/AX 和系统应用位置。
  不用 shell 命令启动 GUI，不加入模型库。
- Sky 普通截图对象保留完整 data URL；模型工具输出以实际图像附件及 `image_attachment_index` 传输，
  不把绝对文件路径冒充 URL，也不在文本重复 Base64。额外返回编码图像像素尺寸、逻辑边界及
  `image_coordinates_usable_for_input`，明确 DPI/图像空间转换。
- 无障碍树包含真实的逻辑元素位置、禁用/离屏状态及提供者动作。没有提供的焦点、选中文本、
  文档文字等保持缺失，不填造假的零值或空事实。
- 索引、图片标识和 canonical app 附着于原有唯一观察状态。交付及执行前复核当前状态指针、token、
  HWND/PID/应用、原生引用和几何/DPI；不会把并发观察 A 的图片与 B 的元素混合。
- 规范键名适配支持已核对的 Control_L/R、Shift_L/R、Alt_L/R、Meta_L/R、KP_/Numpad_ 及常见标点别名，
  实际物理键和平台执行仍只有一套解析/输入核心。
- 滚动对齐方向和双轴参数；JS/类型声明未规定统一物理像素单位，保留原生 wheel delta，不能声称
  跨系统滚动距离与专有 helper 完全一致。宿主保留有界输入：点击最多 127 次、文字/值最多 4000 UTF-8
  字节、有限滚动增量；这些是 MaiChat 资源边界，不冒充 Sky 私有实现的隐藏限制。
- 工具快照升级到 `model-tools-v4-*`，从 v3/v2/v1 显式追加配置更新，保留旧快照、原始对话和调用事实。
  旧名字/旧格式不作为第二个基本模型入口。

常驻采集生命周期本轮没有更改。后台保持采集资源与给模型发送图像的频率不同；顶部条仍反映真实状态。
text-only 不请求/回传新截图，也不擅自停止其他会话共享的采集。不能宣称 Codex 原生流每次截图后都关闭。

## Windows 验证证据

时间：2026-11-26，Windows x64 正常 Release。main 基线
`be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加当前工作区；没有 commit/push/release/install。
正常编译目录 `.cache/r311/src`、`.cache/r311/build-msvc`；185 件输入逐 SHA-256 核对原工作区与编译副本。
接口新增涉及桌面 ABI，正常依赖重编所有本轮实际消费者；多个原生 GUI 目标也完成正常编链，但未冒称运行。

证据在 `.cache/sky-computer-20261126/`：

- `red/`：旧模型适配器对新方法测试产生 62 个预期失败。
- `review-red/`：12 个可复现断言暴露激活后旧状态派发、操作后取消、启动目录取消传播和错误按钮替代等问题。
  修复后相同新增断言通过；保留原失败日志。
- `green-final/results.json`：11 个目标全部 exit 0。包括 Sky 方法、advanced 扩展、75 个底层验证函数组成的
  合同测试、真实 UIA 元素、应用目录安全，以及 catalog/discovery/rollout/model/tool-loop/journal 回归。
- `desktop_computer_element_windows_test` 实际 26 检查、0 失败：自有 Win32 编辑控件写入读回、TreeView
  Expand/Collapse、选中事实、密码/禁用/只读/过期/取消拒绝；前台窗口保持不变，无键鼠注入或屏幕读取。
- 应用目录测试使用自有窗口和真实目录枚举，验证身份、无效参数和取消；没有启动用户已安装软件，
  未将原生启动成功后的真实 App 交互列为已验。
- `final-qt-results.json`：Controller 完整目标通过；Starter 通过；真实会话 UI 的四个既定自有场景通过
  （加 init/cleanup 共 6 passed、0 failed）。未运行真实 provider 或原生用户窗口操控场景。
- 测试迁移保留普通控制器的显示器/按住输入/触摸/笔等回归；旧公开协议测试替换为 canonical 接口断言，
  没有新增兼容输出翻译层。首次迁移中 URL 字段缺失断言、测试文本编码损坏、遗漏取消 callback、树格式
  parser 不识别新增事实的失败日志均保留；修复的是测试夹具及真实协议校验，不降低唯一目标、实际效果或终态断言。
- 正常 App 与 staging 完成 Qt 部署及 3 个程序/116 个 PE 导入检查。单一 ZIP 生产器完成 CRC 核验和实际
  解压；stage、ZIP 副本、下载目录副本均在 bare PATH 下 `--smoke` exit 0，程序 SHA 相同。

## 交付

- 程序：`C:/Users/18034/Downloads/MaiChat-local-20261126-sky-computer/maichat.exe`。
- ZIP：`C:/Users/18034/Downloads/MaiChat-Windows-local-20261126-sky-computer.zip`，264543929 字节。
- ZIP SHA-256：`61d21acf8515585b9d419a332408cc9202d999df0e06eeae492929fef56c5f92`。
- EXE SHA-256：`7113838a56afab320e4d777908421c4066e8cec93e0b2b9804c3eb5bb5722890`。
- 包内包含源码输入清单及体验说明；回执为 `local-handoff.json`。

核验时 PID 67168 仍运行旧 `Downloads/MaiChat-local-20261126/maichat.exe`，没有停止或替换。
体验新代码需退出旧版再启动新目录；本地包不是新的 GitHub Release。

macOS 源码已提供，但本轮没有 Mac SDK 编译或实机验证。原有窗口图像与原生坐标的变换仍未证实，
明确保留 false 标记并拒绝图像推导坐标；原生元素操作及文字观察的逻辑位置可按提供者事实使用。
没有以 Windows 结果替代 Mac 结果，也没有对真实用户账号/数据库做迁移验收。

## Sky 是否内置 GUI 模型

详见 `2026-11-26-sky-runtime-model-research.md`。已检查实际安装的 JS、类型、依赖、文件清单及有限原生
字符串：可见路径主要是状态采集、动作执行和向外层 Agent 返回图像，未发现独立 GUI 决策模型的可读推理
入口或常见权重/SDK；音频能力检查是检查外层模型。原生 helper 闭源，静态检查不能排除全部辅助能力或远程路径。
Codex 外围的 Guardian 审查也不等同于 Sky 内置 GUI 模型。本实现没有新增 GUI 决策模型，仍由当前主模型决策。
