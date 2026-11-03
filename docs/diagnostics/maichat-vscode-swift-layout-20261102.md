# VS Code 索引、临时文件与 Swift 测试分离：2026-11-02

## 用户要求与验收

- 将已验证的 160 版安装到真机并核对版本/进程。
- 工作区根目录生成真实 compile_commands.json，使 VS Code 能索引 MaiAgent 与 MaiChat C++。
  命令需指向当前源码、SDK 与头文件；检查 clangd 实际读库并分析代表性文件。
- VS Code Explorer 隐藏生成目录、日志和临时文件，不删除文件，不隐藏产品/第三方源码。
- Swift 的测试宿主、假数据和启动分流拆到独立 DebugSupport，保留少量正式接入点；
  正式 Release 不编译测试支持，Debug 模拟器保持原有操作，重新构建并回归。

## 已完成的真机安装

11:04 CST 已安装并回读 **2.2.4 (160)**，安装目录
`DE8F2577-9216-4313-B49A-0FF9C5BDAD37/MaiChat.app`。
正常参数 `--no-activate` 启动成功，PID **17932**，可执行文件路径与本次安装一致。
产物 SHA-256 仍为 `04dcd620866b17a4574bd34d4a2ff9223e7b230a04dec3592057bf004e6dcd10`。
证据：`/private/tmp/maichat-ios160-installed-20261102.json`、
`/private/tmp/maichat-ios160-device-app-20261102.json`、
`/private/tmp/maichat-ios160-launch-20261102.json`、
`/private/tmp/maichat-ios160-device-process-20261102.json`。

## 初始索引问题

工作区没有根数据库；MaiChat 子目录只有 18 条、iOS 子目录只有 2 条命令，均指向拆仓前
`OpenSource/multi-ai-code`。旧子目录数据库会优先遮蔽父目录数据库。
已从当前源码的真实桌面与核心构建开启 CMAKE_EXPORT_COMPILE_COMMANDS，分别导出
1498 和 574 条原始命令；工作区汇集时保留自有源码，依赖仍通过真实包含参数解析。
Xcode 导出限定实际 arm64 模拟器，避免 generic destination 额外启动无关的 x86_64 依赖构建。

## 分离前核对

正式源码中的大型测试宿主出现在 AIAssistantView、MaiChatApp、ChatView、MaiFfplayVideoView、
云盘与屏幕素材页面；AIAssistantModel 中包含假会话、草稿与媒体数据生成方法。
XCTest 断言已在独立 MaiChatUITests target，需分离的是 App 内测试宿主与数据支持。
大多只有 targetEnvironment(simulator) 条件，少部分 DEBUG；将统一为 DebugSupport 的
DEBUG 编译边界，UI 测试 scheme 配套使用 Debug。

## C++ 索引结果

工作区根数据库现有 **437 个不同源文件**的真实命令，输入来自当前 checkout 的桌面、
MaiAgent 核心、移动适配层、Android NDK 构建和 arm64 iOS Xcode Clang 片段。
所有源文件和工作目录存在，数据库不再含拆仓前 `OpenSource/multi-ai-code` 路径。
三个旧子目录数据库统一为到根数据库的相对链接。

新增 `MaiChat/scripts/generate-workspace-compile-commands.py`，支持首次指定真实输入，
保存本机输入清单，以及 `--refresh` 刷新已有 CMake 配置后汇集；不猜测 SDK、宏或包含路径。
根数据库、输入清单和 `.vscode/settings.json` 按现有本机生成文件约定忽略。
`.clangd` 可随源码保存，Android 条件补充实际 GNU C++17：NDK 默认 17、Apple clangd
默认 14，真实 JNI CMake 命令会因 NDK 默认值而省略 `-std`。

通过真实 clangd LSP 的 initialize、didOpen、documentSymbol 和头文件 definition 验证：

| 源文件 | 文档符号 | 错误诊断 | 头文件跳转 |
| --- | ---: | ---: | --- |
| MaiAgent.cpp | 31 | 0 | MaiAgent.h |
| MainWindow.cpp | 157 | 0 | MainWindow.h |
| MaiFfplayIosBridge.cpp | 12 | 0 | MaiFfplayIosBridge.h |
| MaiAgentJni.cpp | 10 | 0 | MaiMobileAgent.h |

回执：`/private/tmp/maichat-clangd-lsp-verified-20261102.json` 和
`/private/tmp/maichat-clangd-lsp-android-20261102.json`。
已核对本机安装的 clangd/C++ 扩展配置 schema；保留用户全局已禁用微软 IntelliSense 的选择。
`.vscode/settings.json` 同时设置数据库路径、Explorer 隐藏、搜索排除和文件监听排除；
仅隐藏构建、缓存、日志和临时文件，产品源码、脚本及第三方源码保持可见，没有删除文件。

## Swift 分离结果

新增 `MaiChat/ios/MaiChat/MaiChat/DebugSupport/`，13 个按功能组织的 Swift 文件，
共约 3500 行测试支持；包括启动分流、测试页面、会话和草稿假数据、媒体/云盘素材、
屏幕采样自检、浏览器自检、相机请求代次验证和侧滑计数器。
云盘样本上传工具栏也移入此目录，正式视图只保留一行 Debug 接入。

这些宿主需要在被测 App 进程使用实际页面、WebKit、播放器和 C++ 核心，因此仍属于
App target，统一用 `#if DEBUG` 排除 Release。XCTest 断言继续在独立 UI 测试 target。
未将正式模型的私有 setter 改为公开；跨文件测试通过短小的 Debug 状态设置接口接入。
需要复用的视图从文件私有改为模块内部，未扩大为 public API。

