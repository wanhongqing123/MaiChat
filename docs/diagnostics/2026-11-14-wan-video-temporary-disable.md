# 万相视频生成临时停用：2026-11-14

## 操作和预期

用户因费用要求暂时停用万相生成视频，保留实现代码。基线提交为
`03dc780140568255a93f1ffe6f3b12885a1d23f7`。普通 App 的模型工具清单不再提供
`wan_video`，提示词不再推荐它。直接工厂调用或旧工具请求的新建、修订应明确返回暂停，
不读取凭据、不上传素材、不发供应商请求，也不弹无效的付费批准请求。

暂停覆盖 delegate 的 create/edit/extend/reference 和 revise。原供应商实现保留；
已提交任务的 continue、cancel 和本地 delete 继续走原任务归属与状态校验，避免丢失已付费任务。
万相图片、百炼可灵和共享凭据保持原作用，暂停不是静默改用另一种收费模型的授权。

## 设计与实施步骤

只隐藏 UI 不能阻止旧请求；删除实现会丢失已提交任务的恢复路径。采用现有 Seedance 的
内部注册方式，并在万相共享 execute 入口的凭据读取前拒绝新增费用动作。
直接调用和模型调用复用同一个执行闸门，不增加平台私有策略或凭据旁路。

1. 为共享工厂补无网络失败用例：四种创建模式、同会话既有任务修订、旧指针直接调用；
   检查明确暂停结果、NotStarted/None 执行事实、零凭据/上传回调与任务记录不变。
2. 在普通 Desktop 捕获的真实模型请求中断言视频工具被隐藏，图片和可灵仍可见。
   先记录原实现的实际失败，再修改共享执行和两宿主注册、提示词。
3. 能力快照如实使用临时停用状态，discover 明确不接受新生成；继续、取消和本地删除
   维持原行为。无新生成动作不再触发付费批准。
4. 运行相同失败用例与相关 Core 回归，构建正常 Desktop/Mobile 消费者。
   验证只使用自有临时文件、内存存储与本地假模型；显式关闭现有 opt-in 供应商联网测试。
5. 记录实际构建、源码和验证边界后提交。已发布 260/261 的源码标签与资产不移动。

## 平台协作边界

Root 独占共享工具、两端注册与提示词及最终提交。Mac 只读核实 Swift 没有独立注册或执行旁路；
已有万相任务卡、取消/删除和文案分类保留。Mac 视频窗口缩放为另一项独立任务，不混入本次改动。

本文件先记录需求与实施边界；验证结果在实际运行后补记，不据此预报已停用现装 App。

## Windows 实际验证

先只改测试：标准 `MaiModelStudioToolsTests.cpp` 以原正常 App 的实际静态库链接运行，
新停用用例 exit1，原实现仍请求批准、读取凭据，缺少暂停结果与未开始执行事实。
普通 `AgentController` 配合自有 CapturingModel 的原槽
`desktopAgentSuppliesMarkdownSystemPrompt` 实际2pass/1fail，首错为公开 tools 中仍有 `wan_video`。
两项 RED 均未访问供应商，测试子进程移除了现有供应商 opt-in 环境，AppData 单独隔离。

执行层改为先拒绝 delegate/revise，返回 `temporarily_disabled`，可信执行事实为
NotStarted、None、started=false；直接持有旧 factory 实例也走此闸门。普通配置与上传状态
不伪造，能力快照追加 TemporarilyDisabled，discover 明确 generation_enabled=false。
两宿主改内部注册，提示词同步改为费用暂停，不静默切换收费模型。原供应商方法、Schema、
旧任务查询/取消/本地删除和任务归属校验仍保留。

最终格式化后，普通 Qt6.12/MSVC14.44 cache 的 maichat 与原 Desktop 测试目标编译链接0，
部署检查为3个二进制、114项PE导入及Qt插件全部通过。末轮编译、链接与两项回归 cohort
合计17.618s：ModelStudio标准测试源运行0，Desktop原槽3pass/0fail/0skip。
创建四模式与修订都验证零凭据/上传回调、原任务记录不变；已配置凭据的本地素材用例也未上传。

