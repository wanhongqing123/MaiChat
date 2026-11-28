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
