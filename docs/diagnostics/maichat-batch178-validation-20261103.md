# 连续发送、本地模型入口、手动相机及联网检索交付

时间：2026-11-03，Asia/Shanghai。源码基线 `2c823e74a03e17acea5f00baa8ad7dadd77d67c3`，
最终源码为包含本记录的 main 提交。生产源码在最终构建后冻结；后续两项复测仅改 Debug
fixture 与 XCTest 输入刺激，没有改变产品代码。所有旧失败回执保留。

## 操作与预期

- 创作中心执行任务时持续输入和发送：本地持久入队后清除此条草稿，消息显示“排队中”；
  宿主允许投递时按同一会话 FIFO 发送。输入栏不再显示停止，有文字时只显示发送箭头。
- 正常“＋→拍摄”显示手动快门，等待两秒不自动拍摄；显式工具请求才进入定时模式。
  手动照片只回到最初会话草稿。功能展示卡片继续只是说明，不成为工具执行入口。
- “创作中心→•••→本地模型”默认选择 Debug 内置英文测试模型，运行后出现实际英文结果、
  token 数及驻留状态，可卸载并返回创作中心；用户也可以从文件导入 GGUF / 匹配 mmproj。
- Agent 可调用共享进程内 `llama` 工具。当前本地模型**未接成 Agent 主模型客户端**；
  主模型仍由 `MaiModelClient` 的 Responses/Chat Completions 实现直连供应商。
  将来接主模型应实现同一中立模型接口和完整工具调用语义，不需要把进程内推理绕成 HTTP。
- 联网检索通过 `web_search` / `web_fetch` 的 DeepSeek 结构化 API；curl 保留下载、API、
  自建服务器和连通性诊断用途，其说明引导搜索使用专门工具。

## iOS 模拟器：实际普通 App、截图和录屏

正常 App 为 `2.2.4 (178)`，自有 iPhone 17 Pro Max / iOS 26.5 模拟器
`19D02616-576C-43A6-8D92-2AA160F6F48E`；App 11 与 Runner 2 个 Mach-O 逐项验签。
不登录真实账号，不触发付费模型；合成 ACK、相机只在明确 opt-in 的 Debug 模拟器 fixture 使用。

首次最终同包：`/private/tmp/maichat-batch178-final-ui-20261103.xcresult`、同名 `.log/.mp4`，
实际 **6 pass / 2 fail**。已过原 turn 补充、账号/会话隔离、未知确认重试、真实 C++ archive
引用生命周期、本地模型实际生成与卸载、手动/定时相机模式及回草稿。

两项失败均保留原最终断言后定点复测：

1. 清空中文草稿的批量 delete 刺激未删完，改为逐字删除并检查每步实际文本，最终仍必须空稿、
   无 stop、原任务 busy。
2. 连发测试原固定 3 秒 ACK 等待早于 XCTest 第二条检查，改显式 ACK gate。五条均须在
   1.5 秒内本地入队清稿；确认 accepted=0、attempts=1、entries=5 后才放行，随后检查五条
   独立 ID、重复文字不合并、原 turn 与 FIFO。

复测：`/private/tmp/maichat-batch178-two-recheck-20261103.xcresult`、同名 `.log/.mp4`，
**2/2 pass，45.148 秒**。两次运行合计覆盖全部八项，无失败用例遗留；没有把首次六项
结果改写为首次八项全绿。复测 staging 为
`/private/tmp/maichat-image-spinner-two-recheck178-20261103`。

已实际查看复测截图与 4 秒间隔录屏抽帧：忙碌空稿无停止，输入有且仅有发送箭头；
五条未确认时显示排队消息，放行后成为正常历史，没有持续转圈或新增黑色停止。
模型与 camera 录屏另逐段查看：真实英文 11 输入 / 32 输出 token，卸载到未加载；
手动→取消重开→显式定时 0 到 1 张→停止封存→手动→原草稿一张附件。
本地小模型生成很快，不以此宣称大型模型长加载/取消体验已验收。

## iPhone：实际安装和运行产物

