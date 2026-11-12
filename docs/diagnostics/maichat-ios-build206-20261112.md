# iOS build206：移动补读工具注册与真机交付

本页先保留 a328 阶段的构建与断连记录，末节记录后续 3bef 新候选。
旧 a328 候选已暂停安装；两份候选都没有装后设备回读。

## 操作与预期

用户要求把最新 MaiChat 安装到既有 iPhone。沿用普通 Release、原签名和设备，
把现装 `com.kongshang.maichat` 的 `2.2.4 / 205` 保数据覆盖为 `2.2.4 / 206`。
不卸载、不清容器、不读取或导出真实会话数据库；不改版本源或公开发布资产。
安装后须实际读回版本，并核对普通启动的 PID、可执行文件与安装 bundle 的归属。

## 源码与环境

- 时间：2026-11-12，Asia/Shanghai；Mac/设备操作由 `mac-maichat` 独占。
- 首轮源码：`ce74a918893e3b7ffd00d0280230b0f51d9a23ee`。
- 注册修正源码：`a328d4e87ca4815b2c0fb0ca1b1116dc2945efcb`，main 已推送并读回。
- MaiChat tree：`d5bdb8c0b14d1cde78e1a7bacec8a3a16cc2ab49`。
- MaiAgent tree：`4d7d22a57e8d239f5ad99933721f15f358f4ef6a`，修正未改核心。
- device 构建复用原缓存，Release、arm64、iphoneos27.1、原 Team；
  `CURRENT_PROJECT_VERSION=206` 只作命令行覆盖。

## 交付时发现的注册遗漏

首轮 ce74 普通 generic iOS Release 已构建成功，但设备端在安装前核查发现：
`MaiToolOutputTool` 对象存在于 Core 和最终 Native 合库，签名 exe 却没有 reader factory
或 `tool_output_read` 字面串。这份产物未安装。

根因是 `registerMaiBuiltinTools()` 已注册 reader，而 iOS、Android 共用的
`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp` 手工构建 registry，遗漏该工具。
Desktop 和 CLI 使用内置注册，所以此前 Windows 核心与桌面验证不能覆盖此缺口。

修正仅包含 `MaiToolOutputTool.h` 并普通注册 `makeMaiToolOutputReadTool()`。
现有 configure 已把同一 SQLite store 和 registry 传给 MaiAgent；Runner 已按当前
session 绑定 `toolOutputStore` 和 call ID，无需修改 Runner、Store、ABI、CMake、Swift 或 Java。

## 同例失败与修正后验证

验证使用普通 Mobile C ABI 的 configure、send、snapshot，以及 loopback 假模型。
自有临时 SQLite 记录保存原 message/part/call 与 UTF-8 输出；不执行原工具，不调用宿主工具
或付费供应商，不读取真实数据库。

新增用例先检查实际模型请求声明 reader，再让 Runner 调用并在下一轮模型请求中核对
原 output ID、message/part/call、文字及终页 offset 0。固定要求模型请求恰好两轮。

- 正常旧 Simulator 库构建成功，测试编译、链接成功；真实失败于 `advertised.load()`。
- 生产修正后，只换正常新库，同一测试对象通过声明与实际读回。
- 独立审查收紧两处测试边界：handler 捕获状态先于 fixture 实例声明，清理先停止并 join；
  listener 启动后等 ready 再 send，避免早期失败时 stop 尚未生效而 join 挂住。
- 最终测试对象 SHA256：
  `5608f1ef2d2d4483ef90f7496420a7c043011c7b8cd88301904a59bb44d3516c`。
- 用该同一最终对象对既存旧库实际 exit 1 / 0.625s，对新库 exit 0 / 0.512s。
  未重建两库或扩跑其他组。

Root 已收到并阅读两轮原始测试输出，核对两份 patch 的长度、SHA256、previous/new blob、
完整两源 canonical bytes，以及私有 index 和最终 staged 的两产品树；独立审查无剩余阻断项。

## 构建与设备状态

- 首次指定设备 destination 的 xcodebuild exit 70 / 1.790s，设备不在 Xcode destination 列表；
  同时 CoreDevice details/apps 正常，连接、DDI 和开发者模式均可用。这是编译前环境阻断。
- 改用列表支持的 `generic/platform=iOS`，同 SDK、签名和 ce74 源码，
  普通 Release exit 0 / 141.296s；五个新核心对象在本轮日志有实际编译记录。
- a328 注册修正后的普通 device 增量 Release exit 0 / 21.264s。
  设备端 ready 回执确认：五个同 Core tree 的对象复用、新 Mobile 对象实际重编，
  六对象与 Core/Mobile/最终合库成员一致，最终 Ld 消费该合库；exe 存在 reader factory、
  `tool_output_read`、schema11 与执行恢复标记。
- 最终签名 exe SHA256：
  `8f9cee1a6b749b7d082afb978b30663e92eaffe954dd87c93c501a6f9d08042d`。
  Mac 状态及 ready 回执记 strict signature exit 0、profile 含目标设备、十项升级 entitlement 兼容、
  九个 Release MachO 不含 Debug seam。

