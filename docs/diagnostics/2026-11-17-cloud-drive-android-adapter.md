# Android CloudDrive 20 GB / 100 MB 适配

> 历史分步适配记录。最终合源、21项JVM、双ARM产品构建和正常APK结果见
> `2026-11-17-cloud-drive-android-20gb-thumbnail.md` 及
> `2026-11-17-cloud-drive-20gb-integration.md`；本记录的早期TU/probe边界不代表最终交付状态。

> 历史适配阶段记录。最终整合、双 ABI 正常 APK 和后端上线结果见
> `2026-11-17-cloud-drive-20gb-integration.md`；下文当时待做事项不代表当前状态。

日期：2026-11-17。起始公共提交为 `8aefd68b3097fb37a495846d680d25fc45b5e886`，
本适配由 Root 整合，交付时仍需记录整合后的源码与产品构建身份。

## 操作与预期

Android 云盘普通上传与 Agent 工作区上传使用同一个 `MaiCloudDrive` 客户端。
选择原文件后应按共享媒体策略校验大小；新图片在远程初始化前生成持久 JPEG，
缩略图与原文件必须来自同一版本。暂停和失败保留已有 checkpoint 的原图快照、
JPEG 及身份文件；成功或确认放弃后仅清理本调用拥有的数据。旧 checkpoint 没有
thumbnail 对象时不在途中增加资源。云盘卡片不再常驻显示隐私和大小说明，只在
存在预留空间时保留简短标签。Agent 无法准备图片时应直接报错，不返回 accepted。

## 旧行为与定位

- 普通 Robolectric 组件同例 RED：旧卡片仍有固定提示，413 文案写死账号 10 GB，
  六用例中两失败；原有四个导航槽通过。
- 空间预算同例 RED：20,000,000,000 字节被 Java 旧视频 2 GB 常量误拦，一失败。
- Agent 同例 RED：坏 PNG 仍被 startUpload 接受并排队，而异步错误被丢弃，一失败。
- JNI 的随机读使用 `off_t/pread`。NDK 27 的 32 位默认 off_t 为 32 位，旧回调在
  offset 超过其最大值时直接失败。此问题与 Java long 或进度显示无关。
## 适配

不新增传输栈。JNI 只追加 caller-owned JPEG 路径到冻结的 C API option；
Session 调用继续使用空 option。随机读改为明确的 `off64_t/pread64`，并检查范围。

新图片通过已有 nativeStageFd 在 checkpoint 旁形成最多 100 MB 的私有快照，
缩略图和原文件上传都使用该快照。视频保留原来的 seekable FD 直读路线。
缩略图复用既有 viewer JNI 的普通 Native decoder，保留 HEIC、EXIF 旋转/镜像与
源像素预算，输出至多 320×320 JPEG。`.thumbnail.meta` 绑定来源 ID、原文件名、
字节数和完整 SHA-256，以及 JPEG 字节数和 SHA-256，不把文件路径塞入核心 checkpoint。
辅助文件借用只覆盖同步 C API 调用寿命，multipart 状态继续由共享核心管理。

Agent 在已有工具工作线程执行同一个本地准备过程，再把不可变快照和 JPEG 单次
移交普通上传队列。普通 UI 保持异步；prepared handoff 不重复暂存或解码。
所有上限提示和普通/Agent 预览检查取共享媒体信息，不保留 2 GB/20 MB/10 GB 文案。

## 验证与边界

Java/Robolectric Native Graphics 的 UI、空间预算、JPEG 生命周期、Agent 前置失败
和 prepared handoff 用例通过；精确数量与 XML、日志及源码 SHA 以缓存 handoff 为准。
无效图 fixture 的 decoder 警告属于预期失败输入，未伪装成产品运行错误。

当前 JNI 翻译单元分别以 NDK 27 / API 26 为 armeabi-v7a、arm64-v8a 编译通过。
另在全新自有 API 29 AVD（emulator-5580，x86_64/x86）安装两个无网络私有 probe：
32 位 x86 进程读同一 sparse 文件 3,000,000,000 偏移，原 JNI readAt 同例 low=true、
high=false；新回调 low=true、high=true。probe 复用实际 JNI 内部 reader，未复制替代
reader；其余未引用 JNI 由链接 GC 去除。没有写入 3 GB 实际数据或向服务上传大文件。

该 probe 也核过 JPEG 320×120、暂停保留、续传不重写和完成清理，并已亲看自己的
UI 帧。它不是产品 App 的完整云盘端到端验收，也不是 ARM 真机运行。图片 native
默认 decoder 的共享实现不在此重复验证；JVM 层通过普通 decoder 参数验证适配寿命。

完整产品 native 链接、APK 及新版云盘协议的 Android 整合验证，须在共享核心完成后
继续；不能拿此处的 TU 编译或私有 probe 代替。Root 对共享协议仅在 Windows 验证一次。
真实 Oppo 设备、原有 AVD、用户现装应用和数据均未改动；未读线上配置或部署服务。

本任务原始日志、私有 APK/NDK 对象和截图位于
`.cache/cloud-drive-20gb-android-20261117/`。
