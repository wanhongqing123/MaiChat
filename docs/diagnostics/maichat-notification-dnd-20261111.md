# 各客户端免打扰验证

2026-11-11；基线 `7eb79ce80243cbe32ee6d2e56a4391fab66fdde3`。
用户确认所有客户端，各设备独立持久开关，默认关闭。现有设置/偏好入口新增
“免打扰模式”，说明为“开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。”
逻辑 key `maichat.preferences.do-not-disturb` 不进入账号、房间或 WS 同步数据。

开启只抑制原普通新消息系统提醒及附带声音/振动；不拦截接收和数据库写入，保留未读、
徽标、连接、电话/邀请、采集常驻通知和操作错误反馈。关闭不重放静默历史。异步平台
以到达时状态、代际和最终提交时状态共同拒绝旧回调；只取消可识别的消息提醒。

## 分平台证据与边界

- Windows Desktop：正常原 host 同自有 Fake IM / SQLite 用例 RED7/2→GREEN9/0；普通
  policy11/0，公共当前 balloon 撤销3/0。Root 已亲看设置页原 PNG。消息保存、未读与
  tray 均保留。完整证据见 `maichat-desktop-notification-dnd-20261111.md` 及
  `.cache/notification-dnd-20261111/desktop/final-desktop-proof.json`。
- Desktop 六源 plain delta19026B，SHA256
  `0aab2b153e6e930bccc435638317c32000a19537bfe0013c14f1a69934e1612a`。
  macOS正常configure/build均0，Cocoa policy11/0、DND两槽及hooks4/0，普通私有profile
  `--smoke` PID29522 exit0，stdout读回2.2.58/Qt6.12；exe SHA256
  `d014353dfd6d3c9d3a666c16fce89573e4fd21d19a37b8594be0dadbdcdc8d34`。
  Root核5份原log长度/SHA及测试结果，已亲看收到的设置PNG。未保证OS实际展示横幅或
  清除Windows通知中心历史；本次Mac smoke无dyld模块行，不声称逐模块路径已读回。
- Android：生产偏好/入站 queue epoch/最终提交/按消息 channel 取消已实现，独立只读
  review 未发现生产路径明确阻断。正常 policy7/0；App 与 instrumentation Java 编译
  通过。review 发现 legacy 自有夹具 listener=null、receive 后立即断言的异步竞态，
  已只修测试：自有非空 listener/main Handler、实际回调 latch、error 令断言失败、关闭
  自有 IO 并等待退出。最终正常 instrumentation Java 真正重编 PID46180 exit0、7.440s；
  文件 blob `f2439767cae46c764a4e813090983d4a8053ceee`，其他四源 raw SHA 不变。
  证据 `.cache/notification-dnd-20261111/android/fixture-review/android-dnd-fixture-proof.json`。
  未运行 Android UI 或安装到用户设备；此 legacy component 只覆盖内存接收/未读，
  不宣称生产 DB 落库或 MainActivity 通知提交验收。
- iOS：由 mac-maichat 独占实现；普通 policy 已得真实 DND RED，隔离权限预提示前置
  后真实通知设置缺开关也得到 RED。首件8源 patch26340B，SHA256
  `de801ad4521171c3f3bd2c89d937cefe7f8960b1fcad73c130d1c575321199e9`；Root逐
  previous/new/canonical/blob核合，并用独立index核apply后叠至d73。组合tree
  `1b8eaf0a6b89b98d7dbc17ff93fd5760d0f42258`，不与base7eb+Swift8 standalone tree混比。
  证据 `.cache/notification-dnd-20261111/ios/first/root-apply-receipt.json`。
  两位独立读源审查确认旧badge覆盖阻断；同生产controller实际三例RED，窄修后旧post
  不再改写权威badge，实际提交与add完成后读取当前值，保留已读降0。相对首件3路径
  delta17628B，SHA256 `adbfe58cd8baa89d9d7c101eb1ec10ad73a44475e27a7cf816f4bc9aa92052aa`；
  Root逐9源核合，组合tree `b83ecd221ca7b0776781ae421932740e5b62d203` 与Mac精确同。
  完整普通Core85/0；v2同三徽标例及受控交错例均PASS，该轮实际4/1，设置中心tap失败
  保留。只修测试点击真实thumb后，原设置0→1、普通重启仍1、再0，独立槽1/0；不合写5/0。
  普通unsigned Simulator Release build/link0、110.910s，9个Release MachO测试marker
  全无、Debug dylib三项阳性，普通DND key存在。Root已核6份原log/回执长度SHA及原结果；
  首件旧Debug dylib SHA未捕获，未把launcher当作控制器机器码身份。
  详细记录见[iOS及Mac诊断](maichat-ios-notification-dnd-20261111.md)。

最终iOS及Mac产品树：MaiChat `8bc70424e36d44c0a5769d557e7abb0b1cf314c9`，
Core `1d870d7ff4c0c78614e7987a701c00fb7b4f19c1`。iOS实际运行构建为7eb+最终Swift9，
Mac实际构建为d73+同Swift9；不把分平台cohort拼成一轮全平台测试。三个收到PNG与Mac
原件字节不相同，原/received长度SHA分列，不推断原因，也未重发图。

证据索引位于 `.cache/notification-dnd-20261111/`。现有v2.2.58/iOS205及公开资产保持
原样；本轮未安装、重装、停止现装或发布新版本。各端只使用自有消息、profile、Fake SDK
或专用Simulator；iOS受控传输不是实际OS横幅/声音/真机验收，Android界面尚未实跑。
不将前台guard冒称已验证所有后台系统push。
