# prepare_image generation 控制面实际执行记录

2026-11-15（Asia/Shanghai）。最终指定服务执行状态：PASS_WITH_EXACT_RECOVERY。两服务新源健康、owned canary功能及精确清理完成；初次失败事实保留，不扩大为客户端全部现场验收。

## 授权与来源

Root明确模式AUTH_SYNC_GUARDED_CUTOVER_AND_OWNED_CANARY，authorized=true。
mode9247B/SHA6b2e43f11539ae04b7ec57f16a6479b453cca31a14105ea90fb946bc16b5d696。
source57b92599e1e67b3f6698d6a30ff5e2e6d43a7c5d，
source-only包37267B/SHA5e69151ed8398b617c086d36eb7f3d4c100adcb54fa710e4ee2ae124bbc9da89。
final driver35224B/a023c3de6c5e86c40ddc2357fed6c4e43d61d365a1a884f269e43cb5bb836643；
plan12306B/f9e7788f94e56a0f501b5d277717208cd8a20498cc278971b51eb4c4f817f7a0。
独立final review5794B/6d71d3241d08cc0114a11b97201cd9f7d9f33c370c7e23ace469704639586cdc。

SSH经既存本机任务身份、严格钉IP hostkey、en0；不转凭据。实际执行日志仅safe字段回传。

## 已执行阶段

- A：旧25runtime rawSHA、Node22.23.0、进程恰Node+JS的argv、Linux安装依赖树58986308…与两库元数据全部匹配，exit0/1.465s。仅创建批准的新release、0700backup/canary目录。
- 白名单SCP4件exit0，B最终driver --preflight exit0/1.134s：源pin、现gates和精确owned IDs通过，不调用/auth/me、不造账号。
- C：新runtime25文件、依赖树、语法和两服务身份读/遍历通过；11个原unit/drop-in私有备份。Auth onlinebackup1884160B/da308ff2…，Sync34070528B/30a7ac14…，服务器0600，未传DB或unit内容。只在独立Auth备份副本执行精确1006B/9520c2c0…一表两索引DDL事务及PRAGMA；Sync无DDL。原备份SHA未变，exit0/7.360s。未调用openStore/daemon做预演。
- D：先停Sync/Auth，只加批准的两个99drop-in，Auth先start/health再Sync；exit0/3.541s。
- 新Auth PID165340、新Sync PID165353，cwd和JS入口均在/opt/maichat/releases/prepare-image-generation-20261115-57b92599/auth。25新SHA、source marker、原非task配置全核等，两health200。
- Media PID151848、旧cwd/source/配置保持，未重启或改代码；不改OSS/RAM/lifecycle/gates/白名单。未恢复数据库。

## canary与未完成清理

E --execute exit1/37.002s，19个safe功能check全部true：g0 pending/complete/ready/failed、g1 ACK/可信代/complete/ready、旧receipt不改、固定lifetime、去重/跨账号/读取/hash、旧引用拒绝等。
两代tinyPNG实际client→OSS上传/读取hash同ee77cbc4d58e7c1ddce76e1757b4dc60f9a155014f62a66e2a1b1113f683d83e。
不改生产时钟，不宣称真实24h旧expired现场或Desktop prepare命令已验。

清理首次cleanup_unproven；按同source/plan/frozen driver执行一次--recover，仍exit1/2.844s。
按授权保持健康candidate和server-private recovery journal，不回滚/不盲重复，不执行旧源candidate recovery、不禁用未验证账号。
F最终只读exit0/.868s：两服务仍健康/newcwd+25SHA，Media与非task配置不变。

按Root精确白名单从journal仅读safe布尔：room_removed=true；
g0/g1均attachment_known/object_key_known/complete_requested/complete_confirmed=true；
g0 delete_requested=true/metadata_removed=true，object_absent和uploads_absent未证；
g1 delete_requested=false，三清理项均未证。没有回传ID/key/login/密码/token/URL/raw body。

Root已定位frozen driver media verify适配缺expected_bytes与content_type=image/png：真实Media合同离线400/0HEAD，补参200 exists:false/1HEAD。
等待独立recovery-only companion的SHA/addendum，只修验收适配，不改原driver/plan/journal plan hash/serviceJS/Media/gates，不创建资源。

原失败保留，不把cleanup_pending报PASS。服务端私有备份/journal不得下载。本地安全回执在/private/tmp/maichat-generation-execute-57b92599-caamq11b；后续精确恢复结果另补。

## 精确一次companion恢复与最终回读

Root给出recovery-only companion10647B/SHA9d3a80e2b9ea861668dd84832568bf171b34d1768fae95f04ddfe0777fb9cd6f、
addendum481B/36e7aa8d5b94ee8ba382cf666323ebb29b58ea5f4c09ad19ef56d7952ef54d45，
独立审阅4929B/82523a2866af11282da1a50b6f098e35058540665184a5a231e0203e3a7a15f4 PASS。
只白名单传新两件，原a023 driver/f9 plan不变；server-only0600复制当前journal到本任务唯一新名，未传内容。

P私有前置/备份0/.776s；Q一次companion恢复Node0/6.017s（SSH0/7.312s）。
复用原cleanup，仅补Media verify要求的expected_bytes/content_type，不运行runFlow、不造资源。
wrapper每save核原scope/plan_sha256/ID/gen/init/receipt保持。
status=recovered，七个cleanup布尔全部true，19功能check仍全部true。
旧failure_code cleanup_unproven因历史保留，不能把它当当前失败，也不能删除它假装初次全0。

Z最终只读0/.838s，Auth165340/Sync165353健康、新57b真实cwd/JS、25runtime SHA与非task配置全核；
Media仍151848、旧cwd/代码未变。没有回滚或DB恢复。
完整safe执行回执在private执行任务complete-safe-execution-receipt.json，已回传Root。
实际24h旧expired时钟/真实Desktop prepare命令未验；服务器owned canary不能替代各客户端旧图整链验收。
