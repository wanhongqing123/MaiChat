# 产品候选版本统一为 4.0.1

用户明确指定本轮版本号为 4.0.1。直接在 main 工作区更新产品元数据，保留其他未提交工作；
没有创建 Git tag、提交、推送或公开 Release，不改变服务端/第三方库独立版本。

| 平台 | 最终元数据 |
| --- | --- |
| Desktop | CMake project VERSION 4.0.1 |
| Android | versionName 4.0.1，versionCode 286（现装285顺延） |
| iOS | Xcode Debug/Release MARKETING_VERSION 4.0.1，构建号235（原234顺延） |
| iOS 生成源 | project.yml 同步4.0.1/235，修复原来与Xcode实际项目不一致的旧值 |

Info.plist 沿用 MARKETING_VERSION/CURRENT_PROJECT_VERSION，不硬编码第二套版本。
上述入口已逐值核对；Desktop/iOS 本轮只做源元数据验证，没有声称已重新构建或安装。
更新记录标明候选状态，不把4.0.1描述成已公开发布。

## Android 构建与真机覆盖更新

正常离线 Gradle 版本增量构建成功，未改变 native 参数或功能。845个输入指纹构建期间一致；
相对上一次 Android 生产输入，仅 app/build.gradle 的版本元数据变化。
76个资产与26个原生库均和前版 f2c86a59... 逐SHA一致，固定模型不变。

- 主包：com.kongshang.maichat，ARM64 Debug，testOnly=false，非isolated，假账号开关false。
- 稳定包：`.cache/android-4.0.1-install-20261127/current/MaiChat-4.0.1-v286-arm64-debug.apk`。
- 大小：1,616,466,443字节。
- SHA256：528c19fc7eca0a4c3fd264320c028c8f63490eb8641deb4b0e69a248d67cde98。
- 签名证书仍为87704a9ff5f7394186ca146b6303485abdc8fc594a4f8d2da9aaddc87ea00ea5。

设备89VCD65LEYTGROW8/PKD130通过正常 `adb install -r --no-streaming` 覆盖更新，68.298秒。
未卸载主包、清数据或自动授予此前未授权的权限；设备实际base.apk与稳定包SHA相等。
系统版本为4.0.1/286，数据目录身份与UID保持；新PID27330正常启动，错误日志无FATAL。
亲看“我的”页显示4.0.1 (286)，原登录及消息连接状态可见。没有发送模型提示或付费生成。

证据目录 `.cache/android-4.0.1-install-20261127`：final-apk-receipt.json、
version-only-comparison.json、正常Gradle日志、device-install/receipt.json与profile.png。
旧2.2.43包/收据保留；网络自动选路仍是独立待实施项，本次版本更新没有夹带未验证网络逻辑。
