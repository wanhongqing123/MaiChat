# iOS 本地推理普通入口与共享 C ABI

日期：2026-11-03。开发基线 `83b76394b6a1a5e537972807dbd64b0f7f10b804`，
最终修改以本记录所在提交为准；实际构建及源摘要见下列回执。

## 可复现操作与预期

普通调用在 iOS 工作队列创建一次性推理 job，传入用户已授权的本地模型绝对路径、提示词，
可选同样授权的单张图片及 projector；调用共享 `MaiLlamaRuntime::generate`，不调用 Agent
工具 JSON、不启动子进程、不下载权重。Agent 的 `llama` 工具由共享核心注册并调用同一
`getSharedMaiLlamaRuntime()` 实例。本批不新增创作中心卡片或模型下载界面。

最小验收不需要大模型：使用不存在的模型路径，必须返回明确 `not_found`，调用进程存活，
随后另一请求仍可执行。取消后返回 `canceled`；同一 job 再次生成返回 `busy`；job 释放后
旧 ID 不可复用。跨线程取消/释放不能使运行中的取消标志悬空。成功输出必须带真实的 token
计数、结束原因和截断标志，失败不返回半截成功文本。CPU 以外的加速及真实 OCR 权重
精度/吞吐均不在本次已验证范围。

## 实现与验证边界

- 普通 C ABI：`MaiChat/MobileAgentAdapter/MaiLocalInference.h/.cpp`。
  明确 C 字段承载 UTF-8 字节及长度，不暴露 C++ 类型。job 为不复用的整数 ID，注册表持有
  共享状态；run 借用状态直到同步调用完成，destroy 移除 ID 并合作式取消，不杀线程。
- Swift：独立 `Assistant/AILocalInferenceRuntime.swift`。actor 管理普通调用，阻塞生成只在
  专用串行队列执行；Task 取消通知 C job，输出返回后释放结果和 job。
- C 字段明确传入 UTF-8 字节与长度，拒绝内嵌 NUL、无效 Unicode、相对路径和单图/projector
  不配对。数值/模型/图片上限仍由共用 runtime 唯一校验。图片需是调用方已按显示方向
  规范化的本地静态位图，不宣称自动处理 HEIC/EXIF。
- `mai_local_inference_status` / Swift `status()` 直接查询同一个共享 runtime；不等待生成锁，
  只返回 busy、驻留模型/projector、context 与 modelGeneration，无路径/提示词。
- `mai_local_inference_unload` / Swift `unload()` 显式按需释放 projector/context/model。
  调用者先取消自己的 job/Task 并等待生成返回；unload 不取消其他宿主的生成，必要时等待其
  完成。Swift 的等待只在同一专用 serial queue 内执行，不阻塞 actor/MainActor。
  status 仍可快速查询；卸载不伪增/倒退 modelGeneration。没有增加无条件内存清理观察器。
- 工具注册、上游 runtime 与构建选项由本批其他负责者维护。这里不声称上游完整 CLI 已安全
  嵌入，普通入口直接调用 typed runtime；也不把头文件存在或无模型错误当成真实权重推理。

### 已实际执行的边界验证

1. `MaiLocalInferenceLifecycleTests.cpp` 用确定性 gate 假 runtime 验证真正 C ABI 对象生命周期：
   success → engine failure → exception → success；返回真实 metadata/截断原因；同 job 重复 run；
   取消先于 run；40 次运行中 cancel/destroy 重叠及状态查询；销毁 ID 永不复用；64 个存活 job
   上限及释放后的容量恢复；非法 UTF-8/NUL/相对路径不进入 runtime；底层原始错误不进入结果。
   假推理器仅链接进此独立测试，**不是权重/解码能力验证**。
2. macOS arm64 独立编译并实际运行 `-fsanitize=address,undefined` 和 `-fsanitize=thread`
   两种版本，均 exit 0，无 sanitizer 报告。日志：
   `/private/tmp/maichat-local-inference-bridge-20261103/lifecycle-asan.log`、`lifecycle-tsan.log`。
   编译只使用 bridge、该测试及正式 MaiError/MaiFilePath/MaiFileSystemPosix/MaiBlockingCheck，
   没有更改或重建 App/移动端 vendor 缓存。
