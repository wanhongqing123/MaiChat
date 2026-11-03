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
