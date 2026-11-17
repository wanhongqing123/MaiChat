# Sunshine 与 MaiChat 电脑操作：逐项对照

日期：2026-11-17。比较对象：Sunshine 固定版本 `bb6930bd782ed3edd2eea9a71d46b42183be9358`，MaiChat Windows 输入实现 `94f785d7`，操作说明修订 `9540927c`。MaiChat 现装程序尚未替换。

**先更正结论：此前“排除虚拟设备和手柄，其余已严格 1:1”的说法过满。主要输入种类已有对应实现和开发构建验证，但还存在下文列明的状态、参数、兼容策略及宿主功能差异。通过接收窗口测试，不等于这些差异不存在。**

这份清单区分四层：具体输入能力、配置参数、MaiChat 的模型调用入口、已经排除的设备功能。模型实际调用的是一个 `desktop_computer` 工具；25 个 `action` 是它的操作选择，`input_events` 则是其中一个可装入细操作列表的 action。

标记含义：

- **同类功能**：两边可以完成这类操作；具体参数或边缘规则另列。
- **行为不同**：有可从固定源码证明的差异，不能算严格等价。
- **MaiChat 扩展**：面向模型任务新增的包装、查询或约束。
- **共同限制**：固定版本的普通 Windows 路径和 MaiChat 都没有对应原生映射。
- **排除**：按用户要求不对齐。

以下比较的是桌面输入及其直接相关控制。Sunshine 的视频编码、音频和配对属于其他子系统。上游依赖源码只用于只读核对，没有重新接入 MaiChat。

## 一、鼠标

**1. 相对移动——同类功能，范围不同**

Sunshine：接收横、纵位移，协议每轴为有符号 16 位数。

MaiChat：`input_events → relative_move`，`delta_x/delta_y` 为有符号 32 位数，走 Windows 原生相对移动。仍受选定目标及观察绑定限制。

**2. 绝对位置移动——同类功能，坐标规则不同**

Sunshine：客户端传位置和参考宽高，服务端处理视频留白、缩放、显示原点及物理/逻辑坐标。

MaiChat：`move` 或 `absolute_move`，传目标内 0～1 的比例，依据本次观察的窗口/显示器原生边界定位。预览留白和裁剪须先还原；没有直接接收 Sunshine 那套视频参考平面参数。端点使用 `size-1`，部分上游普通 Windows 映射使用 `size`，边界可能相差一像素。

**3. 左键——双方支持**

Sunshine：左键独立 down/up。MaiChat：`button=left`，支持单独按下、保持、松开及组合操作。

**4. 右键——双方支持**

Sunshine：右键独立 down/up。MaiChat：`button=right`；`right_click` 是固定右键的便利入口。

**5. 中键——双方支持**

Sunshine：中键独立 down/up。MaiChat：`button=middle`。

**6. 后退侧键——双方支持**

Sunshine：X1，按钮编号 4。MaiChat：`button=back`。

**7. 前进侧键——都有注入，状态追踪不同**

Sunshine：X2，编号 5；固定版本的状态数组大小与索引判断使编号 5 没有进入同一套重复过滤/reset 追踪。

MaiChat：`button=forward`，五个按钮都进入持有、去重和清理表。这是行为修补，不能说两边状态规则完全一样。

**8. 持续按住并移动——双方能表达，所有权不同**

Sunshine：通过按钮 down、持续移动、按钮 up 实现。

MaiChat：`mouse_down` 后可跨调用移动、观察、截图，再 `mouse_up`；鼠标普通入口和 typed 按钮入口共用按钮身份，可交叉释放。本机回归已验证截图不会主动松开。

**9. 纵向滚轮——双方支持**

Sunshine：120 单位一格，支持不足一格的细量，协议距离为有符号 16 位。

MaiChat：`wheel` 的 `axis=vertical`、`delta_120` 为有符号 32 位；正值向上。`scroll` 另外提供按行滚动和位置包装。

