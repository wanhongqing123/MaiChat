# Windows Qt 非 BMP 字符输入顺序只读核对

2026-11-16。Windows 协作者在独立原生链路看到自有 QLineEdit 中的 emoji 移到字符串开头，要求 Mac 侧只读核对 Qt 事件排队与既有补丁约定。本侧没有运行 Windows、模型、输入、测试、构建或设备/服务操作，没有修改 Qt、产品或测试源码。

## 复现与预期

同一自有编辑器正常获得焦点，通过生产 Unicode 输入路径一次输入 `A + U+1F600 + B`，应保持原顺序。补充中文与 emoji 交错、首尾 emoji、连续非 BMP 与 ZWJ，以及选择后替换。需要同时记录原生 UTF-16 单元顺序和控件实际接收事件顺序，不用模型自述或最终文本单独证明中间因果。

这条字符重排与屏幕/窗口比例、焦点异步确认分别记录，尚不能据此推定所有用户现场问题同根。

## 已核实的分发差异

本机 qtbase 为锁定提交 `025bdad181de241e81bf853c8a2d7bf3d19261a9`，子模块工作区干净，版本 6.12.0。产品输入发送器 `DesktopComputerWindows.cpp:358` 将 UTF-8 解为 QString，逐 UTF-16 单元发送 KEYEVENTF_UNICODE 按下/释放，没有按字符类别重排。

| 位置 | 行为 |
| --- | --- |
| `qtbase/src/plugins/platforms/windows/qwindowskeymapper.cpp:1142` | 高代理项暂存在 m_lastHighSurrogate，并提前返回 |
| 同文件 `:1146` | 低代理项与前者合为完整 QString，通过 QInputMethodEvent 和 QCoreApplication::sendEvent 直接提交给当前 focusObject；随后清除暂存 |
| 同文件 `:1254` | 普通 BMP 经 handleExtendedKeyEvent 分发 |
| `qtbase/src/gui/kernel/qwindowsysteminterface.cpp:31,78,130,492` | 默认 synchronousWindowSystemEvents=false；默认分发进入同一个 windowSystemEventQueue，之后才交 GUI 处理 |
| `qtbase/src/gui/platform/windows/qwindowsguieventdispatcher.cpp:43` | sendPostedEvents 先处理 Core posted events，再处理 QPA window-system events |
| `qtbase/src/gui/text/qinputcontrol.cpp:48` | KeyEvent 的 text 若为合法高/低代理对可接受；原测试已有 good-surrogate 数据行 |
| `qtbase/src/widgets/widgets/qwidgetlinecontrol.cpp:1880` | 可接受输入最终 insert(event->text()) |

因此，只要前序 BMP 仍排在 WSI 队列中，surrogate pair 的直接同步提交就可能越过它。该机制与 Windows 侧症状相符；本侧仅确认代码构成该条件，尚未独立取得同一失败进程的原生/Qt 事件顺序或替换分支后的运行证据。

## 最窄候选与决定性验证

针对已经实证的 KEYEVENTF_UNICODE/VK_PACKET 路径，合法完整 surrogate pair 应与相邻字符进入同一个 QPA 队列。本文件已有 `sendExtendedPressRelease`（814 行），内部按顺序提交 handleExtendedKeyEvent；可以传完整 pair QString，并沿用接收窗口、时间和原生修饰。保留高代理等待、合法配对、reset 和提前返回，避免再把代理项拆成两个控件输入。

若把同分支中的真实 IME/其他原生输入也一起改为 KeyEvent，应单独确认 preedit、WM_IME_CHAR 和提交语义；可先限定已证 packet 路径。这里仅给方案，没有生成或应用 Qt 补丁。

只把 QInputMethodEvent 改为 postEvent 并不能证明顺序正确：Core posted 与 WSI 是两条队列，Windows GUI dispatcher 还会先处理前者。全局改同步、手动 flush/processEvents、按字符 sleep 或改走剪贴板也不是该队列差异的窄修。

