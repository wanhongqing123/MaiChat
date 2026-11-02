# Android 头像更换：普通入口与模型工具共用核心

- 日期：2026-11-03。
- 开工基线：`main`，`f3a55b78`，工作区干净。
- 目标版本由主任务设置为 `2.2.8 (240)`；最终构建／安装产物由主任务记录。
- 本记录负责 SDK、Session 与工具入口；上传、裁剪界面及主页面由并行任务实现。

## 可复现操作与预期

普通操作从“我的 → 个人资料 → 头像”进入，选择本地图片并裁剪，确认后上传并等待腾讯 IM 确认。成功才更新本人头像；取消或失败保留原资料。打开选择器前绑定账号与登录代际，切号或同号重登后旧选择和在途结果均失效。

模型入口为新增 Android 专属 `maichat_set_avatar`，参数 `{ "file_path": "工作区图片路径" }`。沿用工作区文件访问规则和既有 Agent 审批机制；不接受任意系统路径、任意远端头像 URL 或调用方指定的账号。工具等待 SDK 确认，成功回执为 `updated=true` 与公开稳定 `avatar_url`，不把排队当作更换成功。

## 调用链与边界

1. 普通 UI 与模型工具共同调用 `RemoteIMSessionController.uploadSelfAvatar(File, expectedIdentity, AccountCompletion)`，没有第二条上传业务路径。
2. `avatarChangeIdentity()` 绑定账号、`accountGeneration` 与 IM owner；`isAvatarChangeIdentityCurrent()` 可供选择、裁剪与回调检查。共享事务门禁阻止普通 UI 与工具并发覆盖。
3. 既有账号控制面下发媒体访问权限，`MaiChatAvatarUploader` 签发 `purpose=im_avatar` 元数据；图片字节由客户端直接 PUT 到 OSS。Session 不删除调用方输入，UI 负责临时文件清理。
4. SDK `setSelfAvatar` 在调用、SDK 排队执行和回调阶段检查 owner／登录状态／账号代际，再调用 `V2TIMManager.setSelfInfo` 的 `faceURL` 更新。
5. SDK 确认后才更新 `RemoteIMSelfProfileState`、按新稳定 URL 预取头像并通知页面。独立本人资料 revision 在 SDK 回读和排队到 main 应用时两次校验，旧查询仅丢弃本人资料，好友资料照常交付。
6. 头像事务使用独立 main 回调执行器，Session 销毁也交付一次清理 completion；销毁会主动完成 pending 操作。账号 current 为 false 时禁止 SDK 写入或安装资料。底层异常与本地 profile 安装异常都转换为固定安全错误，完成与事务释放最多一次。

工具注册位于 `MaiChat/MaiChatTools/MaiMobileHostTools.cpp`，按现有 `maichat_set_nickname` 命名风格新增，`approval=true`，且仅 `__ANDROID__` 注册；未修改 MaiAgent 协议或向未实现的 iOS 宿主广播该工具。Android `AIAssistantController` 将该工具放到 worker 执行，再调度 Session 到 main，避免阻塞 UI 等上传与 SDK 回调。

## 本地验证

- `RemoteIMAvatarUpdateTest` 10 项及 `RemoteIMSelfProfileRevisionTest` 1 项使用内存上传器、SDK 回调和合成身份，最终 `OK (11 tests)`。覆盖上传前／上传中／SDK 排队／SDK 回调切号，同号重登、重复回调、UI／工具共享门禁、旧本人查询与好友资料隔离，以及异常不泄漏底层 message。
- 新功能测试最初在缺少账号事务核心时断言失败；共享门禁与资料 revision 用例分别先失败后通过。独立审阅提出的 profile 安装异常用例实际得到 completion 为 0 的失败，补安全收尾后通过。
- SDK、Session、状态类、IM 工具、Assistant worker 路由及上传模块共同使用本机 JDK 17、Android SDK 34、既有编译依赖独立 `javac`，退出 0。
- `git diff --check` 通过；未运行并发 Gradle 或 native 构建，由主任务统一执行。

本轮没有 ADB 操作、腾讯 IM 写入、真实上传或线上头像更换。实际选择／拖动缩放／裁剪视觉、最终 APK／运行进程与真实 SDK 更新仍以主任务验收记录为准；离线测试不代表线上头像已改成功。
