# 2026-11-16：按住鼠标时截图与拖动边界验证

用户要求实测“observe → click → 新 snapshot → type”，并指出滑块操作应能在不松手时观察、
调整，再确认松手；随后要求参考 Sunshine。Windows，源码基线 `7e4a520d`，本次只增加
既有原生 fixture 的断言及工具说明，没有改变输入实现。

## 可复现步骤和预期

测试入口：`DesktopComputerScreenshotChainNativeTest`，显式设置
`MAICHAT_RUN_SCREENSHOT_COMPUTER_CHAIN=1`，使用真实 Windows QPA、生产工具/控制器、
原生输入和屏幕采集。全部画面及输入属于本进程自建窗口，不访问网站、账号或用户数据，
不调用付费模型，也不操作实际验证码。

1. 清空自有输入框并移走键盘焦点，display observe 获取图片与 S1，在这张绑定图片上选点。
2. 用 S1 点击输入框；确认窗口焦点、新图片及新的 `observation.snapshot_id=S2`。
3. 旧 S1 输入被拒绝；S2 一次输入完整文字；核对控件文本与下一张图片/新 token。
4. 自建滑块中，仅由 fixture 发送一对原生 down/up；down 后分别调用 screenshot capture
   和 display observe，核对 OS 按键状态仍 down，接收控件仍 held，release 事件计数为零。
5. fixture 释放自己的按钮后，通过真实模型工具入口执行两次完整 drag：先故意短 40 个
   Qt 逻辑单位，再用新绑定图片把手柄拖至目标。核对按下、带按键移动、抬起事件和结果图。

第 4 步用于验证采集的独立性，不是模型已经拥有分步 down/up 能力，也不是绕过生产中
“用户已按住鼠标则拒绝输入”保护的方案。失败退出有 RAII 释放，仅释放 fixture 自己按下的按钮。

## 实际结果与根因

- 普通增量 target 构建 exit 0；整组 Qt **11 passed / 0 failed / 0 skipped**，35.268 秒，
  包含 8 个文字样本、1 个按住/拖动场景及 init/cleanup。
- 8 个样本均通过准确的 display S1 点击 → S2 输入链路，覆盖 ASCII、BMP 中文、emoji
  首/中/尾、CJK+emoji、ZWJ 和连续 pair。旧 token 拒绝，实际控件全文读回正确。
- 按下期间 screenshot 和 observe 都成功取图，OS 仍 down，控件 release 计数均为 0。
- 短拖返回时手柄没有对齐，鼠标已经 up；返回 `dispatched=true, verified=false`。
- 对齐拖返回时手柄到达目标，鼠标也已经 up；仍是 `dispatched=true, verified=false`。
- 已实际查看 held 截图和 short-drag 截图；画面分别显示按住状态、松开状态和未对齐位置。

源码因果链：`DesktopComputerWindows.cpp` 的 Drag 分支内部执行 hold、插值 move、release，
之后才返回。`DesktopComputerController::perform` 的 display 分支等待 native input 返回后
才调用 observeDisplay。因此松手来自完整 drag 的语义，采集本身没有要求先松手。

当前 action 枚举没有独立 mouse-down/up，也没有跨调用的按住状态所有权。现有工具无法让
模型保持按下、跨一次图像推理后再调整。这里只确认限制并补充说明，不宣称该能力已实现。

## Sunshine 参考

固定源码：`bb6930bd782ed3edd2eea9a71d46b42183be9358`，提交时间
2026-11-15T21:14:04Z。原 LizardByte URL 当前重定向到 SunshineStream；以固定 commit 核对。

- [input.cpp:932](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/input.cpp#L932)：
  按下和抬起是独立输入包，保存 `mouse_press`；移动由另一输入包处理。
- [virtualhid_input.cpp:883](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/platform/virtualhid_input.cpp#L883)：
  move_relative、move_absolute 与 button 分开；普通移动不附带抬起事件。
- [video.cpp:1604](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/video.cpp#L1604)：
  图像采集使用独立 captureThread；采集路径没有逐帧释放鼠标的操作。
- [stream.cpp:2315](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/stream.cpp#L2315)、
  [input.cpp:2318](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/input.cpp#L2318)：
  会话停止时 reset，释放被记录为按下的鼠标按钮和键盘按键，防止卡住。

可借鉴的是输入状态与视频采集分开、显式松开和生命周期清理的设计。若为模型增加同类能力，
还需确定跨工具调用的会话所有权、截图时不改变按键、取消/超时/失焦清理和释放入口；
Sunshine 的串流输入协议不能直接当作当前模型工具的已实现契约。

## 产物与验证边界

正常缓存 `MaiChat/desktop/.cache/build-qt6-ui-20261109`；证据目录
`.cache/desktop-coordinate-flow-20261116/`，包含构建日志、native.log、逐样本 chain JSON、
held-slider JSON 和原始截图。测试运行时核验实际加载的 qwindows.dll SHA256
`28fc68b25b5efce19c813844f5597ada72c3604d8735156cf7422863c15ff264` 和 Qt6Gui.dll
`359f8042598672569c4ae09b24765915643b1a242b4722554f8aa9487943c89e`。

补充 drag 边界说明后，普通 controller/tool contract exit 0；同一缓存重新构建并对
ASCII 完整流程和 held-slider 两槽定点复测，Qt 4/0/0，6.209 秒，实际 PID 46192。
启动前依赖门 6 binaries / 88 imports 通过，测试 EXE SHA256
`872995cc6d2ad616831cdd9948ab206c28c5a9f33eea1dac5a7940b5b020c7df`。
具体源码摘要、产物、PID 和退出码见 `final-native-receipt.json`，输入实现没有改动，
原有三路径 WIP SHA256 保持。

这是普通工具入口与真实原生输入测试，未覆盖实际大模型识图、审批弹窗和跨屏状态保持；
Sunshine 仅审阅源码，没有安装运行。原用户 MaiChat PID 56404、路径
`D:\Program Files\MaiChat\maichat.exe` 在前后检查中保持，未替换或停止。
