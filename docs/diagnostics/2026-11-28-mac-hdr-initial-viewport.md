# Mac HDR 初显画面与控件分离

## 复现与根因

2026-11-28，用户从云盘打开视频后，画面只占窗口一角，标题、底部控件和加载指示
按较大窗口布局。mac-maichat 在 macOS 27.2 arm64 的独立正常源码构建中，
用自有 HLG MOV、本地入口和 MainWindow 云盘入口的 localhost TLS Range 源复现。
基准 `6adfe4c02` 的四个文件与公开 4.0.2 源 `74470f39` 相同。

旧初显测试曾报告通过，但原始可见采样显示 Qt/原生窗口/Metal 为 1008×567，
AVPlayerLayer 仍是 480×270；旧断言只观测 Metal/viewport，遗漏了实际 HDR 视频层。
同一测试补上 AV 层断言后，旧产品 52 个可见样本全部尺寸不符，测试失败。

隐藏准备阶段的 QWidget resize 先于 Cocoa 的 NSView bounds 提交；AV 层读取旧 bounds，
随后 map 仅同步 Metal，不能纠正独立 AV 层。

## 最窄修复与聚合

`MaiMacVideoPlayback::resize()` 使用已确定的 QWidget 逻辑宽高设置同一 AV 层，
不乘 DPR，保留无隐式动画事务；`finishPreparedShow()` 在 map 后同步 AV 层。
不改变默认 2/3 尺寸、播放代际、HDR 元数据或全屏意图。

Mac 四件供件 ZIP 为 2058773 字节，SHA256
`e05359a1dddb00859d6e4a6ea71f0b67ec77b08dc3d406eb93ec6f8b3b706aae`。
Root 独立核包、manifest 前后大小与 SHA 后聚合两个产品文件和两个测试文件。
本机一个 before 为 CRLF，规范化后与供件 LF 完全一致，其余三个 before 原字节相同；
聚合 post 原字节均与 manifest 相同。未覆盖 AgentChatPanel 或其他工作区改动。

## 验证与边界

Mac 同一加强测试新代码 56 个可见样本全部为 1008×567；自有本地 SDR/HDR、
真实 MainWindow 云盘入口的合成 SDR/HDR、用户窗口 resize 和 AV native videoRect 回归均通过。
只截取测试进程指定窗口及子 HUD 的 ScreenCaptureKit 原图，Root 亲看了该 PNG，
视频与上下控制栏完整同框，没有拼接。Qt SDK 未修改。

供件与安全摘要保存在 `.cache/steer-input-20261128/mac-video-delta/`。
这是 Mac 自有组件运行证据，未访问真实用户视频、账号、云盘或数据库，未停止用户 App。
没有重新打包或替换公开 4.0.2，Root 不能将 Windows 编译验证称为 Mac 实机验收。

Mac 随后补充既有 native HDR 全屏/maximize/restore 回归通过，四件 post 未再变动。
Root 合并后的 Windows 正常 App 与视频测试目标编译成功；同一 prepared geometry
用例的 SDR、720p、1080p 三行均通过（加初始化/清理为 5 passed、0 failed），
结果保存为 `final-video-qt.txt`。这验证跨平台构建及 Windows 原路径，不能代替 Mac AV 层运行证据。

## 最终整合源复核：全屏项目待闭合

Mac 消费最终 `23288f8586f3a8cbd82a83d44a3eea0f57add6eb` 干净源码后，
正常 configure 退出 0（10.574 秒），App/GUI/video 目标构建退出 0（103.646 秒）。
实际 App archive 消费者 InputQueue、TurnRunner、ToolLoop 均通过；Header 与 receipt
原 slot 合计 13 passed，local SDR/HDR 初显、cloud TLS Range SDR/HDR、窗口及 HDR resize 均通过。

