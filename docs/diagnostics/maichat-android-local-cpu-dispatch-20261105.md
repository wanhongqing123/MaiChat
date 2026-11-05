# Android 本地主模型 CPU 分派与复用 context 取消

## 现场、操作和预期

- 时间：2026-11-05，Asia/Shanghai。
- 原始源码基线：`28c49ec6896098f5fef418ee0e61f135c502aab0`，`main`。早期 RED/GREEN 按实际未提交
  输入指纹记录；后续封版安装验证绑定 `a2dc0aba88813973ec610879bccb030b22c50428`，分别记载。
- 目标平台：Android 15、OPPO PKD130、`arm64-v8a`；使用正常 MaiChat App 和自建 QA 会话。
- RED 安装版本：Android `2.2.29`（266）。本轮构建版本：Android `2.2.30`（267）。
- 本地模型为随包固定资源 `Qwen3.5-2B Q4_K_M`，与既有 iOS 资源约定一致；本轮不更换模型、
  不裁工具声明或历史、不减少上下文和输出上限，不增加模型代理服务器或第二个应用包。

复现操作：在创作中心普通界面切到本机主模型，新建 QA 会话并发送正常文字请求；观察正文、
停止按钮和进程状态。停止后不销毁驻留 runtime，在同一 context 上再次发送请求，约 8 秒后再停止。

预期：按手机实际 CPU 功能选择兼容内核；不支持高级指令的 ARMv8.0 手机保留基线回退。
请求应产生可观测的本地推理进度和真实回复；首次及复用 context 的停止都应合作取消并恢复可操作状态。
构建或选中高级模块本身不能替代真实回复、工具闭环和性能验收。

## RED：长时间无正文与复用后取消迟缓

### 实机观察

`2.2.29` 的首轮普通 QA 请求持续 **1206 秒仍无正文**。进程仍存活，CPU 约占用两个核心，
PSS 曾接近 1.9 GB；不能据此单独断言一定处于 prefill，也不能将其算作成功回复。
尝试取得原生堆栈时，设备要求 root 权限，未取得可用堆栈；没有提权。

第一次按停止后，脚本在 `3.926s` 的采样已观察到停止按钮消失。
复用驻留 context 的下一轮请求在运行约 8 秒后按停止，`49.029s` 采样仍显示忙碌，
到 **`52.042s`** 采样才首次观察到恢复空闲。该数字是 UI 采样时刻，不是原生 callback 的精确执行时长。

本机原始证据：

```text
.cache/android-primary-install/baseline-stop-latency.json
.cache/android-primary-install/reused-context-cancel-baseline.json
.cache/android-primary-install/baseline-before-stop.png
.cache/android-primary-install/baseline-after-stop.png
.cache/android-primary-install/reused-context-after-cancel.png
```

这些原始截图仅用于本机排障，不作为发布资产；本文不复制账号、会话正文或 QA nonce。

### CPU 编译与硬件证据

设备 `/proc/cpuinfo` 包含 `fphp`、`asimdhp`、`asimddp`，即该机具备本次需要验证的
FP16 向量运算和 DOTPROD 硬件条件。

旧 APK 中 `libmaichat_agent.so` 的 SHA-256 为：

```text
1e663a1d09b240caff2db9f52e872acfe6f82d132e2a97ce06d3d332c8fd4f16
```

其已核对的实际 ELF 中，NEON 功能返回 1，DOTPROD 和 FP16 向量功能返回 0。
旧构建 `GGML_CPU_ALL_VARIANTS=OFF`，没有启用安全的动态 CPU 变体。
这证明旧包未利用该机已有的这些指令功能，但不能单凭此证明 1206 秒等待的全部耗时归因。

旧包运行 `.cache/android-cpu-dispatch/check-apk-backends.py` 明确失败：缺少新的 4 个共享依赖和
7 个 CPU 模块。结构化 RED 位于：

