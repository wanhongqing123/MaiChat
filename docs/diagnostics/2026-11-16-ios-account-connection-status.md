# 2026-11-16：消息连接状态移至账号与安全

用户明确要求将“我的”头像资料卡中的消息连接状态移到“账号与安全”，保留其含义和
实时更新。生产改动仅 SettingsView.swift；原 UI driver 另补通知提示前提及账号页截图导航。

SettingsView 不再在头像账号下展示 SelfConnectionStatus。AccountSettingsView 的账号
section 在登录账号下复用同一组件，显示“消息连接”、状态色点及 state.rawValue。
同一 EnvironmentObject 的 Published connectionState 继续驱动参数更新，未复制或缓存状态。
connected/connecting/failed/disconnected 四态颜色、accessibility label/value/identifier 保留。

收件补丁 3259 bytes，SHA256
`f4a2872993f55cdbab6ae210bb0e62fba0cc30139e845bcd87162e023599658a`。
消费前两个路径 worktree/index 均 clean，分别匹配 7d4b7b51/fd13909e；check 通过后仅应用一次。
新 blob 为 b8b22b7c/acab324e，规范化源码 SHA256 与 producer manifest 逐项相等。
独立只读复审未发现具体 P1/P2，原 UI 断言未删除或弱化，diff check 通过。

Mac 协作者报告正常 Simulator 既有 UI driver 1/0，并已查看 My/Account 两张实际画面；
215 正常 device build exit0。收件时安装回执尚待，不能据此称真机安装或回归完成。
新增 capture 没有实时状态切换断言，也不能单独证明断连/重连的完整 UI 测试。

本机为 Windows，仅核验精确源码、结构与辅助功能语义，未执行 iOS 构建、设备操作或
网络服务变更。原有三路径 WIP 保留；该授权不外推 Windows 进程更新、Mac TCC 或 Sync 部署。

后继协作者回执：215 正常 device build、签名及保数据安装全部 exit0（13.522秒），
fresh metadata 为215，正常启动 PID67054。此项是跨机回执，不是本机设备操作。
用户随后要求头像旁公开账号明确标为 MaiChat；216 正在独立验证，尚未消费其源码。
因此215保持为已交付的中间版本，不标成最终账号文案。当前已提交 SettingsView blob
为 `b8b22b7c`，后继应按精确基线增量核合，不重复应用215；不据文案变更修改 Auth、
Keychain 或服务端账号数据，也不将 IM user ID 解释成第二套登录账号。
