# Desktop computer 通用键盘覆盖与输入读回

## 需求与基线

- 基线：`ff09eaa779c58e9cd50955e8068e2dbf021063b0`，2026-11-11。
- 用户要求键盘覆盖所有按键，并以 QQ 发消息、操作软件制作 PPT、双击或右键打开软件说明通用电脑操作的预期。用户随后明确这些只是例子，不要求 QQ/PPT 专用工具。本轮未操作这些应用，也未向真实联系人发送消息。
- 基线生产八文件共 3224 行（含注释和空行）；主入口 `DesktopComputerTools.cpp/.h` 合 1139 行，另七个测试文件共 5671 行。共享截图和 Agent 回合组件不在统计内。行数不是任意软件任务成功的证据，本轮按具体缺口修改。
- 平台：Windows 11 / MSVC 2022 / Qt 6.12；Mac arm64 / macOS 27.2 / SDK 27.0 / Qt 6.12。

## 普通接口与工具合同

1. `type` 保持 Unicode 文本输入，`key` 处理单键和快捷键。任意文字不依赖切输入法或把字符逐个猜成物理按键。
2. 共享 `DesktopComputerKeys` 增加标准键名目录、左右修饰键与家族查询。保留现有枚举序号和结构字段。标准键盘、小键盘、标点键位、锁键、扩展键及媒体/浏览器键均有明确名称；平台无可靠原生实现时返回 `NotSupported`。
3. 字面 ASCII 标点按当前键盘布局解析所需修饰键；命名 `Equal`、`Minus` 等明确表示键位。`Plus` 与小键盘 `NumpadAdd` 分开；`Ctrl+Plus` 和明确末尾加号 `Ctrl++` 可表达加号，歧义串继续拒绝。
4. 允许 `Ctrl+Shift` 这类纯修饰键组合。泛化修饰键保留“借用用户已按住的一侧，不释放用户的键”；显式左右键按指定一侧操作，释放一侧不清掉另一侧仍有效的家族状态。
5. 增加只读 `list_keys` 工具入口，复用普通键名目录。目录表示可解析名称，平台可发送能力在执行前校验，不将目录当作硬件消费 ACK。
6. 焦点、目标 PID/窗口、交互桌面、受保护输入和取消检查沿原普通 controller/native scope；每次操作的按下与抬起成对清理。持续按住/松开型游戏控制尚待用户选择，不先加入跨调用持键状态。
7. 通用审计发现 DS Pro 不支持图像却可进入 display 图像路径，现复用模型目录能力在原生操作前拒绝。原生 `input()` 错误可能发生在部分派发后，现保留原错误码和原文，同时要求先观察应用状态再决定是否重试；输入前验证/聚焦错误不加此反馈，没有提供外部应用“恰好执行一次”的保证。

## 验收操作与预期

- 普通解析：旧快捷键保持，新增标点、小键盘、左右键、锁键和修饰键组合可解析；非法/歧义名称明确拒绝。
- Windows/Mac 原生映射：主 Enter 与小键盘 Enter、导航与数字区、左右修饰键正确区分；布局字符不按 ASCII 值冒充虚拟键码。系统保留或无可靠等价的键不伪造成功。
- 自有两进程编辑/聊天输入框：观察并聚焦，输入中文、标点及 emoji，独立读回，选择替换；以 Enter/Ctrl+Enter 各发送一次至自有接收计数器。不得向真实账号发消息，不改系统剪贴板。
- 取消和失焦后只释放本次注入的键，保留用户此前按住的键。
- 截图、锁键、音量等可能影响整个系统的动作不在用户桌面盲跑；将纯映射覆盖和实际投递覆盖分列。Mac 前项登录窗口条件仍按原阻断记录，不绕过安全检查。

## 文件分工与顺序

