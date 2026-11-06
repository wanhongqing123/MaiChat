# Android 我的与设置（2026-11-05）

## 可复现操作与预期

在正常 App 登录后进入底部“我的”。原页面只有资料卡和“设置”；现在资料卡保持真实头像、名称、账号、消息状态，下面按主题、偏好设置、存储空间、账号与安全、关于与版本、退出登录分组。返回手势和页头返回均回到上一级；注销提交中的既有导航保护继续生效。

- 主题：跟随系统、浅色、深色；选择不同值后 Activity 重建，恢复同账号的“我的”二级页及 IM 文字、创作中心已完成文字/附件/引用草稿。系统夜间与字号改变在原 MainActivity 中重绘，避免退回聊天列表。偏好跨账号为本机设置，不携带账号内容。
- 偏好：当前简体中文只读；通知显示系统与消息频道真实状态，进入 Android 通知设置，回来后刷新；字号跟随系统/标准/大字体通过 Configuration 实际作用于正文及输入框；语音识别沿用已有服务；触感仅在底部切换不同页签时生效并遵循系统设置。Android 无既有朗读实现，因此没有无效朗读入口。
- 账号与安全：复制账号、切换账号、密码、注销、举报走原实现。退出及切换先明确确认，取消保持当前页面；实现阶段没有退出任何真实账号。
- 关于：显示真实版本；分享诊断日志先选最近 1h/24h/7d/全部，后台完整导出再交系统分享面板；清理日志确认后只处理 `ui.log` / `ui.previous.log`，聊天与工作区不受影响。反馈、用户协议、隐私及儿童隐私沿用现有 mailto / ichat.life 链接，不增加品牌版权或虚假法律入口。
- 存储：普通 JNI 调用 `MaiAppStorageBridge`，与 Agent 配置使用同组 App 自有目录。展示缓存、临时目录及只读工作区实际统计，注明统计边界、24 小时保留和恢复方式；单项与全部清理都先获取核心精确预览，再显示数量/字节确认。全部清理使用两个独立 handle/token，明确逐项报告实际释放与跳过数量；失败不声称全部成功。退页/换账号向正在执行的核心请求发取消，UI 不等待 native destroy；迟到回调不更新旧页面。没有清聊天、模型、工作区或账号数据库的入口。

仓库没有套餐/积分/产品记忆库服务，因此不显示参考图中的固定 500 积分、订阅卡或空记忆库入口。

## 源码与边界

- 工作目录为当前 `main`，保留全部既有未提交工作；Portrait v8 / next_tool / 保护断言未改。
- MainActivity、MaiChatPreferences、MaiChatBaseActivity、MaiChatTheme 负责真实导航/外观；AIAssistantPanel 提供不含 Activity/View 的草稿快照。
- MaiChatStoragePanel / MaiAppStorageClient / MaiAppStorageJni 仅宿主适配；候选、时间、安全检查及实际删除仍唯一位于共享 C++ `MaiAppStorage`。
- App 专有临时根为 files/storage-temporary，缓存根为 App cache，工作区为当前账号 Workspace。三者真实存在且互不嵌套；工作区在本普通桥始终只读。新临时根没有文件时显示真实零值。

## 已做验证

- OpenJDK 17，offline `:app:compileDebugJavaWithJavac :app:compileDebugAndroidTestJavaWithJavac` 已通过；日志 `/private/tmp/maichat-android-my-java-compile-20261105.log`。
- 新 JNI 通过 NDK 27 的 arm64-v8a 与 armeabi-v7a `-fsyntax-only`；`git diff --check` 通过。
- MainThreadMonitor 真实 Android SDK 编译及 22 项临时 JVM 回归通过，含范围边界、堆栈、mtime 无关、全量原字节、失败暂存与仅清指定日志。夹具 `/private/tmp/maichat-diagnostics-monitor-20261105-y2RuwI/`，未清用户日志。
- 共享普通存储桥由对应 agent 单独验证，合成目录 0 failures，见 `maichat-mobile-app-storage-bridge-20261105.md`。

## 待统一最终构建的设备验证

此阶段没有 assemble、安装或运行新 App。Root 完成 iOS194 阶段后，统一构建含 v8 与本 UI 的 Android268，再只对 emulator-5580 执行 `PortraitRegionInstrumentedTest` 八项与 `MySettingsInstrumentedTest` 三项。不能用旧267或失败的早期268产物宣称已验证。

MySettingsInstrumentedTest 使用正常 MainActivity 的生产渲染与本地 QA session 数据，不是线上账号认证验收；测试不会调用真实退出、分享发送、清理用户日志或工作区。ordinaryStorageJni 测试仅清自身唯一 StorageJniTest 目录下的固定文件。启用 instrumentation 参数 `maichat.capture_my_ui=true` 可保留正常 MainActivity 的我的/偏好/关于/账号安全/存储截图（输出路径为 MY_SETTINGS_QA_SCREENSHOT 行）；需要开发者实际 view。主题/字号测试恢复此前偏好。正常登录、主题切换后的完整页面观感、系统设置返回、系统分享、旋转/后台恢复仍需最终安装构建实际操作，不能由本次编译替代。

## 最终正式 APK 验证（同日 12:51）

268 正常 App 与测试 APK 已从本树离线构建、冻结并覆盖安装到专用 emulator-5580
（MaiChat_SMS_20261102，arm64、16KB 页）。源基点仍为 `eee43402 + working tree`。
原八项 PortraitRegion 回归通过；三项设置/外观/普通 JNI 回归通过；新增正常 MainActivity
IM 表格换行用例通过。最后对 Android 存储页进行圆角主题、紧凑按钮及数字分行的视觉
修订后，最终冻结 APK 再跑完整 MySettingsInstrumentedTest 四项，11.077 秒、0 failures。
此次末段只改 Java 存储布局，未改 JNI 或人像处理核心。

已实际查看最终我的、偏好、关于、账号安全、存储和 Markdown 截图；人像正常 MainActivity
预览及关闭画面也已读回查看。APK 仍是 Debug 正常产品入口；本地夹具不代表线上登录验收。
最后重新启动正常 MainActivity 并核对运行进程与 versionCode 268。

验收产物：`/private/tmp/maichat-markdown-br-20261105/`。

- 冻结 APK：`delivery/MaiChat-android-2.2.31-268.apk`；测试包同目录。
- APK 指纹：`android-reviewed-artifacts.json`；最终四项日志 `android-reviewed-ui-tests.log`。
- 最终截图：`android-reviewed-screenshots/`；原有核心及正常人像截图另保留。

模拟器 `/data` 空间不足曾拒绝最终覆盖安装，且 cache 中截图未能保留。本轮只把已结束的
`no_backup/AIAssistantTest-<UUID>` Debug 专用测试目录完整归档到开发 Mac、核对成员及 SHA，
再移除设备上的测试副本；普通账号 `no_backup/AIAssistant`、数据库及用户工作区保留。
两个可恢复归档及清单为 `android-owned-test-backup.*` 和
`android-completed-debug-tests-backup.*`。恢复空间后最终 APK 安装及四项复测成功。
截图现写入测试专用 files 目录，避免低空间时缓存回收造成证据丢失。
