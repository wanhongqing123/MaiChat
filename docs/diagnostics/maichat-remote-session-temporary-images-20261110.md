# 远端 Session 临时图片

用户在 iPhone 创作中心打开另一设备的 Session，助手正文之间的截图显示“图片在所属设备，暂不可跨设备预览”，无法查看实际图片。预期是图片保留原消息和片段位置，显示普通缩略图，并可打开现有图片预览。

2026-11-10，Mac 工作区基线为 `28977e7a42e5bbb680505e5aa7c8aef9ebe0b240`，另有独立电脑控制供件；本轮不改变版本、安装产物或已发布资产。iPhone 已安装的 Release 203 仍是此前记录的 `631f5d9c` / Core `69e18eb1` 输入，不能倒填成本轮源码。

## 定位与冻结行为

- 用户实际截图是助手 screenshot 输出。`DesktopSessionSyncHost::serializePart` 原来仅发送图片类型、文件名和远端片段 ID，没有媒体读取引用。iOS `remoteDisplayMessage` 清除远端路径后添加占位文字；该路径不能被当作手机本机路径解析。
- 用户明确采用临时 OSS 中转，且两次明确“不必在线重传／过期重传”。新 live 图片首次上传一次，Session WebSocket 只同步引用和状态；不回填上传全部历史，不增加查看时的重新上传命令。
- `remoteImage` 元数据按 Root 冻结的 pending/ready/failed 协议读取。对象固定 24 小时业务有效期，以服务首次分配的 Unix 毫秒 `expires_at` 为准；签名 URL 最多 60 秒且不能越过对象有效期。
- 已有本机完整缓存可在对象过期后继续显示；无缓存时显示“图片已过期”，不请求源设备重新上传。网络失败与对象过期分别显示。
- 缓存必须隔离账号、原 owner、会话、消息、片段和 attachment ID；取消、切会话、换账号后的迟到结果不得进入当前行。user 与 assistant 两类消息都保留正文／图片原顺序。

## 官方接口边界

使用专用 `session-temp-images/` 前缀的 OSS 生命周期规则删除临时副本。`PutBucketLifecycle` 是完整覆盖操作，必须先读取并保全其他规则，再合并及读回；业务过期与 OSS 异步物理删除分别记录。官方资料：

- <https://help.aliyun.com/zh/oss/developer-reference/putbucketlifecycle>
- <https://help.aliyun.com/zh/oss/user-guide/lifecycle-rules-based-on-the-last-modified-time>

## 验证进度

Mac 为 macOS 27.2 arm64，使用本机 iOS 26.5 Simulator。Debug 构建仅用 CLI 指定 build 203，未修改工程版本，未覆盖手机上此前安装的 Release 203。

原渲染路径正常编译后，`testRemoteAssistantImageUsesTemporaryAttachmentAndNormalPreview` 实跑 1 FAIL / 0 SKIP，17.588 秒：前后文字均存在，中间只有原占位提示。原日志、截图、UI hierarchy 和源码快照保存在私有证据目录，未把编译或网络环境问题算作此 RED。

修正后最终正常 Simulator 构建 exit 0；同组 7 tests / 0 failures，83.962 秒。六个 iOS 源文件逐 SHA 与构建前清单一致：

- 助手原用例通过，检查图片保持正文位置、屏幕像素匹配自建 `#2d638d` PNG、点击进入原图片预览并返回唯一 composer。
- 用户图片检查原片段顺序及实际像素；不将多段文字提前合并而移动图片。
- pending、上传失败、对象过期及错误 room 绑定分别显示，最终下载边界均为 0 次。
- 网络故障显示加载失败；重试只读取同一个 attachment，未把网络错误显示为对象过期。
- 缓存／取消测试执行 20 项检查：完整缓存过期后仍可读、未缓存及损坏缓存过期后不发请求、原 owner login／账号／会话／片段隔离、取消后的迟到数据清理，以及未取消但已换账号或引用的迟到结果拒绝。
- 原普通问题回答和忙碌任务补充两项回归同时通过。

Simulator 替身仅替换最终下载边界，使用 App 自己生成的 PNG；wire 解码、真实消息投影、缓存、SwiftUI 消息行及原媒体预览均为生产实现。图片下载／签名／TLS／取消／完整文件提交沿用 `MAI_SESSION_IMAGE_DOWNLOAD` 共享 C++ 实现，Swift 只传完整原 owner tuple、固定元数据、取消 token 和私有输出路径。获取凭据后及缓存异步读取后再次核对当前账号／会话，避免旧请求使用新账号身份发出。

另在 Mac 普通构建上实跑 Root 提供的原 persistent 和 temporary 两个 local TLS fixture，2/2 PASS，36.70 秒；这属于临时 URL 延迟修正前的 Network 第一版证据。随后导入 Network v2（header ABI 不变，cpp canonical `5c832405b419ae0e07e0b751cf92fbfa28ef30ae`），其原两组及新增 `--temporary-latency` 的最终结果单独记录，不倒填第一版结果。

## 交付边界

本轮验证范围为 Desktop owner → iOS viewer。iOS 自己作为 owner 发图的临时发布尚未接入；旧历史中没有 `remoteImage` 的图片不自动补传。不把本轮称为所有来源平台已经打通。

线上服务／OSS 生命周期、真实跨设备下载和手机本轮安装验收尚未完成。未调用模型、未读取实际用户数据库、未更换现装桌面程序，也未重打或改名此前已交付的 2.2.54 候选包。私有证据位于 `/private/tmp/maichat-remote-session-images-20261110`；所有 UI 素材均为自建夹具，用户实际会话截图不进入供件或源码。
