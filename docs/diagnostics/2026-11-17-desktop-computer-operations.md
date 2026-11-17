# desktop_computer 当前操作清单

日期：2026-11-17。本轮 Windows 输入实现已提交为 `94f785d7`；窗口发现修订为 `b7001a6d`。尚未替换已安装的 Windows App，统一验证记录见 `2026-11-17-desktop-input-final-verification.md`。

本表列出 MaiChat 当前操作和验收范围。与固定 Sunshine 的逐项行为差异见 `2026-11-17-sunshine-maichat-input-comparison.md`；存在对应原语及通过接收窗口测试，不等于两边逐字段、逐事件严格 1:1。

模型调用的是同一个 `desktop_computer` 工具。`action` 选择要做什么；`input_events` 是其中一个动作，它再用 `type` 指定按下、移动、滚动等细操作。`input_options` 只调整这些操作的方式，不是另一套工具。

当前公开 **25 个 action、8 种 input_events type、10 个 input_options 字段**。普通键鼠、Touch 和 Pen 均使用系统 API，并已通过本机开发构建的实际窗口测试。虚拟手柄、硬件 HID 模拟和控制器反馈已由用户明确暂缓，不是当前能力承诺。

## 25 个公开 action

以下名字均在当前模型 schema 中公开。它们可以按任务组合，清单不规定固定的点击、输入或验证流程。

1. `list`：列出当前桌面的可发现顶层窗口及身份、状态，供选择窗口目标。Windows 包含最小化、小窗口、无标题及浮动工具窗口，不再静默截到 200 项；返回 `count`、原生类名、所属窗口和状态。列出不等于当前可输入；最小化或没有有效几何信息时不会提供可用点击坐标。真正隐藏、仅托盘而无可见窗口的程序及子控件不作为顶层窗口列出；子控件由 `observe` 读取。
2. `list_displays`：列出显示器、原生屏幕边界和显示器 ID，供选择整屏目标。
3. `list_keys`：查询命名键、修饰键和组合键语法。它不发送按键。
4. `observe`：获取目标的新观察和 `snapshot_id`。窗口目标返回窗口边界及可选 UIA/辅助功能元素；整屏目标返回与显示器身份绑定的屏幕图像。窗口观察不提供窗口图像。
5. `focus`：激活已观察的所选窗口。此动作属于窗口目标，不能用于整屏目标。
6. `move`：把鼠标移动到目标边界内的比例坐标。可与本会话已持有的鼠标按钮组合完成连续拖动。
7. `click`：在指定比例坐标单击；按钮可选左、右、中、后退、前进，默认左键。
8. `double_click`：在指定比例坐标双击所选按钮。
9. `right_click`：在指定比例坐标右击，按钮固定为右键。
10. `drag`：一次性完成按下、移动到终点、抬起的拖动。起终点使用同一目标的比例坐标；窗口原生实现当前允许 1～200 ms，整屏允许 1～5000 ms，不能仅依据 schema 的 5000 ms 上限推断窗口也接受该时长。
11. `mouse_down`：在指定位置按下并持续持有所选鼠标按钮，跨调用保留，等待对应 `mouse_up` 或清理。
12. `mouse_up`：只抬起本会话此前持有的所选按钮；未知 up 幂等，不抬起用户的按钮。不要求新的截图 token。
13. `type`：向当前输入目标发送非空文字；最多 4000 个 UTF-8 字节。受保护输入和未释放修饰键会阻止文字输入。
14. `replace_text`：全选并替换当前输入框的文字；允许空字符串表示清空。它不保证保存到业务系统。
15. `key`：发送一个命名键或组合键，例如 `Enter`、`Ctrl+A`、`Ctrl+Shift`；使用 `list_keys` 查询语法。
16. `key_down`：持续持有一个命名物理键或一个修饰键；不接受组合键。首次 down 绑定原生键，后续 up 使用该绑定。
17. `key_up`：只释放本会话已持有的对应命名键；未知 up 幂等，不要求新的截图 token。
18. `scroll`：在指定位置按行滚动；支持 `vertical` / `horizontal`，`lines` 为 -10～10 且不能为 0。正数表示向上或向右。需要原地细滚动时使用 typed `wheel`。
19. `input_state`：读取本会话的持有键、鼠标按钮及其他输入状态；不需要新的观察 token。
20. `reset_inputs`：让本会话持有的控制量回到中立，保留仍有效的输入 context 和普通配置；不要求截图或当前焦点。输入正在执行时返回 Busy，不能把 Busy 当成已经清空。
21. `release_all`：释放本会话持有的全部输入并退役该 context；也用于任务结束或取消后的清理。失败时保留必要的重试绑定。
22. `verify`：读取所选窗口的当前输入文字并与 `expected_text` 对比。它不是网页提交、文件保存或服务器持久化的验证，也不能用于整屏目标。
23. `clear`：清除当前会话的目标选择和观察，并清理自己持有的输入；不删除输入框里的文字。
24. `input_events`：按顺序执行普通输入原语批次，适合精细滚动、相对移动、raw key 和自主组合；单批 1～256 项，逐项延迟及全批延迟总和均不超过 5000 ms。它不是“全部事件同时生效”的原子事务。
25. `input_capabilities`：查询当前后端声明的实现、可用性和前置条件；查询不注入输入，不代表接收窗口已收到事件。