**10. 横向滚轮——双方支持**

Sunshine：独立横向滚轮扩展包。

MaiChat：`axis=horizontal`，正值向右，支持细量。

**11. 关闭高精度后的整格滚动——同类功能，reset 不同**

两边均按横、纵轴分别累计不足 120 的余数，只发送整格量。

Sunshine 固定 reset 路径没有清这两个余数；MaiChat 的 reset/release 清零余数。

**12. 绝对移动后的左右键兼容时序——行为不同**

Sunshine：绝对移动后可能把左键 up 延迟 10 ms，窗口期内右键 down 可能转换成右键 down/up；相对移动会关闭这个延迟。

MaiChat：没有照搬该自动兼容状态机，按请求执行按钮按下和释放。

**13. 相对位移自动转成绝对位置——没有专用对应**

Moonlight 客户端另有保留虚拟光标并将相对位移转换为绝对包的 helper。这不是 Sunshine 新增的一种服务端输入包。

MaiChat 没有该专用虚拟光标 helper；调用者可自己累计坐标，再调用 `absolute_move`。不能把“可以自行组合”写成已存在同一 helper。

鼠标依据：Sunshine `input.cpp:783–989,1308–1346,1857–1929,2318–2323`；MaiChat `DesktopComputerWindows.cpp`、`DesktopComputerWindowsScope.h`；见文末固定源码链接。

## 二、键盘与文字

**14. 物理键按下、保持、抬起——双方支持**

Sunshine：原始键事件。

MaiChat：细操作 `raw_key`，另有命名的 `key_down/key_up`。同一原生绑定可以由多个逻辑身份共享，释放最后一个身份才真正抬键。

**15. 常用键、方向键、功能键、小键盘及左右修饰键——都有对应**

Sunshine：通过键码及 extended 标志表达。

MaiChat：raw 键码和命名键；`list_keys` 可查语法。支持区分左右修饰键和小键盘 Enter。Windows 无可靠独立映射的命名硬件键会明确报不支持。

**16. 归一化/非归一化键码——字段对应，实际执行范围需限定**

Sunshine：保留 16 位键码；旧 `0x8000` 前缀取低 8 位，flags bit 0 表示非归一化。

MaiChat：保留相同字段；但 Windows 执行路径要求映射后的有效 VK 小于 `0xff`。更高值要先映射到可用 VK，否则拒绝。不能把 DTO 接受 16 位写成任意 16 位硬件码都可执行。

**17. 扫描码与扩展键——同类功能**

两边都有扫描码偏好和 extended 键身份。

MaiChat 首次 down 固定原生绑定，后续 up/repeat 使用原绑定；模型没有独立“传任意 native scan_code 整数”的公开字段。

**18. 临时补 Shift/Ctrl/Alt——双方有，判断规则不同**

Sunshine：根据重映射前的客户端持有状态判断是否补修饰键。

MaiChat：根据重映射后的本机绑定和当前键状态判断。例如先把 Alt 映射为 A，再发带 Alt 的 B，两边可能得到不同的实际修饰键组合。

**19. 临时补 Meta/Win——MaiChat 扩展**

Sunshine 此合成分支只补 Shift/Ctrl/Alt。

MaiChat 的 raw 修饰掩码还会补 Meta/Win。双方都能直接发送 Meta 本身，区别在“自动补齐”规则。

**20. 键码重映射——都有，应用时机不同**

Sunshine：通过 `keybindings` 重映射；repeat/reset 发射时仍查询映射。

MaiChat：`key_bindings` 在 raw 首次 down 应用，up/repeat 冻结绑定，避免持有期间配置变化抬错键。

**21. 右 Alt 转 Win/Meta——双方有**

Sunshine：`key_rightalt_to_key_win`。

MaiChat：`right_alt_to_meta`，默认关闭，作用于 raw 首次 down；普通命名 AltRight 不自动经过这套 raw 重映射。

