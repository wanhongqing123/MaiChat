# 当前账号查询与头像修改工具（2026-11-03）

用户在提交推送当前改动后追加要求：Agent 可查询本人账号及个人资料，并能修改头像等
资料。基于已推送 main `a2eb6804`，新增只读 `maichat_get_current_account` 和写操作
`maichat_set_avatar`；原有昵称和安全密码工具复用，不新增重复入口。

操作与预期：创作中心要求查询当前账号，工具无需切换页面即可返回当前公开 MaiChat ID、
IM ID、昵称、脱敏手机号、稳定头像地址。未登录返回 logged_in=false；已登录但暂时离线
仍是已登录，未知资料为 null。不得返回密码、账号令牌、UserSig、API Key、登录会话 ID
或完整原始鉴权响应。

用户指定工作区图片作为头像，按现有写操作审批执行。平台解码并规范成 512×512 JPEG，
复用普通头像服务；三端共用 C++ 签名上传实现，POST 既有 sign-upload purpose=im_avatar，
图片由客户端直接 PUT OSS，不经后台中转。只把稳定 avatar_url 写入本人 IM profile。
输入图片保留；须在 SDK 确认后才返回 updated=true，不把排队或上传成功当作资料已更新。

每个异步阶段绑定账号及登录代际；切换账号、退出登录、取消或超时不得把旧任务继续
应用到新账号。SDK 真正派发写入前及其回调时再次核对，旧回调不能更新新号的缓存。
头像跨端显示沿现有 IM 资料同步路径，不能把自己误插入好友表。

普通 API 与工具入口共用实现；设置页仍走相同头像上传服务。验证使用本地测试账号和
公开素材/合成图片，禁止修改用户真实头像或触发付费模型请求。测试计划包含空资料、
已登录离线、字段白名单、完整手机号/签名地址防泄漏、路径越界、取消、账号切换、
上传与 SDK 失败，以及确认后的头像展示。构建、回归和验收边界随后记录。

## 实现与回归

共享 factory 限定参数、规范化工作区路径并白名单返回六个公开字段；完整手机号或临时
签名头像地址不会被转发。宿主错误也不透出原始鉴权回复。当前查询读取本机缓存，
正常会话同步刷新资料；修复 Android 查询强制联网及 Android/Desktop 刷新令牌时
误清空脱敏手机号的缺口。同账号缺字段保留，明确 null、换账号和退出清除。

设置页与工具共用普通头像服务，三端共用 MaiAvatarClient。对取消生成内部 request_id，
移动桥接在 Agent 停止时发不可由模型直接调用的取消通知。三端都拒绝并行头像修改；
SDK 派发后停止不能撤回远端写入，仍等待真实回执，成功保留 updated=true。账号切换
只隔离旧回调和新号缓存，不宣称已回滚旧账号的已派发操作。

本次完成的验证：

| 范围 | 结果与证据 |
| --- | --- |
| 共享工具契约 | 34 项检查通过：字段白名单、空态、路径、审批、进行中取消及确认真值；`maichat-account-shared-final-20261103-0.log` |
| 共享上传 | loopback TLS 回归通过：签名与 OSS 凭据分离、owner/path/MIME/host 校验、重定向拒绝、大小和取消；`maichat-account-shared-final-20261103-2.log` |
| Desktop | 普通应用 37、SDK 29、工具 11 项均过；另 3 项本地 Auth 缓存用例通过。使用禁网/本地服务器及 Fake SDK，未接触真实 Keychain |
| iOS 172 | 完整模拟器与真机构建通过；正常 Root 的资料/头像边界及静态功能说明两个 UI 用例通过（13.251、5.911 秒），录屏和正常个人资料页已查看 |
| Android 246 | APK 与测试 APK 构建通过；25 项 JVM/Robolectric 用例通过，arm64 JNI 实际设备侧 3 项通过（0.098 秒）；正常 MainActivity 登录页已查看 |
| 核心 | 提交前八套核心回归再次全部通过、所有退出 0；`maichat-account-precommit-core-20261103-results.json` |

日志均位于 `/private/tmp/`。iOS 的资料页账号标签最初因 LabeledContent 的 AX 合并被
错误定位成独立 StaticText；录屏中实际账号、昵称和头像正确。已保留精确账号值，按
真实可见 AX label/value 定位，并加入个人页截图及层级附件；未改产品数据来满足测试。
最终 `/private/tmp/maichat-account172-ios-ui-20261103.xcresult` 两项通过，
`maichat-account172-ios-ui-20261103.mp4` 已回看。显示规范 JPEG 的蓝底黄色竖条头像、
公开账号和昵称；额外静态能力说明仍不可点击，不自动执行账号工具。

## 安装与运行产物

- macOS 桌面 2.2.34 完整包严格验签、`--smoke` 退出 0，按已有更新重开授权原位安装，
  PID 94042。主程序 SHA-256
  `5a28eeafa81d90f2d10bdc87adef666f16508127b3a4df902141fc19ed1bce0a`，与保留的
  `MaiChat/desktop/dist-account-tools-20261103/MaiChat.app` 一致。回执
  `/private/tmp/maichat-account234-desktop-installed-20261103.json`。
- iPhone 14 Pro Max / iOS 18.7 先成功安装启动 171。172 初次安装因开发连接
  unavailable/CoreDevice 4016 失败；用户确认重新连接后重试成功，回读版本
  **2.2.4 (172)**，启动及 processes 核对 PID **22848**。严格验签通过，回执
  `/private/tmp/maichat-account172-device-{install,launch,apps,processes}-20261103.json`。
- Android **2.2.6 (246)** 在 emulator-5580 / API 37 arm64 安装运行，真实 JNI 回归后
  正常启动 MainActivity；不代表 Android 真机验证。

验证使用合成图片、隔离测试账号、本地 TLS 和 Fake SDK，未用真实用户头像变更来验收，
未触发付费模型。设备安装启动不替代真实账号端到端修改体验；Windows Desktop、
Android 真机、真实 OSS + 真实 IM 写入尚无本轮现场验收。后端接口未改、无需部署服务。
好友添加使用公开 account_id（个人资料的“账号”）或手机号，im_user_id 是内部聊天标识。
