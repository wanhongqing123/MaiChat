# Android 无网 Wi-Fi 与蜂窝逐请求路由实现记录

日期：2026-11-27。此记录只描述本次源码实现及定点验证；APK、实机和受控 WAN 场景由主执行任务记录。

源码基线：`be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加当前 main 未提交工作；独立测试产物 `.cache/MaiCurlNetworkRouteTests.exe`，不能等同于已安装 APK 的验收。

## 可复现操作与预期结果

保留 Wi-Fi 与局域网连接，只断开 AP 的公网 WAN；系统默认网络仍为未验证 Wi-Fi，且系统已提供经过验证、允许本应用使用的蜂窝网络。前台发起新的 Responses 请求、共享附件下载或云盘操作：使用同一蜂窝句柄进行 DNS 与 socket 绑定。默认网络已经可用、VPN、代理、数据限制、后台或 LAN 目标保留系统路径。无候选网络时仍允许普通请求尝试，不把 VALIDATED 当成成功闸门。

不得关闭 Wi-Fi、请求唤醒蜂窝、绑定全进程、跳过 TLS、在换网通知后重新发送已有 POST；尚未连接的旧候选租约遇到 VPN/策略撤销时不得绑定旧网络。

## 实现与证据

- `MaiAndroidNetworkRouter` 仅注册默认网络及可见网络回调，不调用 requestNetwork 或 bindProcessToNetwork。只选择已验证、无限制、非 VPN、可用且未阻止的蜂窝；代理、Data Saver 和后台状态禁止主动备选。已有 INTERNET 与 ACCESS_NETWORK_STATE 足够，没有新增权限。
- `MaiCurlNetworkRoute` 是 Responses 和共享下载/云盘共用的唯一实现。Android 后端使用 android_getaddrinfofornetwork 与 android_setsocknetwork，保留原 URL、SNI 和证书校验。IPv4、IPv6 地址由 CURLOPT_RESOLVE 注入；数字地址不需要 DNS。每个重定向跳单独重新选择。
- 本地名称、私有/链路本地/回环地址、映射 IPv6 及默认 DNS 解析到内网的名称保留默认网络。默认 DNS 分类最多等待一秒，备选解析纳入连接预算；最多四个后台解析，等待可取消，超时任务不持有调用方或 curl。
- 网络代次变化丢弃空闲模型句柄，旧在途租约仅在归还时关闭。备选连接使用 fresh/forbid-reuse，避免复用默认网络 socket。socket 绑定前在发布锁内重新验证候选仍获准；策略撤销直接失败。
- Responses 在一次失败尝试期间发现代次变化时不产生 retryAdvice，覆盖“POST 已接收但无文本”和部分输出场景；成功终态仍按原语义完成。备选网络逐跳重定向不向其他来源发送 Authorization。
- 云盘已有显式 first-party DoH 路由优先保留，未被新的备选策略覆盖；普通 Java HttpURLConnection 不在本次共享 curl 接线范围内。主执行任务随后将实时语音 WebSocket 握手接入同一租约；它独占句柄直到录音结束，保留 fresh-connect，清除会关闭升级连接的 forbid-reuse。

## 已运行验证

1. Windows 独立编译 `MaiCurlNetworkRouteTests`，链接仓库既有 curl/OpenSSL 构建产物：通过。覆盖 DNS 与 socket 句柄一致、绑定回调实际执行、发布后旧租约不变、绑定前策略撤销不调用旧后端、IPv4/IPv6/映射地址与 LAN 保留、DNS 失败、超时、取消、代理及 WSS 默认 443 握手回调。绑定测试在 connect 前主动失败，不产生外部业务流量。独立重跑脚本 `.cache/android-network-route-standalone.ps1`，日志 `.cache/android-network-route-standalone.log`。
2. `AndroidNetworkRoutePolicyTest`：javac + JUnit 4.13.2，两个选择矩阵测试通过。先观察缺失策略类的编译失败，再加入策略并运行通过。
3. Android API 26 arm64 NDK clang：新共享路由、连接池、Responses、JNI、共享下载、云盘六个编译单元语法检查通过；Java router/policy 使用 Android 34 SDK 编译通过。
4. `MaiNetworkRouteModelTests`：假服务端接收完整 POST 后发布网络变化，覆盖调用方管理/直接管理重试、未输出、部分输出和终态六种组合。已在原 `.cache/arm64-mobile-file-contract-20261127/build` 正常移动 CMake 图上以 `-j3` 重编当前 Core/Mobile/route 并链接成功，同时构建 `MaiCurlNetworkRouteTests`、`MaiMobileAgentTests`。仅沿用原私有测试图已批准的 37 个第三方/嵌入 CLI 产物，没有导入旧 Core/Mobile ABI 产物；测试目标补齐明确的 curl 依赖。`network-route-tests-provenance.json` 记录原图哈希、沿用产物哈希及当前源码哈希，`network-route-native-build.log` 记录最终 239/239 成功。`deploy-network-route/` 提供三份 ELF 与六份非系统共享依赖，`network-route-deploy-manifest.json` 记录 SHA、NEEDED 与源码/归档证据。六组假 POST 及 `--context-usage-only` 的实机执行由主执行任务运行，本记录不提前宣称结果。
5. 新增 `AndroidNetworkRouteInstrumentedTest`：必须显式提供 `networkProbeUrl`（受控无费用 HTTPS GET）；可加 `requireAlternateNetwork=true` 要求真实备选路由。使用现有 SystemCertificateBundle 导出当前平台根证书，保留 TLS 校验；只输出 curl 码、是否备选、DNS/socket 次数和代次变化，不输出地址、SSID、令牌、正文或完整 URL。

## 验证边界

本分工没有运行 Gradle、完整主原生构建或 ADB，没有切换实机 Wi-Fi/WAN/VPN/蜂窝策略。现有可用 Wi-Fi 上成功 GET 只能验证默认路径，不能证明“默认仍停留在无网 Wi-Fi 时自动改走蜂窝”。受控 WAN、IPv6-only、VPN/始终开启、Data Saver/UID 阻止、代理、LAN 并用及 SSE 切网矩阵仍需对应设备环境验收。选择 VALIDATED 只表示系统判断该网络可用，不能保证供应商请求成功。

参考官方契约：[Android NDK Networking](https://developer.android.com/ndk/reference/group/networking)、[curl RESOLVE](https://curl.se/libcurl/c/CURLOPT_RESOLVE.html)、[curl SOCKOPTFUNCTION](https://curl.se/libcurl/c/CURLOPT_SOCKOPTFUNCTION.html)、[NetworkCallback](https://developer.android.com/reference/android/net/ConnectivityManager.NetworkCallback)。