**22. 重复 down 与未知 up——常规对应，别名边界不同**

普通同身份的重复 down、未知 up，两边均可忽略。

Sunshine 先去旧前缀/规范编辑键码再建立身份；MaiChat 按原始 `code/flags` 保留调用者身份。因此把同一个 A 的 `0x8041` 和 `0x0041` 混用，在 MaiChat 可成为同一物理键的两个逻辑持有，不能认作逐事件等价。

**23. 命名键与 raw 键交叉释放——不能笼统承诺**

MaiChat 普通接口和模型接口确实共享执行核心，但 `key_up` 只找命名身份，raw up 只找 raw 身份。它们不会自动清掉另一种身份持有的同一物理键。

鼠标的普通/typed 身份已经统一；不要把这个结论扩大到任意键盘别名。

**24. 按住后自动重复——raw 功能对应**

Sunshine：最后一个新按下的键替换此前的全局 repeat。

MaiChat：最后一个新持有的 raw 键替换该会话 repeat，验证目标并复用首次绑定。普通命名 `key_down` 不新建这套 raw repeat。

**25. 首次重复延迟与重复周期——对应，可调**

默认首次延迟均为 500 ms，频率 24.9 Hz 对应周期约 40.16064 ms。延迟 0 可关闭调度。

MaiChat 使用毫秒周期，允许小数，另有显式布尔开关；周期是调度参数，不是实时到达保证。

**26. Unicode 文字——双方支持，封装上限不同**

Sunshine：UTF-8 文字，由 Moonlight 拆成不截断码点的至多 32 字节网络包。

MaiChat：`type`/`unicode_text` 一次最多 4000 UTF-8 字节，Windows 转 UTF-16，支持代理对。对受保护输入、未释放修饰键等有额外门禁。

**27. 专用 Undo/Cut/Copy/Paste 键身份——有明确边界**

两边都会把指定非归一化编辑键码转为 `0x100..0x103`。

Sunshine 会继续转交平台；MaiChat 未配置到可用 VK 的映射时明确不支持，不会误发 F11～F14。上游“已转交”不等于证明 Windows 最终能注入这些专用键；常见 Ctrl 组合键是另一种可用表达。

**28. Ctrl+Alt+Shift+F1～F13 宿主选屏——没有相同快捷键**

Sunshine：截获该组合，选择显示器 0～12。

MaiChat：有 `list_displays` 和选择显示器的观察/输入能力，没有这组相同的宿主快捷键拦截。

**29. Ctrl+Alt+Shift+N 远程光标显示开关——没有相同入口**

Sunshine：截获组合，切换捕获/串流中的光标显示。

MaiChat：当前 `desktop_computer` 没有同样的光标显示切换动作。

键盘依据：Sunshine `input.cpp:998–1300,1354–1360`、`config.cpp`；MaiChat `DesktopComputerInputTypes.cpp`、`DesktopComputerWindowsKeyMap.cpp`、`DesktopComputerWindowsScope.h`、`DesktopComputerInputSession.cpp`。

## 三、触摸屏

**30. 多点数量——双方 Windows 路径最多 10 点**

Sunshine 逐包接收单触点，普通 Windows 后端提交完整活动点帧。

MaiChat 一次 `touch.contacts` 可传 1～10 项；活动点上限为 10。

**31. 接触点身份——都有，编号范围/映射不同**

Sunshine 协议为 uint32，但固定普通 Windows 路径转有符号 ID 后拒绝负数，不能据此承诺所有高位 ID 都能执行。

MaiChat 保留完整 uint32 调用者 ID，映射到自己的 0～9 原生槽；调用者只须在后续 move/up/cancel 沿用自己的 ID。

**32. down/move/hover/up/cancel/hover_leave/cancel_all——都有对应**

Sunshine 的 touch 处理器忽略 button_only。

