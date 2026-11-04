# 桌面端点击缩略图整窗闪动（2026-11-04）

## 用户操作与预期

在桌面端 IM 点击已有图片缩略图，图片应连续展开到独立预览窗口；图片以外的主窗口内容应保持原亮度，不出现整个 App 先暗后亮的闪动。关闭后图片回到原缩略图位置，原会话、草稿和滚动位置保留。重复打开与关闭也应一致。

Windows 与 macOS 使用同一 Qt 图片预览实现。本机有 Windows/Qt 环境，没有 macOS 运行环境；平台实际验收分别记录，不以源码共用代替平台运行证据。

用户随后提供 macOS 图片预览截图，明确要求去掉系统标题栏及红黄绿窗口按钮。本轮同时处理这项要求，保留独立预览、图片点击和 Esc/Enter 关闭以及尺寸拖柄。

## 排查起点

- 当前源码：`9d037c5368351d97b75268845f53293f48fb7436`，开工工作区干净。
- 本机没有运行中的 MaiChat 进程，不替换或停止用户桌面程序。
- 现有 Windows 完整 staging 来自 `311874c1`，版本 2.2.34，已经过 windeployqt、90 项 PE 检查与裸 PATH 烟测；其 `MainWindow.cpp`、`ImagePreviewDialog`、`MessageImageLoader` 与当前源码无差异，可用于定位这个图片行为，不能当作整个当前 App 的验收。
- 首要候选：`MainWindow::openImagePreview` 的 IM 缩略图分支给主窗口整个 `rect()` 添加深色 scrim，220ms 内从透明变为 0.62；显示独立 Dialog 后又等 100ms 删除遮罩。关闭时也添加同样的全窗遮罩。创作中心/云盘直接打开同一 Dialog，没有这层遮罩。

## 复现与修复验证计划

1. 使用 FakeRemoteIMClient、本地合成图片及真实 MainWindow，录制完整进入/关闭过程；在远离图片、目标 Dialog 和系统标题栏的主窗内容区采样颜色。
2. 在原实现运行同一回归，确认整窗亮度变化的失败证据；不把构建或依赖启动失败当作业务复现。
3. 最小修改共用图片过渡，使图片以外区域稳定，保留独立可缩放窗口、图片几何动画、单实例和来源恢复。
4. 完整 Windows App 构建后通过 windeployqt 与 PE 门禁，再跑同一真实窗口用例及录屏；核对源码、产物和运行进程。

## 结果

### 有效复现

普通窗口和最大化窗口均录得整个导航栏、联系人列表及消息背景一起变暗再恢复。合成红图自身仍保持红色。定点像素由白色 `#ffffff` 在入场 100ms 降到约 `#c2c2c3`，250ms 降到约 `#676869`；关闭也重复压暗。真实录屏中同一区域亮度约 253→225/219→102→253，与遮罩颜色和 0.62 透明度吻合。

红测正常运行并失败于背景颜色和系统标题栏断言，日志 `.cache/desktop-image-flash/red-qt.txt`。不是 DLL 缺失、启动失败或窗口未出现。根因是主窗 scrim 的淡入和突然撤销，与图片解码或显卡驱动无关。

### 修复

- 实现提交：`277e9406`。移除整窗深色遮罩及淡入淡出，保留图片自身几何过渡、单实例、来源失效检查和关闭后源图恢复。
- 以不绘制背景的透明输入层继续拦截动画期间的底页鼠标操作，避免去掉遮罩时顺带改变模态边界。
- 共用 `ImagePreviewDialog` 设置 `Qt::FramelessWindowHint`，IM、创作中心和云盘都使用同一无系统标题栏窗口；没有改回占据原会话的嵌入式预览。
- Windows 测试 wrapper 增加定点 QTest 函数/参数转发，仍先配置 Qt/SSL 运行路径和系统错误弹窗门禁，再执行 CTest 注册的命令。

### 合并后实际验收

协作者新增共享核心接口、本地模型 chat 实现及桌面版本更新后，本轮已合并并重新编译，验收源码为 **`111f8c1c52f88e1f9acc4a2cccb93dd303f6dd11`**，桌面版本 **2.2.45 / Qt 5.15.2 / VS2019 x64 Release**。未将合并前的 2.2.37 产物当作最终验收。

- 完整 `maichat` 和 `main_window_layout_test` 构建通过；自动 windeployqt、95 项 PE 导入与 Qt 插件检查通过。增量构建的 MainWindow、AgentController、layout test 头依赖分别核得 449/133/385 项有效记录，未沿用不追踪公共接口头变化的对象。
- 同一真实 MainWindow、FakeRemoteIMClient 和本地合成图运行 **5 个业务用例及初始化/清理，共 7 PASS、0 FAIL**：普通/最大化背景稳定、重复点击单实例、缩放窗口及关闭回源、原图像素、右键菜单。日志 `.cache/desktop-image-flash/merged-qt.txt`。
- 普通/最大化进入、预览显示、关闭期间，远离图片的浅背景 PNG 采样均持续为 255；录屏编码后约 253，未再压暗。已亲看 `.cache/desktop-image-flash/merged.mp4` 及 `screens/merged.png.*`，系统标题栏/窗口按钮消失，图片连续进入与缩回，没有整窗明暗闪动。对比图已发给用户，不包含真实聊天或私人图片。
- 合并后的 MaiAgent 核心另用全新 Windows/MSVC14.44 构建目录完成 738 步编译及 **8/8** 核心门禁，137.82s；273 项源码/测试/chat/CMake 指纹前后相同。此前独立核心缓存漏记头依赖导致旧 ABI 链接失败，已隔离旧结果并全新构建，本组不复用先前的通过日志。该组 Graphics 关闭，不替代本轮 GUI 录屏。

### 产物、运行路径与边界

最终本地验证包在 `.cache/desktop-image-flash-merged-package/`。构建目录、完整 staging、实际 ZIP 解压目录的 `maichat.exe` SHA-256 一致：

`7207b04349adef2c66ca76e2b748dd6324139865f98ff6a19302dc18a19ce4ef`

ZIP SHA-256：`b6aa0e6ffb0381060e69d6b3fec0c5c4265537c57503fc6137aaa00eb3d7a769`。

两份均先过 95 项 PE 门禁，再以裸 PATH `C:\Windows\System32;C:\Windows` 执行 `--smoke`：

| 运行目录 | PID | 退出码 |
| --- | ---: | ---: |
| `MaiChat-win64/maichat.exe` | 67024 | 0 |
| `zip-unpacked/maichat.exe` | 57584 | 0 |

WinAPI 实际进程路径、日志版本 2.2.45、Qt 5.15.2、PID 和 OpenSSL 3.5.9 均与对应产物匹配，详见 `verification-manifest.json`。Qt 的日志目录仍解析到真实 Roaming；`--smoke` 跳过登录启动及账号恢复，没有替换安装、清除账号、发送消息或调用付费模型。未运行安装器或上传 Release。

macOS 使用同一预览修复及无边框标志，本机没有 macOS 构建/运行环境，**未宣称完成 macOS 实机验收或已替换用户的 Mac 安装**。
