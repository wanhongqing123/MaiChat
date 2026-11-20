# 持久轮次结果：首批 Codex 架构借鉴

用户已要求补齐能力并同意借鉴优秀架构。本批采用架构对照建议中的增量方案：保留 MaiAgent/Runner/Store，先明确轮次结果边界；不重写异步调度，不扩付费调用策略。

## 设计与边界

- 一轮以初始 assistant ID 为稳定身份；轮内 steering 产生的新 assistant 仅为分段，不产生新轮次。
- 原始 assistant 持有可选 MaiTurnOutcome：Running/Completed/Failed/Interrupted、结果消息 ID、完成时间、类型化错误。旧记录无此事实，必须报告未知，不能从有正文或 completed 时间猜成功。
- 普通 getTurnOutcome 接受会话和可选轮次 ID，读取持久事实；空 ID 找最近被核心记录的轮次，不能越过较新未知 assistant 回到旧结果。
- 元数据存于已有消息而非模型正文。MemoryStore 保值；SQLite v14 增加可空列，兼容 v13，无旧数据回填猜测。重启把遗留 Running 记为 Interrupted，不自动发请求或重做工具。
- 所有 Runner 出口在工具资源清理、正文落库之后保存终态，再发布独立结果事件。落库失败不得发布成功结果。
- session_status 工具只查询当前绑定会话；wait_agent 先校验子会话归属并使用相同结果查询，明确失败/取消/不存在；普通查询不受工具协议约束。
- 不继承审批豁免，不改模型/effort/输出 cap，不复制思考或原始供应商响应，不操作真实数据库或现装进程。
- 新消息布局要求所有受影响 C++ 消费者同源重编；本机 Windows 验证，其他平台不借用旧对象宣称通过。

## 执行步骤

1. 先追加 stale-result、失败/取消/空正文结果、缺失子会话、Memory/SQLite 轮次事实与重启用例。冻结数据合同和查询声明，运行旧实现并保留实际 RED。
2. 在 MaiMessage、MaiAgent 追加结果数据及只读 API；现有初始受理、Runner 生命周期、内存/SQLite 存储复用该数据。更新注释和 SQLite 迁移。
3. 接入普通查询与当前会话模型工具，修正 wait_agent 的结果与归属语义，旧无状态数据不得冒称成功。
4. 同一用例 GREEN，运行普通 Core 回归与存储 e2e；核对源码、构建及测试实际日志，不触发供应商。
5. docs/diagnostics 记录 RED/GREEN、平台和未验证边界；只提交本批文件，保留 qtbase 与既有未提交文档。

## 后继批次

上下文占用/余量状态、实际用量账本及共享预算另沿这一稳定轮次身份接入。本批不会把没有这些功能描述为已实现。后台推测压缩、动态换模和历史分叉仍按需求独立设计。
