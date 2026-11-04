# 本地主模型与 iPhone 输入栏收尾验收

## 操作与预期

基线 `5216279a`，本轮从 2026-11-03 延续至 2026-11-04。用户最新明确要求：

1. 创作中心只选“云端模型 / 本地模型”；本地模型使用同一主 Agent 请求、历史、工具和队列，不再以调用 llama 工具或独立引擎测试冒充主模型。
2. 主模型不显示上下文、线程、输出 token 等实验参数；默认生成至模型结束符，真实上下文和内存边界明确报错。
3. 输入栏只去掉模型停止按钮；加号的相册、拍照、文件入口保留，忙碌或等回执时照样接收下一条消息并排队。
4. 美颜完成后把图片发回聊天，用户点击才预览；明确请求的独立预览继续可用。
5. 安装最新 iPhone App，检查源码、产物和设备实际版本；实际不能完成的真机验证必须单列。

## 已完成的核心检查

- 官方 Qwen2.5-0.5B-Instruct-Q4_K_M，491400032 bytes，SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`。未把 491MB 权重提交源码。
- 完整真实模型 Agent 工具闭环、自动 EOG、上下文与半截工具门禁、合作取消、CPU/GPU 驻留隔离、驻留退出，最终 Core8 8/8（77.05s）。证据 `/private/tmp/maichat-local-primary-model-20261103/metal-core8-final-evidence.json`。
- Mobile C ABI 的真实配置、两来源隔离、私有本地 client probe 的无工具执行/不写会话/参数守卫，最终真实 Qwen 测试 exit0（20.202s）。证据 `/private/tmp/maichat-steer-mobile-native-20261103/primary179-device-probe-final-evidence.json`。
- 同一完整 119 工具目录 macOS Metal 首字 13.979s、GPU 权重391859712 bytes；这是原生诊断样本，不能等同 iPhone 性能或完整正常 App 两轮通过。

## 179 的实际 App 检查与失败记录

- 2.2.4/179 正常模拟器实际运行：忙碌输入栏无停止且保留附件21.445s；五条持续发送等回执仍 FIFO25.520s；真实美颜产物默认聊天卡片、点击才预览16.931s；手动相机不复用定时拍摄32.936s，均通过。Root 已查看实际输入栏和点击前后图片截图。
- 结果包 `/private/tmp/maichat-primary179-composer-portrait-regression-20261104.xcresult` 与同名前缀录屏。调试 fixture 使用独立内存会话，不登录或触发付费请求。美颜消息 UI 使用 fixture 注入，未冒称真实 Agent 发媒体闭环。手动相机使用合成帧，未冒称真机相机硬件验收。
- 独立 TinyStories 自测确实生成完毕，但旧测试用离屏 Picker 后代定位滚动容器，在虚拟化回收后失败。已改为 Form 稳定标识，保留全部实际生成与卸载断言；最终结果另追加。
- 本地主模型正常 App 的 v5（180s）和 v6（360s）首轮都未完成。v6 最终实际状态仍 `busy:true,assistant-text-count:0,gpu-selected:false,error-empty:true`。这些是失败记录，不能宣称两轮 UI 或重开恢复通过。
- 定点真实 Simulator Metal 算子探针证实 Apple2 设备不支持 Qwen 的 Q4_K matmul/F16 FlashAttention，实际 CPU 回落；不能伪造 SIMD 能力。v6 仅为功能验证在该能力已证实不足时提供360s窗口，仍失败，不继续放宽。证据 `/private/tmp/maichat-ios179-metal-registry-probe-20261104/evidence.json`、`/private/tmp/maichat-primary179-main-ui-v6-20261104.xcresult`。
- 179 真机 App 构建成功，11个 Mach-O单体严格签名及外层 deep/strict 全过，安装到 iPhone14ProMax 后设备 apps 返回2.2.4/179。Qwen 通过普通 Store 准备目录后合并既有 catalog，仅追加新 UUID 权重，保留原选择；从设备读回完整模型大小/SHA均精确匹配。
- 真机启动实际返回 CoreDevice10002 / FBS Locked，锁状态 `passcodeRequired:true`，已请求解锁。没有绕过设备锁、读取用户聊天或用 Mac 的 GPU 数字冒称真机通过。独立内存硬件探针尚未运行，`agent_loop_exercised=false` 也不允许作为工具闭环验收。

## 同期变更与最终冻结

02:06 fetch 发现远端已前进七个提交到 `9d037c53`，包括 Android IM/图片展示以及 iOS 云盘 loading 文案。必须保留并合并，不能强推覆盖。合并前 Android254 原ROI四项41.287s通过、正常 Main PID30778、已查看登录页，收据明确只用于合并前检查。最终合并产物和回归另追加。

收尾源码审查又发现模型 open/save 跨 await 与删账号屏障之间的重入竞态：迟到配置可重新创建已删除目录。此处修复和红绿证据另追加；正常切换账号仍须保留旧账号已接受发送收据。

本地 C++ 索引刷新454条真实自有源文件命令，并修正 MobileAgentAdapter 最近层旧 compile_commands 链接，统一指向工作区根数据库。临时构建和权重不进提交。

## 待完成边界

最终账号屏障回归、合并后的正常 App 产物与指定回归、真机解锁后的本地 GPU检查及普通重开，尚待完成。付费 DeepSeek Search/Fetch 供应商召回质量与真实账号模型消息，未做线上付费验收。

## 最终源码合并与账号屏障

本地主模型代码提交 `02e3d5f9`；在 main 合并 `9d037c53` 得到 `bb5dbbaa`；
`6ebfc506` 冻结最后产品源码与 iOS180 / Android255版本。未新建分支、未强推。

账号配置删除竞态已确定性红→绿：真实 open/save 方法跨本地解析 await 暂停，
永久失效并删除目录，再恢复。旧版两个分支均成功并重建目录；修后两个分支均拒绝、
目录不存在，正常保存控制组仍成功。测试使用真实 actor/文件系统及提取的产品方法，
native configure 只用定点文件副作用替身，不冒称完整 App/native 删除验收。
最后源码与合并后复测均通过；完整边界见同目录 `maichat-ios-primary-config-deletion-race-20261104.md`。

180 正常模拟器 App 已构建成功并实际进入原创作中心执行五项回归；最终真机增量构建中。

## 180 最终正常 App 与安装结果

最终正常模拟器五项全部通过：22.134s忙碌输入栏、25.668s五条FIFO、35.013s真实Tiny
引擎生成并卸载、15.738s修图成品留在聊天及点击预览；手动相机同批通过。总5项0失败，
131.443s。结果包 `/private/tmp/maichat-primary180-regressions-final-20261104.xcresult`，
正常 App 全程录屏同名前缀 `.mp4`；Root已查看最新输入草稿、Tiny真实文本/计数、修图
点击前和手动快门画面，并保留完整录屏。Tiny显示32token是独立引擎测试预算，不是主模型
输出限制；该测试页不用于代替主模型对话验收。

180 真机严格签名11/11与deep/strict全部exit0，再次安装后设备返回2.2.4/180，
installationURL末级3373DC48-60BA-4D92-ACD2-A031A7CA3CC8。已知Qwen UUID文件仍在，
491400032 bytes；前一179安装后已完整读回SHA匹配，更新180未重写目录或模型。
`/private/tmp/maichat-primary180-device-final-artifacts-20261104/evidence.json`记录70个
产品源码文件SHA和全部Mach-O，安装后复核所有源码仍匹配冻结。

最终启动仍实际返回CoreDevice10002/FBS Locked；未产生本次真机运行PID，独立GPU探针
与正常重开、主模型两轮对话仍未通过验收。没有用已安装或模拟器五个UI绿测冒充它们。

Android255最终运行回归仍在进行，详见随后单独记录。

## 最终交付状态

Android255合并后9项设备测试和9项JVM测试全部通过，正常Main PID31293，
安装APK哈希与冻结件一致。完整记录见 `maichat-android255-final-validation-20261104.md`。
Root还查看了180全程录屏按10秒采样的关键帧、五个case原始截图及179主模型选择页；
主选择只有云端/本地两项，输入有草稿时保留加号与发送，没有模型停止。逐帧无闪烁
和真实相机硬件并未据此验收，录屏测试间的Home是测试主动退出重启。

交付时iPhone仍明确锁定，已安装180但不能宣称重开或本地主模型实机两轮完成。
本地Qwen源文件已在沙盒，解锁后可从创作中心“更多→主模型→本地模型→使用此模型”
进行文本与工具调用测试。当前主模型只支持已验证的Qwen2.5 Instruct模板；图像历史和
projector明确不支持，不能把独立mtmd工具能力当作主模型多模态已接通。

普通云端既有Responses/Chat协议保留，本地走同一供应商中立ModelRequest/StreamSink
契约的进程内client，不新建虚拟HTTP服务或模型代理。

美颜源码包按用户要求在构建收尾后交付，首部记录main和最后提交摘要，附路径、函数名、
逐文件SHA及原文。当前processing_bbox由人脸框扩大约1.3倍，外层ROI约2.5倍，最终皮肤
alpha还受人脸椭圆限制；确实不会给脖子、手臂、腿美白。本轮只改默认交付提示，未修改
美白参数曲线或全身mask算法。`segmentPerson` 是独立Vision人物分割，没有参与这条美白。

remote_agent_call按用户明确低优先级建议不在本轮新增；无需为该想法打断验证或追问。
