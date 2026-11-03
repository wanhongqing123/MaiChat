# Android 对齐现有 iOS 页面与输入交互

## 现场和范围

- 时间：2026-11-02 晚开始，验证延续至 2026-11-03，Asia/Shanghai。
- 基线：`1997947219596e6520be27ed892bd6ea1820ff2e`，本轮直接在 main 开发。
- 实机：OPPO PKD130，Android 15，ARM64，720 × 1604、320dpi。
- 对照：用户提供的 iOS 云盘、“我”、创作中心、聊天输入条截图及仓库现有 iOS 源码。Windows 没有可用 iOS 模拟器，本轮未重新验收 iOS。
- 原 Android：2.2.6(238)，用户已登录。新版本使用相同包名 `com.kongshang.maichat` 和上次本机安装证书，执行 `adb install -r --no-incremental`，不卸载、不清数据。

## 可复现操作与预期

1. 消息首页、“我”、云盘、创作中心来回切换：五个纯图标底栏，选中轮廓和颜色明确，标题居中。
2. “我”显示头像、昵称、完整账号和连接状态卡；进入资料/设置后返回“我”，异步资料更新保留当前页面。
3. 云盘显示容量卡及目录/媒体行；加号提供原有操作，异常可重试，切 Tab 返回保留当前目录。
4. 创作中心空会话显示两列 19 项功能入口，最后一项可滚到；点击仅填草稿。
5. IM/创作中心空框提示“可按住转文字”，长按进入录音；与 iOS 一样，在屏幕中央显示转写卡、下方提供取消和编辑两个浮区。滑入取消后松手不发送，滑入编辑后松手等待最终文字并回填草稿；短按可输入，点消息空白收键盘，已有草稿保留。

## 实现和审查

- 导航使用 iOS 同源 SVG 转换的 Android 矢量轮廓及实心选中态，统一 24dp 视口，保留无障碍入口名称。
- 个人页面拆为首页/资料/设置；本人 SDK 资料与好友资料分离，异步回调校验账号代际，不把本人加进好友或会话。
- 消息首页仅在内容改变时重建行，指纹包含头像 URL、连接和在线状态。
- 云盘使用 Activity 持有的账号目录状态、独立目录请求客户端和请求代际，保留错误/空态/返回。缩略图文件按实例及请求代际隔离，旧回调不删除新封面。根目录 404 保留独立重试。
- 两个可见空编辑器接入共同长按手势，离窗取消；实时 ASR 使用账号临时 ticket，客户端直连供应商。转写失败不隐式变成发送语音。
- 延迟 ASR final 受编辑/提交代际保护，不能覆盖新草稿或停止新任务；IM 发送失败恢复转写时追加到后续草稿。
- 独立审查发现并修复：消息头像状态刷新遗漏、云盘异常缺重试、离窗缺取消、延迟 final 覆盖新输入。

## 已运行的构建

使用 JDK 17、SDK 34、NDK 27.3.13750724、Gradle 7.5.1。Java/资源打包复用本次新仓库已成功构建的原生结果；没有修改 C++/JNI。

```powershell
./gradlew.bat -I E:/OpenSource/MaiChat/.cache/android-install-19979472/local-build.init.gradle `
  -Pandroid.injected.testOnly=false :app:testDebugUnitTest `
  :app:compileDebugAndroidTestJavaWithJavac :app:assembleRelease `
  -x :app:externalNativeBuildRelease --no-daemon --console=plain --max-workers=2
