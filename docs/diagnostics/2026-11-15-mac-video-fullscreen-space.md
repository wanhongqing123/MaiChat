# Mac 播放器 HUD 的原生全屏 Space 归属

2026-11-15。现场为正式 2.2.62，不能归因于旧版本。用户后来自行运行 R5 私有候选，实际 PID 64730、路径为 `private-r5-app/MaiChat.app`；开发者没有退出或替换 `/Applications` 现装。

普通 `MainWindow.openVideoWindow` 入口进入原生全屏后，播放器 native key/onActiveSpace/visible 均为真，标题与控制栏 Qt/native visible 为真但 onActiveSpace/occlusion 为假，order=-1、parent=0、collectionBehavior=258。控件图标本身非空。原测试额外强设窗口 Space 和激活状态，掩盖了普通入口。

R5 只修改 `MaiGraphicsMacLayer.mm`：HUD 持久归属同一播放器的 native child-window 组，移除 `MoveToActiveSpace`，保留 `FullScreenAuxiliary`、Qt 对象、原 flags/观察者与 owner resize。这样控制栏随播放器进出原生 Space，切回创作中心时不再成为独立假窗。

生产 blob 为 `367cca595db7f45c6454e0565c2dd95bddb0addb`，两测试 blob 为 `9b963a355149bf456cf05e9589b8d15f57c00166`、`dd68fa88db99f88d4772359b126c27eb8684094a`。Root 逐 blob 核合 FULL3-v2，未叠加旧链式补丁。

Mac 实际原生全屏 Space 往返、真实创作中心、图标像素及 native 命中、关闭重开：原影响组 14/0/0，正确 Creation 入口补例 3/0/0。Root Windows 普通库只跑共用 chrome/title/tooltip 三槽，5/0；这不替代 Mac 原生 Space 验证。Qt 普通关闭也可能缓存 WindowServer 编号，验收要求旧 Qt 对象销毁、native contentView 清空/不可见/无 parent，新 owner/HUD 重新绑定，不宣称 SDK 缓存编号必须消失。

这项修复仅证明 Mac Space 归属。用户随后提供的图片打开末帧闪、视频先小后大、全屏关闭还原小窗是另一个正在排查的开合交接问题，其中图片共用路径亦涉及 Windows，不能用本组通过宣称已经修复。原 v2.2.62 Release/tag/八件资产保持不变。
