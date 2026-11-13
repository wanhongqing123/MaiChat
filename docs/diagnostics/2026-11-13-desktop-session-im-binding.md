# Desktop 好友会话绑定创作中心 Session

## 操作与预期

Windows 普通消息页选择好友，点击输入框底部命令按钮右侧的链条入口，选择已有的本机根 Session。绑定后显示会话名和“解除绑定”；取消选择不改变草稿或绑定。新收到的、人类来源的好友消息交给该 Session，只有该次输入对应的助手正文及明确交付媒体自动发回原好友。历史消息不补执行，机器消息、未知来源、审批控制帧和自己的消息不触发回环。

本机 Send/Stop 接管、解绑、登出、账号/控制器代次失配、删除 Session 或执行工作目录变化，会撤销待处理和迟到回传。解绑不停止用户正在执行的模型或工具。未增加“排队中/执行中/完成”等远端提示。

## 原因与实现边界

原欢迎页为 19 个被动网格；现在是有用途、示例和可核对工具名的纵向列表，点击仅追加草稿。好友绑定是新增的显式入口，默认关闭。

现有 Tencent 协议已经与 MultiAiCode 对齐：原生 `message_cloud_custom_str` 对应 Web SDK `cloudCustomData`，使用 `namespace=multi-ai-code`、`version=2`、`origin=human|machine`，保留引用及配文。没有增加 wire 字段、RPC、执行器或模型代理。

- `DesktopSessionImBridge` 复用普通 `MaiSendPrompt`；忙碌时仅保留最多 32 条本机路由输入，不插入或 steering 其他正在执行的任务。同步提交时捕获真实 user/assistant 凭据，不用“最新消息”猜归属。
- 图片、视频、文件和音频缓存通过可取消的本地复制进入既有 `.maichat/session-attachments` 形状。图片/视频使用既有消息片段；文档/音频使用工作目录相对路径说明。不再次下载外部 URL，不调用云附件传输。
- 出站只识别精确所属 assistant 的成功 `agent_send_media` / `current_ai_session` 回执，并复用 `maiPrepareAgentMedia` 校验。观察图片、截图、思考、原始工具文本和任意 Markdown 路径不作为交付权限。音频按普通文件发送，未伪造语音消息。
- 专用准备池各最多 2 个工作线程，物理 admission 最多 32 个，5 秒 watchdog 撤销迟到派发。原生视频缩略图及容器读取在工作线程进行；UI 不 join 准备任务。准备超时释放路由，后续文字可以继续。
- 原生工具 `maichat_session_im_binding` 的 `list/bind/unbind` 调用同一 Bridge API。Schema 为平铺对象；按 action 严格校验参数。读取不审批，有效修改遵循既有逐次审批策略。派发带有注册时固定的 SDK/owner/控制器代次，以及有界的取消租约；开始后无法确认结果时报告 Unknown，不宣称没有副作用。

## 已定位的边界问题

1. Session-only idle 可能晚于下一轮到达：只有精确 assistant 完成后才开始回传。
2. 绑定前已入库的 user 事件可能迟到：忽略精确的既存输入凭据，不将其误判为新接管。
3. 修改工作目录可能让同名媒体指向另一文件：输入保存规范化目录快照，复制后、提交前和 SDK 派发前再次核对。
4. 全局线程池繁忙及原生视频缩略图可能阻塞：改用有 admission/watchdog 的专用后台准备，保持回传凭据至真正的 SDK 派发边界。
5. 多个 SDK scope 可能同时残留同 Session 绑定：本机接管撤销所有匹配的 runtime 项，不依赖 QHash 顺序。
6. 正式启动先创建 MainWindow 后登录 IM：原生工具固定预期身份取自已验证的当前账号，或构造参数中的可信 SDK + owner；从不任意认领以后登录的账号。真实顺序回归覆盖首次登录、控制器重建及 SDK 切换。
7. 四条错误文案曾被 PowerShell 到 Python 的默认编码转换为问号：改用 UTF-8 安全补丁恢复，做字节往返及实际可读错误断言。

## 验证与证据

平台为 Windows 11，MSVC 14.44、Qt 6.12.0。源代码起点 `583a07bc48c53759a089ae8097184730eedf2a94`；Core/mobile 后续固定于 `12d5cd36bc8145043ed80eb219ac59a730f308cd`。Desktop 工作区输入以 SHA-256 清单为准；本轮未改 Core、Android、iOS 或原查询实现。

最终普通 `maichat` 构建、`windeployqt`、模型资源检查及 PE 依赖检查通过（3 个二进制、114 个 imports 和 Qt 插件）。验证期间未启动用户 App、未启用真实用户绑定、未发送真实 IM、未调用真实或付费模型。

