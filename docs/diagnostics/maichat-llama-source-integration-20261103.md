# llama.cpp / mtmd 源码依赖验证（2026-11-03）

操作：按现有 `MaiAgent/third_party` 约定保存官方固定源码，从本地静态构建 llama、
ggml、mtmd，分别验证 Mac、iOS 真机与模拟器的编译/链接；不以下载的 XCFramework
代替源码集成。源码基线为 MaiChat `83b76394` 加本轮未提交 Runtime/构建接线。
此次不操作设备、真实账号、密钥或付费服务，也不下载推理权重。

## 固定来源与审查

- 上游 b12065，提交 `ee17f293b638295651c6ff4752a4dce102c6b8e4`。
- Git tree `625866e031a0127781b4805a4455f10ff3f50647`。
- 官方 codeload 源码包 38,910,289 bytes，SHA-256
  `ccae5be5b19211072aea391a726c80f0848e191211d121228e6c71b9f4ab3bd8`。
- 完整 4,021 个上游文件 / 182,799,898 bytes，每个 Git blob 匹配；构建后再次核对
  `MAI_SOURCE_MANIFEST.sha256` 全部匹配。上游代码没有本地修改或格式化。
- 无超过 40 MiB 的文件、无私钥 PEM 头；19 个 GGUF 都是零 tensor 词表夹具。
  `idea-arch.key` 是上游 Keynote 文档。来源、许可和审查明细在 vendor 的 `MAI_*` 文件。

## 构建接口与范围

`MaiAgent/third_party/MaiLlamaDependencies.cmake` 提供
`mai_add_llama_dependencies()`，幂等创建 `mai_llama`。该 interface 连接真实静态
`llama`、`mtmd`，由上游传递 ggml、CPU、Metal、Accelerate 和 hash 依赖。
普通 Runtime 只需链接该 target；STB 的私有预检 include 由其自身目标私有配置。

tools、common、examples、tests、server、Web UI、OpenSSL、subprocess 和动态后端
加载关闭，mtmd 库与 Metal 源码嵌入开启。未把上游 CLI 重命名后塞进工作线程；
CLI 仍未完成进程退出、信号、全局状态、重复调用和有界输出审计。
`MTMD_VIDEO=OFF` 是禁用其通过 PATH 启动外部 ffprobe/ffmpeg 的适配器；
MaiChat 已有进程内 FFmpeg 不受影响。后续视频需已有抽帧 API 交有界 RGB/timestamp
给 mtmd bitmap/chunk，本次不声称视频文件输入已经接通。最终 mtmd archive 的未解析
符号检查没有 fork、exec、posix_spawn、popen 或 system。

## iOS 16.0 的实际兼容修复

初次独立脚本误用了 16.6；已改为产品最低版本 16.0，并完整重新编译两种 iOS 目标。
这次真实构建揭出上游 Apple BLAS 无条件定义 `ACCELERATE_NEW_LAPACK` 与
`ACCELERATE_LAPACK_ILP64`，强引用 `_cblas_sgemm$NEWLAPACK$ILP64`；SDK 标为
iOS 16.4 才可用。仅把最终 Mach-O 的 minos 写成16.0不能消除这个运行风险。

本地依赖模块仅对 iOS <16.4 在 CPU/BLAS 目标取消这两个宏，保留成熟的 LP64
Accelerate CBLAS、CPU 和 Metal。上游算法同时支持其他 LP64 BLAS；不提高产品 OS
要求、不改上游源、不屏蔽 warning。将 unguarded availability 当作 error 后，两种
目标均重编/重链成功，最终导入为 `_cblas_sgemm`，不再含 NEWLAPACK 符号。

## 实际产物

可复现入口 `MaiAgent/scripts/build-llama-apple.sh`，默认每个构建两 jobs：

