# 本地模型没有在当前回复展示已有照片（2026-11-04）

## 现场操作与预期

用户在 iPhone 的 187 版本沿用先前拍照、旧 `view_image`、继续追问的会话，输入
“我的意思是给我展示下刚才拍摄的图片”。14:52 截图里的助手只回复文件路径和到手机相册
查看的建议，没有调用投递工具，也没有在这次回复显示缩略图。用户进一步明确需要当前
回复里的缩略图，不能只修最初拍照消息的卡片。

预期：沿用实际拍照回执中的相对路径，调用 `agent_send_media(file_path=path,type=image)`，
在新的助手消息中显示图片卡片。旧消息已有同一张照片也不能阻止本次展示；默认不打开
全屏，不调用读图工具，不声称照片已经加入手机相册，不要求用户提供 Photos 资产 ID。

开工提交为 `5a72e8121dafeac1b06264a52f29552b95892362`，分支 `main`。不读取用户照片、
会话数据库或凭据，使用仓库中的公开 JPEG 和自有工作区构造同类历史。

## 已定位事实

- `agent_send_media` 已在 Mobile C ABI 无条件注册，对当前本地主模型可用；普通媒体准备
  和工具执行不要求模型视觉输入。完成回执包含 `delivery=current_ai_session`，Swift 已能
  将它渲染为当前消息卡片；去重只在一条消息内部进行，不会跨消息隐藏同图。
- `MaiLlamaToolCatalog::specification()` 原先只给模型全部工具的名字枚举，没有用途说明。
  第一阶段如果直接回答，发现适配器就把这段回答作为结果返回；模型没有读到具体工具
  描述。`mobile_preview_image` 负责全屏，不能替代当前回复的缩略图投递。
- `agent_send_media` 旧描述强调处理成品，对相机原图及用户要求重新展示的场景不够直接。
  手动拍照的文件是 Agent 工作区文件，不能由相对文件名推断已经保存到系统相册。

## 原始对照

完整 128 项目录、正式基础指令、旧两字段拍照回执、旧读图记录、前次真实本地回复，加上
用户精确新句，在 Mac/Metal 的原实现实际选择了 `mobile_preview_image`，耗时 8.146s。
这是错误展示入口，尚未执行，不满足当前回复卡片的验收。它与截图“完全没有调用工具”
的表现不同，不能把合成历史称为读取并逐字复刻了用户未知的完整历史。

原始证据位于 `/private/tmp/maichat-current-reply-photo-20261104-kgroo1lw/baseline-result.json`。
后续候选、真实工具执行、界面及设备验证结果追加在下方，不能只凭模型回答非空签收。

## 用户修正：撤销额外的工具预选层

用户明确反对为了提速隐藏工具信息，要求通过模型和推理实现优化速度。因此撤销本地自建
Catalog/Discovery 包装，主模型直接接收全部可用工具的原始说明与参数；不保留旧路径或
用隐藏工具、强制某个 tool choice、关键词路由代替模型调用。Responses 供应商的
`tool_search_call` 事件兼容与这层本地包装无关，保持原有处理。

撤销前四组固定采样对照均失败：原始、全部工具首句用途、只改两项描述、用途加描述，
均在第一阶段只选 `mobile_preview_image`，第二阶段没有 `agent_send_media`。首句用途
摘要仍丢掉了全屏工具的后置限制，所以不能只凭“已补说明”签收，也不发布该中间候选。

完整 128 工具的精确 GGUF 模板实测原声明为 27400 tokens/113061 bytes，明确三项展示
语义后为 27609 tokens/114100 bytes；在现有 32768 上下文内分别剩 5368/5159 tokens。
这项是词表测量，不是推理或手机内存验收。直接完整声明、旧描述的真实 Mac/Metal
推理仍误选全屏，耗时 31.465s。不能把“移除预选层”和“展示任务已正确完成”混为一谈。

相机原图和已有媒体在本次回复的展示语义，以及全屏工具仅供明确要求放大的边界，
继续以普通工具的真实描述澄清；工具名称、参数及执行权限保持原接口。

## 真实完整工具调用与已知边界

