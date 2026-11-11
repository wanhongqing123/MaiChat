# MaiChat 2.2.57 发布执行

用户已授权新包及 GitHub Release。Root 独占版本、冻结 SHA、tag/draft/body/publish 和
Windows 资产；Mac 协作者仅制作和上传同一 draft 的 Mac 四件资产。直接在 main 操作。

1. 完成摘要软区自动继续的行为 RED→GREEN 与独立 review；仅增加已明确的安全容错。
2. Desktop 版本 2.2.57，保持手机版本、SDK、依赖、编译参数及部署目标不变。提交并冻结
   完整 SHA/产品树；此前 v2.2.56 的 tag、正文、资产及旧候选目录保留。
3. Windows 复用 normal cache，正式 configure/build 完成 deploy/PE 后再运行 App。
   新 staging、实际 ZIP 解压和 NSIS 解包逐文件/PE/五模型核对，三份各普通/调试裸 PATH
   私有 profile 启动，核进程、版本、exe SHA 和包内 Qt 路径；不执行安装器。
4. Mac 使用同 SHA 正常 configure/build、必要 Core 定点和正式 package 脚本。staging、
   只读 DMG 和实际 ZIP 的文件/symlink/签名/Mach-O/模型检查及六次隔离 Cocoa 启动。
5. 两平台各完整安装/归档两件加 manifest/sums，共八件上传同 draft；逐名核 server
   state、size、digest，下载四份小元数据核原字节。出现传输错误先查询服务器，不覆盖
   正确同名资产、不并发同名上传，不通过 IM 发送 ZIP 或分片。
6. 八件全部核准后发布为 latest；无登录回读公开页、latest/tag/精确 source 与所有资产，
   核旧 v2.2.56 正文、tag 和资产不变。补最终诊断回执，发送正式下载链接。

已知范围：真实模型语义质量、实际用户电脑上的自主任务、Android UI/手机更新和线上
跨设备 WS 不由桌面包门禁替代。此前 Mac Host 全组 89/1/1 与同环境原单槽 3/0 保持分列，
不借发布冒称已解决该一次失败。发布不替换现装、不读取真实 DB、不做线上服务部署。
