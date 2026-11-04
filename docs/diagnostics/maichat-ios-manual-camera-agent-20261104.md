# iOS 手动相机 Agent 回归（2026-11-04）

用户在正常 App 185 于 12:24 输入“打开相机拍照”。模型调用 `mobile_camera_frames({"action":"open"})`，工具返回 `invalid camera frame action or identifiers`；随后出现打开相机的表述、道歉和手动操作建议，实际没有进入所需的手动拍照流程。预期是打开手动取景，等待用户按快门，只返回一张照片；不能把超过两秒自动采样当作手动拍摄。

根因是 iOS 能力接线和声明契约缺口，不能解释成“本地模型不支持打开相机”：

- 提交 `34936e47` 的 iOS 注册分支排除了 `mobile_take_photo`，Swift 宿主也没有它的 dispatch。已有 `maichat_camera_command` 是每两秒采样的预览能力，不能代替用户按快门拍一张照片。
- 旧 `mobile_camera_frames` 声明 `action` 可为 `list/open/delete_batch`，但只要求 `action`；`open` 缺 IDs 符合广告 schema，执行端却要求 `batch_id` 和 `frame_id`。它的 `open` 指打开已归档帧，不是打开相机。
- 之前的 119 工具基线来自 Mac 条件编译的 Mobile C ABI，包含 iOS 当时没有的手动工具。旧九步输入、输出和判分全部保留，但不能用该相机选择结果验收 iOS 能力。

公开 C ABI 配置捕获使用独立内存存储、iOS 宏、屏幕支持及 RVM/LaMa/ORT/temp/cache 注册条件，不读取用户数据库，不执行宿主或模型。冻结旧目录为 128 项，修复候选为 129 项；当前设备 metadata 为 iOS 27.2，主分支采用 `aiSupportsWholeDeviceScreenCapture()` 对应的 true 条件。false 分支的 124/125 项仅保留为旧 OS 控制，不能冒称当前手机。配置与源码快照见 [旧目录 manifest](/private/tmp/maichat-ios-full-catalog-34936e47-yzgwotep/capture-manifest.json)。这仍是 source-driven 配置捕获，实际手机声明须由 186 设备报告绑定。

修复恢复 `mobile_take_photo({})`，将受信 workspace/session 交给 Swift 的现有手动界面，工具结果返回工作区单张 JPEG，不自动加入 composer 草稿。取消、工具 Task 取消或发起 Session 改变均结束等待，丢弃未完成结果并清理待保存文件。已有“＋/拍摄”入口仍保持向原草稿加入一张照片，不能与工具入口混为一条草稿行为。

归档帧参数改为顶层 Object 下的必填 `request`，其中放完整 `oneOf` 分支；执行和删除审批严格解包，既有 archive 接口仍接收平铺字段。不能直接使用顶层 `oneOf`：当前 XML parser 的 `foreach_parameter` 只枚举 Object 根，AnyOf 根会丢失全部参数，连合法 `list/open` 也拒绝。实际注册 schema 的原生 Qwen35 grammar 验证 **6/6 通过**：缺 IDs 的 open、缺 batch 的删除被拒；完整 open、list、完整删除及手动空参数均接受并解析准确。该验证只加载词表，没有 context、decode、相机或网络执行。

129 项实际注册声明、完整原 system/history、32K/temperature 0/seed 0/非思考/AUTO，保持生产 names-only metadata；没有为此修改模型提示或 metadata 策略：

| 自然输入 | 实际最终调用 | 原生耗时 |
| --- | --- | ---: |
| 打开相机拍照 | `mobile_take_photo({})` | 9.833s |
| 新闻 | `web_search({"query":"新闻"})` | 7.969s |
| 已知重复答复历史后的“我要看今天的新闻” | `web_search({"query":"今天新闻"})` | 9.912s |

这些调用只被观察，没有执行相机或线上检索。旧 128 目录的重复 metadata 名称拒绝、补全描述后仍误选 Frames open，以及失败的顶层 `oneOf` 方案均保留。[完整原生证据](/private/tmp/maichat-ios185-camera-routing-20261104-fkcpbbm0/camera-routing-evidence.json) SHA256 为 `e69e641d54096f00607103c42a4d156b53e0a80e96fd3851b0f89f82baa3de3d`；[实际注册 grammar 记录](/private/tmp/maichat-ios185-camera-routing-20261104-fkcpbbm0/grammar-actual-registered-results.json) 为 `14d67ee432d7163080db0ecab697750747cb6756e95f2a00f9c1922de1003e6e`。

186 模拟器正常 Root 的 Debug opt-in 合成相机回归 **6/6 通过**，测试执行 148.722s、xcodebuild 总耗时 158.014s（约 155s）。[日志](/private/tmp/maichat-camera186-regressions-final-20261104.log)和 [xcresult](/private/tmp/maichat-camera186-regressions-final-20261104.xcresult)保留完整过程；根任务已查看实际截图，清单见 [导出附件 manifest](/private/tmp/maichat-camera186-ui-attachments-20261104/manifest.json)。

