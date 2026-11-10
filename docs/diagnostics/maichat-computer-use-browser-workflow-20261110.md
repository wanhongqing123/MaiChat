# 自有私有浏览器：原生地址栏导航验收

## 目标与执行范围

在已安装的真实 Chrome 中，由确定性测试执行器通过普通 `DesktopComputerController`
输入接口完成：观察第一页、`Ctrl+L` 聚焦地址栏、输入第二页完整 URL、Enter 导航、
重新读取地址栏并核对第二页正文。两页均由测试自己的 loopback HTTP 服务提供；第二页
必须收到第一页设置的随机 HttpOnly cookie，浏览器进程和临时 profile 始终相同。

Root 在 Windows 交互桌面实际执行并查看图片。本记录另行只读核对最终回执、三份冻结源码、
图片文件存在性及源码/产物 SHA256；这次只读复核没有重跑 GUI、注入输入或运行构建。
这是普通原生浏览器执行器验收，回执明确 `visual_model=false`，没有调用模型。

## 平台、源码与产物身份

- 日期：2026-11-10，Asia/Shanghai。最终首次采集时间为 11:36:18.304 +08:00。
- 平台：Windows 11 家庭版中文版，10.0.26200；显示器原生边界 `(0,0,2560,1600)`。
- 浏览器：`C:/Program Files/Google/Chrome/Application/chrome.exe`，156.0.8078.80。
- Git 基线：`a8e1f5e42617c8f4c9f52e9da5900f48ec9f978b`。本次浏览器测试三文件当时尚未提交；
  构建也包含当时工作区的 Core 上下文增量，因此不把基线提交等同于完整运行源码。
- 实际执行：正常缓存 `MaiChat/desktop/.cache/build-qt6-ui-20261109/bin/desktop_computer_display_native_test.exe`。

| 文件或产物 | SHA256 |
| --- | --- |
| `MaiChat/desktop/tests/DesktopComputerDisplayNativeTest.cpp` | `FF06678CC54EA209C6ED07BCA130A01EB962870E56141FCB675D7C2CEAEA1F3E` |
| `MaiChat/desktop/tests/DesktopComputerBrowserNativeDependencies.h` | `4C54761B8EF89C861E0D6582F9EFD4C40F16B1DD6E615E44494716F99DAF4777` |
| `MaiChat/desktop/tests/DesktopComputerBrowserNativeFixture.h` | `9C5FF16269C73A281A130B229D2F15B6E030DCCBEFE712E991B254B85CF052AA` |
| 实际测试 exe | `24D609A39B2D9708E1365BE1A22DEBDCC05E140F75DDB86A89B239A899D659B9` |

正常构建保留既有 Qt profile、编译选项和 SDK。v7 的正常 App 与三个电脑测试目标均构建
exit 0，App 的 `windeployqt` 及 3 binaries / 114 PE imports / Qt plugins 检查通过。
最后仅重新构建 display-native 测试目标，包含随后应用的 `MaiContextCheckpoint.cpp` 增量，
exit 0；本节不替代 Core 的独立回归记录。

## 可复现操作与归属约束

1. 在 Root 协调的 Windows 交互环境中使用已部署依赖的正常测试目标，设置
   `QT_QPA_PLATFORM=windows`、`MAICHAT_RUN_COMPUTER_DISPLAY_NATIVE=1` 和一个全新的私有
   `MAICHAT_COMPUTER_NATIVE_EVIDENCE_DIR`，执行 `desktop_computer_display_native_test.exe --browser-workflow`。
2. 测试创建全新的临时 profile 与随机 `127.0.0.1` HTTP 端口。服务器提供带随机路径的
   `/first`、`/second` 及各自 ready 信标，不依赖 Qt Network。
3. 安装目录中的 Chrome 通过自有 `QProcess` 以 `CREATE_SUSPENDED` 创建。核对实际进程
   映像路径、PID、唯一主线程的 PID/TID；先加入自有 `KILL_ON_JOB_CLOSE` Job，再恢复线程。
4. 输入前核对新 profile 的 `DevToolsActivePort` 文件、同一运行进程/Job 和唯一根 HWND。
   该文件仅用于身份回执；导航没有使用 CDP 输入或 DOM 修改。
5. 普通窗口保留可见地址栏。只对该已核实 HWND 设置临时 `DWMWCP_DONOTROUND`、置顶及
   显示器矩形位置，均使用本次窗口的系统 API；不改变其他窗口、全局主题或用户 profile。
6. 截图与每次输入继续要求：同一进程/profile、严格前台/owner 链、同一显示器、完整可见
   覆盖、非圆角偏好，以及没有外来可见表面。未知或拒绝条件不能通过。
7. 通过真实键盘执行导航，核对独立服务器 cookie/route/ready、地址栏 UIA 和正文专用 marker，
   最后取得等待 500ms 的正常观察。结束时确认 Job 已无活动进程、主进程退出并删除私有 profile。

浏览器代码仅增加两个私有测试 helper；原测试 cpp 相对基线只有两个 include 与一个 CLI 分支。
原默认输入及编辑器 Save As 流程保持原操作。没有 `--no-sandbox`、任意现有浏览器 attach、
真实用户 profile、登录账号或付费模型请求。

## 两次真实 F11 失败及定位证据

