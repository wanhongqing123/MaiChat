# 移动网络兼容审计：已连接但无互联网的 Wi-Fi 与可用蜂窝网络

审计时间：2026-11-27 09:50 CST。工作区基线：
`be9711aeac49e2be0c1c2f37069cc910ff90fc90`，含未提交工作。
本记录仅核对当前源码、现有构建配置与官方资料，没有修改生产代码、构建或安装产物，
没有切换设备网络、访问凭据或读取用户数据库；不阻断进行中的 Android APK 安装。

## 问题与验收目标

复现条件应是：手机仍连接指定 Wi-Fi，该接入点的局域网可用但公网出口不可用；
手机已具备系统允许使用的 4G/5G 数据连接。分别在发起请求前与 SSE 已开始后切断 Wi-Fi
公网出口，观察普通互联网请求及流式会话。

预期新请求使用系统允许、适合目标的可用互联网路径；不能仅凭 Wi-Fi 已连接就认定有网。
保持 Wi-Fi 开启和局域网能力，保留 VPN、系统蜂窝授权、省流量/低数据模式及计费约束。
不能为了请求成功绕开 VPN、强制打开蜂窝、全进程绑定网络，或盲目重发可能已受理的 POST。
流式中断与新请求选路是两个验收项，不能用重新生成成功宣称原 SSE 已无缝续传。

## 当前实现证据

| 位置 | 已实现行为与边界 |
| --- | --- |
| `MaiChat/android/MaiChat/app/src/main/AndroidManifest.xml` | 已声明 INTERNET、ACCESS_NETWORK_STATE、ACCESS_WIFI_STATE。所查产品源未发现 ConnectivityManager 默认网络回调、VALIDATED 判断、Network 网络句柄选择或全进程绑定。 |
| `MaiChat/ios/MaiChat/MaiChat/Assistant/AIAssistantModel.swift`、`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp:658` | 原生宿主把请求交给同一 C++ 模型核心。所查生产源码未发现 NWPathMonitor 或 URLSession 网络策略；URLSession 引用来自 DebugSupport/UITests，不能当产品能力。WebKit、IM SDK 等各自平台传输不等同于模型 curl 传输。 |
| `MaiAgent/Agent/MaiResponsesClient.cpp:1156–1237` | Responses 使用 libcurl POST、SSE 回调、连接期限与取消回调；没有逐请求网络句柄、套接字绑定或专属网络 DNS 回调。默认由操作系统/libcurl 路由。 |
| `MaiAgent/include/MaiModelClient.h:319–342` | 默认连接期限 20 秒、无总时长上限、传输无活动期限 120 秒、无模型进展期限 900 秒；恢复次数默认 2。期限及重试可以配置，不是“网络已经可用”的检测。 |
| `MaiAgent/Agent/MaiModelConnectionPool.cpp:47–59` | 成功租约用 curl_easy_reset 清除借用选项，再保留 easy handle；空闲池最多 4。连接及 TLS 缓存可以继续使用，未按移动网络代次隔离。 |
| `MaiChat/Network/MaiCurlDownload.cpp:336–409` | 附件下载共用 libcurl、HTTPS、逐跳代理查询、连接/传输/低速期限及取消；GET 下载完成后才交付文件。该路径也没有移动网络选择回调。 |
| `MaiChat/Network/MaiCloudDriveClient.cpp:1218–1250` | 分片签名/PUT 有有界重试和 checkpoint。它是上传业务恢复，不能推广成所有付费 POST 的安全重放规则。 |
| `.cache/arm64-mobile-file-contract-20261127/build/CMakeCache.txt` | 当前 Android 私有构建配置为 ENABLE_THREADED_RESOLVER=ON、ENABLE_ARES=OFF、OpenSSL=ON；这是所查构建配置，不代表所有历史安装包。随附 curl `lib/asyn-thread.c:302–319` 在解析线程调用 Curl_getaddrinfo_ex。 |

在所查产品路径未发现 CURLOPT_INTERFACE、CURLOPT_OPENSOCKETFUNCTION、
CURLOPT_SOCKOPTFUNCTION、android_setsocknetwork、android_getaddrinfofornetwork 的接线。
因此当前实现可能在系统已经把默认网络切到蜂窝时正常工作，但没有本项目证据保证：
用户保留无网 Wi-Fi、OEM 默认网络选择不同、存在 VPN 或路径已变化时，所有新请求都能恢复。
本轮尚未对这组网络条件做真机复现，不能把源码缺口直接写成已证实的设备根因。

