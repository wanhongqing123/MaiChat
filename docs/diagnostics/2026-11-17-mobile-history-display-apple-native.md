# Mobile历史显示投影：Apple原生消费者

2026-11-17，按协调端独占源码和平台分工，仅在private overlay消费MobileAdapter两件候选，不改跟踪工作区、main、App现装或服务。base10fe0c649aac3917d8ab6c3b9ba3450ddcfc55ae；patch11569B/SHAdeb9010ac1a1d6973cf7206f0153e32d17cdcc8473e21e1e148d834d0167c50c。两个前后Git blob均精等供件manifest。

## 操作与实际构建

复用既有mobile-agent-iphonesimulator-arm64-2c3ea62b0a00消费者。临时启用MAICHAT_MOBILE_TESTS，仅提取普通目标的真实生产/测试编译和链接参数；源码仍为已提交版本。分别在private目录编译旧生产源、候选生产源、候选测试源，全部退出0（4.969/5.080/8.776秒）。复制正常maichat_mobile静态归档，精确替换其中一个MaiMobileAgent.cpp.o，逐成员SHA确认所有其他对象不变；测试仍链接正常Core、SessionIM、History、Network和原平台闭包。测试对象、产物与签名均在private目录，未写原库。

同一候选测试首先链接旧生产对象，simctl原生执行--history-display-only真实退出1/0.641秒，在显示正文断言失败；候选对象同路径退出0/0.461秒。不是调用预制旧exe或以另写probe代替原测试。原测试经普通配置、绑定、读取自有SQLite历史、导入、snapshot/remote_snapshot/messages_page及destroy/recreate检查只读显示投影，确保不把模型导入封装、owner字段或本地文件路径当成显示正文或文件能力。测试只建立自己的DB，没有读取用户历史、凭据或运行模型请求。

## 证据与边界

最终输出：PASS: Mobile imported history is passive display metadata, not the model envelope。旧例失败记录、编译/链接参数、成员比对和safe-receipt.json在/private/tmp/maichat-mobile-history-display-20261117。finally已恢复原MAICHAT_MOBILE_TESTS=OFF，跟踪工作区仍clean，两个生产前像仍等base。

此结果只证明该Apple原生C ABI显示合同，包括持久重建；不证明iOS/Android界面已消费方向、引用、媒体展示字段，不重复公共逻辑组，也未安装App或推进远端main。本记录由源码owner随其最终聚合提交归档。
