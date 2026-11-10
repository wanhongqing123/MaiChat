# 会话模型、推理等级与供应商原生搜索

日期：2026-11-10。Mac 工作区起点为 `0801974d58e5955af0744a4a00b2718e8c65eed4`，叠加本轮精确供件；未单独提交。Desktop/Android/共享 CMake 由 house-maichat 持有，Core/Store/Mobile/iOS 由本机持有。最终源码清单、构建与日志摘要见本机 `/private/tmp/maichat-session-models-20261110/session-models-final-mac-ios-receipt.json`；该文件和必要原件已按普通文件交接，不使用 ZIP 或分片。

## 操作与实现

创建 A、B 两个会话，分别选择 GLM-5.3/low 和 DeepSeek-V4-Pro/max；切换、并行发送、忙碌时尝试修改、重启并再次发送。A、B 保留各自配置；修改忙碌目标原子失败，其他会话不受影响。新/未设模型会话默认 `deepseek-flash`，已有非空型号不被全局默认或 send/enqueue 覆盖。

四个型号为 `glm-5.3`、`glm-5.3-flash`、`deepseek-flash`、`deepseek-v4-pro`。GLM 主聊天使用 Chat Completions，DeepSeek 使用 Responses；普通多路由客户端按确切型号选 wire 和当前账号供应商凭据，不在 GLM 失败时回落 DeepSeek。两个 Flash 支持图片，两个非 Flash 拒绝新的图片输入；校验读取实际目标会话，而非界面标签。

SQLite v10 只追加 `reasoning_effort`，不重写原消息、工具或检查点。effort 支持 low/high/max；空串继承宿主默认，更新中的 nullopt 表示不修改。型号和 effort 先共同校验再写入，目标忙碌或任一字段非法均不写入半份配置。TurnRunner 开始时冻结本轮型号与 effort，后续工具使用 `MaiToolContext.model`，不读取会在菜单切换后变化的全局选择。

GLM 工具跟进保留原始 `reasoning_content`，独立于可见 content，并保持 call/result 归属。压缩请求继承当前轮显式 effort。`web_search`/`web_fetch` 按工具轮次模型复用供应商普通代码入口：GLM web_search/reader，DeepSeek tools/search/tools/fetch。响应保留独立字节/结果数上限；GLM 的 count 是请求偏好，不能用作供应商必定兑现的协议断言，双 filter 返回更多有效结果已覆盖。

Mobile 保留一个多供应商 Agent 和每会话配置，普通 JSON C ABI 新增 set_model/get_session 语义，未新增 JNI 函数。可信宿主才可 lazy prepare 本地权重；viewer 不传路径、Key 或 endpoint。远端先 get_session 再以完整 owner slot/login/session/requestId 验 ACK；传输 ACK 不代表配置保存，错回执不消费正确待确认项。远端 local 可读、可切 cloud，但供应商 effort 禁用。

## Mac Core 与原生 C ABI

使用普通 Release Core cache 与正常 iOS arm64 Simulator cache、相同工具链/flags。全部数据为独立临时库和 loopback/Fake 供应商，无真实 Key 或付费调用。

- 原 effort 矩阵 27 FAIL、Store 7 FAIL 作为 RED 保留；相关原例修复后通过。GLM reasoning、两轮工具跟进、provider 隔离、原史与 effort 压缩均有定点 RED/GREEN。
- 第一批 Core registered 12 组为 11 PASS/1 FAIL，唯一新 GLM count 断言失败；修正后受影响完整 Web 组 PASS（18.31s）。ToolLoop typed provenance 和完整 Compactor 原组均 exit0。不能将这些不同 cohort 写成同次 12/12。
- GLM 改用 Chat 后暴露旧 Chat client 完全不消费 proxyResolver。Mac 原 Desktop proxy 槽失败，私有只读探针显示 callback=0、proxy requests=0、curl 无法解析 model.test。此前将失败归因 NO_PROXY 的简报已撤回。共享 Chat 代理原 24 FAIL → exit0，完整 MaiModelClientTests exit0；两 wire 同核逐跳代理、direct 环境覆盖、跨 origin 去 Key、总 deadline、取消、重定向上限与 TLS 降级拒绝。
- 原 Mobile C ABI 序列 configure A → configure 默认 B → send A 实际 RED：旧库改写 A 的型号。新 native fixture exit0，四个真实型号分别发出实际 Fake HTTP 请求，含 GLM low、队列、忙碌隔离、非法更新原子拒绝、图片能力、重开持久化。
- 额外旧 MobileSteering 原例 exit1：旧 SSE 在 21 条新输入到达后才计划 question，Core 正确把它记为 Canceled；base HEAD 已含此逻辑，本轮未改该取消分支。保留原失败与实际 part/error。test-only 修正保留原页外 live tail，核旧计划取消，再让下一次真实模型请求读完补充后重提问题，问题 Running 时再移出最新页，核稳定 turn、实际 continuation messageId 与完成点取。新原组 exit0，原等待预算不变。
- Windows 由协调端完成最新普通 Core 13/13、console e2e、App/Android native 必要重链；本机不把其结果冒充 Mac 或设备实测。