```text
.cache/android-cpu-dispatch/baseline-backends-red.json
```

### 取消调用链根因

`RequestScope` 退出时先同步 context，再调用 `llama_set_abort_callback(context, nullptr, nullptr)`，
并清空 KV/SSM 内容、撤销本轮取消指针及忙碌状态。
原 `generate()` 在每次使用 context 前重新安装 callback，`streamChat()` 只有初次创建 context 时安装，
复用 context 后没有重新安装。

因此第二轮 CPU 计算无法在图节点边界收到取消，只能依赖外层批次结束后的检查。
上游 `ggml-cpu.c` 在计算节点结束后检查 abort callback；该缺口与“首次停止较快、复用后停止迟缓”的
实机 RED 一致。

## 修复

### 官方 Android ARM64 CPU 变体

`MaiAgent/third_party/MaiLlamaDependencies.cmake` 仅在 Android `arm64-v8a` 分支启用：

- `BUILD_SHARED_LIBS`、`GGML_BACKEND_DL`、`GGML_CPU_ALL_VARIANTS`。
- 官方 7 个 CPU MODULE：`android_armv8.0_1`、`android_armv8.2_1`、`android_armv8.2_2`、
  `android_armv8.6_1`、`android_armv9.0_1`、`android_armv9.2_1`、`android_armv9.2_2`。
- 4 个共享依赖：`libggml-base.so`、`libggml.so`、`libllama.so`、`libmtmd.so`。

保留官方独立的基线特征检测对象和 `-fno-lto`；高级 ISA 只进入对应模块，未给 JNI 或检测对象统一
增加 `-march`/`-mcpu`。官方 score 依据运行时 HWCAP 拒绝不支持的模块，再选择兼容模块。
没有把高级 CPU MODULE 硬链接进 JNI，Windows、iOS 和 Android ARMv7 保留原静态路径。

`ggml` 已通过 `add_dependencies` 依赖各 CPU MODULE。构建正常 `maichat_agent` 目标时，依赖链
`maichat_mobile → maiagent → mai_llama → llama → ggml` 会生成全部模块。

### 受控加载目录

`MaiLlamaTools.cpp` 的 Android ARM64 动态分支通过 `dladdr` 查询已加载 `libggml.so` 自身导出符号，
按 `MaiFilePath`/`MaiFileSystem` 解析真实绝对路径及父目录。加载前要求库和同目录 ARMv8.0 基线模块存在，
并拒绝非空 `GGML_BACKEND_PATH`；随后将该目录传给官方 `ggml_backend_load_all_from_path`。

不扫描用户工作区、下载目录、当前目录或进程可执行文件目录。旧系统若只返回文件名，或库不是可解析的
安装目录文件，会明确返回未配置错误，不猜测目录。当前 Android 包保留 `useLegacyPackaging true`，
使库按正常 APK 安装流程解压到应用 native 库目录；该路径仍需由最终实机结果验证。

### AGP 库收集与 NDK stub

首轮 native 编译虽生成全部变体，但 vendor 默认把新增库放在自身 `bin`，不属于 AGP 7.4.2 的
`soFolder`。AGP 对这些输出会跳过，MODULE 又不在 JNI 的链接片段中；只看 native 编译成功会漏包。

CMake 现于进入 vendor 前保存调用方 `CMAKE_LIBRARY_OUTPUT_DIRECTORY`。Android ARM64 且目录为绝对
路径时，把 4 个共享依赖和 7 个 MODULE 的 `LIBRARY_OUTPUT_DIRECTORY` 恢复到 AGP 的 `obj/<abi>`；
非 AGP 的空目录保留 vendor 默认。4 个共享依赖清除 `VERSION`/`SOVERSION`，避免 APK 安装依赖版本
SONAME 或符号链接。重新 configure/link 并刷新 AGP native metadata 后再验证最终 APK。

