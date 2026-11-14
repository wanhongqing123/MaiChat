# iOS 绑定会话列表重复点选取消：2026-11-14

## 操作与预期

在私聊加号中打开“绑定会话”，点击某行右侧圆圈，再次点击同一对勾。第二次应取消待确认选择、恢复空圈并禁用确认；第三次可重新选择。它只改变 picker 的待确认值，不自动解除已经存在的好友绑定。聊天草稿应保持。

已有绑定 A 时，打开 picker 仍应默认选择 A。用户主动反选后，正常标题/绑定投影刷新不得把 A 暗自选回；取消 picker 后原绑定 A 及草稿仍在，重新打开才再次默认 A。

## 根因与最小修改

基于 `0c57cd0204a335055121fc667e27cff8f7de93e6`。原行按钮每次都赋值 session id，不能反选。首修用一次点选前状态同时切换 selection 与 workspace pin。

独立复核又发现动态刷新问题：旧 `.onChange(imSessionBindings)` 在空选择时，从已有 binding 恢复 session/workspace。最后只保留首次 `.task` 的默认绑定选择；后续投影变化只校验并清除失效选择，不恢复用户明确的空值。Core、Native、消息路由及公开 v2.2.60 产物不改。

## 正常 App 定点验证

使用既有 normal Simulator cache、原私聊/UI 夹具，无真实账号登录、聊天数据库或付费模型请求。仅增强原 `testSessionBindingSelectionPreservesIMDraft`，用行右侧坐标 `0.94/0.5` 复现：

- 首次无绑定反选：原实现 build0/21.548s、test65/25.273s；首修同测试字节 build0/22.482s、test0/25.701s。
- 动态刷新夹具初次把 identifier 放在 sheet 关闭后读取而失败，另误挂一次测试 flag；此为测试设置错误，不计功能 RED，原日志保留。
- 修正夹具后，通过普通 `selectConversation → rename → refresh` 让实际 Native A 标题投影变化，没有直接注入显示数组。旧 observer 实际 RED：build0/16.577s、test65/31.985s，明确失败为“普通绑定标题刷新不得把用户已取消的选择恢复”。
- 最后 observer 修正后同测试字节 GREEN：build0/20.656s、test0/39.482s；反选/重新选、确认禁用、已有 A 投影更新后仍空、真实绑定与草稿保持、重开默认 A、最后显式解绑均通过。

已亲看选中、未选中及标题刷新后仍未选中的正常 App 原 PNG。普通改名辅助按钮仅存在于 `DEBUG && simulator` 且显式测试 flag 下，Release 必须核对应入口负检。

## 设备边界

首份 210 来自首修 iOS tree `f96250cd7bfd9fe0d7916c772b6db1365148dd91`，正常 generic Release build0/105.571s，签名/profile/10项 entitlement/20个 normal对象及9个 MachO marker 均核合；但未包含动态刷新修正，明确 hold、禁止安装。

装前查询报 CoreDevice4016，无法取得可信连接/服务状态，安装命令没有发起；最后实际读回仍为 2.2.4/209。不把该 210 候选称作已安装，不反复查询不可用设备。

最终 iOS tree `9b0ec0ecc41d8085b3006159fc3ee1ee7c6bf85a` 的 normal generic Release 复用原 cache 必要增量 exit0/104.595s，沿同一未安装 build210，不虚增211。

- 最终署名构建 exe SHA256 为 `8f3eeb26b52e7fe1dafcbf8b9a7090ae20b90ab912efa049df33181d47df2590`。
- deep/strict 签名、profile 和10项升级 entitlement 兼容；20个实际普通对象/合库成员及最终 Ld 核合，9个 Release MachO 的测试入口负检通过。新增刷新辅助 flag/identifier 均有 Debug 阳性与 Release 阴性对照；7个模型资源 inode/bytes 复用。
- Core tree仍 `37e6783a52494fe76fef9df981af92290ca5fa3b`，Mobile tree仍 `a16a042dc839402940c95e64281ac8aacb47dd9b`。
- 最终210仅 ready，尚未安装或启动；等待用户恢复连接，安装前必须重新读实际 build 并保证使用更大 override。旧首份040d候选仍 hold，不能代替最后8f3e候选。

公开 Release v2.2.60 不变，Mac 现装未退出或替换。