随后八套 Core 标准测试源全部编译、链接、运行0：MaiModelStudioToolsTests、MaiCoreTests、
MaiStoreTests、MaiToolTests、MaiTurnRunnerTests、MaiPermissionTests、MaiToolLoopTests、
MaiSpecialistNotificationTests。直接链接的当前正常 App `maiagent.lib` SHA256 为
`2de52c259fecf6a60593fc748278369e09eb94e286da0b85aafa941589799ae2`，
源目录为仓库当前 `MaiAgent/`，没有使用旧私有 source 副本冒充当前实现。

正常 Desktop CMake 强制 Core 测试开关为 OFF，所以测试源采用原回归 support 的正常库链接方式。
最初 CMake warning 被包装脚本当异常、尝试不存在的测试 target，以及 support response 缺换行
导致的链接失败均保留原日志，属于验证包装错误，不计功能 RED；修正后复用已经编译的对象，
未因此改生产代码或断言。证据在 `.cache/wan-video-disable-20261114/`，包括
`red-receipt-03.json`、`green-receipt-02.json`、`core-eight-suite-results.json` 和各原日志。

八源薄件共28882B，SHA256为 `43df5aebf98b4209db7521ab509a5d124a1f1dd1d9f2d85fc6208352dee9be51`，
已交给 Mac 在独立视频测试自然结束后按 previous/new/canonical 核合。
Apple/Mobile 普通消费者与后续实际安装结果需按真实回执补记；本节不宣称现装 App 已更新。

## Apple 普通消费者与 211 安装

Mac 在独立视频测试自然结束后核合八源，普通 Qt Core 库必要增量0/40.959s。
原 ModelStudio 测试使用该正常库和依赖编译、链接、离线运行全部0；子进程清除全部
live/recovery opt-in，sandbox 拒绝出网。Core 测试源29020B、SHA256
`8553eafe6289c7ee8e61ba9ba5e78a3a31a05988072fa936b2db57236fbe3076`。

Mobile 新增唯一 `--wan-video-paused-only` 槽，经过普通 CABI configure/create/send，
从自有127.0.0.1 SSE端点捕获真实HTTP请求的 tools 和 system message，确认没有wan_video，
仍有wan_image、kling_bailian_video/image，提示明确费用暂停且不静默换模型。
旧缓存实际RED1；normalMobile build0/71.082s后，同槽GREEN0/0.608s。
dummy key 仅用于自有环回请求，回复没有 tool_calls，不调用供应商。
新增测试源canonical126397B、SHA256
`70020c7b7c4196ab67de9a341830f6e30acba298ff5a4a592a393ccffac4461d`，
Root已按previous/new blob与bytes核合；Core和Mobile两组源哈希分别对应各自回执。

该轮普通消费者构建来源仍为beee29f4加精确八源overlay。GitHub fetch因TLS连接错误128而
未改HEAD，随后用10786B、SHA256 `e03d0e56eee0335abaf29dc5409e9cb76179b043f197bfbcfecda48e01c8d39a`
的两提交bundle，在cohort结束后安全ff到a701，不能将之前构建来源倒填为新HEAD。

211正常generic Release实际基于 `a7012733abf19ff00055d06bf8745397a188e70f` 生产输入，
视频Desktop overlay和Mobile测试源均不进入iOS Release目标。前置私有清单helper曾把测试源
当生产输入而assert，该次没有启动xcodebuild，原失败保留；按真实目标分组后正常Release
exit0/93.375s，strict签名、profile、10项升级entitlement、22个实际合库对象
（含Wan相关两cpp）及9个Release MachO的测试marker负检通过。
本地署名构建exe SHA256为
`e32fee203f07bb5d887dbc604e2724e4a2ba3342f084f62c42a222c6cab0416c`，不是设备容器读回SHA。

装前fresh实际210，保数据覆盖install0/17.945s，装后为 **2.2.4/211**。
首次普通launch实际exit1/10.745s，CoreDevice10002下层FBS7明确Locked，当前没有可确认的新PID。
这是设备锁定导致的启动未完成；安装不重做，等待用户解锁后仅补普通launch与进程路径回读。
未卸载、清数据或读取容器/真实DB，未调用付费模型或真实双账号消息流程。
