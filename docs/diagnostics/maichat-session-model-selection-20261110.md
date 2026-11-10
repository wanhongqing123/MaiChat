# 会话独立模型、推理强度与原生检索

## 需求与可复现操作

1. 在创作中心创建 A、B 两个会话。A 选择 `glm-5.3-flash`，B 选择 `deepseek-v4-pro`；反复切换、发送下一轮并重启后，各自保持模型及推理强度。
2. 主模型菜单同时提供 `glm-5.3`、`glm-5.3-flash`、`deepseek-flash`、`deepseek-v4-pro`，与思考强度独立展开。修改只影响目标会话，失败保留原选择、草稿和页面。
3. 在 A 申请切换 GLM、凭据尚未返回时打开 B；迟到的凭据只能完成 A 的原请求，不能修改 B。再次明确选择原模型会取消该会话先前等待中的切换。
4. GLM 走 Chat Completions 和 GLM 原生检索，DeepSeek 走 Responses 和 DeepSeek 原生检索；按当前轮冻结的型号选择供应商、能力与 Key，缺少 Key 明确失败。
5. 移动端通过共享 Mobile JSON 设置会话，不重建 Agent。固定云默认为 DS Flash，本地模型只登记延迟 slot；首次明确选择或恢复实际本地会话才准备文件，不在云启动时复制 GGUF。

## 初始源码与环境

- 初始产品为 `0801974d58e5955af0744a4a00b2718e8c65eed4`；RED 阶段 HEAD `2c5a81b394f4b0b7dfc8f9698f1b4df1498bfa8a` 仅新增设计与计划。
- Windows 11，MSVC 2022，实际测试进程报告 Qt/QtTest 6.12.0，复用正常 `build-qt6-ui-20261109` 缓存和 native-images Qt SDK。测试使用 offscreen、自建内存/临时 SQLite 会话、假账号及 loopback HTTP；没有调用付费模型或操作真实用户数据库。
- Core/Mobile/iOS 由 Mac 协作者独占；Desktop、Android 为独立范围。完整 Core 实现尚未供件时，新接口只在私有目录核对，未单独替换产品 ABI。

## 原行为的决定性证据

| 用例 | 正常编译 | 原行为实际结果 |
| --- | --- | --- |
| Controller 会话隔离、默认、重启、busy 与实际路由 | exit 0 | 6 PASS / 14 FAIL；5 个会话例失败，GLM 六行误发 `/responses`，Pro 三行被拒绝 |
| Panel 四型号入口与会话显示 | exit 0 | 2 PASS / 2 FAIL；缺主模型入口/值 |
| MainWindow 延迟凭据 | exit 0 | 2 PASS / 1 FAIL；A 保持 DS，后来可见的 B 被错误改为 GLM |
| Host/RemotePanel 五个新增行为 | exit 0 | 3 PASS / 4 FAIL；缺 get/set 配置与远端确认流，已有 Coordinator 归属校验通过 |

原测试 raw bytes、日志长度及 SHA 保存在本机 `desktop-original-behavior-red-receipt.json`。Controller 的 GLM 用例在路径断言即失败，不能把尚未执行的 effort 断言计为独立 RED；Core 供应商 effort 矩阵由 Mac 单独复现。

根因调用链：Controller 发送新轮时用可变全局模型覆盖目标 session；Panel 新建会话复制上一个页面 label；MainWindow 仅保存待选模型字符串，并在 Key 返回时对“当时可见页面”执行切换。原 effort 菜单则写全局 QSettings。

## 实现约束

- 普通 Controller getter/setter 与远端 `get_session`/`set_model` 复用同一持久化操作；ACK 以 `session/model/reasoningEffort/wire` 为准。
- 远端 owner slot、login、session 与 requestId 必须匹配；HTTP 接收成功不等于 owner 已接受。禁止接收 endpoint、Key 或本地 prepare 路径。
- Key 仅进程内存，逐 stream 从对应 provider 回调读取，不跨供应商兜底。服务端仍只做控制面；本轮服务无型号 allowlist 需要调整。
- GLM-5.3 与 DS Pro 为文本型号，两种 Flash 支持图片。图像门禁读取目标 session 能力。
- 保留旧空模型的固定初始化语义；移动端不能依据旧全局 llama 偏好推断一条空 model 会话属于本地。已有显式 `local:...` 会话保留。
- 新交付采用精确 `.patch` 和 manifest；不发送新 ZIP/分片，不打新包或替换现装。

## 官方协议依据

