# 完全访问模式下网页检索仍逐次审批

## 现场操作与预期

- 记录日期：2026-11-04，Asia/Shanghai。
- 用户提供的云端主模型会话截图显示，权限已设为 `FullyAccess`，调用一次 `current_time` 后，`web_search` 仍停留在等待批准；参数为 `{"count":15,"query":"今日新闻 头条 要闻"}`。
- 复现操作：选择完全访问，要求检索今日新闻，待模型取得当前时间后调用上述搜索；同一轮继续抓取结果页和补充搜索。
- 预期：网页搜索、网页抓取属于普通只读工具，无需用户逐次批准，能够继续返回检索结果。实际购买、付款和付费媒体生成提交仍保留既有确认。
- 截图对应的安装包版本、源码提交与运行进程未在本记录中实机核验，不能由截图推断其与当前源码完全一致。

## 平台、源码和验证产物

- 排查与本地回归平台：macOS 27.2（26B123），arm64。
- 仓库：`main`；修改基线为 `404768de1a0a19358faa97566b73ffcb9d90b60d`，本记录生成时尚未提交。
- 构建目录：`/tmp/maichat-pdf-core-gate`；Release，`MAIAGENT_BUILD_TESTS=ON`，`MAIAGENT_BUILD_GRAPHICS=OFF`，使用当前仓库 `MaiAgent` 源码。
- 本次运行的是该目录新编译的单元测试进程，不是手机或桌面产品进程。

冻结的三个源码文件 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `MaiAgent/AgentTools/MaiDeepSeekWebTools.cpp` | `7ab90fc042ed1bd65de691884db83f2956fb4c33dbcbceaaf3ff2b33eb66ef13` |
| `MaiAgent/include/MaiDeepSeekWebTools.h` | `fad8b2c63827b547370f2ab596092739e551ccefb3c62025db2b1e2f435044be` |
| `MaiAgent/tests/MaiDeepSeekWebToolsTests.cpp` | `d22bcd94074fd9d77601b0d45867878379d8aa7c8222fed8d4a6fea090949a74` |

关键运行产物：

| 产物 | 修改时间（CST） | SHA-256 |
| --- | --- | --- |
| `/tmp/maichat-pdf-core-gate/bin/MaiDeepSeekWebToolsTests` | 2026-11-04 10:49:51 | `efb31192362cd0489829110357b5f8595fc48a6380b0fc36a4b9677b3f28ed5d` |
| `/tmp/maichat-pdf-core-gate/bin/MaiPermissionTests` | 2026-11-04 10:48:55 | `b07601b471c005698485bb7f5e01e812588f3234014528b8ee78e0cc90792300` |

## 根因与最小修复

`DeepSeekWebTool` 同时将 `requiresApproval()` 和 `requiresPaidApproval()` 固定返回 `true`。`MaiTurnRunner::toolNeedsApproval()` 先检查付费确认，再检查 `MaiApprovalPolicy::Never`，所以即使完全访问已正确传入，搜索和抓取仍进入逐次确认闸门。原测试也将两种只读检索声明为付费提交，相关实现最初来自 `5216279a`。

用真实注册的 DeepSeek 工具、`MaiAgent` 和进程内假模型构建最小复现，能够独立于设置持久化或手机环境重现：模型依次调用现场搜索、网页抓取和补充搜索；原实现产生权限事件，供应商假服务收到零个请求。未修改设置就能复现，说明该分类足以导致截图中的现象；本记录不据此声称已经排除所有宿主设置问题。

最小修复仅删除 DeepSeek 搜索、抓取的两个审批覆盖，沿用普通只读工具的默认权限。同步更新公开头与测试注释：供应商按次收取检索 API 使用费，不把它归类为购买或媒体任务提交。参数校验、动态凭据读取、客户端直连、TLS、错误分类、有界重试和输出限制继续由原实现负责。通用付费审批规则以及各媒体工具的付费标记均未改动。

## 红绿对照与策略矩阵

先只加入预期行为断言，保留原实现，运行 `MaiDeepSeekWebToolsTests`：共 10 处预期失败，包含搜索/抓取审批元数据及三个策略下权限事件与请求计数。随后应用上述工具分类修复，同一用例通过。

| 策略 | 连续搜索、抓取、补充搜索 | 权限事件 | 本地供应商请求 |
| --- | --- | --- | --- |
| `Never`（完全访问） | 完成 | 0 | 3 |
| `UnlessTrusted` | 完成 | 0 | 3 |
| `OnRequest` | 完成 | 0 | 3 |

