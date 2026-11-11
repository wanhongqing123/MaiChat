# 长上下文自动摘要返回格式错误

## 现场与边界

2026-11-11，用户提供 Windows 创作中心截图：多轮电脑/浏览器工具操作后，界面显示
`Context summarization returned invalid summary data.`。用户此前询问的固定历史字节裁剪
已在 `8c36b3d2` 移除，本次是自动摘要执行链的另一项故障。

本轮源码基线 `04890bb7f0162f43967602f668bf4c1b631a56ff`。本机只读进程查询没有得到
可绑定该截图的 MaiChat 进程，因此现场安装版本、模型及供应商原始响应尚未确认。
没有读取用户数据库、聊天或凭据，没有重放现场电脑操作或发起付费模型请求。

## 确定的失败分支

原 `MaiContextCompactor::validateSummary` 将全部收集文本直接解析为 JSON，非对象或
顶层字段数不等于六时返回截图的这条错误。六个必需字段为 goals、constraints、decisions、
progress、open_questions、source_notes，且各自必须为字符串数组。

原摘要收集器把所有 Responses 可见 item 的增量串接，不考虑它们各自的身份和 phase。
当前 Responses 客户端在 typed delta 与普通 onText 之间择一回调，因此不是双回调重复；
但独立的 commentary/partial item 与最终 JSON 被拼成一段时会失败。
Chat 正常结束、返回整体 JSON Markdown 围栏时也触发同一错误。

正常上报的输出截断已有专门错误，并非这条格式错误的等价原因。没有依据将截图直接归因于
供应商宕机、上下文容量、某个具体模型或上述两种格式中的某一种。

## 最小复现与修复

使用原正式 `MaiContextCompactorTests`、真实本机 loopback HTTP/SSE 和临时存储：

- Responses 的 commentary + final_answer；以及 item.done 才出现 phase 的
  partial/commentary + final_answer。
- Chat 的整体单 JSON 围栏，正常 stop。
- 三个应接受场景在原 Compactor 精确字节上共 6 条断言失败，错误与截图一致。

窄修只涉及 Compactor 和原测试：

1. 按 output item 身份独立收集，使用已完成元数据选择唯一 final_answer；不把明确的
   commentary/partial 当摘要，不拼接多个最终消息。无 phase 时只接受唯一可见 item。
2. 只允许整段单 JSON 围栏去壳，再严格解析六字段格式。不寻找任意花括号，不接受围栏外
   说明、多对象、多围栏、空摘要或错误字段类型。
3. 仅格式/schema 失配允许一次有界重试。保留同一 source_fragments、模型、effort 和
   maxOutputTokens；只追加可信的格式说明，不把坏响应回灌成新指令。增加的指令按原倍率
   做输入预算预检，放不下则不发重试。
4. 工具调用、多个最终 item、收集歧义、超量、网络、取消和正常截断均不重试。
   检查点仍在所有块成功、最终预算与取消检查后原子提交；失败不覆盖旧检查点，不删原史。

## 已执行验证

- 原实现、正常构建、新真实 wire 用例：6 条断言失败；原 exe 和 raw log 保全。
- 最终同两源完整 `MaiContextCompactorTests`：PID 22584，exit 0，2806 ms，stderr 空。
- 13 个真实两 wire 用例及原组保留：第一份坏格式后成功恰两次摘要请求；连续坏格式最多
  两次；预算不足不发重试；网络/取消/工具/超量各一次；旧检查点与原史保持。
- Compactor raw SHA256 `df8b38b5d8e2170a604db6b2b8e629de0d29bf37cd0e92e7c9d967a0a34459a8`。
- 原测试 raw SHA256 `1430f7d58ea8b29dca45a9abe04913809c25eaa37df5a556bd8d3ea91a6b65fe`。
- 测试 exe SHA256 `34007366284f35d8fde4a7f0ab047036d652027b9a7a470f9ccd72473e0ef1b3`。

原件位于 `.cache/context-summary-failure-20261111/readonly-probe/`，最终回执为
`final-run-receipt.json` / `final-verified-stdout.txt`。两源交接 patch 已逐 blob、canonical/raw
哈希及临时 index apply/check 核准；没有改模型容量、公开头、wire 或 CMake。

## 仍需明确的验收范围

目前证明修复了两类可复现缺口，不能认定已取得截图那一次响应的具体原因。没有验证真实
供应商的摘要语义质量或计费耗时，没有替换现场 App。

2026-11-11 的正常消费者补验：

- Windows 正常 Core 13 组全部通过，189.39 秒；其中完整 Compactor 2.91 秒。
  原 ctest/LastTest 日志逐字节保全，回执为
  `.cache/remote-tool-detail-20261111/root/final-core-proof.json`。
- Mac 正常 Core cache 定点构建 exit 0，完整 `MaiContextCompactorTests` exit 0，2.801 秒。
  对端原 JSON 和 raw log 已收到并核读；普通 Simulator Native v2 也已构建成功。
- Windows 正常 `maichat` 已编译/链接、完成 windeployqt 和 PE 依赖检查；私有数据目录及
  Windows 裸 PATH 的 `--smoke` exit 0，PID 48592，实际读回版本 2.2.56 / Qt 6.12.0。
  当次 exe SHA256 为 `2585ece5b80b9389604257aad745866214240a8a7574af5478e2a5165646ae8b`。
  该产物同时消费当时的远端详情修复 overlay；它不是独立摘要补丁的单提交产物或发布包。

以上结果证明当前源码可被正常消费者使用，不能替代截图那一次供应商响应、现场安装版本
和真实长会话的验收。后续跨端详情改动不改变这两个已冻结摘要源的字节。

独立 review 未发现这两个文件的提交阻断，但指出既有 Responses 客户端若直到
response.completed 终帧才更新已完成 item 的 phase，可能不再次通知消费者；当前 late-phase
用例覆盖 item.done，不代表这一终帧传播边界已修复。
