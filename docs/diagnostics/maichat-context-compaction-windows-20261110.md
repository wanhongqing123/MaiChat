# Windows 上下文压缩独立验收记录

## 需求与操作

日期：2026-11-10。平台：Windows/MSVC x64。源码基线为
`a8e1f5e42617c8f4c9f52e9da5900f48ec9f978b`，待审 Core 供件独立叠加；
线上服务仍是 `8cde6a87418c84691598b567f49a68255880bd10`，没有夹入本轮 Core。

用户要求长会话能继续执行，原始消息与工具输出保存在数据库中并可精确回查。
不能用固定字节裁剪冒充语义压缩，也不能仅把超限报错当完整方案。
验收包括已完成历史、正在执行的长轮次、连续工具和截图、重启后的增量检查点、
失败保全、精确原史读取与非法 UTF-8 原字节分页。

## 已完成的历史基线

v6 的 31 源聚合 SHA256 为
`0dccb88ad2c5606c72519308c89b6b0c25899ee8490bcbc823e118d211acbc52`。
逐项核对供件的原始 SHA、Git canonical blob 和当前文件，备份后叠加。
Windows 13 个 Core/console 目标构建 exit 0，registered 12/12 测试通过，249.11 秒。
正常 Qt App 和三个电脑测试目标构建 exit 0，部署检查为 3 binaries/114 PE imports/plugins。
这些结果只属于 v6，不倒填为后续源的验收。

## Console 首次崩溃的证据与修复

原 console 在 ASCII、UTF-8 两例都于 HTTP 请求前发生 `0xC0000005`，不是模型超时或
字符编码错误。保留原 exe、obj、CDB 栈和 COFF 映射后，定位到
`ChatCompletionsClient::contextLimits+0x20`：新库读取新 `MaiModelConfig` 中的回调字段，
CLI 对象仍来自旧 ABI；该对象的 Ninja 头文件依赖数为 0。

确认备份 SHA 后，只移除该构建目录内的 CLI 对象并重编 console，没有修改生产源、
SDK、编译选项或全局缓存。同一 ASCII/UTF-8 probe 均恢复 exit 0、各产生 1 个请求；
原 console e2e 全部通过，6.63 秒。新对象依赖数为 23，包含 `MaiModelClient.h`。

| 产物 | SHA256 |
| --- | --- |
| 原崩溃 console | `309f1b73caecd2a86505bea18c70f7dbca7f23400632a924eb4d913df87f1958` |
| 原 CLI 对象 | `b3e65a24e51372634f1ba2454d0fabd0159bd03be531d3311ca8f88b5703026c` |
| 重编 console | `9fbc0160f89a977ef7b07fc9edf5de6e7986a12263c6cc20b8c6f95723b5ccac` |

现用正常 Qt cache 的项目源依赖也已定点检查：零依赖项均为无 include 的空 autogen/qrc
源，没有据此清理第三方 SDK 或重建无关依赖。

## 独立审查发现与供件边界

1. v6 仅在完整轮次之间创建边界，单个活动轮次内大量已完成工具无法压缩。
2. 非法 UTF-8 历史片段可能被错误当作 EOF，必须保持原字节进展并明确 wire 表示。
3. v7 检查点匹配前瞻未覆盖的尾部图片，追加截图后错误拒绝稳定前缀；v7b 去除此项。
4. v7b 首次出现无正文分隔的大批连续工具和截图时，创建边界仍回退到全部工具之前。

第 4 项的精确操作：无旧检查点，1 个 user 与 ongoing assistant，19 个各 12000 ASCII
字节的 Completed 工具结果，第 20 个为截图工具，随后一个 Image；中间不插入 Text。
Fake 模型窗口 32768、输出预算 4096。Mac 独立原例实际运行显示
`prepare_success=0 / summary_calls=0 / checkpoint_saved=0 / original_source_unchanged=1`，
exit 1；这是行为失败而非编译失败。

Mac 独占创建边界和原图重纳入的窄修正，将完整稳定工具调用与结果成组处理，
原图仍经 `request.messages[].images` 携带精确来源；Running/Pending 不纳入摘要。
已补首批检查点持久化并 reopen 后，再追加同等大批工具及新图的实际执行回归。
v7b 聚合 SHA 为
`5d99bc387f072a60f00868472274ef26c9abf66ec59bb92c1a93b1ce0cd6bb82`，
仅作保留的历史基线。

## 最终 v7d Core 与消费者

相对 v7b 的 5 文件修正为 v7c。审查又发现空历史加超长基础指令会进入新增
`history.front()` 路径；Mac 用同一个探针对象对照得到 v7c SIGSEGV -11、v7b 正常
InvalidInput。v7d 加入空源守卫，保留明确容量错误、零摘要、无检查点，并覆盖超长工具声明。
另将两套 HTTP wire 的旧 `Image returned` 文案断言适配为真实工具组来源 ID；原图
data URL 解码字节与 call/result 配对断言均未删减。

最终 31 源聚合 SHA256 为
`da292832a59f11057f9dc63323907848df810c07e460ff1f826c40c3e537a6b6`。
Root 逐项核对 31 raw SHA 与 Git blobs，v7c 五文件及 v7d 三文件各自先验 previous/new、
apply-check 0，保留前版原字节后叠加。最终窄审未发现剩余确定阻断。

- Windows Core/console 13 目标构建 exit 0；registered 12/12 PASS，92.85 秒。
- 正常 Qt App 与三个电脑测试目标构建 exit 0，windeployqt 与 3 binaries/114 PE imports/plugins 通过。
- 普通电脑 contracts exit 0；原/新 Agent UI 两例共 4PASS/0FAIL/0SKIP，693ms，无 OS 输入。
  初次 offscreen 因缺字体出现方框，保留该图；指定仓库 `Presentation/fonts` 后用同一 exe
  重跑原断言并亲看中文输入/确认及对话记录，未改产品源。证据目录为
  `windows-agent-v7d-owned-fonts/`，日志 `windows-desktop-agent-v7d-fonts.txt`。
- Mac 最终必要四组 4/4 PASS，72.96 秒；两个原探针对象换链均 GREEN。正常 App 与
  unsigned Simulator Release 构建 exit 0，私有环境普通 smoke exit 0。

Mac 最终 proof ZIP 为 435393B，SHA256
`49f56eb7dabb137ee5639a19bc43e74e3c3363104db69d19849a0f180909495c`；
Root 已核 48/48 manifest 长度与 SHA、49 ZIP 项 CRC，原件在 `mac-proof-v7d/`。
本轮三份 Mac 诊断按原字节归档；其中 v6 与 active-turn 文档保留历史 cohort，
最终工具/图片组修正见 `maichat-context-compaction-tool-image-groups-20261110.md`。

以上消费者仍为出包前 2.2.54 构建，不能改称随后 2.2.55 包。新候选须从统一冻结版本重新构建。
console e2e 的后续同步与 HTTP 测试夹具记录见
`2026-11-10-console-e2e-fixture-boundaries.md`。Root 最终独立复跑正式脚本 exit 0，
`windows-console-e2e-v7d-final-root.txt` 保留完整输出；中间失败日志不改称通过。

## 原始证据与限制

原日志、源快照、坏产物和逐项清单保留于
`.cache/context-compaction-20261110/`；v6 主回执为 `windows-v6-baseline-receipt.json`。
各 cohort 的源、测试、实际产物分别记录，不改标、不混计。
测试使用 Fake 模型或本地 HTTP、自有临时数据库；没有读取用户数据库或发起付费模型请求。
真实模型的摘要质量和耗时、现装客户端的行为不由这些测试冒称已验证。