| 执行 | 关键观察 | 结果 |
| --- | --- | --- |
| `browser-owned-1` | F11 模式；首个 `Ctrl+L` 已派发，返回 `dispatched_observation_failed`；只收到 1 帧。该版回执尚未保存底层 observation error。 | exit 1；停止后续输入，Job/profile 清理通过。 |
| `browser-owned-2` | 自有 PID/profile/前台仍有效，UIA 已读到完整第一页 URL；WGC worker/session 各 1，仍只有 1 帧。 | 明确报错 `Windows Graphics Capture timed out waiting for a frame newer than the requested barrier`；exit 1，清理通过。 |

Root 看到初始 F11 图片中没有正常可见地址栏。第二次的 UIA 证据确认焦点变化已经发生，
但输入后未取得晚于屏障的新帧。这是该 F11 fixture 下的真实操作反馈边界，不能据此断言
输入未执行或把旧图当新图。两次失败后都没有盲目重发快捷键或继续输入第二个 URL。

随后把验收环境改为地址栏可见的普通 Chrome 窗口。生产截图链、帧序号/时间屏障和输入
实现没有修改；没有添加 CSS 动画、CDP 输入或另一套采集链来制造帧。

普通窗口另有实测边界：首次请求 outer `(0,0,2560,1600)`，DWM 可见边界实际为
`(9,0,2542,1591)`。第二次只按测得的不可见边框补偿，将 outer 调整为
`(-9,0,2578,1609)`，DWM 可见边界变为 `(0,0,2560,1600)`。没有固定 padding 或覆盖条件放宽。
圆角偏好读取为 0，设置及回读为 1（`DWMWCP_DONOTROUND`），三个 HRESULT 均为 0。

## 最终通过与共用入口回归

最终 `browser-owned-3`：Root 实际运行 exit 0，约 3.19 秒。回执无 failure，浏览器 PID 39548、
已核实主线程 37856、根 HWND 20712246；本轮随机 cookie 和 URL nonce 仅为自有测试数据。

| 证据 | 实际结果 |
| --- | --- |
| 第一页及其携带 cookie 的 ready 请求 | 各 1 次 |
| 第二页及其携带 cookie 的 ready 请求 | 各 1 次；`second_cookie_verified=true` |
| 地址栏 UIA | 初始第一页、输入后的第二页、导航后的第二页共 3 次匹配；无 readback error，scope 均有效 |
| 正文 UIA | `page_marker_uia=true`；正文专用 `SECOND BODY` nonce 与 tab title 不同，要求正几何并与显示器相交 |
| 原生动作回执 | 5 次均 `dispatched=true`、`state=dispatched`、observation error 为空、cancellation 为 null |
| 同一浏览器与 profile | `same_process_profile=true`、`job_contained=true`、profile marker 验证通过 |
| 采集 | worker/session 各 1；收到 19 帧；保存 7 张图，帧序号依次为 `1,4,6,10,14,17,19` |
| 清理 | `owned_job_stopped=true`、`private_profile_removed=true`；独立审计时该 PID 与已记录 profile 路径均已不存在 |

Chrome 在初始和导航后 UIA 读回时省略了 `http://`，代码只接受完整预期字符串或仅省略
这一固定前缀；host、port 与随机路径仍须全部精确匹配。输入完成后的第二页 URL 则带有完整 scheme。

Root 亲自查看 FIRST、输入 URL 和最终 SECOND BODY 图片。中途输入图片早于后续 UIA 完整
读回，只显示部分 URL；完整 UIA、独立第二页请求和最终第二页画面共同支持导航成功。
最终图片中的地址栏仍被选中。内部步骤名 `return-to-page` 只对应发送 Escape，不能解释为
已把键盘焦点恢复给网页；本次没有验证网页键盘焦点恢复。

同一最新二进制随后执行无参数的原默认 native click/type/Enter 回归，Root 实际记录 exit 0、
约 1.21 秒。子 PID 35956，中文文本及 Enter 一次确认符合，3 次动作无 observation error，
worker/session 各 1、frames_received=7。

## 私有证据与验收边界

- 最终目录：`.cache/computer-use-20261110/browser-owned-3/browser-workflow-99b212f3-6f31-4dd0-a166-09cd1bdbdcf9/`。
  包含 launch、corner-policy、placement、readiness、workflow receipt 和 7 张 PNG。
- 最终运行日志：`.cache/computer-use-20261110/browser-workflow-run-3.txt`。
- 历史失败：同目录层级的 `browser-owned-1/`、`browser-owned-2/`，均保留原回执与图片。
- 默认回归：`.cache/computer-use-20261110/browser-final-default-regression/` 及同名 `.txt`。
- 构建：`browser-workflow-v7-normal-window-build.txt`、`browser-workflow-v7b-nonrounded-build.txt`。

原始截图留在私有缓存。原生浏览器与 Windows 控件可能带系统标签，不将其描述为全是假数据，
不把原图复制到公开源码或文档。浏览器启动参数提供本进程后台请求抑制、DNS 映射和本地
拒绝代理；本轮服务器拒绝了 14 次非 fixture 请求。这证明发生过拒绝处理，不能证明零外联，
也不等同于 OS 网络隔离。

本次通过范围为 Windows 上自有新 profile 的真实普通浏览器、地址栏导航、同会话 cookie
连续性、原生观察反馈及清理。实际视觉模型自主决策、用户已有 profile/标签页、网页焦点恢复、
多显示器浏览器任务、浏览器原生文件对话框及其他平台仍需各自独立验收。
