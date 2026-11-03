# 浏览器表单与短信工具（2026-11-02）

## 需求与复现

- 基线提交：`1997947219596e6520be27ed892bd6ea1820ff2e`，main 上已有未提交的媒体和独立浏览器工作，本次保留。
- 用户现场：iPhone、iOS 18.7，在 `https://httpbin.org/forms/post` 执行 open → observe，用同一次返回的标识依次或并行填写 Customer name、Telephone、E-mail address。当前第二次操作报告网页变化。预期未变化的同一文档和控件可继续操作，替换、移除、改名或导航后必须拒绝旧目标并指出原因。
- observe 应有有界等待；表单 DOM 可用但子资源仍加载时应返回可用内容；超时明确返回加载状态及已有内容。标签应为页面关联 label/aria 文案，而不是 custname 等内部 name。
- 文件上传提供用户选择入口与清晰交接；密码保留用户或系统自动填充。工具 close 应退出独立网页，重新 open 同一会话时不应意外关闭正在展示的网页容器。
- 新需求：iPhone 与 Android 都向 Agent 提供短信读取能力。Android 检查现有 `mobile_read_sms` 的授权与读取链路；iPhone 18.7 没有通用收件箱读取接口，正在确认通过系统快捷指令把用户选定的新短信交给 App 的方案。不得把导入的消息描述为读取整个 iOS 收件箱。

## 已确认的代码证据

- `AIMobileBrowserTools.execute` 在每次 click/type 成功后清空 snapshotID 和 observedLabels，单次 observe 的第二个操作必然失败。
- 快照仅使用 aria-label、innerText、placeholder、name，没有使用 input.labels，httpbin 的包裹式 label 因而退化为 name。
- 旧 DebugSupport 自检将操作后旧快照被拒绝当成成功条件，未覆盖同一表单连续填入多个输入框。
- tool close 中 owner 不等条件在 owner 相等 guard 后永不成立，未设置 isPresented=false。
- Android 已注册 mobile_read_sms；MobileTelephonyTools 从 Telephony.Sms.Inbox 查询最多 20 条，要求 READ_SMS 与前台；iOS 当前只支持短信撰写，没有收件箱读取。

## 环境与验收边界

- 本次连接检查：自有模拟器 A15BBB1B-934D-44CC-BDA9-193101D942CF 可用；真机 <connected-ios-device> unavailable；adb 未连接设备。
- 后续记录修复前失败、修复后相同用例、实际截图/录屏及构建产物。模拟器运行不等于 iOS 18.7 真机验收。

## 最终实现

### 浏览器（iOS）

- 在 WebKit 的 `defaultClient` content world 内，用 WeakMap/Map 按文档和真实节点保存标识；普通输入、点击及再次 observe 不整体作废快照。每个操作仍核对文档、网址、节点连接、标签、类型、可见性及只读/禁用状态。页面 DOM 里不再写入可伪造的 Agent id 属性。
- 返回 `snapshot_expired`、`element_not_observed`、`element_detached`、`label_mismatch`、`url_changed`、`manual_input_required` 等具体原因；格式为现有 `invalid_input` 错误中的原因前缀，不新增宿主不认识的顶层错误码。
- observe 默认等待最多 5000 毫秒，可指定 0–10000 的 wait_ms；文档可用后无需等待图片等子资源。超时返回 loading、text/elements、partial 和重试间隔。使用单调时钟，并核对读取期间导航是否发生变化。
- 标签读取 aria-labelledby、aria-label、关联 label，再回退到可见按钮文案、placeholder/title/name；额外提供 name、fieldset 分组、checked 和非敏感 value。密码/验证码不回显 value。
- 并发操作按操作集合管理忙碌态；工具 close 复用普通 close，结束全屏展示；同一会话 open 新网址保留正在展示的网页容器。
- 命中文件输入时返回 `file_selection_required`，由用户打开网页，通过 WebKit 系统选择器选文件。已实际验证来源菜单、文件选择器和取消返回。没有加入 Agent 自动把任意工作区文件注入表单的功能；完整选文件后的上传提交不在本次 UI 回归覆盖范围。
- Android 浏览器没有套用本次 iOS 专用 wait_ms/可复用快照描述，避免共享工具协议承诺未实现的端侧能力。

### 短信

- 两端工具名均为 `mobile_read_sms`，支持 limit（1–20）和 after_ms。
- Android：保留系统 READ_SMS 授权，提取 `MobileSMSInbox.readRecent` 供普通代码与 Agent 共用；仅查询系统 Inbox，不发送短信、不改变已读状态，查询前后核对权限和前台。结果标注 `source=android_sms_inbox`、`inbox_access=true`。
- iPhone：增加 AppIntent“将短信交给 MaiChat”，以及“我的 → 设置 → 短信接入”。用户先开启接入，在系统消息自动化中把快捷指令输入连接到短信内容，可选发送方和收到时间。不会自动创建或启用个人自动化。
- iOS 接入默认关闭，只保存该动作明确传入的消息，不读取系统收件箱；按账号隔离、最多 20 条、导入后保留 24 小时；关闭接入或切换账号清除记录。文件受 iOS 数据保护并排除备份，不加入诊断或云盘。过期记录在后续访问时清理。
- iOS 的普通读取入口与工具入口复用 `AIMobileSMSInbox`；结果标注 `source=shortcuts_import`、`inbox_access=false`。没有提供收到时间时标注 timestamp_source=imported，不伪造发送方或收到时间。
- Agent 读取的短信结果会进入用户要求的模型任务；本次没有把短信内容自动发送给模型或新增后台模型任务。