真机构建 `BUILD SUCCEEDED`：`/private/tmp/maichat-batch178-ios-device-build-20261103.log`。
冻结 App：`/private/tmp/maichat-batch178-device-artifacts-20261103/MaiChat.app`，
同目录 `build-receipt.json` 保存主程序/全部 11 个 Mach-O SHA 和签名检查。

真实 iPhone 14 Pro Max `00008120-001E0DEC0263C01E`：更新安装成功，随后正常无测试参数
启动成功，PID **25361**。再次查询安装信息明确为 **2.2.4 / 178**，并独立查询到该 PID
仍属于此次安装的 bundle。回执：

- `/private/tmp/maichat-batch178-device-install-20261103.json`
- `/private/tmp/maichat-batch178-device-launch-20261103.json`
- `/private/tmp/maichat-batch178-installed-20261103.json`
- `/private/tmp/maichat-batch178-device-process-20261103.json`

没有卸载、清空账号数据或停止用户桌面 App。Debug App 确实包含 1,185,376 字节测试 GGUF，
SHA-256 `270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d`；
只用于英文引擎测试，不是聊天/OCR质量模型，Release 不默认包含此上游 CI 权重。
真机安装/启动不等于真实相机快门、大模型内存峰值或付费供应商验收通过。

## 核心与其他平台

- 持久队列 Foundation XCTest **13/13**；永久失效门禁红对照确实重现迟到确认重建已删账号
  目录，修复后通过。删账号先 await 队列永久失效及本任务结束，再删除工作区；普通换号
  保留旧账号队列。真实 C ABI steer 测试 exit 0，明确拒绝保留 typed errorCode。
- 本地模型 Store **7/7**，真实文件/哈希/并发安装/已有文件不覆盖检查；Debug 资源 6 文件、
  Release 0 文件、错误 SHA 拒绝均通过。
- DeepSeek 本地真实 HTTP/TLS 专测 0 失败；最终核心 **8/8，91.80 秒**。未向真实供应商
  发计费检索，不宣称已验证召回质量、所有目标网站或验证码绕过能力。详见同日 web tools 记录。
- Desktop **2.2.37** 正常本地 bundle 全部 9 个 Mach-O 验签，普通 `--smoke` exit 0，
  2.167 秒；用户 `/Applications/MaiChat.app` PID 43461 前后不变。此为本地 Qt 包，
  不是新发布 Release 的可分发包。
- Android **2.2.16(253)** 两份冻结 APK 在自有 `emulator-5580` 安装成功，安装包 SHA 与
  冻结文件一致。原 `PortraitRegionInstrumentedTest` **4/4 pass，44.444 秒**。随后启动
  正常 MainActivity，PID28876，实际查看空手机号登录页，没有黑屏、布局重叠或错误遮罩；
  未输入账号或触发模型。回执 `/private/tmp/maichat-batch253-artifacts-20261103/runtime-evidence.json`。
- 使用现有索引生成器更新本地 `compile_commands.json`，**450 条**自有源文件实际编译命令；
  绝对路径数据库继续忽略，不进入提交。

## 保留的边界

未知普通 send 没有跨重启幂等保证，保留待确认并阻止同 owner 后续派发；仍允许用户本地排队。
未知 steer 仅在原 native epoch/turn/ID 下重试，绝不自动改为普通 send。更换主模型配置后，
尚无 restore/enqueue 时旧 unknown 按钮状态可能短暂沿用缓存；core 仍拒绝旧 turn，不重播。
真实账号/远端重连、真实相机权限预览及 OCR/VLM 大模型不在本次离线验收范围。

美白“面膜感”本轮按用户要求仅给出源码定位与方向：现有 L 增益对阴影更强、最大色度
衰减 30%，且最终 skin mask 无统一 Gaussian；并非二值 mask 或完全无羽化。
**本批没有修改美白曲线、24MP 通用裁剪门禁或 FFmpeg 覆盖策略**，不宣称这些反馈已修好。

178 完成安装后，用户进一步要求本地模型成为与 DeepSeek/GLM 并列的主模型。
此为后续新增工作，不能以本批普通测试页/工具注册冒充已完成；本记录仍是 178 的验证快照。
