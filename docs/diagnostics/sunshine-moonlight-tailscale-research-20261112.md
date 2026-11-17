# Sunshine、Moonlight、Tailscale 调研

日期：2026-11-12。用户目标是在MaiChat远程桌面替换TRTC的选型中，研究这三个工具的配合方式、跨端能力及自建服务器用途。此次只读取官方文档、公开源码和release元数据，没有安装软件、克隆仓库、下载大型SDK、修改服务器或操作用户现装App；没有性能实测结论。

MaiChat源码基线为 `dabe95b854b6345dae512dbd4eb77607a3973494`。现有远控的画面和控制消息使用TRTC，邀请/授权及输入注入已有业务逻辑；具体审计见 [WebRTC方案](../superpowers/plans/2026-11-12-self-hosted-webrtc-remote-desktop.md)。

## 三者配合

| 工具 | 运行位置 | 负责的工作 |
| --- | --- | --- |
| Sunshine | 被控Windows/Mac/Linux电脑 | 屏幕/音频采集、视频编码、串流会话和输入接收；可使用硬件或软件编码 |
| Moonlight | 操作端Windows/Mac/Linux/iOS/Android等设备 | 接收和解码画面，显示桌面，回传鼠标、键盘及其他输入 |
| Tailscale | 需要互通的两端设备 | 基于WireGuard的虚拟组网、身份/访问策略、NAT穿透与中继兜底；不负责屏幕采集或视频编码 |

Sunshine＋Moonlight局域网即可工作；Tailscale是异地接入选项，不是它们必需的媒体组件。Moonlight官网明确把Tailscale列为互联网串流的可选组网方式，也说明可以串流整个Windows桌面和应用。因此可以把它作为通用远控候选，不限于游戏。

该组合使用GameStream/Sunshine-Moonlight协议＋WireGuard网络，不是此前WebRTC＋coturn方案的同一协议栈。Tailscale DERP或Peer Relay不等同于TURN，不能把两类中继直接互换。

## 核对版本与平台

GitHub官方latest-release元数据在本次查询时返回：

- Sunshine `v2026.914.233613`，2026-09-15T01:17:17Z发布，非prerelease；原LizardByte仓库URL现重定向到SunshineStream/Sunshine。
- Moonlight PC `v6.2.0`，2026-10-04T00:44:48Z发布，非prerelease。此版本号只指PC客户端，不代表iOS/Android商店版本。
- Tailscale源码 `v1.104.6`，2026-11-11T00:29:49Z发布，非prerelease。没有把此源码tag当作设备现装版本。
- moonlight-common-c没有latest-release条目；实际集成须以选定客户端所固定的子模块commit为准，不能任取浮动master混用ABI。

Sunshine支持Windows、Linux及macOS主机，Moonlight官网列出Windows/macOS/Linux、iOS与Android客户端。但所查Sunshine正式版文档仍将macOS主机标为experimental；不能因Mac作为Moonlight观看端可用，就认定Mac作为被控端已同等验证。

正式版macOS说明同时已支持macOS14+ Audio Tap系统音频采集，旧文章“一律必须BlackHole”不再适用于这版。文档注明默认Command键转发存在映射差异，并将右Option映射为Cmd；普通桌面需要核验中文/emoji、Command组合键、DPI与权限。master中的新虚拟HID等变化不能倒填为所选正式版能力。

Sunshine当前Windows文档另列可选付费Virtual HID Driver；普通输入仍有SendInput后备路径。不得把可选驱动的效果或价格当作基础版必需条件，也不应拿该路径代替MaiChat实际键鼠测试。

## Tailscale网络与服务器角色

Tailscale现支持Direct、Peer Relay和DERP Relay三种连接路径，均通过WireGuard端到端加密。直连通常延迟和吞吐最好，发生中继时才由所选中继承担媒体包转发；部署协调服务器本身不会令所有画面流过协调服务器。

对已清理的 `8.148.230.58`（2核、约2GiB、可用约32G），合适的候选职责是：

1. 使用官方Tailscale控制面的情况下，按需要部署自己的Peer Relay或DERP；仅安装普通Tailscale客户端不会自动成为中继。
2. 若控制面也希望自建，另评估Headscale；它是社区自托管的Tailscale控制服务器实现，官方稳定文档提供嵌入DERP，但功能范围并非官方服务完整复制。
3. Peer Relay官方最低要求1.86+、可达的UDP端口及相应tailnet策略，适合评估高吞吐媒体中继；不能未经验证就假设Headscale与官方Peer Relay所有策略能力完全对应。

