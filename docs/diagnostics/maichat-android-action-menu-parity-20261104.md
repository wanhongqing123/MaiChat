# Android 创作中心操作菜单对齐 iOS（2026-11-04）

## 复现步骤与预期

1. 在 Android 创作中心点右上角“更多”，打开原有右上角 264dp 浮层。
2. 点“思考强度”。选项应直接在该行下方展开：简单、标准、深度；当前值显示勾选。
3. 点“操作权限”。强度选项收起，权限选项在同一浮层展开：请求批准、帮我批准、完全访问。
4. 再点已展开的标题应收起；点击浮层外或返回才关闭整个浮层。
5. 点新值立即保存，面板保持打开，成功后更新副标题和勾选；点当前值不重复保存。
6. 任务执行、提交或配置保存期间不能修改；保存失败保留原值并使用原有错误反馈。
7. 退出创作中心或切换会话时不遗留菜单；操作不得发送消息、改变草稿或触发模型任务。

## 源码证据

- iOS `AIAssistantView.swift:273` 将 `AIActionPanel` 放在标题栏右下方；`:1343` 起通过单个 `expandedSection` 管理主模型、强度、权限内联展开，标题点击使用 140ms easeOut。选项 38pt，标题行 52pt，浮层宽 264pt。
- iOS 强度映射：简单=`low`，标准=`high`，深度=`max`；权限映射：请求批准=`on-request`，帮我批准=`unless-trusted`，完全访问=`never`。选择不关闭面板，同值直接返回；权限说明保留付费任务审批边界。
- iOS `AIAssistantModel.save` 先向核心配置，成功后原子保存 `settings.json`；失败保持/恢复原配置。入口在远程会话、忙碌、提交、模型来源切换期间禁用。
- Android `AIAssistantPanel.menu()` 原先先关闭右上角 PopupWindow，再由 `reasoningMenu()` / `policyMenu()` 创建居中单选 AlertDialog，增加了不一致的页面跳转和“取消”按钮。
- Android `AIAssistantController.save()` 已负责核心配置、原子保存账号工作区 `settings.json` 与失败恢复，本次复用该事务，不改变权限协议或供应商请求路径。

## 修改与验证计划

- 只改原浮层的两项内联展示；不扩大到当前 Android 不具备的本地主模型菜单。
- 菜单内容独立于原生 Agent，便于用真实 View 点击测试单项展开、当前勾选、同值不保存和忙碌禁用。
- Panel 继续调用原有 Controller 保存，在保存完成前禁止另一次设置覆盖；状态刷新只更新菜单内容，不重建创作页面或编辑器。
- 主程序构建、Robolectric RED/GREEN 与目标手机截图由 root 统一执行。本记录建立时尚未验证修改后的运行效果。

## 实现与当前证据

- 工作基线：`0437f4cf`（图片背景/气泡修复后的 main）；本次菜单交付拟为 Android 2.2.27(264)，由 root 统一构建安装。
- `AIAssistantActionMenu` 只管理原 264dp 浮层的两组稳定 View：52dp 标题行、38dp 子项、单一展开状态、140ms easeOut、矢量图标与蓝色勾选。清空仍使用原确认流程。
- `AIAssistantPanel` 移除两条旧设置 AlertDialog 路径；选项直接调用现有 `Controller.save`，保存期间保留上次提交值，完成后更新。保存互锁不因普通 `onState` 提前释放；失败沿用原错误区和 Toast。
- 浮层由 ScrollView 包裹；按可见窗口中锚点下方空间限制高度，展开时对同一 PopupWindow 调用 `update`，不通过关闭/重开模拟子菜单。
- 退后台、会话变化、远程/本机变化或登录代际变化时关闭菜单；点击和保存回调再次核对当前归属，避免旧浮层设置影响新会话的界面。
- root 已先运行 Robolectric RED：`AIAssistantActionMenuTest` 三项均在“应出现但不存在的内联选项”断言失败，测试与构造接口编译通过。日志 `.cache/android-image-borders/action-menu-red.log`。
- 实现后 GREEN、构建、实际前后截图与最终产物身份待 root 补记；当前不据此宣称真机验收完成。

## 验证边界

