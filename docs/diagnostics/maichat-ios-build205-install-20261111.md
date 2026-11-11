# iOS 真机 build 205 覆盖安装记录

## 需求、操作与源码

用户授权将本轮上下文恢复修复安装到 iOS 真机。预期为：同一 bundle/签名身份的正常
Release 覆盖现装 2.2.4/build204，保留容器，设备回读新版本，并普通启动新 App。
设备端由 mac-maichat 独占操作，Root 复核脱敏回执和既存日志；日期为 2026-11-11，
Root 首轮附件核合时间为 12:09Z。未读取或导出真实数据库内容。

- 设备：已配对、available 的 iPhone 14 Pro Max。
- Bundle：`com.kongshang.maichat`；Marketing Version 保持 `2.2.4`。
- 本轮构建源码：`c14a7d087384fd592d7ab1e02368f21494943f14`。
- MaiChat tree：`cd1a9d0832bbe6cb3d586dc8fc0ac2d0039c4cd6`。
- Core tree：`1607a9dd61adc4e9228f09f4c967ccca71ef122d`。
- 正常 Team/Release、iPhoneOS SDK27.1、arm64；命令行覆盖 `CURRENT_PROJECT_VERSION=205`，
  仓库中的 build202 不变。SDK 与 deployment target 不作为设备实际系统版本读回。
- 后续 `15d6f985c9f7aa18cb3b55fc084813b04596fad9` 仅为文档提交，两产品树与本轮构建相同。

## 本次 Core 确实进入最终 App

Xcode 的正常 Build MaiAgent phase 每轮执行，构建当前源的 maichat_mobile/Core，随后
将 archive 合入 `DerivedSources/libMaiMobileAgent.a`，最终 App 的 Ld 消费该路径。
本轮原 build 日志实际包含 Runner/Compactor 两对象重编、Core/mobile 重链以及正常
Release App 链接和 `BUILD SUCCEEDED`；不是仅提高 build 号后复制旧 App。

Root 将五源 Git blob/canonical SHA 与 c14 Git 对象逐项核合；设备端提取的对象与两级
archive member 的 SHA 记录也逐项相等。原 final Ld 日志 SHA 为
`562509353935d7a2491fb6cea90ff5b7b2ae15bd776cc054006a944b49c9f8e9`，路径精确指向本轮合库。

| 产物 | SHA256 |
|---|---|
| MaiContextCompactor.cpp.o / 两级对应 member | `24cb7fb2a3efdf380c203eebdc205045f88aedeed3defb2a721990743ef9c087` |
| MaiTurnRunner.cpp.o / 两级对应 member | `0f4c20e0f9e751d6e4e34a80432b25bd5c42a51a013e98a6b95b29bc49af9536` |
| libmaiagent.a | `ae658cd25114953d8c4a42356008fb9a4b8818d0d52cb3a393e984e18678eafb` |
| 最终 libMaiMobileAgent.a | `1c5b9574c522d6f741a839f0cb145bc3c81e944e8cfba243a7219990febe0d5a` |
| 正常签名 App executable | `01f00a53e80643c724af0a349f863d7a4c9b22383b0c4eebab31637089a9fe18` |

设备端回执确认两个新对象、最终 executable 中三项 c14 恢复标记存在；Root 收到的是
提取后的身份回执及原构建/依赖/链接日志，未在 Windows 上持有或重新解析 iOS 二进制。

## 安装与普通启动结果

正常 Release build exit0，132.356s；deep/strict 签名检查 exit0。升级所需十项 entitlement
逐项记录 oldPresent/newPresent/equal，全部兼容；没有把两边均缺失的 key 说成具备该能力。
旧204签名 App 已由设备端独立保全，随后使用相同身份覆盖安装，exit0、56.220s。

安装后既存 CoreDevice metadata 的单 App 脱敏片段直接包含 `version=2.2.4`、
`bundleVersion=205`、目标 bundle 和 builtByDeveloper=true，outcome=success。其原命令
11:58:40.303Z 开始、0.353s/exit0；片段同时保留原完整 JSON 的1394字节及 SHA，
不是为补证重新查询设备。该读回与安全回执一致。原安装日志确认 bundle 安装成功；原普通
无参数 launch 日志成功，回执 exit0、1.482s。后续 CoreDevice 进程原日志实际读到
PID50266 与新安装 bundle URL 对应的 executable。URL 原值不公开，只保留绑定布尔及
路径 SHA；不以单次进程读回宣称长期存活或全部页面验收。

本轮没有卸载、清容器、读取/导出 DB，采用相同 bundle/签名身份的原地升级保数据方式。
未检查用户数据内容，因而不把未读取的数据逐项完整性描述为已实测。

## 既存观察边界与证据归档

初次 devicectl 将空 environment 字典拒为 CLI validation exit64，发生在实际 App launch
之前。另一条私有观察误把普通默认 TERM 当作测试注入；更正观察后，最终已成功的普通
启动无需重放。两项原观察保留，不记为产品失败，也未为补证第二次安装或启动。

证据位于 `.cache/ios-device-205-20261111/`。八份非空 shared 日志按各自长度/SHA 与
发送清单全部精确匹配；strict signature 原日志声明为0字节，Root 按空文件 SHA 重建
归档并明确标为 reconstructed，未伪称收到第九份非空附件。私有原件和脱敏 shared
字节的哈希口径分列，设备/签名标识及安装 URL 不写入本文件。

| 小回执 | 字节 | SHA256 |
|---|---:|---|
| safe-install-receipt.json | 3053 | `9c7d5b27d31096247ea48faa62d4f129df840860ff3b65edef83b4af35f2f49a` |
| native-link-consumption.json | 4523 | `5c6aa8b48209c45d57d9943e7e6fe3dd62d618f5259613507ebc123893f32ef8` |
| shared-log-index.json | 3786 | `a63503ecb7f5500f1cd84501d45599442b05a07f75683b049afcb001bae9f37e` |
| installed-app-readback-excerpt.json | 1095 | `9e020766b5db8d13867d12fc67ed273cb93f1e9aa9b13710164def6b41909167` |

本记录只核本轮真实安装、普通启动及源码消费链。没有运行付费模型或用户现场摘要恢复
任务；功能链的确定性 Core 结果见[原史恢复诊断](maichat-context-history-index-recovery-20261111.md)。
桌面 [2.2.58 正式发布](maichat-release-2.2.58-20261111.md) 的 tag、八件资产和两产品树不变。
新收到的 Markdown 表格显示问题处于独立只读排查，尚未更改产品代码或认定现场根因。
