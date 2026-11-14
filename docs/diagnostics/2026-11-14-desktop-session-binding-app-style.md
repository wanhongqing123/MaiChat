# PC 绑定会话弹窗统一 App 样式：2026-11-14

## 操作和预期

用户在好友私聊打开“绑定创作中心会话”，截图中的原生QDialog长说明、裸列表与灰色方形按钮
与App蓝白视觉不一致。要求整理标题、说明、行选择、留白和主次按钮，不改变历史导入、
绑定/解绑或消息路由语义。基线为 `a7012733abf19ff00055d06bf8745397a188e70f`。

Root先运行正常MainWindow的自有绑定UI原槽，查看真实旧弹窗静态截图；原槽3/0。
测试使用独立settings、自有临时SQLite/会话与Fake IM，不读真实账号或聊天DB。

## 呈现方案与守卫

复用现有AppTextInputDialog、AddContactDialog的白圆角面板、语义字号、蓝色主按钮和灰色次按钮，
列表使用已有搜索结果的浅蓝选中态。标题简化为“绑定会话”，两句短说明保留先导入本机历史、
仅处理新消息及旧消息不触发任务的含义。主按钮标为“导入历史并绑定”，动作仍调用同一个bind。
长标题单行省略，滚动保留；不可用会话维持disabled灰态。关闭按钮仍仅走reject。

以下业务段保持原逻辑：单实例dialog、peer/account/连接代次/controller pin，UserRole稳定会话ID、
UserRole+1 canonical workspace，首次默认/后续不回填空选择，动态刷新保留滚动位置并清除失效选择，
确认前再次校验会话与目录。取消/Esc/关闭不解绑、不发送也不清草稿；解绑仍须显式点击原入口。

## 验证与证据

Windows正常Qt6.12/MSVC14.44目标先完成maichat和原UI测试构建、部署3binary/114PE导入门禁。
使用普通MainWindow原绑定、取消、跨账号、占用、切换创作中心、动态标题/删除和切好友用例回归。
静态视觉取证仅扩充自有会话标题及截图钩子，不用CSS字符串断言代替查看画面，也不增加业务测试分支。

证据目录 `.cache/desktop-binding-style-20261114/` 保留before、after-01和最终自有静态截图。
这些是QWidget静态合成画面，用于核标题、列表、选中/disabled和按钮布局，不宣称原生连续帧验收。
Mac需收到精确小件后在普通Cocoa原槽消费；此处不预报Mac通过，也不替换用户现装。

最终Windows原绑定UI套件 **10pass/0fail/0skip**，MainWindow唯一助手入口原槽 **3pass/0fail/0skip**。
选中、长列表与不可用行的静态取证用例也均退出0，已实际查看原PNG：长标题正常省略并纵向滚动，
已占用行灰显且主按钮禁用。常规回归关闭取证环境，不改变原业务断言。
原before与after均有既存QFont point-size警告，日志保留，未把它作为此次样式根因。

最终MainWindow.cpp SHA256为
`af3bdff1e585b4852e59e86baf46cd955092e2f9b520a92d024e767671cb1a98`，
UI测试源为 `10f6745d91d5e6e9de74e11fab09bd64f5865f301d97225b8af4fce719cbe168`。
独立复核确认，从原cancel连接起到函数结束的业务段与基线逐字节一致，规范化SHA256均为
`7e51c36652f07f2e71a3b32a9b5a110af70cb806c2713db7cbafe685fa043bb3`；
新close仅reject并关闭其自身autoDefault，没有给confirm新增默认确认行为。
原始回执在 `verification-02.json`，列表滚到顶部的截图仅用于查看长标题，选择仍保留。
