# Android 统一扫码识别电脑登录二维码

## 用户操作与预期

2026-11-13，用户报告 Android 的“扫一扫”只支持好友码，没有 iOS 的扫码登录行为。
源码基线为 `901c47d6fce4cb1114bbda1ac8fac871f73a5c2b`。

1. 从消息页加号或添加好友页进入“扫一扫”，应提示可扫描好友二维码或电脑登录二维码。
2. 扫好友码仍返回原账号 ID 并进入添加好友流程，不自动发送申请。
3. 扫电脑登录码应直接读取对应请求，展示目标平台、六位核对码和剩余时间；只有点击批准才允许登录。
4. 无效、附加凭据或歧义二维码应保持扫码页并提示错误，不能派发好友或登录操作。
5. 登录页取消、关闭或完成返回原页面；选择重新扫码则恢复同一个统一扫码页。
   后台、已关闭、重复相机帧和父页恢复不得额外派发请求或抢占相机。

## 现场与定位

已连接 OPPO PKD130 / Android 15，现装普通 2.2.32(274)。亲看实际页面，底部确为
“扫描对方的好友二维码”，与用户报告相同。没有读取真实会话数据库或发起实际桌面批准。

Android 消息/添加好友入口都启动 FriendQrScanActivity，其相机回调只调用 FriendQrPolicy；
电脑登录码因此被当成错误的好友码。Android 已有 DesktopLoginApprovalActivity，
但此前只能从独立设置入口扫描，且 onCreate 不接受扫码结果 Intent。
iOS 的统一扫码页先识别好友 URI，再识别桌面登录 URI，合法请求转入已有确认页。

本轮复用既有严格 DesktopLoginQrUri、账号请求与批准/拒绝逻辑，不修改后端、权限或二维码协议。
先将原好友分流等行为提取为普通接收方法，以同一方法建立设备端失败用例，再补登录路由。

## 实现

FriendQrScanActivity 复用原好友识别，合法桌面 URI 通过内部 Intent 进入原确认 Activity；
子页面首次 onCreate 再次严格校验 URI，仅进入 LOADING 读取详情，不自动批准。
重复帧、后台和关闭后的帧受 active/finished/openingDesktop 保护，pending 另存入实例状态，
避免恢复父页时与确认页抢相机。

确认页面返回重新扫码时恢复原统一扫码页；其余关闭、批准或拒绝结果仅关闭扫码流程，
父页明确返回 RESULT_CANCELED，不把登录子页面的 RESULT_OK/extra 当作好友 ID。
统一入口来源的返回文案改为关闭/完成，原独立“我的”入口与原确认规则保持。

只改两个 Android 生产 Java 文件及相关测试；没有改 iOS、Core、后端、权限、版本或解析协议。

## 实际测试与构建

原日志与源码冻结表位于忽略目录 `.cache/android-qr-login-20261113/`。

- 首次独立 Debug 验证遇到 Meson 读取 Windows 编译器帮助的编码错误和缺少 strings 工具。
  只调整本机构建进程环境：MSVC host 环境、宿主 Python locale、NDK llvm-strings；
  仍为完整普通 Native 构建，没有改产品 CMake/功能开关。首次完整 Debug 构建 exit 0 / 508.206s。
- 曾安装独立测试包，但设备拒绝 shell runtime permission grant，仪器测试没有执行。
  用户指出多出一个同图标 App 后，立即卸载本轮新增的 isolated 及其 runner，改为电脑端测试。
  这不是功能 RED，也没有开启手机的额外调试权限或全局安全设置。
- Robolectric 使用真实页面与普通分流方法，仅在测试内替换无法在 JVM 运行的硬件解码器。
  首轮有三个权限 Intent 夹具失败；核对并取走系统权限 Intent 后，旧行为 10 例中 4 FAIL、exit 1 / 16.580s，
  分别为双类型提示、桌面路由、重复帧和后台迟到帧。
- 同一组测试换新生产后 10/10 PASS、exit 0 / 17.603s。随后补齐重新扫码、子登录结果不冒充好友、
  合法 Intent 先加载、非法 Intent 不加载等边界，最终 14/14 PASS：路由 11、好友策略 1、桌面 URI 2。
- 最终普通 Release 与定点测试一并构建，exit 0 / 37.188s。所有构建输入原 SHA 在前后核对未变。
  仪器测试代码已编译但未执行；没有把该结果或其他平台结果冒充设备完整登录验收。

## 正常包覆盖安装与实画面

最终为普通 `com.kongshang.maichat`、2.2.32(274)，无 testOnly/debuggable，
Release BuildConfig 的本地假账号开关为 false，沿用本机既有安装证书。
签名 v2/v3、zipalign、完整二十六个 ARM64 so、七个 CPU module、固定完整 GGUF 和许可门禁均通过。
签名 APK SHA256 为 `08a3e8b6d03367a5cc516c9ffd2092ca05045a6d790303cccab138984ea6f303`。

只对原包执行 `adb install -r --no-incremental`，exit 0 / 60.792s、明确 Success；
没有卸载原 App 或清数据。设备实际 base.apk SHA 与签名候选完全相同，版本读回 2.2.32(274)。
普通冷启动 MainActivity，Status ok、TotalTime 1065ms、PID 9617；
随后实际前台为 FriendQrScanActivity，同 PID 的 crash buffer 无记录。

已亲自从实际消息页加号菜单进入“扫一扫”，查看更新后的普通 App 截图，
底部明确显示“扫描好友二维码或电脑登录二维码”；登录状态保留。
实际包列表只剩原正常 MaiChat，临时 isolated/runner 均不在设备上。
本轮正常安装未记录需要代点的安装确认动作；不能据此断言改变或绕过了系统安装确认设置。

没有读取真实聊天数据库、批准任何实际电脑登录或执行模型请求。
真实摄像头扫码到实际服务端批准/电脑兑换的全链路未执行；本轮证明普通 App 入口、
严格分流/交接/生命周期回归、正常包安装和更新后的实际提示，登录授权仍复用原逻辑并要求明确点击。
