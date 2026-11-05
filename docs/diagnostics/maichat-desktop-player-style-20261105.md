# Desktop 播放器共用外观与 Windows 主窗口按钮位置

## 需求、操作与验收

- 时间：2026-11-05，Asia/Shanghai；起点 `732e2cfa145e5a94fcca31a27d5d941affb4b8a7`。
- 用户提供深色播放器参考和 Windows 云盘视频截图，并确认本轮先对齐外观和已有操作。
- Windows/macOS 使用同一份播放器界面：深色、自绘文件标题和窗口按钮、细进度条、统一紧凑图标、
  倍速与真实音量控制；继续从云盘、IM 和创作中心独立弹出，不改变主会话尺寸与草稿。
- 用户另要求 Windows 主窗口最小化/最大化/关闭三个按钮下移；按实际内容标题行对齐，给原操作留空间。

验收操作：打开自有无隐私测试视频，检查独立窗口和文件名；暂停/恢复、点击与拖动定位、切换倍速、
调整音量/静音、前后片边界、标题拖动/最大化/最小化/边缘缩放；进入全屏、自动隐藏/鼠标唤醒、
Esc 返回窗口，关闭后主会话、草稿和选择保持。云盘 Range 源取消与失败保留原错误路径。
Windows 实际构建及截图/录屏验证；当前没有连接 macOS，不能宣称其实际运行验收。

## 根因与设计

`MaiFfplayVideoDialog.cpp` 的图标、零边距、精细进度条、紧凑布局、全屏和自动隐藏原本仅在
`Q_OS_MAC` 分支；Windows 使用默认 Qt 控件和 40px 圆形动作按钮。
`MainWindow::openVideoWindow/openCloudVideo` 与工具备用入口又明确设置原生标题栏；
FFplay 标题回调在 Windows 覆盖成 `ffplay`。仅修改样式表不能移除这些来源。

采用共用自绘标题和底部控制区，原生 Metal/D3D11 渲染边界单独保留。
普通窗口的标题/控制区位于视频视口外；全屏时控制区作为同级原生窗口叠层，隐藏不改变视频视口。
这使 Windows 常态界面避开透明 QWidget 与 D3D11 HWND 的混合合成，同时保留全屏操作。
未采用复制 Mac 条件分支或再建一个播放器的方案。

音量复用 FFplay 事件线程：增加独立带 `struct_size` 的音量选项/状态，旧 Host 和 PlaybackStatus
ABI 保持；启动前恢复音量与静音，运行期 setter 和现有工具音量命令进入同一队列。
自绘窗口补真实拖动、缩放及窗口按钮；Windows 主窗口保留原生 resize/Snap/工作区边界行为。

## 实施顺序

1. 保存当前真实播放器 baseline；现有 Qt 测试截图可能被其它前台窗口遮挡，必须核对自有窗口身份，
   不能把底层 App 的像素算播放器通过。Windows 测试经既有 runtime/error-mode 包装器运行。
2. 先补界面/交互 RED、音量初始 PCM/重建/队列边界回归及主窗口按钮位置测试。
3. 收敛 `MaiFfplayVideoDialog` 共用 UI，统一三个入口的 Frameless flags，补音量持久和窗口交互。
4. 默认目标构建、PE/Qt 门禁，执行播放器、音频和主窗口相关回归；亲看自有窗口截图/录屏。
5. 核对来源与最终运行产物后提交；记录 macOS 未实机验证范围。发布按用户本轮指令及既有授权处理。

## 当前证据边界

初次 focused 运行因 PowerShell 5.1 把 FFplay 正常 stderr 提升为错误而提前结束，未算 UI 回归。
改用原包装器的独立进程和原始 OS 管道后测试退出 0，但截图实际被用户 App 遮挡；该截图不作为
播放器外观或非黑屏通过证据。未停止或替换用户正在运行的 App。
后续将通过可激活的自有测试窗口重新取证，并按同一用例验证最终界面。

## 已落地的实现与定点回归

- 共用自绘标题栏保留完整文件名，绘制时按实际宽度省略；窗口按钮 34px，去掉系统标题栏。
  IM、云盘和工具备用入口均使用相同窗口 flags。云盘没有本地文件动作，原 Range 读取路径保留。
- 窗口态标题栏 46px、控制区 84px；精细时间轴、五档倍速、音量菜单、静音和全屏使用共用图标。
  自动尺寸同样计入 130px 控制区域，避免两平台使用不同补偿值。
- 全屏按钮、F/Esc 和工具调用共用 Qt 窗口状态；退出全屏恢复进入前的最大化或普通状态。
  原实现的混合入口测试先失败，再以同一步骤通过。
- Windows 主窗口保留原生 resize、Snap 命中和工作区最大化范围，窗口按钮与内容标题对齐。
  设置页标题固定在滚动容器外。页面切换时同步排布标题行，缩窄窗口时同步收回按钮，再异步校准高度。
  三个窗口布局/原生操作回归均已在 Windows 实际测试程序执行过。

音量由 FFplay 事件线程消费同一 FIFO。新接口单独使用 `struct_size`，旧 Host 和 PlaybackStatus
结构未扩展。新增音量状态、幂等设置与序号回执；音量、静音和执行序号由同一原子快照读取。
准备期间保留用户意图，仅补发尚未入队的字段；启动参数带消费版本，避免旧 GUI 设置重放并覆盖后续
Agent 命令。停止并 join 后读取最终快照，保留切速/重播/换片前已入队的设置。
旧 `maiFfplayRunAtSpeed` 的 `-volume` 参数继续生效；NULL 扩展设置不覆盖原参数。

