# 当前 Android 主包构建与真机覆盖安装

用户明确要求把当前 MaiChat 安装到 Android 真机；同时提出无网 Wi-Fi 与可用蜂窝网络的兼容问题。
安装按已验证功能执行，网络只读审计独立记录，不将未实现的选路方案描述为已入包。

日期：2026-11-27。源码基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90`，含现有工作区修改；
未提交、推送或发布。本轮构建没有修改业务源码或产品版本号。

## 构建与产物

正常 Gradle `:app:assembleDebug --offline --no-daemon`，JDK17、NDK27.3、minSdk26/target34。
通过构建参数选择 arm64、关闭本机假账号/isolated/testOnly，不手工替换 APK 内的 so。
原 normal App lane `3v5h1a4h/arm64-v8a` 正常依赖重编当前 Core/JNI/项目库。
离线工具缺失、参数传递及初始 ABI/testOnly 审计失败均保留；没有通过删功能或测试宏绕过。

| 项目 | 结果 |
| --- | --- |
| 包名 | com.kongshang.maichat |
| 版本标记 | 2.2.43 / versionCode285（未另行递增） |
| 构建 | 正常 Debug，arm64-v8a，testOnly=false，LocalUiAuth=false，非 isolated |
| 稳定 APK | `.cache/android-current-install-20261127/current/MaiChat-2.2.43-v285-arm64-debug.apk` |
| 大小 | 1,616,470,496 字节 |
| SHA256 | f2c86a59d409cb026ecd650eb017ff552064c04d44cb5bac367e24c27a51a7c2 |
| 签名证书 SHA256 | 87704a9ff5f7394186ca146b6303485abdc8fc594a4f8d2da9aaddc87ea00ea5 |
| 资产 | 76/76 与正常合并源逐 SHA 一致，含固定1,396,198,496字节GGUF |
| 原生库 | 26/26 与正常 strip 输出逐 SHA 一致，16个项目库 BuildID 与正常构建一致 |
| 源码/配置 | 845 个指纹在构建期间无变化；未导入私有测试 Core、测试 ELF 或手工替换 so |

注意 `outputs/apk/debug` 的旧包未被当作本次产物；本次正常 Gradle redirect 指向
`intermediates/apk/debug/app-debug.apk`，稳定 APK 是该已审计文件的逐字节副本。

## 真机安装与数据保留

设备 `89VCD65LEYTGROW8`，PKD130，Android15/API35、ARM64。使用 `adb install -r --no-streaming`
保留数据覆盖更新，安装69.418秒。没有卸载主包、清数据或自动授予原来未授权的权限。
签名预检只把32位标识作为预检，完整证书匹配由新 APK 签名校验和 Android 更新安装器执行。
安装后对设备实际 base.apk 做 SHA256，与上述稳定 APK 精确相等。

数据目录 inode57768、UID10043、权限700前后一致；files/shared_prefs/databases目录 inode
也保持一致。只读取了目录元数据，没有读取偏好设置、用户数据库或令牌内容。
旧进程在正常更新时重启，新主进程 PID25369；启动器进入正常 MainActivity。

亲看启动消息页与创作中心截图：账号界面仍在、既有会话可见、布局正常。启动与进入创作中心
的目标进程错误日志未见 FATAL EXCEPTION/Fatal signal。没有为验收发送模型提示或付费生成请求。
这不代表每一项媒体/工具 UI 都已逐项实测。

证据 `.cache/android-current-install-20261127`：final-apk-receipt.json、final-assets.json、
final-native-libraries.json、签名/badging/manifest原文、各次Gradle日志，以及
`device-install/receipt.json`、data-directory-before/after、startup.png、creation-center.png。
旧本地产物已备份；临时 App UID 验收 APK 已在前一任务清理，此次安装的是正常主包。

## 网络兼容范围

本包仍主要使用系统默认网络，没有宣称实现无网 Wi-Fi 到蜂窝的专门选路或流式无缝迁移。
源码/官方依据及推荐方案见 [移动网络审计](2026-11-27-mobile-unvalidated-wifi-cellular-audit.md)。
方案需按可用网络能力、VPN/局域网和用户数据策略处理；非默认网络的 DNS/socket/连接池必须
一致，已开始的生成 POST 不能因切网盲目重发。受控无网 Wi-Fi/蜂窝实测矩阵尚未执行。