## 复现与验证证据

2026-11-02 16:08 CST，新增表单用例在修改前真实失败：同一快照第 2/3 次输入报告“网页已变化”，关联 label 缺失、导航/输入错误混淆、可用 DOM 被加载态阻挡。结果包：`/private/tmp/maichat-browser-form-red-20261102.xcresult`。素材为现场 httpbin 表单的本机副本及本机 POST 回显，不提交真实个人信息到外站。

修复后：

| 检查 | 构建/设备 | 结果与证据 |
| --- | --- | --- |
| 表单连续及并行填写、再次 observe 标识稳定、替换节点拒绝、标签、POST 回显、等待/超时、文件交接 | iOS 模拟器 26.5、SDK 27.1 | `/private/tmp/maichat-browser-form-green-20261102.xcresult` 2 项通过；最终 164 相同表单再次通过 |
| 独立网页原有会话隔离、结束、收起后输入和位置、旋转、停止、失败重试、已有审批返回 | iOS 164、A15BBB1B-934D-44CC-BDA9-193101D942CF | `/private/tmp/maichat-ios164-ui-20261102.xcresult`；这些用例通过。新增文件/短信设置 UI 用例首次因测试点错控件失败，按实际层级修正后另行回归 |
| 文件菜单/文件选择器/取消、表单、短信接入开关 | iOS 164、同一模拟器正常 Root | `/private/tmp/maichat-ios164-recheck-20261102.xcresult` 3 项通过；已查看系统选择器和设置页截图 |
| 系统快捷指令中搜索 MaiChat，发现“将短信交给 MaiChat”动作 | iOS 164、系统 Shortcuts App | `/private/tmp/maichat-ios164-shortcuts-20261102.xcresult` 通过；截图 `.../maichat-ios164-shortcuts-shots-20261102/18249D8D-861C-4E90-ACFD-8DBB0EE7A606.png` 已查看 |
| iOS 消息隔离、持久化、数量/时间筛选、过期、关闭/账号切换、缺失信息和非法参数 | 独立 MaiChatTests 目标 | `/private/tmp/maichat-sms-inbox-final-tests-20261102.xcresult` 4 项通过。为现有测试日志替身补上正式布局使用的 debug 级别，使独立目标正常编译 |
| Android 未授权拒绝；真实系统权限弹窗后授权、重复调用不重弹；注入合成短信后读取及过滤、普通入口、后台拒绝 | Android 239、Android 17/API 37、独立 AVD MaiChat_SMS_20261102/emulator-5580 | `/private/tmp/maichat-sms-android239-{denied,permission,granted}-20261102.log` 各 1 项通过，无跳过。只注入测试短信，不读取真实设备 |

已录制 `/private/tmp/maichat-ios164-browser-sms-ui-20261102.mp4`，查看分段帧与实际截图，确认文件菜单、返回聊天和短信设置的布局。Android 正常 App 启动并查看登录页，未触发模型任务。录屏中的桌面段来自测试之间显式退出 App，不是用户网页操作期间退出。

## 产物与未验证边界

- iOS Debug 模拟器、Release 模拟器和 Debug 真机版 **164** 均构建成功；AppIntent 在 Debug/Release 产物 metadata 中存在，真机包签名严格验证通过。
- 真机包：`/private/tmp/maichat-slider-ios-device-derived-20261031/Build/Products/Debug-iphoneos/MaiChat.app`。16:34 再次安装失败，CoreDevice 4016、设备 unavailable；`/private/tmp/maichat-ios164-device-install-20261102.log`。**164 未安装到真机，不能声称 iOS 18.7 或真实新短信自动化已验收。** 本次没有运行或配置真机个人自动化。
- Android APK：`MaiChat/android/MaiChat/app/build/outputs/apk/debug/app-debug.apk`，versionCode 239。模拟器安装包 SHA-256 与构建文件一致：`b8e5b723d54ae1dcbd8f7e23aed66ebb2492472dceb4330a544a38dce7f2c3d5`；正常 App 启动 PID 8523。Android 真实手机未连接。
- 源码、真机可执行文件和 APK 哈希记录：`/private/tmp/maichat-ios164-sms-artifact-20261102.json`；交付前核对本次涉及的正式源码哈希全部相同。main 保持原提交，上述修改尚未提交。
- Android 首次复用只读旧 AVD 因空间不足无法装 App；改用新建独立 AVD，保留旧 AVD 和其数据。Gradle 使用本机 OpenJDK 17，避免原 GraalVM jlink 构建失败。

接口依据：[Apple 消息自动化触发器](https://support.apple.com/guide/shortcuts/communication-triggers-apdd711f9dff/ios)、[Apple TelephonyMessagingKit 的使用条件](https://developer.apple.com/documentation/TelephonyMessagingKit)。后者需要特定默认运营商消息 App 权益及区域条件，不是 iOS 18.7 普通 App 的通用历史短信读取接口。
