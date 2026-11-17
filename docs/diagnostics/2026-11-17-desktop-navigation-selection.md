# PC 导航空心/实心与统一蓝色

## 用户要求

2026-11-17，用户截图显示云盘已进入内容页，但侧栏云朵仍是轮廓；导航点击后颜色不统一。用户随后明确拒绝未选中灰色，最终口径为：**所有导航同一蓝色，未选中空心，选中实心**。没有继续采用最初提出的灰色方案。

没有联系 house-maichat。保留全部既有 MainWindow 个人/设置分离、图片 R3 和其他未提交工作，本次只改图标资源、导航着色及对应测试。

## 原因与修复

- `navIconResource` 没有 Cloud 分支，因此云盘落到 `makeLineIcon` 手绘轮廓回退；该回退不接收 filled 状态，选中后仍空心。新 cloud.svg/cloud-filled.svg 沿用原手绘云朵轮廓并打包到 qrc。
- `navIconColor(kind, selected)` 原按类型分蓝、紫、绿、橙等颜色。改为一个品牌蓝 `#0879c9`，两态同色，状态只由空心/实心表达；未使用灰色。
- “我的”以前明确跳过 filled 资源，补充相邻 MIT Tabler user-circle.svg 的实心对应资源并取消跳过，避免统一颜色后两态完全相同。
- 未选中悬停底统一浅蓝色，去掉各分类的紫/绿等独立悬停色。页面路由、tooltip、accessibleName、账号及业务回调不变。

## 实际复现与验证

macOS 27.2 / arm64，项目固定 Qt 6.12 源 SDK。真实 MainWindow + FakeRemoteIMClient、自有隔离配置运行，未读取实际消息或修改线上账号。

原产品实际 RED7：六个导航项逐个切换内容页，正常与选中图标像素均未满足统一蓝；实际点击云盘后，图标中心透明，仍是轮廓。

候选首轮只有创作/我的的少量边缘 RGB 差 1，来源为 SVG 抗锯齿的预乘/反预乘舍入；测试改为只严格比较完全不透明笔画，不放宽品牌色。最终实际 **13 passed / 0 failed / 0 skipped**，3282ms，增量构建+运行+普通 App 构建 23.519 秒。

检查包括六项空心/实心图像确实不同、两态笔画均为同一精确蓝色、云盘内部不透明、当前内容页与选中项同步、云盘点击路由、导航基础样式、既有我的/设置并列页。已亲看六页截图中的云盘和我的，两种选中形态正确，其他导航保持蓝色空心。

证据目录 `/private/tmp/maichat-desktop-navigation-20261117`：red-test.txt、verified-test.txt、verified-images。修改前保存 MainWindow.cpp、resources.qrc、MainWindowLayoutTest.cpp 的精确快照；新增三项 SVG。

## 产物

普通 `maichat` 目标构建成功，项目原打包脚本 app-only 打包 2.2.65，runtime audit 26 个 Mach-O 无失败，deep/strict 签名验证 0。包内 TRTC 自带 AudioPlugin.driver 只作为既有 SDK 资源被审计，本轮没有系统驱动安装或新的第三方依赖。

完整打包 App 以裸系统 PATH、隔离目录、禁止外网及真实用户配置/Keychain 文件读写策略运行 `--smoke`，退出 0，3.397 秒。传输候选 executable SHA256 `281796231604abf918c023bf7c152a70490c34b1937875d441b7eb77b7405135`。产物位于证据目录 `distribution/MaiChat-macos-arm64/MaiChat.app`。

源码增量六文件、10672 字节，SHA256 `5d4dedbf9141d5184a76450de3ae715a1f743c9bac552ff701e7468194282858`；完整 before/blob、after/blob 和 SHA 在 `desktop-navigation-final.json`。本地 HEAD `c3ea0bbbbb63f09b67329f57b31fd5e6b92c554c`，没有把其他 WIP 混入增量。

## 安装边界

当前实际 `/Applications/MaiChat.app` 为 2.2.64，进程正在运行，旧 executable SHA256 `abd17c4fceee7605719707194fee87f5934f7eb63358dd54c1c8aedf3c88b186`。未把新包验证说成现装已更新。

已向用户提供新实际截图和完成验证的具体 2.2.65 包；根据根 AGENTS.md 第18行对停止/替换运行程序且授权不清的要求，仅最终退出更新步骤待用户确认。确认前保留当前运行程序，不覆盖安装或中断上传/草稿；后续若批准，正常退出、保留旧包备份、更新并核新进程与版本。

本轮在 Mac 实际运行构建验证，未声称 Windows 现装或 Windows GUI 验收完成。