同例验证应做到：

1. 自有窗口 NativeEvent 见证 VK_PACKET/WM_CHAR 的 UTF-16 顺序，控件事件过滤器只读记录 QInputMethodEvent、KeyPress/Release 的 text 及 textChanged 顺序。
2. 旧代码在原生顺序正确时出现 pair 先提交、前序 BMP 后到；只换候选分支后同例保持原顺序。不得用 QTest::keyClicks 绕过 Windows key mapper。
3. 原 Unicode、选择替换、清空、真实 IME 边界继续成立；私有 Chrome 的普通 native 回放作为外部控件对照，不使用用户浏览器资料或第三方网站。

## 现有 Qt 补丁与构建约定

`MaiChat/desktop/qt/UPSTREAM.md` 明确现状为没有应用上游源码补丁，来源由 sources.lock.json 和 shallow Git 子模块固定。本地没有独立的 Qt overlay 补丁目录/清单；qtbase 内第三方库自带 patches 目录不属于产品 Qt 补丁机制。

`scripts/build-qt.py:103-143` 核子模块 HEAD 与 lock。dirty 状态只用于跳过已安装 SDK 的快速复用、强制执行增量 Ninja；写入 stamp 的 fingerprint 不含源码 diff 摘要。因此临时 dirty 源构建后恢复 clean，旧 fingerprint 仍可能匹配，却复用带过临时修改的 SDK。现有 dirty 重建能力不能代替正式补丁身份或交付源码冻结。

后续若正式采用 Qt 窄修，应使用受版本管理的补丁及精确 preimage/幂等检查，并将补丁摘要纳入 bootstrap fingerprint、更新 UPSTREAM 声明；或改用可重新取得的明确子模块提交并同步 gitlink/lock。不能只留在协作者的未提交 Qt 子模块内。

本函数属于 Windows 平台插件，最终应核实际加载及包装的 `platforms/qwindows.dll` 源指纹和文件 SHA，不能仅以 App exe 或 QtCore/QtGui DLL 更新证明候选已生效。保持现有 profile 和缓存增量构建流程，不另复制整套 SDK。

本侧本轮只有只读源码结论；没有提供运行通过声明，也没有将旧 WM_NULL 候选的静态审阅套用于后续真实前台轮询候选。

## 新十一件候选的独立审阅与补丁换行 P2

后续两次收到的 patch/manifest 完全相同，均为 48441 B、SHA256 `9225d6dee18e29270e16a10c93dde2fc4a00fd1321a252c22d44415b7fb8faf0`，base 声明为 `1a09bf4b6bbf80cebc347209868fd7e3496eb23d`。本侧没有叠加或应用到仓库。

十个文件的私有重建 postimage Git blob 均与 manifest 一致；旧有文本文件的发送方 raw SHA 对应 CRLF，新增文件为 LF，均按各自原始字节形式核对。CMake 本地缺少声明的完整 preimage，仍只审五行新增 Windows test target，没有把它称为完整 source/tree 核等。

生产候选包括：

- `DesktopComputerWindows.cpp` → `96e3e14fcad999b89db22454c098a860e3690e17`：一次正常 SetForegroundWindow 后，跨线程每 5 ms 核真实前台，最多等 250 ms；期间和结束重核 PID/visible/iconic，没有 WM_NULL 或附加输入队列。
- `DesktopComputerTools.cpp` → `877348e0f90ca7e712fa467721469b9e0fcde2a4`：明确实际图片 u/v 到 native display，再到 window 的转换，不假设 PNG 像素就是指针单位。
- Qt overlay → `b20d858833c37946e5064108a938578006c0bd2f`：VK_PACKET 的完整代理对走原有 WSI helper；不同 packet 文本不沿用旧重复键记录；TextEdit 接受 literal LF，LineEdit 和真实 IME 分支保持原边界。
- source lock 固定 Qt 两文件 pre/postimage；builder 增加补丁 digest、实际 tracked diff/untracked 内容摘要与 SDK stamp 身份。