完整 128 项原声明直接推理，加上三项准确展示描述后，污染历史里的原句正确产生
`agent_send_media`，随机小写 UUID 照片路径逐字相同且真实文件存在，耗时 33.6355s。
所有对照保持同一 2B/Q4_K_M、32K、temperature 0、seed 0，没有隐藏全屏工具或强制调用。

随后使用公开 Mobile C ABI，预置自有 SQLite 合成旧历史，公开 configure/send：
真实本地工厂直推 → 真实 TurnRunner → `agent_send_media` 执行 → SQLite 持久化 →
公开 snapshot。新回复恰好有一个真实 completed 图片交付，旧拍照回执仍是原来两个字段。
重新打开数据库的回读也一致。没有伪造新回复、tool part 或图片投递成功。
两次实际模型推理与工具循环共 64.984s，无宿主回调、网络或传感器执行。

原始快照 `/private/tmp/maichat-current-reply-photo-20261104-kgroo1lw/native-e2e/snapshot.json`
SHA256 为 `28e0857490c3d79ce599295e5de5cdb0a1baa2c7d51759943fdfdb006489a90d`。
照片是仓库公开 JPEG 的自有副本，未读取用户照片。此前旧历史是明确标记的合成种子；
只有新用户提交和新助手回复由此次真实 Agent 生成。

固定八项控制为 **7/8**：三种本次回复展示说法均选 `agent_send_media`；明确要求全屏
仍选 `mobile_preview_image`；问好正常回复且不调用工具；新闻选 `web_search`；拍照选
`mobile_take_photo({})`。每项均是完整 128 声明、一次生产工厂 stream，约 29–34s。

失败项是“这张照片里有什么内容”：没有编造场景，但仍误选全屏预览。额外两句明确
“显示不等于视觉分析”的私有指令对照仍然失败，因此没有把这段无效提示写进产品，也
没有隐藏全屏工具、强制 tool choice 或将 7/8 改判为全绿。当前本地配置不支持照片内容
分析；本次修复证明的是请求展示时的实际卡片交付，不是模型所有语义选择都可靠。
回复中仍出现未经验证的“网络延迟”推测和冗长元数据，也保留为模型质量限制。

完整原始对照、所有失败、真实消息和哈希汇总于
`/private/tmp/maichat-current-reply-photo-20261104-kgroo1lw/evidence.json`。上述测时是 Mac
原生推理总耗时，不是 iPhone、首 token 或单独 prefill 时间。没有以“冷进程”冒称冷 OS
文件缓存/Metal 缓存；速度优化需保持完整工具信息并另外测量。

最终完整核心回归 Core8 加六项针对性/默认套件 **14/14**，105.16s；真实模型环境变量
在这些默认套件中明确未设置，真实 2B 测试由上面的固定矩阵另记。完整 native C ABI
iOS 宏与通用配置分别 10.66s、10.32s 通过。符号与 CTest 检查确认旧 Catalog/Discovery
及两个已删除测试目标不在产物中。证据
`/private/tmp/maichat-direct-full128-final-core-20261104-4t87ppdj`。

## 正常 App 界面回归

188 正常模拟器 App 的 Debug opt-in 夹具读取上面的原始 C ABI snapshot 和公开 JPEG，
不制造新完成状态或模型正文。正常 RootView 中进入创作中心，最新回复图片直接可见；
点击后才放大，退出后缩略图保留；再滚回旧拍照消息核对同图仍在；重新启动、进入同一
会话并重新解析原快照，最新回复仍显示照片，输入草稿及附件未被改动。

首轮新用例的图片、点击、返回均通过，随后测试误选隐藏的会话抽屉作为滚动容器，因空
frame 失败。只将测试定位改为可交互的消息视口，保留全部断言，原失败日志保留；不是
产品隐藏照片或删除旧卡片。重跑 **1/1 通过、0 skipped**，30.815s，xcodebuild 32.398s。
原有拍照成功卡片用例在首轮也通过。根任务已查看本次实际截图和录屏抽帧，未自动全屏，
放大/退出时图片连续存在。测试使用公开色条 JPEG，不冒充用户拍摄内容或传感器验收。