MaiChat 公开上述 7 类触摸阶段；touch 使用 button_only 会明确报错。

**33. 持续保持与观察截图——具备**

Sunshine Windows 活动指针定期刷新。

MaiChat 触摸 source 使用正常 50 ms 报告周期，按住期间可以观察或截图；实际双点持续、十点已验证。这里没有实时到达保证。

**34. 同批建立多点——都不能简单理解成同一瞬间按下**

Sunshine 是逐包扩展活动点帧。

MaiChat 整批预检后按顺序建立完整帧，初始接触扩展经过 source 报告周期。`contacts` 数组不是原子同时手势。

**35. 移动单点、抬起单点、取消单点并保持其他点——双方有对应**

MaiChat 已在真实接收窗口验证 peer 身份及接触持续，没有把单点取消说成取消全部。

**36. 满 10 点时释放后复用一个槽——双方可表达**

MaiChat 同批先 up 再 down 可复用；先 down 再 up 不能在已经满槽时凭空容纳第 11 点。原生有序复用和其余 9 点持续已验证。

**37. 接触压力——同类功能**

双方接收 0～1 的压力；普通 Windows 路径将已知压力映射到 0～1024，未知值处理依各自 mask 规则。

**38. 触摸悬停距离——共同 Windows 限制**

协议/DTO 可以携带距离，但双方这里都没有真正的原生距离输出。不能因为字段存在而列成已实现距离控制。

**39. 触摸方向——都有字段，映射不同**

Sunshine 固定转换会折叠方向并按其未知/零值规则置 mask。

MaiChat 保留已知/未知区分，使用自己的角度转换，包括 `(rotation % 360 + 270) % 360`。相同数字不保证两边逐字段相同。

**40. 接触椭圆/接触框——都有，计算不同**

Sunshine 先做视频坐标缩放，固定 Windows 后端以旋转轴的绝对值之和计算矩形。

MaiChat 使用旋转椭圆的 `hypot` 包围框，补缺轴并裁剪虚拟桌面。公式、缺轴及 hover 面积处理不是严格等价。

**41. 接触改悬停或 hover_leave——原生事件不同**

Sunshine 固定 Windows 某些路径清接触标志后提交 UPDATE，没有显式 UP。

MaiChat 对原先接触的点提交 UP，再保留 hover 或离开；因此状态边缘不能标为完全相同。

**42. 取消全部及故障清理——目的相同，生命周期不同**

Sunshine 按其流及设备状态清理。

MaiChat 清自己的 source，记录失败、保留重试所需信息；停止时先撤销 epoch，遇在途工作可以返回 Busy，Busy 不表示已经清空。

触摸依据：Sunshine `input.cpp:1472–1528`、`virtualhid_input.cpp:954–995`；固定上游普通 Windows 后端 `windows_backend.cpp:2692–2852`；MaiChat `DesktopComputerWindowsTouch.*`。

## 四、笔与橡皮

**43. 单笔定位、接触、移动、悬停、抬起和取消——都有对应**

两边这条路径都是单笔，没有调用者多笔 ID 字段。MaiChat 入口为 `input_events → pen`。

**44. 笔 up 与离开感应范围——行为不同**

Sunshine 固定 Windows release 清除接触和 in-range。

MaiChat 的 up 结束接触但保留悬停/缓存位置，hover_leave 才离开。两者对 up 不能视为同一状态转换。

从接触直接改为 hover 时，MaiChat 显式提交 UP；固定 Sunshine Windows 后端清除接触标志并提交 UPDATE。

**45. button_only——行为不同**

Sunshine 协议说明称忽略几何，但固定实现改键后仍构造非接触工具状态并调用定位。

MaiChat 只在已有位置上修改按钮/工具，保留接触、位置与倾斜；没有原位置时不能凭 button_only 新建一次按压。

**46. 笔/橡皮工具选择——都有，切换序列不同**

两边都有 unknown/pen/eraser。