- [DeepSeek 当前型号](https://api-docs.deepseek.com/api/list-models/)：Flash 与 V4 Pro，以及原生能力。
- [GLM-5.3](https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3)、[GLM-5.3-Flash](https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash)：型号、图片与思考强度。GLM Chat 路径保留订阅端点兼容。
- [GLM 思考模式](https://docs.bigmodel.cn/cn/guide/capabilities/thinking-mode)：工具交错调用需返回原始 `reasoning_content`；该字段应与可见正文分离，并由共享 Core 验证原字节和工具归属。

## 当前验证边界

上述表格为修改前实际 RED，各阶段原文件与日志单独保留。既有 2.2.55 包、现装和线上临时图片服务均未因此修改。

完整 Core/Mobile v1 的 34 文件在 Windows 核准 patch SHA、路径、base/new Git blobs 与 raw SHA；原 bytes 保全后完整合入，5 条 CMake 源接线与约定精确一致。正常 `maiagent` 构建 exit 0。该阶段尚未完成 Mac 测试，未将 v1 中间供件当最终冻结；后续窄修正与最终结果分列如下。

独立 Desktop review 随后拦下两个 setter 校验遗漏，并在上述完整 Core 上真实编译/复现：仅有 GLM Key 时切换 DS Pro/max 返回成功并写库；无客户端时未知型号/max 也被写入。同组另两例通过，结果为 4 PASS / 2 FAIL、186ms。补 guard 后同例和完整 Controller 组均通过；custom/local 注入保持兼容，busy 同值不物化旧空字段。

## 审查发现与回归

- 首建会话时选择模型/强度会清空空白页草稿：两个真实 QWidget 用例均得到空输入 RED。修正仅在显式创建首个会话时接管同一 document、光标与 undo；普通历史切换保留原机制，两个原例已 GREEN。
- 模型切换被 owner 拒绝后，正常历史页/事件会隐藏错误：两个数据行真实 RED。模型错误增加 owner 归属标记，正常历史保留它，新 prompt 等明确新状态会撤掉旧标记；原组合例和完整 Host 组已 GREEN。
- GLM 搜索同时设置 domain/recency 时 count 不保证生效，旧实现把合法多结果误判失败。按[官方 count 说明](https://docs.bigmodel.cn/api-reference/工具-api/网络搜索)改为独立 50 条预算，仍保留 2 MiB 接收上限；普通 API 与工具双入口以及 51 条拒绝例由 Mac 做原 RED→GREEN，Windows 最终普通 Web 组也已通过。
- 压缩摘要原来固定 `low`，不符合本轮固定强度约定。只将非空原请求 effort 传给摘要，旧空值仍保持 low；不改预算、schema 或原史。新增真实压缩例同时核对摘要及主请求、GLM reasoning 原字节和 checkpoint。
- 两个旧媒体 fixture 只更改页面 label，实际会话仍为文本模型；现明确设置图片会话模型，保留图片字节、粘贴、取消、全局池阻塞和发送断言。旧“新会话 model 为空”断言改为明确默认且不受 effort 菜单影响。
- Core ToolLoop 原“Image returned”文案断言与先前已采用的 group provenance JSON 冲突，Windows 原套真实 1 FAIL；改核 type、ownership、来源消息/图片/工具组 IDs，保留原路径、MIME、call/result 顺序和持久化断言。

## Windows 实际消费者

Desktop 16 文件被冻结并按 Git canonical 供件给 Mac。正常 Qt App 与四个测试目标编译完成；实际结果：

| 运行目标 | 实际结果 |
| --- | --- |
| Controller 完整组 | 45 PASS / 0 FAIL，54.524s |
| Panel 完整组 | 80 PASS / 0 FAIL，7.040s |
| MainWindow 10 个相关行为及 hooks | 12 PASS / 0 FAIL，2.539s |
| Host/RemotePanel 最后完整组 | 87 PASS / 0 FAIL / 1 SKIP，20.833s |

唯一 SKIP 是原 opt-in 临时图片 TLS 夹具，需要独立启动 `run_session_image_local_tls.py`，不属于模型选择未执行项。已亲看自有真实 QWidget 菜单截图：四个准确型号、当前选中值、独立思考强度与原权限/目录行正常，不将 label 替身当模型网络验收。

最终 Compactor 供件之后正常 App 重新链接、windeployqt 和 PE 依赖门禁通过。裸 PATH 的实际开发版进程 PID `43124` 正常 `--smoke` exit 0，2.977s；exe SHA256 `c751eae4aa29b833eff049a983f5b4ca01a189b3fc202a0610810e8ed86e3861`，路径为正常构建目录。版本文件仍为 2.2.55；这是新开发构建，既有候选包和现装没有被替换。smoke 跳过登录和账号恢复，未读回用户日志。

最终 Core 13 组已按当前源码重新编译/链接，在同一 cohort 运行得到 **13/13 PASS，93.36s**，包括 Store、ModelClient、TurnRunner、原 DS/新 GLM Web、Builder、Compactor、ToolLoop、队列及权限。console e2e 对当前实际 exe 全部通过。首轮 v1 的 12 PASS / 1 旧 ToolLoop 断言 FAIL 原日志仍保留，不与后续结果混计。

### 后续实际消费拦截

Mac 多一个原代理槽，首次暴露 GLM 改走 Chat 后 `MaiOpenAiClient` 完全没有消费 `proxyResolver`：实际错误为 `Network / curl: Could not resolve hostname`，回调和假代理收到请求都为零。先前 NO_PROXY 推测及提前写入的“已 PASS”简报已撤回，不能作为验证证据。普通两 wire 代理原组由 Mac 取得 Chat 24 FAIL→0；修正保留逐跳解析、显式直连、认证分离、跨 origin 不带 Key、取消、TLS 降级拒绝与每 attempt 总期限语义。

Windows 核实 live Ninja 的 TLS 测试宏开启，随后最新 Chat 源重新编译/链接的 **13/13 同组 PASS，78.78s**，console e2e 全 PASS。Mac 原 Controller 完整组最终为 **46/0**（24.382s），Panel 原完整组串行 **80/0**（5.469s），原 3 秒音频阈值未改；Root 已收到并读取这两份完整原 txt 及退出回执。首次并行 Panel 音频超时并未据此认定产品根因。两个首次 Mac full txt 曾被 wrapper 默认文件名覆盖，只有已读取的 observed excerpt/总计保留，不能宣称首次完整 txt 仍可取；最终唯一 run 目录的记录完整。

远端 Local 配置仍可读取、可选择四种云模型，供应商 effort 仅对共享 catalog 声明支持的模型开放。新增同例原来在 effort 仍 enabled 处实际 RED，修正后完整 Windows Host 为 **87/0/1**（20.833s）；ACK 前和拒绝后无乐观勾选、草稿保留断言也通过。Remote 控件原本没有 autoExclusive，未因此添加多余生产补丁。

最后 Chat/Remote capability 的正常 App 再次部署依赖与实际启动通过：PID `45944`，exe SHA256 `d4915ccf6a6ea28ca9266eb56e9f675e5fcd3e66013a2495b885fe4bdd58a430`；仍是 2.2.55 开发构建，非旧候选重标或安装。

## 移动端边界

Android Java 原菜单/协议/图片门禁三处 RED、会话投影两处 RED 均保全；阶段 55/55 和生产/instrumented Java 编译通过。正常 NDK arm64 原 cache 140 项增量编译并链接 JNI 成功，AArch64 与新路由符号已读回。没有连接 Android 设备，不把 Java 或 native 链接当设备界面/收发验收。

独立 review 另发现空白页草稿/附件迁移，以及冷云启动后远端向未准备 local 会话发送这两个边界。Android 在真实 Panel/composer/附件与普通 owner 边界取得 4 FAIL→0：仅在原空白页仍被选中时登记既有迁移标记；owner 按请求真实目标先 get_session，只为 actual local 模型调用固定可信宿主准备，再派发输入。路径不来自远端，也不扫描未使用历史来准备权重。

最终 9 类 JUnit XML 独立实算 **61 tests / 0 failures / 0 errors**；14 项源件/文档 raw SHA 和 Git canonical 全核准，二次 spec 与 quality review 均通过。最后 Core Compactor cpp 沿原 NDK cache 仅三步重新编译/链接，实际磁盘 JNI 为 `323751336` bytes，SHA256 `d7a75c230c5e7fe11710c84f19b83f0af30dd16222edb7ab9de3c786e15d1e2e` 与 receipt 精确一致。没有生成 APK、安装或连接设备执行；armv7 本轮未重编，不能扩大 arm64 结果。

iOS 工人同步核同边界；最终源件、模拟器消费者及真实 C ABI 四型号收发记录另行核合。共享 Mobile opt-in 测试仅将原 Agent/Steering 配置合为同一循环，加 `MaiMobileSessionModelTests`；正常 App 的测试 flag 仍为 OFF，无版本/签名变更。

最后 Chat 生产 cpp 经原 NDK cache 三步重链，JNI 实际为 `323823000` bytes，SHA256 `2abc9bff139e67ca0399e856a5b7e9708f682e65b9f6f148ffb6021ca7b62f68`。13 项 Java 源未改变，不重复运行原 61 例；前述 d7a75 产物保留为之前 cohort。

### iOS 最终同输入验证

普通 owner history 不携配置，首版 viewer 又没有 `get_session`，真实 UI 原例得到 reads=0 RED。v2 补真正读回/严格 ACK、普通 publisher 配置与 digest、remote local effort 门禁及模型选择错误归属。其后 3/3 数字用例曾通过，但亲看横屏截图仍见 fixedSize 让内容越出材质，因此该轮不当视觉验收。恢复 local 的首个 RED 因未实际进入 refresh 被撤回；后续先选 cloud B 再恢复 A，以 `restorePrepares=1` 确认进入路径，只回退 catch guard 即得到 A 错误覆盖 B 的有效 RED。blank prepare 离开页面后仍 create 的原 observer RED 独立保全。

最终 v4 保持生产源冻结，完整 page/selection/account/generation 守卫保护每次 await 与 catch；blank 在 create 前重新检查归属并使用点击时的 effort。菜单改为测量内容、限制 ScrollView 自身 viewport 并裁切，底部按钮真实点开清空确认但未执行清空。v5 只隔离 Debug 协议自检期间的 editor 与轮询，避免空 editor 成为草稿字典的第二写者，未删除草稿或附件断言。

最终同一输入 UIKit **3/3 PASS、0 FAIL、76.474s**，原日志逐例为 15.898s、54.903s、5.673s。两例走正常 Root UI：实际模型/effort 选择、两种 HTTP wire、草稿、发送、重启，以及横屏滚动与底部点击；另一例为 Model/native 隔离 fake peer 协议检查，不冒充真实手势或 live WebSocket。该协议例保留普通 history→get_session/ACK、错 tuple/ID/model/effort 不消费、A 错误不染 B、local effort 禁用、重连新读、publisher、`restorePrepares=1`、blank zero-create 和原文草稿/附件断言。

Root 已逐项复核最终 8 源的长度、raw SHA 与 Git blob；12 份已收安全原件的长度/SHA 与最终 receipt 一致，原日志确有 `TEST SUCCEEDED` 和 `BUILD SUCCEEDED`。视觉 manifest 的三份全屏 PNG 对应两个不同 SHA，均保持原字节；亲看确认菜单内容在材质内滚动，清空确认显示正常。未收到的额外 `ios-review-scope-red.log` 仅列为被引用但未归档的原件，不宣称持有；最终测试、Release 与决定性 refresh RED 原日志已核收。

正常 unsigned Simulator arm64 Release compile/link exit 0，实际版本 **2.2.4(203)**，exe SHA256 `aaa59e9dd1bff48de30a9bd6378dfa6bd4f1b4035e5bd177b7e5022875a4ae36`。Debug 构建与模拟器实际测试 App 的 exe 均为 `cc2cf7dd08b02d4c34785f8e291854aec8242105e23dbd9e793b26bd4f205982`；Release 回执中五个专用 Debug 符号和三项自有 fixture 文本均不存在。没有 Release 安装或实际手机操作。

普通 Native C ABI 使用当前源码及正常 Simulator 库，四个真实型号和 GLM low 的 HTTP 请求、busy/并行/排队、原子失败、重启及图片门禁通过，exit 0；测试源 SHA 与当前文件精确相同。它证明 C ABI 路由，不是 Android 设备运行或付费模型语义验收。

额外旧 `MaiMobileSteeringTests` 的原失败是新输入到达后才发出的旧 question 被正确取消；该语义在 base HEAD 已存在。test-only 修正先核旧 question 确 Canceled，再从真实下一次模型请求重提，捕获实际 continuation messageId，再补 21 条把已 Running 的 owner 推出页窗并核完成 point update。原预算、窗口和断言保留，同正常 Native 库新版 7 项 PASS 原日志已收到；独立只读复核通过，未修改产品来迁就 fixture。

## 统一冻结与交付边界

本轮最终源、实际产物和安全原件摘要见同名 JSON；Mac/iOS 独立诊断见 [Mac/iOS 记录](maichat-session-model-selection-mac-ios-20261110.md)，Android 见 [Android 记录](maichat-android-session-model-selection-20261110.md)。Root 在 main 统一提交代码和本轮诊断，供协作者按精确提交安全同步。

没有新包、版本、tag、公开 Release、现装替换或线上服务操作。未使用真实用户数据库、付费供应商请求或实际手机；跨设备真实 WebSocket 尚未验收。测试与正常消费者构建通过，不等同于真实模型质量、语义或速度已经验收。
