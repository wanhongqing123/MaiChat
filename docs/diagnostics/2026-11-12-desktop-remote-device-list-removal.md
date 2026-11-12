# 桌面远程桌面移除连入设备名单

- 时间：2026-11-12 14:02 +08:00。
- 平台：Windows 11、Qt 6.12.0、MSVC 2022；用户提供的设置截图来自 Mac Qt 桌面。
- 源码基线：`37e1381addf4a58e8a11ca41d6ee2126148a5fed`，验证时叠加本文所述 Desktop 未提交修改。
- 最终产品提交：`3befec3df52cd6e0b6e5380739ceb4e2cbf99afe`，MaiChat tree `29d8bfb555d5fb5fe46ca5dac1a852f2b19fcc1b`；Core tree `30114b3e33472ba1692987e0a90df11f81d853df`。
- 构建与运行产物：`MaiChat/desktop/.cache/build-qt6-ui-20261109/bin/` 中重新构建的远程桌面测试与 `main_window_layout_test.exe`，以及后述私有候选 App；未启动或替换用户现装 App，未接触真实账号数据库、远端设备和服务部署。

## 操作与预期

1. 打开“我的”设置，滚动到“远程桌面”：不再显示“允许连入的设备”、设备名单及其“编辑”入口；被控模式、访问密码、远程控制与 TRTC 代理仍可见。
2. 已有配置曾保存 `allowedUserIds: ["company-iphone"]` 时，其他有 IM 发起身份的设备请求连接，应由被控模式和已有访问密码决定结果：每次确认模式弹窗，无人值守模式遵循密码设置，关闭模式拒绝。
3. 空身份、失效会话输入或非当前对端的鼠标键盘输入继续被丢弃。

## 根因与 RED 证据

旧链路为设置页名单 → `RemoteDesktopSettings.allowedUserIds` → `isSenderAllowed()` → `HostInviteInput.senderAllowed` → `decideOnInvite()` 拒绝。名单判断发生在被控模式之前，正确访问密码无法绕过它。

修改生产代码前运行新增定点用例，确认：

- 无名单的正确密码邀请返回 Reject；每次确认模式也未发出确认请求（controller 2 个失败）。
- 旧名单字段保存后仍写回 JSON（settings 1 个失败）。
- 实际 MainWindow 设置组件仍显示名单相关内容（UI 1 个失败）。

日志保存在构建目录 `tmp/remote-device-list-{controller,settings,ui}-red.txt`。旧实现的空身份不共享用例同时通过，证明此约束需要在移除名单后单独保留。

## 修改与验证

移除名单字段、读写、匹配函数、状态机拒绝分支、设置行和编辑函数；旧配置中的名单字段被忽略，下次保存时不再写出。更新设置提示与相关旧注释。Controller 单独拒绝空白发起身份，保留被控模式、可选密码证明及失败降级、远程控制开关和既有会话/对端匹配。

重新构建并运行五套完整远程桌面测试：controller 29、settings 9、session 21、auth 13、signal 15 项通过，合计 87 项，0 失败。MainWindow 名单移除及原账号设置导航两个用例通过（含初始化/清理共 4 项）。GREEN 日志为同一目录下 `remote-device-list-*-green.txt`。

Windows 原生 Qt 设置面板截图为 `tmp/remote-device-list-settings-green.png`，已实际查看：名单行消失，剩余四组设置完整。UI 测试输出一个已有 `QFont::setPointSize(-1)` 警告；本次未修改字体逻辑。编译输出的 `MainWindow.cpp` 既有 C4858 警告也未扩大处理。

这是本机构建及实际 Qt 组件验证；Mac 验证记录见后续章节。尚未安装替换用户 App 或进行跨设备远程连接验收，不能视为完成这些链路的实机验收。

## 同链审查发现：忙碌拒绝改写原会话

审查发现基线中已存在的独立问题：`handleInvite()` 即使取得 Busy Reject 决策，仍无条件写入新邀请的 `hostSessionId_` 和 `hostRoomId_`。本次名单删除没有引入该赋值，但多设备邀请会经过同一链路，因此同步最小修复。

