# Windows 播放器浮层错位与桌面拖拽坐标核验

## 现场与预期

2026-11-15，用户在 Windows 播放器截图中看到画面、标题栏与底部控制条分处不同位置。
预期：从创作中心打开视频时，首次显示就让标题栏贴齐视频顶部，控制条贴齐底部；
拖动、调整大小、全屏、还原、最小化恢复后继续对齐。

另一个反馈是 `desktop_computer` 拖动滑块时发生漂移。该项单独调查，不把截图中的
模型历史解释当成坐标错误的证据，不操作第三方验证页面。

用户现装 Windows 客户端为 `D:\Program Files\MaiChat\maichat.exe`，启动日志确认为
2.2.65 / Qt 6.12.0，观察时 PID 56404。没有停止、覆盖或重启该用户进程。
基线源码为 `1e2171970a2e782098a8e14372ef8c61aaaa25e7`，播放器四路径与发布
`c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c` 相同。

## Windows 播放器决定性复现

在当前 Windows 原生 QPA、144 DPI / 150% 缩放上，新建独立普通 Qt 测试进程，
使用 Fake IM 和仓库本地 `video-circle-61s.mp4`，经过真实
`MainWindow::openVideoWindow` → `prepareAndShow` → 正式 FFplay/Graphics 播放路径。
未连接账号、模型或外部网站。

初始 native metadata 探针中，用户播放器已关闭；当时仅能确认主窗 DPI，不能据此判断根因。
随后新用例 `MaiFfplayWindowsGeometryTest` 实际得到产品失败：

| 原生客户区 | 基线坐标/尺寸 |
| --- | --- |
| 播放器 / 视频 surface | `(920,561,720,405)` |
| 标题 HUD | `(780,518,720,69)` |
| 控制条 HUD | `(780,797,720,126)` |

三个窗口 DPI 都是 144。标题 HUD 的 Qt 原点仍为 `(520,345)`，owner 已为 `(613,374)`。
因此不能只归咎于某个窗口采用不同 DPI。基线 `baseline.xml` 为 3 tests / 1 failure
（包含初始化、清理），失败是实际 native header 原点不等于 native owner 原点。

根因是独立 `Qt::Tool` HUD 在 owner 的同步 `show()` 映射尚未完成时就读取全局原点并显示。
Windows 随后确定播放器最终位置，但最初的 HUD 留在旧原点。原有
`ownedHudTracksOwnerAndForwardsKeys` 只比较 Qt 坐标且走直接 show，未覆盖该原生首次映射边界。

## 修复与 Windows 验证

仅在 `Q_OS_WIN` 下重写 `setVisible`：同步映射期间暂缓两个 HUD 显示，
`QDialog::setVisible(true)` 返回后，按最终 owner 原点重新排布并显示。
隐藏、重复 show 保留原调用；Mac 的独立修复不走此分支。没有改解码器、DPI 配置或视频尺寸策略。

同一个用例修复后：owner/surface 与 header 均为 `(920,561)`，footer 为 `(920,840)`，
三个宽度均为 720 像素。Windows native message filter 观察到两个 HUD 显示调用，
在 owner 可见前显示的调用为 0。

扩展两种入口（普通 Main prepared、直接 show），并验证移动、resize、全屏、还原、
最小化恢复。全部通过；全屏时 1707 logical pixels × 1.5 的取整可比真实 2560 像素屏幕
边缘多 1 像素，测试仅在该几何比较允许 1 物理像素误差，原始百像素偏移仍会失败。
首次扩展断言使用绝对整像素相等，因上述 1 像素边界失败，保留日志，不称为新的播放器错位根因。

原有 7 selectors 在真实 Windows QPA 同跑全部通过：共享 chrome、标题栏操作、native 边缘拖动、
HUD 跟随/键盘、prepared SDR、准备取消/失败、关闭后重开。Qt 为 9 tests / 0 failures，
包含初始化、清理。实际保存并查看了自有播放器窗口 PNG；没有将 native metadata 代替视觉检查。

普通 `maichat` 目标也已构建，构建后 `windeployqt` 和 PE 依赖检查通过。
构建与证据位于 `.cache/windows-player-geometry-20261115/`；候选包和最终 SHA 另记该目录 receipt。
这不表示用户现装 2.2.65 已被更新。

## Mac R8 合入边界

Mac 协作者提供独立普通 AppKit order probe：基线 8 次 HUD 在 owner 可见前排序，
owner native 仍为 480×270，Qt 已为 960×540；动画也尚未禁用。
Mac-only 修复在 owner 映射与同层同步结束前保留 pending，增加映射重入保护，并提前 native style。
最终 22 次排序中 early HUD = 0，bad animation = 0，普通受影响组为 11/0/0。
生产 2 路径、测试 2 路径逐 manifest blob 核验后应用，再接 Windows 条件分支。
Mac 协作者对 Windows 增量独立静态 review 未见具体 P1/P2；两个可选证据输出问题已在新测试修正。
Mac 运行结果来自协作者，不声称为 Root 本机 Mac 验证，也不声称已替换用户 Mac App。

## desktop_computer 拖拽的实际结论

新增 opt-in `DesktopComputerDragNativeTest`，只控制自有本地 slider。
用真实 controller/native 后端完成捕获窗口 PNG → 从实际像素识别滑块与目标中心 →
0..1 比例 → 原生鼠标拖动 → 接收窗口与 OS 指针读回。
同一窗口在 `(180,120)`、`(245,173)` 两个逻辑位置分别执行。

当前 150% 缩放下，捕获 PNG 为 1080×480；两个位置均准确到达选定的物理像素，
鼠标按下/放开也由自有 receiver 收到。没有复现固定比例缩放或窗口移动后的系统性偏移。
截图颜色中心存在半像素时，先落整数物理像素，再转 Qt logical 坐标；测试最初忽略该取整的
预期错误已纠正，保留中间日志，不将它称为产品 RED。

没有修改 `DesktopComputerTools` 或 Windows 输入生产代码，没有宣称第三方滑块验证已修复。
具体现场起点/中途/终点的漂移形态仍待用户补充；本地结果仅排除已覆盖的坐标路径。
窗口拖拽时长上限仍为 200ms，display 模式支持至 5000ms；该工具契约本次未更改。

## 数据与交付边界

原有 `.gitignore`、MobileAgentAdapter 两路径未提交工作保持原字节，未混入产品构建。
本次没有服务端部署、账号修改或用户数据清理。此前 Sync 服务 R2 部署授权仍待用户明确答复，
与播放器和坐标工作无关。Windows 正常用户 App 的替换需在候选完整可审阅后按仓库运行进程约定处理。