随后发现 AGP 将 NDK 的链接专用 `libz.so` stub 也列入运行时收集。该 stub 不是可随包运行的 zlib。
`app/build.gradle` 最小排除 `**/libz.so`，使用 Android 提供的公共系统 `libz.so`；不替换产品自身的
zlib 实现。APK 门禁增加禁止打包该 stub 的断言。

### 取消与安全诊断

`streamChat()` 在新建/复用 context 的路径汇合后、进入 prefill 前每轮重新安装 abort callback。
`RequestScope` 原有同步、撤销 callback、清 KV/SSM 和清状态顺序不变。

新增固定 Android tag `MaiLlamaPerf`，只记录：

- 实际 CPU 模块的固定白名单文件名与 NEON、DOTPROD、FP16 功能位。
- 请求及模型加载开始/结束、prefill 数量和耗时、每 4 个批次的进度。
- decode 开始、每 128 token 的数量、首个正文回调、结束及取消标记。

日志不含模型输入、token 文本、路径、账号、消息、nonce、密钥或凭据，也未恢复上游完整日志。
`first_text` 的 `decode_elapsed_ms` 从 decode 起点计算；完整首字时间应使用 `request_begin` 与
`first_text` 的日志时间差。工具调用可能先产生工具结果，不能把没有 `first_text` 单独等同于失败。

## 已完成的静态与构建 GREEN

### 源码身份

早期构建验证基于 `28c49ec6896098f5fef418ee0e61f135c502aab0` 加以下未提交输入文件指纹。
这些输入随后进入封版提交 `a2dc0aba88813973ec610879bccb030b22c50428`，后续实际安装绑定该提交；
`192647fcd99045ea8ad1dd2382bef6dd1281b829` 仅追加诊断文档，没有更改 native 源码。

| 文件 | SHA-256 |
| --- | --- |
| `MaiAgent/AgentTools/MaiLlamaTools.cpp` | `f5dfb435e8fb09cf466ea27288fc284b54173201a21226bbb35a87e02686f0c1` |
| `MaiAgent/third_party/MaiLlamaDependencies.cmake` | `88019f5ea843ddc5dc7406679c5245ee9b0267cfca882d054ccd9dcde146bf39` |

两份源码 `git diff --check` 通过，C++ 已运行 `clang-format`。

### Android

- 原生构建完成全部 369 步，随后因 AGP 输出目录修复重新配置、链接、刷新 metadata。
- 排除系统 zlib stub 后，Release 构建 `BUILD SUCCESSFUL`；当前 APK 版本为 `2.2.30`（267）。
- 直接检查 `app-release-unsigned.apk`：`lib/arm64-v8a` 中共 **26 个 `.so`**，包含新增 4 个共享依赖和
  7 个 CPU MODULE，且没有打包 `libz.so`。
- `.cache/android-cpu-dispatch/green-backends.json` 为 **PASS**：核对所需库、共享 SONAME、ELF
  `DT_NEEDED` 闭包、CPU 模块的 `ggml_backend_score`/`ggml_backend_init` 导出、JNI 不直链高级模块，
  以及 7 个特征检测对象均含 `-fno-lto` 且无 `-march`、`-mcpu`、`-flto`。
- 该 APK 中 `libmaichat_agent.so` SHA-256：
  `188ce056f0c6ebb124d29ee7516f5512cadf474f08f2762adf5738c7a6dbb76d`。
- 当前 JVM XML 报告汇总：**96 suites、405 tests、0 failures、0 errors、0 skipped**。
  最后打包任务复用已验证 JVM 结果，日志标为 `testDebugUnitTest UP-TO-DATE`，未声称再次执行全部测试。

本机证据：

```text
.cache/android-cpu-dispatch/native-build.log
.cache/android-cpu-dispatch/verified-build.log
.cache/android-cpu-dispatch/verified-build-no-system-stub.log
.cache/android-cpu-dispatch/green-backends.json
.cache/android-cpu-dispatch/elf-gate.log
MaiChat/android/MaiChat/app/build/test-results/testDebugUnitTest/
```