来源：`MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp:1180` 的基础 23 项，以及 `DesktopComputerInputProtocol.cpp:320` 追加的 2 项。

## input_events：6 种普通键鼠原语

以下 6 种均公开。事件可带 `delay_ms`；范围、权限和原生目标检查仍然适用。

1. `absolute_move`：绝对移动。字段 `x_ratio`、`y_ratio` 均为 0～1，映射到本次观察的目标原生边界。
2. `relative_move`：相对移动。字段 `delta_x`、`delta_y` 为有符号 32 位整数；Windows 使用原生相对输入，不先把鼠标移到 `(0,0)`。原生鼠标加速可能影响实际位移，不应把相对量当成绝对屏幕定位结果。
3. `mouse_button`：原地按下或抬起。`button` 为 `left`、`middle`、`right`、`back`、`forward`，`release` 为布尔值；事件本身不移动鼠标。窗口模式在派发前检查当前鼠标下目标。
4. `wheel`：原地细滚动。`axis` 为 `vertical` / `horizontal`；`delta_120` 为有符号 32 位整数，120 表示一格，正数表示向上或向右，0 是无效果事件。启用高精度时保留细量；关闭时按轴累计不足一格的余数。窗口模式每个原生发送块均需检查当前鼠标下目标。
5. `raw_key`：物理键的按下或抬起。`code` 为 1～65535 的协议键码，包含历史 `0x8000` 前缀，不是 Unicode 字符；`flags` 只允许 bit 0，表示非归一化键码；`modifiers` 只允许 0～31，其中 Shift=1、Control=2、Alt=4、Meta=8、extended=16；`release` 为布尔值。首次 down 缓存原生绑定，后续重复和 up 不因布局或映射变化而重新解析。模型不能传内部 `repeat` 标志。
6. `unicode_text`：发送字面 UTF-8 文字，最多 4000 字节；空字符串是无效果事件。中文及补充平面字符使用同一 Unicode 路径；此项不表示所有应用一定接受这些字符。

来源：`DesktopComputerInputProtocol.cpp:325` 的 6 个 schema 分支；原生执行在 `DesktopComputerWindows.cpp` 的 `InputScope::dispatchTyped`，与命名动作共享一个输入作用域和持有表。

## input_events：Touch 与 Pen

下面两种公开系统指针语法。调用前须检查 `input_capabilities`；接口存在、系统 API 可用与应用实际收到输入是不同的结果。

