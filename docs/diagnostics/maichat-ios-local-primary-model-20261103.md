# iOS 本地 GGUF 作为主模型

基线：main checkpoint `5216279a`（178），随后本轮 179 协作工作区。此前的“本地模型”页仅为独立运行库自测；该页选择不会改变聊天主模型，178 的 TinyStories 自测通过不能作为本功能验收。

## 可复现操作和预期

1. 正常 Root 进入创作中心→右上角“更多”→“主模型”。一级只有“云端模型”和“本地模型”两个来源；选中后点“使用此模型”，不要求选择厂商、文件名单、协议或运行参数。云端沿用账号当前云模型，本地沿用已选文件或已导入的 Qwen。
2. 本机没有模型时显示“添加本地模型”；按需打开独立“本机模型”管理页，通过“添加 GGUF 文件”复用 `AILocalModelStore.importModel`，在后台复制、校验并记录 SHA。导入完成自动回到主模型页并选中新文件，待用户应用。当前只接受已核验的官方 Qwen2.5 Instruct 原生工具模板，摘要由共享核心精确校验，量化方式可变；其他模板明确 NotSupported。TinyStories 仅用于管理页内的独立自测，不作为聊天主模型选项。
3. 主路径只有模型名称选择、应用和必要的兼容说明，不显示 token、线程、文件哈希或大段专业配置。只有 native 配置和账号 settings 原子保存成功才关闭页面。忙碌或文件不可用时保留旧主模型与选择；没有云端 Key 时仍可应用有效的本地模型。
4. 使用同一个聊天输入框、发送队列、native send/steer 及 MaiAgent 请求，得到真实本地多轮文本/工具结果。当前主模型不支持图片/mmproj；不会静默丢图。独立自测仍保留原单图能力。
5. 重启应用和重新进入主模型页，恢复账号保存的同一模型 ID 与参数。模型文件缺失时显示可恢复错误并允许重新导入/改选，不静默转为云端模型。

## 配置与所有权

- `AIModelSettings` 增加可选 `modelProvider`、`cloudModel`、`localModelID`、`localProjectorID`、`localContextSize`、`localMaximumTokens`、`localThreads`，兼容旧 JSON；继续按账号工作区保存 `settings.json`。`cloudModel` 记住切换本地前的 DeepSeek/GLM 选择，切回云端时恢复，普通 UI 不再让用户选择厂商。不保存沙盒绝对路径或 Key。
- 普通主模型内部使用 `32768 / 自动输出 / 2`，用户无需设置。输出传 `0`，由核心根据真实剩余上下文生成到模型结束，不沿用旧 QA 的 128/512 截止值。旧可选参数字段仍能解码，但不再控制常规主模型；独立运行库自测的参数和 CPU 契约保持原有独立行为。GGUF 容器校验成功不代表模型架构、模板和设备内存适配成功。
- 新 Store `resolve(modelID:projectorID:)` 按明确 ID 解析主模型，不读取/修改独立自测的全局选择。对应真实文件测试断言：自测当前选择第二份模型时，原第一份主模型仍解析为同一文件且字节不变，当前自测选择也不被反向修改。
- C ABI configure：`modelProvider="llama"`（云为 `"remote"`）、`wire="local"`、稳定 `model="local:<StoreID>"`、即时解析的 `localModelPath`、空 `localProjectorPath`、JSON 整数 `localContextSize/localMaximumTokens/localThreads`。非空 projector ID 明确拒绝。运行库和工具契约由共享 C++ client 实现，Swift 不拼 Agent 历史/工具模板或伪造云端请求。
- 启动保留已存 GLM/本地选择，删除强制回 DeepSeek 的行为。本地初始化不等账号主模型 Key、Seedance/Bailian 凭据或 HTTPS CA；poll 刷新也不主动领这些云凭据。切换云模型时沿用原账号内存 Key 路径。
- `open/save` 事务门禁覆盖文件解析与领 Key 的 await；native 任一会话忙碌时仍为最终拒切权威。成功 configure 更换 native epoch 后更新现有 outbox 重试门禁，unknown 保留原请求绑定，不自动换模型重播。

## 真实权重 UI QA 设计与边界