已记录的 RED→GREEN：

| 用例 | 初始证据 | 修复后定点结果 |
| --- | --- | --- |
| 共用 Frameless 外壳 | `red-owned-cdb.log` 原生边框断言失败 | `chrome-green-cdb.log` 通过 |
| 最大化→工具全屏→Esc | `fullscreen-mixed-red-cdb.log` 恢复状态失败 | `title-green-cdb.log` 通过 |
| 音量命令入队后立即切速/重播/换片 | `volume-restart-red-cdb.log` 4 条状态保持失败 | `volume-restart-green-cdb.log` 9 行通过 |
| 旧 API/NULL options 的 `-volume 0/37` | `argv-volume-red-cdb.log` 4 行失败 | `argv-volume-green-cdb.log` 4 行通过 |
| 重启准备期立即改音量/静音 | 首次录屏音量断言失败 | 延迟启动、后续 Agent 覆盖、启动意图不重放三个定点用例通过 |

日志均在 `.cache/desktop-player-style-20261105/`。序号回执为之后新增的收尾实现，最终交付仍需
以完整套件结果为准，不能拿较早的定点 GREEN 代替新接口的完整回归。

## 实际画面取证及发现

Windows QtTest 的原生视频/控制子窗可覆盖整个顶层客户区，导致顶层 `QWindow::isExposed()` 为假，
而实际窗口已可见。截图检查使用自有前台 PID、完整原生 Z 序、客户区采样和屏幕边界，实际画面还需
通过像素内容断言。Explorer 的 1px `ThumbnailDeviceHelperWnd` 经 `DwmGetWindowAttribute`
证实为 cloaked，不参加合成，按实际 DWM 可见性判断，不把它算成遮挡。

一次构建跨越音量新 API 落盘时出现测试 EXE/DLL 暂不一致；普通包装器明确返回 `0xc0000139`，
CDB 下留下本测试的系统入口点提示。已按标题、函数名及完整测试路径核实后仅关闭该测试提示，
重新清理相关 C/C++ 对象并构建所有目标，不能把这轮当成产品或界面通过。

彩条实录使用本地生成的 61 秒 `testsrc2` 文件，已亲看窗口播放、全屏、隐藏控件和恢复窗口截图；
字体使用与正常 App 相同的随包字体栈。`owned-evidence-9/` 包含六张阶段截图与 48 帧录屏。
逐帧检查发现退出全屏的第 43 帧视频区短暂空白，前后帧正确；已作为实际问题继续定位，尚未将该
录屏记为无闪帧验收。纯黑 `video-circle-61s.mp4` 只能验证控件，不能作为非黑屏证明。

### 退出全屏空帧的定位与修复

第 43 帧中心 800×350 的 280,000 个像素全为 `(16,16,16)`，即窗口背景；前后帧均有上万种
彩条画面颜色。Graphics Presenter 已保留并重绘 `currentVideo`，D3D11 也未在此销毁解码纹理，
不能把问题归为未保留源帧。定位证据见本机 `fullscreen-resize-analysis.md`。

Qt 5.15.2 的普通 `QWidget::paintEngine()` 会设置 `noPaintOnScreen`，继而使
`setAttribute(WA_PaintOnScreen)` 被忽略；原日志已有 `QWidget::paintEngine: Should no longer be called`。
Mac 原有 `MaiVideoSurface` 覆盖 `paintEngine()` 返回空，但 Windows 使用普通 QWidget，
因此窗口状态变动时 Qt backingstore 可以把背景刷到 D3D11 视口上。

现两平台共用已有的 `MaiVideoSurface`，让原生绘制属性真正生效；未增加截图叠层、淡入或新的
播放器。外壳回归明确断言 `WA_PaintOnScreen`；实录在已知明亮彩条模式下逐帧检查视频中心，
自然黑场素材不启用该条件。新证据 `owned-evidence-10/` 的六张截图和 **47 帧** 实录已通过，
`check-player-center.py` 同样检查全部帧通过；Windows 实际录制槽退出 0，已亲看对应阶段画面。
全屏控制栏隐藏后视频视口几何不变，等待 DWM 发布后截图可见控件真正消失。

## 交付构建范围

桌面版本升至 **2.2.46**。Windows 使用 VS2019/MSVC 14.29、Qt 5.15.2 x64 Release，
FFmpeg/OpenCV/RemoteDesktop 全开。公开 Agent 接口变更后，已按编译规则清理共享核心及所有
sink 消费者；音量新接口收尾后又清理 CAPI 和播放器相关消费者，所有目标完整构建通过。
实际 App 的构建后 `windeployqt` 与 95 项 PE/Qt 导入检查通过，再进行发布打包门禁。

打包前 10 个桌面套件中，原有 9 个发布相关套件均通过或明确前台依赖 SKIP；附加的
AgentController 回归发现旧测试忽略默认文本模型对 `view_image` 的能力过滤。
生产注册逻辑与核心既有协议测试一致，已仅修测试为默认/显式文本、视觉、本地四种模型边界，
不为满足测试而放开文本模型的图片能力。最终发布套件及 staging/实际 ZIP 的 bare-PATH smoke
和远端资产核对另随发布记录填写。

本轮未替换或停止现装 Windows App，未运行安装器。macOS 共用源码已调整，但本机没有 Mac
验证入口，未编译或实际运行；不能把 Windows 录屏当成 Mac 验收。
