# 已登录浏览器 CDP 接入与受控输入评估

2026-11-07 00:30 起，按 Windows Agent 修改内网 CR 页面标题的反馈核对。
源码基线 ef31e763；本轮只做源码评估，没有连接内网 CR、读取真实登录凭据、
修改 CR 标题或验证用户 Windows 上的持久化 Chrome。

复现目标：接入用户明确指定的既有 Chrome 调试端点，打开 CR，点击标题进入内联
编辑，替换为新标题，回车保存，刷新后仍能读到新标题。整个过程中保持原浏览器
及登录态，不重新启动占用同一 profile 的 Chrome，不关闭其他标签页。

## 与源码一致的结论

- `DesktopBrowserTools.cpp` 已使用 Qt HTTP/WebSocket 实现 CDP，不依赖 Node。
  `runBrowser` 总是创建临时 profile，端口为 0；`open` 不接受已有 endpoint 或
  profile 参数。当前确实不能复用用户现有的已登录浏览器。
- 工具现有动作只有 open/observe/screenshot/click/type/close，没有按键、刷新或
  attach。已有 element_id、snapshot_id、expected_url、expected_label 的观测契约
  可以继续复用，无须另造一个完全重叠的浏览器工具。
- `actionScript` 直接赋 `e.value` 后派发 input/change。它没有绕过输入元素的
  自有 value setter；普通 HTML 测试通过不能证明 React 受控输入能保留新值。
  需要实际框架输入和刷新后的服务端结果回归。

## 反馈中需要纠正的归因

- `DesktopComputerController::clear` 实际执行 `sessions.erase(sessionId)`，是清除
  已观察窗口记录，不对 DOM、UIA value 或输入内容做任何修改。返回 state=cleared
  不能当作清空文本的证据。其后 type 报 observe a window in this Session first
  正是这条调用链的直接结果，不能据此认定是 React 回填造成的。
- Windows `maiDesktopType` 已用 SendInput/KEYEVENTF_UNICODE；`maiDesktopKey` 已支持
  Enter 等键的按下/松开。准确的缺口是键组合/替换输入等能力有限，以及浏览器
  登录态与 DOM 定位没有接通，并非完全没有系统键盘注入。
- 没有读取用户的 CR 页面或 React 代码，目前不能确认该特定标题控件的实现。
  React 受控输入可能回填是有效的独立假设，不是本轮失败日志已证实的根因。

## 建议的最小实现范围

1. 在同一 DesktopBrowserManager 增加 attach 接口及工具入口，接受明确的本机 CDP
   endpoint；发现目标并创建本 Session 的标签页，返回 page_id。连接已有浏览器时
   不启动第二进程、不强占 user-data-dir。关闭仅释放本 Session 创建的页及连接。
2. 复用 observe 和元素校验。额外覆盖可点击标题这类不在现有 a/button/input 列表
   中的元素，允许受观测约束的精确定位；不要只修输入，留下无法进入编辑态的缺口。
3. 输入调用原生 HTMLInputElement/HTMLTextAreaElement setter 并派发事件、聚焦；
   增加 CDP Input.dispatchKeyEvent 的按键入口。需要浏览器输入语义时采用 CDP
   输入命令，不能将派发了事件当成已经保存。
4. 增加 reload(ignore_cache)，刷新后重新 observe 核验持久化值。任意 JS eval
   不是完成此场景的前提；如另加 eval，应明确它的操作范围和结果语义。
5. 补测试：同一登录 cookie 的复用；真实 React 受控 input/textarea；回车保存后
   reload 仍一致；多页/多 Session 不串页；detach 后浏览器和用户原页仍存活；
   endpoint 不存在、页面消失、SSO 跳转均返回可理解的失败。

参考：[React input 受控组件契约](https://react.dev/reference/react-dom/components/input)、
[CDP 发现及目标接口](https://chromedevtools.github.io/devtools-protocol/)、
[CDP Input](https://chromedevtools.github.io/devtools-protocol/tot/Input/)。
本轮同步澄清 computer.clear 的工具描述、普通接口注释和返回值：保留原有 cleared
字段，额外明确 scope=window_observation、text_changed=false 及重新 observe 的步骤。
未改变窗口记录的清除行为，没有增加系统输入或浏览器操作。
本文件是评估和验收范围，不能作为 attach 已实现或内网 CR 已实测的交付结论。

## 对“脑手分离”补充方案的核对

现有 `remote/RemoteInputProtocol` 已定义 Session 绑定、自增 sequence、键鼠/文本和
ReleaseAll；`RemoteInputSender` 发送，`RemoteInputInjector` 在目标端检查序号、跟踪
按住的键，断线或静默超时释放；`RemoteInputSink` 分别调用 Windows SendInput 和
macOS CGEvent。它与用户提出的发送/传输/接收分工一致，适合复用而非另造底层协议。
现有传输绑定 TRTC 远控会话，不能直接宣称已有独立的无视频 Agent 输入服务。

独立桌面 Agent 工具也已采用 C++ SendInput/KEYEVENTF_UNICODE；UIA 用于观察元素和
判断安全输入，并非通过 UIA setValue 输入。`maiDesktopActivateWindow` 在操作前调用
SetForegroundWindow，随后比较 GetForegroundWindow；失败返回 Busy，不继续注入。
这条路径不调用 PowerShell，因此 ConstrainedLanguage 不是其运行依赖。

需要补的实际边界：Agent 组合键与替换文本入口；目标端环境自检；接收/注入/读回三种
不同回执；跨端工具入口与已有会话授权、停止开关联动。远控 WindowsInputSink 的
若干 SendInput 调用目前忽略返回值，Injector::handlePacket 的 true 只证明通过协议
校验并调用 sink，不能当成 OS 注入成功或业务保存成功。应先修正这个结果契约，再
把它暴露为 Agent 操作回执；单独加一层 JSON 不会自动补齐证据。

交互用户的 Session ID 不固定为 1；应确认接收端在目标用户的活动交互 Session 和
输入桌面。SendInput 返回已插入事件数量，UIPI 会限制跨完整性级别注入，但返回值
无法单独标明是 UIPI；它也不保证目标应用处理或 CR 保存成功。按需截图/读回依然必需。
参见 [Microsoft SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)、
[交互服务与用户 Session](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services)。

落地应先用已有编译工具在隔离记事本验证组合键、中文、前台拒绝和读回，不需要先用
shell 编写另一套注入器。网页标题场景仍优先补 CDP attach 与受控输入保存/刷新回归；
通用 UI 输入链路作为原生客户端和无 DOM 流程的统一执行层。本轮没有新增跨机输入
服务、放开远程控制权限或操作真实用户窗口。
