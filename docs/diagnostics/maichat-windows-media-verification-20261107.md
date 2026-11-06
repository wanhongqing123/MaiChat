# Windows 播放器增量验收

## 范围和操作

- 日期：2026-11-07，Windows 11 / 10.0.26200（QtTest 报告 windows 10）、Qt 5.15.2、MSVC 2019、qwindows；RTX 5080 Laptop，当前屏幕开启 HDR。
- 基线 `ef31e763bc1eeb23718bf72ee8ca9bae1ef6acaf`；媒体提交 `d76d5d51`，最终受测源码 `ba374531c6896bd3d5235015d17a6d54286136a3`。
- Core tree 保持 `4bd2360c7b02d3c998a13bf391d53e790f6f44e3`，未重复 clean Core。
- 本轮验证 Mac 同伴提交对 Windows 的影响：本地独立视频窗口进行移动、缩放、最大化还原、最小化、全屏、HUD 和菜单操作；从正常 MainWindow 的隔离云盘页面双击视频，检查等待首帧的旋转环和首帧后隐藏。
- 使用仓库自带视频、FakeRemoteIMClient 和一次性本地 TLS fixture；不使用用户会话或线上媒体。未停止或替换现装 `D:\Program Files\MaiChat\maichat.exe`（PID 584），未运行安装器或修改公开 `v2.2.52`。

## 编译和相关回归

- `MaiFfplayVideoDialog.h` 新字段按正常依赖重编全部消费者，无头文件 ABI 复用。`maichat` 构建后 windeployqt / 119 项 PE imports 检查通过，再运行 GUI。
- `MaiFfplayVideoDialogTest` 聚焦组 15 PASS / 3 FAIL / 0 SKIP（含 init/cleanup）。通过项包括 HUD 键盘和最小化、窗口边缘缩放、菜单/音量弹层关闭、自动隐藏/暂停、Agent 独立窗口、IM 宽窄主窗口和同源命令不重播。三项失败如下，未计为通过。
- `main_window_layout_test`、`video_preview_dialog_test`、`desktop_music_player_dialog_test`、`MaiFfplayAudioTempoTests` 四组 CTest 通过。MainWindow 原有可选在线头像现场条件边界仍保留。
- 正式 `cloud_drive_dialog_local_tls` 的 qwindows 正常 MainWindow 路径通过（23.76s），包含真实解码首帧、旋转环隐藏、错误横幅不出现、重新打开和切账号关闭。
- 恢复最终 ba 的所有源码并重新全目标编译 / PE 检查后，正式 offscreen 云盘组再次通过（19.72s）；该模式不执行正常 MainWindow 的原生缓冲分支，不能替代上面的 qwindows 证据。

## 缓冲图标 RED / GREEN

- `886587dc` 的真实 qwindows 控件截图为 44×44，亮度 240..254，差 14；已查看为白底白环。旧测试虽返回成功，不能据此宣称外观通过。
- `ba374531` 显式绘制黑底后，同入口控件截图亮度 0..239，差 239；新版测试要求明暗差 >100，图像检查和首帧后隐藏均通过。未修改阈值、跳过或伪造结果。
- 控件截图仅用于加载环绘制。播放器 `QWidget::grab()` 全景存在原生 backingstore 杂纹，已查看并拒绝将其当作 D3D 视频合成证据。
- 外部屏幕采样器只允许当前测试 PID/路径/创建时间，并检查前台归属和遮挡。此次因 `player_not_foreground` 拒绝捕获，没有绕过检查或截取其他应用；Windows 真实屏幕合成录像仍未验收。

## 三项保留失败

### 最小化后立即全屏

- `titleBarControlsMoveResizeAndRestoreThePlayer` 在最小化、`showNormal()` 后第一次全屏按钮调用处失败；原用例独立复跑同样失败。
- 临时仅加日志的测试探针记录：还原后 QWidget / QWindow 均为状态 0，Win32 `showCmd=1`、`IsIconic=false`；全屏按钮可见、可用且 clicked=1。点击过程中 QWidget 曾为 4 / QWindow 为 0，随后 QWidget 为 0 / QWindow 为 4，queued0 后仍分叉。不能归咎于未收到点击或原生仍处于最小化。
- 为核实本轮是否引入，将仅播放器 `.cpp/.h` 和对应测试临时覆盖为 ef31 的原始内容，正常重编 `maichat` 和测试。同机原用例在相同步骤仍为 2 PASS / 1 FAIL（旧行 516，新行 522）。这是原有 Windows Qt 5 窗口状态问题，本轮未修复，尚未定到 Qt 内部根因。
- 基线覆盖不是完整旧版本包。探针和三文件覆盖均已移除，之后恢复最终 ba 源码并按正常依赖重编；没有提交任何诊断探针或覆盖代码。

### HLG / PQ 原生 HDR 首帧

- 额外覆盖 `hdrPlaybackKeepsTransferMetadata(hlg/pq)` 均在 `hasPresentedHdrFrame()` 失败，尚未执行到新全屏流程。
- Windows 当前 `host_` 零初始化；原生 HDR probe / `present_native_frames` 设置位于 `Q_OS_MAC` 分支。Windows 走普通 SDL 路径，提交帧的 transfer 默认 SDR，而 `presentedHdrFrame_` 只在接受非 SDR 帧时置真。
- 对照 ef31，渲染器和该标志逻辑、失败前测试内容相同。这是既存 Windows HDR 保真能力缺口与跨平台测试期望不匹配；素材 probe 成功不代表播放器已启用原生 HDR。保留两项 FAIL，不更改门槛或改成 SKIP，也未扩展本轮实现范围。

## 原始证据和交付边界

- 私有证据在 `.cache/windows-media-886587dc-20261107/`：构建/PE 日志、`ba-player-focused.txt`、`ba-title-isolated.txt`、`ba-title-state-probe.txt`、`ef31-dialog-overlay-title.txt`、`ba-related-ctest.txt`、`ba-cloud-qwindows.txt`、RED/GREEN JSON 和控件图片。
- 当前源码修改仅为本验收记录。私有 Windows 包如生成，仍需 staging 和实际 ZIP 解压的裸 PATH smoke、PE 闭包、四个 Portrait 模型及 build/staging/ZIP 主程序一致性；包检查通过不覆盖上面的失败和原生画面验收边界。
- Mac 的 Cocoa/HLG 实测和私有签名包由同伴独立验收；Windows 记录不替代 Mac 原生全屏，也不声明已完成跨设备 HDR 亮度一致性。
