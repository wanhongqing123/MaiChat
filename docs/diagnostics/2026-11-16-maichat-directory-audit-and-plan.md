# MaiChat 全项目目录盘点与规整方案

日期：2026-11-16。状态：**只读盘点与方案，尚未实施**。

本目录任务没有改产品代码，没有移动、删除或清理业务文件，也没有读实际消息数据库、聊天内容、附件、凭据、密钥、生产环境文件。新增的只有审计清单和方案文档。

## 建议先定的方向

不是把所有文件塞进一个文件夹，而是四端统一**分类、命名、归属和清理规则**，各端继续使用系统合适的存储根。

| 类别 | 放什么 | 处理方式 |
|---|---|---|
| 持久数据 | 消息库、会话库、配置、待发队列、上传断点、注销恢复记录 | 不当缓存清理，跟随对应业务生命周期 |
| 工作空间 | 用户工程、导入文件、工具最终输出及会话引用的素材 | 保留；用户自选目录继续使用 |
| 聊天附件 | 消息引用的图片、语音、视频、封面、文件 | 与消息引用关联；不能只看文件年龄或目录名删除 |
| 临时文件 | 收发中转、拍摄/导入/导出暂存、分析与转换工作文件 | 按任务管理，保护草稿、分享、下载、播放中的文件 |
| 可重建缓存 | 可从权威来源重新取得的头像、缩略图、可重取的预览/播放副本、shader | 按容量预算和最近使用清理；完整且不可重取的临时图另作资产处理 |
| 模型 | 内置副本、用户导入、个人训练模型 | 独立统计和管理；不混入普通缓存 |
| 日志与报告 | App日志、SDK日志、诊断导出 | 有独立保留期和总量上限；SDK遵循其接口和活跃写入规则 |
| SDK/系统存储 | SDK数据库、Keychain/Keystore、系统偏好、系统组件 | 优先配置支持的根目录，保留内部布局，不直接搬/清内部文件 |
| 开发与发布产物 | 构建、依赖、测试证据、安装包和归档 | 与产品运行数据分开；不靠ignore规则或名字自动删除 |
| 服务与云端对象 | 服务数据库/恢复记录、服务配置、OSS对象前缀 | 按服务owner独立管理，不与客户端缓存合并 |

## 盘点后确认的问题

1. **临时文件并没有统一写入现有“临时区”。** 粘贴、浏览器、相册导入、云盘播放等各有路径，一些中转文件还在持久目录里。
2. **IM 附件与缓存不能混为一谈。** Windows多种附件位于Roaming；iOS/Android的媒体目录由历史消息引用，且存在App共用媒体根和跨owner引用。
3. **有些“小文件”是恢复依据。** 待发、断点、定时任务、发布journal、账号注销重试状态不能因后缀是JSON或文件很旧就清除。
4. **账号目录键并不统一。** 部分模块按同一登录账号的IM路由ID，部分按其服务端ID；迁移必须建立映射，不能把相似hash当成相同归属。这不是第二套登录账号，也不改变用户登录账号。
5. **云端“临时”不等于本地可随意删。** OSS到期后，本地完整图可能是唯一副本；只有确实可再取得的派生文件才归入可重建缓存。
6. **Windows Local/Roaming、各端大小写和命名不一致。** 需要由一处路径服务统一，避免各模块自行拼接。
7. **图形代码仍有OBS继承命名。** `%ProgramData%/obs-studio/shader-cache`是编译可达的路径，但未查盘、未确认独占；不能当MaiChat目录整清。
8. **系统/SDK内部路径存在不透明项。** 这些项已单列“SDK管理/待核”，没有猜成某个固定缓存目录。

## 目标布局（建议，不代表现在已存在）

以下是逻辑分类，不要求数据库或系统安全存储违背平台规范搬家。原有共享消息库继续按owner隔离，不为统一目录而强行拆库。

```text
持久数据根/
  Stores/IM/                       消息库及一致性附属文件
  Accounts/<账号目录>/
    Agent/                         会话库、设置、定时任务
    Workspace/                     默认创作工作区
    Transfers/                     待发/上传断点/恢复记录
    Profile/                       个人偏好、人像数据
  Media/IM/<账号目录或共享资产区>/  已被消息引用的附件
  Models/                          公共模型与账号个人模型分开
  SDK/<SDK名称>/                   SDK自管数据

缓存根/Accounts/<账号目录>/<功能>/
临时根/Accounts/<账号目录>/<任务ID>/
日志根/{App,SDK,Reports}/
```

- 现有用户自选工作目录和最终导出到Downloads、照片库、Files等位置的文件，不纳入自动搬迁。
- 收到/生成的文件一旦进入消息历史且无法保证可重取，应成为受保护附件，不能继续仅靠临时目录TTL保留。
- 在目标文件旁的`.maitmp-*`、`.partial`等原子写暂存可以保留：同卷发布有其必要性，由统一任务记录管理，而不是硬搬到另一个卷的Temp。
- 安装包内模型/运行库/证书、系统Keychain/Keystore和第三方驱动不进入上述可清目录。

## 四端根目录建议

