# 2026-11-16：创作中心按真实工具目录展示说明

用户要求隐藏已经禁用的图像/视频创作引导项，并补齐漏写的实际可用工具说明。
基线 `4bf370f7`，Windows 11、普通 Release 缓存、Qt 6.12、MSVC 2022。

复现：进入空白创作会话，原欢迎页固定 19 类，始终展示图像创作，切换型号也不按该型号
实际目录筛选。新增两例普通 Windows 组件回归实际 RED（构建 exit 0，2 pass/2 fail），
证据 `artifacts/starter-catalog/red-qtest.txt`。

`AgentController::availableToolSpecs` 从同一个实际注册表只读取得公开声明，并复用模型
分派时的可用性过滤；没有调用 discover、取密钥或启动工具。`StarterCatalog` 将公开
声明完整归组，剩余未知公开工具有目录兜底，防止后续新增工具漏列。Panel 保留单列卡片、
同一欢迎滚动容器及只填当前草稿的点击行为。

明确区分型号 ID 和“正在连接模型/模型未开通/模型连接失败”等展示文案。当前会话按真实
configuration.model 筛选，空页面无真实型号时取公开目录，不拿状态标签去过滤网络或
图像能力。独立静态 review 找到该 P2，补 27 组回归后关闭。

当前实际注册中的 image、wan_video、seedance 已为 internal，不进入欢迎目录；
MiniMax/Kling 视频工具仍为 public，按真实配置显示，不在 UI 伪称它们已经禁用，也没有
改注册/启用策略或重启生成工具。已有媒体浏览、上传、下载、音乐和视频播放均保留。

完整普通组件选择组最终 **35 passed / 0 failed / 0 skipped**，包含真实 cloud/media
注册的文字/图像两种会话、准确目录集合、模型切换、27 个状态标签场景、草稿行为和
550/700/1100 宽度。已查看窄宽欢迎页与真实云盘长工具行截图，没有横向越界或截掉标签。

首次统一 GREEN 的 1 个失败是 coverage fixture 错用 deepseek-flash 期望，而实际默认
session 为 glm-5.3；修正 fixture 按真实 session 配置独立查询 registry，并同时测试
glm-5.3-flash，保全部禁用项/媒体能力断言，不放宽生产逻辑。

证据在 `.cache/desktop-input-primitives-20261116/catalog-final-green.txt`、
`catalog-evidence/`。这是普通组件测试及实际 widget 图像，未替换当前正常 App，未发送
模型请求或付费生成任务，不外推为用户安装版本已生效。
