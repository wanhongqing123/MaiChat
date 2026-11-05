# Desktop 消息顶栏添加菜单整理（2026-11-05）

## 复现与预期

- 现场截图：`144115250159782623-1793870827-627348737.png`，用户明确要求移除消息列表顶栏独立的“好友申请”按钮，只保留 `+`，并修正展开菜单外观。
- 原有操作：进入消息页，点击列表顶栏 `+`。顶栏同时存在“好友申请”；展开的是未应用产品菜单样式的默认方形菜单，仅含“发起群聊”“添加朋友”。
- 预期操作：顶栏只有 `+`。点击或键盘激活后，在按钮下方展开同一套圆角菜单，包含有真实功能的“发起群聊”“添加朋友”“好友申请”；Esc 收起并保留当前会话。

## 源码与修改

- 基于 `27cf9755` 的 Windows 工作树；本次仅改动桌面共用的 `MainWindow.cpp/.h`、`MainWindowLayoutTest.cpp`。
- 移除独立申请按钮及其无效样式；审批接口与数据保持现有路径，从 `+` 菜单进入。
- 复用已有 `applyMessageContextMenuStyle` 与线形图标，统一圆角、内边距、悬浮色；按钮采用顶栏圆角图标控件的视觉，提供读屏名称与说明。
- 菜单先按样式和最小宽度计算最终宽度，再把右边缘对齐按钮；由 `QMenu::popup` 保留屏幕边缘避让。
- 收起菜单后恢复 `+` 的焦点；选择动作继续进入原有业务对话框。

## 验证与边界

- 更新 `conversationHeaderNamesActions`：无独立申请按钮，保留可访问的单一 `+` 入口。
- 新增 `conversationAddMenuKeepsActionsAndAnchorsToButton`：真实点击菜单、三个图标动作、按钮锚点、Esc 后保留会话、键盘再次打开，以及通过该菜单加载并同意一个本地 Fake 好友申请。
- 测试局部加载与正常 App 相同的 Inter/Noto Sans SC 字体，结束后恢复；不改变整个测试套件。
- 可选 `MAICHAT_CAPTURE_MESSAGE_ADD_MENU=<path.png>` 输出菜单本身以及同目录 `<path>-header.png`，只抓本地 Fake 测试的 Qt 子树，不截取其他正在运行的用户窗口。

### 本轮 Windows 实际验证

- 2026-11-05 18:02（Asia/Shanghai），由主代理在 Windows 自有 QtTest 进程中运行并截图。当前源码身份为 `HEAD=27cf975593509b41885d5bf631e628c6de1ae5e7` 加本次未提交工作树修改；这是开发构建验证，不能作为最终发布提交或发布包身份。
- 实际进程：`E:\OpenSource\MaiChat\MaiChat\desktop\.cache\build-release-62111144\bin\main_window_layout_test.exe`，QtTest/Qt 5.15.2 x64 Release，MSVC 2019，Windows 10。两次 CDB 日志的 `CommandLine` 分别指定下列槽，未复用旧测试结果。
- `conversationHeaderNamesActions`：PASS；含 init/cleanup 的 QtTest 总计 **3 passed / 0 failed / 0 skipped**，1165 ms。日志：`.cache/desktop-player-style-20261105/message-header-cdb.log`。
- `conversationAddMenuKeepsActionsAndAnchorsToButton`：PASS；含 init/cleanup 的 QtTest 总计 **3 passed / 0 failed / 0 skipped**，1252 ms。日志：`.cache/desktop-player-style-20261105/message-add-menu-cdb.log`。
- 已查看实际截图 `.cache/desktop-player-style-20261105/message-add-menu.png` 与 `message-add-menu-header.png`：消息标题旁仅有加号；菜单为白色圆角卡片，三项图标与文字排列一致，好友申请位于分隔线下。截图来自本地 Fake 账号与测试自有 Qt 子树，无其他运行中 App 的内容。
- 这两项断言验证了实际鼠标、键盘及 Fake 业务回调。生产账号审批、屏幕边缘多显示器避让、macOS 编译/实际运行仍未在本轮独立验收；最终 Windows Release 提交、全套回归及包产物另由发布门禁记录。