本机 iOS 对照基于仓库源码；没有新增 iOS 实机验收。两项设置只通过现有配置接口保存，不触发付费模型任务。

## 构建与真机验证（2026-11-05）

实现基于 `0437f4cf`。OPPO PKD130 / Android 15 / ARM64，2.2.27(264)，同包名同证书原位覆盖。产品源码冻结后全量 JVM 回归383/383，失败/错误/跳过均0，正式构建34秒退出0；本轮无原生源码变化，复用此前同一源码构建的 Agent/GLES 库。构建前后核对 Panel SHA256 `62c2370c9c1551ca5c0cb74a6810a7f7146568a9500c2c57fe0fd881d5e2d32a`，菜单组件 SHA256 `c413719aa057176941d266da245cd88ae690ea40b7fff2cad508cd0c5b60a8e3`。

- 正常 App 复现旧版：思考强度与权限分别出现居中系统单选框和“取消”，截图已留本机。
- 新版右上角浮层 `[160,193][688,509]`；展开强度后为同一窗口 `[160,193][688,753]`，只有简单/标准/深度这一组；切换权限后强度收起，权限组在同窗展开至 `[160,193][688,815]`，无居中弹窗或遮暗整页。
- 实际选择“简单”后，标题副文与蓝色勾选更新、窗口不关闭；点击当前“简单”保持窗口，关闭并重新打开仍选中“简单”。结束已恢复原“标准”。权限始终保留原“请求批准”，点击当前权限不关闭菜单。其他权限值的选择映射由组件测试验证，本轮不提升真机权限。
- 再点已展开标题会收起；点击窗口外关闭；打开菜单后 Home 退后台、再返回 App，旧菜单不残留。未输入或发送消息，草稿保持原空值。
- 横屏1604×720：菜单仍锚在右上角；高度按可用空间限制，权限三项可见，窗口内上滑可完整查看说明与清空行。系统返回关闭正常；结束恢复自动旋转。
- 开发者已查看竖/横屏截图及15秒真实菜单操作录屏关键帧，展开切组始终留在同窗，没有再进入居中系统框。
- 图片回归：同一新版上查看已有发送图片，日期跨午夜增长为“昨天23:13”后，缩略图 `[353,991][514,1351]`，时间 `[526,1282][644,1307]` 和状态仍独立可见，无浅蓝外壳；点击进入正式预览并返回原消息位置。灰边的旧/新像素回归和横屏证据见同目录图片诊断。
- APK 包检查15份 ARM64 库、6份运行库哈希、8项必需资产及模型/许可证检查通过。最终 APK SHA256 `c1e662faa62bc075bd30ad62f737e5a3d257aeea301bcd20f42751b2cfdb2bd1`；拉回设备 `base.apk` 哈希一致。证书 SHA256 `87704a9ff5f7394186ca146b6303485abdc8fc594a4f8d2da9aaddc87ea00ea5`。Agent Build ID `60169abc1ce50d7ce37c7aba0d5b874a605b89d0`，GLES `5a401abee0a45797ab85b1f81a3fa47f6d18d825`。
- 正常运行进程 PID19614，包名 `com.kongshang.maichat`，系统查询版本2.2.27/264；该进程 AndroidRuntime/libc 崩溃日志为空。测试包 `com.kongshang.maichat.test` 已移除，生产账号和数据保留。

证据位于忽略目录 `.cache/android-action-menu-parity/`：`verified-build.log`、`source-hashes.json`、`package-verification.json`、`installed-final-verification.json`、`effort-inline.png`、`effort-low-saved.png`、`effort-persisted-low.png`、`effort-restored-high.png`、`final-policy.png`、`outside-dismissed.png`、`before-background-menu.xml`、`after-background.xml`、`landscape-policy.png`、`landscape-policy-scrolled.png`、`menu-interactions.mp4`、`image-row-final-bottom.png`、`image-preview-final-settled.png`。私人对话截图不提交仓库。

边界：保存失败、后台完成与旧窗口回调的隔离经源码审查；未在真机故意破坏线上账号凭据或配置以制造失败。组件测试覆盖展开、互斥、同值不保存、当前勾选及忙碌禁用，未以这些测试替代正常 App 截图。未触发任务来制造忙碌态，未新增 iOS/模拟器实测。