7. `touch`：触摸接触点批次。`contacts` 为 1～10 项，每项包含 `phase`、32 位无符号 `pointer_id`，定位阶段需要 `x_ratio` / `y_ratio`；还可携带 `pressure_or_distance`、`contact_area_major`、`contact_area_minor`（均为 0～1）和 `rotation`（0～360 度，`null` / 65535 表示未知）。阶段为 `hover`、`down`、`up`、`move`、`cancel`、`hover_leave`、`cancel_all`。Windows 使用项目自有 User32 实现，`implemented=true`，`available` 检查所需系统 API。`pointer_id` 是调用者管理的接触点身份，不是 Windows 的原生指针编号；同一接触点后续沿用它即可。按住期间可观察或截图；单点 up/cancel 保留其他接触点。整批先校验，再按系统报告周期建立或更新完整接触点帧，不承诺所有 down 在同一瞬间发生。实际双点持续、十点、压力/接触面/方向以及满槽同批 up+down 均已收到对应原生事件。
8. `pen`：笔输入。阶段包含上述 7 项及 `button_only`；`tool_type` 为 0=未知、1=笔、2=橡皮，`buttons` 为 0～7；`tilt` 为 0～90 度，`null` / 255 表示未知。Windows 使用项目自有 User32 适配器，`implemented=true`；`available` 检查所需的三个系统 API。定位、接触压力、倾斜/方位角、橡皮端、悬停和释放已在同一普通/工具会话的真实窗口中测试。三位逻辑侧键按 OR 映射到 Windows 的一个 barrel 位，不是三个独立硬件键；悬停距离、接触椭圆和笔杆自转没有此后端的原生映射。`button_only` 使用已接受的原生位置，按下前重新检查该位置所属窗口；释放不依赖新的位置。

释放和取消阶段只消费其身份/阶段所需的数据；陈旧但有限的几何值不应阻止清理。DTO 的存在不等于平台实现或可用性。

来源：`DesktopComputerInputProtocol.cpp:363`、`DesktopComputerInputTypes.cpp:93`，以及 `DesktopComputerWindows.cpp` 中 Touch/Pen 分支和 `WindowsInputSession::capabilities()`。

## 10 个公开 input_options

省略整个 `input_options` 表示沿用本输入 context 的配置；显式传 `{}` 表示恢复默认值。以下字段允许单独设置，普通 C++ API 与模型入口使用同一套校验和执行核心。

1. `keyboard_enabled`：键盘输入开关，默认 `true`。
2. `mouse_enabled`：鼠标/指针输入开关，默认 `true`。
3. `native_pen_touch_enabled`：Touch/Pen 输入开关，默认 `true`；开启不能使尚未可用的平台能力自动可用。
4. `always_send_scan_codes`：Windows raw key 尽可能使用扫描码，默认 `true`；具体键码空间仍由 raw key 的字段确定。
5. `right_alt_to_meta`：将右 Alt 映射为 Meta，默认 `false`。
6. `high_resolution_scroll`：保留细滚动量，默认 `true`；关闭时使用按轴余数累计。
7. `keyboard_repeat`：raw key 的自动重复开关，默认 `true`。
8. `repeat_delay_ms`：首次重复前延迟，0～2147483647 的整数毫秒，默认 500 ms；0 禁止该自动重复计时。首次计时从原生 down 开始派发时起算。
9. `repeat_period_ms`：重复间隔，为大于 0、有限且不超过 2147483647 的毫秒数，允许小数；默认 `1000/24.9`，约 40.16064 ms。后续周期从上次派发完成起算，不补发积压的重复键。
10. `key_bindings`：原始键码与替换键码的二元数组，默认空。模型入口最多 65535 项，每项两个 1～65535 的整数，原始键码不能重复；首次 down 应用映射，已持有键的 up 使用原绑定。

`inheritExisting` 是宿主内部元数据，不是模型可传字段。配置只影响输入策略，不是驱动安装或模型设置。

非归一化编辑键码先转换到专用编辑键身份，再应用 `key_bindings`；未配置且系统不能映射的专用键明确返回不支持，不会误发 F11～F14，也不会擅自改成 Ctrl 组合键。重复和释放仍使用首次 down 的绑定。

来源：`DesktopComputerInputProtocol.cpp:215` / `:387`、`DesktopComputerInputTypes.h` 的 `DesktopInputOptions`、`DesktopComputerInputTypes.cpp:233` 的统一校验；Common 和 Windows 原生入口都调用该校验。

