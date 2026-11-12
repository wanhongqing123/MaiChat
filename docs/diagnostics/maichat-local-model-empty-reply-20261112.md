# 创作中心本地模型空回复

## 复现操作与预期

2026-11-12，用户在 iPhone 创作中心选择自带“本地模型”，先问天气，再发送“你好”。
截图中两条助手行只剩“用时 0 秒 / 18 秒”和复制、引用按钮，没有可见正文。
预期是实际答复；未生成正文或请求失败时，应有可见状态，不能只留下空行。

未读取真实会话数据库或重放用户工具操作。用户截图没有完整模型返回或错误日志，
因此下面区分已证实的软件缺口、合成复现和仍未确定的手机触发条件。

## 源码与模型

- 开始基线：`11a14983f3ac5c4e5f9bf1e3c613249450f857d2`。
- Core 修正：`37e1381addf4a58e8a11ca41d6ee2126148a5fed`，Core tree
  `30114b3e33472ba1692987e0a90df11f81d853df`。
- iOS 组合修正：`cfed9c9361b43bdba15d32a7891f4ce252375f56`，MaiChat tree
  `9722004de75b0cca6ee65547803d0d48c9bb644d`。
- 后续 `3befec3df52cd6e0b6e5380739ceb4e2cbf99afe` 仅移除 Desktop 远程设备名单，
  未改变上述 iOS、MobileAdapter、Core 输入；Mac 当前测试仍按 cfed 记录。
