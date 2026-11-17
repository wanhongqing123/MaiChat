# iOS 底栏颜色与连接提示位置

2026-11-16，iPhone 14 Pro Max，正常 App `com.kongshang.maichat`，版本 2.2.4。用户先明确只改底部“我的”选中颜色，后要求整排选中按钮同色，再要求把头像处的消息连接提示放到“账号与安全”。

## 实现

- `ChatView.swift` 的 navProfile 复用品牌 blue；随后 `RootView.swift` 的 CompactTabBar 五个 accent 全部使用该 blue。只改变选中图标色，未选中样式和红色未读角标继续原规则。
- `SettingsView.swift` 移除头像资料卡第三行 SelfConnectionStatus；在 AccountSettingsView 的“账号”分组增加同一实时状态行，左侧“消息连接”，右侧状态点及 enum.rawValue。仍由同一个 RemoteIMAppState 驱动，没有改变连接、账号或登录逻辑。

## 已执行

213 正常 Release device build/签名校验均 0；用户明确要求先装 iOS 后，正常保数据覆盖安装 0（61.485 s），设备元数据 2.2.4 (213)，无测试参数启动 PID 66929。用户随后报告测试完成。

214 正常 Release device build 0（128.235 s），签名校验 0；底栏统一蓝色后保数据覆盖 0（12.768 s），fresh 元数据 2.2.4 (214)，正常启动 PID 66971。没有卸载、清用户容器或注入测试账号。

签名候选主程序 SHA256：213 `dc463271fa6f6d7709b426e24876d41a9ebe0598601713b1aa77357b74aeb6cd`；214 `9ce4c52bcdb356e80b968ce58c6d1c122e7e217a21c2bde655bdd7efb749225c`。这里是传给安装器的本地产物 SHA，没有将它称为独立读回的设备二进制 SHA。

215 的消息提示搬移已在正常 Simulator App 的独立 bundle `com.maichat.review.nav.MaiChat` 自验：实际“我的”截图没有状态行，实际“账号与安全”截图显示消息连接行，两个画面均已亲看。复用现有 MySettingsUITests 设置页驱动，补账号页导航截图；1 test / 0 failures（49.777 s）。首次旧驱动被新测试容器的通知前置提示挡住，保留失败记录，修正的是测试启动参数中该提示的预置条件，没有放宽原设置/安全清理断言。

早期独立 Simulator 构建禁止签名时，产物启动出现 Code Signature Invalid；对私有副本逐 framework 重签后可运行。215 使用正常 Xcode Simulator ad-hoc 签名流程，正常 App/现有 UI driver 全部运行通过。没有改动真实手机的权限来绕过此问题。

## 记录与边界

完整候选、安装元数据、正常启动回执和 UI 附件位于 `/private/tmp/maichat-ios-profile-tab-blue-20261116-u7rd400j`。215 的设备安装/版本/进程最终回执另存该目录。

开发期间本机 parent HEAD 仍为 c3ea0bbb，保留其它 Android、Swift remote-page、播放器及诊断 WIP；相关独立代码增量交 house-maichat 合源。本记录不将 Windows/Mac 用户现装程序或 Sync 服务声明为已经更新。


## 215 实机与公开账号文案补充

215 正常保数据安装 0（13.522 s），fresh 2.2.4 (215)，正常无测试参数启动 PID 67054。源码核对：正常登录在 applyUnifiedAccountSession 中直接将 session.user.id 保存为 maiChatAccountID；masterUserID 来自 session.user.imUserId，属于 IM 路由标识。头像旧显示分支会在公开 ID 为空时借用 masterUserID。本次去掉该显示兜底，明确增加“MaiChat 账号”标签；资料页同一字段也统一名称。没有增加另一套登录、账号同步或凭据读取。

216 正常 device build 0（121.049 s），Simulator build 0（29.523 s）；已看正常 Simulator “我的”显式账号标签截图。该模拟器夹具没有公开账号 ID，因此空值画面只用于标签/布局核对，不当作用户账号缺失证据。216 未安装到手机，后续合消息行右滑修复为 217，避免重复覆盖。

2026-11-16T16:06:30+08:00 用户再次明确“安装到真机”。fresh connection 从 unavailable 恢复为 paired/disconnected 后，使用原完整正常217候选再次保数据安装成功 0（12.787 s）；正常无测试参数启动成功 0，PID 67368；fresh apps metadata 核 2.2.4 (217)。候选主程序 SHA256 449f6224240734de439cf77e647733344308dce3fb7728927e3b8ae0e3827371 与安装前一致；没有把本地候选 SHA 说成独立读回设备 SHA，没有改配对/TCC/账号容器。三条完整触摸 UI 回归为 Simulator 3/0，手机本次核安装/版本/实际进程，不冒称已在用户会话完成全部手势验收。
