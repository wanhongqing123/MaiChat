# MaiChat 2.2.59 发布执行

用户已明确授权“iOS 安装到真机，windows/mac 发送到 Github Release”。
Root 唯一协调版本、tag、draft、平台串行上传窗口、发布和公开回读，并持有 Windows 出包。
Mac 端唯一持有 Mac 供件及 iOS 真机构建/保数据安装；本授权不扩为退出或替换 Mac 现装。

1. 以 `76185fba4df003927d3d4499b656d3e888ab5357` 的已验业务源码为基线，
   只将 Desktop CMake 版本 2.2.58 上调 2.2.59，提交并冻结完整 SHA/各产品树。
   现有正式 v2.2.58 Release 441590927、c14a7d08 源 tag 和八件资产保留。
2. Windows 复用 normal Qt6.12/MSVC cache，完成版本必要 configure/App 增量、部署和 PE 门禁。
   新鲜目录生成 Setup EXE/portable ZIP；实际提取 NSIS/ZIP，三副本文件、依赖、模型核同。
   staging/真实 ZIP/真实安装器提取各做普通及插件调试 bare-PATH 私有 profile smoke，
   核版本、实际 PID/路径、包内模块与源产物 SHA；不运行安装器或替换用户程序。
3. Mac 同一冻结 SHA/2.2.59，复用原 normal cache；生成 arm64 DMG/ZIP，核 staging、
   只读挂载 DMG、真实 ZIP 三副本文件/symlink、strict 签名、依赖、五模型与六个真实 Cocoa smoke。
   允许同卷 clone 节省空间，不新增大 SDK/cache、不物理复制模型；不替换 Mac 现装。
4. 每平台四件：两份二进制归档 + release-manifest-平台.json + SHA256SUMS-平台.txt。
   二进制文件名含真实版本、实际日期及冻结短 SHA；metadata 固定完整源/树、实际长度/SHA、
   平台/架构/签名边界及真实包门禁，不添加 Electron metadata。sums 覆盖两件归档和 manifest。
5. Root 创建 annotated v2.2.59 tag 和同源 draft，上传平台窗口互斥。每次 POST 结束后 GET
   核 name/size/digest/state；传输异常先确认服务端状态，不覆盖已正确资产，不并发发布写入。
6. 八件服务器摘要、四份 metadata 实际下载、跨平台模型及 peeled tag 全等后，正式 publish/latest。
   再无登录核公开页、API/latest、八资产/metadata 和旧 v2.2.58 未变；返回正式下载链接。
7. iOS 从最新 Core/iOS 产品树做 normal Release，读取真实连接/现装 build 后使用更大 override，
   校验签名/权限兼容，保数据覆盖并普通启动，读回版本、PID/产物。旧 206 候选不冒充最新。

发布说明区分代码/组件回归、真实网络与模型现场边界：未完成的双账号绑定 E2E、手机现场本地模型空答
根因及未复核的 Tencent 19 quota pending 不写成全部解决。已验 255/94、Core/iOS 各组保持原源码批次；
版本行变动不为发布重复旧业务广组，但包/版本/签名/实际归档及运行门禁必须重做。
500GB 云盘为已完成服务配置，重发桌面包不再次重启服务、不改变单文件/会话附件配额。