- 共享解析与原契约测试：`DesktopComputerKeys.h/.cpp`、`DesktopComputerContractsTest.cpp`。
- Windows 映射与自有原生 fixture：`DesktopComputerWindows.cpp`、`DesktopComputerNativeIntegrationTest.cpp`。
- Mac 映射：`DesktopComputerMac.mm`，依据原 SDK/Quartz 公开契约。
- 工具目录与家族门禁：`DesktopComputerTools.cpp`，复用普通接口。
- 先真实失败用例，再最小实现，定点回归后正常消费者构建。直接 main；本轮不改版本、不打包、不安装或发布。

## 实测证据

### 共享协议与 Agent

- 旧解析器加新原契约用例：117 条断言失败；共享实现后完整契约通过。
- 工具目录与 schema 原实现：3 条断言失败；目录及 128 字符上限修正后通过。
- DS Pro 图像 gate 5 条、window/display 输入不确定反馈各 1 条，共 7 条真实失败；修复后完整契约 exit 0。旧 observation 不允许重复输入，原错误码/native detail 断言保留。
- 最终正常 Agent 集成测试仅选择两个原 scripted-model / Fake-backend 用例：中文 observe/act/readback 与 display fresh-image，PID 11236，4 PASS / 0 FAIL / 0 SKIP（含两个 hooks），416 ms。首个误跑整个 executable 的空 stdout / exit 77 保留，不计通过。此结果不代表真实视觉模型自主完成 QQ 或 PPT。

### Windows

- 旧后端精确源码的独立只读映射 probe：18 个键例、23 条断言失败，无输入。新普通映射：183 个目录名中 177 个有映射、6 个明确 NotSupported，0 条失败；同时核 VK/scan/E0、布局标点和系统保留组合，无输入。
- 原生自有窗口历史失败全部保留。右 Shift 在当前中文 IME 下接收 raw scan/E0 与硬件映射表不同；一次 scan-mode 产品实验仍收到相同结果，实验已撤回，没有在产品保留该特殊处理。
- 接收器保留 raw 消息，并独立读取左右按键状态。中文 IME 将 A 的 keydown 转为 VK_PROCESSKEY 时，在自身消息循环 TranslateMessage 前用 ImmGetVirtualKey 读取 original VK；不切换或禁用输入法，不用预期值反填观测。原始失败不改记为通过。
- 修正 IME 观察后原例实际通过左右修饰、双 Ctrl 顺序、A/9、主 Enter、NumEnter 和 Num0，随后发现真正的 Insert E0 缺口：本机 HKL 的 MapVirtualKeyEx 未给导航键前缀，本轮映射重构因此丢失旧实现明确的导航 E0 分类。新增完整独立导航/NumPad 矩阵先实跑 20 FAIL，恢复明确分类后原只读矩阵为 177 支持 / 6 NotSupported / 0 FAIL。最终 Windows.cpp raw SHA `d1608e83ab95c5a877631b86c3f9e52cf09126d454af2e0f634d5c6d1e0c887f`。
- 最新同原生例父 PID 46940 / 子 PID 45648，exit 77；Windows 拒绝测试窗口进入前台，`input_attempted=false`。本轮完整原生矩阵仍待焦点，不算 GREEN；没有强制抢焦点或留下自动 UI 等待。
- 私有导航单文件交接 patch 曾因写入 CRLF 而不能应用，首错与错误附件保留；已另存 canonical LF 件。Root 最终三路径 patch 独立由 Git 生成并经临时 index apply/check，Mac 逐 blob 核准使用后者，不使用该错误中间件。
- 最终 normal App / Contracts / scripted Agent 构建 exit 0，8.009 s；部署校验实际报告 3 binaries / 114 PE imports / Qt plugins。Contracts PID 17124 / exit 0，0.716 s。原 App 自有 `--smoke` PID 37776 / exit 0，1.551 s，裸 Windows PATH、私有数据目录，运行路径及五个 Qt/platform 模块均为 normal build/bin；exe SHA `aea51d3b7fbdbd998d89020230e6044abd6008bc8c22abfa1ea8df93d2aad573`。

### Mac

