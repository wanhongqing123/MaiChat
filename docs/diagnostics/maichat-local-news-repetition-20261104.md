# 本地中文新闻任务复读与云端完全访问检索审批

基线main/404768de。用户2026-11-04 10:41 iPhone截图：多次要求新闻，模型只回复
“好的，请告诉我你接下来需要什么帮助？”，约3秒，未执行工具。已知完整可见历史为
assistant固定句→user新闻→assistant固定句→user妈的 听不懂话吗→assistant抱歉固定句
→user我要看今天的新闻→assistant固定句。顶部遮挡消息不杜撰。
此前已核验这台设备安装的是 183；截图本身没有显示构建号。

随后10:43截图的工具授权属于云端模型，用户明确澄清：current_time一次后，
web_search(count15/query今日新闻 头条 要闻)仍在完全访问下等批准。两项分别排查。

预期：自然中文新闻请求触发真实检索，而非套话/承诺/编造；完整最新消息和角色不得丢失，
不能依赖清空用户历史掩盖问题，不能换模型却不测原用例。完全访问下普通只读搜索/抓取
不额外弹付费任务审批；真正媒体任务提交等既有确认机制保持。

## 本地复现证据

以精确HEAD系统说明、119个公开CAbi工具目录、同GGUF和合成已知历史原生重放，
按需发现1.633秒和直接完整schema13.942秒均逐字复读固定句、零工具调用。
模板角色和最后user正确，额外EOS未插入；不是最新消息丢失。短system也未解决。
无历史仅问我要看今天的新闻，0.5B错误选择file_read(news.txt)，确证工具选择失效。

官方Qwen2.5-1.5B Q4_K_M候选已严格下载校验，模板与旧白名单一致。但同新闻/污染历史、
完整schema/仅两个news工具等对照仍无法触发正确检索：有的询问地区，有的声称不能实时
访问，有的只口头说稍等。它能正确选相机和完成绝对路径隐藏nonce读取回灌，但不能因此
晋升为默认模型。未修改App随包基座。

证据/private/tmp/maichat-local-primary-model-20261103/news-reproduction-evidence.json
与candidate-comparison-evidence.json（22项），包含实际请求、结果、调用ID和SHA。
仅使用公开工具目录和合成历史，不读取用户DB或执行联网/相机等操作；file_read仅限新建
scratch目录，真实文件结果按ID回灌。耗时不是受控性能比较。

## 云端权限

已定位web_search/web_fetch把API按次计费误分类为requiresPaidApproval，付费检查早于
Never策略，导致云端也弹确认；不是用户没保存权限设置。修复与红绿矩阵见
maichat-full-access-web-retrieval-20261104.md。只修正这两个只读工具的metadata，未改
通用付费闸门、账号配置或授权UI；测试均用loopback合成提供方，无真实供应商/手机验收。

## 后续实际验收

先交付权限最小修复，避免与尚未通过质量测试的本地候选绑定；随后继续评估工具适配更好
的官方小模型（包括新模板的严格审阅、手机内存和原始新闻用例），候选未通过不得发布。
正式本地结果需同自然问题、同历史和真实工具动作验收，不能只看非空回复或工具JSON。

## 候选和路由原型的反例

官方 Qwen3-0.6B Q8_0 候选（639446688 字节，完整 SHA-256
`9465e63a22add5354d9bb4b99e90117043c7124007664907259bd16d043bb031`）在独立审阅的
4100 字节原生模板下仍失败。临时对象仅允许该精确模板，关闭 thinking 的对照、按原作者
建议的真实 top-k/top-p/presence 采样链与三个固定 seed 都未解决污染历史中的套话。没有
修改产品模板白名单或随包权重。完整 22 项候选对照位于前述 candidate-comparison 证据；
后续不同采样与路由实验另有独立记录，不合并成单一通过率。