- `PrimaryModelsUITests/testImportedLocalPrimaryModelChatsTwiceAndRestoresAfterRelaunch` 使用正常 Root、真实 backend.open/configure/send 和 C ABI/MaiAgent/runtime。仅在 Debug 模拟器显式 `--ai-ui-test --ai-session-reveal-ui-test --ai-primary-model-ui-test` 时允许环境提供公共 GGUF 路径；导入按钮跳过系统文件选择结果，随后仍调用普通 Store 的实际复制/哈希导入。
- 输入通过 `MAICHAT_PRIMARY_MODEL_FIXTURE_PATH` 显式提供，缺权重时测试跳过，不能记为推理通过。候选公共文件为官方 Qwen2.5-0.5B-Instruct Q4_K_M，固定权重和 SHA 由主任务的权重回执记录。最新 UI QA 不操作专业参数，第一条为简短英文，第二条询问上一轮内容，使用与普通用户相同的自动运行配置。
- QA 状态只观察实际 native snapshot 的 `model/wire`、configured、busy 和真实已持久化 assistant 非空文本数量；没有安装生成 handler，没有伪造回复或预置聊天历史。测试还要求正常聊天消息区出现复制回复入口，并重启检查同一原会话与当前 Qwen 选择。
- 这条 QA 的文件路径由测试预置，不等于系统“文件”选择器已验收。当前只完成源码和 `swiftc -frontend -parse`、定点差异检查；尚未由本子任务构建、安装或实际运行 179 App。真实本地主模型质量、工具调用、流式取消、内存和 UI 回执由主任务后续统一验证。

## 冻结后独立检查

- 已将冻结后的实际 Store 与实际 `AILocalModelStoreTests.swift` 复制到原独立 Mac Foundation harness 后重新运行，`7 tests / 0 failures`，包括新增明确模型 ID 与自测选择隔离断言。实际运行时间 `2026-11-03 23:51:47`，日志 `/private/tmp/maichat-local-model-store-tests-primary179-20261103.log`；未运行 App、推理引擎或网络。
- 新主模型 View、从当前源码原样抽出的 `AIModelSettings`、实际旧自测 View、实际 `AILocalInferenceRuntime` 和 C ABI 头，在 iOS 16 arm64 device/simulator 目标独立 typecheck 均 exit 0，无 warning/error。实际 Core Store 也对两目标 emit module 成功。回执 `/private/tmp/maichat-primary-model-ui-typecheck-20261103/results.json`。该 harness 只给 `AIAssistantModel` 提供最小宿主接口 stub，没有执行其 backend/lifecycle/save；不能将此项写为完整 App 编译或 UI 通过。
- 默认 8192 的边界只做源码核对：`MaiTurnRunner::buildRequest` 首次请求仍附完整 mobile 工具 specs，local 模板输出逐段按实际模型 tokenizer 计数，`promptTokens + maximumTokens > contextSize` 明确报出所需预算，绝不截断。尚未实测完整 mobile 首次请求的 token 数，因此没有声称 8192 足够或一定超限；仅含单个 file_read 工具的核心用例不能替代完整工具集证据。真实 32k UI QA 的首次请求由主任务测量。

## 179 首次正常 UI 红证据与定点修正

- `2026-11-04` 首次真实 PrimaryModels UI case 在 `20.822` 秒、测试第 43 行点击 `32768 token` 时失败。回执 `/private/tmp/maichat-primary179-main-ui-20261104.xcresult` 和同名 `.log`；日志 `107–137` 行完整 AX 已显示实际菜单选项 `32,768 token`，参数控件已经是 Picker。CPU 线程保留的 Stepper 与此次失败无关。
- 已实际查看 `/private/tmp/maichat-primary179-picker-red-20261104.png`，能看见真实导入的 Qwen GGUF、上下文 `8,192 token` 与生成上限 `512 token`。根因是 SwiftUI `Text` 的整数插值按区域设置插入千分位，测试按无分隔符文字查找失败；不能把该结果宣称为主模型生成失败或已通过。
- 参数选项增加稳定 AX ID `primary-context-32768` / `primary-maximum-tokens-128`，测试继续实际打开 Picker 并选择选项，不直接改配置。合法的已存非标准上下文/生成长度也作为额外选项保留，恢复时不静默换成常用档位。
- 主模型页导入按钮和说明同步收口为“当前已验证 Qwen2.5 Instruct 的原生工具模板；其他模板暂不支持”。普通独立自测保留原能力，TinyStories 仍排除在主模型选项外。修正后的实际 UI 回归待主任务重新构建运行。

## 179 第二次正常 UI 红证据与 fixture 初始化

