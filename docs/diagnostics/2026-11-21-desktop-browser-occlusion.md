# Desktop browser 遮挡与加载状态修复

2026-11-21，Windows；本地主线基线 `38d25b9d7a41ad29128e95289f44937c676ca452`。
用户现场为 macOS：文档 readyState=complete、visibilityState=hidden，observe 被拒绝。
本机先做普通工具与原生 CDP 的自有本地页面复现，不访问用户浏览器 profile 或供应商。

## 修改前复现步骤与预期

1. 正式 DesktopBrowserManager/desktop_browser 打开自有 loopback 页面。页面包含输入框、
   点击计数按钮、屏外按钮和隐藏按钮，独立 profile；不使用模型生成任何测试请求。
2. 自有 fixture 明确报告 hidden，先验证原实现 observe/操作/截图受 visibility 门禁阻断。
   这是门禁的确定性复现，不代替真实 macOS 遮挡验收。
3. 保持响应正文未完成，让 readyState=loading。预期 observe 返回 URL、snapshot_id、
   visibility、空 elements；screenshot 可针对同一个合法观察 URL 保存有效 PNG。
   about:blank 或越过 allowedHost 仍不得成为可操作快照。
4. 默认 open 为 headless；显式 mode=windowed 启动有窗口。两种模式均不得将窗口可见性
   当成 DOM 正确性或截图前置条件，不改 User-Agent，不自动重启或切模式。
5. observe wait_ms 默认为0，允许0..5000。等待 readyState=complete 或到期，期间可取消；
   到期返回当前诚实状态，不假装 ready，不产生重复动作。interactive 仍明确报告 ready_state。
6. 点击前滚动元素到中间，再检查节点连接、CSS可见性、有效尺寸和禁用状态。
   原路径是 DOM element.click()，不是屏幕坐标点击；不得新增(0,0)坐标回退或键盘提交。
7. snapshot_id/element_id/expected_label、URL、单 allowedHost、每次open新profile及page类型
   过滤保持；失败观察清除过期可操作状态。按钮变隐藏、标签变化、旧快照仍应拒绝动作。

## 实施与验证顺序

- 在 DesktopBrowserToolsTest 增加确定性 hidden/loading/新参数契约；先运行旧生产观察RED。
- 修改 DesktopBrowserTools.h/.cpp：保持旧普通API重载可用，新增模式/等待的明确入口；
  工具schema与普通调用复用，不新增依赖或平台脚本。
- 用同一自有fixture GREEN，覆盖取消、等待到期、host越界、旧快照、隐藏控件和两种模式。
- 构建正常Desktop App，部署和PE门禁通过后从完整输出运行 --smoke；再运行同源普通工具
  完整open/observe/screenshot/type/click流程并查看PNG。若能验证正常App的调用路径，单列证据；
  否则不得将普通库test当成GUI对话区调用已验。
- 真实 macOS 完全遮挡、macOS窗口激活和站点反爬表现当前无本机环境，不预称通过。

## 范围决定

默认无头，保留windowed，符合后台任务用途。activate为可选P2，本轮不引入跨平台抢焦点
行为；观察/截图/DOM动作无需它即可工作。两模式都保持原浏览器profile隔离。
源码本轮可静态确认的第四处 visibility 门禁位于 actionScript，也必须移除，否则仅修复
observe/screenshot 仍不能在遮挡状态完成输入与点击。

当前文件先于实现写入；实际日志、耗时及边界将在验证后补记。用户现装、旧Release和其它
任务WIP不改动；不提交或推送本批以外的改动。

## 实施与实测记录

- 原生产、自有hidden页面、正式普通工具测试：exit1，精确返回
  `Agent browser tab is not visible; show it before continuing`。另一份旧生产测试在
  schema无mode字段处失败，确认新参数尚未实现；均不是编译错误冒充功能RED。
- 移除observe、screenshot和actionScript三处document可见性门禁，visibility仅返回观测。
  默认headless，可显式windowed，不改UA；旧普通API通过重载保持可调用。
- 同主机loading观察带nonce、实际URL和空elements，可截图；about:blank只报告导航中，
  不建立可操作URL。wait_ms验证整数0..5000，等待受取消和截止时间约束，返回ready_state。
- DOM动作滚动后重新核实尺寸/CSS/连接/标签/URL/id及禁用状态；延迟点击在真正触发前
  再核查同一组条件。独立复审指出延迟任务只查部分条件的P2，已修复，并用两个自有按钮
  在滚动后异步改变aria-disabled/label验证不会点击。既有密码拒绝和旧快照拒绝仍过。
- 截图前后均核对实际URL，导航竞争不保存像素；不抢焦点、不运行平台激活脚本。
- `desktop_browser_tools_test`正常消费者构建通过。最终默认case exit0，实际覆盖
  headless普通页面、type/click、屏外按钮滚动、延迟守卫、loading PNG、wait_ms到期及
  complete后返回、非法参数、windowed和窗口完全遮挡。确定性hidden case另已exit0。
- Windows完整遮挡测试只定位本测试直接创建的Chrome/Edge子进程窗口，以自有不激活的
  opaque窗口盖住它；CDP截图/观察/输入成功。首轮document.visibility仍为visible，
  不将首轮冒称原生hidden复现；后续原生hidden补验见下。PNG显示网页而不是遮挡窗口。
  证据：`.cache/browser-covered-test.log`、`browser-covered-headless.png`、
  `browser-covered-windowed.png`，结果`DOM/type/PNG succeeded`。

### 明确保留的边界

额外检查Windows最小化：DOM complete且visibility=hidden，截图CDP超时；日志
`.cache/browser-minimized-observation.log`保留。最小化不是完整遮挡，不用它替代原现场，
也不声称windowed在所有hidden状态均可截图。默认headless不依赖原生窗口显示。
真实macOS遮挡尚未在本轮执行；无Mac GUI/系统激活验收。未访问Bing等外部站点验证反爬。
完整流程证据来自同源真实浏览器与正式普通工具，不是付费模型调用；正常App部署与smoke
由五批集成验收另记，不能将此test等同于在已登录App聊天界面完成真实Agent轮次。

## 最终同ABI补验

第6批改变MaiTool虚表后，正常缓存重新编译全部相关消费者并重链本测试；再次运行
确定性hidden和完整case，均exit0。随后为观察实际像素再保存一次真实窗口覆盖PNG：
`visibility=hidden; DOM/type/PNG succeeded`，并实际查看确认网页标题、输入框、按钮完整，
不是遮挡窗口或黑屏。证据：`.cache/tool-batches-20261121-final/browser-native-covered.log`
及同目录`browser-native-covered.png`。这是Windows实际原生hidden补验，不代替Mac验收。
