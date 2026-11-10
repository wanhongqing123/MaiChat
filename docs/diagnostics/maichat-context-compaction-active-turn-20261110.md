# 活动轮次压缩与原字节分页修正 v7（2026-11-10）

## 基线与复现

本记录补充 v6，不覆盖其源码包、诊断、日志或消费者验收记录。v6 源码 cohort 为 `0dccb88ad2c5606c72519308c89b6b0c25899ee8490bcbc823e118d211acbc52`。v7 工作区基线提交为 `7dffeb5bf8b45a1ebec6a780106b86dc619dabde`，未独立提交或推送。

独立复审发现两个可重复问题：

1. 一个当前 user，加一条 `completed=0` assistant；助手含 20 个已经 Completed 的大工具结果。旧边界要求完整轮次，因而摘要调用为 0，超过窗口后直接报错。
2. 旧工具原字节为 `A + 8 个 0x80 + Z`，以 4 字节预算读取。旧边界只看续字节位形，回退到偏移 0 后返回空文本、无后续页，误报 EOF；首位就是 `0x80` 时连偏移 0 都会被拒绝。

预期：活动轮次可以总结稳定工具前缀，当前 user、最近完整工具与观察以及 Running/Pending tail 保持原文；摘要仍需持久源身份和 CAS。原史分页必须真实消费源字节，合法 UTF-8 标量内部边界仍拒绝，孤立坏字节不应被吞掉。

使用 v6 正常 Core 加入新断言后实际产生 13 FAIL，见 `/private/tmp/maichat-context-compaction-v7-20261110/red-v6-behavior.log`。这不是构建失败或假造模型错误。

## 活动轮次检查点

检查点新增 `partialMessageId`、`partialPartCount`、`throughPartId`、`partialSourceFingerprint`、`preservedUserMessageId`。SQLite v9 在原表追加五列，旧 v8 完整消息检查点仍可迁移和复用。

`sourceMessageCount` 仍定位完整持久前缀；部分 assistant 必须紧随该前缀。其对应的当前 user 作为显式 anchor 原文保留。部分源指纹覆盖稳定 parts 的原始字段、会话身份及完整前缀指纹，不含 assistant 的完成时间、parts 总数和未覆盖 tail。匹配器也只校验已覆盖内容；工具/图片分组在创建边界时判断，不能因之后追加到保留工具后的观察而推翻原源身份。因此追加工具、收尾和重启不会使已有效摘要无故失效，而已封存原字节发生变化仍会被 CAS 拒绝。

单轮工具超限时，压缩稳定旧工具前缀，保留最近完整工具结果及其调用。遇到 Running/Pending 即停止。现有实现把一批工具的图片统一追加在整批结果后，不能可靠反推各图属于哪一个调用；有紧随图片时把该批与观察整体保留，不能在回执和像素之间切开。Builder 的图片数量和跳过策略只计剩余 tail，避免旧前缀的图片数量误吞新图。

增量取源使用覆盖集合差：先前 partial 覆盖的 parts 不重传；先前保留的 user 在成为旧轮次时才首次进入摘要。当前 user 始终作为原文保留，不因完整前缀物理包含它而消失。所有原始消息和片段仍在 DB。

## SQLite 与内存字节的组合问题

补充实跑一个 SQLite-backed TurnRunner：1 个 user，假模型一次发出 20 个工具调用，各工具结果含 `0xFF`，再次请求前触发压缩。初次 v7 实现产生 4 FAIL，见 `red-sqlite-memory-snapshot.log`：摘要确实调用，但检查点不推进，主请求无法继续。

证据是 SQLite 既有写入策略会替换非法 UTF-8，而 `buildRequest` 又用助手内存原字节覆盖持久快照。若把这份内存副本用作摘要源身份，CAS 会正确发现它与 DB 不符。

修正仅分开两个视图：摘要及源指纹使用实际已持久快照；模型 tail 使用最新内存内容。持久边界按已覆盖片段 ID 集合对齐后再分区，不假定两个向量的到达位置相同；未覆盖的内存和持久 tail 都保留。没有归一化所有指纹，也没有改变 SQLite 原写入政策。原组合随后通过：检查点推进、主模型继续完成，已存工具字节前后相同。Memory 中封存的 `0xFF` 改成 `0xFE` 仍被识别为源变化。