- 第二次实际 case 已通过真实导入和菜单选择 `32,768 / 128`，但应用后在 `57.839` 秒失败。回执 `/private/tmp/maichat-primary179-main-ui-v2-20261104.log`、同名 `.xcresult`/`.mp4`；已实际查看 `/private/tmp/maichat-primary179-apply-red-20261104.png` 和导出 AX `/private/tmp/maichat-primary179-main-ui-v2-shots-20261104/5AE1CCBC-FA8B-470F-8786-14F53E246050.txt`。主模型页和底层均显示“创作中心尚未准备好”，观测状态仍 remote，不能记为本地主模型生成失败。
- 调用链证据：`--ai-session-reveal-ui-test` 被 `AIAssistantUITestRouting.usesStandaloneFixture` 识别；`AIAssistantView` 的 onAppear 因此跳过真实 `model.appear()`。Root 初始已经选择创作中心，测试也未经过 `Root.openAssistant()` 的点击准备链。新的 Primary fixture 分支虽然移除了假历史安装，却漏掉了真实准备入口，所以 `backend.open` 未发生、`backend.root` 为 nil。
- 定点修复仅在显式 Primary Debug 模拟器 fixture 设置测试账号后调用真实 `AIAssistantModel.appear()`，进入原 `prepareFirstPage→backend.open/configure` 与实际 snapshot 轮询。其他 fixture 行为、产品保存门禁和整条真实导入/两轮生成/重启断言不变；没有安装假 handler 或预置回复。修正后 parse 和差异检查通过，仍须主任务重建并实际回归。

## 完整 mobile 工具目录预算与默认上下文

- 主任务的真实 C ABI 捕获与 Qwen vocabulary/tokenizer probe 保存于 `/private/tmp/maichat-mobile-tool-budget-20261104/budget.json`。119 个真实工具、specs 73415 bytes，经 native 工具模板与 runtime 相同 tokenize 规则后，首次短问 23457 prompt token，加默认输出预算 512 共 23969。该证据直接证明 8192 不足，32768 可容纳这次基线请求；没有裁剪工具 Schema 或历史来使测试通过。
- 此 probe 未加载权重、未生成；它不包含可选 RVM/LaMa 和 iOS 27 截屏工具，是 mobile 配置基线下界，不是全部真机工具集验收。更长历史、附加工具和输出仍可能超过 32k，运行库继续明确报预算错误。
- 根据实际证据，iOS 主模型未设置上下文时的默认改为 32768，UI 说明完整工具目录与系统说明约需两万余 token。显式保存的较小/非标准用户值不被改写，UI QA 仍通过真实 Picker 选择 32768。普通独立运行库 API 的原默认不变。

## 179 第三次正常 UI 红证据与 App 沙盒边界

- 第三次实际 case 已完成真实 backend 初始化，在应用模型时收到 native 明确错误 `Local model file is outside the application root.`，case `59.273` 秒失败。原回执 `/private/tmp/maichat-primary179-main-ui-v3-20261104.log` 和同名 `.xcresult`；导出 AX `/private/tmp/maichat-primary179-main-ui-v3-shots-20261104/0DAD85E3-1306-4805-90C9-639F1D8AADF6.txt:82` 明确记录了该配置错误。
- 根因是 QA 路径边界与真实产品不一致：既有 Core `AIAssistantPathPolicy.appRoot` 对所有 `--ai-ui-test` 返回临时工作区的两级父目录，也就是 App/tmp；普通 Store 的合法已导入 GGUF 位于同 App 的 Documents。真实产品使用 `NSHomeDirectory()` 覆盖整个本 App 沙盒，不存在这个 tmp-only fixture 问题。
- 定点修正仅将显式 `--ai-primary-model-ui-test` 排除在 legacy tmp-root 特例之外，使 Primary QA 使用和产品相同的 `NSHomeDirectory()`。其他旧合成 fixture 仍保留原边界。native 越界校验、源文件导入、真实模型调用与测试断言均不变，没有使用根目录 `/` 或导入源目录授权。源 parse 和差异检查通过，实际 UI 回归待主任务下一冻结构建。
- 另将仓库现有 `AIAssistantPathPolicyTests` 两个测试类方法原样抽入独立 Foundation harness，实际跑过同 App sibling 可访问、其他容器/符号链接逃逸拒绝、媒体引用边界，`2 tests / 0 failures`。日志 `/private/tmp/maichat-primary179-path-policy-20261104.log`。实际 Core Policy 对 iOS 16 arm64 device/simulator 独立 typecheck 通过；Mac 测试不会执行 `targetEnvironment(simulator)` 分支，Primary 参数组合的实际运行验证仍由下一正常 App 回归完成。