| 平台 | 持久数据根 | 缓存根 | 临时根 |
|---|---|---|---|
| macOS | 保留 `~/Library/Application Support/MaiChat/Desktop IM/` | `~/Library/Caches/MaiChat/Desktop IM/` | `$TMPDIR/MaiChat/` |
| Windows | 以 `%LOCALAPPDATA%\MaiChat\Desktop IM\` 为产品主数据根；Roaming旧数据经专项迁移认领 | 主数据根下 `cache/` | `%TEMP%\MaiChat\` |
| iOS/iPadOS | `<沙盒>/Library/Application Support/MaiChat/` | `<沙盒>/Library/Caches/MaiChat/` | `<沙盒>/tmp/MaiChat/` |
| Android | `files/MaiChat/` 与 `no_backup/MaiChat/` 按原备份类别分工；SQLite/安全存储保留平台API管理 | `cache/MaiChat/Cache/` | `cache/MaiChat/Temp/` |

Android的Temp与Cache是兄弟目录，互不包含；其他端同样保持清理根互不嵌套。这与现有共享存储服务的根目录契约一致。大体积媒体/模型的备份排除、换机传输、FileProvider/文件分享规则需要同步核对，不能仅改路径字符串。

## IM目录怎么整理

| 状态/内容 | 拟归属 | 什么时候可以回收 |
|---|---|---|
| 录音、粘贴、相册导入、发送前格式转换 | `Temp/.../<任务>/IMSend` 或 `Imports` | 草稿/发送/分享租约结束后；不能弹出分享面板就立刻删 |
| 附件下载中的部分文件 | 任务目录或目标旁原子暂存 | 失败且不再恢复，或完整提交之后 |
| 已发/已收消息仍引用的原图、语音、视频、文件 | `Media/IM/...` | 依据消息引用和用户明确清理操作 |
| 缩略图、可重新生成的封面 | `Cache/.../IM` | 无活跃读写且可再生时 |
| 云端已到期但本地还完整的临时图 | 受保护本地资产/按产品保留策略处理 | 不能仅因云端标记temporary自动删除 |
| SDK离线数据库/内部缓存 | `SDK/...`或SDK规定的位置 | SDK生命周期及官方清理接口，不由普通缓存扫描代替 |
| 待发队列、上传断点、发布/注销恢复记录 | `Transfers`等持久状态 | 完成或明确取消后由原状态机退休 |

## 清理规则建议

- **Temp**：成功、失败或取消后由任务统一退休；崩溃遗留可设24小时保留。回收前同时检查活跃租约和可恢复草稿、outbox、断点、分享等持久引用，进程退出不等于已经无引用。
- **Cache**：设置容量预算，按最近使用淘汰；正在播放、分享、解码、下载的文件保留。自制或唯一头像原片归Profile/Media持久资产；不是可再生缓存的完整附件也不能混入此规则。
- **Logs**：统一入口、分类保留，可先沿用App日志7天并补总量上限；SDK日志不套用未经确认的AppLog文件规则。
- **持久数据、成品、模型**：独立统计与用户操作，不做后台“腾空间”式清理。
- 保留现有“先预览精确集合，再清理”的流程；变动文件、链接、未知归属和仍被引用的项目跳过。

## 实施顺序（本轮不实施）

1. **先收口路径入口**：四端使用同一套逻辑分类/目录登记；平台仅提供系统根。普通页面、IM与工具复用，不能只改工具入口。
2. **先规范新写入**：每个新文件登记用途、owner、任务/消息引用、可重建性、保留规则。新路径不能顺带扩大模型的文件访问权限。
3. **按清单处理旧数据**：记录来源/目标/大小/校验及引用映射；数据库按SQLite一致性备份或关闭生命周期处理，不能单独拷一个正在写的DB文件。
4. **分批迁移并可回退**：活动任务继续使用原路径；无活动租约时迁移，更新消息/会话/队列/缓存引用。跨卷需验证可用空间、复制和校验，同卷可用受控原子移动；不覆盖不同内容的目标文件。
5. **最后退休旧目录**：确认没有旧引用和继续写入者后，再处理空目录/旧副本。迁移兼容是阶段性措施，不长期保留两套正式写入路径。

验证至少包括：四端IM图/音/视频/文件收发、离线与重启恢复、草稿/待发/断点恢复、跨账号引用、远端图云端到期后的本地读取、旧会话工作区与导出路径、分享URI、模型加载、清理时的活动租约、多进程日志、SQLite WAL、失败/断电回退。全部用自有fixture和测试账号；不会以真实用户消息试迁移。

## 服务与开发目录的处置建议

- 服务保持代码、`/etc`受限配置、`/var/lib`持久数据和系统日志分工；auth与sync即使共用代码目录，也保留各自OS owner和DB边界。现有部署示例不是生产实盘证明，本轮不连线上主机。
- OSS对象前缀、进程内credential cache、HTTP/WS连接、systemd PrivateTmp不是同一种“本地目录”，各按原owner和TTL处理，不为整齐而合并。
- 开发统一推荐输出为 `.build/<平台>/<配置>/`、`.artifacts/<版本>/`、`.qa/<run-id>/`，但先以脚本登记/索引收口；保留MaiChat与MaiAgent兄弟布局，不搬源码、Git子模块或固定SDK输入。
- 构建派生物、依赖源码、安装包、正在使用的SDK和验收证据分开标记；Git ignore不等于可删。外部开发通信工具`multi-ai-code/accounts/.../remote-im`排除于MaiChat产品迁移/清理。

## 审计范围与证据

源码/配置候选扫描覆盖1816个自有文件，发现2637处路径相关表达式；它们不是2637个实际目录，网络URL拼接、测试和构建配置已另行归类。目录实例中的账号、UUID、任务ID、环境覆盖值和用户指定目录均为动态值。

- 本机源码基线：`c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`及已授权WIP；目录规则按当前文件核对。
- Windows/Android与services/构建独立源码基线：`b974999b66c7c6ac355099632701195a714ac2da`及报告声明的现有WIP。
- iOS/macOS/共享：45个目录族，见附录A；Windows/Android细目见附录B；服务、对象前缀、构建/SDK/测试见附录C。
- 未遍历真实用户目录、生产DB/环境文件、对象桶或实际SDK产物；未知SDK内部路径、系统崩溃/驱动缓存等按“受系统/SDK管理”明确列出，没有编造绝对路径。
- 共享清理契约参考 `MaiAgent/include/MaiAppStorage.h` 与 `MaiChat/MobileAgentAdapter/MaiAppStorageBridge.h`：App自有根互不嵌套、绑定账号/会话、活动引用保护、固定预览集合；不是对整个系统Temp或Workspace的删除授权。

以下附录是完整来源清单，供后续逐条决定保留、改名、归类或迁移；**不是可直接执行的清理清单**。


## 附录A：iOS、macOS与共享组件

## iOS / macOS / 共享目录审计（只读源码）

P=持久应用数据，C=App缓存，T=临时根，D=Documents，W=会话工作区。iOS P为沙盒Library/Application Support，C为Library/Caches，T为tmp。只记录源码规则，不代表实机已创建或占用统计；不读用户DB/消息/凭据。

| 平台 | 类别 | 当前路径规则 | 用途与保留性质 | 源码 |
|---|---|---|---|---|
| iOS/iPadOS | IM历史 | P/MaiChat/ChatHistory/{messages.sqlite3,*.json} | 完整离线消息/索引；持久数据，不能按cache删除；P不可用时有T下fallback | MaiChat/ios/MaiChat/MaiChat/IM/LocalChatHistoryStore.swift:2020 |
| iOS/iPadOS | IM收件 | P/MaiChat/RemoteIMMedia/Incoming/{Images,Videos,VideoCovers,Voices,Files} | 历史消息引用的媒体/文件；全App共用媒体根，迁移需查跨账号引用 | MaiChat/ios/MaiChat/MaiChat/IM/LocalChatHistoryStore.swift:337 |
| iOS/iPadOS | IM发件和草稿 | P/MaiChat/RemoteIMMedia/Outgoing/{Images,Videos,VideoCovers,Voices,Files} | 草稿/录音/发送副本/封面混合；完成发送不等于文件已无历史引用；排除系统备份 | MaiChat/ios/MaiChat/MaiChat/IM/LocalChatHistoryStore.swift:337 |
| iOS/iPadOS | 设置及登录 | UserDefaults + 系统Keychain | 偏好、账户显示/待办标记与安全令牌；仍由系统API管理，不按普通目录搬迁 | MaiChat/ios/MaiChat/MaiChat/Settings/LocalSettingsStore.swift:95；IM/MaiChatAuthClient.swift:283 |
| iOS/iPadOS | 腾讯SDK | SDK自管数据；日志默认Library/Caches | 产品未指定SDK内部消息DB路径；随包V2TIMManager.h:835仅确认日志缓存根，内部子目录不猜测 | MaiChat/ios/MaiChat/MaiChat/IM/TencentIMClient.swift:167 |
| iOS/iPadOS | Agent账号 | P/AIAssistant/Accounts/<owner-hash>/{sessions.sqlite,settings.json,Workspace,...} | 按owner持久隔离；归属marker会沿用AIAssistant根，Guest是未登录分支而非另一账号 | MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift:140 |
| iOS/iPadOS | 待发队列 | Agent账号根/outgoing-messages.json | 消息重试/恢复状态；不是可随意清理的JSON | MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift:5176 |
| iOS/iPadOS | 人像偏好 | Agent账号根/PortraitPreferences/ | 用户个性化偏好/训练状态；工作区edited-*/skin-mask-*是输出，.portrait-*.tmp是中间文件 | MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MobilePhotoTools.swift:376；MaiPortraitPhotoProcessor.swift:556 |
| iOS/iPadOS | 导入/本地模型 | D/MaiLocalModels/{catalog.json,*.gguf,.import-*} | 模型与导入暂存分开；用户导入/个人模型不能按普通cache删 | MaiChat/ios/MaiChat/MaiChatCore/Assistant/AILocalModelStore.swift:126 |
| iOS/iPadOS | 会话附件上传 | P/MaiSessionAttachmentUploads/*.maicloud.json | 上传断点、幂等和恢复记录，持久任务状态 | MaiChat/ios/MaiChat/MaiChat/CloudDrive/MaiCloudDriveClient.swift:281 |
| iOS/iPadOS | 云盘任务 | P/MaiCloudDrive/<account-hash>/{*.task.json,*.maicloud.json} | 上传任务和断点状态，需由完成/取消状态机退休 | MaiChat/ios/MaiChat/MaiChat/CloudDrive/MaiCloudDriveModel.swift:501 |
| iOS/iPadOS | 云盘相册导入 | P/MaiCloudDrivePhotoImports/ | 上传源和.maicloud-part，物理在持久目录；已有按任务/文件年龄清理逻辑 | MaiChat/ios/MaiChat/MaiChat/CloudDrive/MaiCloudDriveModel.swift:269,636 |
| iOS/iPadOS | 云盘/会话下载 | T/{mai-cloud-drive-playback,mai-cloud-drive-thumbnails,mai-session-attachments,mai-cloud-drive-agent-playback,mai-cloud-drive-explicit-saves}/ | 播放、预览、缩略图、导出中转；explicit-saves并非用户最终保存目录 | MaiChat/ios/MaiChat/MaiChat/CloudDrive/MaiCloudDriveClient.swift:321,491,613,629,646,668,676 |
| iOS/iPadOS | 视频圈 | T/{mai-video-circle,mai-video-circle-marked}/ | 视频下载/加水印中间文件；组件结束后回收，保正在播放/导出的租约 | MaiChat/ios/MaiChat/MaiChat/VideoCircle/MaiVideoCircleClient.swift:235,293 |
| iOS/iPadOS | 远端临时图 | C/MaiSessionTemporaryImages/<account>/<key>/{file,complete.json,staging-*} | 远端图完整缓存/校验收据；云端到期后可能是唯一完整本地副本 | MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift:1106 |
| iOS/iPadOS | 创作导入 | T/AIAssistantImports/ | 相册/视频/文件的创作中转；保草稿/任务引用 | MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift:3036,4485 |
| iOS/iPadOS | 照片工具 | T/agent-photo-<UUID>.jpg | 相册读取/转换临时副本；不是系统照片库原片 | MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MobilePhotoTools.swift:170 |
| iOS/iPadOS | 浏览器 | WKWebsiteDataStore.nonPersistent + W截图 | 系统非持久站点状态、用户可见截图；不推断内部WebKit缓存目录 | MaiChat/ios/MaiChat/MaiChat/MaiChatTools/AIMobileBrowserTools.swift:270,391,694 |
| iOS/iPadOS | 短信待办 | P/AgentSMS/inbox.json | 短信导入/确认待办；持久用户状态 | MaiChat/ios/MaiChat/MaiChat/MaiChatTools/AIMobileSMSInbox.swift:7,41 |
| iOS/iPadOS | 日志 | P/MaiChat/Logs/maichat-ios-*.log | 按DiagnosticLogFileStore的日期/大小轮转，日志不是模型/消息数据库 | MaiChat/ios/MaiChat/MaiChat/Diagnostics/AppDiagnosticLog.swift:53 |
| iOS/iPadOS | 诊断导出 | T/MaiChat-Diagnostics-*.log；T/MaiChatRemoteDiagnostics/*.md | 分享/IM发送报告中转；有导出/过期清理规则 | MaiChat/ios/MaiChat/MaiChat/Diagnostics/RemoteDiagnosticsCoordinator.swift:196；MaiChatCore/Diagnostics/DiagnosticLogFileStore.swift:89 |
| iOS/iPadOS | 安装包资源 | Bundle/{MaiLocalModels,MaiAgentModels,MaiPortraitModels,MaiAgentGraphics,MaiAgentCertificates,Frameworks} | 只读模型/证书/字体/运行库；不是可清缓存 | MaiChat/ios/MaiChat/scripts/build-mobile-agent.sh:83 |
| iOS/iPadOS | 调试验收 | P/MaiChat/{ScreenCaptureSmoke,ScreenFrameAcceptance}/；T/AIAssistantUITest-* | 由特定测试flag/Simulator启用；不混入正常用户数据统计 | MaiChat/ios/MaiChat/MaiChat/MaiChatTools/AIMobileScreenCapture.swift:250；Assistant/AIAssistantModel.swift:1770 |
| iOS/iPadOS | 用户导出/选取 | 系统Photos/Files选取器、用户选定目录 | 用户最终副本和外部原片；不随App缓存或账号退出清理 | MaiChat/ios/MaiChat/MaiChat/CloudDrive/MaiCloudDriveModel.swift；Chat/ChatView.swift |
| macOS | 根目录 | P=~/Library/Application Support/MaiChat/Desktop IM；C=~/Library/Caches/MaiChat/Desktop IM；T=$TMPDIR | Qt产品身份组织MaiChat/应用Desktop IM；不是任意当前工作目录 | MaiChat/desktop/src/main.cpp:74；desktop/qt/qtbase/src/corelib/io/qstandardpaths_mac.mm:63 |
| macOS | IM历史 | P/RemoteIMHistory/<IM-owner>/messages.db | 与Win同一产品逻辑，根映射到Application Support；完整本地历史 | MaiChat/desktop/src/main.cpp:34 |
| macOS | IM收发媒体 | P/{RemoteIMImages,RemoteIMFiles,remote-im-voice,remote-im-videos,RemoteIMVideoCovers,AgentMedia}/ | 与Win目录相同；有消息/草稿引用，不能统称可再生缓存 | MaiChat/desktop/src/im/TimSdkRemoteIMClient.cpp:314；app/RemoteIMApplication.cpp:1707；MaiChatTools/MaiChatHostTools.cpp:387 |
| macOS | SDK | P/{SdkConfig,SdkLogs}/ | 显式传给腾讯SDK；内部子层和账号结构由SDK管理 | MaiChat/desktop/src/im/TimSdkRemoteIMClient.cpp:1979 |
| macOS | Agent账号与工作区 | P/Agent/Accounts/<owner-hash>/{agent.db,agent.db.schedule.sqlite,Workspace,remote-agent-outbox.json,ComposerMedia,Portrait} | 会话、定时任务、待发草稿、已导入媒体、人像资料；持久状态 | MaiChat/desktop/src/ui/MainWindow.cpp:1728；agent/AgentChatPanel.cpp:4137 |
| macOS | 粘贴/浏览器/工具临时 | T/{MaiChatAgentTemp,maichat-paste,maichat-ai-paste,MaiChatAgentBrowser-*}/ | 实际消费者分散；草稿和活动浏览器profile须等宿主结束再回收 | MaiChat/desktop/src/agent/AgentController.cpp:365；ui/MainWindow.cpp:6895；agent/AgentChatPanel.cpp:4120；MaiChatTools/DesktopBrowserTools.cpp:358 |
| macOS | 头像/云盘预览/视频圈 | C/{avatars,CloudDrivePlayback,CloudDriveThumbnails-*,CloudDrivePreview-*,VideoCircle}/ | 可重建缓存/预览临时；尊重正在读写/播放的文件 | MaiChat/desktop/src/ui/MainWindow.cpp:515；ui/CloudDriveDialog.cpp:765,1119；ui/VideoCircleDialog.cpp:116 |
| macOS | 云盘任务 | P/CloudDrive/<account-hash>/ | 上传断点，不是普通cache；与Win共享同一实现 | MaiChat/desktop/src/MaiChatTools/DesktopCloudDriveTools.cpp:236；ui/CloudDriveDialog.cpp:163 |
| macOS | 会话附件 | P/session-attachments/<hash24>/{image-publications,uploads}/；C/session-attachments/<hash24>/{image-cache,downloads}/ | journal/断点与图缓存/下载分两根；同Win组件 | MaiChat/desktop/src/agent/DesktopSessionAttachments.cpp:71 |
| macOS | 日志与报告 | P/logs/；C/remote-diagnostics/ | 应用日志有7天/5MiB轮转；SDK日志与诊断报告有各自规则 | MaiChat/desktop/src/logging/AppLog.cpp:22,38；diagnostics/RemoteDiagnosticsController.cpp:188 |
| macOS | 远程桌面 | P/RemoteDesktop/<owner>/settings.json | 访问模式与本地配置；安全内容仍经原安全接口 | MaiChat/desktop/src/ui/MainWindow.cpp:8853 |
| macOS | 偏好/登录 | QSettings原生Preferences + 系统Keychain | 偏好与安全凭据；不按目录迁移/扫描实际值 | MaiChat/desktop/src/ui/MainWindow.cpp:1702；auth/SecureRefreshTokenStore.cpp |
| macOS | 安装包 | MaiChat.app/Contents/{MacOS,Frameworks,Resources}/ | 可执行/SDK/Qt/模型/证书/字体，随包校验；不是可清数据 | MaiChat/desktop/src/agent/AgentController.cpp:446；desktop/CMakeLists.txt:787 |
| 共享工具 | 截图/观察/二维码 | W/{screenshots,computer,generated}/ | 工作区资产/可能被会话引用；普通截图不是OS临时目录 | MaiAgent/AgentTools/MaiScreenshotTool.cpp:127；MaiChat/desktop/src/MaiChatTools/DesktopComputerTools.cpp:301；MaiChat/MaiChatTools/MaiQrCode.cpp:94 |
| 共享工具 | 屏幕帧历史 | W/.maichat/screen-frame-archive/<owner>/ | 屏幕帧、索引和receipt，由引用/保留策略管理 | MaiChat/Network/MaiScreenFrameArchive.cpp:488；ios/MaiChat/MaiChat/MaiChatTools/AIMobileScreenFrameArchive.swift:121 |
| 共享工具 | 绑定/同步附件 | W/.maichat/session-attachments/ | IM/Session导入的附件副本；持久引用资产 | MaiChat/desktop/src/agent/DesktopSessionSyncCoordinator.cpp:818；agent/DesktopSessionImBridge.cpp:209 |
| 共享工具 | 可再生中间产物 | W/{frames_<id>,wmframes,search,browser,.maiagent/model-previews}/ | 现共享清理允许范围仍需保护清单；移动端设置页只统计Workspace、不开放清理 | MaiAgent/Agent/MaiAppStorage.cpp:47,149；MaiChat/MobileAgentAdapter/MaiAppStorageBridge.h:19 |
| 共享工具 | 原子提交暂存 | 目标旁*.maitmp-*、*.partial、*.tmp*、*.download、.maicloud-part | 下载/媒体/PDF/模型导入的同卷暂存；不能强制全部移入跨卷Temp | MaiChat/Network/MaiCurlDownload.cpp:182；MaiAgent/AgentTools/MaiPdfDocumentTools.cpp:113；MaiChat/desktop/src/MaiChatTools/DesktopPortraitService.cpp:315 |
| 共享工具 | 任意输出 | W或用户选定的绝对/相对输出路径 | 转码、文档、导出等用户产物；不能静态枚举所有用户目录 | MaiAgent/AgentTools/MaiFfmpegTools.cpp:317；MaiAgent/include/MaiTool.h |
| 图形/Windows | 继承的shader目录 | %ProgramData%/obs-studio/shader-cache/*.v2 | 源码可达且已编译；未查盘，未知是否产生/独占，不可当MaiChat整目录清理 | MaiAgent/Graphics/d3d11/d3d11-shader.cpp:252；util/platform-windows.c:446；Graphics/CMakeLists.txt:181,212 |
| iOS/iPadOS | 本地音乐导入 | T/MaiChatLocalMusic/ | 音乐选取/传输的播放中转；保留当前播放/导入租约，用户原始文件在系统选定位置 | MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantView.swift:5121,5166 |

## 附录B：Windows与Android

## Windows / Android 目录盘点（仅源码方案）

日期：2026-11-16。读取基线：HEAD `b974999b66c7c6ac355099632701195a714ac2da`，并读取当前工作区源码；其中 `MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp` 有既有 WIP，未修改。此文只记录路径构造、归属与已有清理规则，**不执行迁移、删除或代码变更**。

没有打开用户数据库、SharedPreferences/凭据实际文件、聊天正文、附件或日志产物；没有遍历 `%APPDATA%`、`%LOCALAPPDATA%`、Android 沙箱或外部开发通信账号数据；没有调用凭据 API、GUI、设备、构建或服务操作。下文中的路径是源码推导，未核对任何运行实例的实际目录。

源码路径缩写（均相对 `E:/OpenSource/MaiChat`）：

- `D/` = `MaiChat/desktop/src/`
- `Q/` = `MaiChat/desktop/qt/qtbase/src/`
- `A/` = `MaiChat/android/MaiChat/app/src/main/java/`
- `A-app/` = `A/app/com/kongshang/maichat/`
- `A-im/` = `A/im/com/kongshang/maichat/`
- `A-ai/` = `A/assistant/com/kongshang/maichat/`
- `A-media/` = `A/media/com/kongshang/maichat/`

每条来源的文件前缀和行号可由上述定义精确展开；行号针对本次读取的工作区文件。

### 1. 根目录、身份和分类口径

| 根/API | 默认规则（不是实机路径枚举） | 账号隔离 / 用途 | 源码 |
|---|---|---|---|
| Windows 产品身份 | Qt organization=`MaiChat`，application=`Desktop IM` | 一份 OS 用户下的产品根；再由子目录或存储键隔离业务账号 | `D/main.cpp:71–85` |
| Windows `AppDataLocation`（下称 `W-Roaming`） | `FOLDERID_RoamingAppData` + `/MaiChat/Desktop IM`，常规示意 `%APPDATA%\MaiChat\Desktop IM`；部分调用为空时退到 `home/.maichat-desktop` | 当前不少“cache”命名的 IM 文件实际落在此持久目录 | `Q/corelib/io/qstandardpaths_win.cpp:42–50,91–106`；`D/main.cpp:72–75`；`D/im/TimSdkRemoteIMClient.cpp:314–319` |
| Windows `AppLocalDataLocation`（`W-Local`） | `FOLDERID_LocalAppData` + `/MaiChat/Desktop IM`，常规示意 `%LOCALAPPDATA%\MaiChat\Desktop IM` | Agent 默认账号数据；不是 Roaming | 同上 Qt 文件 `:91–106`；`D/ui/MainWindow.cpp:1722–1736` |
| Windows `CacheLocation`（`W-Cache`） | LocalAppData + 组织/应用名 + `/cache`，常规示意 `%LOCALAPPDATA%\MaiChat\Desktop IM\cache` | 整 App cache；只有部分具体目录有账号后缀 | `Q/corelib/io/qstandardpaths_win.cpp:160–172` |
| Windows `TempLocation`（`W-Temp`） | `QDir::tempPath()`，取 OS 配置的临时根，不硬编码实际用户名 | OS 用户临时区域；必须只清 App 自己的子树/文件，不清整个 Temp | `Q/corelib/io/qstandardpaths_win.cpp:188–190` |
| Windows `QSettings()` | 默认 NativeFormat/UserScope，组织/应用身份来自上述设置；Windows 通常为 `HKCU\Software\MaiChat\Desktop IM`，不是 AppData 里的一个 JSON 文件 | 部分 UI/Agent 默认项全 App；会话绑定另有账号 scope 键 | `D/ui/MainWindow.cpp:1692–1716,4980–5006`；`D/agent/DesktopSessionSyncCoordinator.cpp:63–74` |
| Android App 私有根 | 由 `Context` API 决定；常见示意 `/data/user/<Android用户>/<applicationId>/`，不可据此硬编码；生产 applicationId=`com.kongshang.maichat`，isolated debug 可加 `.isolated` | Android 用户/工作资料/安装包隔离，与 MaiChat 业务账号隔离是两件事 | `MaiChat/android/MaiChat/app/build.gradle:23–27,58–60`；`A-im/RemoteIMMediaPaths.java:17–22,43–45` |
| Android `filesDir` / `cacheDir` / `noBackupFilesDir` / DB / prefs | 后续表分别写为 `A-Files`、`A-Cache`、`A-NoBackup`；SQLite 使用 `getDatabasePath`；SharedPreferences 由平台维护其目录 | `filesDir` 和 `no_backup` 都是持久数据；“不备份”不等于临时。`cacheDir` 可被 OS/用户清除，但业务上不一定可重新下载 | 各具体构造调用见下表；媒体根迁移边界见 `A-im/RemoteIMMediaPaths.java:13–22` |

分类：`P`=持久状态/用户或消息引用数据；`C`=可重建/可淘汰缓存；`T`=操作临时文件；`M`=SDK 或 OS 管理、App 无法仅凭现有源码给出内部格式。是否在 `cacheDir`/名含 `cache` **不是**清理授权。上传断点、待发送队列、数据库引用附件、已过云端期限的唯一完整本地图片都需要额外保护。

### 2. Windows IM、会话附件、离线状态

| 目录 / 文件 | owner 与根路径规则 | 隔离 / 分类 / 用途 | 已有清理边界及方案约束 | 精确源码 |
|---|---|---|---|---|
| `RemoteIMHistory/<IM userId>/messages.db` | `main` → `LocalMessageDatabase`；`W-Roaming`，为空退 `home/.maichat-desktop` | 每腾讯 IM userId 独立目录；`P`；完整本地聊天/联系人历史，SDK 漫游为补充 | 不能按缓存删除；SQLite 打开期间不能独立搬/删数据库或相关日志文件。离线恢复依赖此库，不是另一个 `OfflineMessages` 目录 | `D/main.cpp:34–39,162–164`；`D/storage/LocalMessageDatabase.cpp:415–431` |
| `SdkConfig/` | `TimSdkRemoteIMClient::sdkConfigJson` 显式传 `sdk_config_config_file_path=appDataDir("SdkConfig")` | 无账号后缀；App 全局 `P/M`；腾讯 SDK 配置/内部状态 | 内部文件、账号子层和离线消息 DB 名称不在产品源码中展开。不能把全目录当某一个业务账号的数据删除；需 SDK 生命周期/官方清理接口约束 | `D/im/TimSdkRemoteIMClient.cpp:314–319,1979–1983` |
| `SdkLogs/` | 同上，显式 `sdk_config_log_file_path=appDataDir("SdkLogs")` | 无账号后缀；SDK 全局诊断 `C/M`，物理位于 `W-Roaming` | 产品此处未给出 TTL/日志文件格式；不要套 AppLog 的七天规则或假定某账号专属；清理时需避免 SDK 正写 | 同文件 `:1979–1983` |
| `RemoteIMImages/<SHA1(url)>.<suffix>` | 腾讯 IM 接收图片路径；`appDataDir` | 无账号目录；`P`（消息引用的持久下载），虽函数名为 cache | URL 摘要不是账号隔离；未在此 helper 看到 TTL/配额淘汰。应依赖消息引用/可重新下载条件决定清理，不按普通缓存整删 | 同文件 `:322–327,2305` |
| `RemoteIMFiles/<SHA1(url)>.<suffix/bin>` | 腾讯 IM 接收文件路径；`appDataDir` | 无账号目录；`P`；历史附件本地文件 | 后缀按真实文件名/URL，未知用 bin；移除会使本地历史附件失效。显式另存的用户副本不能一起清 | 同文件 `:365–372,2335–2338` |
| `remote-im-voice/<SHA1(url)>.amr` | 腾讯 IM 语音下载；直接 `AppDataLocation` | 无账号目录；`P`；按内容解码的稳定路径 | 不应因“voice cache”命名而视为可无损清除；此函数未提供 home fallback | 同文件 `:345–354,2432` |
| `remote-im-videos/<SHA1(url)><suffix>` | 腾讯 IM 视频及封面下载；直接 `AppDataLocation` | 无账号目录；`P`；`.mp4`/`.jpg` 等 | 同上；与视频气泡/封面引用关联。直接 AppData API 为空时的路径行为不能假定仍在统一根 | 同文件 `:356–363,2385,2417` |
| `RemoteIMVideoCovers/` | `RemoteIMApplication` 普通/机器消息发送视频封面生成 | 无账号目录；`P/T` 混合：生成阶段是临时，发送后可能为 SDK/历史引用 | 只有已退休、无消息引用的生成物才适合清；不能一律认为可立即删除 | `D/app/RemoteIMApplication.cpp:1707–1713,2117–2131` |
| `AgentMedia/`（通用文件再加 UUID 子目录） | `MaiChatHostTools` 发送 Agent/绑定媒体前复制；`W-Roaming/AgentMedia` | 无账号子目录；`P`；App 自管副本保留文件 basename | 与 IM 已发消息引用相连；不是 Agent Temp。清理要区分引用和正在发送的租约；不删除外部原始文件 | `D/MaiChatTools/MaiChatHostTools.cpp:387–399` |
| `session-attachments/<SHA256(accountId)前24hex>/image-publications/*.json` | `DesktopSessionAttachments` → `W-Roaming` | MaiChat 账号 hash 隔离；`P`；临时图发布 journal/幂等与重试绑定 | journal 不是图片缓存，不能仅按图片 OSS 到期删；回执/待恢复状态要按组件状态机处置 | `D/agent/DesktopSessionAttachments.cpp:71–78,93–105,156–209` |
| 同账号前缀 `/uploads/*.maicloud.json` | 同组件，`W-Roaming` | `P`；按 room key+源文件路径 hash 的上传断点 | 未完成上传/恢复依赖它；只有明确中止/完成回收契约后清。不能作为普通 cache 整删 | 同文件 `:365–379` |
| `W-Cache/session-attachments/<account-hash24>/image-cache/*.image` | 同组件 | 账号隔离；物理 `C`，业务为完整临时图本地副本 | 完整文件存在且大小匹配可在服务端固定期限后复用；删除后若云端已过期可能无法再取。清理 UI/统一方案必须保留这一不可恢复边界 | 同文件 `:317–340` |
| 同账号前缀 `/downloads/<UUID>.<suffix>` | 同组件；签名 read link 下载 | 账号目录隔离；`C/T`；会话文件预览/下载落点 | 失败会删 target；成功后由使用者生命周期决定，不可清正在传输/预览文件。是否升级为用户保存副本需明确，不等同下载工作区 | 同文件 `:394–442` |
| `Agent/Accounts/<SHA256(IM owner)>/remote-agent-outbox.json` | 实际主产品 `MainWindow` 传给 `RemoteAgentPanel`；`W-Local` 优先 | `P`；每 IM owner 的待发/恢复队列，条目还绑定 owner/session | 不属于离线 SDK 缓存。重启不能证明送达，源码保留原身份并要求显式恢复；不能清理后自动重放或把删除当成功发送 | `D/ui/MainWindow.cpp:1722–1730,5466–5468`；`D/agent/RemoteAgentPanel.cpp:703–730` |
| `W-Local/remote-agent-outbox.json` | `RemoteAgentPanel` 未传参数时的构造 fallback | 非账号目录的默认组件路径；`P`，**不是主产品当前传入路径** | 盘点工具不能只看组件默认构造就误报主产品使用全局 outbox | `D/agent/RemoteAgentPanel.cpp:703–708`；主产品 override 见上行 |

Windows SDK 接收的补拉历史也进入同一产品历史流：`TimSdkRemoteIMClient.cpp:1974–1976` 以非 live 方式派发。这里没有从源码证据推导出另一个可独立清理的“离线消息文件夹”。

### 3. Windows 配置、Agent、临时工作和诊断

| 目录 / 文件 / 系统存储 | owner 与默认规则 | 隔离 / 分类 / 用途 | 清理或迁移边界 | 精确源码 |
|---|---|---|---|---|
| `Agent/Accounts/<SHA256(IM owner)>/agent.db` | `MainWindow`：`W-Local`，空时用 `W-Roaming`，均不可用则返回空，不退 cwd | 账号隔离；`P`；Agent 会话、消息、会话模型等权威状态 | 不按 cache 清；打开/运行中应由 Agent 存储层协调关闭、备份、迁移；账号目录 hash 不是明文账号 | `D/ui/MainWindow.cpp:1720–1736,5336`；`D/agent/AgentController.cpp:291–300` |
| 同路径 `agent.db.schedule.sqlite` | `AgentController::openScheduleStore(databasePath + ".schedule.sqlite")` | 与 Agent DB 同账号；`P`；定时任务/恢复状态 | 不能只搬 agent.db 遗留 schedule DB；不能清成“可重新生成” | `D/agent/AgentController.cpp:302–305` |
| 同账号 `Workspace/` | `MainWindow` 显式给 Agent 页/会话同步默认工作目录；工具默认也在 DB 父目录下 | 账号默认目录；`P`；导入、下载、生成及工作文件 | 属用户工作成果，不能归 cache。会话还可明确指定别的目录；外部目录不得跟着 App 目录批量迁移/删除 | `D/ui/MainWindow.cpp:4834–4838,5475–5479`；`D/agent/AgentController.cpp:349–352` |
| 工作目录 fallback | 若宿主未设置，Agent 页按 AppLocalData、Documents、AppData、Home 顺序尝试其 `Workspace`；已配置目录则校验后使用 | `P`；组件 fallback，不是覆盖主产品账号默认值的第二条正式路径 | 必须以该会话实际配置为准，不假定所有工作区都在 LocalAppData | `D/agent/AgentChatPanel.cpp:258–276,2523–2529,2589` |
| QSettings 默认项/偏好 | `agent/reasoningEffort`、`agent/approvalPolicy`；UI 字体等也使用 QSettings；`session-sync/device-id` 为设备级 | `P`；这些默认项并非全部账号隔离 | 不纳入目录大小 cache 清理；设备 ID 与 UI 偏好迁移需独立定义，不能通过移动文件夹覆盖注册表 | `D/ui/MainWindow.cpp:1696–1716`；`D/agent/AgentChatPanel.cpp:2483–2488`；`D/agent/DesktopSessionSyncCoordinator.cpp:63–74` |
| QSettings `imSessionBindings/<SHA256(identity)>` | `MainWindow` 会话 IM 绑定 scope | 账号/identity 键隔离；`P` | 应按组件的 scope 清，不清整个产品 QSettings | `D/ui/MainWindow.cpp:4980–5006` |
| 系统 Credential Manager 项 | `SecureRefreshTokenStore` 使用 WinCred generic target；默认 `MaiChat Desktop Auth Refresh Token`，其他命名为 service:account | 系统安全存储，不是产品目录；`P`；刷新令牌 | 迁移/退出由账号服务+安全存储 API 完成；禁止文件复制或普通文件明文导出。此盘点未调用 CredRead/读真实凭据 | `D/auth/SecureRefreshTokenStore.h:10–16`；`.cpp:60–71,100–113` |
| `W-Temp/MaiChatAgentTemp/` | Agent 装配时传给 AppStorage tool 的 temporary root | 无账号后缀；`T` | 只清自己这一根且要先退休活动操作；不可清整个 OS Temp | `D/agent/AgentController.cpp:372–376` |
| `W-Cache/` 整 App cache | 同装配传给 AppStorage tool | 无账号总根；`C`（其中细项语义需另判） | 不得声称只影响当前账号；session 临时图完整缓存有到期后不可再下载的特殊边界 | 同文件 `:372–376`；临时图见上一节 |
| `W-Temp/maichat-paste/paste-<ms>.png` | IM composer 剪贴板图片 | 无账号后缀；`T`，发送时读原图 | 尚在草稿/发送的文件不能删；不能连带删除用户剪贴板原始文件 | `D/ui/MainWindow.cpp:6809–6818` |
| `W-Temp/maichat-ai-paste/paste-<ms>.png` | Agent composer 剪贴板图片 | 无账号后缀；`T`，随后导入工作区 | 只在导入/草稿租约结束后回收 | `D/agent/AgentChatPanel.cpp:4123–4130` |
| 用户另存目录 | IM 附件默认建议 DownloadLocation，记住本次 lastAttachmentSaveDir，也可在文件对话框任意选 | 用户文件，`P`；不是 App 管理目录 | 保存后的用户副本不能纳入 App 缓存清理或账号注销；不对默认 Downloads 整目录做动作 | `D/ui/MainWindow.cpp:7258–7287` |
| `W-Cache/avatars/<SHA256(url)>.png` | MainWindow 头像缓存 | URL 隔离，无账号目录；`C` | 可重建但要尊重使用中解码/写入；URL hash 不代表可对一个账号独立删除 | `D/ui/MainWindow.cpp:514–531` |
| `W-Roaming/CloudDrive/<SHA256(accountId)>/` | DesktopCloudDriveTools 上传状态 | MaiChat accountId 完整 SHA256 隔离；`P` | 上传断点/源绑定不是普通 cache，须按上传完成/明确中止流程清理 | `D/MaiChatTools/DesktopCloudDriveTools.cpp:236–242` |
| `W-Cache/CloudDrivePlayback/` | 同组件播放缓存 | 非账号目录；`C` | 使用中的播放租约/校验完成文件要由播放缓存组件回收，不静默删正在播放文件 | 同文件 `:245–249` |
| `W-Roaming/logs/maichat-<date>[-pPID][.N].log` | `AppLog`，App 启动后安装 Qt handler | App/进程级，不按账号；`C` 诊断持久文件 | 单文件5MiB、保留7天；只删匹配自身命名且日期可解析的过期文件；`.instance.lock` 持有至进程退出。不能套到腾讯 SdkLogs | `D/main.cpp:82–85`；`D/logging/AppLog.cpp:22–30,38–60,99–117` |
| `W-Cache/remote-diagnostics/maichat-diagnostics-<UUID>.md` | RemoteDiagnosticsController 合并报告 | nonce 命名，运行时账号/好友 guard；非账号子目录；`C` | 只识别有效 UUID 文件，超过24h删除；未过期最多64份；发送/导出活动时避免外部删除 | `D/diagnostics/RemoteDiagnosticsController.cpp:188–207` |
| `%ProgramData%/obs-studio/shader-cache/<FNV1a(shader)>.v2`（源码推导） | `MaiAgent/Graphics` OBS 衍生 D3D11 后端；`os_get_program_data_path_ptr` 使用 `CSIDL_COMMON_APPDATA`，不是 AppData/Local | 系统共同 ProgramData 根，无账号/产品实例隔离；shader 编译字节码及校验缓存，`C/M` | 名称仍为 `obs-studio`，可能与另装 OBS 共享命名空间。**没有查盘，不能称已经创建、MaiChat 独占或可由 MaiChat 整目录删。** 后续方案可改为 MaiChat 私有 shader cache，但本次不改代码/不迁移。读缓存校验失败会删除该 `.v2` 再编译，说明可重建用途，不构成共享目录删除授权 | `MaiAgent/Graphics/util/platform-windows.c:418–433,446–453`；`MaiAgent/Graphics/d3d11/d3d11-subsystem.cpp:1431–1438,1450`；`MaiAgent/Graphics/d3d11/d3d11-shader.cpp:236–295,301–330` |

没有在 `desktop/src` 查到产品自建的 MiniDumpWriteDump/SetUnhandledExceptionFilter/Crashpad/Breakpad 路径。AppLog 是运行/错误日志，不等同原生崩溃 dump。Windows WER、腾讯/Qt/驱动内部可能另有 OS/SDK 产物，本次源码不足以列出默认路径，不能将其猜测为 MaiChat owned 目录。

### 4. Android IM、离线状态、配置和账号数据

| 目录 / 文件 / 系统存储 | owner 与根路径规则 | 隔离 / 分类 / 用途 | 清理或迁移边界 | 精确源码 |
|---|---|---|---|---|
| `A-Files/RemoteIMMedia/Incoming/{Images,Videos,VideoCovers,Voices,Files}` | `RemoteIMMediaPaths.forApp` + TencentIMClient 接收附件目标 | 全 App 共用根，**无账号目录**；`P`；文件名来自 SDK UUID/remoteId，经安全文件名处理 | 历史 DB 存相对路径长期引用；不放 cache 正是为防 OS/清缓存使历史附件失效。按账号删除前必须排除其他 owner 仍引用的相同文件 | `A-im/RemoteIMMediaPaths.java:13–45,56–84`；`A-im/TencentIMClient.java:454–487,514–516`；`A-im/AndroidChatHistoryStore.java:180–205` |
| 同根 `Outgoing/{Images,Voices,Files,VideoCovers,…}` | RemoteIMMediaStore、IM/绑定发送 | 全 App 共用目录；`P/T` 混合：图片复制、录音、相机图、文件副本、封面；PCM 中间文件为临时 | 已发/草稿引用不可整删；临时 PCM 由录音宿主完成/取消后删；不要对整个 Outgoing 作无条件 TTL | `A-im/RemoteIMMediaStore.java:31–74,77–114`；`A-app/MainActivity.java:381–382`；`A-im/MaiSessionIMOutgoing.java:53–72` |
| `getDatabasePath("maichat-history.db")` 及 SQLite WAL | `AndroidChatHistoryStore` / SQLiteOpenHelper | 单一数据库文件，按 SQL `owner_id` 隔离；`P`；联系人、私聊/群聊、历史检索与离线恢复 | 不是每账号一个 DB。单账号注销不能删整个数据库；已有 purge 在事务里删该 owner 的行，且附件先排除其他 owner 引用；开启 WAL，文件迁移/备份须经 SQLite 生命周期 | `A-im/AndroidChatHistoryStore.java:24–25,69–82,180–205,222–272`；`A-im/RemoteIMSessionController.java:244–253` |
| `A-Files/remote-im-settings/settings.properties` | RemoteIMSessionController + LocalSettingsStore | App 全局最后登录 userId；`P`，不是令牌库 | LocalSettingsStore 只存 `loginUserId`，不应将此文件误当包含全部账号鉴权；切换/注销经 controller 更新 | `A-im/RemoteIMSessionController.java:247–250,286–298,395`；`A-app/LocalSettingsStore.java:12–36` |
| `SharedPreferences("maichat-phone-auth")` + Android Keystore | MaiChatAuthClient | App 当前登录槽；保存账号标识/显示字段和 **Keystore 加密的 refresh**；`P`，非缓存 | 不读取/复制实际密文或 key；由账号退出/撤销流程移除，不以普通文件迁移代替 Keystore 生命周期。访问令牌/UserSig 不从本表推断落盘路径 | `A-im/MaiChatAuthClient.java:32,294–295,304–318,913,1017–1045` |
| `SharedPreferences("maichat-agent-outgoing-"+IM userId)` | AIAssistantController / AIAssistantOutgoingQueue | IM owner 名称隔离；`P`；待发送 entries 与恢复信息 | 不是 SDK 的“离线收件”目录；清除会丢失待发草稿/恢复依据。只通过明确的发送/取消/账号处置流程退休 | `A-ai/AIAssistantController.java:461,474–488` |
| SDK 内部消息/日志目录 | TencentIMClient 调用 `initSDK(context, sdkAppId, new V2TIMSDKConfig())` | `M`；产品未指定 logPath/dataPath，也未把当前 userId 拼为 SDK 目录 | 本次无法从产品源码给出 SDK 默认绝对路径、内部账号子目录、离线 DB 文件或保留期。应另核固定 SDK 文档/自身契约，不能猜成 A-Cache 或 RemoteIMMedia，也不能代用户清目录 | `A-im/TencentIMClient.java:600–631`；依赖版本 `MaiChat/android/MaiChat/app/build.gradle:256`（`imsdk-plus:9.0.7665`） |
| `A-NoBackup/AcceptedAccountDeletions/<SHA256(IM owner)>.json` | MaiChatAcceptedAccountCleanup | 账号 hash 隔离；`P`；已获服务端202回执后的本机清理重试凭据/状态 | 不是 cache。清理中断后靠此继续；目标预检拒绝路径越界/链接，marker 只在同一 request/auth identity 完成后移除。本次未读任何实际 pending 内容 | `A-im/MaiChatAcceptedAccountCleanup.java:32–43,90–134` |

Android 的后台/离线消息先进入同一 SDK 回调、元数据持久化和 `owner_id` 历史库；`TencentIMClient.java:491–505` 可见先持久化再完成 admission。没有从本次源码得到另一个 App 自有的 `OfflineMessages/` 目录，不能把共享历史 DB 或待发队列当作可清离线缓存。

### 5. Android Agent、临时图、下载工作区与诊断

| 目录 / 文件 | owner 与默认规则 | 隔离 / 分类 / 用途 | 清理或迁移边界 | 精确源码 |
|---|---|---|---|---|
| `A-NoBackup/AIAssistant/Accounts/<SHA256(IM userId)>/` | AIAssistantController.accountRoot | 登录账号 hash 隔离；`P`；Agent 根 | 若 legacy-owner marker 对应该 owner，会继续使用 `AIAssistant/` 根而非 Accounts 子目录；空 owner 分支是 Guest。不能看到 legacy 情况就整棵删根，因为 Accounts/Guest 可含其他归属 | `A-ai/AIAssistantController.java:1338–1360` |
| 同根 `sessions.sqlite`、`sessions.sqlite.schedule.sqlite` | Android configure → MobileAgentAdapter | `P`；会话/消息/模型状态，以及独立任务 schedule DB | SQLite 整组维护；schedule 不是可重建临时文件。native configure 接到显式 database 路径；不应自己另造第二个数据根 | `A-ai/AIAssistantController.java:1422–1433`；`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:649–653` |
| 同根 `settings.json` / `trusted-roots.pem` / `Workspace/` | Controller、ModelSettingsStore、证书导出与配置 | `P`；账号模型设置、信任证书副本、导入/生成/下载和用户工作成果 | 设置写入用 AtomicFile 并在 owner 变更时阻断/回滚；Workspace 不是 cache。不能把用户成果随着“清临时”移除 | `A-ai/AIAssistantController.java:1283–1292,1396–1433`；`A-ai/AIAssistantModelSettingsStore.java:16–43` |
| legacy `A-NoBackup/AIAssistant/` + `legacy-owner-user-id.txt`；`Guest/` | 兼容归属根分支 | `P`；需按 marker 归属判定，不代表所有内容同账号 | 已有 accepted cleanup 对 legacy 根明确排除 Accounts、Guest、marker，并把 marker 最后删除；只可复用其精确归属逻辑，不可通用 rmtree | `A-ai/AIAssistantController.java:1338–1360`；`A-im/MaiChatAcceptedAccountCleanup.java:45–68` |
| `A-Files/storage-temporary/` | storageRootsForHost | 无账号后缀；`T`；普通存储 UI 与 Agent tool 共用 temporary root | 必须按当前 owner/current supplier 及活动任务租约清；不等于全 filesDir 可以清 | `A-ai/AIAssistantController.java:1386–1405` |
| 整 `A-Cache/` | 同方法传给 AppStorage | 全 App cache，不是当前账号专属；`C` | OS 可清，但实际业务子目录有临时图完整副本/播放租约等特殊边界；方案不得把清 cache 表述为完全可无损恢复 | 同文件 `:1399–1405,1427–1430` |
| `A-Cache/MaiSessionTemporaryImages/<SHA256(auth account)>/<identity hash>/<signature>/` | AIAssistantRemoteImages.Cache | MaiChat auth account、slot/login/session/message/part/attachment、元数据签名隔离；完整文件+`complete.json`；物理 `C` | 完整 receipt、字节数和 SHA256 匹配后，即使 OSS 已过期仍可复用；删除后可能不可再次下载。不是“到了服务器 expiry 就可安全删本地”的 TTL cache | `A-ai/AIAssistantController.java:462`；`A-ai/AIAssistantRemoteImages.java:24–36,106–132,142–163` |
| 同图片根 `<account hash>/staging-<UUID>/` | 同 Cache.staging/discard | `T`；私有下载发布阶段 | 只删已知 staging 文件与 complete.json，拒绝异常目录/链接，不递归删意外文件；中断 stage 可由后续 OS cache 清理回收，先确认不在活跃传输 | `A-ai/AIAssistantRemoteImages.java:135–175` |
| `A-Files/session-attachments/<auth-account hash24>/*.maicloud.json` | MaiSessionAttachments.upload | MaiChat accountId SHA256前12字节→24hex；`P`；room key+源 canonical path hash 的上传 checkpoint | 断点/已完成回执保持功能状态；不能按下载cache规则清。来源文件可能在Workspace或用户选取区域，不是此目录的子文件 | `A-ai/MaiSessionAttachments.java:107–120`；`A-ai/MaiCloudDrive.java:204–211` |
| `A-Cache/session-attachments/<UUID>.<ext>` | MaiSessionAttachments.download | 无账号目录，运行时 expectedAccount/generation/room key 校验；`C/T` | 失败删target；成功预览/导入期间不得外部清。清整个目录会影响其他账号残留预览文件，不能认为目录名已提供账号隔离 | `A-ai/MaiSessionAttachments.java:145–192` |
| `A-Files/cloud-drive/<auth-account hash24>/` | MaiCloudDrive 上传与断点 | 账号 hash24隔离；`P` | `.maicloud.json.source` 无对应checkpoint时才由启动扫描回收；未完成上传源/checkpoint不可作为普通cache清 | `A-ai/MaiCloudDrive.java:204–237` |
| `A-Cache/AIAssistantCaptures/capture-*.jpg` | Agent 拍照暂存 | 无账号目录；`T`，随后导入Workspace | 相机结果、导入与草稿租约结束后回收；不删除用户相册原件 | `A-ai/AIAssistantController.java:4299–4306` |
| `A-Cache/screen-share-latest.png`、`screen-share-next.png` | MobileScreenCaptureService | 单个当前屏幕共享服务，`C/T` | 必须先停止/退休该服务的写入再清，不能通过一般目录清理打断现行共享 | `A/MaiChatTools/com/kongshang/maichat/tools/MobileScreenCaptureService.java:134,166` |
| `A-Files/diagnostics/{ui.log,ui.previous.log}` | MainActivity → MainThreadMonitor | App/进程诊断，不按账号；`C` 但物理在持久filesDir | UI卡顿/帧耗时和主线程栈，不等同崩溃dump；2MiB轮换为previous；clear通过同一写worker串行，仅删这两个精确文件名，不递归其他诊断 | `A-app/MainActivity.java:375`；`A-app/MainThreadMonitor.java:21–43,77–85,102–109,177–183` |
| `A-Cache/diagnostic-exports/MaiChat-<ms>.txt` | MainActivity 普通诊断导出 | nonce/时间戳、非账号目录；`C/T` | 分享/附件使用后可显式回收，但不能覆盖活动日志或把整个diagnostics目录当导出缓存删 | `A-app/MainActivity.java:4479–4489`；`A-app/MainThreadMonitor.java:114–165` |
| `RemoteIMMedia/Outgoing/Files/remote-diagnostics/<owner tag>/maichat-diagnostics-<id>.md` | MaiRemoteDiagnosticsTransport，为可见IM发送准备的本地受管副本 | owner tag隔离；`P`，因为历史消息引用 | 与普通IM附件相同，不能套24h导出cache TTL。目标仅在写入失败时删；产品历史清理还要检查其他owner引用 | `A-im/MaiRemoteDiagnosticsTransport.java:119–135` |
| `Workspace/maichat-diagnostics-<UUID>.txt` | AIAssistantController workspaceDiagnosticOutputForHost | 账号Workspace；`P` 用户工作文件 | 不等于临时诊断缓存；须由明确的工作区文件管理或用户选择删除 | `A-ai/AIAssistantController.java:4525–4531` |
| `A-Cache/shared-video-*`、头像临时图等 | 显式分享/媒体准备 | 全App临时空间；`T` | 仅已退休的准备副本；不能删原IM媒体/相册用户原件。需要以该操作实际cleanup handle为准 | `A-app/MainActivity.java:5936–5955`；`A-media/MaiChatAvatarImage.java:28,94` |
| 系统相册显式保存 | 新版走MediaStore `Movies/MaiChat`、`Pictures/MaiChat`；旧版走相应公共目录 | 用户公有媒体，`P`，不在App私有目录 | 不能随退出登录、清缓存、卸载前App自清或账号注销删除；这里只核路径，不读取相册 | `A-app/MainActivity.java:6039–6049,6076–6085,6155–6164` |

`MaiChat/android/MaiChat/app/src/main/res/xml/backup_rules.xml` 明确排除 `files/RemoteIMMedia` 云备份，原因是大媒体与配额；这不改变其持久性。`file_paths.xml` 暴露的是 App `files-path`/`cache-path` 的分享能力，不构成这些文件可随意清理的声明。

Android 主 Java 源码中未查到设置自定义 `setDefaultUncaughtExceptionHandler` / Crashlytics / native dump 路径的代码。`MainThreadMonitor` 是明确存在的 UI 诊断日志，不能称作完整崩溃日志。系统 tombstone/ANR、logcat 和腾讯 SDK 私有崩溃日志不由本次产品源码路径构造确认，暂记 `M/未核`，不遍历设备补猜。

### 6. 清理/统一目录方案应保留的边界（不执行）

1. **AppData/Local/cache/temp 是物理位置；持久性是业务契约。** Windows 接收媒体当前在 Roaming；Android IM 媒体当前刻意在 filesDir。统一方案若迁目录，需要同时处理数据库相对/绝对引用、SDK当前路径、活动下载与预览租约；这不是简单搬文件夹。
2. **两种账号身份不可混用。** IM history/Agent默认根主要按腾讯 IM userId 或其SHA256；云盘/Session附件按 MaiChat auth accountId。Android IM历史是SQL owner隔离且媒体文件可跨owner引用，Windows SDKConfig/日志与多个IM缓存又是全App根。不能把一个目录hash通用认成所有模块的同一账号。
3. **临时图 server retention 不等于本地 TTL。** Windows/Android都允许已完整下载的本地副本在OSS到期后继续可读；清理之后可能无法恢复，需在方案中明确保留策略或用户可见损失边界。
4. **待发、上传断点、注销重试、schedule DB 都是持久状态。** 必须按owner组件的完成/明确取消/accepted-account流程退休；不依据后缀 `.json` 或体积把它们列为cache。
5. **普通退出登录与账号注销不是同一授权。** 账号注销已有Android精确owner purge与链接预检，不能替换成“删除整个私有根”。用户保存到Downloads/相册/外部工作区的文件永远不随App cache清理。
6. **日志各有owner。** AppLog七天规则、MainThreadMonitor双文件轮换、远程诊断24h/64份规则不能互相套用，更不能未经SDK契约删SdkConfig/SdkLogs内部文件。
7. **SDK未知项先列待核。** Android腾讯默认日志/离线DB内部位置、Windows SdkConfig内部账号结构、OS级crash目录均未由本次源码确认；方案应列验证入口而非写死猜测路径。
8. **共享 ProgramData 与第三方系统组件不属于 App 缓存清理。** D3D11 shader 路径具有真实编译可达性：Windows 产品开启 Graphics（`MaiChat/desktop/CMakeLists.txt:116–124`），`MaiAgent/Graphics/CMakeLists.txt:173–183` 编入 `platform-windows.c`，同文件 `:207–225` 编入 D3D11 shader/subsystem；设备创建路径调用 shader cache 建目录，shader Compile 读写 `.v2`。这证明代码路径存在，不证明本机已执行。ProgramData 的 OBS 命名空间需先确认共用 owner；近期获独立授权的 ViGEm 安装、驱动/broker/系统目录也绝不能因服务于 MaiChat 而归为 MaiChat cache 可清。本次未查询/操作安装状态、UAC或第三方系统目录。

### 7. 明确排除：外部开发通信工具

`C:\Users\18034\multi-ai-code\accounts\<开发通信账号>\remote-im\...`（包括此前协作收包使用的 `house-maichat` 之类路径）属于 **Multi-AI Code 外部 remote-im 开发通信工具**，不是上述 `MaiChat/Desktop IM` 或 Android App 的产品数据目录。

此边界来自任务明确说明和既有协作上下文，不是通过扫描该目录得到的推断。本次没有读取/遍历其账号数据，也没有将其纳入产品目录清理、迁移、配额或账号注销方案。产品源码中 MIME 表对 `electron/remote-im` 的参考注释也不意味着产品运行时使用那个外部目录。

### 8. 验证边界

这份清单是只读源码检查结果，不是磁盘占用统计、敏感数据扫描或实际清理验收。根目录API的返回值未求值；没有证实当前运行产物是否已采用同一HEAD/WIP，也不宣称原生/设备验证。SDK overlay 的六个新增文件保持 freeze，本任务未修改它们或任何产品源码；唯一写入为本 Markdown。

## 附录C：服务与开发构建

## services 与开发构建目录（只读方案 checkpoint）

日期：2026-11-16。源码基线 HEAD `b974999b66c7c6ac355099632701195a714ac2da`；此表只读仓库内已跟踪服务源、README、示例部署配置、构建脚本及目录规则，未核实实际运行部署。**唯一写入是本 Markdown**。没有读取实际 `.env`、数据库、账号消息、上传对象、凭据、密钥或证书内容；没有执行服务启动/部署、迁移、清理、测试、构建或 GUI/设备操作。

下文路径均为源码/示例约定；环境变量可覆盖，不能等同当前生产主机实际路径。`P`=持久数据/恢复状态，`C`=可重建缓存/构建派生物，`T`=操作临时，`M`=宿主或第三方工具管理。Git ignore 规则不是删除授权；`C/T` 也必须先确认没有运行任务、构建、下载、打包或证据保留租约。

引用缩写：表中独立写出的 `store.js`、`session-sync-store.js`、`session-sync-service.js`、`deploy/...` 均相对该行明确标注的服务根（账号/同步为 `services/auth/`）；`build-qt.py` 相对 `MaiChat/desktop/scripts/`；独立 `app/build.gradle` 固定指 `MaiChat/android/MaiChat/app/build.gradle`。所有行号针对本次读取的工作区源码，未带入实际环境文件或数据文件内容。

### 1. 服务持久数据与部署 owner

| 目录/对象 | owner、根/API/默认或示例 | 类型、隔离与用途 | 必须保留/清理边界 | 精确源码 |
|---|---|---|---|---|
| 账号 SQLite，示例 `/var/lib/maichat-auth/auth.sqlite` | `services/auth/server.js` 必须取得 `MAICHAT_AUTH_DB_PATH`，没有临时目录 fallback；`openStore(filename)` 建父目录 | `P`；单一服务 DB，按用户/account/owner 等 SQL 字段隔离，包含账号、登录会话、安全挑战/失败记录、好友/资料同步、注销恢复、举报/审计、云盘/附件元数据 | 不能当缓存删；保存数据库及 SQLite WAL/SHM 一致状态。`mkdir mode0700` 只说明新父目录创建意图，不证明现部署权限已正确 | `services/auth/server.js:215–225`；`services/auth/store.js:8–15,17–222`；`services/auth/deploy/auth.env.example:2` |
| `/var/lib/maichat-auth/` 示例宿主持久根 | systemd 示例 `User/Group=maichatauth`、`UMask=0077`、`ReadWritePaths` 指定该根，代码在 `/opt/maichat/auth` | `P`；服务账号拥有的运行数据；代码安装目录与数据根分开 | 不根据仓库 `.gitignore` 或文件后缀清整根。真实 DB env 值若改到别处，必须同时核 systemd 可写边界；本次未读实际环境/单元 | `services/auth/deploy/maichat-auth.service.example:8–20` |
| Session Sync SQLite，示例 `/var/lib/maichat-session-sync/session-sync.db` | `MAICHAT_SESSION_SYNC_DB_PATH` 必须配置；`openSessionSyncStore` 建父目录，WAL、FK开启 | `P`；按 account/slot/login_session_id/session 隔离；设备/Session目录、临时信箱、去重收据、持久 room messages/state | 不是全部“临时同步缓存”。`cleanup()`只按已定义期限删 envelope/receipt；room 历史/状态的删除另有所属权限操作。不能清整个 DB 代替清信箱 | `services/auth/session-sync-server.js:248–263`；`session-sync-store.js:7–15,15–84`；`session-sync-service.js:189–192,509–514`；`deploy/session-sync.env.example:3` |
| `/var/lib/maichat-session-sync/` 示例宿主根 | systemd `maichatsync` 用户组、0077、独立 ReadWritePaths | `P`；与 auth 分离的 OS owner | 不能因与 auth 共用 `/opt/maichat/auth` 源码目录而把两者数据混成一个 owner。备份/恢复要保留各自权限、数据库一致性和登录代际关系 | `services/auth/deploy/maichat-session-sync.service.example:9–21` |
| friend mirror 运行状态 | `friend-mirror.js --run` 读取同一 auth DB；独立 systemd oneshot/timer | `P` 状态仍在 auth SQLite，不是另一份文件缓存 | 不另造 `friend-cache/` 删除策略；退出/同步失败需看 DB job 状态，不能把持久同步重试当临时文件 | `services/auth/friend-mirror.js:110–134`；`deploy/maichat-friend-mirror.service.example:7–18`；`store.js:123–178` |
| 云盘、Session附件、临时图发布元数据 | `cloud_drive_videos`、`cloud_drive_folders`、`session_attachments`、`session_image_publications`、`session_image_publication_generations` 均在 auth DB | `P`；owner_user_id/account/房间/发布代际约束。上传状态、配额和 `cleanup_pending` 是恢复状态，不是本地文件内容 | 即使远端对象过期，小型发布身份回执仍用于幂等/防重放；不能按24h删回执或把失败清理行删掉假装成功 | `services/auth/store.js:269–358,417`；`services/auth/README.md:104–116,157–216` |
| 视频圈元数据/审计/待清理状态 | auth DB `video_circle_posts/blocks/reports` 等 | `P`；按作者/用户隔离，审核与远端对象状态有关 | 不与用户云盘元数据等同；需要按已定义审核/注销/远端删除确认流程清理 | `services/auth/store.js:222–268`；`services/auth/README.md:110–116` |
| 账号 DB 的安全过期行 | auth 启动先补 `cleanupExpiredRecords()`，之后每天维护 | 数据库内期限清理，不是磁盘目录 TTL | 只按源码指定的 sms/password/QR/social/session 条件删行；不能扩展成“所有旧文件”或自行缩短留存 | `services/auth/server.js:258–262`；`services/auth/service.js:151–166` |
| 注销/持久对象删除恢复记录 | account deletion、CloudDrive/Session attachment cleanup队列与对应 DB表 | `P`；完成/失败恢复必须可追踪 | 服务每小时等周期尝试远端清理；源码中的 `cleanup_pending` 不能以删 DB/文件夹替代。此盘点不执行任何 cleanup endpoint/script | `services/auth/server.js:263–275`；`services/auth/README.md:110–116,169–173`；`store.js:189–203` |
| SQLite/策略备份的显式目的地 | DB online-backup helper 接受已批准 private directory+basename，新文件O_EXCL/NOFOLLOW、0600；RAM/lifecycle工具要求显式新 backup | `P`，运维恢复材料，可能敏感；路径由操作员指定，仓库无统一自动缓存位置 | 必须 preserve。未完成备份也带 incomplete 标志，不应被一般 cache cleanup 静默抹掉。不能为做目录方案执行这些脚本或读取现有备份内容 | `services/auth/scripts/prepare-image-generation-backup.py:19–65`；`services/oss-media-signer/scripts/upgrade-session-temp-image-ram.py:94–109`；`upgrade-session-temp-image-lifecycle.js:77–84,99–121` |

`auth` 与 `session-sync` 存储均显式允许测试传 `:memory:`；这不意味着生产 DB 是可重建临时数据。服务启动需要 env 路径，表中 `/var/lib/...` 是示例部署值。

### 2. 云端 uploads、凭据、tmp/cache/log/socket/runtime

| 路径/资源 | owner 与规则 | 分类与边界 | 精确源码 |
|---|---|---|---|
| OSS `cloud-drive/<account>/...` | 素材签名服务与账号服务联合控制；客户端直接与私有 OSS 传字节 | 远端 `P` 对象，不是服务主机 `uploads/` 目录；上传完成/未完成分片、配额、删除确认依赖auth DB及OSS状态。桶名/部署值本次不读取 | `services/auth/README.md:101–145`；`services/oss-media-signer/README.md:50–60`；`index.js:155–157,250–269` |
| OSS `session-attachments/<account>/YYYY-MM-DD/<UUID>.<ext>` | 持久Session附件专用前缀，与个人云盘分开 | 远端 `P`；未完成Multipart有8天自动中止配置契约，不代表完成附件8天删。房间清理需权限和远端确认 | `services/auth/README.md:157–173`；`services/oss-media-signer/README.md:50–60` |
| OSS `session-temp-images/<account>/...` | 临时图专用前缀；auth固定首次预留起24h到期，OSS生命周期为兜底 | 远端 `T/P` 状态组合：对象有期限，发布身份回执需持久。专用生命周期不改变持久云盘/Session附件规则 | `services/auth/README.md:176–219`；`services/oss-media-signer/README.md:98–131` |
| 其他签名对象前缀 | `ark-asset-inputs`、`avatars`、`kling-inputs`、`minimax-inputs`、`video-circle`、`seedream-inputs`、`wan-inputs` 按purpose固定 | 云端对象；不是主机文件夹。生命周期和owner要按用途分别核，不用一个统一“上传cache TTL” | `services/oss-media-signer/index.js:30–39`；README `:235–248` |
| model-credentials 临时凭据 | handler从受限环境读长期密钥，签发短期供应商凭据；监听loopback9200 | 当前handler/入口未定义本地 DB/uploads/cache 路径；请求/结果在进程内，响应no-store。不因名称“credentials”推断有可搬/可清文件目录 | `services/model-credentials/README.md:3–13`；`index.js:8–10,28–74`；`server.js:45–48` |
| auth ASR 临时凭据缓存 | `asr-credential.js` / `aliyun-asr-credential.js` 的闭包 `cached` 与有效期检查 | 进程内 `C`，不是磁盘目录；失效先丢旧值，不写长密钥/返回值到源码或日志。重启会丢缓存但不等于获准重启服务 | `services/auth/asr-credential.js:48–72`；`aliyun-asr-credential.js:31–51` |
| `/etc/maichat/{auth,session-sync,model-credentials,media}.env` | 仅部署示例的 EnvironmentFile 路径；auth/sync各自OS用户，凭据服务/media为maichat | `P/M` 配置/签名材料，**must-preserve，禁止当cache、禁止内容扫描/复制到报告**。systemd管理器读取后传给进程，不需要放宽为服务用户可直接读 | 各 `deploy/*.service.example`：auth `:8–19`，sync `:9–20`，model `:8–18`，media `:8–18`；`services/oss-media-signer/README.md:86–95` |
| `/etc/maichat/cloud-drive.env` 与文档提到的历史受限key JSON | README给出的运维配置/历史凭据路径 | `P/M`，不能由目录清理方案删除。文档指出历史JSON当前服务不加载，不等于它可以删；需明确系统owner和撤销流程。本次只记录路径类别，不读内容 | `services/oss-media-signer/README.md:86–95,182` |
| 服务临时目录 | 四个systemd示例均 `PrivateTmp=true` | OS服务私有 `T/M` namespace；没有源码固定 `/tmp/uploads`、`cache/`、`runtime/` 本地存储规则。不能从单元内 `/tmp` 推断同名宿主目录，更不能清全系统tmp | auth单元 `:20`、sync `:21`、model `:16`、media `:16` |
| HTTP/WebSocket socket | auth 127.0.0.1:9100、admin9101；sync9102；credentials9200；media9000；WS连接Map是内存 | 运行时OS描述符/进程状态，非可删磁盘socket。当前服务入口没有Unix socket pathname、PIDFile或RuntimeDirectory配置；`RestrictAddressFamilies`允许AF_UNIX不代表创建了.sock文件 | auth `server.js:255–308`；sync `session-sync-server.js:59–79,248–263`；model/media各`server.js:45–50`；部署示例 |
| 服务日志 | 源码用stdout/stderr；systemd示例未配置专用日志目录/文件 | `M`；由宿主service manager/journal配置决定留存。仓库 `.gitignore` 的logs/、*.log是开发忽略规则，不是生产日志清理策略。本次不检查journald/nginx实际目录/配置 | `services/auth/server.js:272,292`；四个service示例；根 `.gitignore:50–57` |
| 静态法律站点 `/var/www/ichat` | tracked `services/legal/site/` 发布内容，由Web部署owner管理 | 源发布产物/网站内容，不能当App缓存；Nginx/TLS密钥等宿主材料未在此目录盘点中读取。此处不评审法律文本或作法律建议 | `services/legal/README.md:1–8` |

#### 容器 volume / host 边界

在 **services 自身的已跟踪文件** 中未发现第一方 Dockerfile/Compose/volume manifest；当前明确部署契约是 systemd+host路径。第三方curl/llama/onnxruntime仓库里的 Dockerfiles 属依赖开发/CI，不是 MaiChat 服务的生产卷规范。

若未来容器化，`/var/lib/...` 只是容器内目标名，不能据此猜宿主bind源、named volume实际路径或owner。方案必须显式登记 host source / container target / UID-GID / 数据库一致性备份 / secret只读注入；持久DB与WAL不应留在可替换image层，配置与签名材料不能混入镜像或普通构建缓存。本次没有查询容器、宿主挂载或运行服务，因此不宣称实际存在某volume。

### 3. 开发、构建、SDK与打包目录

这些是开发机器/CI目录，不是 MaiChat 用户产品数据；不能把用户 AppData、Android沙箱或生产 `/var/lib` 搬进这里统一“清缓存”。重建成本只给相对等级，未测磁盘占用/用时。

| 目录/根 | owner/API/规则 | 用途、重建成本 | 不可自动删除边界 | 精确源码 |
|---|---|---|---|---|
| 根 `build/` | 根ignore只明确列出生成图标 `build/icon*.png/.icns/.ico`；CMake build树由 `-B` 或脚本参数决定 | 不是已证明的单一统一构建根；其中具体已声明生成图标成本低 | 不能从目录名字把整棵build一键删。先区分源码资源、调用方选的构建树、当前运行程序/其他任务持有目录 | 根 `.gitignore:33–37`；下面各构建脚本参数 |
| `MaiChat/desktop/build`（打包默认） / `build-agent`（测试脚本默认） / 其他自选 `-B` | package脚本默认BuildDir=build，run-tests默认BuildDirectory=build-agent，均相对desktop | CMakeCache、目标对象、测试/程序、子项目/vendor；成本中到高，必须与源码/编译器/依赖一致 | 活跃normalcache、链接/测试、正在运行exe、其他agent lease不能删/搬；CMakeCache包含本机SDK路径，不作为可跨机器复制的配置真相 | `MaiChat/desktop/scripts/package-windows.ps1:14–27`；`run-tests-windows.ps1:4,13–23,74` |
| 根或子目录 `.cache/` | 根ignore `**/.cache/`；很多独立验证/分析/下载/fixture通过调用方自选这里 | `C/T`混合，可能还有审阅证据、恢复资料、源码快照/离线JDK；成本不一，本次未遍历内容 | ignore不证明可清。要按manifest/任务owner识别，当前验收证据与独立构建lease保留；不能用全树删除代替目录归属方案 | 根 `.gitignore:20`；构建/测试脚本接受外部build/output目录的约定 |
| `MaiChat/desktop/qt/{qtbase,qtsvg}` 与 `sources.lock.json`、Qt patches | Git子模块源码+锁/受控补丁，是固定Qt SDK的源输入 | source，非cache；离线重建依赖它们，替换会改变UI/Unicode等行为 | 必须保留Git身份、锁、补丁、许可证及未提交工作，不改兄弟工程布局。不能为清SDK build顺手删源树 | `.gitmodules:1–8`；`MaiChat/desktop/scripts/build-qt.py:121,129–159` |
| `MaiChat/desktop/qt/.build/<system>-<arch>/` | build-qt.py构造；每module子build，`install[-sdk_profile]`、`installed[-profile].json` | Qt编译缓存+安装前缀+验证stamp；重建成本高，脚本要求至少12GiB可用空间（不是本机占用测量） | 多个产品构建共享SDK；当前被引用的prefix不可删。stamp与源码dirty fingerprint、编译器等共同决定可重用性，不能只留stamp删库或跨profile覆盖 | `build-qt.py:128–137,159–193,211–225`；`MaiChat/desktop/cmake/MaiChatQt.cmake:22–54` |
| FFmpeg/codec/OpenSSL/ONNX 的 build与install | FFmpeg脚本接受BuildDirectory/CodecInstallDirectory，输出`<build>/install`；OpenSSL同类显式BuildDir/install；ONNX Android根在`MaiAgent/build/vendor` | 编译派生 `C`，依赖固定源、工具链、ABI、许可证；成本高 | 不删第三方源/补丁/许可证；不移动已有install破坏CMakeCache/运行包依赖。活跃编译、打包、被测试加载的DLL/SO必须保留 | `MaiChat/desktop/scripts/build-ffmpeg-windows.ps1:5–24,52–64,86–87`；`MaiAgent/scripts/build-openssl-windows.ps1:5–20`；`build-onnxruntime-android-jni.sh:12–31` |
| `MaiAgent/build/vendor/onnxruntime-android-{build,jni}` | 脚本分别为编译中间根和ABI产物根；Gradle从jni根收SO | native编译/被消费产物 `C`；重建成本高 | 删除后Android preBuild明确失败，不能把它当无影响的app/build。必须先具备本地源/工具链才能离线重建 | `MaiAgent/scripts/build-onnxruntime-android-jni.sh:12–31`；`MaiChat/android/MaiChat/app/build.gradle:15,163,224–235` |
| 桌面dist/staging/zip | package默认OutDir=dist，staging=`<OutDir>/MaiChat-win64`；zip名含版本/日期/git短hash | 可发布产物与PE/运行时核验对象，重建成本中；不是产品用户目录 | 脚本会重建同名staging/zip，这并不授权现在执行。当前运行/验收使用的解压/staging要保留，发布证据和来源receipt要随版本留档 | `MaiChat/desktop/scripts/package-windows.ps1:8–16,57–63,315–327` |
| `desktop/vendor/tencent-im/.../ImSDK.dll` 与分发副本 | package从固定vendor相对路径取腾讯SDK，复制到staging vendor树 | 外部SDK输入/再分发受许可；不是自动可重建cache | 不能因文件是DLL就删；须保留对应SDK版本、来源和许可。不能与产品运行中的SdkConfig/SdkLogs混为一类 | `MaiChat/desktop/scripts/package-windows.ps1:122–126` |
| `MaiChat/desktop/third_party/{libvirtualhid,ViGEmClient}` | 当前工作区Git子模块输入 | 源/许可证/协议基准，非App cache；外部系统驱动/broker安装又是独立owner | 删除源码/安装驱动/启动broker/清系统目录互不等价，不能纳入普通缓存清理。当前受控overlay的patch/lock/generator属于审阅源输入；生成副本是构建派生，需保留当前A/B证据/receipt | `.gitmodules:9–14`；既有SDK overlay准备记录（本次未读取/修改overlay六文件） |
| Android project `.gradle/` | Gradle项目状态，根ignore按Android匹配；全局`GRADLE_USER_HOME`另由开发环境确定 | 依赖/任务/增量元数据 `C/M`；重建成本中 | 活跃Gradle/daemon/任务锁不可删。全局Gradle缓存可被多个工程共享；不是App缓存，不能按此仓库清全局目录 | 根 `.gitignore:77–80`；`MaiChat/android/MaiChat/gradle/wrapper/gradle-wrapper.properties:1–5` |
| Android `app/.cxx/mobile-agent-adapter` | externalNativeBuild.buildStagingDirectory显式配置；下层ABI/variant/hash由AGP管理 | CMake/NDK对象、配置、native staging `C`，重建成本高 | ABI/NDK/Java/路径关联，不能跨机器原样当配置迁移；活跃构建/编译数据库引用要保留 | `MaiChat/android/MaiChat/app/build.gradle:20–26` |
| Android `app/build/`、APK/测试报告/资源stage | Gradle输出根；generated Graphics、bundled-primary-model、TLS/portrait测试assets等子根由tasks指定；APK属AGP输出 | `C` 构建产物，但可含大模型副本、打包许可证、测试报告/签名产物；成本中到高 | 不删除源模型/许可证来“重建”；APK install/发布验收记录需保留版本对应。签名keystore配置来自外部环境，**不是**app/build里的可再生缓存 | `app/build.gradle:95–163,178–215`；根 `.gitignore:77–80`（忽略build和local.properties） |
| 固定模型源与Android大模型stage副本 | Gradle从共享`MaiChat/ios/MaiChat/BundledPrimaryModel`准备固定主模型到`app/build/generated/bundled-primary-model-assets/MaiLocalModels/Primary`，同时带manifest/来源/许可证 | 前者是共享受校验输入，后者是体积较大的派生副本；离线重建依赖源权重存在、校验和与许可证完整 | 不能因`.gguf`/`.onnx`大就视为随手可重下cache；不删源权重、Git LFS恢复对象或许可证。stage可重建也需先满足源输入和空间条件，不影响另端消费同一源 | `MaiChat/android/MaiChat/app/build.gradle:137–159` |
| Android SDK/NDK/JDK、`local.properties`、签名材料 | 开发工具/外部配置；wrapper固定Gradle发行版相对`GRADLE_USER_HOME/wrapper/dists` | `M`，非Repo/App数据；本机SDK路径配置不可与用户业务配置混同 | 本次未读实际local.properties、keystore或全局工具缓存。不能让产品清理方案删除工具链/签名材料；重建APK不代表可以重建keystore | `app/build.gradle:8–11,20–21,43–52`；`gradle/wrapper/gradle-wrapper.properties:1–5` |
| `.git/`、Git LFS对象与source submodules/vendor | 版本身份、索引、子模块对象、可能的本地提交/大模型对象 | source/recovery材料，非普通cache；丢失后可能无法离线恢复或丢未推送工作 | 不自动清理/移动；`third_party/vendor/models` 名称本身不决定可再下载性。必须保留license/notice/lock/patch，遵守MaiChat和MaiAgent兄弟布局约束 | `.gitmodules`；构建脚本固定相对source/lock路径；根AGENTS约定 |

### 4. 服务测试与fixture边界

| 位置/规则 | 用途与成本 | 清理边界 | 精确源码 |
|---|---|---|---|
| auth/sync内存DB与fake OSS maps | 常规unit/integration使用 `openStore(':memory:')` 或 `openSessionSyncStore(':memory:')`；假对象/上传映射在内存 | 不产生一个可纳入产品清理的持久目录；测试通过不代表生产数据可重建 | `services/auth/service.test.js:18`；`cloud-drive-integration.test.js:40,94–97`；`session-sync.test.js:12–14`；`services/integration/account-media.test.js:12` |
| `os.tmpdir()/maichat-auth-test-*`、`maichat-auth-qr-test-*`、image/steering/room-upgrade等mkdtemp根 | 文件DB、重启/迁移fixture；运行时新建 | fixture在finally关闭DB后删其**自己创建**的目录。不能只凭同前缀删系统临时目录中的任意文件，也不读取实际残留来猜归属 | `services/auth/service.test.js:1415–1456,1461–1521,1526–1556`；`session-sync.test.js:177–192,641–684,754–771` |
| services各`node_modules/`、lock文件 | npm依赖（包含本地SQLite addon/WS等）；node_modules可重装但受Node ABI/平台和离线包缓存影响 | `package-lock.json`/package.json是源，不删；活跃Node/测试持有的模块、现服务代码目录不自动变更。本次未安装npm或扫描已装目录 | `services/auth/package.json`、`package-lock.json`；`services/auth/.gitignore:1`；根 `.gitignore:1–2` |
| 已跟踪测试资源/证书fixture与生成测试TLS assets | 固定视频/图片/CA/keystore等是测试输入；部分TLS材料由Gradle在build/generated临时生成 | **本次只列路径规则，不读密钥/证书内容**。tracked fixture/许可/校验基准不能按cache删；generated测试材料仅在对应测试退出且证据已留后可重建 | `MaiChat/android/MaiChat/app/build.gradle:178–215`；Android `src/androidTest/assets/`（仅tracked路径名） |
| 测试证据/截图/日志/receipt | 源码与部署来源、hash、失败trace、验收截图等常由显式cache输出目录接收 | 不是业务用户数据，但在审阅/事故/发布期间是必须保留的证据；应先按任务owner与保留期登记，不做全cache整树删 | 根ignore只说明忽略；本轮方案本身也在`.cache/directory-plan-20261116/` |

### 5. 当前方案的排除项与待确认项

- 真实服务env、数据库文件/备份、密钥证书、对象桶、容器卷、journald/nginx日志和挂载owner没有被本次读取或验证。路径示例不是实盘存在证明；不能据本表直接部署或清理。
- 没有定义一个统一的主机 `uploads/tmp/cache/logs/socket/runtime` 树。云端OSS前缀、SQLite内持久状态、进程Map、systemd PrivateTmp、开发构建缓存属于不同owner；命名相近不应合并。
- 用户AppData、Android私有数据、生产服务`/var/lib`、运维`/etc` secret、外部SDK/系统驱动、开发`.cache`要分别制订归属/授权/备份/恢复/活动租约规则。
- `C:\Users\18034\multi-ai-code\accounts\...\remote-im\...` 是外部开发通信工具数据；继续明确排除于MaiChat服务和产品目录方案。本次没有遍历该目录。
- 当前PC原生测试、驱动安装/UAC、系统第三方目录均由root另行管理；本任务没有查询或影响它们。SDK overlay六文件未改。所有条目是只读checkpoint，可供方案peer合并，不是整理实现或已完成清理验收。
