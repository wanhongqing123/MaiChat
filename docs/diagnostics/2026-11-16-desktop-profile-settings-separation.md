# Desktop “我的”与“设置”分离

2026-11-16，macOS 27.2 / arm64，Qt 6.12.0。用户截图显示侧栏“我的”实际进入“设置”，头像/账号/昵称/密码/退出与免打扰/字体/字号/远程桌面混在同页。用户进一步明确“在我的下面加个设置按钮图标”。

## 最终操作

侧栏“我的”与顶部头像进入个人页：头像、MaiChat ID、昵称、密码和登录管理。侧栏在“我的”下方紧接新增齿轮“设置”，进入独立应用偏好页，只含本机免打扰、字体、字号和远程桌面。两页并列导航，各自有独立选中状态；没有个人页右上设置入口或子页返回按钮。齿轮复用已打包 Tabler settings.svg/settings-filled.svg，tooltip 与 accessibleName 为“设置”。

保留原账号/偏好控件实例、信号连接与持久化路径；切页不重建账号、不改变远程桌面权限默认值。Windows 两页标题固定在滚动区外，缩放同步两个 header。范围仅 MainWindow.cpp/h 和原 MainWindowLayoutTest.cpp。

开发前保存 Main 图片 R3 preimage 2fc63ca22c73d5d5f15cc91356d75479ab311753、header f5bb7b8a10aebf7365fb76bd9d283c2b4cb66251、原 LayoutTest c646dadc9f481f17475b0c12bdaceeaca7ee846a。parent HEAD c3ea0bbb，保留其余 WIP；没有声称本机已消费后继 PC 输入提交。

## 验证

真实 MainWindow + 自有 FakeRemoteIMClient 样本：原混合页缺独立个人页 RED1；新增侧栏图标用例在旧右上入口版同样 RED1，最终侧栏版同例 GREEN。14个相关 selector 实际总计16 passed/0 failed/0 skipped（含init/cleanup，3480ms；构建加运行34.389s）。覆盖：点击我的/设置、齿轮在我的下方且同尺寸非空图标、独立选中状态、消息页切换后回个人、账号/偏好控件分离、头像/昵称原操作、退出确认、边框像素、免打扰持久化及消息/未读保留、原远程桌面设置、原导航图标/选中/缩放回归。

两张包含完整侧栏的实际 Cocoa 图已亲看并发用户。样本没有账号修改服务，相关修改按钮禁用属于样本状态，不是对用户账号的操作。

最终 normal maichat App 目标 build0（7.427s）；普通 --smoke 使用隔离数据和禁止外网/用户路径读写策略，退出0。这里只核正常构建启动，不冒称完整分发包runtime或用户真实会话验收。没有停止或替换/Applications/MaiChat.app，没有部署服务、安装驱动或申请权限。

最终唯一完整包25130B，SHA256 88d2f96c40dd282fadb0d5899f099e7fed4a98c93786ee3e438eb8288ef09b52，相对上述3preimage应用一次；旧20261B右上入口包已作废且Root未apply。私有replay check/apply均0，三postimage与当前源逐等。Windows实际布局/标题槽交house-maichat独立核，本机不冒称Windows已验证。

证据目录：/private/tmp/maichat-desktop-profile-settings-20261116。


## 用户要求少字后的最终皮肤

最终以46759B / SHA256 34d75acd25d9c697716da810d522ebd7ee71e7f4c554a02607372a4568b62ab9 为唯一完整增量，仍相对最初3preimage。20261B和25130B均未由Root应用，全部作废。保My下齿轮并列导航。

个人页只保留一份头像/昵称/公开账号和消息连接状态，昵称编辑移到资料卡；移除重复“当前账号/昵称”行，账号安全只保密码与当前登录。删除“安全表单”等实现术语和重复说明；退出按钮精简为“退出登录”，原确认与回调不变。

页面限宽840逻辑像素，浅灰背景/白卡/圆角，统一动作按钮样式。设置分通用和远程桌面两卡。DND/字体/字号不占用解释段落，模式短标签横排，详细含义移tooltip及accessibleDescription，密码/控制/代理原表单告知保留，未改默认权限或接口。账号长文字可换行、可复制。

普通Cocoa同14槽最终16/0/0（3279ms；增量构建加运行18.614s），含最小窗口下布局/关键控件边界验证。新短退出文本更新测试预期；圆角边框采样按DPR转换为物理像素，仍逐点断言直边完整。快捷键槽增加真实active window前置后，原110/100%和几何断言均通过；此前焦点失败日志保留，没有修改快捷键生产代码或放宽断言。

最终normal maichat build0（6.957s），隔离普通smoke0（3.749s），主程序SHA256 7badca61ccbb32c9e2e56142988aa04ddf40c68c5f778c5de93164cd146f5d7d。最终两图已亲看并发用户。私有replay check/apply0且3newblob逐等。用户桌面现装仍未替换，Windows实际槽由Root独立安排。