Sunshine 固定 Windows 路径只设置 ERASER 标志；MaiChat 根据 hover/contact 使用 INVERTED/ERASER，角色变化先结束旧笔画、离开再选择新工具。MaiChat 已验证真实橡皮输入，但不是复制上游逐事件序列。

**47. 笔压力——同类功能**

接触阶段的已知压力映射到 0～1024；未知及非接触阶段按各自状态/mask 处理。

**48. 倾斜和方位角——同类功能，零值/未知边界不同**

双方都把倾角及方位角转换成 native tiltX/tiltY。

MaiChat 处理 90 度和基轴数值边界，并显式提交零倾角；Sunshine 固定 Windows 后端对零值 mask 的处理不同。

**49. 三个逻辑笔侧键——Windows 都只有一个 barrel 位，合并规则不同**

Sunshine 协议有三位，但固定普通 Windows 后端忽略具体按钮身份，以最后一次逻辑变化覆盖一个 barrel 状态。

MaiChat 保留三位，用 OR 合并：只要任意一位仍按住，barrel 就仍按住。例从 `buttons=3` 改到 `2`，两边可能产生不同结果。不能说双方都原生拥有三个独立侧键。

**50. 笔悬停距离——共同限制**

双方的字段都不能据此证明实际输出了 Windows 原生悬停距离。

**51. 笔的接触椭圆——共同限制**

双方这条普通 Windows 笔路径都没有实现该原生映射。触摸屏接触框与笔接触椭圆是两回事。

**52. 笔杆自转——共同限制**

这里的 rotation 用于倾斜方位角，不是另一个笔杆自转参数。双方都不能据此宣称已输出独立 shaft twist。

**53. 保持刷新——都有，周期不同**

Sunshine 固定普通 Windows 活动指针刷新为 50 ms。

MaiChat 笔刷新为 16 ms；周期不同，错误处理也不同，不能写成调度完全相同。

**54. 取消、释放与错误状态——目的相同，回执不同**

Sunshine 主要记录后端错误，不返回目标应用处理完成确认。

MaiChat 尝试 up/leave/destroy，保留同一释放 epoch 的真实错误；模型可得到结构化失败和状态。API 派发成功仍不等于应用完成。

笔依据：Sunshine `virtualhid_input.cpp:998–1076`；固定普通 Windows 后端 `windows_backend.cpp:2917–3030`；MaiChat `DesktopComputerWindowsPen.*`。

## 五、顺序、会话与安全约束

**55. 输入顺序——都有顺序，调度方式不同**

Sunshine 每流队列串行派发，并对部分同类运动包合并。

MaiChat `input_events` 是 1～256 项显式顺序列表，总延迟不超过 5000 ms，不主动复制其网络包压缩；不是全部成功或全部回滚的事务。

**56. 运动包合并——不是同一能力**

Sunshine 同参考尺寸绝对位置可取最后一个；touch/pen 运动按身份、阶段和状态条件合并，按下/抬起等边缘不随意合并。固定相对/滚轮求和分支与注释还存在矛盾，不能只读注释宣称效果。

MaiChat 没有复刻 Moonlight 可靠/不可靠通道或该网络队列策略；模型可以显式安排动作和延迟。

**57. 持有状态的作用域——不同**

Sunshine 既有全局键鼠/repeat 状态，也有每流输入状态、滚轮余数及配对客户端上下文；输入配置是宿主全局配置。

MaiChat 按原生 session、任务 turn 和窗口/显示器目标管理，持有期间禁止不释放就换目标；观察 token 和默认 120 秒空闲清理属于新增宿主约束。

**58. 停止/断开清理——Sunshine 已有，MaiChat 也有**

Sunshine 流停止会 reset，取消 repeat 和延迟工作，释放追踪到的键鼠、处理设备状态。

MaiChat 在 turn 完成、取消、失败、clear、超时、析构时清理自己的控制量。不要把它描述成 Sunshine 没有断开释放。

