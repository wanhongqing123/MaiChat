# Remote Session 临时图片集成记录

## 复现与固定语义

2026-11-10，源码基线 `28977e7a42e5bbb680505e5aa7c8aef9ebe0b240`，main 工作区。
用户在 iPhone 创作中心查看 Desktop 所属 Session，助手正文之间的 screenshot 只能显示
“图片在所属设备，暂不可跨设备预览”。本轮实现 Desktop owner 首次发布，iOS/Desktop viewer 读取。

新 live image 在原位置先发布 pending，经服务 ACK 后上传一次临时 OSS，随后发布同一 part 的
ready 引用。user/assistant 共用协议。WS 仅传状态和引用，媒体字节由客户端直连 OSS。
服务首次预留起固定 24 小时，不因查看、登录更新、签名刷新或上传完成而续期。
原端离线不影响有效引用；到期不重新上传。完整本机缓存继续可看，未缓存显示“图片已过期”。
旧历史没有引用的不自动补传，iOS owner 的首次上传不在本轮范围。

签名 URL 最多 60 秒且不越过原对象期限；控制响应延迟不能把仍有效的图片误判为无效。
业务到期拒读、短 URL 到期和物理删除是三个独立事实。清理复用原 exact-key
abort/delete/HEAD 与完成竞态保护，专用 `session-temp-images/` 生命周期是异步兜底。
生命周期工具先核从未启用版本控制、备份并合并原规则，写入前再次检查，再读回全部规则。
线上规则尚未应用，不能声称图片已在真实桶按期限删除。

## 已完成的源与验证

| 范围 | 最终证据 | 边界 |
| --- | --- | --- |
| 账号/Sync 控制面 | 相关 74/74，通过真实本机 Auth↔Sync HTTP | 假 OSS，无线上账号 |
| signer/生命周期 | Root 最终 27/27；版本控制保护先 7PASS/2FAIL，再同例 9/9 | pinned SDK 请求被测试替身截获，无云操作 |
| RAM 工具 | 新 5/5、原 2/2 | 假 RAM，不创建密钥或改线上策略 |
| Windows App/Host | 正常构建 exit0；3 binaries/114 PE imports/plugins；完整 Host 79PASS/0FAIL/1SKIP，21404ms | 唯一 skip 是需临时 TLS 环境的显式用例，另行实跑 |
| Windows 实际 Qt/TLS | 3PASS/0FAIL/0SKIP，5279ms；原 owner 离线/重登录、固定过期后 warm cache、另一账号 cold cache、恰好一次 init/read | 自有 localhost TLS 和 PNG；缩略图 2:1、点击原非模态预览 80×40 均验证 |
| Mac App/Host | 同 12 文件 canonical 正常构建 exit0；Host 79PASS/0FAIL/1SKIP，20381ms；TLS 3/0/0，5065ms | 独立 HOME、loopback-only，无 OS 采集/输入 |
| Mac 普通 Network v2 | persistent 35.80s、temporary 0.80s、近到期延迟 1.96s，均通过 | 与 Root 同 C++ canonical `5c832405b419ae0e07e0b751cf92fbfa28ef30ae` |
| iOS 最终 Swift | 原 UI 1FAIL → 最终 7 tests/0 failures，83.962s；20 项缓存/归属/取消检查 | 仅最终下载边界替换为 Simulator 自有 PNG，未调用线上 OSS |
| iOS 最新 C++ 重链 | Debug build-for-testing exit0；六 Swift SHA 未变 | UI 结果复用相同 Swift，未重复或扩大为线上验证 |
| iOS Release | 正常 unsigned Simulator arm64 Release build/link exit0，120.197s | 未安装；四个专属测试下载标记在实际 Release exe 中不存在 |

iOS Release exe SHA256 为
`7cad468caa9df150e92a1af09142faf468fbaef98d18145ee62d09d159579828`。
构建通过命令行 `CURRENT_PROJECT_VERSION=203`，未编辑工程版本。真实手机和桌面现装未改。

## 定点问题与同例修正

- 原远端 renderer 把 text/image/text 合为一行占位；同例先失败，再恢复三行原顺序。
- 图片完成后原 pending 行高度未更新，80×40 被画成 105×33；增加实际蓝色像素比例断言
  复现，再通知 MarkdownView 原布局过滤器重算，保持原 widget 与正确比例。
- Core 的工具完成事件早于结果图片追加，按工具 part ID enroll 会漏掉助手截图。
  真实 Agent 加脚本模型/图片工具在下一响应仍 busy 时先失败；仅对已观察到的 live message ID
  做延迟 point-read 后，同例通过。没有扫描 catalog/backfill 历史来规避失败。
- 近到期相对签名 TTL 原来拿客户端收到响应后的剩余时间比较，1.2 秒延迟可误拒有效图片。
  同例 RED→GREEN 后按请求开始时间校验相对 TTL，同时收到响应仍须核绝对期限未过期。
- Desktop 图片点击起初不能打开大图；真实 Qt/TLS 点击同例先失败，再复用既有
  ImagePreviewDialog，非模态显示已缓存原图，不重发上传/下载；账号和行替换取消旧预览。

## 归档与限制

Root 已核 iOS 源 ZIP 219544B、7/7 raw/base/canonical，先保全原文件后合入工作区；
后续 Release 的六 Swift SHA 与该供件完全一致。完整 Mac proof 共 37 项逐项长度/SHA256 匹配，
9 张最终自有截图逐张检查。原预览录屏明确属于首轮历史 GREEN，没有冒充最终重链或真机结果。
proof ZIP 12597440B，SHA256
`dd4069e820661e421a2838e754a8d8d9cc42d844b3f079940195f90dfcb0fc73`。
本地索引：`.cache/remote-session-temp-images-20261110/mac-final-proof/root-review.json`。

全服务宽范围测试另有两个未修改的 Windows 环境清理错误：Auth 文件夹测试的 SQLite
关闭顺序导致 EPERM、signer 旧目录 fsync 的 EPERM。相关最终范围测试均通过，不能把宽范围
结果写为全套全绿。详见服务端独立记录。

本轮未部署 Auth/Sync/signer、RAM 或桶生命周期，未发真实模型请求、未访问真实用户数据库，
未更新现装、手机或既有候选包，也未发布 Release。真实 OSS 私有访问、过期拒读与最终删除、
实际账号跨设备及手机业务验收仍需后续执行。
