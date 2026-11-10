# 连续工具批次与原始图片的上下文压缩（v7c / v7d）

记录时间：2026-11-10，Asia/Shanghai。验证平台：macOS 27.2（26B123）、arm64。
本记录追加独立复审发现的两个边界，不替换 [v6 压缩记录](maichat-context-compaction-20261110.md) 或 [v7b 活动轮次记录](maichat-context-compaction-active-turn-20261110.md)。

## 可复现操作与预期结果

在临时会话中保存一条 user 原始指令和一条 `completed == 0` 的 assistant。
assistant 先连续保存 20 个 Completed Tool part：前 19 个输出各 12000 ASCII 字节，第 20 个是截图工具的小输出；随后统一追加 Image part。没有 Text part 或人为分隔符。
FakeModel 声明窗口 32768、输出预算 4096。调用普通 `MaiContextCompactor::prepare`，另由真实 `MaiAgent → TurnRunner → executeTools` 生成同形数据。

预期：触发实际摘要调用并原子保存检查点；当前 user 指令保持原文；完整 call/input/result 成对封存；原始图片继续通过模型请求的 `images` 输入进入媒体编码路径。原历史及其来源指纹不变，图片来源如实表示为整个工具组，不能猜单图属于哪个工具。
关闭并重新打开 SQLite 后，同一检查点直接复用；再追加同样大的第二批 20 个工具及图片，只摘要新增来源，最新图片仍保留。

另一个普通 API 边界是空历史：FakeModel 窗口 8192、输出 1024，系统指令或工具描述含 40000 ASCII 字节。没有可摘要原史时应明确返回 InvalidInput，不能崩溃、虚构摘要或写入检查点；正常小空请求仍可组装。

## 源码和构建身份

工作区 main HEAD 为 `7dffeb5bf8b45a1ebec6a780106b86dc619dabde`，本子任务没有提交或推送。
下列 canonical 使用排序、无额外空白的 JSON（相对路径 → 原始文件 SHA256）再计算 SHA256，覆盖相同 31 份源文件。

| cohort | canonical | 本次状态 |
| --- | --- | --- |
| v7b 基线 | `5d99bc387f072a60f00868472274ef26c9abf66ec59bb92c1a93b1ce0cd6bb82` | 原始源码、回执、消费者证据保留 |
| v7c 中间版本 | `94e145a1ce1af6531a80465943d80a3da57862f820753a283ec3b90c8282444c` | 连续大工具组已可压缩；独立发现空历史预检越界和旧 wire 文案测试不兼容 |
| v7d 冻结版本 | `da292832a59f11057f9dc63323907848df810c07e460ff1f826c40c3e537a6b6` | 两探针及 4 组相关 Core 门禁通过 |

相对 v7b 共改 6 文件：`MaiContextBuilder.cpp`、`MaiContextCompactor.cpp`、`MaiContextCompactor.h`（注释）、`MaiContextCompactorTests.cpp`、`MaiTurnRunnerTests.cpp`、`MaiModelClientTests.cpp`。
v7d 相对 v7c 只改 Compactor 的空历史 guard 及上述 Compactor / ModelClient 测试。没有新增 API、字段、schema、CMake、供应商协议或采集代码。

正式私有构建目录为 `/private/tmp/maichat-pdf-core-gate`，沿用 Release / arm64 / Graphics OFF / tests ON 的原配置。
`frozen-native-closure-v7d.json` 逐项记录 78 个自有 Core TU 的源码、对象、命令和依赖 SHA；无缺失依赖文件，无源码或自有头晚于对应对象。
v7d `libmaiagent.a` SHA256 为 `1898fca2821d84e508bdc715cbd5a265f65766d7379c370883d64c1215b319a6`。

## 根因与修复

v7b 为保护最新工具和观察，遇到紧随图片的工具组会向前回退到整个连续组的起点。真实 executeTools 先保存整批相邻 Tool，再统一追加所有图片；只有一个大组时起点是 0，因此没有可总结来源，摘要调用数为 0，最终只报超限。

修复保留通常情况下的最新完整组。在选中模型的计数或明确估计预算下，如果受保护尾部加摘要、引用和 guard 预留仍无法适配，则将稳定 Completed 工具组整体封存，边界位于最后完整 Tool 之后、首张 Image 之前。已有检查点后追加另一个大组也适用。已有有效检查点且请求实际适配时，追加观察不会仅因触发阈值再压原保留组。Pending / Running 仍不进入已封存前缀。

Builder 保留原图片路径、MIME 和实际图片输入，并附 `tool_group_image_observation`：精确消息 / 图片 part ID、工具组首尾 part ID、工具数量与归档数量，所有权明确为 `tool_group_only`。
整组已归档且展示表不超过 4096 字节时附 call_id / tool / part 小索引；其他情况使用精确范围与数量，由普通历史 API 或 `session_history list_parts/read` 分页恢复原始调用和结果。这个字节值只限制展示索引，窗口仍由模型 token 计数或明确估计判断。原图片没有被路径文字或摘要替代，也不再声称来自某个“preceding tool”。

v7c 新增 framing 预检对空历史直接取 `history.front()`，在超长系统指令下形成越界。v7d 先检查历史非空，空历史继续经过原有无来源分支返回明确容量错误。
ModelClient 旧测试只要求观察文案包含 `Image returned`，与新的真实组来源协议不符；改为解析 HTTP wire 的 JSON，核对真实持久消息 / 图片 / 工具首尾 part ID、组数量及所有权。原 HTTP call/result 配对、媒体类型、base64 解码后 PNG 逐字节相等的断言全部保留。