**59. reset 与退役——作用域不同**

MaiChat `reset_inputs` 保留仍有效的 context/config；`release_all` 释放并退役 ownership，失败保留重试绑定。

Sunshine 的 reset、客户端保留设备和重新连接生命周期与这两个模型 action 不是同一接口。

**60. 配置关闭后的释放——不同**

Sunshine 某些键鼠 passthrough 入口关闭后直接忽略，包括 up。

MaiChat 仍允许纯释放，不因开关关闭、旧 token、失焦或取消而阻止清理。

**61. 能力查询——都有，但内容不同**

Sunshine 有平台能力位及协议能力广告。

MaiChat `input_capabilities` 返回分功能 implemented/available/前置条件。两边的能力声明均不是目标应用 ACK。

**62. 非法参数与失败回执——不同**

Sunshine 某些数值取 clamp，畸形包丢弃，平台失败主要记录。

MaiChat 明确校验 finite、范围、重复身份和几何，返回 InvalidInput/NotSupported/Busy/Canceled 等。部分事件可能已经发出，失败后尝试清理；不能盲重放已消费 token 的整批操作。

## 六、10 项输入配置对应

这些是参数，不是另外 10 个工具。MaiChat 在 `input_events` 中传 `input_options`；省略表示继承，显式 `{}` 表示恢复默认。

1. **键盘开关**：Sunshine `keyboard` ↔ MaiChat `keyboard_enabled`，默认开；纯释放行为见第 60 项。
2. **鼠标开关**：`mouse` ↔ `mouse_enabled`，默认开；MaiChat 也以此门控 Touch/Pen，纯释放清理另行允许。
3. **Touch/Pen 开关——行为不同**：Sunshine `native_pen_touch` 影响能力广告，实际 touch/pen passthrough 检查 `mouse` 和 viewport；MaiChat `native_pen_touch_enabled` 是每 context 的实际派发门控，默认开，仍须后端可用。
4. **扫描码偏好**：`always_send_scancodes` ↔ `always_send_scan_codes`，默认开；MaiChat 作用于 raw 键解析。
5. **右 Alt 转 Win**：`key_rightalt_to_key_win` ↔ `right_alt_to_meta`，默认关；MaiChat 为 raw 首次 down 策略。
6. **高精度滚轮**：`high_resolution_scrolling` ↔ `high_resolution_scroll`，默认开；关时累计整格余数。
7. **重复开关**：Sunshine 可用首次延迟 0 禁止；MaiChat 另提供 `keyboard_repeat` 布尔值，默认开。
8. **首次重复延迟**：`key_repeat_delay` ↔ `repeat_delay_ms`，默认 500 ms；MaiChat 接受 0～2147483647 的整数毫秒。
9. **重复频率/周期**：Sunshine 默认 24.9 Hz；MaiChat `repeat_period_ms` 默认 `1000/24.9`，接受有限正小数，最大 2147483647 ms。
10. **键码映射**：`keybindings` ↔ `key_bindings`；MaiChat 最多 65535 对，键码 1～65535，原码不能重复；默认通用 Shift/Ctrl/Alt 映射左侧。应用时机及修饰键语义见第 18～21 项。

**配置作用域须看清：**键鼠开关对普通和 typed 输入都有门控；扫描码偏好、raw 映射、右 Alt 转换和 raw 自动重复不能笼统解释成每个命名 `key/key_down` 都执行同样配置。

## 七、MaiChat 的 25 个 action，一一对应

Sunshine 提供底层原语，通常没有同名“大模型 action”。以下说“可组合”表示原语能表达，不能误读为 Sunshine 已有相同模型接口。