## 坐标、快照和权限约束

- `target=window` 的比例相对于所选窗口的完整原生边界，不是浏览器 viewport、网页内容区或聊天面板。UIA 元素返回的 `center_x_ratio` / `center_y_ratio` 已换算到该窗口，可直接用于窗口目标输入。
- `target=display` 的比例相对于选中显示器的原生边界；多显示器的 `bounds.x` / `bounds.y` 可以为负数，不能强行以主屏 `(0,0)` 为原点。
- 对同一幅完整整屏图像，设实际查看的图像尺寸为 `Iw×Ih`、图内像素点为 `(px,py)`，则 `x_ratio=px/(Iw-1)`、`y_ratio=py/(Ih-1)`。输入映射为 `nativeX=bounds.x+x_ratio×(bounds.width-1)`，Y 同理；原生执行再做像素取整。
- 整张图像被缩放后，用缩放后图像自身的尺寸和图内点计算比例即可。预览容器如有留白，应先减去图像在容器中的偏移，再用实际绘制图像的宽高计算；不能把整个容器尺寸当图像尺寸。裁剪图还必须先还原裁剪偏移，不能直接冒充整屏比例。
- 普通输入使用同一会话、目标和观察绑定的 `snapshot_id`。输入会消费观察；整屏输入回执尝试附带下一次观察，窗口输入后可重新观察。单独使用其他截图入口不会续期该 token。token 最长有效期与持有输入的空闲期限是不同约束：当前分别为 5 分钟和默认 120 秒。
- `mouse_up`、`key_up`、纯释放事件批次、`input_state`、`reset_inputs`、`release_all` 不以新的截图 token 或保持当前焦点为前提；只处理本会话的输入身份。过期、取消、失败和释放仍需保留失败 up 的重试信息。
- 原生目标身份、焦点、交互桌面、鼠标命中窗口、外部已按下的键/按钮、受保护输入和任务取消均可能使请求被拒绝。权限由宿主审批策略与平台原生检查共同决定；不能用有 schema、dry run 通过或 `dispatched=true` 代替应用层成功。
- `dry_run` 不派发本次操作，保留有效观察；独立的超时/取消安全清理仍按其自身规则处理。
- 输入会话采用非阻塞准入；已有操作或后台 repeat 正在派发时，普通 API 可返回准入 Busy。Busy 不是完成。普通 API 调用者可在明确无副作用的准入拒绝后有界重试；其他 Busy 可能涉及部分派发和清理，工具入口还涉及 token 消费，不能拿原 token 盲重放整个工具调用。
- repeat 延迟和周期是调度配置，不是接收端实时频率保证；目标校验与实际派发耗时会使间隔增加。当前实机观测不能宣称兑现 15 ms 的端到端周期。
- 可以按任务组合 `mouse_down → observe/截图 → move 或 relative_move → mouse_up`，也可以组合持有修饰键、普通键和清理动作。观察本身不会把仍有效的持续按住改成一次性点击。这些是可组合的基本操作，不是固定业务工作流。

## 已明确延期的公开范围与历史数据

