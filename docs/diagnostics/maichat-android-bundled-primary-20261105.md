# Android 随包主模型与同窗来源选择（2026-11-05）

## 用户目标与执行顺序

在现有 `fcdba865` / Android2.2.28(265) 上继续补齐 iOS 截图中的「主模型」：创作中心更多菜单内展开「云端模型 / 本地模型」，点选实际生效，同值不操作，不新增管理、导入、参数设置或应用确认页。用户追加：本轮完成后构建、验证 Windows 桌面产物并发布到 `wanhongqing123/MaiChat` 的 GitHub Release。

## 交互验收

1. 在正常创作中心打开更多，第一行为主模型。点标题在同一浮层展开两项，当前来源显示勾选；再点收起，展开强度或权限时只保留一组。
2. 切换时显示「正在切换…」，菜单保留当前来源勾选且禁止重复配置；成功后更新标题、勾选。失败保留/恢复此前真实配置和持久设置，可再次选择云端，不静默回退。
3. 首次本地从 App 随包模型准备，不依赖登录模型凭据、文件导入或运行时下载。切换及恢复不得删除聊天历史或输入草稿。
4. 返回云端恢复此前云端模型；本地状态下保存强度或权限仍为本地，重启恢复来源与历史。忙碌、提交、账号代际或会话失效时不提交迟到设置。
5. 模型仅支持文本。含图片历史的本地请求保留明确原始错误及完整上下文，不能删除图片或偷偷回云端。

## 设计与任务计划

- [ ] 普通设置与事务：`AIAssistantModelSettings` 表示来源、原云端选择及选项；Controller 复用同一配置事务，实际核心配置成功后才原子保存非敏感设置，写盘失败回滚原核心参数。账号关闭/代际变化隔离旧事务。
- [ ] 随包模型：复用 iOS 固定 manifest/SOURCE/prepare 脚本及许可，正式 Qwen3.5-2B Q4_K_M，1396198496B，SHA256 `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。GGUF 不提交 Git；构建缺失或哈希错误必须失败。下载仅在开发准备阶段。
- [ ] Android helper：`MaiBundledPrimaryModel.resolve(Context)` 在 worker 流式校验并发布内容 hash 命名的沙盒文件；不覆盖可能被 mmap 的既有文件，不使用硬链接。失败不留下可见半成品，目标路径位于现有 Core appRoot 允许范围。
- [ ] UI：`AIAssistantActionMenu` 新增顶部主模型及同组互斥选择，沿用已验收材质与生命周期；Panel 通过普通 `selectModelSource` 入口调用 Controller，和既有保存互锁合并，失效窗口不操作新窗口。
- [ ] 原测试加新来源/事务/资产/菜单测试先 RED，再统一 GREEN。各负责文件冻结后运行根统一 Gradle，避免构建中改源码。
- [ ] 正式 ARM64 APK 校验模型ZIP entry、manifest/许可、运行库哈希和同证书；原位安装，拉回安装APK核哈希。正常页面两轮记忆、来源重启恢复、实际工具读自有随机文件闭环、失败恢复与取消，记录首字/总耗时及内存，不触发付费模型。
- [ ] 完成当前代码后提交推送；再执行 Windows 完整构建/部署/PE 门禁，完整 staging 与实际 ZIP 解压目录分别裸 PATH smoke，核对版本/路径/哈希后发布 MaiChat 仓库 Release。

## 核心与实机边界

当前 APK 原生库 BuildID `60169abc1ce50d7ce37c7aba0d5b874a605b89d0` 已实际含 Qwen3.5 graph、llama decode、聊天模板、工具解析及普通 MaiLlamaModelClient。通过既有通用 JNI configure 即可接入，无需另造 JNI、模型代理或另一份推理核心。

配置对齐当前 iOS 源码：`modelProvider=llama`、`wire=local`、`model=local:builtin-qwen3.5-2b-q4_k_m`、context32768、maximumTokens0（自然结束/剩余上下文）、threads2。本轮不裁 system/history、不预选或删工具、不固定问候、不擅改生成上限来掩盖模型表现。

连接手机 OPPO PKD130 / Android15 / ARM64，SoC MT6835，实测总内存5729396KB；调查时可用约1987940KB。Android 当前仅 CPU，不套用 iPhone Metal 时延。真实回复/工具闭环必须单列证据，`local_model_probe` 只生成工具调用不能冒充实际执行。

## 当前阶段证据

模型设置4项 RED，10秒退出1；配置事务3项 RED，11秒退出1。日志位于 `.cache/android-action-menu-visual/primary-settings-red.log` 与 `primary-store-red.log`。来源恢复、未知provider、无云Key、原子写盘/回滚、过期账号等断言先于正式实现建立。

## 实现阶段与正常 APK（验收仍在进行）

已接入普通Controller与菜单；所有来源/选项保存复用一份配置事务。AtomicFile最终rename仅记录错误、不抛异常的问题通过实际非空目录rename失败用例先RED，随后显式fd.sync及实际base字节回读校验处理。配置过程中普通API与键盘发送均拒绝接收新轮次，保留草稿；本地新图片/视频在创建会话或发送前拒绝，原history不裁剪。

固定GGUF与上游license/card完整核验通过，正常Debug/Release都打包，不采用缺模型变体。共用Python prepare脚本修正Windows打开临时文件时verify/replace/unlink的WinError32，原4项脚本测试在Windows先2 errors，再4/4通过。期间有整GET与分段GET两份传输证据；最终本地资源为1396198496B、SHA57a108…一致，不把临时文件显示0B当作网络未传输的结论。

最终JVM405/405，失败/错误/跳过0；Gradle正式构建1分4秒退出0，正常测试APK也已构建。2.2.29(266)同包同证书原位安装成功，最终APK1524847969B、SHA256 `994064c08d4db944a45da8b4ce4b9463b4093eb9a6917f553c233ae3d067cffd`。ZIP内GGUF为未压缩entry，完整size/SHA与固定源相同，manifest/source/license/card逐字哈希相同；15份ARM64库、6份运行库哈希及既有8份模型/许可资产门禁通过。APK检查与测试不是推理验收。

普通App已确认主模型第一行、同窗两项选择。初次云端→本地约11.11秒后完成（UI轮询间隔约3.8秒，为上界），准备期间显示正在切换、保留旧来源勾选并互锁；成功后本地勾选更新、面板保持。原账号和旧会话保留；新建独立 `QA_PRIMARY_2350c1ad76f2` 会话进行合成记忆/文件测试，不发送到外部IM，不触发付费模型任务。

2026-11-05 01:25:46 UTC普通页面提交首轮本地记忆请求。至01:30 UTC仍显示真实busy/正在思考，尚无正文；进程15206为2.2.29，PSS约1.6–1.9GB，mai-turn与其计算线程合计约199% CPU。debuggerd明确返回root required，未提权。当前无实时prefill/decode日志，不能仅据CPU占用断言具体阶段或宣称两轮/工具验收已通过。Android验收继续；Windows只并行准备构建，尚未发布。

本机证据：`.cache/android-primary-install/verified-build.log`、`first-local-switch.json`、`first-local-switch.mp4`、`local-preparing.png`、`local-selected.png`；`.cache/android-primary-model/verify-apk-model.py`及模型准备日志；`.cache/android-primary-validation/run-2350c1ad76f2/observations.ndjson`。私人对话截图、QA素材与模型不提交。
