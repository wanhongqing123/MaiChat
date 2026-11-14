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
