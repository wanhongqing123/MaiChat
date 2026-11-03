# iOS 分屏播放器倍速标签遮挡：2026-11-01

- 开始时间：12:07 CST；源码 main，HEAD `1997947219596e6520be27ed892bd6ea1820ff2e`
  加前序媒体修复工作区。前序 151 真机包尚未因设备不可用而完成安装。
- 用户现场：创作中心上半屏播放视频，播放结束后展开倍速选项；
  `0.25× / 0.5× / 1.0× / 1.5× / 2.0×` 标签下部被截住。
- 复现：正常 Root 分屏播放器，播放中及结束后点击倍速，检查五个选项及文字；
  再检查全屏、横屏、选择倍速后返回分屏。
- 预期：整行菜单和每个标签完整可见、可点击，不与时间/进度条或播放器边界重叠。
  改变布局不重播视频、不改变用户选定的时间、暂停状态或倍速。

## 现场证据与定位

- 记录了正常 Root 分屏播放器：菜单 48 pt 高，选项 36 pt 高，标题 17 pt 高，
  文字几何边界没有超出按钮。原自动化边界断言通过，但实际截图菜单下半部分
  与黑边融为一体；不能把这些断言通过当作视觉问题已解决。
  `/private/tmp/maichat-speed-menu-before-20261101.xcresult`、
  `/private/tmp/maichat-speed-menu-before-layout-20261101.log`。
- 改用与现场相同的 16:9 SDR 和 HLG 片段复核，确认菜单跨在画面和黑边交界。
  原始分辨率局部图及像素检查进一步确认文字像素仍在，主要是半透明黑底进入
  黑边后没有可辨认的下边界，且文字过于贴近画面分界线。
  `/private/tmp/maichat-speed-menu-wide-baseline-20261101.xcresult`。
- 临时尝试文字栅格化和单独控件层均未作为最终方案保留；没有修改 Metal/HDR
  渲染、视频解码或播放状态。

## 修复

- 倍速菜单使用不透明深灰底和细边框，背景跨过画面/黑边时仍完整可见。
- 面板从 48 pt 增至 60 pt，保留上下 6 pt 间距，每个选项为 48 pt 高；
  文字相应上移并增加留白。左右以播放器安全区对齐，内部不重复叠加安全区间距。
- Xcode 工程与 project.yml 同步为 2.2.4 (152)。保留前序连续滑动、精准 seek、
  HDR 和结束状态修复，不修改用户媒体。

## 验证

- 16:9 SDR/HLG 分屏结束状态 **2/2 通过，22.905 秒**，已人工查看两张实际截图，
  面板下缘、圆角和五个标签均清晰可见。
  `/private/tmp/maichat-speed-menu-panel-20261101.xcresult`、
  `/private/tmp/maichat-speed-menu-panel-attachments-20261101/`。
- 增加实际截图像素检查：菜单下缘与播放器黑边应有可见对比，而非仅验证文字坐标。
  已回归 SDR/HLG 的竖屏分屏、竖屏全屏、横屏全屏、横屏分屏及选择倍速。
- 免费测试片段为 960×540、20 秒，SDR H.264 和标记 BT.2020/HLG 的 10-bit HEVC；
  测试运行环境提供 Base64，模拟器测试宿主写入当前 App 临时目录。首次采用固定
  容器路径的尝试因 Xcode 重装换容器而失效，已改为可移植输入，不作为产品失败。

## 最终验收与交付边界

- HLG 的四种布局通过，29.035 秒；正常 Root 分屏/全屏、图片视频往返、导航恢复
  通过，19.336 秒。`/private/tmp/maichat-speed-menu-check-20261101.xcresult`。
  该轮 SDR 在旋转动画未稳定时的点击打开了会话侧栏，未将该轮整体计为通过。
