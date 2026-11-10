# iOS 真机更新至 2.2.4（204）

## 目标与当前状态

2026-11-10，用户明确要求更新 iOS 真机。执行端为 Mac 协作者，Root 负责源码与收到证据的独立核对。操作范围是正常用户 App 的 Release 构建、保数据覆盖安装及只读页面验证；桌面版本、包、tag、公开 Release 和线上服务不在本次范围内。

**真机已更新至 2.2.4（204），初次正常启动成功；菜单实机验收待设备连接恢复，未通过。**

此前的私有 XCUI verifier 在设备预检显示 `Unlock iPhone to Continue`。协作者仅向该 verifier 的 xcodebuild 发 SIGINT 停止预检，避免以后某次解锁突然执行 UI；没有停止或修改正常 App、数据及安装。当时发出解锁请求，并约定只恢复这项只读菜单检查，不重新安装。

后续状态：用户已明确反馈解锁；但执行端最新 devicectl 读回 tunnel=unavailable、transport 缺失，私有 verifier 安装请求出现 CoreDevice4016 usage assertion。已请求重新接数据线；计划用 UseDestinationArtifacts 读取已装用户 App，不重复安装正常 204。该设备连接阻断与正常 App 安装/初次启动结果分列，不继续把当前条件写成用户尚未解锁，也不把菜单计为通过。

## 可复现操作与预期

1. 只读确认 paired physical device、Developer Mode 和 DDI 可用，按准确 bundle 读回现装版本。
2. 以冻结源码和原正常 device-derived 缓存、原 Team 构建 `MaiChat` 用户 App，使用 Release/arm64/iPhoneOS。仅命令行覆盖 `CURRENT_PROJECT_VERSION=204`，不修改工程版本源或共享 CMake。
3. 核对与旧 App 的升级签名身份及数据相关 entitlement 兼容，再直接覆盖安装；不卸载、不清容器、不手工读取或导出真实用户数据库。
4. 读回安装版本，正常启动并绑定进程与安装路径。只读打开模型菜单，不修改用户会话配置，不代发供应商请求。
5. 菜单验收需要设备已解锁；锁屏预检不能记为 UI 或菜单通过。

## 源码、设备与产物

| 项目 | 值与证据范围 |
| --- | --- |
| 源提交 | `b8782ef66d78611cf916356d0fcaaffdb31e9a6d` |
| MaiChat tree | `090bda434f1bbea8a67b01f37f61ba78e8045db3`；Root 与 Git 提交独立核准 |
| MaiAgent tree | `b1698e10ee4494a925a7e0e69fdf50de4ec893ff`；Root 与 Git 提交独立核准 |
| Target/bundle | 正常 `MaiChat.app` / `com.kongshang.maichat`，非 UI-test 宿主 |
| 仓库默认值 | 2.2.4 / build 202；Info.plist 使用构建变量，未改源 |
| 安装前后 | 2.2.4（203）→ 2.2.4（204），来自执行端 metadata 读回回执 |
| 设备条件 | 真机 iOS 27.2，有线 paired tunnel connected、DDI 可用、Developer Mode enabled；来自执行端状态 |
| 正常 Release exe SHA256 | `5dcaa7656d1086c24186f7af2ae76a984b6444b0974c84711a23fdf3188449ea`，来自执行端产物回执 |
| 初次正常启动 | exit 0 / PID 47849；进程 exe 父路径与 installed 204 URL 相同，来自执行端绑定回执 |

正常 signed 203 App 在安装前以 APFS 克隆保全；204 正常产物另存。升级比较的 application-identifier、Team、keychain groups、app groups、iCloud、ubiquity、push 和 Apple 登录等项均在回执中为相同。没有把“未使用卸载”扩大为已逐项检查用户数据内容：本次没有读取真实数据库来做内容验收。

## 已收到的证据与核对结果

| 已收到的共享脱敏日志 | 字节数 | SHA256 | 直接可见结果 |
| --- | ---: | --- | --- |
| device-release-build-redacted.log | 343412 | `effe25d80d67139df5658c0aa9dc8b8486007f3c02a2d2a29a141bc1d1242f98` | 正常 Release/arm64/iPhoneOS、命令行 build 204、BUILD SUCCEEDED |
| normal-app-signature-verify-redacted.log | 233 | `d0b6d1a317e68ddb169a81e84c1b180eda3b1d77f515af7d4d3f029eead45f2c` | valid on disk / satisfies Designated Requirement |
| device-install-redacted.log | 237 | `6b8ae90fb9ea4653b840aa201addcc9847afa09776c735b25026025352fc4093` | App installed，准确 bundle 相同 |
| normal-app-launch-v3-redacted.log | 67 | `4f68496be7aac424002b0c82aae8a8d95a727c4688926193afe4bceff3227167` | 准确 bundle 的 Launched application |

Root 独立读取四份文件并核对长度与 shared SHA；安装/启动日志未逐项显示版本、PID、exe SHA 或 entitlement 比较，这些细节来自结构化执行回执，不冒充 Root 直接读取设备或二进制。安装回执记录 build/signature/install/initial launch 均 exit 0，覆盖安装耗时 56.243s。

安装回执 SHA256 为 `bc0c5b1f2dc4ab94499f06fb2c009b5a11fedb92be5107ee2105669dbb2aeb44`，日志分层 manifest 为 `e46c39831e04033f97380668acb5061e324a98abc7c791e456e1e17874632c34`。同名 JSON 保留具体口径与状态。安全原件存于本机 `.cache/ios-device-update-20261110/received/`；含设备标识、签名身份或容器 URL 的原始材料留在 Mac 私有目录。

分层 manifest 的 `originalPrivate*` 覆盖 Mac 私有原始字节，`shared*` 覆盖转交的 UTF-8 脱敏字节。Root 只对收到的共享文件计算哈希；不能拿脱敏命令或文件与私有原件 SHA 比较，也不宣称已独立持有或重算私有原件。

## 保留的错误与未完成边界

- 两次先前 launch exit 64 是 devicectl 显式空 environment 字典解析错误，未执行 App。去掉显式空参数并清理 `DEVICECTL_CHILD_` 继承后 launch 0；不计作产品崩溃，也不隐去前两次错误。该首因来自协作者状态，早期原错误文件未作为此次四份共享日志转交。
- 私有 verifier 首次 xctestrun 将 TestHostPath 指向 exe 的夹具错误已保全；改为 `.app` 后才进入真实设备预检。使用 `-collect-test-diagnostics never`，避免收集设备 diagnose；未改仓库或正常用户 App。
- PID 47849 只代表初次正常启动及当时路径绑定。回执明确未断言其在其后的锁屏/XCTest preflight 阶段持续存活。
- 目前没有实际菜单截图或通过结果。已停止的设备预检不是 UI PASS；解锁后只继续只读菜单检查，不重装、不自动改变会话、不代发模型请求。
- 本次未操作真实数据库内容、付费模型、桌面包或线上服务。此前 Simulator 三项测试与 unsigned Simulator Release 仍保留原标签，不能替代这次真机菜单验收。