构建仍报告既有 Gradle 资产任务隐式依赖警告及 NDK `riscv64` 元数据提示；本次构建未将其记为错误，
也没有用屏蔽警告替代 native/APK 门禁。这里验证的是实际 unsigned APK 内容，不代表已核对最终签名
安装包、已安装包和运行进程身份。

### 公共核心的 Windows 回归

相同两份源码指纹下，Windows MaiAgent 已重新编译并运行八项核心 CTest，**8/8 PASS**，退出 0，
总计 `169.67s`。证据在：

```text
.cache/windows-release-245/test-phase-core-20261105T100848-a5b2540a/
```

这只覆盖公共核心回归，不等于 Windows 全部 GUI、发布 staging、ZIP 解压 smoke 或 GitHub 发布已完成。
Windows 发布另见 `maichat-windows-release-245-20261105.md`。

## 尚待实际安装构建验收

以下是封版时的待验清单；实际安装与各项后续结果按时间追加，不能把未完成项填写为通过：

1. 签名安装 `2.2.30`（267）后，核对 APK SHA、已安装 native 库、应用版本和运行进程，确保不是旧包。
2. 在普通创作中心切换云端/本机来源，验证持久化恢复、忙碌互锁和失败后可切回云端。
3. 用 `MaiLlamaPerf` 确认实际选中兼容的 DOTPROD/FP16 模块；记录模型加载、prefill、decode 和首字时间，
   不将 feature 位或计数进度直接写成真实回复已过。
4. 沿 RED 的相同步骤复测首次停止和复用同一 context 后停止，核对恢复空闲及后续可继续发送。
5. 自建 QA 会话完成两轮记忆及重启恢复，并通过已有文件工具读回 QA 文件完成真实工具闭环；
   不把文件预先拼进 prompt 或用测试替身替代模型调用。本文未声称首字、工具、记忆已通过。
6. ARMv8.0 旧机回退尚无实机结果。当前仅有官方 score 路径、无高级 ISA 的检测对象及基线 ELF 证据。

旧 `2.2.29` 的模型完整性 instrumentation 挂起后由操作者结束测试进程，日志末尾为
`Process crashed`；这不是独立确认的 native 崩溃，也不记为通过。后续 `2.2.30` 的独立重测
为 **1/1 PASS**，详见下节；完整性测试与普通界面推理仍分别报告。

每轮请求仍按现有实现清空 KV/SSM 后完整 prefill，未新增跨轮前缀缓存。动态 CPU 模块的实际收益和
完整工具/历史条件下的可用速度，需要上述实机测量，不能由构建成功推定。

## 2.2.30 签名安装与模型完整性实机结果

封版源码 `a2dc0aba88813973ec610879bccb030b22c50428` 对应的 `2.2.30`（267）已原位安装到同一
OPPO Android 15 真机；未清理生产数据或更换包名。显式使用 `adb install --no-incremental -r`，
避免本次验证依赖主机的增量文件服务；这不证明此前挂起的根因就是 IncFS。

- 最终签名 APK 与从设备回拉的 `base.apk` SHA-256 完全一致：
  `267c7ea87a3639b5e652be970d3ad5a18d0324132f07f65dd6b1cc3780144efd`。
- 安装版本、同一证书、26 个 ARM64 库和未打包 `libz.so` 均核对；主库 BuildID
  `32b1a13dd001164f9784bc3e8dca3d8f9d97bbd0` 与当前 native 输出一致。
- 唯一完整性 instrument 用例 **1/1 PASS，6.481s**。私有模型文件实际读完
  `1396198496` 字节，SHA-256 为固定的 `57a10858…846a1d8`；独立复核耗时 `6.446s`。
  回执明确 `private_app_file=true`、`inference_exercised=false`，并记录本次测试进程/线程身份，
  不复用旧回执。测试包随后卸载，仅保留生产 App。

