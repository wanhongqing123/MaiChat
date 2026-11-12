# Android 真机安装最新代码

## 需求与实际设备

2026-11-12，用户要求将 MaiChat 安装到 Android 真机。预期为安装最新代码的普通 App，
保留已有数据，实际读回版本、安装包身份、正常启动进程并查看画面。

首次 `adb devices -l` 为空；用户要求再次检查后，OPPO PKD130 已授权连接，
实际 Android 15、ARM64。当前用户为 0，指定包名查询为空，`dumpsys package`
明确返回 `Unable to find package: com.kongshang.maichat`，因此本轮为首次安装。
没有卸载、清数据、读取真实数据库或执行模型请求。

## 源码、正常构建与中间问题

- 构建源码：`15074abadcdd33db56a00be2e779fa70e1ccef60`。
- MaiChat tree：`29d8bfb555d5fb5fe46ca5dac1a852f2b19fcc1b`。
- Core tree：`30114b3e33472ba1692987e0a90df11f81d853df`。
- 普通 Release：`com.kongshang.maichat`，源码版本 `2.2.32 / 274`，未改版本源。
- JBR 17.0.14、SDK 34、NDK 27.3.13750724、CMake 3.22.1；复用原
  `app/.cxx/mobile-agent-adapter/RelWithDebInfo/3k666l63/arm64-v8a` cache。

首个本机驱动把 SDK stderr 警告当作 PowerShell 5 终止错误；改为子进程内重定向并保留原日志。
随后普通组装暴露旧 cache 的 `MAICHAT_MOBILE_TESTS=ON`，而源码默认为 OFF。
Gradle 自动选择了独立测试 executable；其中两个测试因缺少 sqlite3.h 编译失败，生产 JNI 已链接成功。
没有修改生产代码或把这些测试记作通过。

本轮私有 init 在原 ABI、第三方 seed、运行库设置之外，显式选择全部十五个生产 shared target，
包含七个 CPU module，不构建独立测试 executable；未改 CMake 参数、功能 flags 或新增 native cache。
最终普通 `assembleRelease` exit 0 / 33.011s。日志含增量复用，不宣称所有对象均在本轮重编。
旧测试缓存问题没有修复；本轮交付范围为正常 App 安装。

## APK 校验

正常 Release 使用既有本机安装证书签名，非官方发布操作。zipalign、签名 v2/v3 校验通过；
实际 badging/manifest 核准包名、`2.2.32 / 274`、仅 ARM64，没有 testOnly/debuggable。
生成的 Release BuildConfig 中 DEBUG 与 USE_LOCAL_UI_AUTH 均为 false。

完整二十六个 ARM64 so 与本轮 stripped 输出逐 SHA 相同；可用的 native OBJ BuildID 核等，
七个 ggml CPU module 齐备，无 NDK libz stub。JNI 中 reader factory 有定义，
包内实际 JNI 同时含 tool_output_read 及新的空答收尾提示。
固定 GGUF 完整 1396198496 bytes、SHA256
`57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`，ZIP 中未压缩；
manifest、来源、模型卡、许可、人像模型与相关必需资源检查通过。

最终签名 APK 为 1598119483 bytes，SHA256：
`3a351811f41ec8aee60ecf8bad60a6691325621183575390593c3e5fbfa113ea`。
本机安装证书 SHA256：`87704a9ff5f7394186ca146b6303485abdc8fc594a4f8d2da9aaddc87ea00ea5`。

## 实际安装与启动

使用 `adb install -r --no-incremental`，exit 0 / 119.247s，原输出明确 `Success`。
亲看系统安装页确认应用与版本后点击“继续安装”；安装完成后的系统增强防护推介选择取消，
没有更改全局防护设置或逐项授予应用敏感权限。

设备实际包信息读回 `2.2.32 / 274`，通过 pm path 定位实际 base.apk，
设备端 SHA256 与上述签名 APK 完全相同。
普通无测试参数启动 MainActivity，`Status: ok`、COLD、TotalTime 1718ms；
PID 28298、/proc cmdline 与包名匹配，随后再次读回相同 PID、前台为 MainActivity，
该 PID 的 crash buffer 无记录。

已实际查看新启动截图：正常手机号登录页，没有空白或崩溃；未输入账号、获取验证码、
勾选年龄/法律条款或登录真实账号。安装与正常入口启动完成，不扩大为全部功能或本地模型性能验收。

本机原构建、打包、签名、安装、设备回执及截图保留在忽略目录
`.cache/android-device-install-20261112/`。未修改产品源码或版本；本诊断单独提交，原 `.gitignore` 修改保留。