### 后补证据核对

Root 已核收六份共享日志的长度和 SHA256。destination、ce74 构建和 a328 构建是脱敏副本，
清单把各自 private 原件与 shared 副本的长度、SHA 分开；final Ld、factory 符号和
安装前 4016 三份日志与所列原件同字节。实际 Ld 行包含最终合库路径，符号日志包含
已定义的 `T __Z25makeMaiToolOutputReadToolv`，两份构建日志分别记录五个 Core 源实际编译
和最后 Mobile 源重编。

Root 同时核对六份源码 canonical SHA 与 a328 提交相等，三归档共十二个 member SHA
记录与对应 object SHA 自洽。真实对象、归档、profile、签名材料和完整依赖日志仍在 Mac 私有目录，
Root 没有下载二进制重算；六份 header 依赖仅收到所需行摘段，不冒充完整 deps 原日志。
十项 entitlement 记录包括 presence 与 equal；签名旧基线为此前已验证的 205 产物，
不是本轮从设备容器导出的材料。codesign 默认静默的零字节日志仅记录空文件 SHA 和实际 exit 0，
没有非空签名日志附件。

Mac 已把同一签名 App 同卷移动到独立 ready 路径冻结，未复制整包。
装前须从 ready 路径复核签名和 exe SHA；此移动不构成安装。

## 当前阻断：物理设备断开

安装前再次读设备 app metadata 时，CoreDevice 返回 4016，物理 iPhone 从此前
wired/connected 变为 unavailable。没有执行任何 install 命令，最后实际读回的现装版本仍是
`2.2.4 / 205`；没有 build206 的装后版本或启动 PID。

4016 原日志的 `CurrentlyAssertableStates` 为空，表示当前不满足所需的 CoreDevice 状态；
`unavailable` 分类来自 Mac 的设备状态回报，不仅凭错误号断定数据线或具体连接故障。

Mac 已异步请用户重新接数据线并解锁；不反复查询不可用设备，不留自动 UI 等待。
a328 阶段的构建和签名已就绪，安装与实机启动验收尚未完成。

## 后续空答修正使旧候选暂停安装

随后用户报告创作中心本地模型空回复。Core `37e1381a` 和 iOS `cfed9c93` 已新增有界补答与
持久空答说明，详见 [空回复诊断](maichat-local-model-empty-reply-20261112.md)。
旧 a328 签名 build206 没有这些修正，已明确暂停安装，不能在连接恢复后直接使用。

最新产品冻结为 `3befec3df52cd6e0b6e5380739ceb4e2cbf99afe`：MaiChat tree
`29d8bfb555d5fb5fe46ca5dac1a852f2b19fcc1b`，Core tree
`30114b3e33472ba1692987e0a90df11f81d853df`。
Mac 已获指令复用原普通 device-derived cache 做必要 Release 增量，按实际构建源码记录，
不为后续 docs-only 提交重建。新 device 候选还须核对最终链接、签名/profile 和升级兼容。
连接恢复后装前读实际现装 build，再选更大的命令行 build override，保数据覆盖并核装后版本与普通启动 PID。
Simulator Release 的通过不能替代这些设备候选与装后证据。

### 3bef 新 device 候选已就绪，仍未安装

随后收到 Mac 普通 generic Release iPhoneOS/arm64 小回执：build exit 0 / 115.758s，
版本 `2.2.4 / 206`，signed exe SHA256
`c5fa4614a47cd95d4eec56403fa30d24ae30a1872adefc6ca7f7b33158e66c71`。
执行端回执报告 deep/strict 签名 exit 0、profile 含目标设备且 Team/有效期有效、
十项升级 entitlement 与既有 205 基线核等；Runner 本轮正常重编，六对象和三归档成员与实际 Ld 输入核合。
最终 exe 包含 reader factory、新 Core 收尾提示和 Swift 空答说明；九个 Mach-O 中十个测试 marker 均未出现。
这些签名、成员和二进制检查按执行端回执记录，Root 没有传输并重新计算完整二进制。
实收小回执 SHA256 为 `f9345cc67efcf20ce176d6870b1a6007df839aa7e157b6b7e5e096bb3097da5c`。

GGUF 复用硬链接且执行端核 inode 保持；新 App 同卷移动到独立 ready 路径，旧 a328 原字节仍保全，
其安装 guard 继续禁止。该轮没有查询设备、install 或 launch，最后实际现装读回仍是历史 build205。
当前状态为新候选 ready、等待连接恢复；装前仍须读实际 build，再决定是否需要更大的 override。

## 验证边界

Mobile 功能用例实际运行于普通 Simulator Native 库。真机版本、启动和链接的最终结果
须以装后设备回读及原日志为准，不能由 Simulator 通过推断。

当前主动生成 retained output 的生产工具是 shell；移动端未因此新增本地 shell，
也未宣称所有移动工具都自动产生可补读快照。reader 复用既有会话归属和存储校验。

私有核对记录位于 `.cache/tool-output-recovery-20261112/mobile-registration/`。
生产源码未增加测试专用入口；本轮不触发现场模型任务或付费请求。
