# MaiChat 自建 WebRTC 远程桌面可行性

Sunshine＋Moonlight＋Tailscale作为另一条非WebRTC候选的后续调研，见 [三工具调研](../../diagnostics/sunshine-moonlight-tailscale-research-20261112.md)。用户尚未确定部署或集成路线。

2026-11-12，源码基线 `dabe95b854b6345dae512dbd4eb77607a3973494`。用户希望利用已清理的 `8.148.230.58` 自建远程桌面基础设施，替换 TRTC。当前为只读源码审计和方案，未部署新服务、修改客户端或变更现有账号服务/公开包。

## 操作与目标

用户从手机或电脑发起现有远程桌面邀请，被控电脑完成现有授权后显示首帧并接受键鼠控制；停止、拒绝、超时或断线时按原会话身份停止共享并释放按键。目标是远程桌面画面和输入均通过 WebRTC 完成，验证后移除 TRTC 依赖。腾讯 IM 聊天和已有账号鉴权不属于本次替换目标。

## 已核实的现状

- `MaiChat/desktop/src/remote/README.md`：画面走 TRTC，邀请/接受/拒绝/停止信令借道现有 IM 文本通道。
- `remote/TrtcEngine.h/.cpp`：引擎还承担屏幕采集选择、编码、远端画面绑定、进退房和自定义消息；不是只替换服务地址即可迁移。现默认屏幕编码策略为1920×1080上限、10fps、目标1600kbps。
- `remote/RemoteInputProtocol.h`、`RemoteInputSender`、`RemoteInputInjector`：已有输入编解码、归一化坐标、可靠/不可靠事件分类、会话/序号校验及释放按键逻辑，可在适配新的传输后继续复用。TRTC专属的包大小和限频须改为明确的WebRTC传输策略，不能直接删掉所有工作预算。
- iOS `RemoteDesktopSession.swift` 和 Android `remote/.../RemoteDesktopController.java` 均直接使用TRTC收画面并以自定义消息发送输入，因此Windows/macOS/iOS/Android要一起完成兼容迁移。
- 新服务器已知为2逻辑CPU、约2GiB内存、40G云盘；清理后可用约32G。公网出网带宽上限、计费方式、UDP可达性尚未核准，不能承诺并发数、分辨率或与TRTC相同的跨网质量。

## 推荐起步架构

以现有一对一远程桌面为目标，采用客户端WebRTC优先直连、coturn作STUN/TURN兜底；不在这台小机上转码视频。

1. 沿用现有账号/邀请授权语义，在经授权的会话中增加SDP及ICE候选交换。现有IM信令是否直接承载这些消息，须核实消息类型、大小、限流和陈旧消息隔离后冻结协议；不先假定原invite格式已足够。
2. `8.148.230.58`承载带认证和短时凭据的coturn。直连成功时画面不经服务器转发；不能直连时中继媒体与DataChannel。需要配置真实公网/NAT地址、UDP/TCP及TLS回退，并核准云安全组和主机端口。长期TURN密钥只留在控制面配置，不写入客户端。
3. Nginx仅用于需要的HTTPS/WSS入口和反向代理，不能用普通HTTP WebSocket代理替代TURN。若直接复用现有鉴权信令入口，新机器未必需要额外运行Nginx。TURN/TLS与HTTPS使用443时须先解决同IP/端口分配，不假设两服务能直接同端口监听。
4. 客户端接入跨平台WebRTC媒体/数据通道：画面通过视频轨道，键盘/点击/文本通过可靠有序通道，鼠标移动使用低延迟通道并保留合并/限流。采集、硬件编解码、渲染、权限与生命周期逐平台接入；不能将截图PNG接口视为现成低延迟视频通道。

多人同时观看或会议需求再评估SFU（例如LiveKit）；它会将媒体转发负担集中到服务器，当前一对一目标先不增加该部署范围。

## 容量与费用判断