## 官方 API 能力与不能作出的推断

Android 的 INTERNET 表示网络配置具备互联网用途，VALIDATED 表示系统探测到公网可达，
但后者也不保证某个供应商域名永远可达。新连接通常走应用的系统默认网络；网络变化应从
NetworkCallback 的能力回调读取，不把 Wi-Fi 类型当作可达性、速度或非计费证明。
默认网络可能是 VPN，普通 NetworkRequest 的默认 NOT_VPN 也不能被误用于绕过它。
[Android 网络状态文档](https://developer.android.com/develop/connectivity/network-ops/reading-network-state)

若确需逐请求指定系统允许的非默认网络，NDK 已有 android_getaddrinfofornetwork 和
android_setsocknetwork，均从 API 23 可用；前者用该网络的 DNS，后者绑定单个 socket。
android_setprocnetwork/bindProcessToNetwork 会影响后续全进程连接与解析，不适合本任务。
[Android NDK Networking](https://developer.android.com/ndk/reference/group/networking)

不能只给 curl 设置接口名就宣称 DNS 与 socket 已走同一网络。当前构建没有 c-ares，
curl 的 DNS_INTERFACE 选项要求该后端；普通接口选择也不等于 Android Network、VPN 或
系统数据策略。可用套接字回调在 connect 前配置实际 socket，但 DNS、代理目标与连接池
仍需一致处理。[curl DNS_INTERFACE](https://curl.se/libcurl/c/CURLOPT_DNS_INTERFACE.html)、
[curl SOCKOPTFUNCTION](https://curl.se/libcurl/c/CURLOPT_SOCKOPTFUNCTION.html)

iOS 的 NWPathMonitor 适合状态展示和诊断，不能证明特定请求成功，也不能靠其状态禁止
用户尝试 Retry。Apple 建议描述请求策略后实际连接，避免用可达性预检作成功保证。
[Apple：适应网络变化](https://developer.apple.com/videos/play/tech-talks/111378/)

URLSession 的 waitsForConnectivity 只针对建连等待；连接建立后再断开仍返回错误。
它不会自动作用于当前 libcurl 请求。蜂窝、昂贵网络及 Low Data Mode 是 URLSession 的
独立策略；不能为了回退全部放开用户限制。
[waitsForConnectivity](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/waitsforconnectivity)、
[allowsConstrainedNetworkAccess](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/allowsconstrainednetworkaccess)

Apple 说明 URLSession 的 Wi-Fi Assist 在任务开始时选择路径；开始后不会据此在两种无线
网络间迁移原连接。MPTCP 另需 entitlement、配置及服务端支持，不能作为现有 curl/SSE
的无条件修复承诺。旧版 Apple 网络文档亦指出 BSD socket 不具备高层 API 自动激活蜂窝/
按需 VPN 的全部能力。[Apple：网络可靠性与 Multipath TCP](https://developer.apple.com/documentation/foundation/improving-network-reliability-using-multipath-tcp)、
[Apple：Socket 与 Socket Stream](https://developer.apple.com/library/archive/documentation/NetworkingInternet/Conceptual/NetworkingTopics/Articles/UsingSocketsandSocketStreams.html)

## 单独风险：已有部分流恢复可以重新发送模型 POST

这不是本轮新增行为，也没有在本轮修改：

1. `MaiResponsesClient.cpp:1043` 将连接、解析、收发、超时和空响应等 curl 错误列为可恢复
   传输错误；业务层同时排除部分鉴权、额度等确定失败。
2. `MaiResponsesClient.cpp:1271` 将 sawOutput 写入 retryAdvice。直接 stream 调用在
   `1349` 仅允许“无输出且未由调用者管理重试”的尝试自行恢复。
3. `MaiTurnRunner.cpp:1020` 设置 callerManagesRetries=true；`1507–1547` 按建议及剩余
   次数退避并重新请求，那里没有 `!advice->sawOutput` 条件。所以 Agent 路径可以在已有
   部分响应后再次发送 POST，不能描述成“有输出必不重发”。
4. 请求完成必须收到供应商终态；无终态不能由 EOF 推断成功。未完成正文仍是草稿，
   真实 item.done 独立保留；没有成功的完整工具批次不会因恢复而擅自执行旧工具。
   这些保护避免伪造完成和重复工具副作用，但不能证明供应商没有接受或计费上一请求。

sawOutput 也不是“请求尚未送达”的证明：`MaiResponsesClient.cpp:735` 明确把
response.created/in_progress 当协议进展，不置 sawOutput。供应商可能已经接收整份 POST
而尚未输出文字。当前请求头构造 `1156–1168` 没有通用 Idempotency-Key；本路径亦没有
X-Client-Request-Id 去重契约。即使以后增加相关 ID，它也只是关联标识，除非供应商明确
承诺并验证其幂等语义，不能据此自动重投。

### 请求是否已发出的证据边界

| 证据 | 能说明什么 | 不能说明什么 |
| --- | --- | --- |
| 未进入传输、DNS/连接阶段明确失败且本次尝试没有发出应用请求 | 可作为请求前路径选择失败的依据 | 单看一个通用 curl 错误码或没有模型文字，不够证明这一条件 |
| CURLINFO_REQUEST_SIZE / HEADER_OUT 阶段计数 | 本次 curl 尝试已经发出 HTTP 请求信息；重定向时可能累计多请求 | 不能证明源站已受理、完成或计费；代理握手须与源请求区分 |
| CURLINFO_SIZE_UPLOAD_T、XFERINFO uploadNow | 已上传的字节量/进展，可辅助标记请求发送阶段 | 零值不能单独证明未发请求；请求头可能已发，回调也不等于远端确认 |
| HTTP 状态、响应头/正文、response.created、增量事件 | 已有服务端/协议进展，是否终态另行校验 | 状态码为零或没有收到响应，不代表服务端未执行 |
| 供应商任务 ID、可查询请求 ID、幂等键契约 | 可按供应商正式能力查询既有任务或恢复 | ID 名字本身不是去重保证，不能从客户端推造 exactly-once |

curl 将 REQUEST_SIZE 定义为已发请求总量，将 SIZE_UPLOAD_T 定义为上传字节量；
RESPONSE_CODE 为零仅说明未收到服务端状态码。DEBUGFUNCTION 可观察阶段但会收到敏感
头及数据，若后续增加定点观测只能保留计数/枚举，不能记录 Authorization、正文、签名
URL 或原始 debug 数据。[REQUEST_SIZE](https://curl.se/libcurl/c/CURLINFO_REQUEST_SIZE.html)、
[SIZE_UPLOAD_T](https://curl.se/libcurl/c/CURLINFO_SIZE_UPLOAD_T.html)、
[RESPONSE_CODE](https://curl.se/libcurl/c/CURLINFO_RESPONSE_CODE.html)、
[DEBUGFUNCTION](https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html)

## 推荐最小实施顺序：先 Android，保持单一传输核心

本节是后续实施建议，不是已实现或已验证功能。

第一步只补 Android 系统默认网络观测与请求前状态，接到共享网络边界。优先使用系统
选择的 validated/default 网络，系统默认是 VPN 时保留 VPN，不按 transport=cellular
强抢网络。不用 bindProcessToNetwork，不切 Wi-Fi，不唤起被用户禁用的蜂窝，不改变
全局代理/DNS，不靠公共探测网址决定供应商 POST 可以重投。无可用路径时显示等待/失败，
保持取消和用户恢复入口；VALIDATED 是提示与选择依据，不是请求成功门禁。

若设备复现证明系统默认确实滞留于无网 Wi-Fi，再在同一普通网络核心中补逐请求网络
租约：只选系统许可、数据策略允许且不会绕过 VPN 的已可用网络。非默认网络可能有
额外平台许可/后台限制，不能因看见它就认定能使用。DNS 与新 socket 使用同一网络句柄，
保留域名、SNI、证书和代理语义；IPv4/IPv6 都须覆盖。没有安全、允许的路径就明确失败。

网络代次或路由变化时，空闲 curl 连接/DNS 缓存不能跨网络错误复用；按代次隔离或丢弃
旧空闲句柄。当前仍在途的 SSE 不因一次状态通知被主动杀掉重发，租约结束时再回收。
旧连接与新网络不能仅靠改 socket 回调混用，因为复用连接时不一定再触发创建回调。

iOS 先补状态可见性与错误归因，不承诺强制迁移 curl 的接口，也不把 NWPathMonitor
当选择器。如果真机证据要求高层系统网络能力，应评估将平台传输后端正式接入同一请求/
流式/取消核心，使用系统策略和 URLSession 等公开接口；不是旁边再跑第二套模型请求。
这不是本轮的最小 Android 修改，也不能以临时 URLSession 预热或探测请求保证 curl 选路。

统一禁止新增“断流就换网重发 POST”的传输重试层。普通只读 GET 可按既有取消与预算
恢复；上传只按受控 checkpoint/业务幂等规则恢复。模型及付费生成只在可验证的请求前
失败、供应商明确幂等/续传合同，或用户明确恢复时启动下一尝试。已有媒体任务优先查询
原任务 ID，不重新提交生成。在途 SSE 断开后保留已完成内容、未知结果及恢复提示；
用户再次恢复也不能把未确认的上一任务宣传成未执行。

具体候选文件：Android `MainActivity.java`/原生宿主生命周期、`MaiAgentJni.cpp`，
`MaiChat/MobileAgentAdapter/MaiMobileAgent.cpp` 的可信配置入口，
`MaiAgent/Agent/MaiResponsesClient.cpp`、`MaiModelConnectionPool.cpp` 及
`MaiChat/Network/` 的共用传输接线。不要只修某一个工具或在各业务模块各写一套选路。
iOS 入口为 `Assistant/AIAssistantModel.swift` 与共享网络宿主；具体传输迁移须另行设计验收。

## 可执行验证矩阵

这些均为待执行步骤，必须由主执行代理安排真机与受控网络；本审计没有实施网络切换。
测试使用本地生成的合成会话、受控 HTTPS/SSE 服务和无费用假任务，不发送付费请求。

| 场景 | 操作与观察 | 通过条件 |
| --- | --- | --- |
| 基准 | Wi-Fi 与蜂窝均可用；发起合成 GET、SSE | 默认/代理/VPN 路径符合系统策略；SSE 正常终态 |
| 请求前无网 Wi-Fi | 保持 Wi-Fi AP 与局域网连接，只关闭其 WAN；蜂窝保持系统允许；再发新请求 | 记录默认网络能力变化，新连接走允许的可用路径或明确等待；不要求用户关闭 Wi-Fi |
| 蜂窝不可使用 | 在前一场景中由验收者关闭 App 蜂窝授权或施加系统策略 | 不尝试越权选路；可取消、可恢复，无无限重试 |
| 捕获门户/DNS 故障 | 受控 Wi-Fi 分别模拟门户、DNS 失败、公网黑洞 | 区分 connected/INTERNET/VALIDATED 与实际目标错误，不把门户页面当模型响应 |
| DNS 与 socket 一致 | 新请求选择网络后查询受控域名，覆盖 A/AAAA、IPv6-only 与代理目标 | DNS 与连接在同一路由上下文；证书域名验证不变，无固定 IP 或 TLS 绕过 |
| VPN | 分别启用普通 VPN、按需/始终开启策略；切换 Wi-Fi WAN 状态 | 不直连绕 VPN；不可用时失败/等待，而不是绑定底层蜂窝逃逸 |
| 局域网并用 | 无网 Wi-Fi 上保留 LAN 设备访问，同时发普通互联网请求 | 不关闭 Wi-Fi或全进程绑蜂窝；LAN 不被公网选路策略误伤 |
| 低数据/计费限制 | 验收者启用 Low Data Mode/Data Saver/后台限制；区分主动发送与后台大下载 | 不修改系统设置或强行放行；遵守宿主数据策略并给明确状态 |
| 新请求与缓存 | 完成一请求后改变默认网络，再发同域名请求 | 旧空闲连接与 DNS 不被错误复用；不影响仍可工作的在途请求 |
| POST 已收未出首事件 | 假服务器记下 POST 后暂不回 SSE，再中断连接 | 记录“可能已受理”，不因 sawOutput=false 无条件再次提交；对当前版本另记已有自动重试实际次数 |
| SSE 已部分输出 | 假服务器发送 response.created、文字、item.done 后断开 | 不声称完成或无缝迁移；保留已完成项；逐次计数请求，暴露已有 Runner 重发风险 |
| 媒体任务已创建 | 假服务返回任务 ID 后网络中断 | 新网络仅查询原任务；没有新的生成提交，取消与未知状态不伪造成功 |
| 生命周期 | 网络抖动时后台/前台、换账号、取消；Android 与 iOS 分别验收 | 回调注销、租约归属、取消和晚到结果正确；没有跨会话误投 |

每次记录时间、平台/OS、设备与实际安装版本、源码/库/安装产物 hash、系统默认网络的
非敏感能力摘要、合成服务请求计数、curl 阶段计数、HTTP/SSE 终态与用户可见结果。
不记录 SSID、用户正文、密钥、完整 URL、真实照片/数据库或完整网络调试包。
只有通过对应目标平台与实际安装构建的场景，才能声称该场景完成实机验收。