| 验证范围 | QtTest 通过/失败 |
| --- | --- |
| Bridge，含媒体、精确归属、期限、接管与 SDK scope | 31 / 0 |
| 原生绑定工具，含真实启动顺序及取消事实 | 22 / 0 |
| 绑定 UI | 8 / 0 |
| SDK 编码契约 | 33 / 0 |
| Host 工具 / 本地库 / App / 消息匹配器 | 25 / 36 / 44 / 21，均 0 失败 |
| 既有 AgentPanel 欢迎页、MIME、布局 cohort | 18 / 0 |
| 安全的 MainWindow 布局与设置 cohort | 17 / 0 |

合计 255 个 QtTest 结果通过，0 失败。首次 Bridge 测试的三处 fixture 假设错误已先记录并定点重现：Windows 原生分隔符不等于 Qt 路径字符串；按时间排序后的最后一行不必是新 Pending 回复。修正为规范化目录边界和精确 Machine Pending 条件，未弱化业务断言。空模型沿用 Core 的有界收尾失败语义。

原始构建命令、退出码、测试日志、前后源码与二进制哈希见 `.cache/session-im-bridge-20261113/verification/`。已查看 owned QWidget 的选择列表、未绑定/已绑定窗口、宽窄尺寸、欢迎列表及纯文本 MIME 截图。主窗口窄尺寸实际受现有最小宽度约束为 980 逻辑像素，未将其表述为 850。

本轮仅完成合成 IM/模型和实际 Qt 组件验收。真实双账号 SDK 收发、真实模型端到端以及最终 staging/ZIP 解压目录 smoke 由最终集成流程另行记录，不能以本记录替代。

## Windows 完整候选门禁

产品源已提交 `761b00bffd6eb43f628abd6c627a8ec7f097bd41`，Desktop 树
`47dfde2d9cc4277920f18eb0544e0e4186f5a440`；Core/iOS 保持移动搜索提交 `12d5cd36...` 的已验树。
正常 NativeClient EXE SHA `1fdc7aa7c796cdec1dc85ce4a205ecb9176b62d4ecdf1b4c7a065615a37591e3`。
完整 staging 与真实 ZIP 解压目录各用裸系统 PATH 启动自有 `--smoke`，均 exit0；PID 分别 3632/44876，
日志里的版本、实际进程路径和 EXE 哈希与候选一致，五个必要 Qt 模块分别来自各自包目录。
81 个 stage/ZIP 文件逐字节 SHA 相等，真实 ZIP 为 241,855,035B，SHA
`bcbde7d834c7b82c6b03e90a95c9b6f8d8b890c7e638c9557284bbec5d62020d`。

候选位于 `.cache/session-im-release-20261113/windows/`，门禁在 `candidate-gates.json`。
仅启动两个自有 smoke 子进程，私有 profile 与日志路径已核准；未停止用户程序、启用真实绑定或发消息，未发布 Release。
以上闭合 Windows 包门禁，不能代替 Mac 正常消费者或真实双账号流程。

## Mac 普通消费者

同一 `761b00bf...` 产品树在原 normal Qt 6.12 cache 编译 App 与四个新增/SDK 测试 target，均 exit0。
使用 Cocoa、私有 CF home 及既有自有 sandbox 运行：Bridge 31/0、原生工具 22/0、UI 8/0、SDK 33/0，
合计 94/0、无跳过；选择列表、1100/实际980宽绑定条/解除按钮的原 PNG 已在本机及回传后查看。
Mac 视频测试日志封面来自 placeholder，证明后台调度/取消/FIFO/SDK 契约，不代表原生视频帧提取已验。

现装 App 尚未替换或重启。没有证实现成的两专用测试登录或无付费真实模型服务，故上述仍是
正常组件与合成 IM/模型验收，不能冒充真实双账号端到端。

Mac app-only 候选也已准备并核准：原脚本仅在本轮私有副本中把同卷 ditto 复制改为 clone，原共享脚本未改；
打包 30.669 秒、exit0，26 个 arm64 MachO 的正常依赖门禁通过。五个模型共 53,898,648B 逐 SHA 相同且
APFS 全块共享，无模型物理复制。候选 EXE SHA
`80e4a9805a6c5053698430976d4c9ae8966b1df2313226037d7e57f46c27d4f0`。
现装/候选均为 `com.kongshang.maichat`、2.2.58/2.2.58、adHoc、Team 未设置、entitlements `{}`，
strict deep 签名均 exit0；现装 EXE `9d29c92f...` 与 PID45041 尚保持。

候选路径为 `/private/tmp/maichat-desktop-binding-mac-20261113-xk_u8rvz/candidate/MaiChat-macos-arm64/MaiChat.app`。
已完成全部授权的准备和校验；替换/正常退出重启需依仓库约定取得最后明确授权，未将清理磁盘或 iOS 安装授权
扩大解释为 Mac 退出授权。没有启用真实好友绑定、读取私密凭据或声称真实双账号/真实模型 E2E 已通过。
