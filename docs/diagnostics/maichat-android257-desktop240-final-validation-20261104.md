# Android257 / Desktop240 最终冻结内容验收

2026-11-04，针对本地主模型工具发现与 Apple 后端选择修复，构建并验收正常
Android **2.2.20/257** 和 Desktop **2.2.40**。本记录的执行者没有修改生产源码、
测试断言、模型输出预算或超时，没有停止或替换现装 Desktop App。

## 操作、源码与次序

Android 预期操作：在正常 iOS 检查及真机安装/启动尝试之后，仅对自己的
`emulator-5580` 保留数据覆盖安装冻结的正常和测试 APK；启动正常 MainActivity，
核对安装版本、实际 PID、设备端 APK SHA，并查看真实截图。Desktop 预期是构建正常
`maichat`，独立复制并严格验签，在自己的用户目录及禁止真实 home 写入的 sandbox
内执行原有 `--smoke`，不进入真实账号登录或主窗口。

最终内容冻结回执为
`/private/tmp/maichat-local-primary-model-20261103/sim-metal-gate-production-freeze.json`，
10 文件内容集合 SHA-256：
`b3eb0dd454fec47a43034802cd87d6665277a87f18be956e51a4298a85a8628d`。
Android 对 932 个相关生产及测试文件保存哈希，构建、冻结、运行前后均相同；
Desktop 在构建、冻结和 smoke 前后核对该 10 文件及 smoke 调用链源码。

构建时基准 HEAD 为 `387683b937edb734e6af549421c8100b044b8b68`，生产变更尚未提交。
最终生产提交绑定待根任务提供精确提交后核对补记；当前不把基准 HEAD 宣称为产物源码。

根任务放行本轮 Android 运行时已回执：正常 iOS183 模拟器 7 项 UI 全通过，包含真实
两轮回复和重启；正确 CPU 回退下总耗时 70.260 秒。183 真机包的 11 个 Mach-O strict、
外层 deep strict 及随包 491 MB 权重已核对，实际安装、启动均成功。
本轮放行时真机 GPU 两轮 probe 的 JSON 尚未完成，本记录不据此推定推理通过。
此前 182 的安装失败属于旧检查点，不能替代或覆盖此次 183 的成功回执。

## Android 构建、签名与实际运行