3. wrapper 使用实际 C header 导入，以 `swiftc -typecheck -swift-version 6 -import-objc-header`
   完整类型检查通过；fixture/UI 测试通过 Swift parse；C header 通过 C11 语法检查；
   bridge `-Wall -Wextra -Werror -fsyntax-only` 通过。parse 不替代完整 iOS 类型编译。
4. 新增 unload 后只跑定点 fake-runtime 分支 `--unload-only`，exit 0：确认它已进入 runtime
   但仍等待另一个生成，未偷取消该 job；测试显式取消自己持有的 job 后卸载完成，重复卸载
   幂等、状态清空且代际不变。日志 `.../maichat-local-inference-bridge-20261103/unload-boundary.log`。
   job 注册表未改，不重复先前已通过的全组 sanitizer；此新增观察不冒充真正权重释放测试。

### 真实引擎 C ABI 与 iOS 验证

- `MaiLocalInferenceTests` 链接正式 runtime：不存在 GGUF 连续三次返回 `not_found`，失败后
  仍可查询 status，驻留模型/代际不改变，**已实际 exit 0**。只在本子任务临时目录编译 bridge
  和测试并链接 `/private/tmp/maichat-pdf-core-gate/lib/libmaiagent.a`，没有修改该引擎缓存。
  等 Core8 构建完成后再次只重链自己的测试并运行 exit 0；最终引擎 archive SHA-256 为
  `eea2ab9aeba7a9e6727606b89278b497f6fd9a1263379b959707c597d916fe1f`。
  收据 `/private/tmp/maichat-local-inference-bridge-20261103/real-runtime-missing-model.log`，
  同名 `.json` 保存实际命令和最终归档摘要。此项确为真实 runtime，但仍不等于权重推理验证。
  后续真实 C ABI 还验证空状态 unload 连续两次幂等、status empty/context0/generation 不变，
  exit 0。最终与工具 `--unload` 一同完成的 archive SHA-256 为
  `0be1336b0878c2b8453e10394a8f91d017d9870e11711ff6d27b2e938380fdc8`，
  收据 `.../maichat-local-inference-bridge-20261103/real-runtime-unload.log` 与同名 `.json`。
- DEBUG simulator 的 `--ai-ui-test --ai-local-inference-ui-test` 使用独立
  `AILocalInferenceUITestFixture.swift`，保留正常 Root 控件，仅增加验收探针。每次走普通 Swift
  API→C ABI→实际链接引擎，缺模型两次且每次失败后 status 成功；重新点击再完整执行一次。
  `LocalInferenceUITests/testOrdinaryLocalInferenceMissingModelKeepsRuntimeAndAppAlive` 检查结构化
  收据、计数和 App 仍可交互。没有下载或加载真实模型、触发付费请求；fixture 不进入 Release。
- 正向 QA 单独 opt in：runner 环境 `MAICHAT_LOCAL_INFERENCE_TEXT_MODEL_RELATIVE` 只接受精确
  `qa-local-inference/tiny.gguf`；主任务把公开 TinyStories fixture 拷入隔离 App Documents，
  不能通过该环境变量传任意路径或 URL。文件必须为非 symlink 的规范本地路径，大小
  1,185,376 字节且 SHA-256 为
  `270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d`。
  来源固定为 `ggml-org/models` 的 `499bc8821c6b12b4e53c5bffcb21ec206f212d81`
  `tinyllamas/stories260K.gguf`，仅临时 QA 权重，不加入仓库或 App 资源。
  `LocalInferenceUITests/testOrdinaryLocalTextInferenceReusesResidentModel` 经普通 Swift 入口
  连续两次 CPU raw prompt 生成（`Once upon a time`、context 256、max tokens 16、threads 2、
  temperature 0、关闭 chat template），检查各次真实 token 数/非空文本、完成不 busy、
  context 正确及驻留 modelGeneration 不变，再真实 unload 并核对 empty/context0/代际不变。
  没有 opt in 时明确 skip；缺模型测试显式清空
  该变量，不能因同时运行正向 QA 而失去错误路径。收据不记录生成文案或文件路径。
