# desktop_computer：窗口状态与控件 ID

日期：2026-11-18。Windows 源码基线：
`223bdb08ac69616a3020ee0cf8b403b8dd9fc0d6` 加本次 action 增量。
本记录随最终源码和验收更新；不表示已替换用户正在运行的安装版。

本轮共享四源最终供件 SHA256 为
`0c78c470c76ca8cd2f255edc64e2c992a6d4be8fcbf5221348619bda9cb002dd`；
Controller Git blob 为 `16e55b1c570ebfe822bd4c68e06b40a8b222a14e`。
Windows 原生 fixture 源码 SHA256 为
`84022241feaabf7696e86c5e4e5eeb9a6d2f0ae1d91038eb67c79e8db1216431`。
实际状态用例进程 PID 64060，执行
`MaiChat/desktop/.cache/build-qt6-ui-20261109/bin/desktop_computer_actions_native_test.exe`，
执行文件 SHA256 为 `7a9ec1463f2fb984d252923d068473d8b369becbccc2e8e2c20a2df9119a6c84`。
同构建目录普通 `maichat.exe` SHA256 为
`a28ae22e5809dc3272ca437b099fa84dcce033f100eb73600e4cb30e0fd4f82f`；
这是已通过构建/runtime 检查的候选产物，尚未替换安装版。

## 用户操作与预期

Agent 先发现窗口并观察，再选择返回的控件 ID 点击。窗口最小化后可以先取得
状态观察，再恢复窗口。控件被移动、删除或换成另一个控件时，旧 ID 必须报错，
不得按旧坐标点击。停止任务后旧观察失效，旧任务的迟到清理不能影响新任务。

实现复用现有 Controller、输入所有权和 native input session。没有加入第三方库、
驱动、Shell 操作路径或 UIA Invoke 输入路径；原有坐标操作和精细输入事件保留。

## 普通 API 与模型调用

新增 `minimize`、`restore`；继续使用已有 `focus` 请求正常前台激活。状态操作返回
操作前后窗口状态及可读的焦点事实，并消耗旧观察。最小化或 cloaked 窗口可以只
返回状态，不能据此执行普通输入；其他无效几何仍拒绝观察。不跨虚拟桌面或绕过焦点策略。

`click`、`double_click`、`right_click` 可以使用当前观察返回的 `element_id`。
模型传 ID 时不传 `x_ratio/y_ratio`，工具重读控件身份、所属窗口、角色、文本、
可用状态和边界后，计算控件中心并走原有鼠标输入。激活窗口后还会再次核对。
ID 只属于该 Session 的该次观察，不是应用永久 ID，也不接受系统 native ID。

```json
{"action":"observe","target":"window","window_id":"123"}
{"action":"click","target":"window","window_id":"123","snapshot_id":"当前观察的token","element_id":"当前观察的控件ID"}
```

独立弹出菜单可先通过 `list` 获取其窗口 ID，再作为显示观察的控件来源：

```json
{"action":"observe","target":"display","display_id":"65537","element_window_id":"456"}
```

`element_window_id` 仅指定读取哪一个窗口的无障碍树，输入目标仍是 `display_id`。
没有把整个桌面无界遍历作为控件列表。系统或应用未暴露的控件仍需观察图像后
使用原有坐标方式，不能承诺每个任务栏图标、Qt 菜单都有可操作控件 ID。

## 坐标与覆盖范围

窗口和观察元数据的 `native_screen_bounds_unit` 明示 Windows 物理屏幕像素，
Mac 则是屏幕点。控件 `bounds` 和中心比例相对于本次选定的窗口或显示器。
PNG 尺寸单独报告，不能当作 native 输入坐标。选择 ID 时这些换算由工具完成。

无障碍结果分别报告 provider、status、visited_nodes、returned_elements、
provider_elements、counts_known、truncated 和 limit_reason。其中 returned_elements
是经过目标范围过滤后实际交给模型的数量。无法证明完整遍历时，不把部分集合里的
单个匹配当作唯一身份。无障碍不可用与截图/坐标输入是否可用分开报告。

## 失败与取消

失败保留错误码，并给出 input_dispatch_attempted、window_operation_attempted、
当前 owned input_state 和 needs_observation；不编造已接受的事件数。
OS 已排队的最小化请求可能在取消后生效，因此需要重新观察，不承诺回滚。

任务完成或 Stop 会原子撤销并移出匹配 turn 的快照；执行中的 worker 用局部
shared_ptr 保活并看到撤销。native 引用在 map 锁外释放。旧 turn 清理保留新观察。
取消清理仍释放自有按键，即使被取消的请求带 dry_run；正常 dry_run 不注入输入。