在 `MaiChat/android/MaiChat` 执行：

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
CMAKE_BUILD_PARALLEL_LEVEL=2 \
bash ./gradlew --offline --max-workers=2 :app:assembleDebug :app:assembleDebugAndroidTest
```

退出 0，`BUILD SUCCESSFUL in 21s`，67 项任务中 25 项执行、42 项复用。
两个 APK 均经 build-tools 34.0.0 的
`apksigner verify --verbose --print-certs` 验证，退出均为 0。
这是本地 Debug 签名，不是发布签名或商店分发验收。

| 冻结产物 | 字节 | SHA-256 |
| --- | ---: | --- |
| `app-debug.apk` | 216039362 | `7ea9f65c41ebb43d17bf949cced3a15feacb0320604e2edb5d62d0c8c5d4a36b` |
| `app-debug-androidTest.apk` | 1013404 | `65e185713ca589ffc20d9433ff16a2f7b080b6c64fb57f18df95ee91d797105c` |

正常包 manifest 回读为 `versionName=2.2.20`、`versionCode=257`。
只对 `emulator-5580` 执行两个冻结包的 `adb install -r`，两次均 `Success`；
未卸载、未清数据。随后停止该模拟器上本轮自己的包进程并正常冷启动
`com.kongshang.maichat/.MainActivity`，`Status: ok`，实际 PID **3475**。
设备回读版本为 2.2.20/257，正常与测试 `base.apk` 的设备端 SHA 均精确匹配表中冻结包。

已实际查看 `normal-main.png`：完整登录页包含标志、空手机号框、禁用验证码按钮、
未勾选的年龄和协议框、可读协议链接及密码登录入口；没有观察到空白、错误遮罩或控件重叠。
没有输入凭据、获取验证码或登录。

最终 257 与前一 256 检查点的源码差异只有 `MaiLlamaTools.cpp`、
`MaiLlamaModelClient.h` 和 Android 版本配置。Apple 能力选择调用受 `__APPLE__` 宏控制，
但显式空设备列表也用于其他平台的 CPU 配置，不能宣称整个修改在 Android 上被忽略。
实际 APK 的 30 个原生 `.so` 中 28 个与 256 字节相同；变化的仅是两个 ABI 的核心库：

| 257 原生库 | SHA-256 |
| --- | --- |
| `lib/arm64-v8a/libmaichat_agent.so` | `cbf0f4c79d49ff28d33c353fd5f4b244a56e30aa78818bdf35b76330e114e283` |
| `lib/armeabi-v7a/libmaichat_agent.so` | `e7867b0675ed96a857ca504479d41400790853c13d0a6e0ee503e6e1fc0973f2` |

原 ROI、Graphics、ImageLoader 测试及对应功能源码未改变，根任务明确要求不重跑。
其真实结果保留在 **256 检查点**，没有算成 257 再跑过的测试：

| 256 原测试类 | 结果 | 实际命令耗时 |
| --- | --- | ---: |
| `PortraitRegionInstrumentedTest` | 4/4 | 43.74 秒 |
| `GraphicsDirectPresenterInstrumentedTest` | 3/3 | 5.92 秒 |
| `MessageImageLoaderInstrumentedTest` | 2/2 | 0.73 秒 |

三类 UI JVM 测试及对应生产源码也与已有通过回执一致；本轮没有重复 JVM 测试，
没有重复运行共享核心测试。257 本轮范围是新产物构建、签名和正常入口安装/运行绑定，
不包含 Android 本地主模型推理或性能验收。

## Desktop 构建、严格签名和隔离 smoke

执行原 Release 构建目录的正常目标：

```sh
cmake --build /private/tmp/maichat-desktop-agent-window-build-20261102 --target maichat -- -j2
```

构建退出 0。将正常 2.2.40 App 复制到独立冻结目录后，按需本地 ad hoc 签名；
9 个 Mach-O 的逐个 strict 验证及外层 deep strict 均通过。冻结主程序 SHA-256 为
`585b44274995160b08295bb7b3d8c30412a90b3fc3e8ac26c1720d0dd469f5c8`，
242 文件哈希清单 SHA-256 为
`e3609b2e8816a2f56601ff516686363d8f04795001eb9a4da5b2d28059002084`。

使用独立 `CFFIXED_USER_HOME`、`TMPDIR`，并由 sandbox 禁止真实用户 home 的全部
`file-write*` 后，直接执行冻结 App 的原有 `--smoke`；PID **14352**，退出 **0**，
耗时 **2.72 秒**，日志只写入自己的用户目录。静态调用链核对确认这条路径以 300 ms
拒绝登录窗口，跳过 `LoginDialog::start`、真实账号登录、MainWindow 和 LocalSettings；
没有模型请求，也没有读取账号令牌。

Qt 探针仍发现 `QDir::homePath` 和 native QSettings 文件名指向真实 home。
这项路径回退边界保留在回执中；本轮使用的 AppData/cache/config 已指向隔离目录，
真实 home 写入由 sandbox 拒绝。shared plist 仅做 stat，前后元数据相同，未读取内容。
现装 `/Applications/MaiChat.app` 的 PID **43461** 前后相同，没有停止或替换。
smoke 前后全部冻结 App 文件哈希及 10 个冻结源码哈希相同。

这是正常 Desktop 的共享核心构建及封装回归，不能替代 Desktop 主模型 UI、推理、
完整可分发包或发布签名验收。

## 收据位置

- Android257：`/private/tmp/maichat-android257-final-artifacts-20261104/`，包含
  `evidence.json`、`runtime-evidence.json`、两个 APK、签名/manifest 日志、30 库比较、
  安装/启动/版本/设备 SHA 日志和实际查看的 `normal-main.png`。
- Android 构建日志：`/private/tmp/maichat-android257-final-build-20261104.log`。
- 256 原 9 项结果：
  `/private/tmp/maichat-android256-discovery-final-artifacts-20261104/runtime-evidence.json`。
- Desktop240：`/private/tmp/maichat-desktop240-validation-20261104/`，包含冻结
  `artifacts/MaiChat.app`、`evidence.json`、`artifact-file-sha256.json`、签名日志、
  隔离路径探针、sandbox 规则、smoke 日志及 `validation-summary.txt`。
