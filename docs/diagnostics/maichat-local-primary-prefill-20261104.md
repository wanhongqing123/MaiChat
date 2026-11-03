# 本地主模型完整工具目录长上下文 prefill

## 现场与预期

2026-11-04，正常 iOS 模拟器 App 2.2.4/179 的本地主模型已选中官方 Qwen2.5，但完整
119 个以上工具目录的首次短问仍长时间停留“正在思考”。Root 提供的现场进程 PID 86882
累计 CPU 超过四分半钟，原 180 秒验收时限内没有首字。

预期是保留正式工具声明和完整历史，正常 App 首轮可以开始回答；不能用只注册 file_read
的短提示通过替代此体验，也不能删除工具或延长 UI 超时掩盖慢调用。

现场 sample：`/private/tmp/maichat-primary179-prefill-v4-20261104.sample`。其中 mai-turn
的全部 15 个样本均在 `prefillTokens -> llama_decode -> CPU graph_compute`，热点为
`ggml_compute_forward_flash_attn_ext`；主/辅助计算线程合计 24 个顶栈样本，Metal 工作线程
处于 usleep。它已经使用 CPU Flash Attention，不能归因为默认关闭 FA。

固定上游 `llama-context.cpp:4229` 默认 FA=AUTO。`op_offload=false` 也不能直接等同
禁用 Accelerate BLAS：后端顺序为 GPU/ACCEL/CPU，调度器先按权重 buffer 和算子支持选择
后端；BLAS 可直接处理其支持的大矩阵。实际瓶颈证据是长上下文注意力在 CPU 执行。

## 最小修改

- 独立 `generate` 保留固定 CPU。仅主模型 `streamChat` 在 Apple 上优先已注册的 Metal
  设备，全部 transformer/output 层、KQV 和支持的算子使用 GPU；不存在设备时保留可观察的
  CPU 回落，其他平台当前仍用 CPU。FA 保持上游 AUTO。
- 驻留身份加入执行偏好，普通 CPU generate 与主模型不能误用同一已载入配置。实际 GPU
  状态来自 `llama_model::memory_breakdown()` 的权重 buffer 设备统计，不把配置偏好、设备
  存在或链接了 Metal 库冒称为已经使用 GPU。
- 每个 prefill 批次前检查取消，完成后同步再检查。上游公开 abort 路径没有把回调接入该
  Metal backend，GPU 只承诺批次/同步边界合作取消，不承诺立即抢占 kernel。
- 用户后续明确要求自然输出。主模型 `maximumTokens=0` 为默认：完整 tokenize 后使用真实
  上下文余量，生成至 EOG；非零显式预算只保留给普通代码/测试。没有默认 512/4096 输出
  截断。真实上下文或内部原始输出内存边界耗尽时明确失败，保留已经交付的正文，不执行
  未完成工具批次；独立 generate 的实验预算契约保持原样。

首轮选择器曾按名称 Metal 查询，而固定 vendor 实际 registry 名为 MTL；实际状态如实显示
GPU 权重为零，测得的是 CPU 回落而非加速。随后按真实注册表匹配，保留该轮作为 CPU 基线，
没有将它报告为 GPU 成功。

## 定点实测

使用同一真实官方 Qwen GGUF、相同捕获的 119 工具移动端请求、同一 32768 上下文，首次
文本到达后主动取消，不执行任何模型生成的工具。提示包含约 23457 个输入 token；捕获来自
正式 Mobile C ABI 工具目录，但不含可选 RVM/LaMa/iOS27 屏幕工具，因此仍是现场目录下界。

| 观察 | CPU 回落基线 | 实际 Metal |
| --- | ---: | ---: |
| 首次文本 | 190.698 秒 | 13.979 秒（最终复测） |
| GPU 权重 buffer | 0 | 391859712 字节 |
| 实际 registry | 空 | MTL |
| 生成预算 | 测试显式 512 | 默认自动 0 |

两轮输入及 prefill 相同，均在首字取消，输出预算差异没有裁输入或改变上下文分配。
实际 Metal 首字耗时约缩短 13 倍；不将 macOS 原生测量时间等同于 iOS 模拟器或真机表现。

预热后的第二次完整 prefill，在开始后 150 毫秒请求取消，最终取消调用等待 0.116 秒，结果为
Canceled。随后调用普通 generate，实际 GPU 权重为零、驻留代数增加；再调用主模型恢复
MTL 权重和新代数，证明两种执行配置隔离。它是一个实测样本，不是 GPU 最坏取消时延保证。

## 证据与边界