额外通过实际 `seedream_image` 的 `delegate` 调用验证边界：即使 `Never`，也产生一条不可记为会话授权的确认请求，测试在事件中拒绝后结束。此用例使用空凭据；即使审批链回归失效，也不会提交真实付费请求。原 `MaiPermissionTests` 中完全访问下付费提交逐次确认、不接受会话放行的用例也通过。

## 验证记录

- `MaiDeepSeekWebToolsTests`、`MaiPermissionTests`、`MaiArkMediaToolsTests`、`MaiGlmMediaToolsTests`、`MaiCreativeMediaToolsTests`、`MaiModelStudioToolsTests`：6/6 通过，38.96 秒。
- 原有八套核心回归：`MaiCoreTests`、`MaiFilePathTests`、`MaiStoreTests`、`MaiModelClientTests`、`MaiTurnRunnerTests`、`MaiToolTests`、`MaiToolLoopTests`、`MaiPermissionTests`，8/8 通过，183.52 秒。
- 三个 C++ 文件已使用 `/opt/homebrew/opt/llvm/bin/clang-format -i --style=file`；`git diff --check` 通过。
- 红灯日志：`/tmp/maichat-full-access-web-red-test-20261104.log`。
- 六套定点回归日志：`/tmp/maichat-full-access-web-green-test-20261104.log`。
- 八套核心构建与回归日志：`/tmp/maichat-full-access-web-core8-build-20261104.log`、`/tmp/maichat-full-access-web-core8-test-20261104.log`。

## 正常产品构建与入口回归

本阶段仍以 `404768de1a0a19358faa97566b73ffcb9d90b60d` 加本记录所列三个文件为权限修复检查点；根任务仅额外提高各平台构建版本。未改动本地模型权重、模型路由或其他产品行为。

### iOS 2.2.4 / 184（根任务回执）

根任务完成正常 iOS 模拟器 3/3 UI 回归，并导出、查看忙碌时仅保留加号、行内模型切换和权限紧凑展示截图；结果位于 `/private/tmp/maichat-webapproval184-regressions-final-20261104.xcresult`。这些 UI 回归没有实际访问供应商搜索或抓取接口。

真机包构建成功，冻结目录为 `/private/tmp/maichat-webapproval184-device-final-artifacts-20261104/`。已读取其 `evidence.json`：本记录三个源码 SHA 均精确一致；11 个 Mach-O 单体严格验签全部退出 0，根任务另回执外层 deep/strict 验签通过。`MaiChat.debug.dylib` UUID 为 `F62F17C9-2646-3F18-85EA-D40C0164D4F1`，SHA-256 为 `14111e09deb5766409e7d303366320012cbb7c74cada491a71067c7bf2256504`。随包模型仍为 491400032 字节，SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`。

根任务尝试安装 184 时 CoreDevice 返回 4016，设备不可用；已异步提示重新连接、解锁，待设备恢复。该安装尝试不算安装、启动或真机验收成功。以下 Android 运行在此真机尝试完成、根任务明确放行后进行。

### Android 2.2.21 / 258

在 `MaiChat/android/MaiChat` 使用已有 JDK17 执行：

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home \
CMAKE_BUILD_PARALLEL_LEVEL=2 \
bash ./gradlew --offline --max-workers=2 :app:assembleDebug :app:assembleDebugAndroidTest
```

构建退出 0，25 秒，67 项任务中 25 项执行、42 项复用。正常包和测试包复制到 `/private/tmp/maichat-android258-permission-artifacts-20261104/` 后，分别通过 build-tools 34.0.0 的 `apksigner verify --verbose --print-certs`。正常包 manifest 为 2.2.21/258；均为本地 Debug 签名，不是商店发布包。

| 冻结产物 | 字节 | SHA-256 |
| --- | ---: | --- |
| `app-debug.apk` | 216038934 | `bf2f64fee216e1573295182ab5e418accd5727d53904ddeaaa6ce8be136dfb5f` |
| `app-debug-androidTest.apk` | 1013404 | `e16be5d9918769848506e690d0680b7c43953b262958406f7f4dca87f6017a18` |