- 主任务随后执行了下面的177完整App与真实小模型验证；真实设备安装、OCR/VLM大权重精度/性能仍未通过验收。

## 独立 macOS normal build smoke

2026-11-03 19:23 CST，正常桌面本地 build bundle 2.2.35：
`/private/tmp/maichat-desktop-agent-window-build-20261102/bin/MaiChat.app`。
按既有打包脚本同款 `codesign --force --deep --sign -` 仅重签该隔离 bundle，随后
`codesign --verify --deep --strict` 通过。签后主 executable SHA-256：
`beb60c3664511a092c6d03fd349322f1101c35ed8f3a12fed6818c84a65ef2af`。

直接运行该 executable 的 `--smoke`，Cocoa 原生登录测试窗口自行退出：exit 0，3.496 秒，
测试 PID 50465。源码该分支不调用 `LoginDialog.start/restore`，不进入登录后的 IM、
会话同步及模型凭证请求；正常本机 AppLog 写入独立 PID 日志。用户安装 App
`/Applications/MaiChat.app` 的 PID 43461 前后不变，没有停止、替换或登录用户账号。
日志与机器可读收据分别为 `/private/tmp/maichat-desktop235-smoke-20261103.log`、同名 `.json`。
此 bundle 仍依赖 Homebrew Qt，**不是最终可分发包验收**，也不是完整聊天/播放器实测。

## 后续 normal 2.2.36 完整链接与 smoke

主任务增加共享 llama builtin 工具与按需卸载后，使用同一个隔离 Desktop cache，
`cmake --build ... --target maichat --parallel 2` 构建正常 App exit 0；工具 `--unload`
后续落地后必要增量再链接 exit 0，核对最终 `MaiLlamaTools.cpp` / `MaiTool.cpp` 对象时间不早于源。
首次构建包含 Apple file-scope OBJC/OBJCXX 和真实 llama/mtmd/ggml/Metal 依赖，不是只编译测试桩。
构建日志 `/private/tmp/maichat-desktop236-normal-build-20261103.log`、
`/private/tmp/maichat-desktop236-unload-relink-20261103.log`。

`codesign --deep --force` 初次重签已有效的 ONNX dylib 返回 internal Code Signing subsystem，
保留原错误收据；该嵌套 dylib 实际 strict 验证有效。逐一验证自己 bundle 的嵌套组件、
仅重签无效组件，再签外层后 `codesign --verify --deep --strict` 通过，没有改动 SDK 源库
或扩大权限。此期间主任务另有磁盘满证据并清理其旧 QA staging；不凭 codesign 文案单独
推定根因或当作产品功能失败。签名日志 `/private/tmp/maichat-desktop236-sign-20261103.log`。

2026-11-03 20:05 CST，签后主 executable SHA-256
`0d866784a4d88fcadf7fd4d892c5882df9360825eac6d6fb20895d3631eede48`。
正常 2.2.36 Cocoa `--smoke` exit 0，2.81 秒，测试 PID 65730 自行退出。
用户 `/Applications/MaiChat.app` PID 43461 前后不变，没有替换/退出该 App、登录或发付费请求。
日志 `/private/tmp/maichat-desktop236-smoke-20261103.log`，同名 `.json` 保存本次源文件 SHA、
最终主程序摘要、版本、签名/构建日志与前后 PID。只执行原正常 App 登录 smoke，
没有重复本批已通过的播放器 UI；仍依赖 Homebrew Qt，不能称最终分发包或线上账号验收。


## 主任务最终平台验收

源码主入口为 `MaiLlamaRuntime`，工具名 `llama`，独立 `arguments` 支持 `--status`、
`--unload`；生成参数只允许公开头及 description 中的本地子集。当前推理固定CPU，
Apple Metal后端已从源码编译/链接，但不据此声称GPU推理已验收。模型权重不随App打包，
驻留期间不可原地覆盖；替换前先调用普通unload或工具独立 `--unload`。

