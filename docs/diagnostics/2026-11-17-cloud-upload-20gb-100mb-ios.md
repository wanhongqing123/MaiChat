# 云盘20GB/100MB：iOS呈现、供件和生命周期

用户明确选择十进制视频20,000,000,000B、图片100,000,000B，并要求去掉常驻“仅自己可见/视频2GB/图片20MB”说明。用户确认全平台同步，分工保持house负责services/shared/Win/Android、mac负责iOS/macOS；共用逻辑选择Windows验证一次。

## 实际交付226

CloudDriveView去掉隐私/上限常驻行，只有reservedBytes>0时短显“已预留”；用量、配额条、目录和导航保留。Model错误提示从共用media_info读取实际阈值，空文件独立报错。既有正常RootView原生消费者1/0，37.786s，build+test147.175s。模拟器153.94s录屏及进入云盘前后半秒帧、dark-cloud截图已亲看，无旧提示或残留空行；测试数据不是用户真实配额。

Release-iphoneos226构建0/165.065s，deep/strict签名0，exe SHA256 8029a2cdce6be07b16a3eb8bbb85972c5dc5748ebb32eec3ea312ca3396382af。标准保数据安装0。首次后继launch/metadata遇4016且device list明确unavailable，未动配对/TCC；用户表示已核对，之后设备恢复，metadata0=3.0.1(226)、普通无参数launch0及独立进程查询0/PID71291，与新安装bundle路径一致。没有独立设备exe SHA读回。

226仅交付已完成的呈现更新，原共用2GB/20MB核心和线上服务未被冒称已经放宽。用户已收到此边界。

## 227宿主实现

Root提供C API末尾追加字段cloud_drive_thumbnail_path_utf8。原始CRLF文件SHA7a37bfbc9685ada0119a85d2300131c951394cb0fffcc8bbf658cd87c0ec4860、规范LF blob b785b14dbd6bb1366fb51b4a237a8ba444cb7783，规范LF SHA95d70e2ca765d373947052219e8c05a7687e62a0006b210dc00c6b73ebc936ac逐等后消费。只消费Root接口，没有并写共享实现；接口头不是完整功能供件。

新增Apple宿主ImageIO小图工厂，输入<=100MB、源像素<=6400万，输出JPEG两轴<=320且<=2MB。用1MB缓冲在既有checkpoint旁生成私有原图快照，JPEG和C read_at上传body读取同一不可变副本；不会改写用户原图，不把100MB整文件读入Data，不新建存储根。复制前检查磁盘预算，复制后SHA核对来源；原图快照/JPEG/小manifest只在持久完成回执或确认abort后清理。暂停、普通错误、进程退出保留，同一路径/内容续传重用；旧小图checkpoint没有host标记时维持原source模式。

普通和Agent云盘上传都复用Model.upload→Client.performUpload，同一JPEG工厂、同一共享C++ multipart，无Swift HTTP旁路。字段指针在包围整个同步CAPI调用的withCString内借用；Session/临时图和视频传nil。新图片以持久快照的read_at供原件，FileHandle读操作加锁，采用UInt64偏移。Cloud读取/下载门禁改为共用media_info策略；Session继续走独立mai_session_attachment_media_info，不扩大Session预算。

## 原生验证与发现

MaiChatCoreTests现有scheme下直接编译同一helper源，无实现副本；原生10/0（0.408s），包括大于20MB编码原图、小JPEG/原字节保留、缓存身份/损坏拒绝、暂停保留/完成清理、旧checkpoint、过大/取消先拒绝、无效图、EXIF旋转只应用一次、透明原图仅派生JPEG合成白底。实际App Debug集成构建0/93.54s，已亲看生成JPEG。

实际21MB用例曾失败：NSURL缓存旧fileSize，导致流式副本完成后被误判源变化。清除缓存后重新取resourceValues，同一例GREEN；没有放宽大小断言。早期两次测试配置失败分别是选错scheme和helper未在单元target编译，均已用既有scheme/同源target membership修复，不冒称产品回归。

