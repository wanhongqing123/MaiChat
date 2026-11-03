# Android 设置双语音服务与展示页修正（2026-11-03）

## 现场、基线与预期

- 源码基线 `b8234d71`，实机 OPPO PKD130 Android15 上的2.2.8(240)。此前已完成并实测截图头像保存。
- 用户对照 iOS 设置指出语音识别应有腾讯云、阿里云两套可切换，Android 却只显示不可点击的“腾讯实时转写”。此前只恢复腾讯普通引擎，不代表双服务已经实现。
- 用户明确要求空云盘去掉“还没有图片或视频”和上传引导；创作欢迎卡只展示功能，不可点击填草稿或显示按压选择态。
- 设置比较图还显示账号注销和举报记录，按现有 iOS/服务合同提供真实入口。“短信接入”的含义在当前仓库无实现证据，已请求该页面截图；未放入无功能占位入口。

复现/验收操作：

1. 设置中选择任一语音服务，返回 IM/创作中心长按；所选服务实际握手、音频直传、返回文字，上滑至取消区不发送，编辑区等待真正 final 后仅回填。冷启动仍保留选择。
2. 云盘空列表显示容量卡和留白，没有空图示及引导；加载/实际错误可恢复。
3. 欢迎卡短按/按住不改草稿、不弹键盘、不触发功能；列表滚动仍贴边，最后一项可达。
4. 举报只读 GET，表单/失败/空态可返回设置；账号注销只查看options与验证表单，本轮禁止发送真实验证码或提交真实注销。

## 实现与当前边界

共享录音器每次长按固定服务；腾讯维持16k单声道AAC，阿里云使用16k/16bit/mono PCM。两者复用现有 native curl WSS，客户端直接访问各供应商；账号服务只签发各自临时凭据。两套凭据独立内存缓存、到期提前刷新、退出/同号重登拒绝旧响应；没有长期密钥、没有识别代理。