## v4 真实预填充等待与选择页收口

- v4 的正常 Root 已真实配置为本地模型，并准确发送第一条短英文请求；`/private/tmp/maichat-primary179-main-ui-v4-20261104.log` 的 180 秒非空回复等待仍超时。录屏阶段图 `/private/tmp/maichat-primary179-v4-ui-stages-20261104.png` 和等待图 `/private/tmp/maichat-primary179-v4-ui-wait-20261104.png` 已实际查看，能看见应用成功退回聊天、用户气泡和持续“正在思考…”，没有误触或异常遮罩，也没有已完成回复。sample `/private/tmp/maichat-primary179-prefill-v4-20261104.sample` 显示 mai-turn 在线程中的 `llama_decode→ggml_compute_forward_flash_attn_ext`，是 CPU 预填充计算证据；不据此宣称推理通过。
- 用户随后明确要求和云模型相同的选择体验，并拒绝人为输出上限，最后进一步明确一级只能有“云端模型/本地模型”。最终主页面只保留这两个来源和应用按钮；厂商沿用账号已选值，本地文件选择、导入和自测放独立管理页，完全移除主路径上的参数、文件名单、哈希与大段技术说明。旧自测仍独立，不把 Tiny 包装成正式主模型。
- Primary UI 回归同步移除手选 32k/128 的操作，保留真实文件导入、实际应用、两轮非空回复、native local ID/wire 与重启恢复断言；等待时间和业务断言没有放宽。此次 UI 收口已做源 parse/差异检查，实际新画面及自动输出/硬件执行验证仍由主任务统一构建运行。
- 最终测试真实点击“本地模型”后进入管理导入，结束回到两项主选择页应用；重启额外核对与退出前完全相同的 native model ID。主路径不再读取文件名作为来源断言，界面只显示“当前：本地模型”。
- 最终两项来源界面与实际 Settings 已在 iOS 16 arm64 device/simulator 独立 typecheck 通过，回执 `/private/tmp/maichat-primary-model-ui-typecheck-20261103/results-two-source-final-20261104.json`。仍只给宿主 Model 提供接口 stub，不替代完整 App 验收。另运行当前源码抽取的实际 Settings：旧 8192/128/8 字段可解码但常规配置为 32768/自动输出 0/内部线程 2，GLM 的云端选择跨本地/云端及 JSON 往返保持；日志 `/private/tmp/maichat-primary179-automatic-settings-20261104.log`。

## 真实执行设备与成功判据

- C ABI snapshot 的可选 `localGpuSelected/localGpuModelBytes/localGpuBackend` 接入 Model 的实际状态字段；这些值来自运行库实际选择和驻留事实，不能由设置偏好推断。主选择页面不展示这些诊断参数。
- 显式 Primary QA 状态输出 GPU 是否实际采用、GPU 模型字节数、实际 backend 与无错误布尔值。两轮完成断言都要求实际 GPU 已采用、模型 GPU 字节大于零、backend 非空、busy 已结束且当前 UI/native 均无错误，避免把截断的部分文本加失败后 idle 误判为成功。180 秒等待未加长。
- 重启恢复阶段尚未执行新一轮推理，允许 GPU 未加载、字节为零，但仍要求同一个实际 native model ID、local wire、无错误及两轮已持久化非空历史。最新 App 的自动输出、Metal 执行与 UI 成功状态仍待主任务统一构建运行，不以核心单独基准代替实际 App 回执。

## 独立自测嵌套 sheet 的测试容器红证据

- `/private/tmp/maichat-primary179-composer-portrait-regression-20261104.log` 中，旧独立自测已经打开实际 Picker 并选中 Tiny；随后测试遍历到三个 CollectionView，但都因容器 `isHittable` 判定失败而回退到不存在的 Table，测试在 `22.250` 秒、第 45 行 swipe Table 失败。这是多层 Form/sheet 的 AX 容器选择错误，尚未开始本地生成。
- 只把该测试的滚动容器改为包含 `local-model-picker` 按钮的 CollectionView，保留原真实生成、非空 token、卸载与退出全部断言；不更改产品 Form 或推理逻辑。实际复测待 Root。该批其他美颜、相机和输入行为用例的结果由各自回执记录。