| 参数 | 最终平台 | 最低 OS | 实际结果 |
| --- | --- | --- | --- |
| `macos` | arm64 MACOS / SDK27.0 | 13.3 | 八静态库、smoke 链接并运行 exit0 |
| `ios-device` | arm64 IOS / SDK27.1 | 16.0 | 八静态库、smoke 交叉链接成功 |
| `ios-sim` | arm64 IOSSIMULATOR / SDK27.1 | 16.0 | 八静态库、smoke 交叉链接成功 |

每个目标实际得到 `libllama.a`、`libmtmd.a`、`libggml.a`、`libggml-base.a`、
`libggml-cpu.a`、`libggml-blas.a`、`libggml-metal.a`、`libvendor-hash.a`。
八归档总大小约17.5MB。最终链接通过 Foundation、Metal、MetalKit、Accelerate 和
Threads；无外部 SDK、模型或可执行文件依赖。Metal 归档确有嵌入 shader 源码符号。

`MaiLlamaBuildSmoke` 不只检查配置：链接真实库，执行 backend init/free、检查固定
ggml commit 与 llama/mtmd 公共默认参数，再由 mtmd-helper/STB 解码合成的1像素 PNG。
这同时拉入 helper、STB 和 hash 的真实链接闭包。Mac 运行通过；iOS 两份仅交叉
编译/链接，尚未在设备或模拟器执行，不能作为模型推理、OCR 精度或内存峰值验收。

证据 `/private/tmp/maichat-llama-source-build-validation-20261103.json` 包含每个 archive、
smoke 的 SHA、平台/minos、编译参数、日志路径和源码审查。最终日志前缀：
`maichat-llama-source-macos-final-20261103`、
`maichat-llama-source-ios-device-16-final-20261103`、
`maichat-llama-source-ios-sim-16-final-20261103`。
Runtime 的真实模型生成与普通工具测试由该层另行记录，不能用本次无模型 smoke 代替。

App 可复制 vendor 的 `MAI_THIRD_PARTY_NOTICES.txt`（12,340 bytes）作为许可资源，
保留主 MIT、hash、STB、miniaudio、sheredom 及实际 CPU llamafile 的原许可文本。
根 ignore 会忽略四个上游文件（含 `build-xcframework.sh`），完整提交时应依据来源
清单纳入；本任务不执行 stage、commit 或 push。

## Android armeabi-v7a 的实际构建修复

正常252产品构建保留既有两种 ABI，32位分支实际失败于上游可选
`ggml-cpu/llamafile/sgemm.cpp:311/314`：`vld1q_f16` / `vld1_f16` 未声明。
证据 `/private/tmp/maichat-local252-android-build-20261103.log`；不能通过删除32位
ABI 规避。该优化实现使用 ARMv7 不提供的 FP16 NEON load intrinsic。

自有依赖模块只在 Android armeabi-v7a 设置 `GGML_LLAMAFILE=OFF`；上游
`ggml-cpu/CMakeLists.txt:84` 确认此 flag 只控制该 SGEMM 源和对应宏。
普通 ARM/NEON 内核保留，其他 ABI/后端设置不变，完整上游 pin 没有补丁。

随后使用真实 NDK27.3.13750724、API26，从新的隔离目录构建完整 llama、mtmd、
ggml CPU 及带真实 PNG helper 的链接 smoke，build exit0。新增可复现入口
`MaiAgent/scripts/build-llama-android.sh` 也实际运行通过。compile_commands 中该
SGEMM 源为0项、`GGML_USE_LLAMAFILE` 未定义，普通 CPU 16源仍编译；相同 NDK
编译器的 `__ARM_NEON=1`。六份静态库最终链接为 ELF32 / ARM，仅依赖 Android
libm、libdl、libc++_shared 和 libc；没有缺符号，没有操作设备或安装 App。

证据 `/private/tmp/maichat-llama-source-android32-validation-20261103.json`
含六归档/ELF/script/module SHA 和编译链接记录。上游量化专用的8GiB size_t常量
在32位仍产生一次转换警告；量化 CLI 没有启用，不把该构建说成零 warning。
正常252产品增量 Gradle 和实际 App 验收由主任务继续，不能用这个交叉链接代替。