| 验证边界 | 已核对结果 |
| --- | --- |
| 正式 Swift 工具入口 | 等待 2.2s 后仍为 pending、0 文件、0 草稿附件；定时采样未启动；按快门后恰好一张 `photo-*.jpg`，MIME 为 `image/jpeg`，后续刷新不增加文件 |
| 用户取消与重试 | 取消结束工具等待、0 文件；再次打开后可以正常按快门得到一张照片 |
| Task 与 Session 改变 | 两种取消均关闭旧请求并丢弃待拍摄结果；新 Session 可以重新拍摄，不污染旧/新草稿 |
| 既有手动入口与界面回归 | “＋/拍摄”不复用定时模式，定时停止后仍能手动拍一张加入原草稿；主模型切换和 busy composer 两项同时通过 |

工具用例源码见 [ScreenFrameArchiveUITests.swift](../../MaiChat/ios/MaiChat/MaiChatUITests/ScreenFrameArchiveUITests.swift)。它们调用真实 Swift 宿主入口和手动 UI，使用模拟器合成画面与独立工作区；这不是传感器拍摄验收，也不是模拟器中完整模型→工具执行的实机闭环。

当前设备产物为 **2.2.4/186**，[设备构建日志](/private/tmp/maichat-camera186-device-build-20261104.log)含 `BUILD SUCCEEDED`。[冻结证据](/private/tmp/maichat-camera186-device-final-artifacts-20261104/evidence.json)记录 11 项严格签名验证退出码全为 0、deep verify 为 0，以及随包模型 1396198496 bytes、SHA256 `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。这些只证明构建与冻结产物核验；本记录尚不签收安装、实机手动 sensor 拍摄或完整手机 QA。

`MaiAgent/` 核心沿用 `34936e47`，archive SHA 仍为 `6cd5308f078a82d91b1735a5942504506098ed1a94ae914dbca2a89376361fa8`，本次没有重跑未变化的 Core8。完整宿主 iOS 宏和通用配置测试各约 10.51 秒、退出 0，覆盖相机注册、归档隔离、引用与取消、截图和既有 Agent 流式/审批/持久化。

完整 iOS 宏 target 初次红灯来自旧测试使用 `TARGET_OS_IPHONE` 而产品注册使用 `MAICHAT_MOBILE_IOS` 的条件差异；保留原红灯后二者统一。只改测试六处平台 guard 和两条注释，没有修改断言或产品文件。原测试源 SHA 为 `de94d175f98c5f9eef484ac0f94cd63b24674a6d426f8c0950672cb4d2e5b6a8`，最终为 `e72a6e76d3903f77e84fd9b473c561d656cd29778a6e13abfd32de65e2e671c0`。原 951 项源码集合保留为 as-built 记录，后续集合另记测试修正；其余 950 项和冻结 App/APK 均未变。

Android 2.2.23/260 已在自有 emulator-5580 安装、冷启动，PID5737；两个设备 APK SHA 与冻结产物精确一致。实际空登录页已查看，没有登录、模型或相机调用，也没有清数据。Desktop 2.2.43 的 9 个 Mach-O、外层签名及隔离无网络 smoke 通过（3.09 秒），原 `/Applications` PID43461 的路径和启动时间不变。平台、完整宿主测试和测试 guard 红绿证据位于 [本轮完整清单](/private/tmp/maichat-camera-platform260243-20261104-bhk9fze9/final-evidence-manifest.json)。

186 冻结设备包的 dylib UUID 为 `3099EBFC-66B1-330C-89DE-B2480C4C7082`。根任务尝试安装时，CoreDevice 返回 **4016**，`devicectl list devices` 显示这台 iPhone 为 unavailable；已请用户重新连接并解锁。没有把该尝试登记为成功：设备最后一次已核验的版本仍是 185，本次 186 未安装、未启动、未运行新设备 QA，不能宣称已完成修复后的传感器拍摄或实机模型调用。

设备检查已读取当前 metadata：iPhone15,3、iOS **27.2**；早期用户报告的 18.7 不能继续作为当前系统版本。186 设备探针改为复用正式 `aiSupportsWholeDeviceScreenCapture()` 及同样的 temp/cache/RVM/LaMa 注册条件，并回传本次实际声明的工具名称。下一步只读取这次独立 probe 的报告，不读取用户会话或凭据；其中选择测试仍不会执行相机或联网工具。

实机 186 操作、截图/录屏、运行进程及最终源码/安装产物绑定由根任务后续追加。原 185 失败与此前严格判分不回写为通过。

## 最终产品提交绑定

对产品提交 `bec74c0ec1a2c8514077011435cd7f62109d910d` 完成只读核对：950 项产品冻结
输入、最终测试源码和 iOS 12 项输入的原始 Git blob SHA-256 全部匹配。旧 as-built 951 项
中的测试文件仍明确记录为 `de94d175...`，不与提交中的 `e72a6e76...` 冒称相同；两套完整
宿主测试的冻结二进制及 green 日志已绑定到后者。没有重新构建 App 或修改产品文件。

两个 Android 260 APK、Desktop 243 全部 242 个普通文件、iOS 186 的 11 个 Mach-O 及
1396198496 字节模型重哈希均与冻结回执一致。既有 xcresult metadata 回读为 6/6 通过；
真机安装回执仍为失败 4016，**186 尚未安装、启动或完成实机相机 QA**，等待用户连接设备。
本次未操作应用、设备或用户数据。

各平台和完整宿主测试目录已写入 `final-source-binding.json`。中央绑定位于
`/private/tmp/maichat-camera-platform260243-20261104-bhk9fze9/final-source-binding.json`，
SHA-256 为 `687472d53a25acaa50a911c7e5b4855669be936413fe24c2e10bf5952ccfb4a4`。
