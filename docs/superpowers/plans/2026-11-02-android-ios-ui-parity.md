# Android 对齐当前 iOS 界面 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Keep implementation on main as required by AGENTS.md. User has explicitly disabled brainstorming and repeated design approval.

**Goal:** 将 Android 的消息、我的、云盘、创作中心和输入交互对齐用户提供的当前 iOS 截图，并在 OPPO 真机自行验收和更新现装 App。

**Architecture:** 保留现有账号、IM、云盘、Agent 与媒体核心，只调整本端展示及缺失的触摸接线。导航图标复用 iOS 同源 SVG 的轮廓和 Filled 版本；云盘目录状态由 Activity 持有并注入生命周期独立的页面。按账号和会话校验异步操作，不新增模型代理或第二套业务路径。

**Tech Stack:** Android Java Views、VectorDrawable、现有 C++/JNI、JUnit/Robolectric、ADB、OPPO PKD130 Android 15。

## 原始操作与预期

1. 登录后打开消息：标题居中、五个底栏仅图标、选中使用同源实心轮廓；联系人摘要/未读仍正常。
2. 打开“我”：灰底白圆角个人资料卡和设置行；点卡进入资料，点设置进入账号/隐私操作，返回仍在“我”。
3. 打开云盘：居中标题和加号、容量卡、文件夹/媒体单行；目录返回、切 Tab 返回和失败重试不丢当前位置，不主动上传或删除用户文件。
4. 打开创作中心空会话：原有功能入口以两列卡呈现，统一蓝色矢量图标；所有项目可滚到，点击仍仅填草稿。
5. 聊天/创作中心空输入框显示“可按住转文字”；按住开始录音，居中转写卡与独立取消/编辑浮区复用 iOS 行为，松手编辑只回填最终文字；有文字时保留原生选择。点消息空白收键盘且保留草稿，滚动、链接、复制不被吞掉。

## 任务与文件

- [x] **输入交互优先。** 修改 `MainActivity.java` 的 composer/语音方法及 `AIAssistantPanel.java` 的输入触摸方法；新增专属手势绑定，复用实时 ASR ticket 与录音核心。用 `ComposerVoiceInputTest`、`ChatKeyboardDismissalTest` 先复现，再实现；实机长按后移出取消，不向真实联系人发送测试消息。
- [x] **导航与消息。** `MainActivity.java` 的 `bottomTabBar`、`tabButton`、`renderConversationList`、会话列表局部刷新；`res/drawable/ic_nav_*` 使用 iOS 相同 SVG，保持 44dp 点击区和五个入口的无障碍名称，去掉底栏文字。
- [x] **我的与资料。** 将 `renderSettings` 改为首页、个人资料和设置的明确层级，复用现有昵称、账号复制、密码、二维码、扫码登录及切号入口。读取本人资料，异步刷新不能误加自己为好友或串到另一账号；保持页面滚动和返回状态。
- [x] **云盘。** 修改 `MaiCloudDriveDialog.java`，新增 `MaiCloudDriveDirectory.java`；Activity 注入实例目录状态，独立目录请求客户端，保留预览/下载/续传/删除确认和完整错误态。`MaiCloudDriveDirectoryTest` 覆盖根过滤、父目录返回、切号、已删除目录与循环链。
- [x] **创作中心。** `AIAssistantPanel.java` 欢迎分支、顶栏和网格；`AIAssistantWelcomeAction.java` 和 `ic_feature_*`。保留源码中的欢迎标题，19 项入口各自填入既有草稿，空列表从顶部滚动，末卡可达。
- [x] **审查和交付。** 先按用户截图检查范围，再独立审查账号代际、请求取消、返回和手势冲突；构建同包名、同本机签名的普通 APK，`testOnly=false`，原位升级保留本次已登录账号与草稿。

### 2026-11-03 追加：居中转写浮层

用户现场指出原反馈缺少 iOS 居中卡及两个操作浮区。以当前 `ChatView.swift:2818`、`MessageBubbleLayout.swift:600` 为准：黑25%遮罩、蓝卡居中、独立52dp叉号/铅笔圆按钮；按钮优先扩大16dp命中，再用横移兜底；SEND/EDIT保留浮层等待最终结果，EDIT仅回填并聚焦，CANCEL立即取消。共用组件附在现有窗口，不能销毁输入控件或中断主手指。

## 回归与真机步骤

使用已核对的 JDK 17 和 SDK，不重新构建未变化的原生依赖。集中执行测试，避免多个 Gradle 并发修改同一构建目录。

```powershell
./gradlew.bat :app:testDebugUnitTest --tests com.kongshang.maichat.ComposerVoiceInputTest --tests com.kongshang.maichat.ChatKeyboardDismissalTest --no-daemon --console=plain --max-workers=2
./gradlew.bat :app:testDebugUnitTest --no-daemon --console=plain --max-workers=2
```

输入两组断言应先明确失败，修复后通过；云盘目录与欢迎草稿测试可用独立 javac/JUnit 快速红绿。纯配色不添加实现镜像测试。

APK 组装复用 `.cache/android-install-19979472/local-build.init.gradle` 所定义的同源依赖和本次新编译 ARM64 原生结果，去掉 IDE 设备定向参数，显式 `android.injected.testOnly=false`。安装前检查六个关键原生库和资源，安装后拉回 `base.apk` 对照哈希、进程路径及版本。

在 OPPO 的正常 App 上记录修改前后截图和触摸录屏，亲看标题、图标、资料层级、云盘空/目录/媒体、创作卡片和键盘状态。录音仅测试长按及取消，不发送真实语音。当前 Windows 无 iOS 模拟器，以用户现有 iOS 真机截图和源码为参考，不宣称重新通过 iOS 实机验收。