## 验证记录

- 审查反例：引用生命周期 4 个断言、不完整解析 6 个断言，真实 RED 共 10，
  `.cache/desktop-actions-review-red-test.log`，1.51 秒。
- 修复后普通 App 构建及 runtime 检查退出 0，3 个运行组件、114 项 PE 依赖与 Qt 插件。
  `.cache/desktop-actions-reviewed-green-build.log`。
- 同源码共享回归：contracts、input ownership、input events、input session
  4/4 通过，总 2.87 秒，`.cache/desktop-actions-reviewed-green-contracts.log`。
- Windows 原生用例首次因未能确认自有前台与整屏覆盖而跳过，退出 77；
  `.cache/desktop-actions-native-final-1.log`。这轮不计作原生通过。
- 最终缺字段诊断：缺 snapshot_id 两例实际 RED 2 后修复，普通构建/runtime 再次 0；
  contracts/ownership/events/session 再次 4/4 通过（1.60/0.56/0.10/1.43 秒）。
  `.cache/desktop-actions-fields-red-test.log`、`desktop-actions-fields-green-tests.log`。
- 原生二、三轮确认唯一阻塞是 foreground_not_owned；自有背景 outer/DWM bounds
  均覆盖 2560×1600，无其他进程遮挡，物理输入松开。一次普通生产激活 API 返回 Busy，
  未获得前台，诚实退出 77。`.cache/desktop-actions-native-final-3.log`。
- 已准备显式手动激活等待，默认关闭，最大 60 秒。仅在普通激活被拒后等用户点击
  自有标题栏；不再次请求焦点或注入输入，所有物理按键松开后才继续。此路径已编译，
  尚待用户配合实际运行，不计通过。
- 用户明确当前不在电脑前，因此没有启动手动等待。独立的 Windows 原生状态用例
  `desktop_computer_actions_native_state` 实跑 16 checks/0，5.85 秒。
  `.cache/desktop-actions-native-state-1.log` 及
  `.cache/desktop-actions-20261118/native-state-1/actions-native-state-only-receipt.json`。
  已知自有 HWND 的 UIA 遍历 9/9、available/nontruncated；同 ID fresh resolve、
  移动后物理坐标变化、销毁后拒绝及预取消均过。真实 minimize/restore 的 IsIconic
  回读通过；第 2 次取消检查发生在 OS 请求排队后，回读确见窗口最小化，再正常恢复。
  前后 foreground HWND 均为原窗口；没有请求焦点、截图或输入，明确 input_not_tested。
- 原计划中的 browser 回归失败于可见性前置条件，8.04 秒。只对自有网页 fixture
  临时追加一次末次观察日志后，8.00 秒重现明确错误
  `Agent browser tab is not visible; show it before continuing`。
  `.cache/desktop-actions-browser-diagnostic-test.log`。该测试不调用新 Controller，
  Browser/CDP 源码与基线无差异；临时测试改动已按原始 SHA 逐字节恢复。
  这项记作环境条件未满足、未通过，不修改产品可见性检查。
- Mac adapter 由 Mac 负责人单独接入、编译及验证；公共逻辑不重复验收。
  最终 MM blob `85a10e6ff14140098c842a3fe54b7c71699ef38f`；原三件供件加匿名控件
  两件最小增量各精核应用一次。所有 36 个 agent TU 与 shared4 v2 同新 ABI 编译，
  完整 archive/普通 consumer 链接退出 0。匿名控件修正后 MM 重编 0（2.697 秒），
  其余 35 个同 ABI 对象复用，链接 0（0.386 秒），原生权限守卫 14/0。
  AX/ScreenCapture 权限均 false，未请求权限；授权后的 AX/状态操作及点击未验。
  这是 consumer 编译链接证据，不是新的完整 Mac App 打包或安装验收。
- 最终合源检查：`.cache/desktop-actions-merged-final-build.log` 构建退出 0；
  `.cache/desktop-actions-merged-final-tests.log` 五项通过，总 9.46 秒：
  contracts 1.56、ownership 0.64、events 0.15（679 checks）、session 1.37（251 checks）、
  原生 state-only 5.72 秒（16 checks）。该组合不包含受前台/可见性限制的两项交互验收。

原生用例只操作自建窗口/按钮/菜单。显示截图通过自有覆盖与新帧标记检查才保存；
窗口 WGC 截图绑定自有 HWND，并检查覆盖及前台。
不操作用户窗口、读取用户消息、修改系统显示设置或替换当前安装程序。