1. **`list`：列窗口。** MaiChat 扩展；Sunshine 输入接口没有同名窗口目录 action。
2. **`list_displays`：列显示器。** MaiChat 扩展；Sunshine 有选屏及 viewport 元数据。
3. **`list_keys`：查命名键和组合语法。** MaiChat 扩展；查询不发按键。
4. **`observe`：获取目标观察和快照。** MaiChat 的模型观察接口；Sunshine 有视频画面，但没有相同的 snapshot/UIA action。
5. **`focus`：激活目标窗口。** MaiChat 高层入口；不是 Sunshine 的独立输入包。
6. **`move`：绝对移动鼠标。** 对应绝对鼠标原语，坐标契约见第 2 项。
7. **`click`：单击。** MaiChat 包装“移动＋按钮 down/up”。
8. **`double_click`：双击。** MaiChat 包装两次 down/up。
9. **`right_click`：右击。** MaiChat 的固定右键包装。
10. **`drag`：完整拖动。** 包装 down、移动、up；成功返回前配对松开，失败会尝试清理，up 失败保留重试绑定；不验证视觉对齐。Windows 窗口路径当前时长上限 200 ms，显示器路径上限 5000 ms；需要过程中观察时用单独 down/move/up。
11. **`mouse_down`：按住鼠标。** 对应按钮 down，加位置及 owner 约束。
12. **`mouse_up`：释放鼠标。** 对应按钮 up，只处理本会话持有。
13. **`type`：输入文字。** 对应 Unicode 输入，加目标和安全校验。
14. **`replace_text`：全选并替换。** MaiChat 组合入口，可用键＋文字原语表达；不保证业务保存。
15. **`key`：命名键或组合键短按。** MaiChat 解析名称，再组合键事件。
16. **`key_down`：保持一个命名键。** 对应 down，不接受整个 chord；raw repeat 规则另列。
17. **`key_up`：释放一个命名键。** 对应命名身份 up，不自动清除另一个 raw 身份。
18. **`scroll`：按行滚动。** 包装位置与双轴 wheel；更细量可用 `wheel`。
19. **`input_state`：查询本会话持有状态。** MaiChat 扩展；Sunshine 有内部状态，但没有同名模型查询。
20. **`reset_inputs`：回到中立。** 与 Sunshine reset 目的相关，scope/回执/保留项不同。
21. **`release_all`：释放并退役会话。** 与 Sunshine 停止/断开清理目的相关，不是同一生命周期接口。
22. **`verify`：读回焦点文字并比较。** MaiChat 扩展；不表示业务提交或文件保存已成功。
23. **`clear`：清除目标/观察并清理输入。** MaiChat 扩展；不删除输入框文字。
24. **`input_events`：执行细操作列表。** 包含 absolute_move、relative_move、mouse_button、wheel、raw_key、unicode_text、touch、pen 八类；不是 Moonlight 网络包队列。
25. **`input_capabilities`：查询后端能力。** 对应能力发现的目的，返回结构与 Sunshine feature bits 不同。

## 八、按用户要求排除的部分

**设备模拟/驱动层：排除。** 硬件 HID 身份与报告、Raw Input 可见的虚拟键鼠设备、驱动/许可/broker、ViGEm 设备、虚拟设备重建/重绑定、稳定设备 ID/MAC 策略，都不作为当前功能承诺。普通系统键鼠、触摸和笔输入不等于这些设备模拟功能。

**手柄型号：排除。** `generic`、Xbox 360、Xbox One、Xbox Series、DualShock 4、DualSense、Switch Pro。`auto` 是选择策略，不是第八种硬件能力。

**手柄输入与状态：逐项排除。**

1. 接入/分配及类型、能力、按钮声明。
2. D-pad、ABXY、Start/Back/Home、肩键、摇杆按下、扩展按钮位。
3. 左右摇杆四轴及双扳机。
4. 控制器触摸板接触点与压力。
5. 加速度计和陀螺仪。
6. 电池电量及充电状态。
7. active mask、断开、保留后重绑等生命周期。

