# MaiChat 2.2.58 正式发布核准

## 需求、源码与公开状态

用户要求利用 session 数据库保存的原文回查，恢复摘要失败后的本轮执行，减少交互中断。
本次承接已授权的 Windows/Mac 出包和 GitHub Release 发布。

- 冻结源码：`c14a7d087384fd592d7ab1e02368f21494943f14`。
- MaiChat tree：`cd1a9d0832bbe6cb3d586dc8fc0ac2d0039c4cd6`。
- Core tree：`1607a9dd61adc4e9228f09f4c967ccca71ef122d`。
- Annotated tag：`aa5af711a7defe49473c011152bf44204080f8f0`，peeled 精确等于冻结源码。
- Release id：441590927；2026-11-11T11:32:18Z 发布，draft=false、prerelease=false，Latest 同 ID。
- [正式下载](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.58)。本文件为发布后的文档记录，产品源码和正式资产保持冻结。

## 复现、修复与验证

[原史索引恢复诊断](maichat-context-history-index-recovery-20261111.md) 记录现场需求对应的
最小复现、根因和完整守卫。原生产行为在硬预算区摘要格式失败后结束 Runner；修复后，
具备真实同会话 history 工具的请求可生成确定性的部分原史索引，按准确 ID 查数据库，
沿当前工具循环继续。原始记录保留，实现不主动重放已完成工具。

真实 Core Runner、自有 store、正式 session_history 和计数工具的同例先 RED 后 GREEN：
旧源 PID48644/exit1，SessionError=1、原文读取=0；新源 PID40604/exit0，SessionError=0、
原文读取=1，并生成最终回答；两个动作前后均各执行一次。后续测试从实际模型请求中的
索引解析定位信息，再通过正式 history 工具读取，未从 raw store 私下代喂 ID。

另核无工具最终 hint 的完整预算、索引输入预算与语义摘要输出预算的区别，以及旧检查点、
来源变化、取消、存储、无 history 能力等边界。完整 Compactor PID21800/exit0；normal
13 目标 build PID31500/exit0；必要 13 组 ctest PID49080，13/13 PASS、124.18s。
Compactor exe SHA256 为
`4ce9930248c9cb0f50488ff958bb09092ec80a2db94adb0152b8c7ccdd88e6f7`。
Mac 同一冻结源码的完整 Compactor 定点 build0/12.439s、CTest0/3.314s。

## 正式包验收

- Windows 正常 configure/build 分别 exit0、8.473s/15.921s，部署与 PE gate 通过后才运行
  新包。实际 ZIP CRC 通过，81 文件与便携 staging 全 SHA 同；NSIS 实际提取的 81 应用
  文件与安装器 staging 全 SHA 同，另 6 件为预期安装器支援文件。两 staging 仅各自
  使用说明不同，各自归档副本完整相等。
- Windows 三份 PE closure 为 37/37/42，missing 为 0；五模型精确。六次普通/插件调试
  bare-PATH 私有 profile smoke PID 为 41072、27300、22020、30000、33584、22952，均
  exit0，实际 2.2.58/Qt6.12.0，五个 Qt 模块来自对应副本。exe SHA 为
  `e228c029d259259efd2bfa952161e361f99484e6ab027a77e7b0c2d5f190ceba`。
- Mac staging、只读 DMG、实际 ZIP 三副本的 338 files + 37 symlinks 全同；26 目标
  Mach-O、五模型、strict 签名、DMG 布局和 ZIP CRC 均通过。六个真实 Cocoa 普通/调试
  进程 PID 为 67039、67202、67603、67760、68239、68391，均 exit0，实际 2.2.58/Qt6.12.0，
  六个 Qt/Cocoa 模块来自各自 bundle。六份原日志按长度/SHA、PID、版本和完整路径核准。
- Mac source exe SHA 为 `dd246d90aafd31b2066caf24ea92ce95add7d7c68f1a9d48a655f2e73256d97f`；
  signed exe SHA 为 `9d29c92ffdb37b719ad359d7fda9d581186b8f38b5e0ee00526c96dcdc434588`。
- Windows 实测 Windows11 x64，主程序和安装器 NotSigned；安装器只提取未执行。
  Mac 实测/交付 macOS27.2+ arm64，ad-hoc、未公证，不声称旧系统运行兼容。Mac 四份
  独立旧 docs 造成的 OpenCVGitVersion dirty 字样已如实记入 manifest，产品和 real index
  clean；源码身份由冻结 SHA、两产品树和实际产物 SHA 共同核准。

## 上传、发布与公开回读

Mac 四件先串行 POST，driver exit0 后 Windows 四件串行 POST，0 重试、0 覆盖。
8/8 服务器 name/size/digest/state 精确后 publish+latest。四份 metadata 在 draft 和公开
browser_download_url 分别实际下载，长度/SHA 与原件相同；五模型跨平台完全一致。

Root 在 11:32:59Z、Mac 在 11:33:18Z 分别无登录核准公开 page、expanded assets、API、
Latest、四份 metadata 下载和 annotated tag。两份回执逐项交叉核合，旧 v2.2.57 正文、
8 件资产身份和 tag 未变。完整本地证据在 `.cache/release-2.2.58-20261111/`，
可提交摘要及回执 SHA 见同名 JSON。

| 资产 | 字节 | SHA256 |
|---|---:|---|
| MaiChat-macos-arm64-v2.2.58-20261111-c14a7d08.dmg | 214030691 | `6c0aed1da7b71ee6ea2970736dbef7721797fea3e2b89bad231c26f902c7c08c` |
| MaiChat-macos-arm64-v2.2.58-20261111-c14a7d08.zip | 214628061 | `8cd847eff3147ea563c92009c37e01c419b154f3235f87cc0286bbb432d95cac` |
| MaiChat-Setup-win64-v2.2.58-c14a7d08.exe | 201134448 | `54110ddae14cdab40de2643714a2bd5f8f64cafa6c9bfd9dd6d1e8ba04f2125e` |
| MaiChat-win64-v2.2.58-20261111-c14a7d08.zip | 241604380 | `ce28461f0f16b67d089ba7fb66657b1b3f9de994c38761494f2c74728e25ddb3` |
| release-manifest-macos-arm64.json | 4110 | `e96e1bd481a8a0edec068cd745b2e50451c619e877be7fecfcf268cbd4beabe0` |
| release-manifest-win64.json | 11401 | `d4ccc0c2d67fd79e15f121a8d94fc2bca91a20395a6670caee515817343d4593` |
| SHA256SUMS-macos-arm64.txt | 332 | `3aac7204938b6bd3f452eb148cccdbb74c71ef0f6032f0398d867d88469f7d65` |
| SHA256SUMS-win64.txt | 314 | `1d9db5346917ec094020a96f349fa4ae321685f17df58b28951cfc71877ff07a` |

## 验证边界与生效方式

用户需在实际执行任务的 Windows/Mac 端更新到 2.2.58。桌面包门禁未替换现装、读取真实
用户数据库、调用付费供应商或部署服务。后续已授权的 iOS 真机安装按独立交付记录处理；
此前 UI/Host/Native/mobile 的结果保持原源码批次，不合并成一次全平台实机验收。

确定性 fixture 验证了正式准备、索引、工具和数据库的执行链，不能保证真实模型每次都
正确选择回查。授权、取消、网络、存储、来源冲突，以及必保内容仍无法装入硬预算的情况
继续明确报错；本次修复不表示任意错误均可恢复，也不承诺原生输入交付 exactly-once。