Xcode 工程显式登记全部文件；`project.yml` 保持递归源码目录，两处版本同步为
**2.2.4 (161)**。共享 scheme 的 Test 从 Release 调整到 Debug，Run 保持 Release。

## 构建与模拟器回归

基线提交：`1997947219596e6520be27ed892bd6ea1820ff2e`，main 工作区叠加既有未提交修改；
本次没有提交、切分支或清理其他改动。
设备为隔离模拟器 `A15BBB1B-934D-44CC-BDA9-193101D942CF`（iPhone 17 Pro Max，iOS 26.5）。

Debug build-for-testing 通过：
`/private/tmp/maichat-swift-split-complete-debug-build-20261102.log`。
Release 使用 workspace 构建全部 Pods，并在命令行统一已有 App 最低系统版本 16.0；
旧 Pods 自带 8.0/9.0 的最低版本不被当前 SDK 接受，未修改 Pods 源码或业务配置。
Release 构建通过：`/private/tmp/maichat-swift-split-complete-release-build-20261102.log`。

第一组 6 项回归全部通过：正常 Root 会话侧滑/草稿保留、快速反复侧滑、从图片起拖不误开、
图片展开收回到原缩略图、停靠媒体翻页不重播、思考强度菜单。
结果包：`/private/tmp/maichat-swift-split-primary-20261102.xcresult`。
其中正常 Root 使用本机生成素材与隔离会话，不触发付费模型任务。

已实际查看导出截图及录屏取帧：全屏图片没有底栏/标题，收回后底栏恢复、原缩略图位置稳定；
目录与聊天页边缘层级保持原样，侧滑中消息和草稿随整页移动；媒体页连续移出/移入。
录屏：`/private/tmp/maichat-swift-split-regression-20261102.mp4`；
取帧：`/private/tmp/maichat-swift-split-drawer-sequence-20261102.jpg`、
`/private/tmp/maichat-swift-split-media-sequence-20261102.jpg`。
第二组 7 项全部通过：云盘连续浏览/缩放、IM 图片视频混合浏览、本地音乐控制、音乐迷你条、
相机请求代次/合成采样回到原草稿、真实 sampler 采样/引用回收、群聊和私聊隔离。
结果包：`/private/tmp/maichat-swift-split-secondary-20261102.xcresult`。
已查看相机采样中、停止后的素材网格与群聊回信截图；相机素材是模拟器生成内容，
此项不代表真机相机权限和传感器已经验收。未连接 Android 设备，不运行或宣称 Android UI 验收。

另启动拆分后的本机 WebKit 自检，核对结果文件修改时间晚于本次启动，返回
`PASS: observe, type, click, verify, screenshot`；回执
`/private/tmp/maichat-swift-split-browser-20261102.json`。
Release 二进制中未出现抽查的 7 个测试宿主/样本工具栏标记，回执
`/private/tmp/maichat-swift-split-release-markers-20261102.json`。
本次启动的隔离本机服务已停止，保留诊断产物；未触碰其他模拟器和后台任务。

## 真机 161 安装边界

arm64 Debug **2.2.4 (161)** 构建成功，App 严格深层签名与 10 个嵌入库签名均通过。
主程序 SHA-256：`186dbcb61aac3b4d43943731ceef2c4df2cdc314c9e62896b491404eeba62324`。
`/private/tmp/maichat-ios161-artifact-20261102.json` 保存版本、提交、补丁及全部 App Swift
源码哈希；构建后核对源码未变。

安装时 iPhone 再次 unavailable，CoreDevice 返回 **4016**，所需受信连接/开发服务状态为空。
证据：`/private/tmp/maichat-ios161-install-20261102.log`。
**161 未安装、未启动、未完成真机 UI 验收**；最后成功确认的真机版本仍是本日 11:04 安装的 160。
未重启设备或清理用户数据；恢复受信开发连接后可继续原位安装。

## 13:34 CST 真机 161 安装成功

用户再次要求安装后，既有 iPhone 14 Pro Max 恢复为 available (paired)。
核对已验证 161 产物的签名、主程序哈希和 App Swift 源码清单均未变化，随后原位安装成功。
设备回读版本为 **2.2.4 (161)**，安装目录
`AFFE1922-F7F6-40A3-BF59-1F624B9A0B6A/MaiChat.app`。
无测试启动参数、`--no-activate` 正常后台启动成功，PID **18520**；再次查询运行进程，
可执行路径与这次安装目录一致。未清空 App 数据；此次完成安装和启动核对，不代替真机手势验收。

回执：`/private/tmp/maichat-ios161-retry-installed-20261102.json`、
`/private/tmp/maichat-ios161-device-app-20261102.json`、
`/private/tmp/maichat-ios161-launch-20261102.json`、
`/private/tmp/maichat-ios161-device-process-20261102.json`。
本次结果更新了上一节的未安装状态。

## 2026-11-03 新增核心后的索引刷新

后续开发美颜核心后，已用当前桌面真实 CMake 数据库、iOS Adapter、Android Adapter
及既有 Xcode 片段重新汇集根目录 compile_commands.json，共 441 项，包含 Portrait 的
Engine、Personalization、Bridge 三个正式 C++ 文件。保持原有各子目录别名和隐藏配置，
不把构建临时目录暴露到 VS Code。运行既有生成脚本即可再次刷新。

Apple clangd 对 Bridge 的解析、AST 索引、语义高亮和 inlay hints 检查为 0 错误，日志
`/private/tmp/maichat-portrait-index-parse-check-20261103.log`。此次仅禁用 `--check` 的
可选编辑动作自测，因为其 ExpandDeducedType 对复杂类型报内部错误；未抑制编译诊断，
未修改 VS Code 或 clangd 的正式配置来隐藏问题。后续真机已升级到 170，详见本日
美颜与播放器诊断记录；上文 161 安装记录仅代表当时状态。
