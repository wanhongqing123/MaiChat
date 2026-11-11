# MaiChat 2.2.57 正式发布核准

## 需求、源码与公开状态

用户授权 Windows/Mac 新包及推送 GitHub Release，并强调摘要报错造成任务暂停。
本次统一交付摘要格式与软区恢复、远端工具详情及此前桌面修正。

- 冻结源码：`f360d0d46b3401582370319c48261628f3863be0`。
- MaiChat tree：`8c35d599aa66b4f2e4c6cf29221fe2eed249f9e3`。
- Core tree：`fda19e6be48bc4de88f6985e2fa99eeedf44033e`。
- Annotated tag：`b86fb689e04940d9a50d62f4b433a60389fe0193`，peeled 精确等于冻结源码。
- Release id：441434249；2026-11-11T09:11:15Z 发布，draft=false、prerelease=false，Latest 同 ID。
- [正式下载](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.57)。本文件为发布后的 docs-only 记录，不是另一份产品源码。

## 复现、修复与功能边界

[摘要软区恢复记录](maichat-context-soft-budget-recovery-20261111.md) 保存旧源行为 RED、
实际预算与同 Runner GREEN。原请求仍适配时只对摘要 Protocol 失败暂缓整理，继续当前
工具循环；已有工具不被实现主动重放。后续每次请求重算预算及 usage 倍率，成功推进
检查点后恢复软阈值整理。硬超限、取消、网络、存储及来源变化保持失败。

Windows normal Core 必要 13/13 PASS（167.61s）；Mac 同源完整 Compactor PASS（2.927s）。
已有 UI/Host/Native、Android Java 与 iOS UIKit 结果保留各自源码批次。此前 Mac Host
全组 89/1/1 与同环境原单槽 3/0 分列，失败原因未证实。

## 正式包验收

- Windows 正常 configure/build、windeployqt 与 PE gate 成功。实际 ZIP 81 文件与便携
  staging 全 SHA 同；实际 NSIS 提取的 81 应用文件与安装器 staging 全 SHA 同，另 6 件
  为预期安装器支援文件。两类 staging 仅各自使用说明不同，未扩大豁免。
- 三份 Windows PE closure 为 37/37/42，无 missing/retired Qt import；五模型精确。
  六次裸系统 PATH、私有 profile 普通/插件调试 smoke 均 exit0，PID 为
  41164、43844、47188、47332、46840、48660；均回读 2.2.57/Qt6.12，五个 Qt 模块来自
  对应包内。exe SHA `27caae84124c805de99f368bbae1a45a955caf5762bf3a1bde7566c51c502b49`。
- Mac staging、只读 DMG、实际 ZIP 三副本 338 files + 37 symlinks 全同；严格签名、
  26 目标 Mach-O、五模型、DMG 布局和 ZIP CRC 均通过。六次 Cocoa smoke PID 为
  22532、22723、23130、23225、23728、23826，均回读 2.2.57/Qt6.12.0，六模块来自各自
  bundle。首份汇总缺最后一例 dyld 和版本原文，补发六份既存 raw log 后逐长度/SHA、
  PID、版本和模块路径核合，无重跑或改包。
- Mac source exe SHA `b292eaa6fedbca52c334a7162a20fc627fce7895f88eabb43bb66c794c0c4ec4`；
  signed exe SHA `b7d195c9496b3f474d3434e4bf5faf68cab8f583c09be478fcf539fdca6eaa6d`。
- Windows 实测 Windows11 x64，主程序及安装器 NotSigned，安装器只提取未执行。
  Mac 实测/交付 macOS27.2+ arm64，ad-hoc、未公证，不声称 14.4 运行兼容。Mac 四份
  独立旧 docs 导致的 OpenCVGitVersion dirty 字样已如实记入 manifest，产品与 real index
  clean；构建与产物身份由冻结 SHA、两产品树、实际进程和文件 SHA 共同核准。

## 上传、发布及公开回读

Mac 四件先串行 POST，Windows 四件随后串行 POST，不覆盖正确资产。8/8 服务器
name/size/digest/state 精确后才 publish+latest。四份 metadata 在 draft 和公开
browser_download_url 分别实际下载，与原件逐字节/长度/SHA 同；五模型跨平台一致。

Root 无登录、无 cookie 回读公开 API、Release page、expanded assets、四份下载及
annotated tag 全部通过，Mac 又独立无登录核准。旧 v2.2.56 的正文、8 资产身份和 tag
未变。内部完整回执在 `.cache/release-2.2.57-20261111/`，公开摘要见同名 JSON。

| 资产 | 字节 | SHA256 |
|---|---:|---|
| MaiChat-macos-arm64-v2.2.57-20261111-f360d0d4.dmg | 214030741 | `5f652de25b228d7f444ecec069e6527263f42d0f64b7a7078291a064aeca8e22` |
| MaiChat-macos-arm64-v2.2.57-20261111-f360d0d4.zip | 214611833 | `b04750c595b4af0a0d610af2558fad1ae643ef48a3e0d495d66e97f143672e33` |
| MaiChat-Setup-win64-v2.2.57-f360d0d4.exe | 201138461 | `f8b83b212b0b7e593999f09dac6c2739a3b9abad8a4de13ca80d82266f391421` |
| MaiChat-win64-v2.2.57-20261111-f360d0d4.zip | 241598327 | `87f7978a683cbcc12f93f145513d4cfba8624de457611f94d5b256a1769b455c` |
| release-manifest-macos-arm64.json | 4109 | `73a2ed3de40dbc9f1fddd18369d16455f380de817325880a8084fdfb02a66cf2` |
| release-manifest-win64.json | 11401 | `54d5e4b49096eebf780241457dd73adc6262bf72833da5d28cbf266d94547a9a` |
| SHA256SUMS-macos-arm64.txt | 332 | `91afbf2342ded077101a5294aa5b99da7e1e96d037a7e5970899a04cbf147f57` |
| SHA256SUMS-win64.txt | 314 | `12c2c200617760430b95f1328d46df4b0de23a022b99dd173f19a21fc6ec826e` |

本次未替换桌面现装、未更新手机包、未操作原现场进程或部署线上服务。用户需更新到
2.2.57 才会使用新代码。未读取真实用户数据库或调用付费供应商，真实供应商现场、
PPT/QQ 自主任务及线上跨设备 WS 不由包门禁替代。
