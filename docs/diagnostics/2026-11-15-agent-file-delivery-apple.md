# agent_send_file 与指定好友文件发送

日期2026-11-14至15；Apple macOS27.2 arm64、Qt6.12、iOS26.5自有模拟器2440。代码待统一提交；未替换现装 Mac。此前211仅含Wan video暂停，曾普通launch Locked。当前212已正常覆盖安装与启动；真实双账号联网功能未验。

## 用户操作与合同

在当前创作中心让 Agent 交付工作区已有文件，agent_send_file(file_path, caption) 显示文件卡片，含真实 basename、大小、说明。PDF点击复用本地预览，其它类型沿已有预览/分享/另存为，不自动执行，不发文件字节给模型、不带peer_id。

Core普通 maiPrepareAgentFile 与工具共用实现：可信 workspace 内相对路径、物理链接守卫、非空普通文件≤512MiB。七字段 delivery=current_ai_session、type=file、path、mime_type、bytes、caption、name。原 agent_send_media 枚举不扩。

指定好友 maichat_send_media(type=file) 复用普通持久 sendFile，保留 basename/MIME/bytes/caption，沿现有1GiB门禁。两者大小是 App 既有门禁，不冒称 SDK 套餐；Android PDF既有100MiB预览限制保留。

## Core与付费工具

Apple正常普通Core八套25命令exit0，含文件普通API、独立工具/注册、四图像族测试；供应商/opt-in live环境已清，拒绝公网，只用离线或loopback。Wan video及五付费图像暂停由协调端独占提供，已逐previous/new/canonical核合。后续UTF8窄边界只重跑两个受影响Core套compile/link/run均0，不扩大无关组。

POSIX ASCII symlink指向非UTF8 basename：APFS raw open拒绝errno92 EILSEQ/exit77，Apple无法造referent，绝不记为漏洞复现。协调端Android自有NDK CLI用同普通API/factory真实RED：direct success且path/name含FF；tool抛json::type_error316。加入解析后严格UTF8校验，回InvalidInput，不改通用Path/MIME或原文件；Android同CLI实际GREEN，direct_invalid_input=1、tool_returned/error/invalid_input=1，own目录/entry/binary清理核验。Apple条件fixture保明确SKIP，原失败保留。

## Mac卡片与独立复核

普通Core工具、内存Fake模型实际产PDF/ZIP，normal UI打开既有native PDFKit预览、切历史、缺文件点击变不可用。初版功能3/0截图却显示两行被单行QPushButton高度裁掉；亲看后改布局最小高度与透明label，最终静态QWidget PNG已查看。

独立review两个P2均用原正常UI RED：A点击校验后切B，回A按钮永久disabled；同ID换workspace仍可开old root。仅AgentChatPanel与原测试窄修：launch/finished重取ownerSession rawdir与capturedroot；存活A卡元数据即使暂不可见仍回填，preview必须当前A。原2例+校验中换目录+原卡片全Qt6/0，host原file正/负与image回归Qt5/0。MainWindow只公开原预览普通入口，不加全局root限制，不改IM sender范围。

## iOS取消与显示

普通宿主file正/负例经过正常App+签名runner，0实际IM/provider。独立review指出原文件队列首尾checkCancellation导致复制后URL丢失、取消父任务清理又在入口被拒。copy完成后与prepared/SDK前两门闩在正常App均RED，未派发副本真实遗留。

仅file准备持有副本的独立任务复用原文件队列，父cancel传播本调用的Cancellation盒；保住URL后清理，不改全局submit语义。派发前清理独立于已取消父任务；已交普通sendFile的SDK失败行附件保留。相同2例GREEN，原friend槽也GREEN，实际3/0。旧helper构建前提失败和未签名Simulator启动失败另保，不能称所有历史尝试都0。

iOS当前AI卡片复用IM FileBubbleContent与既有本地预览/分享，普通Core路径策略2例、正常App卡片/PDF开关例均0，已亲看最终clean截图。独立review的caption/浮点bytes边界，用原始JSON1.0/1e0/精度折叠fraction与六String缺失/非String得12条真实RED；PathPolicy和原parser窄补，same+原2共3/0。无字典转码规范化假测。

## 交付边界

源码基线3908+逐exact overlay；安全ff到38252959后保全61个本地文件和原index，先前构建不倒填新HEAD。Android显示我方2源经协调端原JVM跨host修正，最终2f6d56b4/7949b2d4精确合回；14/0归属协调端离线JVM，不是我方或Android真机卡片验收。

新Native纠正18源与Swift薄消费已过必要同库/宿主验证后，正常Release212一次合并图像暂停、文件和绑定。公开260/261不变，视频R3独立HOLD。原证在私有maichat-agent-send-file、ios-file-send、paid-image-apple、native-cas-apple、device212任务目录，manifest供协调端核对。


## 212真机安装事实

2026-11-15正常generic Release212构建exit0/141.811s。source实际38252959d52412a4126aba212b97bd3c42e1803d + exact overlay；不等价声称未来统一commit。原五子树73输入inventory SHA256 c3a9cd25b058628d85a4c3b213da3c118beff69586c4e77174ecce66d4b32f95。

原核验遗漏五子树之外的Shared MaiChat/MaiChatTools/MaiMobileHostTools.cpp，首wrapper1保留。它实际在构建前的6072B独立供件中冻结，补充该旧freeze精确SHA/bytes/blob核对后verifier0；不改原73清单或倒填当时来源，不因证据修正重编产品。全部29个正常原生对象与实际最终合库成员逐SHA相同；9个Release MachO无Debug seams，10类升级签名字段相同，profile/strict正常，模型inode/bytes复用。

exe SHA256 0fb27d2ee269992434830f1548fd7b0ba674026f99b07226ab30bce853b6242c。fresh实际211→212安装exit0/13.358s；安装后版本2.2.4/212，普通launch0/.477s、PID61216，进程回读0/.391s且executable URL匹配新安装bundle。无fixture参数/环境，无卸载、容器或DB读取。仅证明正常签名更新与进程，双账号联网回发及付费供应商E2E未验。

最后corrected Native32/0（8.754s）、Shared18/0（.489s）、真实Mobile loopback request0（.626s，agent_send_file=1，Wan/fiveimage=0）；受影响Core5普通同库compile/link/run全部0。原Swift Host新增activity必须走独立SDK permit断言，新wire与原Host同正常App签名runner2/0（7.468s/6.965s）。测试support include/link与错误大小写target前提失败单列，不称所有历史尝试均0。
