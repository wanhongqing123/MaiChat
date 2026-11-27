# Android 真机命令环境验收准备

日期：2026-11-27。用户要求用已有 Android 真机验收移动进程内命令和 file_* 退出模型注册的改动。

## 连接定位

初次 `adb devices -l` 仅列出 emulator-5580（x86_64），历史真机 89VCD65LEYTGROW8 的
`get-state` 返回 not found。没有重启 ADB server、断开模拟器或操作现装。
进一步只读 PnP 检查发现 OPPO VID_22D9/PID_2046 的 USB Composite Device，子接口为
MEDIA/MIDI function，没有 Android ADB 调试接口。物理 USB 连接存在，但不能据此执行调试命令。
无线 mDNS 也没有发现已配置调试端点。已请用户开启手机 USB 调试并确认电脑 RSA 授权，
同时准备当前源码的 ARM64 原生验收产物；不能拿 x86_64 模拟器或旧 ARM64 二进制代替。

## 验收对象与边界

优先构建当前 Core、MobileAgent 和 MaiMobileAgentTests，使用自有临时工作区及 loopback 模型，
运行 `--file-command-contract-only`：核对实际请求无 file_*、有唯一进程内 exec_command 和
apply_patch，真实文件/编码/审批/工具结果路径成立。命令核心补测 ls/cat/cp/mv/find/grep/read-text、
续读与源变化、容器边界、取消和输出限制。

测试不得读取真实相册、用户数据库或密钥，不覆盖/卸载 App、不清数据、不切换用户模型。
在 `/data/local/tmp` 创建随机独占测试目录；只推送同源测试与精确动态依赖，隔离 TMPDIR 和
LD_LIBRARY_PATH。真机原生测试不等于正常 App UI 实机验收，结果需分别记录。

旧 Android 正常 lane 位于
`MaiChat/android/MaiChat/app/.cxx/mobile-agent-adapter/Debug/6ex341a5/arm64-v8a`，
NDK 27.3/API26。旧产物只作依赖和构建方式参考，不能直接用作本版测试结论。
Windows 私有移动 lane 的 OpenSSL/platform factory 链接缺口不能作为 Android 真机不能验收的理由。

## 当前可执行验收包与阻塞

进一步 PnP 查询确认 USB 总线设备报告型号 PKD130；目前只显示 MIDI 子接口，ADB 仍仅模拟器。
ARM64 真机验收包已从当前源码重新构建完成，四个测试及六个动态库共 139,160,048 字节，
位于 `.cache/arm64-mobile-file-contract-20261127/deploy`。10 件部署文件按清单逐 size/SHA256 核等。
403 个项目源/头及 3 个补充测试 main 的构建后哈希全等；未重用旧 Core/Mobile/Graphics ABI。
FFmpeg/curl/OpenSSL 内嵌 CLI 当前源码重编，SQLite 按当前 preupdate 配置重编后链接通过。

构建成功不等于真机通过。当前 `device-connection-pending.json` 明确
`deviceExecutionPerformed=false`，没有上传、运行或安装到真机；仍需手机端调试接口和 RSA 授权上线。
本次没有停止现装、清数据或替换 APK。

## 真机执行、故障定位与最终通过

用户开启调试后，设备 89VCD65LEYTGROW8/PKD130 以 device 状态上线，Android API35，
ARM64，非模拟器。首轮四项结果为 0/0/1/1：移动请求合同、脚本解析通过；文件命令及薄工具各
5 个断言失败，原始日志在 `physical-9a782e802fa5`，仅诊断 ELF 与结果另存，没有覆盖原件。

具体根因：copy 的完整文件发布在无 JVM 回调的独立命令环境返回 ENOSYS；move 还使用硬链接。
真机独占目录内的原始 syscall 探针证实 link 被 EACCES 拒绝，而 renameat2(NOREPLACE) 成功，
目标存在时 EEXIST 且源保留。于是普通 Android 文件发布/移动优先原生 NOREPLACE，核对移动后
文件 dev/ino；真实权限/已有目标/跨卷错误不回退重试，不使用会覆盖目标的普通 rename。
旧兼容 ABI 保留，仅不支持原语时沿既有契约处理。