- 纯映射 Protocol2 同一份 118 输入/断言源：v1 为 117 PASS / 1 FAIL，v2 为 118 PASS / 0 FAIL。严格核 CmdRight+Plus 的 Equal 物理码、implicit Shift、MetaRight，以及当前 TIS 的 `=,+,=,=` 四个真实转换值。先保留 Command 精确查找，只有同一物理键的基础 glyph 稳定时才补充 glyph/modifier 解析，不固定 US 或借小键盘替代。
- 最早“Command 逆译必须仍是 +”不符合 Apple glyph 与 modifier 分离的 key-equivalent 定义，其原 1 FAIL 保留；没有宣称旧断言变真。
- 四份已安装 Dvorak 布局资源的 Cmd+A/Q/W/Z 共 16/16；不选择输入源，前后 active layout 相同。映射 probe 无 CGEventPost 导入，Dvorak probe 无 TISSelectInputSource 导入，均不算真实输入。
- 六个相关源码的 normal App + 完整 Contracts build 0，14.341 s；Contracts PID 2918 / exit 0，1.722 s，exe SHA `0bc658934736d2636a4576aab510eb785bf44ddb7b9148ff8534d86e54257e36`。App SHA `de6a63910f611a53575f49e2e8f4ff1465d8da1f506b6ef04bfd7b05ad5f058e`，本轮只构建，未启动或安装。
- 到件 build/test/receipt 与 Dvorak 原件共七份，Root 已逐长度/SHA 校验；六源码 canonical/blob 与当前文件完全一致。原 SDK 最低版本和重复库 warnings 照原日志保留。
- 最终九源码输入补做 normal App / Contracts / Nativefixture 三目标 compile/link：exit 0，19.721 s；三个产物均未执行，不把此前六源码的 Contracts 运行计成最终九源码原生通过。Nativefixture SHA `8085f199ed07e4419010c954da1f9acb9117802c2f34a65d9c5217c79af2c500`，另外两个 exe SHA 未变。Root 已核三份最终原件长度/SHA、九个 canonical/blob 及两产品树。

## 交付边界

- Windows 的 Fn、Eisu、IntlRo、IntlYen、NumpadEqual、NumpadComma 明确 NotSupported。Mac 的 Insert、CapsLock、NumLock、ScrollLock、F21+ 及未验证的媒体/浏览器键明确 NotSupported，不用不同语义键假冒成功。因此不能宣称两平台所有硬件键都已可用。
- 锁键、音量、系统截图等全局效果本轮仅核映射，未在用户桌面执行。持久 key-down/key-up、跨调用游戏持键未加入。
- NumLock 的 Microsoft overview 正文明确不视作 extended，而旧 keybd_event 示例带该 flag；复核没有据旧样例猜改当前非 E0 映射，也未将此文档差异当作已证实产品故障。当前 HKL 只读 MapVirtualKeyEx 返回 `0x45`，未进行真实 toggle。
- Mac 原生交互仍受此前 loginwindow 条件限制。本轮未运行原生输入，不请求新权限，不将历史 native FAIL 改记为 SKIP 或 PASS。
- 支持 observe、输入和结果反馈，不等于已验证任意应用长流程、发送事务去重或 PPT 美观程度。真实 QQ、PPT、实际模型视觉决策均未验。
- 不修改 Core、CMake、版本或公开资产，不打包、安装或停止现装程序。公开 v2.2.56 不包含本轮修改。

## 依据与证据位置

- [Microsoft KEYBDINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput)、[ImmGetVirtualKey](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetvirtualkey)、[GetKeyState](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getkeystate)。
- [Apple key equivalents](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MenuList/Articles/SettingMenuKeyEquiv.html)、[Qt KeyboardModifier](https://doc.qt.io/qt-6/qt.html#KeyboardModifier-enum)、[GLFW CJK IME 右 Shift 处理](https://github.com/glfw/glfw/blob/master/src/win32_window.c)。
- 本机原件：`.cache/desktop-keyboard-20261111/shared/`、`windows/`、`root/`。源码、产物、运行及到件哈希摘要见同名 JSON；测试产物不包含用户内容、签名材料或凭据。