本机证据为 `.cache/android-primary-v230-integrity/qa-result.json`、`installed-verification.json`、
`installed-model-integrity.json`、`integrity-instrumentation.log` 与 `uninstall-test.log`。
上述 GREEN 仅说明安装产物和模型文件完整性；普通界面的推理、记忆及工具验收仍需独立完成。

## 普通界面运行中的性能定位边界

从正常 MainActivity 启动后，主模型恢复为本地模型；新建独立 QA 会话并通过普通输入框发送请求。
本次进程 `PID=22683`，安全日志确认实际选择 `libggml-cpu-android_armv8.2_2.so`，
`neon=1/dotprod=1/fp16=1`。模型载入 `8122ms`；本轮完整 prefill 为 `25395` tokens，
`batch=512/threads=2`，未裁工具目录、系统指令或历史。

已观测到 `2048/90421ms`、`8192/365577ms`、`16384/846127ms`、`22528/1336309ms`
等递增进度，说明运行仍在执行预填充；这不是助手正文，也不能计为记忆或工具验收通过。

只读源码核查与一次安全设备状态采样得到：

- 两个 native 工作线程分别在 big cores `6/7`，`NI=0`；没有发现落在后台低优先级的证据。
- `n_batch=n_ubatch=512`，decode/prefill 均为两线程；`-O2`、LLAMAFILE 和 CPU_REPACK 均启用。
  OpenMP 关闭不会关闭 ggml 自己的线程。
- 本地模板沿用既有 `enable_thinking=false`；源码明确远端 `reasoningEffort` 当前不映射到本地模板。
  本轮没有临时开启其它推理模式，也不宣称菜单中的三档强度已经在本地模式取得差异化验收。
- 宿主保留 `llama_context_default_params()` 的公开 `flash_attn_type=AUTO`，未显式关闭 FA。
  源码支持当前 CPU F16 attention 进入 tiled FA 路径；尚未直接记录运行时 resolved flag，
  因此不把这条配置调用链当成新的真机观测。
- Qwen3.5 是混合 attention/SSM 架构。固定 vendor 默认使用独立 fused GDN CPU 算子，
  每个 head 内逐 token 更新 FP32 状态；DOTPROD/FP16 不能保证所有算子同比提速。
  当前没有逐算子 profile，不能据此认定具体占比。
- 固定 vendor 没有公开 context 参数或环境开关来选择内部 unfused GDN chunk 路径。
  本轮没有修改 vendor 内部字段或声称该实验路径有效。
- 在 `02:47:56 UTC` 的单次采样中，big cores 当前/最高为 `2100/2400MHz`，
  thermal status 为 `3`，skin 约 `44.4°C`。这说明存在热限制条件，但不能把后段变慢全部归因于它。
  未更改系统电源、性能或前台策略。

请求结束时清理 KV/SSM 会导致下一轮和工具结果重新完整 prefill，不会解释本轮首轮的慢速；
当前没有每个 batch 清理。直接取消清理也不安全：全局 runtime 没有会话身份，且 Qwen 的
recurrent end-state 不能任意回滚。任何后续前缀缓存都必须比较完整实际 token，并保存对应的
KV/SSM checkpoint，对取消、异常及不同前缀明确失效；本轮没有引入该功能。

### 首轮完成：本地执行已确认，响应行为失败

正常创作中心同一 QA 请求在 `02:32:19 UTC` 进入 native，`02:58:57 UTC` 首次产生文本。
完整 request-to-first-text 为 **1598.111s**；prefill **1589.401s**，decode **5.367s**，
request 总计 **1603.613s**（约 26 分 44 秒），`canceled=0`、`eog=1`。
`first_text` 上的 `decode_elapsed_ms=9` 仅表示预填充结束后的 decode 差，绝不能当成 9ms 的首字耗时。

