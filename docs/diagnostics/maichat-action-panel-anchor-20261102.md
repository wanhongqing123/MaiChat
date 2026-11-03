# 创作中心更多菜单位置偏移：2026-11-02

05:21 CST 用户发送真机 158 截图：右上角更多按钮弹出的思考强度、操作权限和清空对话面板，
出现在页面中央偏下，未紧挨右上角按钮。按菜单定位问题继续处理；尚无用户补充其他指向。

基线 main HEAD `1997947219596e6520be27ed892bd6ea1820ff2e` 加前序工作区，
真机 2.2.4 (158) 安装与进程已在上一项核对，用户截图的交互来自该版。

## 复现操作与预期

1. 正常 Root 进入创作中心长会话，点右上角更多。
2. 面板应出现在标题栏下方、右边缘接近更多按钮，不能漂到中央；点外部应关闭并保持原消息位置。
3. 输入草稿打开键盘，再点更多；键盘正常收起，面板仍贴标题栏，草稿保留。
4. 菜单展开后切横屏、再回竖屏，位置继续跟随当前标题栏；扩展思考强度/操作权限选项仍可操作。
5. 视频分屏中同样按当前创作中心标题定位，不使用整个窗口的垂直中心。

## 初步证据

`AIAssistantView` 的 `.overlay(alignment: .topTrailing)` 闭包同时返回全屏 Color 和面板，
没有显式的共同布局容器。先通过正常 Root 的实际截图、位置断言复现，不将该代码结构直接
当作已证实根因。

05:25 CST 正常 Root 158 基线第一次点更多即复现：面板右边缘 **345.5 pt**，
按钮右边缘 **424.0 pt**，偏左 **78.5 pt**；位置用例在 6.543 秒失败。
已查看 `EA557C23-F958-42C4-9CA7-59A07AFA80DB.png`，面板确实居于长消息中间偏下，
与用户截图一致。证据：`/private/tmp/maichat-action-panel-before-20261102.xcresult`、
`/private/tmp/maichat-action-panel-before-shots-20261102/`。

## 修复

159 将遮罩与菜单放进同一 `ZStack(alignment: .topTrailing)`，让 overlay 接收一个明确的
布局根。菜单继续使用当前创作页面的边界与标题高度，保留原有宽度、材质、选项和外部点按关闭。
不改变消息、输入草稿、视频实例和已确认的会话侧滑样式。菜单增加稳定无障碍标识，
用于读取实际屏幕位置；基线仅加标识便复现，定位修复随后单独加入。

## 模拟器验证

159 第一轮中，视频分屏定位（10.690 秒）、权限选项（8.510 秒）、思考强度选项
（8.289 秒）、图片起拖防误打开（33.253 秒）通过。
普通 Root 的竖屏、键盘收起和横屏菜单定位断言也全部通过，但其横屏关闭步骤失败：
脚本点在整个屏幕 x=5%（约 48 pt），截图显示页面从约 62 pt 的横屏安全区边界开始，
该点在实际页面/遮罩之外，菜单没有收到关闭点击。没有将这轮含失败的整体记为通过。
`/private/tmp/maichat-action-panel-after-20261102.xcresult`。

将关闭操作改为当前面板下方 20 pt、面板水平中点，即实际页面内的可见遮罩，
保持全部位置、键盘、草稿和旋转断言不变。普通 Root 同一完整操作 **22.044 秒通过**，
竖屏→键盘→横屏→关闭→回竖屏→再次打开/关闭均正常，原草稿仍在。
`/private/tmp/maichat-action-panel-final-20261102.xcresult`。
产品代码在这次用例坐标修正时没有变化，前述四项通过结果仍适用。

已查看基线偏移截图、修复后的竖屏/键盘/横屏/回竖屏以及视频分屏截图；并查看普通 Root
录屏采样，展开与关闭时面板留在当前标题栏下方，旋转后随当前页面重新布局。

- 原始录屏：`/private/tmp/maichat-action-panel-after-20261102.mp4`、
  `/private/tmp/maichat-action-panel-final-20261102.mp4`。
- 连续采样：`/private/tmp/maichat-action-panel-final-frames-20261102.jpg`。
- 截图：`/private/tmp/maichat-action-panel-after-shots-20261102/`、
  `/private/tmp/maichat-action-panel-final-shots-20261102/`。

## 真机核对安排

真机 Debug 2.2.4 (159) 已构建，严格签名与 10 个嵌入库独立签名通过。主程序 SHA-256：
`5103edfd138d3826ac60d75966d15a2e6465708f675757bfb487c96d99b43590`。
`/private/tmp/maichat-ios159-artifact-20261102.json` 记录源码与产物哈希。

真机检查仅在显式启用 `MAICHAT_VERIFY_LIVE_DEVICE_MENU=1` 时运行，使用普通 App、
无 fixture 参数，只打开与关闭菜单、记录按钮/面板位置数字，不输入或发送消息，不修改设置。
未添加真机截图附件，并在本次 xctestrun 配置将系统与用户附件保存策略设为 `keepNever`；
该取值已核对本机 Xcode `xcodebuild.xctestrun(5)` 文档，不将用户消息保存到测试附件。

## 安装结果与验证边界

05:35 CST 真机 UI 检查在运行前检查阶段被系统阻止：`com.apple.dt.deviceprep -3`，
**Unlock iPhone to Continue**；没有启动任何测试或触摸。已发出异步解锁提示，等待后设备
仍锁屏，于 05:38 CST 停止本次等待（`TEST EXECUTE INTERRUPTED`，退出 75），
先完成用户已授权的安装。该次没有真机测试通过结果，也没有保存用户屏幕附件。
日志：`/private/tmp/maichat-action-panel-device-20261102.log`。

随后原位安装成功，设备回读确认 **2.2.4 (159)**，安装目录为
`27818C16-FCD5-40BC-A72D-E23A5297392F/MaiChat.app`。
`/private/tmp/maichat-ios159-installed-20261102.json`、
`/private/tmp/maichat-ios159-device-app-20261102.json`。安装后再次核对产品补丁哈希未变。

`--no-activate` 启动同样被锁屏拒绝：CoreDeviceError 10002 / FBSOpenApplicationErrorDomain 7
**Locked**；后续进程列表为空。`/private/tmp/maichat-ios159-launch-20261102.log`、
`/private/tmp/maichat-ios159-device-process-20261102.json`。
因此当前完成的是模拟器画面与功能回归、真机构建与安装，真机启动和菜单触控检查待解锁继续。

Android `adb devices -l` 无连接设备，未验证。本次未清理用户数据、修改真实会话/模型设置、
发送模型请求、提交/推送代码或部署服务。新增的真机检查默认跳过，仅显式启用才进入普通 App。