产物目录：`/private/tmp/maichat-local-primary-model-20261103`。

- `metal-full-catalog-performance.log`：实际完整目录、首字、取消和 CPU/GPU 隔离结果。
- `full-catalog-cpu-fallback-baseline.log`：完整保留的 CPU 回落基线。
- `metal-selection.log`：载入后的实际 GPU buffer 观测。
- `metal-production-freeze.json`、`metal-performance-commands.json`：源码/探针/二进制哈希与命令。
- `metal-targeted-final-evidence.json`：真实主模型自动 EOG、显式长预算预检、执行配置隔离、
  原生工具闭环、取消及旧 CPU generate 的定点回归。
- `metal-full-catalog-ordered-final.log`：最终有序退出版本的完整目录性能复测。
- `metal-core8-final-evidence.json`、`metal-core8-final.log`：最终 GPU/auto0 生产版本的
  Core8 全部通过，`ctest -j2` 用时 77.05 秒，源码与产物哈希已记录。

实际使用 GPU 后，“保留驻留模型直接退出”还暴露了另一条真实边界：Metal 静态设备析构
要求权重 resource sets 已释放，单独保留 NoDestructor owner 会触发该断言。最终版本在
新驻留配置完成首次实际使用后登记有序退出 unload，最新清理先于该配置初始化的后端静态
对象执行；旧处理器幂等清空，所有者本身继续避免跨组件静态析构。相同驻留退出 case 从
SIGABRT 恢复为正常退出 0，旧红日志保留为 `metal-resident-exit-before-ordered-cleanup.log`。

正常 iOS/Android/Desktop App 的重建、实际进程及最终交互表现由 Root 在新产物上继续验收。
没有登录、用户文件、线上密钥、付费模型调用或真实设备操作。

## iOS Simulator v5 的实际 Metal 能力差异

正常 App v5（2.2.4/179）在同一个 iOS 26.5、arm64 Simulator 上仍未在原 180 秒窗口
完成首轮，原红日志保留为 `/private/tmp/maichat-primary179-main-ui-v5-20261104.log`。
进程 98582 在本次探针前已退出，因此没有将独立探针冒称为该进程的运行栈。

定点探针直接链接 App 使用的 Sim vendor cache 的 `libggml*.a`，使用相同 Simulator 和
iPhoneSimulator27.1 SDK。它不载入模型、不访问 App 数据、不调用工具，只查询系统 Metal
设备、真实 ggml registry 和合法 Qwen 形状的算子支持。观察如下：

- `MTLCreateSystemDefaultDevice()` 返回 `Apple iOS simulator GPU`；MTL0 注册为 GPU。
- 设备只报告 Apple2，`has_simdgroup_reduction=false`、`has_simdgroup_mm=false`。
- `ggml_backend_dev_supports_op` 对 Q4_K 矩阵乘法和 F16 KV Flash Attention 都返回 false。
- 固定上游 `ggml-metal-device.m:1518` 明确因缺 reduction 拒绝矩阵乘法，1809 因缺
  simdgroup matrix multiplication 拒绝 Flash Attention。该 Simulator 的设备存在并不
  表示能够执行 Qwen 的关键计算；这些计算需要由支持它们的 CPU 后端执行。
- 首次嵌入 36 个 MSL 库的编译共 12.238 秒，缓存后 0.016 秒，第二轮设备与算子查询总计
  0.030 秒；无法将长时间 prefill 归因为持续编译内核。

实际编译参数含 `GGML_USE_METAL` 和 `GGML_METAL_EMBED_LIBRARY`，archive 注册名称是
MTL。正常 App 的 `MaiChat.debug.dylib` 确实包含 Metal 注册函数及 72 个嵌入 start/end
符号。当前 embed 分支直接引用这些链接地址，没有通过 `_mh_execute_header` 查主 executable
section；`GGML_METAL_TARGET_OS` 的 xcrun metal 参数只在非 embed 分支使用。因此现有证据
不支持改 cached macos 或 debug dylib 布局作为本次性能修复。

证据目录：`/private/tmp/maichat-ios179-metal-registry-probe-20261104`，其中 `evidence.json`
记录 App、源码、archive、探针哈希，前后生产冻结哈希全部一致；`run.log`、`run2.log`
分别保留冷/热内核初始化和算子结果。没有为诊断修改生产代码或放宽 UI 超时。

这证明了当前 Simulator 的能力边界，不能替代真实 iPhone 正常 App 的 GPU 性能验收。
不强行启用上游已明确拒绝的 SIMD 内核，也不将 macOS 的 13.979 秒报告为 iOS 实测。