阿里云 StartTranscription/PCM/StopTranscription/TranscriptionCompleted 按真实协议处理。固定 `X-NLS-Token` WebSocket header 的依据为[阿里官方 Go SDK ws.go](https://github.com/aliyun/alibabacloud-nls-go-sdk/blob/master/ws.go)，协议依据为[官方实时语音识别接口](https://help.aliyun.com/zh/isi/user-guide/api-reference)。不输出 token、task正文、签名 URL 或实际转写内容。

实现计划见 `docs/superpowers/plans/2026-11-03-android-settings-speech-parity.md`。

## 2026-11-03 真机验证与复查

现场设备为 USB 连接的 OPPO PKD130 / Android 15 / ARM64，包名 `com.kongshang.maichat`。09:23 原位安装 2.2.9(241)，保留登录、历史和此前真实保存的头像。没有卸载、清数据、额外包名或线上发布。

| 操作 | 实际观察 |
| --- | --- |
| 09:25 创作中心选择阿里云后按住空输入框 | `asr-provider-aliyun` → `asr-aliyun-ready` → PCM发送 → partial；滑到右侧编辑区后 `end-sent samples=177600` → 真正 `server-final chars=12`，只回填草稿并弹键盘，没有创建任务 |
| 09:25 IM 阿里云按住→左侧取消 | 真正 partial 后取消，输入框仍为空，原5条消息不变 |
| 09:26 创作中心改腾讯云后按住→编辑 | `asr-provider-tencent` → ready → AAC → partial → `end-sent samples=91136` → `server-final chars=12`；只回填，没有发任务 |
| 09:28 IM 腾讯云按住→取消 | 真正 partial 后取消，消息及空草稿不变 |
| 空白处点击 | 真机 `mInputShown=false`，键盘收起 |
| 云盘空列表 | 只有容量卡、加号和文件区留白；无正常空态图示/文案 |
| 创作卡片短按、按住 | 无选中背景、不弹键盘、不填文字；已有自建 `parity-draft` 草稿保持不变，结束后只移除本次自建草稿 |
| 创作列表滑到底 | 最后一项“跨设备会话”可见；滚动条位于列表的屏幕右边缘 |
| 我的举报 | 真实只读GET成功，当前账号显示“暂无举报记录”；返回设置正常 |
| 注销表单 | 真实只读options成功，呈现当前账号的验证方式；未请求短信、未输入秘密、未提交注销 |

截图保存在本机忽略目录 `.cache/android-ui-parity/v229-*.png`，均为 `adb exec-out screencap` 直接到本机，未另存手机相册。录音使用设备真实麦克风和电脑朗读的固定测试句，未以文件注入代替采集。日志只有固定阶段、计数和异常类，不保存实际转写正文或临时凭据。

实测同时暴露第二次转写编辑后首行可能被裁：首轮两行输入框约58dp，第二轮仍以44dp单行高度量高。局部 `ComposerEditText` 按实际宽度和真实文字布局量高，空草稿及完整可容纳的短草稿复位内部滚动，六行以上保留滚动。Native Robolectric 证明旧控件清空后保留scrollY=24，修复后两项通过；普通SDK未完整复现OPPO的父布局44dp约束，最终由同序真机操作决定：09:36 连续两轮 EDIT→删除自建草稿→空白收键盘→EDIT 均完整显示两行。最终网络修复产物上09:43也再次回填完整两行，见 `v229-delivery-aliyun-edit.png`。

## 最终构建、安装与交付身份

- 全部原生变更均在新仓库的 ARM64 `maichat_agent` 目标编译通过：固定语音认证header、宿主工具注册，以及验收中新发现的curl resolver修复。后者详见 `maichat-android-curl-eventfd-20261103.md`，未把发生过的候选闪退隐藏成成功。
- 最终 Gradle `testDebugUnitTest`、`compileDebugAndroidTestJavaWithJavac`、`assembleRelease` 成功；306项JVM测试，0失败/错误/跳过。AndroidTest为编译检查，没有以此冒充真机仪器测试执行。
- 15份ARM64原生库、6份运行时库哈希及3份必需资源门禁通过；zipalign/apksigner通过，`testOnly=false`，同包名同本地证书，版本2.2.9(241)。没有重新索取发布签名材料。
- 09:41:36最终原位升级成功。APK大小100208340 B，SHA-256 `f864e6eafb4f2910cb3741e29306879d4eae7f81008c41322e730997a913492a`；从手机实际 `base.apk` 拉回所得哈希相同。原生Build ID `951ef201cafd8bf0f8691572c5da0d76d8c5a4c4`。
- 最终安装路径 `/data/app/~~j06GqXgDr1ezlmuYsJL7Sg==/com.kongshang.maichat-LGN9u7c1uqVqVkI8b21Mkg==/base.apk`。最终观察PID9842，09:45:48腾讯SDK connect/login ok，09:47“我”显示消息已连接；原头像和5条聊天记录仍在。密集三次冷启期间一度显示消息连接失败、该进程未见SDK连接日志；稍后一次冷启动恢复，未对未定位瞬态加入猜测性改动。
- 修复安装后的多次冷启动和页面/语音操作无新crash记录；最终产物上阿里云创作编辑收到真实final12字，腾讯云IM收到真实partial后取消。当前选择阿里云，跨冷启动保留。
- 本机截图 `v229-delivery-settings-final.png`、`v229-delivery-cloud-final.png`、`v229-delivery-card-held.png` 和 `v229-delivery-cards-final.png` 均已亲看。最终卡片按住无高亮，输入框仍空；云盘文件区留白。三张最终实机页面截图已通过IM发送给用户。

本轮源码基线为 `b8234d71`；最终源码提交 `30f32eec7f347c4556427219918d980d8102b236`，其父提交 `e0feeefe` 包含curl修复及定点回归。上述构建后未再修改生产源码，提交后工作区干净。安装产物以以上完整哈希固定，不引用早期候选的哈希。

## 账号注销清理的审查边界

普通代码与工具入口共用设置service。注销工具仅打开本地验证表单，不能传密码/验证码或直接提交；举报工具只读。202受理后保存私有回执和捕获的原账号证明，离线退出时只清匹配认证代际；后台按owner清理5张表、未被其他owner引用的媒体与私有AI目录，不触碰其他账号、Guest或外部工作区。失败保留pending，启动重试。

交叉审查发现并修复两项具体中断问题：快速A→B→A时必须等待A的全部旧worker退出；legacy所有权标记必须最后删除，避免中断后失去归属判断。均以本机fixture先复现失败再验证修复。真实注销、短信挑战和注销后线上清理未在用户账号上执行。

## 未覆盖范围

当前 Windows 没有 iOS 模拟器；只按用户提供的 iOS 真机截图及仓库实现对齐，不宣称本轮重验 iOS。安装包针对连接手机的 ARM64，未验证 armeabi-v7a、多ABI商店包或正式发布签名。iOS截图中的“短信接入”仍待该页截图明确用途，本仓库未找到对应实现，未添加占位入口。