桌面采集和硬件编解码仍在本方案的被控电脑与观看端完成，不安排此小ECS承担桌面视频转码。实际中继并发、可用画质和成本仍取决于公网带宽、运营商路径及出网计费，当前这些参数尚未核准。

Tailscale默认使用官方协调服务；只自建DERP不等于自建整个控制面。官方有个人免费及商业计划，个人计划适用范围与产品商业集成要按当前官方条款区分，不能笼统承诺MaiChat所有用户均可免费使用官方网络。

## 与iPhone现有使用方式的关系

Tailscale官方FAQ明确iOS/Android通常只能同时启用一个系统VPN。用户此前使用Shadowrocket，因此直接启用独立Tailscale App存在共存问题，不能承诺它们可同时保持启用。

官方libtailscale提供进程内C库和用户态组网，头文件有TCP/UDP形式的dial/listen入口。这可作为App内集成研究方向，但本次没有验证它与Moonlight的多UDP通道/ENet、iOS后台生命周期、现有代理和网络权限的兼容性，不能据API存在就承诺无需VPN的直接替换。

Moonlight官方setup文档还要求iOS/tvOS首次配对在同一网络进行；原型应先按所选实际版本完成局域网配对，再验证异地接入，不能以一张画面代替配对/授权闭环。

## MaiChat集成范围

现成工具作为独立方案可以先用于自有设备的体验对照。要让用户仍在MaiChat内部完成远控，则须评估：

- 宿主Sunshine的安装、服务生命周期、配对与证书，以及如何保留MaiChat现有的邀请、授权、停止和会话身份隔离。
- 客户端复用moonlight-common-c的可能性。其README明确供PC/Android/iOS共用；已读C头文件提供连接、解码/渲染回调、鼠标、键盘和UTF-8文本输入接口，但App仍需实现解码渲染、UI、线程与生命周期适配。
- moonlight-common-c必须使用它指定的ENet子模块；官方警告普通其他ENet版本的API/ABI不兼容。不能简单链接到系统或另一SDK的ENet。
- Tailscale用外置App还是进程内网络、官方还是自建控制面，以及自己的账号/设备授权如何衔接。
- Sunshine、Moonlight PC和moonlight-common-c官方仓库均为GPL-3.0；直接复用或分发时需明确与MaiChat发布方式的许可证兼容范围。本次仅记录上游许可证事实，没有作出可随意嵌入/闭源分发的法律结论。

## 建议验证顺序

先用自有Windows主机和iPhone/Mac观看端验证现成工具的完整配对、桌面输入及延迟，再独立验证Mac被控端。网络依次覆盖局域网、不同宽带、蜂窝网、强制Peer Relay/DERP，记录最终实际连接路径；不能把首次探测短暂relay当作稳定路由。

按相同分辨率/帧率/目标码率与TRTC对照首帧、静态文字清晰度、滚动/视频、鼠标延时、中文和组合键、CPU、断线及释放按键、横竖屏/DPI、时延和出网量。保留真实失败及版本边界后，再决定作为外部工具使用，还是深度集成到MaiChat。当前仅形成候选建议，未替换TRTC或宣称实机通过。

## 官方来源

- [Sunshine项目](https://github.com/SunshineStream/Sunshine)
- [所查Sunshine正式版](https://github.com/SunshineStream/Sunshine/releases/tag/v2026.914.233613)
- [同正式版文档](https://docs.lizardbyte.dev/projects/sunshine/v2026.914.233613/md_docs_2getting__started.html)
- [Moonlight官网](https://moonlight-stream.org/)
- [Moonlight设置/配对文档](https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide)
- [Moonlight PC正式版](https://github.com/moonlight-stream/moonlight-qt/releases/tag/v6.2.0)
- [moonlight-common-c说明](https://github.com/moonlight-stream/moonlight-common-c)
- [Moonlight C接口](https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/Limelight.h)
- [Moonlight核心许可证](https://github.com/moonlight-stream/moonlight-common-c/blob/master/LICENSE.txt)
- [Tailscale连接类型](https://tailscale.com/docs/reference/connection-types)
- [Tailscale Peer Relay](https://tailscale.com/docs/features/peer-relay)
- [Tailscale控制面/数据面](https://tailscale.com/docs/concepts/control-data-planes)
- [Tailscale与其他VPN](https://tailscale.com/docs/reference/faq/other-vpns)
- [Tailscale方案与定价](https://tailscale.com/pricing)
- [libtailscale](https://github.com/tailscale/libtailscale)
- [Headscale](https://github.com/juanfont/headscale)
- [Headscale嵌入DERP](https://headscale.net/stable/ref/derp/)
