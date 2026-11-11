# 各客户端免打扰验证

2026-11-11；基线 `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`。
用户确认所有客户端，各设备独立持久开关，默认关闭。现有设置/偏好入口新增
“免打扰模式”，说明为“开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。”
逻辑 key `maichat.preferences.do-not-disturb` 不进入账号、房间或 WS 同步数据。

开启只抑制原普通新消息系统提醒及附带声音/振动；不拦截接收和数据库写入，保留未读、
徽标、连接、电话/邀请、采集常驻通知和操作错误反馈。关闭不重放静默历史。异步平台
以到达时状态、代际和最终提交时状态共同拒绝旧回调；只取消可识别的消息提醒。

## 已有证据与当前边界

- Windows Desktop：正常原 host 同自有 Fake IM / SQLite 用例 RED7/2→GREEN9/0；普通
  policy11/0，公共当前 balloon 撤销3/0。Root 已亲看设置页原 PNG。消息保存、未读与
  tray 均保留。完整证据见 `maichat-desktop-notification-dnd-20261111.md` 及
  `.cache/notification-dnd-20261111/desktop/final-desktop-proof.json`。
- Desktop 六源 plain delta 已冻结供 macOS 正常消费者，19026B，SHA256
  `0aab2b153e6e930bccc435638317c32000a19537bfe0013c14f1a69934e1612a`。
  macOS 结果尚待回件；不保证清除 Windows 通知中心历史。
- Android：生产偏好/入站 queue epoch/最终提交/按消息 channel 取消已实现，独立只读
  review 未发现生产路径明确阻断。正常 policy7/0；App 与 instrumentation Java 编译
  通过。review 发现 legacy 自有夹具 listener=null、receive 后立即断言的异步竞态，
  已只修测试：自有非空 listener/main Handler、实际回调 latch、error 令断言失败、关闭
  自有 IO 并等待退出。最终正常 instrumentation Java 真正重编 PID46180 exit0、7.440s；
  文件 blob `f2439767cae46c764a4e813090983d4a8053ceee`，其他四源 raw SHA 不变。
  证据 `.cache/notification-dnd-20261111/android/fixture-review/android-dnd-fixture-proof.json`。
  未运行 Android UI 或安装到用户设备；此 legacy component 只覆盖内存接收/未读，
  不宣称生产 DB 落库或 MainActivity 通知提交验收。
- iOS：由 mac-maichat 独占实现；普通 policy 已得真实 DND RED。UI 首轮被既有权限
  预提示挡住，按现有测试入口隔离前置后继续；不是免打扰功能 RED。尚无最终 patch、
  UI GREEN 或 Release 回件。本轮不操作真机205或真实数据库。

现有 v2.2.58 / iOS205 与公开资产保持原样。各端只使用自有消息、profile、Fake SDK
或专用 Simulator；不将前台 guard 冒称已验证所有后台系统 push。