## 源码/当前边界

原工作区main c3ea及旧WIP保留。20文件历史移动端候选与Root71有5个preimage差异，未将整份WIP发出当正式增量。当前云盘专属6文件包相对Root71生成：CloudView/Model/Client前像逐等，helper/test两新文件，PBX仅从Root底座投影版本/成员；51846B/SHAa5c5b52050e63cae5827153a7d957966b328c0e8b14515102088ed9712900cc3。该Root投影工程尚未另行构建；实际证据对应Mac本地已运行源。没有要求house重验iOS。

后续已消费house冻结共享5件，header原postimage跳过，其余4件逐before/after blob核等后仅应用一次。完整普通上传、Agent上传、Session与临时图、19GB虚拟Range及竞态的公共证据采用Windows结果，不重复跨平台运行。Apple侧完整Debug编译0/64.099s，实际重编Client/Vod；最终原生12/0，另包含远端thumbnail ready但本地JPEG缺失时交由共享核心决定续传的RED→GREEN，以及完成清理不等待无关FileProvider复制的RED→GREEN。

最终Release-iphoneos227构建0/118.144s，deep/strict签名0，版本3.0.1，exe SHA256为5f34e2613beb90fef227cb07e6236b4766b1ab641c5bee5f2bf27cbdf778cb81。实际构建使用保留原WIP的本地main c3ea工作区，相关共享5件和云盘宿主最终增量分别核对；不把Root71投影工程冒称独立编译产物。公开v3.0.1 Desktop八资产不变，不上传替换它们。

## 生产部署：已上线

用户20GB/100MB与全平台同步授权，以及house明确交出的唯一部署执行lease适用。外包55498B/SHA3dafe43949b17257b283809d2be7c4b1e6698bcf2d63f51e6ad83b2548a659d6核等；内部候选manifest为7da22c555613ab13f71daaaced05995d207c153f72ecae9b86313dbb4496955c，base8aefd68b，candidate52ac50e7234d。只变Auth cloud-drive-service/server/store和Signer index/server五JS；27件runtime闭包的其余22件从逐hash匹配的线上旧源复制，复用同指纹安装依赖，不安装新包、不部署整个main。

既有严格pin的SSH transport连接8.134.253.45，Node仍/usr/bin/node-22 v22.23.0。readonly预检0/2.955s，27个before全等；两库better-sqlite3 readonly online backup的quick_check均ok，备份仅留服务器0600；在独立Auth副本上练习5列和unique-partial index，未恢复或覆盖活DB。首次staging的runuser窄PATH和新目录受private umask影响的访问检查都在停止服务前失败，保留失败证据；显式既有runuser绝对路径及本任务新代码目录0755后同门禁通过，不改旧配置。

新源目录/opt/maichat/releases/cloud-drive-20gb-20261117-52ac50e7234d。按Sync→Auth→Signer停止，新增两个精确任务drop-in，Auth仅追加非秘密的video20e9 policy文件，未写quota；Signer→Auth→Sync启动。初apply退出1：Sync刚start后立即访问监听口遇短暂URLError。没有退回不识别sidecar的旧cleaner，随后独立有界健康检查及全部postimage/config/dependency核对退出0，失败事实仍在回执。

实际Auth PID173186、Signer173183；两者cwd/argv/entry SHA均为新候选。Auth进程白名单数值与同active module纯factory共同回读video20000000000、image100000000、quota500000000000；factory探针只用数值及无效合成key，不调用HTTP/DB、不读取密钥。Auth/Sync GET loopback health均200 ok:true；Signer未认证POST cloud-drive/read按合同403 Internal authorization required，该403是权限门禁正常，不是服务故障。Sync新PID173208但仍旧prepare-image-generation目录、argv与entry SHA cd1869c1069e69a66851d31d904f7659a8cd02af9233cfd957d14edb9ee01dd9，源码不变。所有既有unit/drop-in/env文件字节与安装依赖指纹不变，500GB专用drop-in保留。Session2GB/20MB不扩大。

