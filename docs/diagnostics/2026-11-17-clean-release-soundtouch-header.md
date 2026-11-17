# 干净 Mac 发布构建缺少 SoundTouch 空配置头

2026-11-17，Mac arm64，Desktop 3.0.1 候选源码
`a5d81db3e00ace0ad8d78b914a7873c3828f86bc`。

在保留原工作区和现装应用的前提下，用独立主项目 clone、锁定 Qt 源码新建 SDK，
配置普通 Release 并构建 `maichat`。预期从仓库源码直接编译 SoundTouch；实际构建
87.695 秒后 exit 1，`AAFilter.cpp`、`BPMDetect.cpp` 经 `STTypes.h:53` 包含
`soundtouch_config.h` 时报告文件不存在。旧工作区有被忽略的同名文件，掩盖了缺项。

`STTypes.h` 仅在 `__GNUC__ && !ANDROID` 时包含该文件。现有 CMake 的公开包含目录
指向源码 `include/`，安装列表也直接引用它，并没有生成步骤。vendor `.gitignore`
第 17 行 `*config*` 把发布包自带的头漏出了 Git；这不是运行时配置或平台宏缺失。

Mac 端及独立只读 reviewer 分别核对官方 `soundtouch-2.3.3.tar.gz`：606780 字节，
SHA-256 `43b23dfac2f64a3aff55d64be096ffc7b73842c3f5665caff44975633a975a99`，
与已有 `MAICHAT_SOURCE.md` 记录一致。成员 `soundtouch/include/soundtouch_config.h`
为 190 字节、三行英文注释，明确供非 autotools 构建使用；SHA-256
`2356e64a66fc519f4968ed0b692143fa8047c8824b403868c2e546b5524591d1`，
Git blob `3942afe48e56a2f0f9df056d8c7fcddf01635a3c`。文件不定义宏或实现。

修复只把该原始头强制纳入主项目 Git 跟踪，并补充来源说明。没有改 CMake、样本类型、
音频算法、第三方版本或依赖拓扑；没有新增第三方 Git，也没有复制旧构建目录的配置。

同一 Mac 私有源码只补原文件后，`SoundTouch` 目标 16 步、1.620 秒、exit 0。
Root 恢复文件后核对完整字节及 blob；Windows 音频速度与八套核心回归共九套
全部通过，0 失败、69.22 秒。
同一 Mac 私有 overlay 的完整 `maichat` 后续 870/870 步、332.496 秒、exit 0，
人像模型部署 4/4 通过，未发现其它干净源码缺项。该诊断构建仍不代替新冻结后的
应用重配、相关回归、包内运行时及最低系统版本核验。
此前 Windows 两包及六次 smoke 均保留为历史候选；新冻结提交后双端重新构建、打包，
最终回执与 GitHub 附件必须使用同一新提交。此修复不触碰现装应用、用户数据或驱动。
