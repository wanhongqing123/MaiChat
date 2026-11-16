# 2026-11-16：通用 PC 输入原语与状态组合

用户要求对照 Sunshine 完善 PC 的单个操作与组合能力，说明不绑定特定任务，也不强制
“观察、点击、输入”的固定步骤。源码基线 `4bf370f7`，在 main 保留原有三路径 WIP 开发。

## 能力与实现

普通 `DesktopComputerController` 与模型工具复用同一实现。原生
`DesktopNativeInputSession` 独立提供 input/state/validate/releaseAll，不需要调用工具协议。

| 能力 | 对外动作/参数 | 关键边界 |
| --- | --- | --- |
| 五种鼠标按钮 | click/double_click/drag 的 button；mouse_down/up | left/right/middle/back/forward；原 right_click 保持右键别名 |
| 持续按住与移动 | 独立 down/up、move | 观察与截图不松手；重复 owned down/未知 up 幂等，不移动指针 |
| 键盘单键与组合键 | key、key_down/up、type、replace_text | 物理键可保持；短 tap 不释放原有 owned 修饰键；Unicode 文字遇 owned 修饰键 Busy |
| 双轴滚动 | scroll，axis=vertical/horizontal | 正数分别向上、向右；保持已有 owned 修饰键 |
| 状态与释放 | input_state、release_all、clear | 只描述/释放本会话合成的输入，不接管用户预先按住的键 |
| 目标与观察 | display/window scope、绑定 token | 保留既有坐标、几何、焦点和一次快照约束；up/清理不依赖有效 token |

持有状态独立于一次工具调用、截图和观察对象，绑定 controller/session/target/稳定 turn。
一个拥有者持有输入时，其它会话可观察，不能插入输入。有效拥有者活动续期，120 秒无活动
回收；后台每 250ms 检查目标/权限。接口回报剩余空闲时间。

清理包括明确 up、任务结束/取消、原生/安全检查失败、clear/析构和失效。Core 工具注册表
在全部 run 退出路径执行稳定 turn 的 onTurnFinished；单个 hook 失败不阻止其它 hook，
原始错误保留，取消不会隐藏清理失败。GUI 生命周期另外幂等回收，并忽略旧 turn 的 UI
终结事件，避免影响替代任务。

观察撤销使用原子失效标记，不让 GUI Stop 等待正在平台调用中的 ObservedWindow 锁。
原有激活函数保留，新增可取消重载。平台排队工作必须有界，调用结束前清除回调，并避免
过期 ticket 的迟到激活；已经开始的 OS 请求仍有部分生效边界，不承诺撤回。

## 复现与证据

先用自有窗口调用真实工具/native API，记录鼠标事件、键盘事件、控件读回及输入状态。
全部测试无付费模型、账号操作、外部网站或真实验证码；滑块仅是能观察按下/移动/抬起的
自有事件接收器，不是产品的任务分支。

- 原接口 mouse_down 未支持：普通 native fixture 实际 RED，随后独立 down → observe →
  move → up GREEN。这证明原语可组合，不是给模型规定任务顺序。
- 早取消和安全输入拒绝保留已有 Ctrl：独立 controller seam 实际 28 个失败 → GREEN。
  覆盖 window/display、其它 session、旧 turn 不撤新 turn/token。
- Core 统一结束资源清理：66 个实际失败 → GREEN，覆盖正常/错误/异常/取消/早退、多个
  hook 含 internal、首次错误保留、三次 steer 的唯一原始 turn 身份。
- 独立 up 的 dry_run 与持有 Meta 后 launcher：附加 9 个失败 → GREEN。
- 模拟平台等待 GUI 的普通线程测试：GUI release 等状态锁的 2 个失败，改原子撤销后 GREEN。
- Windows 自有输入框实测 Ctrl+Unicode 原来返回成功且文字变化，新增字面输入保护同例 RED。
  最终同例 type/replace_text 均 Busy、文字不变且 owned Ctrl 已释放。
- GUI 旧 idle/error 误报新任务终结：4 行实际 RED → 完整 AgentControllerTest 49/0/0。
  全组另遇旧 fake reasoning-only 触发既有补答的测试误路由，修正 fixture 真实正文及严格
  推理隔离断言后全组通过，不更改 Core 补答行为或删断言。