补充审计边界：systemctl对多份EnvironmentFiles重复打印同名属性，原preflight字典只保留最后一条。因此已在任务末端保留所有重复属性再次审计：baseline捕获的unit/drop-in、canary/nls/sync env逐before SHA比较；其余既有auth/media/cloud-drive/旧flagoff env的mtime及ctime均严格早于本任务输入创建，确认任务期间未更改，但不把它们称为已捕获before SHA比较。没有输出env值或下载这些文件。实际canary也证明原控制面凭据仍正常。

## 线上精确owned canary

仅复用既有root-private canary账本中已verified的测试身份A，不创建/修改账号，不读业务DB，不借用用户登录，不使用第二身份。普通API新建一张777B JPEG原图及776B JPEG sidecar：main先complete真实409拒绝；thumb与main各自sign/PUT/ListParts/complete通过；普通列表仅一张卡，两条direct OSS读取SHA与各自样本逐等，sidecar签名不含x-oss-process。普通DELETE200/cleanup_pending=false，两条仍有效签名URL再GET均404，列表卡消失；测试会话已logout，token、ID、对象key和URL留服务器私有journal，不回传。整个Node canary0/3.034s。

没有实际传输20GB视频或100MB图片，没有把tiny canary称为大文件带宽/真机全量上传验收；大上限/分片数组与公共状态机采用house既有测试。没有第二身份线上隔离检查。安全最终回执在私有任务根deployment-final-safe-receipt.json，已发house归档；数据库和凭据未下载。

## 227真机状态

第一次标准保数据安装命令等待60.418s超时退出2，未卸载或清除数据、未改配对/TCC。随后既有设备有线连接恢复，独立apps查询0已读到com.kongshang.maichat版本3.0.1、build227、新bundle目录51C374B2-B12B-4DBD-9AD9-C0529E61EB55。这证明安装已生效，不能把初安装命令改记为0；也未独立读回设备exe SHA。普通无参数launch首次被系统明确以Locked拒绝，已请求用户解锁后继续核对启动，当前不声称启动验收完成。

## macOS宿主接通

house后继Qt宿主8件包71674B/SHAee1695aeb923ad1b8ef4ea19772d98ac759e3ac9fd039c00f49323f51592084d，在private71源码副本逐before/after blob与LF SHA核等消费一次；未覆盖原工作区WIP。正常maichat target构建、重链、TRTC嵌入与模型资源门禁0/48.417s。公共Dialog/Agent/helper行为采用house Windows证据，未重复运行。macOS现装和公开3.0.1附件不变，不将构建声明为新Mac现场上传验收。house已聚合推送feature commit e1222952a62134899d233ca4ed69fea88ef340fa；已消费的最终增量不再重复叠加。

私有证据 /private/tmp/maichat-cloud-upload-limits-20261117，含Root只读摘要/接口/设计、前像、候选patch/manifest、UIKit录屏截图、unit失败/成功结果、AppDebug构建、真机安装/metadata/进程回执。

## 真机解锁后最终核对

2026-11-17后继检查lockState passcodeRequired=false；正常无参数launch退出0，PID71713。独立进程查询退出0，exe路径为新安装51C374B2-B12B-4DBD-9AD9-C0529E61EB55/MaiChat.app/MaiChat；另一次独立apps查询退出0，版本3.0.1/build227，bundle与进程路径对应。既有安装超时与Locked失败证据保持，未复装、卸载、清数据或改配对/TCC。最终安装及正常启动核对完成；无设备exe SHA读回或20GB/100MB真机全量实传声明。完整私有回执cloud-upload-completed-platform-receipt.json及device227-completed.json。