同批最后的 `macFullscreenKeepsNativeVideoGeometry` 有一次 `!nativeTransition`
断言失败（约 6.9 秒）；这是最终源的新失败记录，不能被前一阶段同用例通过覆盖。
2026-11-28 14:17（北京时间）仍在定位，未修改产品或断言，未重新打包或签名。
须核失败循环、Will/Did 原始通知及延迟回调时序、Qt/native 状态、owner/AV 几何和 Space，
区分实际原生转场不收敛与观察状态未收敛；此刻不认定根因或宣称 Mac 最终验收全绿。

失败绑定：无 data row，循环 0，`maximize-return 0` 已成功，接着第一组 70ms 全屏连点
在 3569 行失败。原 binary SHA256 为
`d3121717616a9af40c547dda5df39ae55f13e671d59c539ba26857fda51072e7`。
Root 精核 717 字节安全回执（SHA256
`f74a2ecc4eb9cfecff6e947f527b56c8bfce58e48b045e592063051ee7c5e73c`），
其中测试 TU、observer TU、Mac 测试 helper TU 的大小与 SHA 均与 final Git blob 相同。
原 run 未启用逐帧或原生通知 trace，没有证据断言 Will/Did 缺失。
Mac 准备仅在外置 observer 诊断副本记录通知到达、Did 排队/执行和窗口 identity；
原产品与测试 TU、操作节奏、截止时间及断言均保留，增强观察的 binary 单独绑定。

## 一次增强观察：未复现原失败

Mac 随后供件 `fullscreen-observer-single-diagnostic-23288.zip`，29989 字节，SHA256
`e3e05c932eae1ad8b8b10e0676ae36193716e634dfa63ce03d081f69280b8ad8`。
Root 独立核全部 15 项 manifest 的大小/SHA，原 observer 与 final Git blob 完全相同，
仅日志 diff 精确重建 diagnostic post，测试 TU 摘要与 final 相同。
新诊断 exe 的供件绑定为
`1621168768e30e839597b44301c649ec1b8d559140388d46b13b91298833cf23`；
包内没有该二进制，Root 未独立重算 exe 摘要，不将源码/回执核验夸大为二进制复验。

独立正常构建后的单 slot 增强观察退出 0，QtTest 为 3 passed / 0 failed（含初始化/清理），
14.274 秒，进程总计 17.3 秒。这是另一个带日志的 executable，不是原 binary 重验。
Root 从原 Qt 日志重新提取记录，与 `observer-events.json` 逐条完全相同：

- 36 条观察器接收记录，包含主窗产品/test 两个 observer 各 16 条、重开窗产品 observer 4 条，
  不能把双 observer 的计数说成 36 次独立全屏转场。
- 每个 event 的 Will 同步调用或 Did 排队/执行/返回阶段完整，18 个 Did 回调均有对应记录。
  所有记录的 bound/current window identity 相同、owner 与 active 为真。
- 最大 Did 排队 155ms。首轮 DidExit，product 回调在 5000ms 执行、5153ms 返回；
  test 回调在 5153ms 执行。此处时间均沿用同一原始 wallMs 的尾部，完整值见原轨迹。
- 279 个逐帧样本属于原主窗 156228，最后一个样本在关闭/重开前恢复至 owner/AV bounds
  740×440，native/Qt 均非全屏、active Space 为真；重开窗通知另用 observer 3 记录。

原始日志、逐帧 JSONL、绑定与 Root 机器复核结果保存于
`.cache/steer-input-20261128/mac-final-fullscreen/`，复核输出为 `root-independent-review.json`。
这次没有观察到丢通知、Did 卡三秒或已记录时刻的窗口 identity 替换，
不能据此回推原失败根因或关闭最终验收。原失败仍为 2 passed / 1 failed（6938ms）。

该增强日志在转场回调中构建 JSON、调用窗口属性及写日志，可能影响调度。
下一步仅准备外置的固定容量内存记录探针供审：回调不格式化、不写文件，
不调用 owner->winId 或读取 frame；正常结束再一次导出，并记录线程与溢出事实。
原产品、原测试 TU、70ms/3 秒及断言均不变；尚未批准或执行追加运行。
不以增加 timeout、重复取绿或猜测修改状态机代替失败现场，未打包/签名/发布新版本。