操作者亲看普通 UI 的 `first-complete.png`：模型错误地把用户自己提供的虚构测试标签当成敏感
内部标识而拒答，没有按要求确认该普通标签。助手原文独立 SHA-256 为
`29a55b7fdb0e654290e37939dd25b0a3f32c0dfa3dc4116a0ec5eace4f8819d1`；没有工具调用或审批。
这是实际生成后的**行为失败**，不因 native 正常结束或后续澄清而改写成成功。
本机 `first-performance-summary.json` 独立记录完整 TTFT 与 decode 区间。

### 复用驻留 context 的取消回归 GREEN

不销毁驻留 runtime，在同一普通 QA 会话再次提交短请求，等待 8 秒后定位正常停止按钮。
包括 UI 定位时间，Stop 实际发生在提交后约 `11.422s`；点击后第一次 UI 采样在 **3.017s**
内已恢复 idle，实际截图 `resident-cancel-idle.png` 显示加号恢复、停止按钮消失。
同一 native 请求记录 `canceled=1`、总耗时 `11.920s`。

对比原先 `52.042s` 才观察到 idle 的 RED，复用取消问题已用同一现场操作取得改善证据。
`3.017s` 是含 UI 采样开销的恢复上界，不是精确的 native 中断延迟。

后续澄清记忆测试明确不重复期望标签值，并保留旧问题及 variant 审计，用来区分记忆丢失与
错误敏感分类。文件工具 fixture 也只使用操作者自建、未进入 prompt 的随机普通词语短句，
避免把十六进制外观的测试串与真实凭据混淆。两者仍需由实际结果验收，不能由准备步骤推断通过。

### 澄清记忆：回忆正确，严格输出格式失败

第二轮只询问第一条消息中由用户自己给出的虚构标签，并澄清它不是系统标识或凭据，
**没有再次提供期望标签值**。原问题与该 `clarified_memory` variant 均在本机计划中留有审计。

实际回复包含正确标签，但额外加上完整介绍句，没有遵守“只返回标签”。原文 SHA-256 为
`ec688d3410b4d29c895496063f41c7bfaaaf6df0124cb5449d5ce6fb0555ba15`；
`contains=true/equals=false`。操作者亲看 `clarified-memory-complete.png`：
这证明澄清后的跨轮回忆，**不代表严格格式通过，也不抹去首轮误拒答**。
本轮 prefill `1764.106s`、decode `6.198s`、总耗时 `1770.698s`（约 29 分 31 秒），没有调用工具。

### 普通重启恢复 GREEN

会话空闲后正常 force-stop 并重新通过 MainActivity 启动，进程从 `22683` 变为 `30178`。
不读取数据库、不清生产数据、不修改模型设置，通过普通创作中心页面确认同一 QA 会话仍在，
首轮实际拒答和澄清轮答复的原文哈希均一致；菜单恢复 `本地模型 / 标准 / 请求批准`。
操作者亲看 `restart-local-source.png` 及前后会话截图，结构化证据为 `restart-qa-result.json`。

### 文件工具请求的输入边界

自建工具 fixture 为 98 字节、由 16 个普通词组成的随机短句（96 bit 随机熵），仅推送到唯一的
自有 Downloads 路径。设备文件 SHA 与计划一致，再通过普通 `+ → 文件 → 下载` 选择器导入。
导入后的草稿只有工作区 `import-96a46377.txt` 引用，`30` 字符、`hidden_nonce_in_draft=false`；
最终待发输入为路径引用加读取要求，`197` 字符，也不含文件正文。