还测试了一个非产品路由原型：保留完整 system、历史和工具原定义；第一阶段只允许原生
`tool_search(names)`，允许空数组表示无需新工具，并在目录描述中提供全部 119 个原始
工具说明。第二阶段用原始请求与所选工具完整 schema 生成实际调用。工具 grammar 来自
上游原生解析规则，没有按“新闻”等关键词替模型选工具。

- 0.5B 在新闻、普通问候、相机四个选择用例中均选择错误。
- 1.5B 能为两种新闻历史选择 `web_search`，相机选择也正确；但普通“你好”仍选联网工具，
  而且实际读取唯一 scratch 文件后继续重复读取直到预算耗尽，最终还错误声称没有读到。
- 旧 nonce 的字符串命中不能当完成：同时记录 `clean_completion=false`，不能把预算耗尽
  后碰巧出现 nonce 的文本记成成功。
- 同一严格 grammar、实际词表下，合法 `tool_search({"names":[]})` 的所有 token 均可接受，
  完成后 EOG 可接受；起始 EOG 和普通正文被禁止。排除了“空数组被 grammar 禁止”的猜测。
- 追加通用目录说明也没有修好普通聊天，不继续靠提示词反复试出单个绿灯。此原型不满足上线。

证据：`controlled-routing-evidence.json`（14 项实测，未完成/被停止的调用单独注明）与
`controlled-routing-empty-grammar-evidence.json`，均位于同一临时诊断目录。后者 SHA-256
为 `b846bccf90d0758b67d021d8b698ca0f21efe7776a3c910472ea2fd988ede9d4`。以上没有执行
真实联网、相机或用户文件操作；唯一执行的工具仅为新建测试 scratch 内的文件读取。

公开 C ABI 目录包含 119 个工具，不含运行时可选宿主工具。这些结果证明已知输入下的
原生复现与候选失效，不能替代手机最终目录、实际工具结果和设备内存的验收。

## 本次修复与交付检查点

随后验证的 Qwen3.5-2B Q4_K_M 在实际生产 archive、32768 context、temperature 0、
非思考、AUTO 下通过现场污染历史新闻、fresh 新闻、短问新闻、普通问候和相机选择。
真实 relative 文件读取、匹配调用 ID 的结果回灌和准确返回隐藏 nonce 也通过。
本机合成搜索服务完成一次检索后正常停止；同会话追问准确复制了工具结果中的随机标题，
并明确说明该内容是测试数据。完整命令、原始判分、最终复测与能力限制见
[原生证据记录](maichat-qwen35-2b-native-evidence-20261104.md)。

产品固定新权重 1396198496 字节，SHA-256 为
`57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`；保留原作者
Apache-2.0 许可证和模型卡，分别记录作者 revision 与量化发布 revision。正常 Debug、
Release 都由既有资源阶段校验并打包，不增加下载、模型管理、参数表或设置弹窗。
旧内置选择 ID 映射到当前内置模型；导入目录、旧清单和可能仍被 mmap 的文件保留。
增量 App 的原有受管 GGUF 在新文件验证完成后移除，防止打包两份权重。

原生层只新增经审阅的 7755 字节模板精确允许项，并明确采用已测试的非思考模式。
工具调用继续由原 TurnRunner 审批、执行和回灌，没有关键字替模型选择工具、强制调用或
云端回退。Metal 选择依据实际 GGUF 元数据，覆盖混合量化矩阵、FA256、SSM_CONV 与
GATED_DELTA_NET；任一必要算子不支持则纯 CPU。实际发现上游 Metal 对某些类型报告
支持却没有 matvec kernel，故在探测前排除这些类型，并保留独立红绿对照。

验证记录：

- 原始八套核心回归最终 8/8，198.70 秒；相关原生八套定点测试此前 8/8，115.62 秒。
- 13/13 Swift 6 Store 测试通过，含旧选择迁移和旧文件保留；四项增量打包测试通过。
- 原有账号生命周期三项测试通过。宿主 C ABI 普通回归和真实 2B/32K 非执行探针退出 0。
- 正常 iOS 185 的四项 UI 回归通过，159.18 秒：忙碌时继续发送且仅保留加号、紧凑权限
  面板、行内云端/本地切换、真实两轮中文“松柏”记忆及重开恢复。后者使用实际模型回复，
  直接断言持久化 assistant 文本中的代号，不再仅断言回复非空。