## 原字节分页与 wire 表示

边界检查先核对完整合法 UTF-8 标量，拒绝过长编码、代理项、越界和截断序列；仅在合法标量内部禁止切分。非法旧字节按单字节单位处理。参考 Chromium 的 [ReadUnicodeCharacter](https://raw.githubusercontent.com/chromium/chromium/main/base/strings/utf_string_conversion_utils.cc) 校验解码值的契约。

普通 API 的 `text` 返回原字节。结果新增 `sourceBytesRead` 和 `textIsValidUtf8`。模型工具明确返回 `representation` 与 `source_bytes_read`；非法字节的 `text` 是 UTF-8 replacement 预览，同时提供精确 `raw_base64`。偏移、总长度和后续游标始终计源字节，不能把替换预览长度当成已读源长度。

两种孤立续字节原例均可按 4 字节预算逐页还原全部原字节。混合孤立字节、中文和 emoji 的用例也验证：坏字节推进，合法标量内部偏移仍返回 InvalidInput，无空页误报 EOF。

## 源码冻结与验证

冻结的相对 v6 增量为 15 文件；CMake 和宿主源无 v7 修改。最终 v7b 完整 31 源 cohort SHA-256 为 `5d99bc387f072a60f00868472274ef26c9abf66ec59bb92c1a93b1ce0cd6bb82`，仍以 v6 cohort 作为 previousCanonical。

中间 cohort `918462c0cb3eee4162b1de9e850ed9e0ec30b71c4cda1ac9efa971909a314052` 的正式 Core 13/13 已通过（137.88 秒），console e2e 全部通过，协调端还完成了该 cohort 的 Mac App/iOS 消费者验证；这些记录保全在 `cohort-918/` 和协调端记录中，不能表述为最终 v7b 结果。

随后补测未覆盖 tail 追加图片：其 sealed digest 未变，但旧 matcher 因向未覆盖 tail 前瞻而错误失效，真实 RED 见 `red-tail-image-identity.log`。最小两文件修正只移除该前瞻，保留创建时的媒体组保护；同一 probe 对象换链后 `green-tail-image-identity.log` 显示 digest 未变、checkpoint 匹配、复用成功且不再调用摘要。两文件增量为 `tail-fix-from-918.patch`（SHA-256 `196c2250b9c478f53bb5ad9297b15de166be61d45ad22076339572ef42762752`）。

供件目录 `/private/tmp/maichat-context-compaction-v7-20261110/` 包含：

- `source-freeze-v7b.json`：最终 previousCanonical、各文件前后 SHA、当前基线与完整 cohort；`source-freeze-v7.json` 保留中间 918 版本。
- `previous/`：保全的 v6 源文件；`full-files-v7b/` 与 `v7b-source-delta.tar.gz`：最终 15 个变更后的完整文件。
- `source-delta-v7b.patch`：相对保全 v6 的最小增量；它和从 918 起的两文件补丁均在对应快照上通过 `git apply --check`。
- `green-compactor-3.log`、`green-store-3.log`、`green-sqlite-memory-snapshot.log`：新用例已通过，含 partial 追加/收尾/重启/新轮提升、Running/Pending、图片观察、失败/取消/CAS、v8 迁移、非法字节精确分页。
- `native-build-v7b.log` / `final-targeted-v7b.log`：最终两文件修正后的正式构建及 Compactor、Store、TurnRunner、Builder 四组定点回归。完整 13 组和 console 记录属于上述中间 918 cohort，末次只重跑受影响四组；不沿用 v6 的 PASS，也不把中间 cohort 的消费者结果冒称最终 v7b 验收。
- 最终 gate 回执列明每个 cohort 的退出码、产物 SHA 和冻结源码一致性。

所有模型为确定假模型或本机 loopback；数据库均为临时测试库。没有读取用户 DB、调用真实模型、改手机或替换现装 App。正常 Mac App、iOS 和 Windows 消费者由协调端在冻结源上独立重编与验收，本记录不预先宣称其结果。