日志、结果包和录屏：`/private/tmp/maichat-photo188-current-reply-ui-selector-20261104` 的
`.log/.xcresult/.mp4`；截图目录 `/private/tmp/maichat-photo188-final-ui-attachments-20261104`。
测试源码修正绑定在 `/private/tmp/maichat-photo-current-reply-scroll-selector-20261104`，
原 711 项输入只有该测试变化，其余 710 项相同。重链接后的 10 项 App Mach-O 字节相同；
入口 wrapper 签名变化，但移除签名的临时副本 payload 哈希及 UUID 相同，不混称 11 项
字节全部相同。最终源码输入见 `/private/tmp/maichat-photo188-20261104/source-freeze-final.json`。

## 188 真机产物与连接边界

188 真机包构建成功，11 项 Mach-O 独立 strict 与外层 deep/strict 签名检查通过，随包
Qwen3.5 2B 仍为 1396198496 bytes、SHA256
`57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。
debug dylib UUID 为 `530E0DC8-5621-3263-B594-81D6C91C5A25`。冻结回执位于
`/private/tmp/maichat-photo188-device-final-artifacts-20261104/evidence.json`。

首次安装立即失败，CoreDevice **4016**，手机连接的 `tunnelState=unavailable`，
`CurrentlyAssertableStates` 为空。已请求用户重新连接并解锁 **iPhone**；不是把问题归因
于 Mac 锁屏。原始日志 `/private/tmp/maichat-photo188-device-install-20261104.log` 和
`/private/tmp/maichat-photo188-before-install-devices-20261104.json` 保留。当前手机最后
核验安装版本仍为 187；**尚未安装或启动 188**，不能宣称真机展示及性能验收已完成。

Android **2.2.25/262** 已在自有 emulator-5580 更新、正常启动，PID7995；设备 base.apk
与冻结包 SHA256 `b9a64aeed0a79e5d49ca9eb8da7a95f61c550b2d999bc2c7b7d18939bed66a66`
一致。根任务也查看了空登录页截图，没有登录或触发模型/照片任务，不冒称登录后验收。
Desktop **2.2.45** 的 9 项 Mach-O、外层签名和无网络独立 AppData smoke 通过，3.784s；
用户 `/Applications` PID43461 的路径及启动时间不变。平台证据：
`/private/tmp/maichat-photo-platform262245-188-20261104-rd63qylq/final-evidence-manifest.json`。

## 保留完整声明的提速实验

私有实现仅改变逻辑 batch，物理 microbatch 保持 512。固定相同 128 项完整声明、同一
27,721-token 输入、原模型/F16 KV/32K/temperature 0/seed 0，顺序
512→1024→2048→2048→1024→512，各两次。保留每逻辑批次的同步与取消守卫，也保留
请求后的 KV 与 SSM 清理；全部生成 87 tokens 到实际 EOG，调用及参数逐字相同。

| 逻辑 batch / microbatch | 平均总耗时 | 平均 prefill | 取消等待（各一次） |
| --- | ---: | ---: | ---: |
| 512 / 512 | 33.331s | 29.461s | 0.272s |
| 1024 / 512 | 33.574s | 29.784s | 0.684s |
| 2048 / 512 | 33.080s | 29.156s | 1.518s |

2048 的总耗时差仅 0.754%，小于基线 2.66% 的波动；取消等待反而明显增加。因此**不发布
该 batch 调整**，产品保持 512/512，不宣称已完成提速。所有取消均无正文或工具调用回调。
这只是 Mac 私有实验，取消每档一次，非手机稳定性或热状态验收；系统缓存、并发普通
模拟器 UI 活动等边界均保留在原始记录，不把冷进程称为冷 OS 缓存。

原始实验、计时钩子、配置、源码差异及不改仓库/核心缓存的证明位于
`/private/tmp/maichat-full128-batch-paired-20261104-6o98jts2/summary.json` 及同目录文件。
主要代价确实在重复 prefill。后续若做前缀加速，Qwen3.5 Hybrid 必须一起保存注意力和
SSM 状态，当前没有账号/会话缓存身份字段，不能直接保留全局 KV 或在生成后简单裁剪。
这属于需要独立设计、验证内存及归属失效规则的工作，本次没有擅自引入。