编码失败来自独立测试未注入 Android 所需的旧编码解码回调。测试补入严格 Latin1 回调并验证
实际调用，保留无回调时 NotConfigured、原文、编码、EOF 和续页断言；未改产品解码算法。

最终源码（含必要格式收口）同设备同四用例：

| 目标 | 退出码 | 秒 |
| --- | --- | --- |
| MaiMobileAgentTests-final --file-command-contract-only | 0 | 1.515 |
| MaiCommandScriptTests-final | 0 | 0.259 |
| MaiFileCommandTests-final | 0 | 1.292 |
| MaiInProcessShellToolTests-final | 0 | 0.269 |

最终证据 `physical-5922d012b2d0/receipt.json`，10 个 ELF/运行库均在推送后逐 SHA256 核等。
`final-deploy-manifest.json` 和 `final-source-verification.json` 保存当前精确源码/产物证明。
已清理本次独占测试目录；首轮原始 FAIL、诊断和探针均保留在 PC。

## App 身份补验与清理

ADB 启动原生程序的身份为 UID2000(shell)。为验证 App 私有目录权限，另建独立 QA 包
`com.kongshang.maichat.shellverify20261127`，只有 INTERNET 权限，无相册/存储授权。
该包直接在同一 App 进程的后台线程调用已验证 C++ Core/薄工具，没有启动 Shell 或测试子进程。
实际 UID10044，目录属同 UID；复制/移动、读取/搜索、普通补丁和越界拒绝六项全通过。
这不是正常 MaiChat UI 全流程验收，不能混同原四 suite 或模型自主调用。

APK SHA256 `feb06945353264d2840590285a15f04b62782c16a11a5464ce29cb9bf014c750`；
证据 `.cache/android-appuid-shell-verify-20261127/device-run/{device-receipt.json,app-uid-result.png}`。
初次安装等待系统确认超时，确认后核对已安装 APK hash 再运行；未绕过账号、锁屏或签名验证。
QA 包已卸载，旧测试临时目录已按真实路径+随机所有权标记核等后清理。
现装 MaiChat 版本码285、PID26319 前后不变；未安装覆盖新产品包、清数据或发真实供应商请求。

Chromium 分层核对见 [文件后端审计](2026-11-27-chromium-file-backend-audit.md)。

## 最终兼容性与 App UID 复验

用户追问 Android 版本普适性后，进一步按 NDK API30/minSdk26 规则补公开接口探测，
并加入缺公开符号模拟及相对路径回归。当前 Android 端不直接尝试原始 renameat2 syscall。
具体 RED→GREEN 及 Chromium 分层见上列审计；不能把模拟分支通过说成真实旧系统通过。

精确源码/ABI 产物证明为 `verified-source-verification.json`、`verified-deploy-manifest.json`。
Core SHA256 `08feaba90adeddb8453e5d196ce1bfb1f05038e932fc6449b875b439bf620b9f`。
同设备执行五个 `*-verified` 目标：Mobile请求1.588s、Script0.249s、FileCommand1.291s、
薄工具0.280s、API可用性0.268s，退出码均0。
证据 `physical-api-870a48813048/verified-five-receipt.json`；前述原四目标/fixed/final阶段日志保留。

最新源码额外 App 私有目录定点：UID10116、六项全通过，独立APK SHA256
`78549e27b6f063c6220bf2ea3adb39bec907f695b62a2d5dc50bbc1b04243825`。
证据 `.cache/android-appuid-shell-compat-20261127/device-run/device-receipt.json`，已亲看截图。
APK 已卸载，新增独占临时目录已清理，主 App PID26319 全程不变。现装版本285未覆盖；
这仍不是正常 MaiChat 全界面或真实供应商模型自主流程的验收，iOS/真实旧版Android也未运行。
