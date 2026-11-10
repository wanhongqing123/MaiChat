# 创作中心屏幕采集停止入口与桌面窗口边界

## 需求与可复现操作

- 基线：`c71b0450d464ace6406af102951daf8a8d17f924`，2026-11-10。
- 用户说明屏幕采集由创作中心 AI Agent 启动，需要在创作中心直接关闭。
- Agent 获取屏幕后，原生采集会话可能继续驻留；在创作中心应能看到状态并停止。停止期间界面保持响应，失败可重试，停止成功后状态条消失。切换本机会话或空白页不丢失此进程级入口、草稿或附件。
- Windows 普通窗口叠放在浅色应用上时外沿不明显。用户提供 QQ 参考，期望细外框与柔和窗外阴影；最大化与全屏不增加空白边距。

## 设计与边界

1. 新增独立 `DesktopScreenCaptureControl`，只读取既有 `maiScreenshotCaptureState()`，停止复用既有 `maiStopScreenshotCapture()`。后者会等待原生工作线程退出，所以由后台任务执行；界面线程只投影状态和处理完成回调。
2. 状态条和“停止采集”按钮放在 `MainWindow` 的创作中心公共层级，位于左右会话列表和本机/远端内容 splitter 上方。条目明确“本机 / 所有会话共用”。采集中、驻留待命、停止中、失败重试分别投影；默认没有采集时不占布局空间。普通可注入 control 供宿主组合与确定性回归使用。
3. 这是关闭当前原生采集会话。之后新的截图请求可以重新启动；不取消模型回合，不撤销既有 computer observation 输入凭据，不修改 TRTC 共享路径。
4. Windows 保留原 native resize/Snap，启用 DWM 原生阴影并绘制鼠标穿透的中性细外沿；最大化和全屏隐藏外沿，不修改内容布局边距。macOS 保留系统窗口框。
5. 普通控制器测试和真实组件测试使用自有替身/临时数据。原生停止只在本次创建的截图进程或自有窗口 fixture 验证，不操作现装用户 App。

## 实施顺序

- [x] 保存现有普通创作中心组件截图与 Windows 外沿基线。
- [x] 新控制器：状态轮询、异步停止、重复点击合并、失败重试、销毁后安全完成。
- [x] 创作中心公共状态条：唯一、可见与布局、停止动作、跨本机/远端会话及 owner rebuild 保留。
- [x] Windows 外沿与 DWM：正常窗口/失焦/最大化/全屏/恢复及原 resize/Snap 回归。
- [x] Windows 修正公共归属后正常目标编译、查看实际截图，核对源码与运行二进制，记录验证结果。

## 文件职责

- `agent/DesktopScreenCaptureControl.h/.cpp`：采集生命周期的普通 GUI 宿主接口。
- `agent/DesktopScreenCaptureBar.h/.cpp`：公共状态与停止入口，无会话状态。
- `tests/DesktopScreenCaptureControlTest.cpp`、`tests/DesktopScreenCaptureBarTest.cpp`：异步边界和实际部件行为。
- `tests/DesktopScreenshotIntegrationTest.cpp`：原自有原生窗口整链追加按钮真实停止段。
- `ui/MainWindow.h/.cpp`、`tests/MainWindowLayoutTest.cpp`：Windows 原生窗框与真实窗口回归。
- `MaiChat/desktop/CMakeLists.txt`：新普通源和测试接线。

## 交付边界

本轮不变更版本、不打包、不安装、不发布。公开 v2.2.56 和当前用户运行的 App 保持原样。最终实测结果将在本记录补充；计划项不代表已通过。

## v1 历史结果：公共归属纠正前

用户看到候选截图后明确指出：采集常驻并被所有 session 使用，放在某个 session 内不合适。原候选把状态条放入 `AgentChatPanel`，虽然跨本机会话切换时保留，但视觉和宿主层级仍属于 session 内容，用户的纠正成立。该版 UI 验收已撤回为历史；Panel 三文件恢复原实现，改由独立公共 bar 与 MainWindow 持有。以下 v1 表格不能作为最终公共入口通过。

Mac v1 也只记历史：normal configure 和 App/Panel/control/integration 四目标 build0；control 11/0，1.634s；Panel 82 PASS / 1 FAIL，13.530s，唯一失败为原音频播放槽的 3 秒预算，Qt 原文指出当次需 4 秒。驱动因失败自然结束，native integration 没有运行，不能记成权限 SKIP。公共归属纠正后停止该版后续验收，没有终止用户现装 App。

