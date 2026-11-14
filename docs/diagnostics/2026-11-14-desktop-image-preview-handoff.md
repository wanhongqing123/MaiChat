# PC IM 图片预览交接闪帧：2026-11-14

## 操作和预期

从好友私聊中的图片缩略图点击放大，等待独立预览出现，再点击图片或按 Esc 收回。
放大与收回都应连续显示同一图片，不露出一帧会话背景，也不在放大末尾把低清块突然换成原图细节。
取消、窗口调整尺寸和来源控件消失仍按原路径关闭。只修改 PC，移动端图片显示不改。

基线为 `42709270088b999579b8b477f5f8ba73df3f9886` 的 Desktop 产品树，等于正式
v2.2.60 的该段预览实现。本轮私有候选递增为2.2.61，已发布2.2.60的标签和资产保持原样。

## 根因证据

实际缩略图由 `MessageImageLoader` 后台读取后设置到 QLabel，没有原生 Graphics 子视图。
MainWindow 的转场先在主窗子 QLabel 上放大220ms，结束后才显示独立原生 ImagePreviewDialog，
约100ms后删除主窗 snapshot。关闭时顺序相反。

原有背景稳定测试刻意排除了图片运动区域，且 `QWidget::grab()` 会同步绘制主窗口，
不能证明独立窗口与主窗交接时的真实桌面合成帧连续。本轮采用自有合成图和临时IM库，
外部 gdigrab 无损录制，不在动画期间抓 QWidget；Win32 后台探针只读取当前测试进程所属窗口
中图片固定点的实际屏幕像素。试验限定主屏原点(0,0)、DPR1.5，不宣称混合DPI验收。

### 关闭白帧

snapshot 的 show/raise 只排队绘制；独立原生预览先被隐藏，主窗尚未画出接手图片，露出白背景。
三次原操作均被后台探针记录为 RGB16777215，原 Qt 用例实际2pass/1fail，录制进程退出0；
原始桌面帧中图片完整消失，下一帧才恢复。单纯删除 setWindowOpacity(0) 仍2次白帧，
证明问题不能仅归因于透明模式。最终在隐藏原生预览前先 repaint 交接 snapshot，
同测试和操作连续三次不再出现白点，未放松像素断言。

### 打开细节突变

原放大 snapshot 使用已缩小的缩略图，几何放大后才切到独立预览的原图。
自有图中加入细节块，在实际桌面帧中按彩色边界定位图片，比较接近完成尺寸时的细节对比度。
原实现三次均从对比度0的无细节块突然变成原图约67，外部分析实际退出1；
Qt对象/可见性用例仍会通过，不能拿该PASS替代细节门禁。

最终转场从已解码原图一次准备最终屏幕尺寸的 raster，避免每帧重新采样超大原图；
放大结束的最后一帧直接呈现该像素图再显示独立窗口。关闭按当前预览尺寸重新准备。
同一图、相同分析阈值的三次原生录制均通过，末段对比度约53至67，不再出现原来的整片灰块切换。

## 最终 Windows 运行构建

原normal Qt6.12/MSVC14.44 Release cache，native client、FFmpeg/OpenCV/Portrait等原开关保留。
maichat 与 main_window_layout_test 正常构建退出0，windeploy及3个二进制/114项PE导入门禁通过后
才运行实际UI。标准图片显示、单一预览/调整尺寸/点击关闭、普通及最大化背景稳定四行全部通过，
Qt总计6/0（含init/cleanup），4.410s。

最后同当前源码哈希的 Native 记录：三次打开/关闭、235个后台实际像素样本、0遮挡、0坏点；
外部细节分析三次通过，输入文件前后SHA不变。记录要求显式私有输出前缀，默认不运行，
不把无屏幕的CI当作原生像素验收。已查看进入末段、收回首帧、收回过程与关闭后的原始视频帧。
最初受其它置顶窗口遮挡的录制、DPI边界舍入/文件替换等夹具错误单列；遮挡录制像素已退役，
保留诊断日志，不计作功能RED。

