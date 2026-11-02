# 新仓库 Android 真机整包构建与安装

## 需求与验收操作

- 时间：2026-11-02，Windows 主机；目标为 OPPO PKD130、Android 15 真机。
- 用户要求使用整个当前 MaiChat 仓库构建正常 App 并安装。用户随后明确接受
  卸载旧版后新装，不再等待原发布签名；此授权来自本轮消息，不沿用历史卸载授权。
- 源码：`E:\OpenSource\MaiChat`，`main` 提交
  `1997947219596e6520be27ed892bd6ea1820ff2e`。
- 目标包名：`com.kongshang.maichat`，源码版本 `2.2.6 (238)`；不增加隔离包名。
- 预期：完整 App、共享 C++ 核心和 JNI 从当前源码编译链接；构建后核对 APK
  版本、ABI 和运行时，安装后回读设备 APK 哈希、版本及进程，亲看正常启动画面。
  本次不通过付费生成或真实电话验证功能。

## 安装前设备状态

- ADB 原有连接一度 offline，执行 `adb reconnect offline` 后恢复为 device。
- 真机原装 `2.2.3 (223)`，更新时间 2026-10-20。
- 拉回原装 APK 的大小为 159805729 字节；原证书 SHA-256 为
  `84187afba7b96fc21b8b4af6c658a56b2b4223749db197cb18c442d5bfe54256`。
- 本机没有发布签名配置。用户已明确按卸载后新装处理；新包使用本机安装签名，
  不申请或传输发布私钥。

## 构建环境与证据

- JDK `C:\Users\18034\.jdks\jbr-17.0.14`；Gradle 7.5.1、AGP 7.4.2。
- SDK 34、Build Tools 34.0.0、NDK 27.3.13750724、CMake 3.22.1。
- Android 工程：`MaiChat/android/MaiChat`。CMake 在新目录生成后，由 Ninja 实际
  编译、链接当前 ARM64 的 `maiagent-console`、`maiagent_graphics`、
  `maiagent_obs_gles`、`maichat_agent`、`maichat_ffplay_hosted` 五个目标。
  未复用旧 `libmaichat_agent.so`；此包针对本次 OPPO 的 ARM64 ABI。
- 新旧仓库 OpenSSL、ONNX Runtime、FFmpeg、dav1d、x264、LAME 的 Git tree
  逐项一致。旧工作区源码提交为 `aae043b87422bcf84b0a4a5ae8d9ceebeb8b6754`。
  只复用相同 ABI、NDK 和 RelWithDebInfo 的第三方导出库及生成头。
- 两 ABI ONNX Runtime 复制后分别校验 SHA-256；每 ABI 15 个 OpenSSL/FFmpeg/
  编码库静态归档复制前后均校验 SHA-256，未迁移旧 CMake 构建树。
- 首次配置因本机 Perl 缺 `Locale::Maketext::Simple` 失败。失败后的 AGP 重配
  会删除原生配置目录，预先复制的依赖也被删掉。因此使用仅在忽略目录保存的
  CMake project hook，在 AGP 清理后复制并校验依赖，再编译新源码。
- 本机构建配置：`.cache/android-install-19979472/local-build.init.gradle`、
  `seed-dependencies.cmake`；日志为同目录 `assemble-release-exports.log`。
- Ninja 首次仍尝试重建已有 FFmpeg 导出库，因本机 Windows 宿主编译器没有
  MSVC 头文件环境而报 `ctype.h` 缺失。仅将生成规则中该第三方任务改为
  `verify-ffmpeg-cache.py`：核对新旧源码 tree、未修改状态与每个归档哈希后复用。
  App、JNI、共享核心及图形目标保持正常编译，最终原生构建返回 0。
- AGP 再执行外部构建会重新生成上述第三方规则，因此最终先完成独立 Ninja
  原生构建，再用 Gradle `:app:assembleRelease -x :app:externalNativeBuildRelease`
  组装本次新产物。没有把跳过原生任务当作编译证明。
- APK 检查发现单独构建后 AGP 未自动收集 `libc++_shared.so`，在本机打包配置中
  显式附带同 NDK 的运行库，并把 ABI 限为 ARM64，避免留下不完整的 v7a 目录。
  安装前确认 15 个原生库、RVM 模型和 QPDF 许可齐全；六个关键库与本次
  stripped 输出逐项哈希一致。
- OpenSSL、FFmpeg 编码库与 ONNX 为同源构建产物复用，不能称为本次重新编译。

## 安装失败定位及修正

- 第一次设备定向打包使用 `android.injected.build.abi`，AGP 为 APK 添加
  `android:testOnly=true`。OPPO 安装页提示“安装包是测试版本”，ADB 明确返回
  `INSTALL_FAILED_TEST_ONLY`；这不是签名不匹配。
- 取消 IDE 设备定向参数，通过本地 ABI 配置正常打包，并显式设置
  `android.injected.testOnly=false`。安装前用 `aapt dump xmltree` 核对标记
  不存在或为 false，再执行普通的 `adb install --no-incremental`。
- 正常包安装前 `pm list packages com.kongshang.maichat` 为空，旧版已由用户
  卸载；本轮没有执行 `adb uninstall`，也没有安装第二个包名。

## 最终产物与真机验证

- APK：`.cache/android-install-19979472/MaiChat-Android-v2.2.6-19979472-local.apk`。
- 大小：100116156 字节；SHA-256：
  `a4cbbd2fae665f8098b5581939b83f14a733cf9a92baae0b9a8ca354aef20aa7`。
- 包名 `com.kongshang.maichat`，版本 `2.2.6 (238)`，仅 ARM64；本机安装签名，
  APK v2/v3 校验通过，`testOnly` 为 false/不存在。
- 最终 Gradle 组装 `BUILD SUCCESSFUL`；普通 ADB 安装返回 `Success`。
- 设备回读 `versionCode=238`、`versionName=2.2.6`，更新时间
  `2026-11-02 23:16:39`。从设备拉回 `base.apk` 后，大小与完整 SHA-256
  和本地候选逐项一致。
- `am start -W` 返回 `Status: ok`，实际进程 PID 22213，前台为
  `com.kongshang.maichat/.MainActivity`。回读为 WARM 启动，不冒充冷启动。
- 亲看真机 720×1604 截图：正常手机号登录页、验证码入口、协议和密码登录
  均可见，没有黑屏、空白或安装错误页。同 PID 的 crash 缓冲区为空。
- 本次只完成安装与启动验收，尚未登录真实账号验证语音、会话同步、媒体和
  付费模型任务；没有发布 GitHub Release 或替换 Windows 安装版。