- 顶层 `feedback` 已从当前 schema 移除；手工传入返回 `NotSupported`。普通输入仍返回动作回执和状态。
- 7 种虚拟事件不公开：`gamepad_attach`、`gamepad_state`、`gamepad_touch`、`gamepad_motion`、`gamepad_battery`、`gamepad_detach`、`gamepad_rebind`。模型 parser 明确拒绝；Windows 原生执行也明确拒绝。
- 7 种具体 profile 暂缓：`generic`、`x360`、`xone`、`xseries`、`ds4`、`ds5`、`switch`；`auto` 只是历史选择元数据，不是第 8 种设备能力。
- 6 类控制器反馈暂缓：普通震动、扳机震动、运动事件订阅、RGB 灯、玩家灯、自适应扳机。其 DTO/纯 JSON 序列化可以保留历史数据含义，不代表存在反馈 provider。
- 普通 C++ `DesktopInputOptions` 仍保留 8 个历史字段：`controllerEnabled=true`、`ds4BackAsTouchpad=true`、`motionAsPlayStation=true`、`touchpadAsPlayStation=true`、`randomizeControllerMac=true`、`driver=Auto`、`defaultProfile=Auto`、`backButtonTimeoutMs=-1`。默认值仅兼容既有 DTO，**任一非默认值现在明确返回 `NotSupported`**；未知枚举或非法范围仍为 `InvalidInput`。模型入口连这些字段的默认值也不接受，不再公开配置它们。
- `DesktopInputEventKind`、profile/driver 枚举、控制器状态和反馈结构仍有历史成员；纯结构校验不等于可执行。能力查询中的 Windows `profiles` 为空、`max_controllers=0`。本次没有把这些数据类型的保留算作功能可用。
- 前 26 action / 15 type 的历史清单和冻结前像保留在 `.cache/desktop-computer-system-api-inventory-20261116/`、`.cache/desktop-contracts-system-native-20261116/`，不用于宣称当前公开能力。

## 输入依赖和执行核心核对

- 当前活跃源码、CMake 模块/目标及仓库配置中，未发现 libvirtualhid、Hid adapter、Input Router、ViGEm provider、ViGEmClient 的 include、链接、构建目标或 Git 子模块引用。检索包括当前未提交的新源；没有将 docs/cache 的历史字符串当成活跃依赖。
- `.gitmodules` 当前只有原有 `qtbase`、`qtsvg`；输入 SDK 的源码 Gitlink 已不存在。Git 本地子模块配置未发现 LVH/ViGEm 条目。原有 Qt 配置不属于本次输入 SDK。
- 活跃输入构建使用项目自有 `maichat_desktop_input_types`、Windows 键码映射和 User32 触摸/笔适配器。系统输入调用 SendInput/User32，没有安装驱动、联系 broker 或激活许可的产品路径。
- Controller 的普通动作与 typed 动作均先准备同一个 owned native session；Windows 共同进入 `WindowsInputSession::perform`，使用同一个 `InputScope::held_` 及原生绑定。没有旧/new ledger 的 borrowed 状态桥。
- 旧的一次性自由函数入口仍作为普通 API 包装存在，但复用同一个 `InputScope` / `dispatchNativeInput` 实现，且不支持跨调用的独立 down/up；Controller 没有回退到另一套 `backend.input()` 执行器。没有发现仍活跃的 SDK/系统 API 双执行核。
- 以上是源码/构建依赖结论，不等于本轮执行了系统驱动卸载；本次审计没有探测、安装或卸载系统驱动。

来源：`MaiChat/desktop/CMakeLists.txt:936`、`:944`、`:959`，`.gitmodules`，`DesktopComputerTools.cpp:170` / `:938` / `:1051`，以及 `DesktopComputerWindows.cpp` 的 `WindowsInputSession`、`InputScope` 和一次性包装。

## 验证边界

延期配置校验的早期纯 C++ 用例从 512 项检查中的 18 项失败变为 0 项失败；日志在 `.cache/desktop-native-options-deferred-20261117/`。这只验证配置拒绝规则。Windows recording 合同、DPI 检查和编译结果均不代替真实窗口收到输入的证明；实际输入验证单独列在下面。

此前 SDK 移除批次的 71 项实际键鼠、两进程输入/UIA、ScriptModel Agent 回归及 512/188/184 项合同结果保存在 `2026-11-17-native-input-dependency-removal.md`，属于该次产物记录。本轮最终正常回归为 DTO 577、Common 241、Windows recording 323、Touch recording 319、Pen recording 262 项零失败；实际键鼠 71 项、Touch 62 项和 Pen 产品链均零失败、无 skip。完整 9 组回归、整屏输入、跨进程编辑器及 ScriptModel Agent 原生链路也全部通过。统一证据见 `2026-11-17-desktop-input-final-verification.md`。这些验证开发构建；虚拟设备和实时 repeat 保证不据此验收，也不宣称所有第三方应用均已实测。