2核2GiB适合作为信令与小规模中继的验证节点；是否足够正式使用必须结合公网带宽与实际会话压力测试。以现有码率1600kbps单向连续转发估算，服务器出网约 `1600000 × 3600 ÷ 8 = 720000000B`，即0.72GB/小时，未含音频、重传和协议开销。该数值是估算，不是实际测量或固定WebRTC码率。

能够移除TRTC的计费依赖，不代表中继免费；仍有ECS公网带宽/流量及维护成本。当前尚不知道实例为固定带宽还是按流量计费。

## 验证顺序与移除条件

先用自有窗口完成一对一画面和输入，再验证同局域网、不同宽带、手机蜂窝网、强制TURN、UDP受限的TCP/TLS回退，以及断线/重连/退出时旧会话和按键释放。保留原授权、停止入口、屏幕几何/DPI及横竖屏映射等断言。

四端实际客户端、硬件编解码和打包依赖通过后再统一移除TRTC运行代码、SDK、构建与分发依赖；最终只保留一条正式远控实现，不长期维护静默TRTC回退。方案阶段不修改已发布v2.2.58或现装App。

## 官方依据

- [WebRTC peer connections / signaling](https://webrtc.org/getting-started/peer-connections)
- [WebRTC TURN server](https://webrtc.org/getting-started/turn-server)
- [WebRTC Data channels](https://webrtc.org/getting-started/data-channels)
- [coturn](https://github.com/coturn/coturn)
- [Nginx WebSocket proxying](https://nginx.org/en/docs/http/websocket.html)
- [LiveKit SFU architecture](https://docs.livekit.io/reference/internals/livekit-sfu/)
- [阿里云ECS公网带宽计费](https://help.aliyun.com/zh/ecs/public-bandwidth)

## 追加：成熟产品架构调研

用户要求核对业界主流做法。以下是官方公开实现/文档中的代表性实例，不把样例当作市场份额统计，也不把所有远控产品都归为WebRTC。

| 参考产品 | 已核实的设计 | 对MaiChat的意义 |
| --- | --- | --- |
| Chrome Remote Desktop / Chromium remoting | 原生 `WebrtcTransport` 创建libwebrtc PeerConnectionFactory，视频媒体与控制/event DataChannel并存；网络策略分别控制STUN、relay和outgoing，记录候选路径为direct/relay | 最贴近现有原生多端客户端的WebRTC实现参考 |
| RustDesk | `hbbs`负责ID/协调及打洞，无法直连时使用`hbbr`；relay实现使用它自己的rendezvous消息体系 | 参考协调/直连/中继分层；hbbs/hbbr不能直接作为标准coturn给WebRTC客户端使用 |
| TeamViewer | 官方说明由主服务器握手，再按网络条件直连UDP/TCP，其余经路由网络中继；支持多种出站连接回退 | 成熟远控需要受限网络下的可达性及中继兜底，不仅验证局域网首帧 |
| LiveKit / mediasoup | 公开架构为SFU，接收端点媒体并选择性转发；mediasoup明确是低层媒体API | 多人/一对多可作为候选，不能仅因其是WebRTC服务器就认定比一对一P2P更适合当前产品 |
| Selkies | WebRTC引擎将有序input和无序pointer分开，处理输入背压及事件次序；是云桌面到浏览器场景 | 可参考控制通道语义，不把浏览器/Linux宿主方案当作四个现有原生端的直接替换包 |

本机现有Chromium检出HEAD为 `78bf15daf4149ebe14096b05e14c593c30d65846`，只读核对 `remoting/protocol/webrtc_transport.cc`、`network_settings.h`、`ice_config.h` 和 `webrtc_video_stream.cc`；同时读取官方main源。没有新克隆、下载SDK或构建Chromium。

一个需要明确保留的差异：核对的Chromium `CreateOutgoingChannel` 默认可靠通道，没有设置maxRetransmits/maxRetransmitTime；不能把“鼠标必须不可靠”冒称Chrome现成实现。MaiChat现有移动事件分类和Selkies无序pointer可作选型依据，最终仍须验证跨通道次序、点击时的坐标、旧会话消息隔离和按键释放。

调研后的优先候选是完整原生libwebrtc媒体/数据栈＋经鉴权的现有信令＋自建coturn；客户端采集/硬件编解码/渲染适配及清晰度、首帧、输入延迟、重连是主要工程工作。Nginx处于入口层，选型重点是端点引擎、连接路由与远控体验，不是用反向代理替代RTC。此为根据产品匹配度作出的工程建议，尚不是跨端性能验证结论。

先通过自有窗口与自有设备对照TRTC测首帧时间、输入响应、文字清晰度、CPU、丢包重连和强制中继出网量；覆盖局域网、不同宽带和蜂窝网，再据证据决定正式替换。小服务器先承载验证规模，后续是否加带宽或区域中继按实测决定。当前仍未部署新服务或修改产品源。

追加官方来源：

- [Chromium原生WebRTC传输实现](https://chromium.googlesource.com/chromium/src.git/+/refs/heads/main/remoting/protocol/webrtc_transport.cc)
- [RustDesk自建架构](https://rustdesk.com/docs/en/self-host/)
- [RustDesk relay实现](https://github.com/rustdesk/rustdesk-server/blob/master/src/relay_server.rs)
- [TeamViewer连接与安全架构](https://www.teamviewer.com/en/global/support/knowledge-base/teamviewer-remote/security/security-statement/)
- [mediasoup官方架构](https://mediasoup.org/documentation/overview/)
- [Selkies WebRTC引擎](https://docs.selkies.io/latest/reference/webrtc_engine)

## 追加：LiveKit以外的候选

| 候选 | 已核实的组成 | 与MaiChat的匹配及代价 |
| --- | --- | --- |
| mediasoup | SFU媒体模块；服务端JavaScript API＋C/C++媒体worker，C++客户端libmediasoupclient基于libwebrtc | 可深度定制房间及媒体路由，但需要自行完成业务信令/鉴权；C++端仍须用libwebrtc创建媒体track，不是避开完整媒体栈的捷径 |
| Janus | 通用WebRTC服务器，插件提供VideoRoom/VideoCall等能力，提供HTTP/WebSocket等API传输 | 适合定制网关/房间，但原生四端采集、控制、界面及生命周期仍需适配 |
| libdatachannel | 轻量C++/C WebRTC数据和媒体传输库，公开支持Windows/macOS/iOS/Android等平台 | 符合原生C++集成方向，但其侧重点是传输，不能代替完整屏幕采集、编解码、渲染及远控质量调优；需要与现有媒体模块组合并验证 |
| RustDesk整套远控 | 既有客户端、专用ID/协调和中继体系 | 可作为独立远控产品使用或评估深度集成；不能仅装hbbs/hbbr就让现有MaiChat TRTC客户端接入，当前协议、身份及界面整合范围较大 |
| Apache Guacamole | 浏览器接入的远程桌面网关，后端桥接RDP/VNC/SSH | 若产品目标转为浏览器统一运维入口可考虑；与当前MaiChat原生客户端和自有屏幕/输入通路的集成方式不同 |

当前优先做工程匹配比较的仍是完整libwebrtc＋coturn，以及成套LiveKit SDK/服务；前者侧重一对一直连控制及自有逻辑，后者可减少房间/连接管理工程并方便多人扩展。此选择不是实测性能排名；需要对Qt C++与移动SDK能力、媒体适配、带宽和包体做同条件验证。没有为本次比较在服务器安装软件或在本机下载大型SDK。

补充官方来源：

- [mediasoup服务端设计](https://mediasoup.org/documentation/v3/mediasoup/design/)
- [libmediasoupclient C++客户端设计](https://mediasoup.org/documentation/v3/libmediasoupclient/design/)
- [Janus官方文档](https://janus.conf.meetecho.com/docs/)
- [libdatachannel项目](https://github.com/paullouisageneau/libdatachannel)
- [Apache Guacamole架构](https://guacamole.apache.org/doc/gug/guacamole-architecture.html)