**手柄反馈：逐项排除。** 主震动、扳机震动、运动采样订阅、RGB、玩家灯、自适应扳机。Sunshine 声明/发送能力也不代表每个 profile 和客户端都完整接收。

**对应 7 类事件不向模型公开：** `gamepad_attach`、`gamepad_state`、`gamepad_touch`、`gamepad_motion`、`gamepad_battery`、`gamepad_detach`、`gamepad_rebind`。后两项是 MaiChat 的历史抽象名，不是 Sunshine 同名独立 wire 包。

**对应配置也排除：** controller 开关、DS4 Back 映射触摸板、运动/触摸板按 PlayStation 映射、随机控制器 MAC、driver、default profile、Back 长按转 Home。历史数据类型仍存在，不算执行能力。

## 九、宿主/测试辅助接口，单独说明

**63. 查询实际鼠标位置**

Sunshine `get_mouse_loc` 的公共注释明确为平台输入测试提供；Windows 实现调用 GetCursorPos。

MaiChat 内部也读 GetCursorPos 做命中检查，但 25 个 action 没有独立的“返回当前鼠标坐标”操作。这属于辅助接口差异，不能暗中算同名模型功能。

**64. enable_mouse_keys 声明**

Sunshine 公共平台头声明此宿主辅助接口；本清单没有把公共声明直接当成已核的 Windows 具体行为。

MaiChat 没有同名模型 action。它不应与“可以发送小键盘键/普通鼠标事件”混成一项能力。

**65. 连接后的微小鼠标唤醒**

Sunshine 分配输入后有延迟的 `(+1,+1)` 再 `(-1,-1)` 捕获唤醒副作用。

MaiChat 没有复刻该连接副作用；这也不是用户可选择的一种普通输入动作。

## 十、验收与来源

现有记录证明：Windows 开发构建中键鼠 71 项、触摸 62 项、笔的普通/工具真实接收链路均通过；另有 9 组回归、窗口发现 3002 项、跨进程编辑器、整屏输入和 ScriptModel Agent 验证。

Touch/Pen 的 319/262 是组件合同检查数，不能写成 319/262 个实际第三方应用已验收。实际测试使用自有接收窗口；未替换已安装 App，不证明所有应用、每个数值组合或 Mac 平台均已验证。

这份逐项对照将“原语已有对应”和“行为严格相同”分开。后续是否需要改变已列差异，应逐项讨论，不能再用一个笼统的 1:1 覆盖。

固定上游一手来源：

- [Sunshine 输入处理、兼容序列、键盘、触摸、队列与 reset](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/input.cpp)
- [Sunshine 输入配置](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/config.cpp)
- [Sunshine 平台转换](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/platform/virtualhid_input.cpp)
- [该固定版本使用的普通 Windows 后端：按键、Touch/Pen 原生映射](https://github.com/LizardByte/libvirtualhid/blob/2234c0079a6a7b0e2f25990d3c705be85bcd37fd/src/platform/windows/windows_backend.cpp)
- [Sunshine Windows 平台输入及测试鼠标位置查询](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/platform/windows/input.cpp)
- [Sunshine 公共平台接口及 get_mouse_loc 用途说明](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/platform/common.h)
- [固定 Moonlight 客户端发送实现及 relative-as-absolute helper](https://github.com/moonlight-stream/moonlight-common-c/blob/94c65fd364d17b223f00a8763c9a5d94a66cb8dd/src/InputStream.c)

MaiChat 依据：`MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp`、`DesktopComputerInputProtocol.cpp`、`DesktopComputerInputTypes.*`、`DesktopComputerInputSession.cpp`、`DesktopComputerInputOwnership.*`、`DesktopComputerWindows.cpp`、`DesktopComputerWindowsScope.h`、`DesktopComputerWindowsKeyMap.cpp`、`DesktopComputerWindowsTouch.*`、`DesktopComputerWindowsPen.*`。实际产物记录见同目录 `2026-11-17-desktop-input-final-verification.md`。