- 已导出并查看 UI 截图与录屏取帧；Simulator 实际纯 CPU，两轮界面显示约 30/38 秒，
  不把该延迟当作 iPhone GPU 实测。正常 App 资源中只有新模型一份。

iOS 185 的正常模拟器结果为
`/private/tmp/maichat-qwen35-185-regressions-final-20261104.xcresult`，录屏与导出附件在
同名前缀目录。真机 build 成功，11 个 Mach-O 单体 strict 及外层 deep/strict 验签通过；
`MaiChat.debug.dylib` UUID 为 `F6B4D07A-343B-3A9F-8EA7-7E2CECB41897`。
冻结包及源码/权重哈希位于
`/private/tmp/maichat-qwen35-185-device-final-artifacts-20261104/evidence.json`。

设备开始不可用时安装 184 返回 CoreDevice4016；用户再次要求立即安装后，185 安装成功，
已正常启动，设备回读 `com.kongshang.maichat` 2.2.4/185，安装 bundle UUID
`4C80399A-6598-424F-A467-2A4AA192C8D7`，PID **27276**。安装、启动、版本及进程回读
分别记录在 `/private/tmp/maichat-qwen35-185-device-{install,launch,appinfo,processes}-20261104.json`。
没有卸载、清数据或读取用户会话。交付启动后没有再重启用户正在测试的 App；本轮未运行
新真机 QA 探针，不能宣称已验收 A16 的新模型延迟、峰值内存或实际供应商新闻闭环。

Android 2.2.22/259 在专用 emulator-5580 安装、冷启动，PID5226；两个设备 APK SHA 与
冻结产物精确一致，空登录页已查看，没有登录或模型调用。Desktop 2.2.42 的 9 个 Mach-O
验签及隔离无网络 smoke 通过，原 `/Applications` 进程 PID43461 未动。原生核心、平台与
宿主 C ABI 完整证据分别位于：

- `/var/folders/30/c_mhtwjd49xgfqyq61r1_57m0000gn/T/maichat-native-final-evidence-20261104-na3m96xn/manifest.json`
- `/private/tmp/maichat-local-platform259242-20261104-acymy8tu/final-evidence-manifest.json`

剩余已知模型限制为很长随机绝对路径复制不可靠、metadata 声明过选造成额外开销。
本次修复不等于所有模型任务均可靠，也不包含尚未解决的美颜肤质/全身皮肤覆盖问题。

## 最终产品提交绑定

2026-11-04 对产品提交 `34936e476f5cc43e73b9a3c7fd9d577f7310253d` 完成只读绑定：
平台冻结输入 942 项、原生输入 9 项及 iOS 输入 29 项，共 960 个唯一文件，其原始 Git
blob 的 SHA-256 均与构建时冻结输入一致。重新计算两个 Android APK、Desktop App 全部
242 个普通文件，以及 iOS 11 个 Mach-O 和 1396198496 字节模型，均与原冻结哈希一致。

既有 Simulator 185 的 xcresult 回读为 4/4 通过；真机安装、启动、版本与进程回执绑定到
2.2.4/185、bundle UUID `4C80399A-6598-424F-A467-2A4AA192C8D7`、PID27276。本次仅
读取既有回执，没有重新构建、签名、运行测试或操作设备，不新增真机模型体验验收结论。

Android、Desktop、iOS 冻结目录及原生证据目录各新增 `final-source-binding.json`。
中央绑定为 `/private/tmp/maichat-local-platform259242-20261104-acymy8tu/final-source-binding.json`，
SHA-256 为 `6bd8c7dfab47c6277a05ffb2551e15d540d662a95b534f5711097d9f0f1bf047`。
