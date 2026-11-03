# 桌面 Agent 指定起播时间后重复播放（2026-11-03）

用户补充：Mac 创作中心让 Agent 任意选择视频并从指定分钟（例：第 5 分钟）开始。
定位已成功，但画面播放几秒后反复回到该起点，重复若干次才连续播放；不只是进度条。
iOS 尚无用户现场结论，应同步做指定起点回归。当前安装桌面 2.2.32，待交付 2.2.33。
源码 main `1997947219596e6520be27ed892bd6ea1820ff2e` 加当前未提交工作区。

预期：一次打开／定位只执行一次，从指定源时间持续前进；不能被旧时钟、重复派发、
显示刷新或重建播放器反复拉回。现有第一帧起点断言不足，须持续观察后续帧与请求次数。

现有路径：DesktopPlayVideoTool 解析 start_seconds，经 openVideoWindow 传入控制器；
maiFfplayRunAtSpeed 只加一次 -ss。运行时绝对 Seek 在事件线程 atomic_exchange 消费。
以上是代码路径，不据此排除重复工具调用或底层状态问题。

## 实际证据与失败回归

只读核对本机当前账号 Agent SQLite 的 parts 调用记录，未修改线上或本地用户数据，
不把聊天原文、媒体路径、账号信息和令牌导出到日志。以下四次调用属于同一 assistant
message，调用 ID 各不相同（只记录数值与时间）：

| created（Unix 毫秒） | 工具 | 请求 | 旧返回 |
| --- | --- | --- | --- |
| 1793634921659 | maichat_play_video | start_seconds=1200 | opened=true |
| 1793634922702 | maichat_video_command | seek_seconds=1200 | accepted=true |
| 1793634923575 | maichat_video_command | seek_seconds=1200 | accepted=true |
| 1793634924957 | maichat_video_command | seek_seconds=1200 | accepted=true |

这证明该次现象存在多次独立工具调用，不能归因为一次定位被解码器重复执行。
记录不能证明模型为什么重复下发；“因为不能查询完成状态而尝试再次确认”仅是推断。
接口可确认的缺陷是：返回只有已入队，且 action 枚举没有只读 status，重复定位全部
执行。播放器正确执行这些独立定位，因而画面被反复拉回。

本地测试实际注册工具，打开 20 秒公共合成素材，从 5 秒起播，再间隔 700 毫秒重复
请求 5 秒。修改前 `agentRepeatedTargetDoesNotReplayFrames` 失败：实际 position_us
倒退超过 100 毫秒（并非只检查滑块）。失败记录：
`/private/tmp/maichat-start-repeat-red-20261103.xml`。

## 修复边界

- play/command 返回实际只读状态：ready、running、position_seconds、duration_seconds、
  paused、seeking，以及有待处理定位时的 seek_target_seconds。无真实时间采样时为 null，
  不把请求目标充当当前位置；`opened` 只表示窗体已创建，`accepted` 不代表已完成。
- 新增 `maichat_video_command action=status`。关闭后仍返回 closed，查询不恢复播放、
  不打开窗口、不触发 Seek。
- 打开与控制工具共享 GUI 线程内的最后导航记录：当前窗体、Session、assistant message、
  目标源时间及导航版本。只有同一回合、同一窗体、同一目标且期间没有新导航才跳过
  重复 Seek，明确返回 deduplicated=true。不同调用 ID 不会绕过同回合保护。
- 新用户回合仍可回到相同位置；同回合明确重新观看可使用 force_seek=true。
  手动拖动、章节/相对定位、重播、换片使导航版本递增，不吞掉之后的新定位请求。
  无消息/会话标识的普通宿主调用不去重。不使用时间窗口去抖，不缓存取消指针。
- 工具说明明确起播已定位、检查用 status，并纠正现有音频算法说明为 SoundTouch。
  解码/GOP 精准定位路径没有为此修改。

## 修复后验证

同一实际注册工具用例转绿，连续三次请求同一目标均返回 deduplicated=true，真实源
播放时间不倒退；只读 status 不增加导航版本。同轮 force_seek、新用户回合、手动拖动
后的定位可执行，百分比与绝对秒数的相同目标可识别，错误 force_seek 类型被拒绝，
关闭后 status 返回 closed。结果
`/private/tmp/maichat-start-repeat-green-20261103.xml` 与后续
`/private/tmp/maichat-desktop-system-activation-20261103.xml`。
原生鼠标用例在非活跃测试 Space 的失败另作环境边界记录，不混作本次定位回归通过。

iOS 170 模拟器补跑从 6 秒起播后持续 6 秒的真实播放时钟采样，检查时间单调且继续前进，
通过（11.743 秒）。`/private/tmp/maichat-controls170-start-final-20261103.xcresult`。
XCTest 启动空闲等待期间视频已播放，因此首次 AX 读取不能要求仍恰好在 6–8 秒，
测试据此改为首次时间不早于起点并持续观察；没有修改生产起播逻辑来满足测试。
此用例覆盖 iOS 单次起播，不证明 iOS 的多工具重复调用已去重；本轮去重位于桌面工具。

2.2.33 已完整构建、打包、严格验签、启动检查，并按用户已有授权原位更新并重开。
实际安装 `/Applications/MaiChat.app`，PID 53063，主程序 SHA-256
`e1d9bca47dd2718b9eebea3c1b8b488b2f747122f7f7b90c906ce800e0ffd22b`。
回执 `/private/tmp/maichat-desktop-controls-installed-20261103.json`。
验证使用真实注册工具和本地公开合成素材，未为验收调用付费模型。