仅对本轮自己的 `emulator-5580` 执行两个冻结包的 `adb install -r`，均返回 `Success`，没有卸载或清数据。停止该模拟器上的本轮包进程后，正常冷启动 `com.kongshang.maichat/.MainActivity`，返回 `Status: ok`，实际 PID 为 **4492**。设备回读版本为 2.2.21/258，正常与测试 `base.apk` 的设备端 SHA 均与表中冻结 APK 精确匹配。

已实际查看 `normal-main.png`：正常空登录页完整显示标志、手机号框、禁用的验证码按钮、未勾选的年龄与协议框、可读法律链接和密码登录入口，没有空白、重叠或错误遮罩。未输入账号、获取验证码或登录。本阶段未运行 Android 模型推理或实际网页检索。

与 257 冻结输入比较，原 932 项输入中只有 `MaiDeepSeekWebTools.cpp`、`MaiDeepSeekWebTools.h` 和 Android 版本配置变化；30 个 APK 原生库中，只有两个 ABI 的 `libmaichat_agent.so` 变化，其余 28 个字节相同。

Android 构建日志：`/tmp/maichat-android258-permission-build-20261104.log`。产物目录内 `evidence.json`、`runtime-evidence.json` 记录源码、签名、版本、安装、启动、设备 APK SHA 和截图；未把前一检查点的旧功能测试记为本轮重新运行。

### Desktop 2.2.41

使用原 Release 目录执行 `cmake --build /private/tmp/maichat-desktop-agent-window-build-20261102 --target maichat -- -j2`，退出 0。将正常 App 复制到自己的冻结目录 `/private/tmp/maichat-desktop241-permission-validation-20261104/artifacts/MaiChat.app`，未替换 `/Applications/MaiChat.app`。

冻结 App 的 9 个 Mach-O 逐个 strict 验签与外层 deep/strict 验签均通过，按需仅对冻结副本做本地 ad hoc 签名。主程序 SHA-256 为 `8c13a34397ea31238f760f01b55afc6a3d39833d6f0436637fdc14db6e8dd509`；242 个普通文件哈希清单 SHA-256 为 `32b67da66309c4f22a532a90e1e0534c313a6c8c0c2cb1570b3f9377f5efb976`。

复用既有 Qt 用户路径探针，并确认 `main.cpp`、日志、认证、SecureRefreshTokenStore、登录窗口这五项安全 smoke 调用链源码与 2.2.40 的已审查版本相同。使用独立 `CFFIXED_USER_HOME`、`TMPDIR`，另由 sandbox 拒绝真实 home 的数据读取及全部写入，并拒绝全部网络，直接执行冻结 App 原有 `--smoke`。PID **43412**，退出 **0**，耗时 **3.29 秒**；日志版本为 2.2.41，日志路径位于自己的隔离目录。此路径 300 ms 后拒绝登录窗口，在账号登录、MainWindow、用户会话和模型初始化前退出，没有读取用户令牌或数据库。

Qt 探针显示 AppData/cache/config 已指向隔离目录，但 `QDir::homePath` 和 native QSettings 文件名仍报告真实 home；该路径边界保留，真实 home 的数据读写由本轮 sandbox 明确拒绝。未检查用户偏好文件内容。smoke 前后 939 项冻结源码哈希及 App 全部普通文件哈希一致；原用户桌面进程 **PID43461** 的启动时间与路径前后相同，没有停止、替换或重启。

Desktop 构建日志：`/tmp/maichat-desktop241-permission-build-20261104.log`。冻结目录内包含 `evidence.json`、签名日志、`artifact-file-sha256.json`、Qt 探针日志、sandbox 规则和 `normal-smoke.log`。源码快照为 `/tmp/maichat-permission-platform-source-freeze-20261104.json`，939 文件内容集合 SHA-256 为 `d4e33b0f8bf55f6ebe47a2b40c71f13411ab8671b5a0ffd3cccf18eb0b08b915`。本阶段证明正常目标构建、签名及隔离启动，不代表已安装发布包、notarization 或主模型 UI 闭环验收。

## 验证边界

核心搜索、抓取响应均来自 loopback 合成服务，凭据只使用进程内假值；没有访问付费供应商或使用真实 API Key。产品阶段执行了正常 iOS 模拟器 UI、Android 自有模拟器安装与登录入口查看、Desktop 隔离 smoke；iOS 真机安装仍受设备不可用阻断。本记录证明当前源码中的权限分类、核心执行链、产品构建和上述入口回归通过，未验证真实模型自行规划检索、实际供应商联网检索或该修复的手机闭环体验，不宣称已完成真机验收。
