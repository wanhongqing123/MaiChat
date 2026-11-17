# iOS 欢迎页卡片精简

2026-11-16。用户要求移除三端“AI问答”介绍卡；随后明确移动端图片/视频创作已禁用，欢迎页也应隐藏对应两张卡。

iOS根因：AIAssistantView.welcome为静态网格，仍写有三张介绍卡。最终只删除“AI问答/图片创作/视频创作”三个welcomeFeature调用和同步注释；没有改输入框、文字/语音入口、聊天、工具注册、模型/服务能力或任何账号数据。

复用普通App testWelcomeMobileCapabilitiesRemainScrollAccessible，补三张卡不存在和ai-composer存在断言，原定时任务/二维码/跨设备卡及滚动断言全部保留。正常Simulator 219实际1/0（8.240s），另ASK-only 218同例1/0（9.097s）。最小展示修改未新增测试或触发模型任务。

218正常device build0/131.495s，签名0，保数据安装0/14.994s，fresh2.2.4(218)，普通启动PID67651，候选主程序SHA256 66f65c5d19ddb9d9a0e42ad266fdebf2515ccc3c09d097b341106a03cb41575f。218当时只去QA，收到两张禁用生成卡补充后追加219；不把218称为三卡全部完成。

最终219的source2相对原AIAssistantView及已合217的UITest327455fa一次增量。Root已确认218 ASK-only两源从未apply，所以不叠两份包。证据目录 /private/tmp/maichat-ios-welcome-no-qa-20261116；219正常安装/版本/进程另补回执。Android与Desktop同请求由house-maichat对应agent独占，本机不冒称替代它们验收。

219最终normal device build0（140.364s），严格签名0，保数据安装0（12.492s）；fresh实际2.2.4(219)，普通无fixture启动PID67679，候选主程序SHA256 8bff1ed4d33e85cd0b3b15d948d7563e8ea70478ba45d7b5ef4229d42e10f773。这里是本地候选SHA+安装回执+实际版本/进程，不冒称独立读回设备二进制。219三卡全无的Simulator顶部/底部真实图已亲看；没有读取真实账号聊天或触发模型请求。

最终2源包2067B/SHA256 e9a38fd8ac933aee30a374740347945e86b2aa8070da35634735afb6bd9da114。私有replay check/apply均0，两个postimage与当前源逐等。iOS输入框相关代码零改动。