## Mac Desktop

正常 Qt 6.12 cache 编译 maichat、Controller、Panel、Host、MainWindow 目标均 exit0。

- 最新 Controller 46/0（含 Mac 代理槽），Panel 原完整组串行 80/0，原音频 3s 断言未改。初轮并发 Panel 曾在该槽超时，不能据此推定产品根因。
- Host 原完整组 86/0/1；唯一 skip 是原 opt-in temporary-image TLS。最后 Local capability 增量只重跑对应三槽，含 hooks 为 5/0。MainWindow 相关 15/0 未变输入复用。
- 首轮 Controller/Panel full txt 被旧 wrapper 重名覆盖，只剩当时实际读取的 observed excerpts/总计，不能声称完整旧 txt 可取。随后改唯一 run 目录，以上最新原 txt/receipt 均保全。
- 实际 Cocoa dev App 私有 CFFIXED_USER_HOME、仅 loopback、--smoke --debug：PID56814、exit0，exe SHA256 `d215b790a0f29b5248f8ad57696337adb64e7625b9f196893a47b32a7c9e07c2`。
- 同正常 Panel test target 的真实 Cocoa QWidget 菜单槽 3/0，实际 grab 图已亲看四型号与独立思考组。只抓自有控件，不截用户桌面；不是完整 staging 或用户登录会话验收。

## iOS 最终构建与边界

平台：macOS 27.2 arm64；iOS Simulator 26.5；SDK 27.1。正常 App 版本实际为 2.2.4/build203，未改工程版本、签名设置或安装真机。Debug 构建 exe 与 Simulator 实际安装的专用测试 App SHA 相同：`cc2cf7dd08b02d4c34785f8e291854aec8242105e23dbd9e793b26bd4f205982`。

最终同 cohort SessionModelsUITests 为 3/3、0 FAIL，76.474s：

1. 正常 Root 中 A/B 选择、effort、实际两供应商 loopback HTTP、草稿保留、切换和重启；不是仅检查 label。
2. 正常横屏菜单限高滚动、最后型号、底部清空入口的真实点击与确认展示；未执行清空。全屏 XCUIScreen 原 PNG 已查看。旧 fixedSize 导致内容越过材质边界，现测量内容并限制 ScrollView 自身 viewport/clip。早期 app.screenshot 旋转裁切异常原图保留，不作为最后全屏验收。
3. 通过正常 Model/native API 与自有 peer I/O 验普通无配置 history → get_session → 严格 ACK；错 tuple/ID/model/effort、迟到拒绝、重连读回/不重放、publisher 配置与 digest、remote Local capability、恢复 local 失败隔离、blank 准备取消与草稿/附件迁移。

P1 原例真实 reads=0 RED 保留。恢复 local 的首 fixture 恰好重选当前页而早返回，不能当作该 catch 的决定性 RED；纠正为先选 B 后恢复 A，并要求 prepare=1 后，仅恢复原无 guard catch 即真实 RED：selectedB=true，但 error 被 A 覆盖。blank 原 create observer 在切走后仍被调用的 RED 独立保留；修正后 create 前核 token/页面/代次，effort 在点击时捕获。

Model 层协议场景直接种存储草稿；若同时挂 editor，editor 自己的 local text 会成为第二写者。这一旧 fixture 干扰已记录，最后 Debug 宿主只在该场景运行时不挂 editor、暂停轮询，完成后再挂正常 Root。它不是远端真人手势或 live WS 验收；真正输入/选择和横屏交互由前两项正常 UI 用例覆盖。没有删除相关断言来过关。

最终普通 unsigned Simulator Release 编译/链接 exit0；exe SHA256 `aaa59e9dd1bff48de30a9bd6378dfa6bd4f1b4035e5bd177b7e5022875a4ae36`。核特定 Debug peer/native 测试符号与自有 fixture 文本不在 Release exe 中。

本轮没有实际手机安装、真实跨设备 WebSocket owner 操作、付费供应商调用、Android 设备/APK 安装或新包/Release/服务部署。Android Java 61/61 与 arm64 native 链接由协调端验收并按 14 项 canonical 核合，不把它写成设备通过。此前 2.2.55 候选包不包含本轮变化。

官方依据：
- https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3
- https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash
- https://api-docs.deepseek.com/api/list-models/
- https://docs.bigmodel.cn/api-reference/工具-api/网络搜索
- https://docs.bigmodel.cn/api-reference/工具-api/网页阅读
