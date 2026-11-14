# Android 远端旧图片的显式单张重新加载

日期：2026-11-15（Asia/Shanghai）。实现基线：`384801817451d2c9c7f4adf0e3ce7f63606ecfa1`。本阶段直接在 `main` 修改，不创建分支，不自行提交、安装、启动 App 或执行真实账号请求。

## 复现与预期

在 Android 创作中心打开同账号电脑上的会话，找到缺少 `remoteImage` 回执、发布失败或临时对象到期的 `kind=image` 片段。原先只能显示所属设备占位或到期状态；现在用户点击这一张的“从所属设备重新加载”，应只准备这一个原消息/片段，取得完整发布回执后显示缩略图。源设备离线时明确提示并禁用来源请求。已有完整本地副本先显示，不能因点击而续期、上传或重复下载。

普通列表绑定只允许读取既有完整缓存、下载已有合法回执，不自动准备来源图片，不批量回扫历史，不调用模型。没有独立 `MaiImagePart` 的 JSON-only `agent_send_media` 工具结果不在本次入口中转换为图片。

## 已批准设计与执行顺序

1. 先用自有 fixture 复现缺少普通显式入口；保留原消息和片段 ID，旧图不注入假回执。
2. Controller 增加独立可变命令适配 `ImagePublicationCommands`；不把可能上传来源图片的操作放进标明只读的 `SessionReads`。
3. 点击先在附件 I/O worker 验证当前完整缓存。只有缓存未命中才重新读取在线目录中的当前设备、登录会话和 session，并发出一次 `prepare_image`。
4. 一张图片的并发点击共享一个票据。票据绑定账号、登录代次、选择代次、原设备元组、消息、片段和原元数据；切换后撤销本地票据，晚回执不得写新页面。
5. 成功回执严格验证后，只替换原片段的 `remoteImage`，先发布可见 State，再复用既有共享 C++ 下载与完整缓存入口。旧版本、不在线、来源失败及超时均结束请求，只有下一次明确点击才重试。
6. UI、Controller、桌面来源发布和服务端续期分别由独占 writer 修改，串行使用同一 Android Gradle 输出目录；最后执行联合回归并冻结源码哈希。

## 协议边界

请求载荷严格为四键：

```json
{"action":"prepare_image","requestId":"<request UUID>","messageId":"<persisted message ID>","partId":"<persisted image part ID>"}
```

设备 slot、当前 owner login 和 session 来自已鉴权信封。调用者不能覆盖账号、来源路径或 URL。成功返回 `ok/requestId/messageId/partId/remoteImage`，`remoteImage.state` 必须为 `ready`，并保有既有完整临时图片身份、大小和 Unix 毫秒到期时间。未到期的旧 ready 对象可复用原发布登录身份，当前回执信封仍需匹配当前目录 owner。

新可选 `remoteImage.generation` 是 `0..9007199254740991` 的安全整数，缺失等于旧代 `0`。客户端仅接受原代或精确下一代的成功回执；并发 pending 可以先广播，但不能冒充成功。若当前图片已进入更新代或另一个不同的 ready 回执，旧 ACK 不得覆盖它。代 `0` 保持 276 已完成缓存的原签名；非零代进入缓存元数据签名。

`generation` 不进入共享下载器的严格七字段载荷。客户端仍向 `MAI_SESSION_IMAGE_DOWNLOAD` 仅传 `attachment_id/name/content_type/media_type/size_bytes/expires_at/output_path`，不取得或自行消费签名 URL，不增加服务器媒体转发。

兼容旧客户端时，同一已鉴权 owner 和精确 `command_id` 返回既有“不支持的远端操作”等拒绝，显示“所属设备版本不支持重新加载图片，请更新所属设备”。错误 owner 或错误 request/message/part 回执不消费有效票据。每个命令最多等待 120 秒，无自动轮询。

## 复现与修复证据

- 新普通入口缺失红测：`.cache/android-remote-images-20261115/publication-red.log`。提供完整、已到期但仍保有本地副本的 fixture，源设备离线时应直接显示该副本；修复后无需 prepare 或下载。
- 扩展命令用例验证精确四键、并发合并、错 owner 和错请求元组、旧版本明确错误、离线不发送、A→B→A 晚回执、pending 广播先于 ACK、跨代拒旧 ready、下载中元数据变化后丢弃暂存文件。
- 互审发现 `pending/failed` 完整回执在 App 重启后只查内存会隐藏已有完整磁盘副本。定点红测：`.cache/android-remote-images-20261115/pending-cache-red.log`。修复为先校验本地完整副本，再返回 pending/failed 状态；缓存未命中时这两种状态仍不发送 HTTP 或 prepare。
- UI 自有 fixture 的失败、离线、单张按钮互锁、稳定图片容器和完整磁盘副本探测由独占 writer 以红→绿验证，日志在 `.cache/android-image-republication-20261115/`。

## 验证结果与边界

在 Windows、JDK 17、Android SDK 34、离线 Gradle 上重新编译当前 Java 源码，运行 22 项抽屉/图片 UI、16 项真实 Controller worker、10 项缓存/元数据策略、9 项薄适配和 6 项原有 Controller/IM 历史移交，共 **63 项全部通过**，零失败、错误或跳过。仅跳过 Native 构建，没有跳过 Java 编译。日志：`.cache/android-remote-images-20261115/publication-combined.log`。

这些用例全部使用自有图片字节和受控传输边界，未访问供应商、真实 OSS、账号服务或 IM SDK。桌面 Host 的来源准备、Auth 服务的同登录续期授权，以及 277 正常安装产物和真实图片行须由主任务分别验证。276 的真实会话读取及菜单验证不能替代新入口的真实图片验收；服务端仅通过源码测试也不等于已部署续期契约。
