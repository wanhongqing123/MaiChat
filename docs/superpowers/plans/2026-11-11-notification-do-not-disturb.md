# 客户端免打扰实施计划

**目标：** 用户明确要求所有客户端增加免打扰，不弹新消息通知；开启期间继续接收、保存
消息并更新未读，关闭后只提醒新的实时消息。

**范围：** Qt Desktop（Windows/Mac）、iOS、Android，各设备独立本地持久设置，默认关闭。
逻辑 key 为 `maichat.preferences.do-not-disturb`，不进入账号/房间 JSON 或 WS 同步。
沿现偏好存储与 UI，不新增服务、Native/CABI 协议或版本变动。OS 备份/迁移继承不等于
运行中的跨设备同步，本轮不扩大备份规则。

## 用户操作与共同合同

- 在现有“设置/我的→偏好设置”中提供“免打扰模式”开关。
- 说明：“开启后不弹出新消息通知；消息仍会正常接收，未读数量保留。”
- 只抑制现有普通新消息提醒及附带声音/振动；保留未读、徽标和连接，不扩电话/邀请、
  采集前台常驻通知、操作错误反馈及普通触感。各端原来未提醒的消息类型不新增提醒。
- 提交系统通知前读取最新设置；异步队列还核到达时设置及代际，过期开关状态的回调丢弃。
- 开启即时取消本 App 可识别的新消息通知，不能误删关闭后新提交的通知；不 cancelAll。
- 关闭不枚举未读补发，也不回放已静默消息；下一条新实时消息继续沿原前后台规则提醒。
- 已交给 OS 的历史提醒撤销按实际平台能力记录，不将前台 guard 当作所有后台 push 静音。

## 分工与必要测试

- [x] Desktop：现 MessageNotification 普通策略与 MainWindow 设置行/入站最终提交点。
  QSettings 存 key；同 UI 线程同步提交不新增待发队列。保留 tray icon 与未读，开启清
  应用提醒去重状态。Windows 可用公共 empty title+message 清当前 balloon；不承诺清
  系统通知中心历史，其他平台不盲发空白通知。Windows policy11/0、布局自有用例9/0、
  当前气泡撤销3/0；Root亲看原设置PNG。macOS必要消费者单列待回件。
- [x] Android：MaiChatPreferences、RemoteIMNewMessageNotificationPolicy、MainActivity。
  独立本地 prefs；UI queue 快照 flag+generation，最终 notify 核最新状态，防止 on/off
  期间老回调补弹；只清 message channel，不影响捕获 ongoing。原 policy/JVM 测及正常
  App/test 编译，policy7/0。独立review指出的legacy夹具异步等待/listener问题已只修测试
  并正常重编；兼容设备不可用，instrumentation UI 未实跑，不宣称DB/MainActivity验收。
- [ ] iOS（mac-maichat 独占）：AppPreferences、MySecondaryViews、MaiChatApp、
  RemoteIMAppState 与 MaiChatCore 普通 policy。MainActor 可观察设置；prefix 限定清理，
  异步授权/add 的代际守卫及旧回调取消不能误删新通知。原普通策略和正常 Simulator UI
  同例；按需 guard 权限预提示，不为免打扰触发授权请求。
- [ ] 各端验证默认关闭、持久、前后台抑制、开关期间异步竞态、关闭后新消息可提醒、
  接收/未读保持与非消息通知/反馈保留。使用自有消息/profile/Fake SDK，不访问真实 DB、
  账号或在线供应商，不操作用户现装。
- [ ] Root 核 plain patch/manifest/source identity、独立 review、必要普通消费者后统一提交；
  在 `docs/diagnostics/` 分列平台实际构建、测试与未验边界。独立 Windows 指针修复继续
  进行，两任务不互相冒充验收。
