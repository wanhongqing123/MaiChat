# PC 真实 token 计量与冷启动投影

记录时间：2026-11-26，Windows 原生 MSVC x64。
工作区基线：`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，包含本次及原有未提交修改。
未更改用户数据库、安装目录、运行中的应用、远程服务或 `.cache/r311` 的源码和构建。

## 可复现操作与预期

在独占临时账号根中创建会话，回环 Responses 服务返回 input=15000、cached=14000、output=1000、total=16000，配置窗口为 20000。

- 状态和冷启动普通查询均保留最近主采样的同一来源和计数。
- Codex 显示基线为 12000，剩余比例为 50%；缓存仍占上下文窗口。
- 非缓存输入为 1000，blended 为 2000；二者是算术推导，不另加缓存写入或推理明细。
- 摘要请求只增加 turn/thread 累计，不替换主采样或其 model/provider/window/response 身份。
- 显式 output=0 的下一次主采样仍更新 input/total，并在重开会话后保持 Provider 零值。
- 型号、来源、检查点或其存在性变化后，旧采样作废；未知窗口和缺失总数不生成占用百分比。
- 删除本夹具独占的派生索引后重开，仍从原始 JSONL 恢复相同计量；清空会话消息后不复用旧占用。

## 根因证据与实现

原 `MaiContextStatus` 只有 before/after 本地估计和输出累计，不能表达最近主采样的真实 total、缓存明细及缺失来源。Runner 还曾因 output=0 提前返回，使输入/总量更新无法进入状态。

新增 `MaiModelContextUsage` 普通值类型和普通查询；`MaiContextStatus::usage`、JSONL `token_usage_record.context_usage` 及轮次元数据共用同一编码。保留 Provider/Derived/Missing，未将供应商原始 payload 复制进状态。

`MaiAgent::getModelContextUsage` 按会话当前型号及配置来源核对冷启动事实。来源指纹只使用 scheme/host/port 的 SHA-256，不保存 URL、路径、查询或凭据。历史采样保留自己的身份；摘要和新轮次不会给旧测量重新贴来源标签。

显示算式核对 `E:/OpenSource/codex` 固定提交 `3d86e43b075adeba2193ec5c22ac92f55afd82e6` 的 `TokenUsage`：窗口使用 latest total；`non_cached_input=max(input-cached,0)`；`blended_total=non_cached_input+output`；比例基线仅供显示，不改变压缩预算。缺失缓存只在算术中按零降级，原字段仍为 Missing，结果为 Derived。

## 原生验证及边界

算术测试先出现 11 个行为失败，再转绿；编码测试先出现 5 个字段/校验失败，再转绿；缺失缓存算术降级测试先出现 1 个行为失败，再转绿。

完整编译 114 个当前核心翻译单元；使用已存在的第三方链接闭包，未链接旧 MaiAgent 对象。344 个源码/头文件/测试先复制到独占目录并核对复制前后 SHA-256；Runner 定稿后仅更新其实现和测试快照，头文件布局未再变化。

原生通过：`MaiModelContextUsageTests`、`MaiSessionRecordCodecTests`、`MaiRolloutRuntimeFactsTests`、`MaiJournalRuntimeTests`、`MaiContextCompactorTests`、`MaiSkillTests`、`MaiSkillRuntimeTests`。此批 Compactor 覆盖 12 种增长场景；随后 14 场景的补充测试由压缩排障任务单独记录。Journal 新用例首轮因测试响应没有 final answer 触发原有重试，已补上真实完成消息后用同一用例回归通过。

独占构建：`.cache/pc-usage-20261126/full-native/cohort-01/`。核心库 SHA-256：`800b616c074cbfa4bba6dee72a9645c020952c9896d8ad9a1aabbf3edf293bc1`。完整源码清单、链接输入及七项测试产物哈希见该目录的 `final-source-manifest.json`、`link-inputs.json`、`verification.json`。

这些结论仅覆盖独占原生核心与回环协议/存储测试；未将该库替换到用户安装或运行进程，不作为实际桌面包、UI 或实机验收结论。桌面正常构建与安装运行验证由主任务继续执行。