- 随包 Qwen3.5-2B Q4_K_M：1396198496 bytes，SHA256
  `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。
  Mac 从此前已验证的 205 产物独立读 manifest 并核模型 SHA，与 Windows 候选一致；没有查设备容器。
- 普通 Swift 配置为 context 32768、maximumTokens 0、threads 2。

## 已证实的缺口

1. Native runtime 允许首个 token 即 EOG，空输出可返回成功；仅 reasoning 或空白也未必产生可见正文。
   当前实际模板始终禁用 thinking，不能据此断言它自动开启思考或解析器吞掉正文。
2. 旧 Runner 仅在执行过工具后才处理无正文完成；普通首轮空答可直接 completed/succeeded。
   这是基线已有行为，不是新增 tool execution observation 引入；无工具问候不产生该 observation。
3. Swift 显示层有意剥离 reasoning 正文，完成 footer 独立显示。后端错误只通过 8 秒 banner 展示，
   过期后空行仍留下。截图无 banner 不能证明从未发生错误。

## 修正

Core 对无正文完成复用既有一次有界收尾：使用当前请求和已有真实结果补答，关闭工具声明。
补答返回工具调用只报错，不执行；仍无正文则发布 SessionError。正常正文和取消不额外请求。
补答提示不再假称已执行工具。没有关键词脚本答复、隐式重跑原工具或改动 llama 模板、采样和模型预算。

iOS 仅在已完成、非 active、最后显示片段且非 question projection 的助手行增加中性说明
“本次未生成可显示的回复”。判断整条 sourceParts；正常正文、工具、问答、媒体、附件及未知 kind
均保守不提示。说明仅是 View 文本，不写消息、数据库、复制内容或模型上下文，也不展示隐藏思考。

## 验证

### Core

普通生产 Runner 配合自有脚本模型复现 empty、reasoning-only、ASCII whitespace 三种首空。
最终同一测试对象 SHA256 为
`19a3dee11e95b66396305f63f49ff763a22036ecb50fa44fbf49d16c92bd1f86`。
旧生产 14 条严格断言失败、exit 1 / 45.636s；仅换新生产后 exit 0 / 45.598s。
覆盖补答正文、持续空答有界报错、正常与取消不重试，以及补答乱返合法 file_write 时零工具 part、零文件副作用。

原八套 Core CTest 在 Windows/MSVC Release 实际 8/8 PASS，CTest 99.06s；没有存储、协议或控制台修改。
首轮 60s 运行预算曾到期并保留，后按原 target 已有 45.58s 及多 Agent 析构等待事实调整有界预算；
该观察轮不冒充功能 RED。原始日志和源码、测试对象、exe SHA 保留在
`.cache/local-model-empty-20261112/`。

### 真实模型与平台边界

- Windows 当前普通 Native target 的五组真实 GGUF 问候、系统说明和历史用例全部正文非空、UTF-8 合法，
  exit 0 / 16.853s。该简化请求不含手机完整目录，也不经过新增 Runner 补答，不能替代手机验收。
- Mac 复用普通 Simulator CABI，显式 phone-capable 屏幕能力、131 工具、2/32768/0/high 配置，
  对自有“你好”做 model-only probe。无 GPU，120.221s 返回 Canceled，正文空、调用数 0、无工具执行。
  它不是 Runner/send 复现，也不是未经修改的 Simulator Swift 配置；未延长预算或重跑。
  first_token_ms=-1 只代表没有 onText，不能推断没有 prefill、reasoning 或生成 token 进展。

### iOS 页面

普通 Debug Simulator 旧 View 的四种正例实际出现同款 footer，reasoning 未显示，8 秒 banner 消失；
中性说明缺失导致 8 条断言失败。用例结束后 Xcode 自动诊断收尾被取消、driver exit -15，
与实际功能 RED 分列。

负例直接驱动普通 AIMessageRow，保留 fragment/sourceParts 边界；reasoning 携路径是组件输入边界，
普通 timeline 会过滤 reasoning，不能把该负例当作真实 timeline 附件复现。
审查修正了 fixture 的 attachment_id 解码键，没有改变生产 Model。

Mac 在精确 cfed 源码完成普通 Debug Simulator build，exit 0 / 58.501s。
两个 UIKit 用例实际全部通过，分别为 34.044s、54.131s，共 88.175s：四种正例走普通页面，
十二种负例走普通行组件；原 test log 明确为 2 tests、0 failures、TEST EXECUTE SUCCEEDED。
普通 Release Simulator build exit 0 / 117.932s，exe SHA256 为
`9a99e912477cf6cd7c2e71399d9d6e1c9151ca3874214dfdc3a8ecb4a7e1c8c5`。
执行端完整证据记录六个 Native 对象与合库成员逐 SHA 相同；五个新测试 marker 在
Debug 十一个镜像中的 debug dylib 为阳性、Release 九个镜像中全阴性。
Windows 独立核对六份源码的 canonical SHA/blob 与 cfed 一致，原编译/Ld 摘段确认
Runner 本轮正常重编、Debug/Release 最终链接均消费本轮 libMaiMobileAgent.a；
原 UIKit 日志 31441 bytes、SHA256
`8d9e4b86dd7d39f584be8a664fbf620d928aa92435ca3e3714ea803230000554` 独立读回一致。
对象/产物 SHA、归档成员提取和 marker 扫描仍是执行端记录；未将 Mach-O 传到 Windows 重新计算。
完整证据、原日志摘段和原件长度/SHA 索引均已接收。

两张最终展示图已在 Windows 亲看：0 秒及 18 秒行均保留 footer 并显示中性说明，后者 banner 已消失。
IM 图片传输后分别为 86539/88395 bytes，与 Mac 原 PNG 的 129586/131320 bytes 不同；
收到图 SHA256 为 `454cdea35cebbef7fac9138df95c86eb91de7200e893ba3135fc407a9b801190`、
`4c7f018fcb6e5a7731467a06981842fd8fb4d4afe1a0f79e39f08efb49ee4c54`，只作为实收展示图，
没有将其冒充逐字节相同的原 PNG。

物理 iPhone 最后确认仍为 build205，旧 a328/build206 候选未安装且继续冻结。
上述 Release 是 Simulator 产物；新的空答修正尚未在用户手机运行，手机现场空答的具体触发条件仍未确定。