在隔离临时目录中，用**这份实际 builder**和精确 Qt 两文件 preimage 执行补丁校验/应用，确认锁定 postimage；未写现有 Qt 子模块或 SDK。包内已有 `test_qt_source_patches.py` 三个私有 Git 测试实际 3/0。

独立新增检出边界得到实际 **RED**：

1. 新 patch 是 LF 2771 B，锁定 SHA256 `a66f64b74b19c5b10e2a0f6892451bd8617d05be18429c401174d8ecb97e67de`。
2. 把它提交到自有临时 Git，保留仓库当前 attributes 规则，设置 `core.autocrlf=true` 后重新检出；文件相对 Git 仍为 clean，但变成 CRLF 2820 B、SHA256 `499494abfcb2173b15cfb7a4de18d2fba4a1acc9327537c01f2897d64e883b9a`。
3. `build-qt.py:38-40` 直接对 patch.read_bytes() 验 SHA，真实 helper 返回 `Qt patch digest mismatch: patches/windows-packet-surrogate-order.patch`，没有继续修改任何 Qt 源。

这是正常 Windows Git 检出不能 bootstrap 的构建 P2，现有三个测试未覆盖。最窄建议为受控补丁目录加入 `*.patch text eol=lf`，另加真实 autocrlf 检出测试。没有修改收到的 lock 摘要或补丁本体。

在**同一自有临时夹具**加入该单行属性后重检出，实际仍为 2771 B LF，SHA 与原 lock 完全相等，原 helper 的幂等 postimage 校验成功，得到私有 **GREEN**。收到的十一件候选本身尚没有该 attributes 修复，不能将私有建议验证冒称已集成收口。

新 MouseWitness 已从 WM_LBUTTONDOWN 自身经 ClientToScreen 记录坐标，修正了上一版稍后采样光标的观测边界。八个 Unicode 数据行有最终文本和 UIA 强断言；nativeCharactersMatch 目前为日志字段，若要声明原生单元顺序正确，仍需查看对应实际日志，不能只依据测试总数推断。Mac 本轮未执行 Windows 输入/Qt SDK 构建、真实模型或设备/服务操作。

## 最终 EOL 门闭合

收到十二件修订：50932 B、SHA256 `9bee7eeabf6045071db34d21594bf4aa3d241f49d0a3d2813653fda98f9b4842`。相对上一包只有 UPSTREAM、补丁目录 `.gitattributes` 和检出测试三处变化，其余九个补丁块逐字未变。

最终三件 postimage 精确核等：UPSTREAM `c1b7d87fbe0d859d6f97cb713488f977a769aed4`；属性 `9812ceb1ffd9bbf0c98ab6f94243da0324852c1c`（20 B，`*.patch text eol=lf`）；测试 `45aac43becbd1db8e85268a312b364d4928ef744`。Qt overlay 仍是原 2771 B LF、SHA a66f64b7…，Qt 两文件锁定 postimage 和 builder 没有再变。

在私有副本仅运行收到的新 `test_clean_windows_checkout_keeps_the_locked_patch_bytes`，实际 **1/0，0.976 s**。正向是真实 autocrlf=true 重检出后应用成功，负向移除属性后同 Git blob 再检出必须被 digest 门拒绝，Qt 私有源仍保留原 postimage。测试基线使用 write_bytes 固定 LF，避免 Windows write_text 自身换行转换污染对照。

**EOL P2 已在最终收到的源码修订中闭合，未见本次修订新增具体 P1/P2。** 仍不把该纯 Git/bootstrap 验证说成 Windows 原生或真实模型验收；发送方报告的 native/Chrome/播放器结果另属其运行证据。本侧没有应用到现有 Qt、产品、测试或 SDK，没有重跑 Qt/原生组，没有操作现装或服务。