修改前新增两个精确时序用例并运行，均失败：

1. A 请求进入每次确认，B 随后请求被忙碌拒绝，再确认 A：实际进入 `room-b`，预期 `room-a`。
2. A 已共享，B 请求被忙碌拒绝，再发送 A 的输入与 stop：输入记录为空，stop 未调用引擎，状态仍停在 Sharing。

RED 日志：`tmp/remote-device-list-busy-tuple-red.txt`。修复将邀请拒绝回复放在接管 host 会话上下文之前，并直接使用被拒邀请自己的 session ID；只有需要确认或接受的邀请才写入 host tuple。原会话的用户确认拒绝仍沿用原 `applyHostDecision()` 路径。

修复后只重新构建并完整运行 controller 套件：31 项通过、0 失败（`tmp/remote-device-list-busy-tuple-controller-green.txt`）。其他已通过的测试和 UI 未重复运行；本节没有扩大协议或输入授权规则。

## Windows 普通 App 与包启动

2026-11-12 14:25–14:29 +08:00，在精确 3bef 源码复用上述普通 Release cache 构建 `maichat`，
exit 0，post-build windeployqt 完成；PE 门禁覆盖 3 binaries、114 imports 和 Qt plugins。
build、完整 staging、实际 ZIP 解压后三份 exe 均为 165797888 bytes，SHA256
`98c62d231a2fda5ece4857d563da1faa35ac6dc9ab2468ddea36ac3e5101ad3a`。
staging 与 ZIP 的 81 个文件逐 SHA 相同；ZIP 为 241653560 bytes，SHA256
`29c535871971de35ff340c83afa533689524c6e18c3e2e5b9835c83ca16d7b1e`。

两包分别在仅含 Windows/System32 的 PATH 下运行普通 `--smoke`，PID 46796、47072，均 exit 0，
启动日志实读 2.2.58 / Qt 6.12.0。五个实际加载的 Qt 模块（Core、Gui、Widgets、Svg、qwindows）
逐项来自对应包内。候选证据保留在 `.cache/local-model-empty-20261112/desktop-candidate/`
的 `build-receipt.json`、`candidate-gates.json`、`staging-smoke.json`、`zip-smoke.json` 及原日志。

私有 smoke 脚本仅增加新子进程的 USERPROFILE 重定位，配合 APPDATA/LOCALAPPDATA；
启动日志实际路径均核准在候选目录的 smoke-profiles 内。未读、复制或清理旧 profile/log，未恢复账号、
安装或发布。普通 Windows/Qt 启动不宣称零用户注册表写入；这些 smoke 也不代表远端真实连接验收。

## Mac 普通 App 与 Cocoa 验证

执行端核对 3bef 的十二份 Desktop 源码 canonical SHA/blob、plain patch 及两产品树后，
复用原普通 Qt 6.12 cache 构建 App 和四个测试 target，exit 0 / 88.112s。
App 版本为 2.2.58，exe SHA256 为
`bf38076853eb1eec3f893a99d5dac30fc316e7579d02f5ebd41e731264c73e33`，本轮未额外运行 Mac App smoke。

测试使用真实 Cocoa、各自私有 CF home 和原 sandbox：controller 31/0（7436ms），
session 21/0（1ms），settings 9/0（341ms）；MainWindow 仅上述两个指定用例及 hooks，4/0（1074ms）。
四份原 txt 均在 Windows 独立读回，长度/SHA 与执行端索引逐项相同，Totals 均为零失败。
两张实收设置图已亲看：远程桌面设备名单行消失，模式、密码、远控、代理完整；
账号设置页没有诊断字段。IM 实收图仅用于显示核对，不宣称与执行端原 PNG 逐字节相同。

该轮使用 Fake IM/TRTC 与真实 widgets，没有实际远程连接或用户输入注入。
没有部署、安装、现装 App/设备操作或版本、发布改动；跨设备远程连接仍未验收。
