# prepare_image generation：最窄两服务执行 runbook

2026-11-15。仅 source-only 准备，当前未获部署模式，以下远端写入步骤均未执行。
Root 统筹供件与模式；Mac 使用已验证的现成本机 SSH 通路执行，不转交凭据。

## 基线与边界

- 目标8.134.253.45；既存 hostkey 严格验证、en0 只读 SSH 实际0。
- 原 release：/opt/maichat/releases/session-temp-images-20261110-8cde6a87。
- Auth：active/PID160473，真实 cwd=原release/auth，入口server.js。
- Sync：active/PID160474，真实 cwd=原release/auth，入口session-sync-server.js。
- Media：active/PID151848，真实 cwd=原release/media，入口server.js。
- Root 已将实际磁盘JS与Repo38480181逐canonical核等；完整bytes/SHA在已供 confirmed-current-route-safe.json。
- 必须分清 Auth store.js=87de4c3f…与Sync session-sync-store.js=5e85f858…。
- 旧/opt/maichat/auth等同名路径不是当前进程cwd，不能用作上线指纹。
- 只切Auth+Sync。Media代码/unit/运行目录、OSS生命周期/RAM、密钥、featuregates和白名单不动。
- 新表/列只additive；缺省g0继续，旧receipt/旧publication/旧到期身份不可变。不扫历史、不改用户record、不调用模型。

## 执行入口：缺一即停

Root须提供精确commit、bundle bytes/SHA、preimage/new逐文件SHA、Node/C++ tests和独立review、
显式JS白名单（Auth3JS、Sync1JS及最终必要的Sync入口cap文件，以具体清单为准）、
运行依赖固定来源、两库精确路径、migration入口、唯一新release/drop-in路径、
source-only canary驱动/owned资源/gate许可、失败回滚策略及explicit执行模式。

未收到以上输入，不能scp、建远端release/backup、访问生产DB、切源或restart。
canary必需模式若预先没有gate许可，必须在切源前停报；不请用户再作含糊确认。

## 顺序与停点

| 阶段 | 允许动作/必须证据 | 停点 |
| --- | --- | --- |
| 1 本地核件 | 验commit/bundle/白名单旧新SHA；拒绝路径逃逸、链接、非白名单代码、env/DB/key载荷和安装hook | preimage不符或review不完整即停 |
| 2 远端只读预检 | 重核Auth/Sync/Media Active/PID/cwd、过滤后的JS入口、Node实路径、unit/drop-in指纹及依赖 | 基线漂移、停Auth会牵连Media、缺DB精确路径即停 |
| 3 新目录组装 | 仅创建新的release/auth；只放显式JS及获准的不变依赖；node_modules若复用须固定旧release只读来源；不npm install | 不覆盖原目录/全局current；Node --check及逐文件回读不符即停 |
| 4 服务端私有备份 | root专用0700任务目录、umask077；原unit/drop-in原样私有保存；两库分别SQLite online backup至唯一0600文件，quick_check=ok和SHA | 不传本地、不查用户行、不打印env；失败backup标incomplete即停 |
| 5 migration预演 | 仅在服务端备份副本执行最终批准入口；schema/完整性检查，旧码仍能用additive后的schema | 需要重写旧receipt/用户record或旧码不可回滚即停 |
| 6 两服务切源 | 先停Sync再停Auth；安装本任务两drop-in、daemon-reload；先start Auth/health，再start Sync/health | 需新PID、真实cwd/JS入口/磁盘SHA核新source；Media PID/cwd/代码不应改变 |
| 7 owned canary | 仅批准账号/新Room/message/part/tinyPNG，精确协议来自最终供件 | 403/未授权gate精确停报，不开放名单/改gate/绕Auth |
| 8 收尾 | 只用正常API清理精确owned资源；保服务端私有回执、失败/未运行事实 | 清理不确定留恢复记录，不报ready或全绿 |

组装时保证两服务完整运行import链；不变JS/package/dependency也要有允许清单和旧指纹。
不能整包复制仓库/通配scp，不能把环境文件带入新release。
只语法检查，不在预检阶段启动真正服务入口或触发供应商调用。

## 备份、切源与回滚的关键细节

- SQLite使用online backup API，source以mode=ro读取，不能immutable=1、cp活库或单拷WAL。
  只执行backup、备份quick_check和文件散列，不SELECT业务表，不输出错误行内容。
  可审source helper见services/auth/scripts/prepare-image-generation-backup.py；无CLI执行入口。
- 两库分别在线自洽，不宣称跨库同一事务快照。备份只留服务器，不自动拿旧DB覆盖新写入。
- 备份unit/drop-in可能含敏感配置，只私有复制；不用systemctl cat/status或全量journal输出。
  只输出安全systemctl属性/HTTP状态/路径/指纹，既有EnvironmentFile/用户/权限/gates保持。
- 两个唯一task-owned drop-in同时设置新WorkingDirectory；若ExecStart原为旧绝对路径，
  必须重置到原Node实路径、已审非敏感参数和新JS入口。若是相对入口，也核真实解析路径。
  不修改原主unit或其它drop-in。后续模式未授权前，不执行这些操作。
- 切源失败时先stop Sync再stop Auth，核非本任务unit/drop-in仍等于备份；
  只撤本任务已核新SHA的drop-in，daemon-reload，按Auth→Sync恢复原cwd/入口/代码SHA并核health。
  发现其它操作者变动则停报，不整目录覆盖。
- 正常回滚只回代码/两服务入口，保留additive schema与新记录；不还原DB、不删新表/用户行。
  数据恢复属另项操作，必须单独明确决策。保留失败release与私有备份。

## canary最小成功条件

1. 批准驱动使用owned新资源，并具备最终新cap；旧bool connector不得冒cap。
2. 正常控制链形成一代g，按批准客户端失败/取消方式得到failed，再显式按需单图prepare_image得到g+1。
   不SQL改状态，不造未实现API，不修改生产clock或等待24h。
3. Auth可信代映射只创建本次唯一新对象；旧g/g0 receipt及身份不改；迟到旧回执不能冒认新代。
4. 真实tinyPNG字节走client→OSS，读取后比bytes/SHA；服务端仅control metadata，不代理媒体字节或模型。
5. 旧expired/default0/immutable的时钟边界由已通过的offline Node测试承担；
   在线failed→g+1不能冒称完整24h旧expired现场验收。
6. gate拒绝时只记精确阶段/错误码；已切源则按预先批准策略保持健康待验或代码回滚，不临场扩权。
   owned清理不确定留私有恢复记录，禁止列桶/扫历史/手删DB行。

## 最小回执

实际commit/bundle/白名单、explicit模式、每步exit/时间、旧新/回滚PID与cwd/入口/SHA、
服务端备份完成指纹、schema-only/health结果、canary范围/清理与未验边界。
所有前置失败单列；没有真正续期不能报ready。

当前只完成旧通路只读核验与本runbook/source helper准备；未scp、restart或访问生产数据库。
