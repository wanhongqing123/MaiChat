# MaiChat 2.2.58 发布执行

本次承接用户已授权的桌面修复和 GitHub 发布。用户追加要求：利用 session 数据库原文
回查完成摘要失败后的恢复，不因可恢复的整理错误直接结束任务。Root 独占版本、冻结
SHA、tag/draft/body/publish 和 Windows 上传；Mac 仅制作及上传同一 draft 的 Mac 四件。

1. 按 context-history-index-recovery 计划完成旧生产行为 RED、新恢复 GREEN、能力及
   预算边界和独立 review；normal Core 必要 13 组通过后统一提交、冻结完整 SHA/两树。
2. Desktop 2.2.58。既有 v2.2.57 tag、正文、8 资产和旧候选目录保留；不移动正式 tag。
3. Windows 复用正常 cache/SDK/VS 参数，deploy/PE 完成后才运行 App。独立 fresh staging、
   实际 ZIP/NSIS 提取逐文件、三份 PE、五模型和六次私有 bare-PATH smoke 全核。
4. Mac 同源码正常 Compactor 定点、configure/App/package；staging、只读 DMG、实际 ZIP
   三副本逐文件/symlink/签名/Mach-O/模型和六个真实 Cocoa 进程及模块路径逐项核对。
5. 每平台安装/归档两件加 manifest/sums，各 4 件。普通附件只用于小供件/回执，不通过
   IM 发 ZIP/分片。大件平台串行交接 POST 锁，不覆盖正确同名资产，传输异常先 GET。
6. 8/8 server name/size/digest/state、4 metadata 实际下载、跨平台五模型和 tag peeled
   核准后 publish+latest；无登录公开 page/API/全部资产/metadata/tag 回读，旧 v257不变。
7. 收口发布诊断及 docs-only 提交，保持产品树与正式包精确同，发送正式下载链接。

边界：不读取真实用户数据库，不调用付费供应商，不替换现装，不部署服务。不把确定性
fixture 当作模型现场一定自动检索的保证；真实授权/存储/取消/网络和保留内容仍不能放下
的错误不伪造成功，不无限重试。此前各 UI/Native/Host/mobile 的源码批次与未验项保持原状。

## 执行结果（2026-11-11）

上述步骤已完成。冻结源码 `c14a7d087384fd592d7ab1e02368f21494943f14`，MaiChat tree
`cd1a9d0832bbe6cb3d586dc8fc0ac2d0039c4cd6`、Core tree
`1607a9dd61adc4e9228f09f4c967ccca71ef122d`。Core 必要 13/13 PASS（124.18s）；
Windows/Mac 正式包、归档实际提取、依赖、五模型、各六次包内运行门禁均通过。

Annotated v2.2.58 `aa5af711a7defe49473c011152bf44204080f8f0` 精确 peeled 到冻结源码。
Release 441590927 于 2026-11-11T11:32:18Z 正式发布并为 Latest；8/8 资产和四份公开
metadata 下载经 Root/Mac 独立无登录核准，旧 v2.2.57 保持不变。正式下载链接已发用户。

[发布诊断](../../diagnostics/maichat-release-2.2.58-20261111.md) 及同名 JSON 保存资产、
功能与包门禁、公开回读和验证边界。本次收口只提交文档，产品树、tag、正式资产不变。
后续用户授权的 iOS 真机安装另列交付记录，不改写本次桌面发布验收。