```

本地安装包仅针对这台 ARM64 手机，检查 15 个原生库、6 个关键库与打包前哈希一致，PDF 许可和 RVM 模型资源齐全，`testOnly` 未启用。原生依赖完整构建的证据见此前安装诊断，不把这次跳过原生任务解释为新的原生构建验证。

## 真机验证进度

第一次 2.2.7(239) 原位升级返回 `Success`，冷启动正常，当前账号及原聊天保留。已亲看消息、“我”、个人资料、设置、云盘实际画面，并验证资料/设置返回。

### 普通引擎修复与首次实机通过

运行中看到语音进入“正在听”后出现“语音转文字失败”。增加只含固定阶段/数字/异常类型的日志后，PID 32490 实机顺序为 `asr-recorder-started → asr-ticket-ready → asr-service-failed code=4004`。因此不是未授权或没有领取凭据，而是 Android 仍保留此前大模型 2.0 引擎。10/30 同一问题及用户选择普通引擎的历史记录见 `maichat-asr-v2-sts-20261030.md`。

正式修复同步为 iOS 当前采用的 `16k_zh`，删除 `result_mod`，按普通协议 `result.index/slice_type/voice_text_str` 聚合分段。只有最终完成才交付文字，无正文的 final 保留已收到文字，不保留另一套大模型解析路径。接口依据为[腾讯普通实时语音 WebSocket 文档](https://cloud.tencent.com/document/product/1093/48982)。

- 修复前定向测试 8 个断言失败，修复后该组共 26 例通过。
- 全部 JVM 单测 207/207 通过，Android instrumentation 源码编译通过；fixture 未在现装 Release 上执行。
- 2.2.7(239) 修复包再次原位安装，PID 2540 上观察到 `asr-ready → asr-audio-sent samples=1024 → asr-partial → asr-recording-cancelled`，存在多次非空实时文字。主执行者已亲看聊天、创作中心的录音及取消截图，不在文档复述实际拾音内容。
- 使用固定合成测试语句辅助麦克风检查；实际识别同时可能拾取现场声音。测试操作以取消结束，没有由验收脚本提交真实 IM 或模型消息。
- 消息页点空白后 `mInputShown=false`，自己输入的临时 ASCII 草稿仍在，随后仅移除该临时草稿。
- 创作中心新建一个未发送消息的空会话，亲看两列图标卡和滚动后第 19 项“跨设备会话”，已有会话保留。

### 用户追加指出的居中浮层差异

00:10 左右用户指出转写反馈仍是底部条，缺少 iOS 的居中卡和取消/编辑浮区。该反馈成立，随后补齐共享浮层、窗口坐标命中、等待 final 和编辑不发送逻辑。

### 最终构建和居中浮层实测

- 350ms 长按启动，取消原生后续长按任务；浮层不建 Dialog、不夺焦点。共享组件保留末180 codepoint，按真实 `StaticLayout` 显示末八行。
- 最终完整命令构建通过；JVM 单测 **223/223**，失败/错误/跳过均为0，instrumentation 源码编译通过。
- **PID 5787** 的聊天和创作中心均实测居中蓝卡、两个圆形操作区；拖到左叉显示红色和“松手取消”，松手后浮层消失，未发送。
- 创作中心拖到右铅笔，显示蓝色高亮及“正在准备编辑…”，`asr-end-sent → asr-server-final chars=46` 后回填46字，`focused=true`、`mInputShown=true`，空会话没有消息。主执行者亲看 `final-ai-finishing.png`、`final-ai-edited.png`；随后仅移除本轮新增草稿。
- 主执行者另验欢迎卡仅填“请帮我解答：”，点空白收键盘且保留草稿，随后仅清除该次草稿；云盘加号菜单显示上传、新建文件夹和刷新，未执行写操作。

| 核对项 | 最终证据 |
|---|---|
| 版本/包 | `2.2.7 (239)` / `com.kongshang.maichat`，普通 Release 安装包、`testOnly=false` |
| APK 字节数 | 100163228 |
| APK SHA256 | `e2c5a1482ac8560008c3b5ab24628abbe17ead5adf28888ed1a17624d2bb263d` |
| 手机回拉 | 从实际 `pm path` 返回的 `base.apk` 回拉，SHA256 完全一致 |
| Android 源码差异 SHA256 | `93496432292b1f5bd4fe5867f82a713575acd3c6770f90cc55f8967a0091a7e6`，相对上述基线且包含新文件，本机 `android-source.patch` |
| 运行 | 冷启动成功，安装路径/版本已回读，PID5787 crash buffer 无记录 |

验证边界：没有用两个测试账号实际发送最终文字/语音、触发模型或付费生成。云盘真机账号为空，只验空态/菜单，目录状态有单元测试但真实目录/媒体行未验。视频浮窗上的遮盖、横屏、iOS 和 Android 模拟器未在本轮复测；没有制作正式发布包或上传 Release，不能将现场修复检查等同于整项产品上线验收。

本机截图/XML/录屏在 `.cache/android-ui-parity/` 与 `artifacts/composer/`，包含账号或聊天画面，均仅保留本地，不提交仓库。没有发布 APK、运行安装器、上传 Release、发送真实测试消息或发起付费模型任务。
