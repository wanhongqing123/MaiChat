# Chromium base 文件后端核对

日期：2026-11-27。用户要求核对 C++ 文件接口在 Android/iOS 的实现方式，避免把“能调用系统 API”
误说成“每个 Android 版本、文件系统和权限下每种操作都能成功”。

## 固定参照与实际调用链

本机只读参考 `E:/OpenSource/chromium/src`，固定提交
`6e988ed832ca0bb7ba6ca46a710b6a7b33e1fbc5`；核对文件无工作区改动。

| 资源/操作 | 源码事实 |
| --- | --- |
| 普通路径打开 | `base/files/file_posix.cc:617–636` 先处理 Android Content URI/虚拟文档，其余路径直接调用 C++ 中的 `open()` |
| 普通复制 | `base/files/file_util_posix.cc:1412–1424` 创建两个 `base::File` 并复制内容；不是统一包装 Java 文件复制 |
| 普通移动 | 同文件 `1479–1507` 的 `MoveUnsafe` 先调用 `rename()`，失败才复制并移除原路径；该语义允许替换，与我们的无覆盖/外部不删除策略不同，不能直接照搬 |
| Android URI | `base/files/file_android.cc`、`base/android/content_uri_utils.cc:61–80` 经 JNI 调 `ContentUriUtils.java`，取得并复制 FD，同时保持 Java 描述符的生命周期 |
| Android 文档授权 | `ContentUriUtils.java` 使用 ContentResolver、ParcelFileDescriptor、DocumentsContract；不能把 URI 当普通 POSIX 路径 |
| Apple 普通复制 | `base/files/file_util_apple.mm:20–26` 调系统 C 函数 `copyfile()`；文件后端也不要求全部用 Swift |

核心结构是共享 C++ 接口，按资源类型选择普通文件后端或平台授权后端。Swift/Java 是调用平台
框架的一种适配语言，不是普通系统文件 API 的唯一调用语言。C++ 调普通接口同样受到 App 沙盒、
文件权限和系统策略限制，不会因此取得相册/其他 App 文件的访问权。

[Android NDK 官方说明](https://developer.android.com/ndk/guides/stable_apis) 明确提供 C/C++ 原生 API，
同时要求处理高于 minSdk 的 API 可用性；这不意味着 Android 完整实现任意 POSIX 扩展。
本项目也不能用一台设备通过来保证所有 Android。硬链接和原子无覆盖重命名必须区别于基本读写，
按实际支持情况处理失败，不能退成可能覆盖目标的“先检查再普通 rename”。

## MaiChat 当前对应范围

- 普通 App 文件：共享 C++ 仿真脚本和 MaiFileSystem，移动模型不再暴露 file_*；不启动系统 Shell。
- 平台授权资源：相册/文档已有读取与导入副本能力；通用目录授权挂载仍未实现，不能宣称完整
  Content URI/虚拟文档后端已经与 Chromium 等价。外部原件不开放删除、移动、覆盖。
- 本轮 Android 普通文件发布/移动优先内核 NOREPLACE；不支持时的旧 Java 发布兼容路径仍保留
  已有的并发限制。已有目标、跨文件系统和权限拒绝不会进入该回退。
- 参考 Chromium 的接口分层、资源识别和平台适配，不复制其允许覆盖/复制后删除源的移动语义。

## 对应真机证据

PKD130、Android 15/API35、ARM64：四个最终原生目标全部 exit 0，日志在
`.cache/arm64-mobile-file-contract-20261127/physical-5922d012b2d0`。
首次两个失败及诊断在 `physical-9a782e802fa5`，没有被覆盖。

额外独立测试 App 在 UID10044 的私有目录执行同一已验证 Core/工具，六项检查全部通过，
包括实际 mkdir/echo/cat/cp/mv/find/read-text、补丁及越界拒绝；没有启动子进程。
界面截图与 JSON 在 `.cache/android-appuid-shell-verify-20261127/device-run`。
该定点 APK 不替代原四套测试，也不代表现装 MaiChat 的完整 UI/模型自主流程已经验收。
测试包已卸载，现装 MaiChat PID26319 保持不变，未覆盖 APK、清数据或访问用户相册/文档。

## 新版接口的可用性收口

本机 NDK27 的 `sysroot/usr/include/stdio.h:200–201` 将 renameat2 标为 API30 起提供，
而项目 minSdk26。最终 Android 实现改为从公开 libc 动态解析该接口，不强链接新版符号，
也不试探原始 syscall：旧系统的 seccomp 拒绝可能终止进程，不能只指望返回 ENOSYS。
普通文件缺接口时进入既有明确宿主适配；没有适配则失败。目录原子发布缺接口时返回
NotSupported 并保留暂存目录，不退化成非原子覆盖。移动相对目标无父段按当前目录核对。

测试只在当前 Android15 设备模拟新版公开符号缺失，**不是**真实 Android26 系统验收：
旧未探测版本 15 处断言失败；加入探测后通过；补充相对路径案例先复现 2 处失败，再修正。
最终 `.cache/arm64-mobile-file-contract-20261127/physical-api-870a48813048/verified-five-receipt.json`
记录五个目标全部 exit0，API 用例为 failures=0、lookups=1、fallback_calls=3。
该独占目录已清理，原始 RED/后继 GREEN 日志在 PC 保留。

同一最新 Core `08feaba90adeddb8453e5d196ce1bfb1f05038e932fc6449b875b439bf620b9f`
再次进入独立 APK 的 App UID10116/私有目录，六项定点全通过；截图与回执在
`.cache/android-appuid-shell-compat-20261127/device-run`。测试包已再次卸载，主 App PID26319 不变。
独立直接编译 POSIX 文件系统的 Lifecycle 目标补链 libdl，并真实编译链接通过，未冒称设备运行。