一次自动键入的光标位置被草稿精确断言拦住，尚未发送；随后只在这份自有 QA 草稿中用正常
全选替换恢复等价输入，重新核对后于 `03:43:43 UTC` 提交一次。
新的 native 进程再次确认 `armv8.2_2 / neon=1 / dotprod=1 / fp16=1`，模型载入 `7913ms`，
本次完整 prefill `25585` tokens。输入中出现单词 `file_read` 不算实际工具调用，
必须分别核对真正工具卡、工具结果与模型最终回复；此处仅记录准备和已提交事实。

### 文件工具闭环 GREEN，耗时不达日常可用标准

普通 UI 出现真正的 `file_read ×1` 已完成工具组。展开后输入严格为
`{"path":"import-96a46377.txt"}`，结果为单个行号、Tab 和原 fixture 的完整正文，已独立归一比较。
没有新审批按钮，没有改成免审批模式；该只读操作按既有 `请求批准` 策略直接完成。
操作者亲看 `file-read-tool-completed.png`，本机 `file-read-tool-evidence.json` 留有结构化回执。

随后自动进入工具结果后的第二个模型请求，完整 prefill 从 `25585` 增至 `25662` tokens。
第二段正常结束后，实际助手最终文本 `97` 字符，strip 后与隐藏词句**严格相等**，不是仅包含匹配；
原文 SHA-256 为 `06a0bf24d94478ff457181e68f20ce5f1440abdf73195d49ad7b68982b28756a`。
操作者亲看 `file-final-complete.png`：工具结果与独立助手最终正文都可见，停止按钮消失。
原始标记因滚动离开可见树时，自动 observer 拒绝归属，未被绕过；后续用已验证的唯一导入路径、
工具详情哈希和同一 QA 页面重新核对，才记录最终结果。

这验证了 `普通导入 → 模型调用 file_read → 工具实际读文件 → 模型最终复述`。
用户输入、导入草稿和最终待发输入均未包含期望词句；没有测试替身、固定返回或云端回退。
没有读取数据库或其它会话。当前 UI 未导出工具 `call_id`，因此**不宣称新增设备端 ID 逐值比对**。

| 阶段 | 完整 prefill | decode | native 请求总耗时 |
| --- | ---: | ---: | ---: |
| 产生工具调用 | `1710.548s` | `34` tokens / `16.698s` | `1735.755s` |
| 工具结果后最终复述 | `1874.544s` | `22` tokens / `8.801s` | `1883.704s` |

整个文件工作流从首次 native request 到最后正常结束为 **3619.476s**（约 **60 分 20 秒**）；
最终首字为 **3610.638s**。第二段 `first_text.decode_elapsed_ms=4` 不是完整首字时间。
真实工具闭环通过，但该耗时严重不符合日常交互标准，**不认定性能问题已修复**。
首轮误拒、澄清记忆的严格格式失败亦保持原结论，没有因工具通过而抹去。

在 `04:39:55 UTC` 再次仅采一份热状态摘要：status 仍为 `3`，big cores 当前/最高仍为
`2100/2400MHz`，没有观察到 `4/5/6`；不把全部耗时归因于热限制。全程未修改系统性能设置。

最终本机证据为 `file-final-reply-evidence.json`、`file-workflow-performance.json`、
`ordinary-qa-result.json`。这些包含自建 QA 内容，仅留在忽略的缓存；公开诊断不包含原始词句、
测试记忆标签、用户账号、令牌或签名材料。

### 收尾恢复

性能采集器仅终止各自新建的 adb logcat 子进程，均已退出；测试包已在完整性验证后卸载。
随后通过普通菜单恢复测试前的 `云端模型 / 标准 / 请求批准`，重启普通 App，
再次通过 UI 核对相同设置已持久化。当前正常进程 `PID=6025`，PSS `166538 KiB`，
已释放重启前的驻留本地模型运行时；未发送云端模型请求。
屏幕旋转恢复为原来的 `free`，返回消息页；生产数据未清理，自建 QA 会话和小文件保留供检查。
收尾回执为 `restored-defaults.json`，截图为 `restored-cloud-after-restart.png`。