- 等待旋转后按钮位置稳定再点击，同一 SDR 四种布局 **1/1 通过，39.986 秒**，
  包含独立宿主截图的 8 秒诊断窗口。
  `/private/tmp/maichat-speed-menu-sdr-check-20261101.xcresult`。
  常规测试默认不启用此停留，仅 `MAICHAT_SPEED_MENU_CAPTURE_HOLD=1` 时启用。
- 两种编码、四种布局共 8 组显示与点击检查已分别通过。每组检查五个选项的可点击性、
  不小于 44 pt 的触摸区域、完整位于播放器中，并实际选择不同倍速；竖屏同时检查
  截图中面板下缘的对比。已有图片/视频连续浏览和 Root 暂停位置回归通过。
- 已人工查看正常 Root 的竖屏分屏、竖屏全屏、横屏全屏、横屏分屏截图，以及宿主
  `simctl` 独立截取的实际横屏画面，完整菜单、边框、五个标签均可辨认。
  `/private/tmp/maichat-speed-menu-check-attachments-20261101/`、
  `/private/tmp/maichat-speed-menu-sdr-check-attachments-20261101/`、
  `/private/tmp/maichat-speed-menu-landscape-screen-20261101.png`。
- 横屏验证初次像素断言使用了带方向标记的 App 截图，产生了错误取样；改用整屏
  截图并独立核对。像素断言限于用户现场的竖屏，横屏按实际画面、几何和点击验收。
- 一并移除 Root 对全屏视频重复施加的触摸遮蔽，由原生分页宿主统一管理模态状态，
  关闭/返回分屏后输入栏与五个导航按钮恢复可操作。

真机 arm64 Debug **2.2.4 (152)** 构建成功，App 深层严格签名和 10 个嵌入库独立签名
通过。主程序 SHA-256：`8728770a56bc192ef9c008cdd1e865008cda311a7d4eb048fd9f023396872fdb`；
清单 `/private/tmp/maichat-ios152-artifact-20261101.json`，同时记录源码 HEAD、补丁和
新增分页文件的哈希。

首轮按既有授权尝试安装，但设备服务报告 iPhone 不可用（CoreDeviceError 4016），
`/private/tmp/maichat-ios152-install-20261101.log`。**首轮未安装成功，真机操作未验收**；
后续安装结果见下方重试记录。
没有清理用户 App 数据、Keychain 或媒体文件；本轮没有提交、推送或部署服务。

## 真机安装重试：2026-11-01 19:50 CST

- 用户再次要求安装到真机；此时 iPhone 14 Pro Max 已显示 available (paired)。
- 操作：复核既有 152 包的版本、主程序哈希、深层签名与源码补丁哈希，原位安装，
  正常启动，再查询设备上的 App 版本与运行进程。
- 预期：设备安装 2.2.4 (152)，运行进程来自同一安装目录；保留原 App 数据。
- 以上产物复核全部通过。安装与启动结果另记于下方，安装成功本身不等于媒体交互实机验收。

19:52 CST 核验结果：

- 原位安装成功，设备 App 清单确认 `com.kongshang.maichat` 为 **2.2.4 (152)**。
  安装回执 `/private/tmp/maichat-ios152-installed-retry-20261101.json`，
  设备版本 `/private/tmp/maichat-ios152-device-app-20261101.json`。
- 正常启动成功，未传入 UI 测试参数或测试素材。
  `/private/tmp/maichat-ios152-launch-20261101.json`。
- 连续两次查询确认运行进程 PID **15817**，其可执行文件位于刚安装的同一目录
  `0BD0CC01-19CE-4825-8A81-B9A10FE7DDFC/MaiChat.app/MaiChat`。
  `/private/tmp/maichat-ios152-device-process-20261101.json`、
  `/private/tmp/maichat-ios152-device-process-confirmed-20261101.json`。
- 本次仅完成真机安装、正常启动和版本/进程核对，未操作用户媒体进行倍速菜单实机验收。
  没有卸载 App 或清理数据、Keychain、媒体；没有修改产品代码。