实际最后编译的产品源 SHA256：
`2d9f932a464c124087042a301a1c647745dcd372c23b4aa89d3c57b5acdc55f6`（MainWindow.cpp）。
测试源 SHA256：`52fe6e0be07a60297b83f5013706f50ce5d2dcd6fcf8f618d37ffb302d77f311`。
私有原始证据在 `.cache/desktop-image-preview-flicker-20261114/`：
`native-pixel-red`、`hypothesis-opacity-removed`、`hypothesis-painted-handoff`、
`detail-handoff-red`、`final-native-261`、`final-regressions`，各次不覆盖。

## Mac 边界

同一精确三源供件在原normal Cocoa Qt6.12缓存 configure0/8.673s、App与相同测试目标 build0/33.090s。
仅三个selector、普通/最大化两数据行实际6/0，run0/7.721s；App版本2.2.61 arm64，
exe SHA256 `dbac0c557902b7c33194fbb45d387659efb7532b72d74c9fe21d3c781ca294a3`。
供件Desktop tree为 `cd9df4fdc3eaa6cf50fdde2468b1379d1e61c9c5`，三输入前后canonical SHA一致。

CGPreflightScreenCaptureAccess=false，未申请权限、未设置抓屏/record环境、未运行可选录制fixture。
常规用例的 QWidget.grab 只算同步合成功能像素检查；不宣称Mac已排除原生220至320ms或首关闭帧闪动，
也不以Windows录像替代Mac原生连续帧验收。没有停止或替换用户正在使用的Mac App。

无真实账号登录、聊天库读取、剪贴板操作、模型或付费请求。此次只覆盖预览转场，
原有UI线程完整原图解码耗时没有另行重构；不把关闭白帧证据混写成解码性能结论。

## 正式 2.2.61 交付

发布源码固定为 `ffc1102f6d2bf20b9cb5b4ce2ddfd67af0d4bf02`，annotated tag
`v2.2.61` 对象为 `589ba4525e9c598a6cb68925ddd20fa9af533f5b`，peel 精确回该源码。
后续本诊断补记只归档证据，不移动标签，也不把归档提交倒填为构建源码。

Windows 正常 Release 在该提交上 configure0/8.417s、build0/11.562s，实际 exe SHA256
`ac5a108c3560d13b310f690a3a2c640ac54cc6331051e1317f811951d4db7778`。
Setup、便携 ZIP 和实际解压副本均完成依赖闭包与五模型校验；六个独立普通/debug smoke
实际版本2.2.61，使用各自包内Qt和私有AppLog，未运行安装器或替换用户现装。
打包后的私有脚本曾因旧固定数字60的版本断言退出1；实际安装器版本为2.2.61.0。
该失败及部分metadata原件保留，修正为从目标版本读取数字分量后仅续签名检查和metadata，
两个payload前后SHA相同，没有重打包或倒填原退出码。

Mac 同源码正常版本/Git元数据增量后产出DMG/ZIP。staging、只读DMG与实际ZIP解压副本
338个文件及37个符号链接逐项相同，26个MachO依赖与strict签名检查通过；
22条正式命令退出0，六个独立普通/debug smoke实际版本2.2.61，原Popen环境、私有CFhome的
实际AppLog/PID及sandbox规则均保存。五模型与Windows及先前锁定版本逐SHA/bytes一致。
Mac为adHoc签名、未公证；App声明14.4但部分依赖声明15或26，仅实际验证本机27.2。
这些启动检查不补足前述原生动画录屏边界。

Root在Mac四次串行上传完成且窗口归还后，GET核对8个uploaded资产的size/digest，
并实际下载两个manifest与两个SHA256SUMS，草稿门禁PASS才单次发布。
Release `445007211` 于 `2026-11-14T04:30:59Z` 发布为非draft、非prerelease。
随后在无Authorization、无Cookie的14次GET回验中，8资产、4份实际metadata下载、
源码/tag peel、跨平台模型和网页入口全部PASS，official latest精确指向同一Release。
先前2.2.60的body/assets/tag保持原样。Mac另行只读GET回验得到相同发布与latest结果。

正式下载：[MaiChat 2.2.61](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.61)。
原始交付证据位于 `.cache/release-2.2.61-20261114/`，包括
`draft-readiness-01.json`、`publish-response-01.json`、`public-verification-01.json`，
以及两平台handoff和四份实际下载的metadata。未替换或重启用户当前Mac App。
