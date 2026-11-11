# iOS 免打扰模式

2026-11-11，iOS 源码基线 `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`。完成 iOS 构建后安全快进到 `d73d8405ea4145e43b4c1020eff0ef7cf20f1417`，九项 Swift overlay 原字节未变。用户要求所有客户端增加免打扰；本记录只覆盖 iOS 及 Mac 对已冻结 Desktop 实现的必要消费。

进入“我的 → 偏好设置 → 消息通知”，默认关闭“免打扰模式”。开启后不提交新消息通知及其声音，继续接收、保存消息并更新未读/徽标；关闭只恢复之后的新消息，不补发静默历史。重启保留选择。统一说明为“开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。”

逻辑 key 为 `maichat.preferences.do-not-disturb`，默认 false，仅本设备持久，不进入账号、房间或 WebSocket JSON。iOS 只清理现有 `remote-im-message-` 本地消息通知；电话、邀请、捕获常驻通知及错误反馈不受影响。设置本身不请求通知授权，开启时停止应用的通知权限预提示。系统换机备份是否继承不作额外保证。

普通通知控制器在最终提交前复核偏好 permit 代际，串行化 add 与旧提交完成后的撤销；prefix 枚举回调后再次核对代际。旧工作不能删除关闭免打扰后提交的同 peer 或新 peer 提醒。只有普通未读状态更新能改写权威 badge，异步旧预览不能回写捕获值；content.badge 提交时读取当前权威值，add 完成后也仅恢复当前值，允许已读降至 0。

## 实际验证

平台为自有 iPhone 17 Pro Max Simulator / iOS 26.5；正常 App Debug 构建、真实设置入口，配合仅 DEBUG && Simulator 编译的受控通知传输替身。不使用真实账号或 DB，不发付费模型请求，不操作现装 iOS 205。

- 原策略保留行为、仅增加未使用的可选参数后，DND 契约原例 1 FAIL；原设置入口缺开关 1 FAIL。更早一次 UI 被权限预提示遮挡的夹具前置失败独立保全。
- 正常完整 `MasterChatStateTests` 85 PASS / 0 FAIL，命令 exit 0 / 2.091s。覆盖 DND 抑制、持久化、旧 permit 失效和实例归属，以及原消息接收、历史与徽标策略。
- 首件生产控制器实际 badge 三例 3 FAIL：开关切换后的旧 permit、同代际预览乱序、已读归零后旧预览，均能让旧 count 覆盖当前 badge。原控制器源、构建和日志已保全；旧 Debug launcher hash 单独记录，未保留旧 `MaiChat.debug.dylib` hash，不把 launcher 当作控制器机器码身份。
- 窄修后同三例 PASS；旧 add / on→off / 同 peer 新 add，旧 prefix 枚举 / off / 新 peer，旧权限查询和发送失败分类的受控交错例 PASS。该轮为 4 PASS / 1 FAIL，唯一失败是设置 UI 自动化点击了包含整行标签的 switch AX 框中心空白，原记录保留。
- 只修测试为点击截图右侧真实可见 thumb；同设置槽实际完成 0→1、普通终止重启仍 1、再关闭至 0，1 PASS / 0 FAIL / 19.814s。未强设 defaults。已亲看开启和关闭原 PNG；不把分轮结果写成单轮 5/0。
- 正常 unsigned Simulator Release 构建及链接 exit 0 / 110.910s，2.2.4/build 203（命令行覆盖）。exe SHA256 `c8e293068ad10f72fc75fbe5c17c01226cf780d52515a22c39180bdc0e4a1021`。九个 Release MachO 均不含三个 DND 测试 seam marker，实际 Debug dylib 三项阳性，正式 DND key 阳性。

Mac 仅消费精确 Desktop 六源，原 patch 19026 B / SHA256 `0aab2b153e6e930bccc435638317c32000a19537bfe0013c14f1a69934e1612a`。正常 Qt 6.12 Release cache configure 0 / 8.172s；App、policy、MainWindow 目标 build 0 / 33.198s。Cocoa policy 11/0；两个 DND 槽及 hooks 合计 4/0（960ms），覆盖自有 SQLite、未读、关闭不补发和重建窗口持久化。自有设置图已亲看。私有 profile、原 loopback-only sandbox 下正常 App --smoke PID29522，exit 0 / 3.331s，stdout 读回版本 2.2.58 / Qt 6.12.0；exe SHA256 `d014353dfd6d3c9d3a666c16fce89573e4fd21d19a37b8594be0dadbdcdc8d34`。该次无 dyld 模块行，不冒称已由日志逐模块读回路径。

## 原件及边界

原件位于执行端 `/private/tmp/maichat-dnd-ios-20261111-fkji_u_b`，plain v1/v2 patch、逐源 canonical/blob/raw manifest、各轮原日志、Release 回执和三张设置原图按长度/SHA256 单独索引。IM 收到的图若与执行端原 PNG 不同，必须分列，不据此推断转码原因。

iOS 受控测试验证正式控制器的通知提交/撤销决策，没有代替真实后台 OS 横幅、声音或真机验收。Mac 日志验证实际提交请求，未保证系统已展示横幅。已经播放的声音或已被用户看到的通知无法撤回；Windows 当前 balloon 撤销及 Android 证据由各自平台另行记录。未改服务、Native/CABI、版本、公开 tag/资产，也未安装、重装或停止用户现装程序。