同一 11 文件源码，跨平台 Git blob 合同的补丁 SHA256 为 `d914d64039ae8552ac9cccdd744ea7df5ce5f46278c5434552722d211a70af6b`，预核 `MaiChat` tree `d8e91570abd410dab7fb98b29cd81c5fe4afba52`；`MaiAgent` tree 仍为 `4ab2cc60987f23c7d67659a4ea05e6f89cc92bcd`。

| 验证 | 实际结果 |
| --- | --- |
| 新普通控制器完整组 | 11 PASS / 0 FAIL / 0 SKIP，1.792s，含 2 hooks |
| 原创作中心完整组，含新增三例 | 83 PASS / 0 FAIL / 0 SKIP，16.287s，含 2 hooks |
| 原生自有窗口截图→正常面板按钮停止 | 3 PASS / 0 FAIL / 0 SKIP，0.900s，含 2 hooks |
| 窗口原四项及新增两项 | 8 PASS / 0 FAIL / 0 SKIP，2.453s，含 2 hooks |
| 正常 App 与受影响目标 | build exit0；windeployqt 完成，3 binaries / 114 PE imports / Qt plugins 检查通过 |
| 正常 App 私有资料目录、裸 Windows PATH smoke | PID 40812，exit0，1.671s；实际加载的 Windows `qwindows` 与 Qt 模块均来自本 normal build/bin |

实际开发 exe SHA256：`376b4b5b852b558cde8cf8108fd747b4632e2c4ca37febe0f316d7d253f8f36e`。版本源未改，所以开发 exe 的版本仍读为 2.2.56；它与公开 v2.2.56 包的产品内容不同，不能据此称已更新安装包。

原生整链只捕获本测试进程创建的 nonce 标题窗口。点击页面的停止按钮后核对 `workerRunning=false`、`capturing=false`，随后帧计数不再增长；历史原 PNG 与输入草稿都保留。控制器替身测试和此原生测试分列，不将替身当作原生通过。

开发者已亲看 Windows 活跃状态、失败重试、窄页面重启状态、原生停止后页面及窗口外阴影截图。源码字段、二进制 SHA、原始日志 SHA 和图片 SHA 见同名 JSON。Mac 消费验证单独补充，不借 Windows 结果冒称。

## 已取得的证据

- 改动前正常 Windows Qt 6.12 创作中心组件运行并截图；新入口用例在旧产品实现上实际 `1 FAIL`，原因是找不到停止按钮。新控制器 no-op 实现编译、链接正常后，其九项生命周期用例实际 `9 FAIL`，没有把编译失败当行为失败。
- Windows 原四项标题栏/原生缩放/Snap 用例先过；新增外沿与外部阴影两例实际 `2 FAIL`。实现后同六项加 hooks `8 PASS / 0 FAIL`，2.453 秒。自有纯色背景上的窗口外底部阴影像素从 0 变为 28350（DPR 1.5）。开发者已查看含窗口外区域的实际截图，细外沿及柔和阴影可见。
- 首次阴影探针没有等背景首帧合成，得到的数值是假阳性，已撤回。最终夹具先 `repaint`、处理事件、等待合成并逐像素确认背景为纯色，再比较窗口外区域；不以单独的 `DWM ncrender=true` 判断存在阴影。
- Root 首次独立测试目标误链整个 `maichat_agent`，产生 UiZoom 链接首错；改为仅编译普通 control 源并链接其实际依赖。另 Panel 新代码的两个参数类型首错按原 API 修复。上述构建错误均不计为功能 RED。
- 初次 offscreen 基线缺字体；改用正常 Windows QPA 取得可读的中文截图。测试依赖从同一 normal SDK 的 bin/plugins 解析；缺测试 DLL 或 QPA 的启动错误没有计为产品功能失败。

## v2 公共入口最终 Windows 验证

相对 v1 的精确 delta：51921 字节，SHA256 `f84cf1d7946660d3dc29e89ab1a1f8a4db5fa9e19cb34d281632a009567bfa27`。`MaiChat` tree 为 `3bdf317adf244f3cb33c9cb72cea8bdbbfb0ba24`，Core tree 仍为 `4ab2cc60987f23c7d67659a4ea05e6f89cc92bcd`。三份 Panel 源/测试与 HEAD 完全相同；最终新增功能只由公共 MainWindow/Bar/Control 组合提供。

