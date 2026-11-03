# Android 设置与双语音服务对齐 Implementation Plan

> **For agentic workers:** Use subagent-driven-development with disjoint file ownership. Work directly on main as AGENTS.md requires. User has disabled design approval loops.

**Goal:** 按本轮 iOS/Android 对比图补齐语音服务切换、设置功能，并落实用户对云盘空态和创作欢迎卡的明确修正，在同一 OPPO 正常 App 自行验收。

**Architecture:** Main 仅持久化服务选择和处理页面导航；共享 VoiceRecordingController 每轮固定一个 provider，复用账号临时凭据及 native curl WSS。账号控制面复用既有认证/刷新、注销和举报合同；媒体/语音客户端直连供应商，不增加代理。敏感注销验证只用本地 fixture，真机只读 options/reports，不请求短信或提交注销。

**Tech Stack:** Android Java Views、现有 C++/JNI curl、16k PCM/AAC、JUnit/Robolectric、ADB、OPPO PKD130 Android15。

## 用户操作与预期

1. “我 → 设置 → 语音识别服务”可选腾讯云、阿里云；重开 App 保留所选值。新一轮长按固定选择，切换不把旧 partial/final 交给另一轮。
2. IM 和创作中心都按当前服务走真实客户端识别。原有居中转写、取消/编辑和等待最终结果保持有效；直接语音消息使用原 AAC 路径。
3. 空云盘只有容量卡及正常操作，文件区域留白；不再显示“还没有图片或视频”、上传引导和空图示。请求中的 loading、实际错误及重试仍有真实状态。
4. 创作欢迎卡只介绍功能；按下没有选择高亮，点击不填草稿、不打开键盘、不发起任务，原有输入草稿保持。
5. 设置账号/支持部分按 iOS 的顺序、颜色、箭头呈现；“我的举报”读取当前账号记录；“注销账号”进入真实验证表单，必须本人填写密码或验证码、确认账号并二次确认，模型不能取得验证内容。
6. iOS 对比图中的“短信接入”未出现在当前仓库实现，已查询仓库和已有 IM 记录并请求该页截图确认实际用途；不填入无功能入口。明确的项目继续独立完成。

## 实施任务

- [x] **双 ASR 核心。** 新增 SpeechRecognitionProvider、Aliyun credential/cache、Aliyun transcript/stream，扩展共享录音器及固定 WSS auth-header；Auth ASR 区由 ASR agent 管理。协议/过期/账号切换/路由先红后绿，header 依据官方 Go SDK，等待真实完成再交付，不以 partial 冒充最终结果。
- [x] **设置入口。** Main 在 Session 创建前恢复 `maichat.speech.provider`；设置菜单保存 enum id，configure 共享 recorder。服务只在下一轮长按生效，原已登录账号、头像和聊天保持。
- [x] **账号与举报。** AccountSettingsService/Auth 固定 allowlist、DeletionPanel 与回执缓存由账号 agent 管理；Root 实现 SafetyReportsPanel/Main 导航、离页清理和提交期间导航保护。网络不持全局 auth monitor，回调检查身份。fixture 覆盖请求体、状态、重复提交和迟到结果。
- [x] **纯展示 UI。** Root 移除 cloud 正常空态说明及欢迎卡 action/draft，选择反馈透明，保留原有卡片布局和末项可达。
- [x] **交叉审查与构建。** 查取消、session/owner、credential缓存、线程与敏感日志，统一 NDK 增量构建及 Gradle 单测/AndroidTest编译/Release，正常同包签名原位升级。
- [x] **目标平台验证。** 腾讯/阿里云分别长按→实时文字→取消，至少一次编辑等待final只回填，不发真实 IM/模型消息。选项跨冷启动保留、设置返回、举报只读、注销只读表单、云盘留白、欢迎卡不改草稿及按压态均亲看截图。当前 Windows 无 iOS 模拟器，不宣称重新验证 iOS。

## 文件边界

- Root：MainActivity.java、AIAssistantPanel 欢迎/selector、AIAssistantWelcomeFeature.java、MaiCloudDriveDialog 正常空态、MaiChatSafetyReportsPanel.java、通用设置图标。
- ASR agent：media ASR 录音/协议/凭据，MaiChatAuthClient 语音区，CurlRealtimeSpeech JNI及固定header枚举；child只写 Aliyun transcript 和其测试。
- 账号 agent：MaiChatAccountSettingsService、MaiChatAccountDeletionPanel、Auth account/password邻近wrapper及其测试；只使用局部 patch，不整文件重写 Auth。

验收产物、源码基线、版本/进程与剩余边界统一记录于本轮 diagnostics，截图与音频只保留本机忽略目录，秘密与真实返回正文不入日志。