正常 Core 9 套与额外四套 FilePath/Tool/ToolLoop/Permission 全部执行 exit 0。额外套使用
普通 Core 目标的编译参数和当前 Core 库编译链接，分别保存源码/EXE/进程/退出码证据；
没有新增第二套产品实现或改测试默认入口。

## 产物与 SDK 对应

Windows 普通缓存 `MaiChat/desktop/.cache/build-qt6-ui-20261109`；证据在
`.cache/desktop-input-primitives-20261116/`。图形测试经过 Windows runtime gate，运行
windows QPA，明确记录 SDK 插件。用户原正常 App PID 56404、路径
`D:\Program Files\MaiChat\maichat.exe` 保留，未停止、替换或安装。

本轮 bootstrap 对受控 Qt overlay 重新链接后，SDK/部署两份 qwindows SHA256 为
`47bcb160c01ffe4a171c6147b5b8b07063088c9db26756b757d2581f99ebc549`，Qt6Gui 为
`20799bc1aa02f1ff619c9c28128c13a4dfdea82dfd229aae4bab285d86474616`。
旧冻结包的 28fc/359f 摘要不能套用到重链产物。已核两份 .text 与其余非 .rdata 节相等，
安装 stamp 保持上游 025bdad1、overlay SHA a66f64 和两个受控 postimage
75789622/29924055。详情 qt-sdk-relink-provenance.json。

最终完整 Windows native 组 **11 passed / 0 failed / 0 skipped**，90.433 秒，实际加载
上面两份 SDK 摘要；包含 8 组 Unicode、五按钮独立与短操作、双轴 wheel、持久 modifier
组合、字面输入拒绝、取消/异常/析构/回调寿命。见 native-verified.txt、逐样本 JSON 与
windows-native-final-receipt.json。此前套用旧 SDK 摘要产生的前提失败记录保留，不算
产品输入 RED；更新产物凭据后未删原生行为断言。

## 平台与验收边界

Windows 验证包括真实原生输入/采集及普通工具入口；不等于付费真实模型自主识图选点验收。
Mac adapter 由独立平台协作者消费冻结接口，当前测试身份 AXIsProcessTrusted=0，不能
把编译、记录 post 的 seam 或空操作回执冒称全局原生键鼠实测。最终源
`98d2a14d125e7421cfdc9a888a9b76ebc3564c38`、SHA256
`a05a42e60043a5775bb120eff0778e18ff89b91aa684c4a760a57412e59cb5e4` 已精确消费。
R3 头 `c3db90986d21092128da57594399cd3988afb38f532352b5e06faab125f1a71b`
实际编译 exit 0，非注入与 record-only 矩阵 exit 0，OS posts=0。

后继消费回执进一步限定：该 Mac 编译使用最终原生适配文件、最终 Platform R3 头和
本地稳定 Keys/Error 依赖；尚未在该工作区独立编译链接 `c38c5af1` 的 common Core、
Controller、InputOwnership 和 StarterCatalog。Windows 的 Core/UI 结果不能替代这一
Mac common 层验证；目前也不宣称 Mac 全树构建或完整集成验收通过。为保留该机现有
Android/Swift/播放器 WIP 及用户连续 iOS 真机回归，本轮未执行全树更新、全 Qt 构建
或包装。该未验证项保留，后继通过独立精确源 overlay 再核，不扩大 TCC 或安装授权。

Mac 草稿两处同步 main 等待被独立审查判为 P1；最终改 heap ticket、有界可取消等待、
离线键位解析和 commit 前检查。旧草稿同例两入口实际等待超过1.2秒，最终六例全部
通过（超时253～254ms、取消19～23ms、晚 activation/layout 均0、回调弱引用释放）。
原子同码弹出旧 ledger、Unicode 继承 owned modifier 两个 P2 同矩阵旧13失败 → 新0。
两名独立 reviewer 复核精确源码/补丁/R3头及这些修复，未见剩余具体 P1/P2。
没有申请或绕过系统授权，也没有把 iPhone 保数据安装许可外推到 Windows 更新或 Sync 部署。

通用说明见 [坐标与输入状态](../development/desktop-computer-coordinates.md)。Sunshine 对照
固定 `bb6930bd782ed3edd2eea9a71d46b42183be9358`，来源与最初独立截图/完整 drag 边界见
[按住鼠标观察记录](2026-11-16-desktop-held-mouse-observation.md)。