| 验证 | 实际结果 |
| --- | --- |
| 普通公共 bar 完整组 | no-op 6 FAIL 后，最终 8 PASS / 0 FAIL / 0 SKIP，0.683s，含 2 hooks |
| MainWindow 公共归属、会话与原窗口相关 9 项 | 原缺公共 bar 1 FAIL 后，最终 11 PASS / 0 FAIL / 0 SKIP，4.810s，含 2 hooks |
| 恢复 HEAD 的原 Panel 完整组 | 80 PASS / 0 FAIL / 0 SKIP，14.504s，含 2 hooks |
| 原生自有窗口→公共 bar 按钮停止 | 3 PASS / 0 FAIL / 0 SKIP，0.937s，含 2 hooks |
| 正常 App 与受影响目标 | build0，windeployqt 与 3 binaries / 114 PE imports / Qt plugins 检查通过 |
| 私有目录、裸 Windows PATH 正常 App smoke | PID 47840，exit0，1.743s；运行路径与当前 normal build/bin 一致，Qt 模块来自同目录 |

普通 control 的 11/0 结果沿用未改源的原始回执，未冒称与上表同次全套执行。新公共 bar 的独立测试不访问原生桌面；原生整链另用实际 WGC 和自有窗口验证。

主窗口测试通过真实本机会话列表切换 A/B，逐项保留草稿；实际切到远端 owner 路由时，公共条仍显示本机状态，停止仍作用于同一个本机 backend。普通 `setMediaServiceAccessProvider` 触发 owner 页面重建后，bar 和外部 control 地址不变，再次停止仍使用同一 backend。默认 control 由公共 bar 在析构时管理，外部注入保持借用；会话页不持有它。旧 owner rebuild 本身销毁会话草稿的行为没有扩围修改。

开发者已查看 `creation-common-local.png` 和 `creation-common-remote.png`：条目横贯左右两列，位于会话内容之外。Windows v2 实际开发 exe SHA256 为 `d1ea324f9e8a6eeba90fb071c957d377071a8b4353e300ae0a54d5c43fa343e7`。Mac 最终公共入口消费者结果随后单列。

## Mac v2 消费与原生验证边界

同一 `3bdf317a` 产品树已核准。normal configure0；App/bar/MainWindow/native 四目标 build0，40.255s。offscreen 公共 bar 完整组 8/0/0，393ms；两项新增 MainWindow 公共归属原组 4/0/0（含 2 hooks），508ms。本轮没有再跑已恢复 HEAD 的 Panel 或未改源的 control 全组。开发者已亲看两份自有 local/remote QWidget 截图，公共栏横跨左右两列；这些组件截图不作为解锁桌面的物理显示验收。

同一正常 App 的私有 HOME、裸 PATH Cocoa `--smoke --debug`：PID 60422，exit0，3.788s，实际六份 Qt/Cocoa 模块均来自 normal Qt 6.12 SDK。exe SHA256 `12f9eb49a803a55214b4a4425aa3763b017fcaf1dde85049041d032bfa99b6a7`。Root 已逐项核收到的七份原始 receipt/log 的长度和 SHA 均等 Mac 最小回执。两份接收 PNG 与供方保全原 PNG 的字节不同，接收版本仅用于亲看公共区域位置，JSON 分列收到与供方原件摘要，不声称图片逐字节相同。

原生 fixture **实际 exit1 / 2 PASS / 1 FAIL / 0 SKIP，432ms**，首错为 `GUI session is unavailable for screen capture`，发生在开始原生采集之前，尚未执行停止按钮。原日志按失败保留，不能记为权限 SKIP 或 Mac 原生 GREEN。

私有 Objective-C++ 探针复用 Core 原 `readDesktopAvailability`，只读五个布尔值，不采集画面、不申请权限。相同二进制、相同私有 HOME/TMPDIR/Cocoa 环境，唯一变量为 sandbox wrapper 有/无；主线程和工作线程四组观察一致：`guiSession=true`、`onConsole=true`、`loginDone=true`、`frontmostKnown=true`、`notLoginWindow=false`、`canCapture=false`。因此是前台被系统报告为 `com.apple.loginwindow`，Core 既有安全条件拒绝采集；隔离 wrapper 的有/无没有改变这次结果。没有为验收改 Core 或放宽原断言。

已明确请用户解锁 **Mac，而非 iPhone**。不留自动 UI 等待；解锁后仅续同一个自有窗口 fixture。当前只能声称 Mac 编译和公共组件回归通过，Mac 原生采集/停止验收仍待桌面条件可用。现装 MaiChat、真实会话、版本和已发资产未改。