## 原始 RED → GREEN

证据根目录分别为：

- `/private/tmp/maichat-context-history-20261110/`：协调端独立原始探针和基线产物。
- `/private/tmp/maichat-context-compaction-v7c-20261110/`：中间版本及正式新增用例 RED；`baseline-94e/` 另外保存 31 源和该版本实际失败门禁。
- `/private/tmp/maichat-context-compaction-v7d-20261110/`：最终源码清单、delta、构建、原日志、两探针及 SHA 回执。

| 同例 | RED | v7d GREEN |
| --- | --- | --- |
| 20 个连续工具 + 原图，普通 prepare | v7b 同探针 exit 1；prepare 0 / summary 0 / checkpoint 0 / 源不变 1 | 同一个 probe.o 重链：exit 0；prepare 1 / summary 4 / checkpoint 1 / 原图保留 1 / 源不变 1 |
| 新正式 Compactor 图片用例，单图和双图 | v7b `red-unfenced-v7b.log` 真实 6 FAIL | SQLite 创建 / reopen 复用 / 同样大第二批均通过 |
| 空历史 + 40000 字节系统指令 | 原 v7b 同对象返回明确错误；94e 同对象 SIGSEGV、exit -11 | 同一个 probe.o 重链：exit 0；InvalidInput / summary 0 |
| 两种 HTTP wire 的图片观察 | 94e 原测试各 1 个旧文案断言 FAIL，共 2 FAIL | v7d 两 wire 的真实来源 IDs、完整 call/result 和原 PNG 字节全部通过 |

`exact-unfenced-receipt-v7d.json` 和 `exact-empty-history-receipt-v7d.json` 记录重用探针对象 SHA、链接参数、原 RED SHA、新 archive / binary SHA 和实际进程 exit。原 RED 日志及回执未覆盖；空历史原 RED 日志为空是因进程在输出前崩溃，SIGSEGV 见原 receipt。

真实 executeTools 单图和双图各执行两批同样大的 20 调用，无 Text fence；各有 `main_requests=3`、`summary_requests=8`，最终主请求继续完成。
测试读取 `request.messages[].images[]` 指向的原始 70 字节合法 PNG，并与原字节相等；HTTP wire 测试继续独立解码媒体 data URL 与源 PNG 比较。
大索引另以 50 工具回归：提示使用有界精确范围，普通历史分页可发现每一个原始工具输出，不依赖猜 part ID。
既有追加图片后检查点复用、有效原前缀身份、失败 / 取消不推进、非法 UTF-8、旧链接精确回查、主输出 0 哨兵等断言没有放宽。

## 正式门禁和验证边界

v7d 官方 targets 构建 exit 0（14.57 秒）。冻结版本运行：

```sh
ctest --test-dir /private/tmp/maichat-pdf-core-gate \
  -R '^Mai(ContextCompactor|ContextBuilderImage|TurnRunner|ModelClient)Tests$' \
  --output-on-failure --timeout 180 -j 2
```

4/4 PASS，exit 0，总时间 72.96 秒：TurnRunner 47.05、Compactor 2.50、Builder 0.32、ModelClient 72.95 秒。
原 stdout、CTest LastTest 和回执为 `frozen-core-gate-v7d.log`、`frozen-last-test-v7d.log`、`frozen-core-gate-v7d-receipt.json`。
此前 v7c 实际 3/4 的失败记录明确保留，不冒充 v7d 绿色；v6 / v7b 的 13 组和消费者结果也没有改标版本。

验证使用 FakeModel、本地 HTTP loopback、临时 SQLite 和测试 PNG，未运行真实模型或付费供应商、读取用户数据库、修改线上服务或替换现装产品。
本记录验证 Mac arm64 Core 与实际 HTTP 媒体序列化；Mac / iOS / Windows 宿主消费者由协调端分别构建并提供对应版本回执。
摘要语义质量和真实供应商时延仍需对应产品验收；本测试证明原史保存、增量检查点、预算分支、精确回查和原图输入的可执行链路。

## 协调端最终宿主消费者回执

2026-11-10，协调端在同一 `da292832…37a6b6` 完整 31 源冻结上，沿原正常缓存构建 Mac App exit 0（5.732 秒）与 unsigned arm64 iOS Simulator Release exit 0（15.805 秒）。两个日志均明确重新编译 `MaiContextCompactor.cpp`，前后逐项 31 源 SHA 未变。

- Mac 实际构建版本 2.2.54，exe SHA256 `26e0115b76225d32062cf865d413be15eaf1352ed010730ed962493370ed6675`。
- iOS Simulator Release 实际版本 2.2.4（203），exe SHA256 `92278a6f716c0107d19620f7a14307fc7f95a3084d54171f40c38f8c9947ca99`；`CODE_SIGNING_ALLOWED=NO`，没有安装到模拟器或手机。
- 两个实际 exe 均读回 partial checkpoint / raw byte history / group observation 等 8 个功能标记。
- Mac 普通正常缓存 App 使用私有 `CFFIXED_USER_HOME`、offscreen，sandbox 拒绝网络及真实 AppData；自有 PID 45851，`--smoke` exit 0（3.222 秒）。这是启动/退出门禁，不是实际 GUI、模型质量、供应商速度、硬件或新包验收。

原命令、编译日志、实际二进制身份与 smoke 原 log / JSON 均在协调端 `context-compaction-final-v7d-proof.zip` 中；旧 v6 / 918 / v7b / v7c 原件分别保全，不冒称最终源。正常打包及版本冻结属于随后获授权的独立交付步骤，本段不把 2.2.54 构建冒称新版本安装包。
