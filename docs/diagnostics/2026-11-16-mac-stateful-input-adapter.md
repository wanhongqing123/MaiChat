# Mac 通用按住输入适配与主线程生命周期

2026-11-16。用户要求参考 Sunshine 优化通用 PC 原语，不针对特定滑块。house-maichat 负责公共 API/controller/Windows/工具目录，本侧仅 Mac adapter 和独立私有验证，不修改共享头或宿主 CMake。

## 参考与设计

固定 Sunshine `bb6930bd782ed3edd2eea9a71d46b42183be9358`，从官方 GitHub 读取 [input.cpp](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/input.cpp)、[virtualhid_input.cpp](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/platform/virtualhid_input.cpp)、[video.cpp](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/video.cpp)、[stream.cpp](https://github.com/SunshineStream/Sunshine/blob/bb6930bd782ed3edd2eea9a71d46b42183be9358/src/stream.cpp)。观察到持久键鼠 ledger、按 stream 串行有界队列、独立双轴滚动、视频/输入共享 viewport 映射、会话停止后 reset。该版本 Mac 已使用 libvirtualhid，不能把它称为 Quartz 示例。

本地原 InputScope 每调用创建 private CGEventSource、结束全释放，只能覆盖一次性动作。新 ordinary session 保留 source 和 owned ledger，支持五按钮、独立 mouse/key down/up、双轴 wheel、持有修饰键时鼠标组合及短 key tap；同 target 的持有状态不重新全量匹配 AX 窗口。目标变更、取消、权限/secure 拒绝、异常及销毁按协议清理 owned。targeted up 不覆盖剩余 hold 的原 binding/source，不读取清理请求的 cancel。

键名按共享 grammar 首个等价单键 canonical；up 使用 down 时登记的 native code/侧别，不重新解释当前布局。独立重复 down/未知 up 幂等；atomic 同码非 modifier key/button 拒绝 Busy 并清理 owned，避免错误 pop 老尾项或无事件假成功。Type/Replace 遇 owned Control/Shift/Alt/Meta（含左右）拒绝 Busy 并清理，避免 Unicode 变成快捷命令。

硬件已持有的输入不登记为本 session owned；冲突 up 不制造硬件释放，清理其它 owned 后退休 private source。[Apple 说明](https://developer.apple.com/documentation/coregraphics/cgeventsourcestateid?language=objc) private source 有独立状态表并随 source 释放。状态数组仅是 synthetic ledger，不是应用接收 ACK。

## 独立 P1/P2 与最终修复

首个 draft 49cb84ee 经独立 review 发现：nativeKey/activation 的 dispatch_sync(main) 会与 Controller 状态锁、GUI Stop 或主线程析构 join 构成死锁；atomic 与既有 hold 混用会错误释放尾项；literal 输入未拒绝 owned modifiers。

最终消除两处 dispatch_sync：layout 使用 heap ticket 在 main 复制 CFData 和键盘类型，worker 离线 UCKeyTranslate。activation 保留一参入口、增加带按值 cancel 的两参入口；只值捕获 window。队列工作在目标只读准备完成后、真正激活前，以 liveness mutex 同步检查 live/deadline/cancel；等待者返回前以同锁清空 callback。已提交的 OS 动作不承诺回滚；过期/撤销的排队工作不再开始激活。AX 引用设 100 ms 消息 timeout，避免无响应应用无限阻塞清理。

最终 cpp blob `98d2a14d125e7421cfdc9a888a9b76ebc3564c38`，87613 B、SHA256 `a05a42e60043a5775bb120eff0778e18ff89b91aa684c4a760a57412e59cb5e4`；实际编译 R3 Platform 头 5696 B、SHA256 `c3db90986d21092128da57594399cd3988afb38f532352b5e06faab125f1a71b`。Root 同 agent 重复 review PASS，严格应用一次后 hash 一致，已合源 push c38c5af1。

## 验证层级

- 真实库/真实头独立 clang + QtCore 编译 0；不注入事件的 empty/up/cancel/callback 合同 0。
- 实际查询测试进程 `AXIsProcessTrusted=0`。未申请新权限，未借安装身份，未发全局输入。
- C 接口记录 seam 覆盖 owned ledger、source、五按钮、双轴、左右 Ctrl、短 tap、targeted up、取消/异常、secure/权限拒绝及硬件冲突；所有会改变系统输入的调用只记录，不进入 OS，矩阵 0。
- 真实 GCD main/Observed-lock 等待：layout 与 activation 的 timeout/cancel/main-side guard 六用例全 0；超时约 254 ms、取消 19–23 ms，泵过迟到队列后 activation/layout 均未执行，callback 弱引用释放。补正常 layout/activation 正向 0；只读准备在 main 延迟 400 ms 时超时后激活计数仍 0。
- 精确旧 49cb 负对照：两个入口均挂住超过 1.2 s，仅结束自有记录进程；旧 atomic/literal 矩阵 13 fail，最终源同组 0。

全部私有源、冻结头、记录 seam、负对照和回执位于 `/private/tmp/maichat-mac-stateful-input-20261116-2yluss6x`。没有将记录 seam、只读布局或本侧编译称为真实全局键鼠/窗口激活验收。没有更新用户 Mac/Windows App 或部署 Sync。

消费边界：本机工作区仍 C3 加既有移动/播放器 WIP，Mac 的 common Core/Controller/Owner/Catalog c38 整体尚未在本机独立编译链接；上述编译仅针对最终 Mac TU、共享 R3 头和稳定 Keys/Error 依赖。Root 的 Windows/Core/Catalog 运行记录另行负责，未与本侧结果混报。
