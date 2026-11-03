# Android255 合并后最终定点验收

## 操作、产物与预期

2026-11-04，保留并合入远端 `9d037c53` 后，以生产冻结提交
`6ebfc506f767d8c6969b948c8481643304a3f684` 构建正常 Debug App 与 AndroidTest APK。
本轮 Android 版本为 **2.2.18/255**，不是此前合并前的 2.2.17/254。

预期操作是先完成 iOS180 正常模拟器检查及真机构建/安装/启动尝试，再仅向
`emulator-5580` 覆盖安装这两个冻结 APK；运行原 PortraitRegion 四项、远端新增或修改的
图片加载两项和 Graphics 三项，以及三个 UI JVM 类。最后启动正常 MainActivity，核对
安装版本、进程、安装包哈希并实际查看截图。无登录、付费调用、源代码修改或应用数据清除。

Root 在本轮运行前明确放行：其 iOS180 正常模拟器五项 UI 回归 5/5；真机已安装并核对
2.2.4/180、保留公开 Qwen 文件，启动尝试返回 `10002 Locked`。这是 Root 的平台回执，
本轮 Android 执行者没有代做 iOS 安装，也没有绕过锁屏。**真机 GPU 推理仍待解锁验收。**

## 构建与来源绑定

命令在 `MaiChat/android/MaiChat` 执行：

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
CMAKE_BUILD_PARALLEL_LEVEL=2 \
bash ./gradlew --offline --max-workers=2 :app:assembleDebug :app:assembleDebugAndroidTest
```

结果 `BUILD SUCCESSFUL in 8s`，退出 0。构建前、构建后、复制冻结时及运行后的生产源码
哈希均一致，运行时 HEAD 仍为上述冻结提交。期间其他执行者仅更新交付诊断文档。

产物独立保存在 `/private/tmp/maichat-batch255-merged-final-artifacts-20261104`。

| 产物 | 字节 | SHA-256 |
| --- | ---: | --- |
| `app-debug.apk` | 216001182 | `c2f4c8ae5d4209b628da9ccc15ba8b54e3828079452db48687c895477300c2a5` |
| `app-debug-androidTest.apk` | 1013404 | `398742f0d6a8bd4a8f5b327879dbd7f6fec1dfb96091772d45b43a503db0580a` |

两个 APK 均经本机 build-tools 34.0.0 的 `apksigner verify --verbose --print-certs`
验证，退出均为 0。这是本地 Debug 签名包，不代表发布签名或商店分发验收。证书公开摘要、
APK 内各 ABI 动态库哈希、构建命令和源码哈希保存在 `evidence.json` 及签名日志。

构建日志：`/private/tmp/maichat-batch255-merged-final-build-20261104.log`。

## 实际运行结果

仅对 `emulator-5580` 执行正常包及测试包 `adb install -r`，两次均 `Success`、退出 0；
未卸载、未清数据。按原测试类运行，没有修改断言、裁测试或放宽超时。

| 层次/类 | 通过数 | 测试报告时间 |
| --- | ---: | ---: |
| `PortraitRegionInstrumentedTest` | 4/4 | 41.653 秒 |
| `MessageImageLoaderInstrumentedTest` | 2/2 | 0.084 秒 |
| `GraphicsDirectPresenterInstrumentedTest` | 3/3 | 5.282 秒 |
| `MaiIMAttachmentPanelTest` | 2/2 | JVM XML，0 失败/错误/跳过 |
| `MaiImagePreviewTest` | 5/5 | JVM XML，0 失败/错误/跳过 |
| `MaiImageThumbnailViewTest` | 2/2 | JVM XML，0 失败/错误/跳过 |

设备共 9 项、JVM 共 9 项全部通过。JVM 命令为同一离线 OpenJDK17/双 worker 环境的
`:app:testDebugUnitTest`，以三个 `--tests` 精确指定表中 JVM 类；Gradle 退出 0，显示
`BUILD SUCCESSFUL in 4s`。各类 XML 已复制到同一收据目录，没有把其他缓存报告计入本轮。

随后用正常 `com.kongshang.maichat/.MainActivity` 冷启动，`Status: ok`、
`TotalTime: 688` 毫秒，实际 PID **31293**。已装包显示 `versionName=2.2.18`、
`versionCode=255`；分别在设备上计算正常包和测试包 `base.apk` SHA-256，均精确匹配
上述冻结产物。运行后冻结 APK 自身哈希也未改变。

已实际查看 `normal-main.png`：完整登录页可见标志、空手机号框、禁用验证码按钮、未勾选的
年龄/协议框、协议链接和密码登录入口；未观察到空白、错误遮罩或控件重叠。没有输入凭据、
获取验证码或登录。

## 收据与验收边界

同一产物目录包含：

- `evidence.json`：最终构建、源码、签名、冻结 APK 和各 ABI 库的来源绑定。
- `runtime-evidence.json`：安装、各类测试、版本、PID、设备端 APK 哈希及截图观察。
- `portrait-region-tests.log`、`message-image-loader-tests.log`、
  `graphics-direct-presenter-tests.log`：三个真实设备测试日志。
- `jvm-ui-tests.log` 和三个 `TEST-*.xml`：本轮 JVM 实际结果。
- `normal-main.png` 及启动、版本、哈希日志：正常 App 的实际运行证据。

本轮的正常 App 验收止于未登录入口；三类 JVM 测试和合成 Graphics/图片测试不能替代
登录后的聊天、全部导航和手势体验。没有在 Android255 运行本地主模型长上下文推理，
也没有据此声明 Android 或 iPhone 的模型性能通过。

当前 iOS Simulator Metal 缺必要 SIMD 能力的定点证据见
`maichat-local-primary-prefill-20261104.md`。macOS 原生 Metal 的耗时不能替代真实 iPhone；
设备仍锁定，**iOS180 本地主模型真实 GPU 性能及完整正常 App 对话保持待验收**。