- macOS真实引擎：约1.2MB TinyStories（1,185,376字节，SHA-256
  `270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d`）直接/工具生成、
  复用、上下文/文件配置切换、UTF-8输出上限、活动/排队取消、恢复与卸载均通过。
  `/private/tmp/maichat-llama-runtime-validation-20261103/evidence.json` 保存真实模型来源、
  源SHA、各测试与库产物SHA。最终八套核心重新链接，8/8通过，33.42秒；没有源码漂移。
- iOS正常App `2.2.4(177)`完整构建，App/Runner共13个Mach-O逐项签名验证。相同App在
  新的自有iPhone17ProMax/iOS26.5模拟器运行两个独立普通API用例：缺模型后重复调用与
  status可用；实际TinyStories rawprompt、context256、max16、两次真实生成且驻留代际不变，
  再显式unload，确认empty/context0/代际不变。首轮2/2通过、25.808秒。
- 人工看首轮截图发现正常Root首次通知引导遮挡画面，未将遮挡截图当界面验收；仅在UI测试
  点击应用内“稍后”，不授予系统权限。正常App代码和推理核心未改。最终同两项2/2通过，
  17.678秒，`/private/tmp/maichat-local177-ios-unobscured-20261103.xcresult`；截图及同期录像
  已逐段提帧查看，正常Root可见并能再次检查。QA结果是AX元数据，不用固定文本伪造推理。
  已核对App生成token非零、输出非空、重复调用和卸载状态；未把测试权重加入资源或提交。
  模拟器临时Documents中的唯一QA权重经SHA确认后删除。
- iOS真机 `2.2.4(177)`构建成功，11个Mach-O及外层签名逐项验证，冻结到
  `/private/tmp/maichat-local177-device-artifacts-20261103/MaiChat.app`，同目录receipt存校验值。
  实际安装命令失败，CoreDevice **4016**，手机开发连接不能满足trusted connectivity及
  coreDeviceServicesLoaded条件；没有宣称已安装/重开，也未完成真实iPhone峰值内存或速度验收。
- Android `2.2.15(252)`两ABI正常APK构建成功，版本/安装产物SHA与模拟器上的base.apk核对。
  自有5580模拟器执行原PortraitRegionInstrumentedTest，**4/4通过、45.458秒**，保证现有
  JNI/人像入口仍正常。之后冷启普通MainActivity，PID27137，未登录界面截图已人工查看。
  `/private/tmp/maichat-local252-artifacts-20261103/receipt.json`保存App/测试包、实际安装及ELF
  摘要；30个so对应两ABI，许可资源与源码一致。本轮没有Android Java普通推理入口或实际
  GGUF生成的端到端验证，不把共享C++工具注册和交叉编译扩张为此类验收。
- 工作区索引使用现有生成器从Desktop、核心、iOS和Android真实数据库汇总，448个自有源码
  命令，包含新增runtime/bridge。第三方头仍通过实际include解析；本机数据库保持忽略不提交。

最后一轮曾因磁盘满使iOS复制dylib、Android元数据写入和三项Core测试失败。清理本任务
9个过期的App/Runner复制阶段后重跑受影响构建及Core8，全部恢复；旧失败日志保留。
清理未触源码、其他任务/用户数据或最终真机包，原xcresult、录像、截图、探针源和SHA记录
保留，清理清单 `/private/tmp/maichat-owned-obsolete-qa-cleanup-20261103.json`。

验证边界：DeepSeek-OCR/Qwen视觉大权重尚未加载、未测试OCR质量/真实iPhone内存峰值；
视频文件→现有FFmpeg→mtmd的正式接线尚未完成。`MTMD_VIDEO=OFF`关闭的是上游启动
外部ffmpeg/ffprobe的适配，不是因为MaiChat手机端没有FFmpeg。主聊天模型调用仍保持原有
直连供应商路径，本地库没有引入代理服务或自动下载。

主任务完整源/模拟器/真机边界回执：
`/private/tmp/maichat-local-inference-final-receipt-20261103.json`。
