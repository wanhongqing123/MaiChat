# 主模型配置与账号删除的异步竞态

## 可复现顺序与预期

1. 旧账号的 `AIAssistantBackend.open/save` 进入本地模型解析或云凭据领取的 await。
2. 在该 await 恢复之前，执行删除账号专用屏障并移除旧账号工作区。
3. 释放旧操作。预期旧操作失败，旧工作区、settings 与数据库都不重建。普通换号不永久撤销旧实例，已经接受的旧账号发送收据仍按原有 outbox 流程保存。

## 根因

原删除屏障只覆盖 queue/pump/intake/media cleanup。新的 async `backend.save` 等待凭据/Store 后没有账号删除门禁，接着 `configure` 会创建 Workspace、打开旧 sqlite，再写 settings。`open` 也会在 await 后直接配置；`initialPreparation.cancel()` 只设置取消标志，缺少检查时不能阻止其后续文件副作用。

## 确定性红→绿

- 仓库回归入口：`python3 MaiChat/ios/MaiChat/scripts/test-primary-model-lifetime.py --work-dir <独立临时目录>`。脚本从实际源码提取 `AIModelSettings`、backend `open/save` 与生命周期方法，不手写一份替代的 async 业务流程。
- 为独立验证生命周期，它以真实文件系统模拟 native configure 的目录/sqlite 文件副作用，在解析完成、await 尚未返回处用 actor continuation 精确暂停；不启动 App、模型、网络或用户数据库。
- 红源码冻结 `/private/tmp/maichat-primary-lifetime-race-20261104/AIAssistantModel-red.swift`，SHA `1f6f26e3e6ec38132e5aaf0907b5fb74e26e4777973b84d21a4fa6d464e1d46d`。原 `open/save` 释放后都返回成功并重建已删目录；正常 save 控制组通过，整个脚本 exit 1。日志 `/private/tmp/maichat-primary-lifetime-race-20261104/red.log`。
- 修改后同脚本、同暂停/删除/恢复顺序：`open/save` 都失败且目录保持不存在，正常 save 仍成功，exit 0。日志 `/private/tmp/maichat-primary-lifetime-race-20261104/green.log`；各日志保存实际方法来源 SHA。

## 修复与边界

- backend 新增永久失效状态，删除专用屏障首先 await `backend.invalidatePermanently()`。actor 会先完成已进入同步区的写操作；此后任何暂停中的旧配置恢复都必须拒绝，之后才允许删除目录。
- `open/save` 入口及各 await 后检查生命周期；`configure` 创建目录之前、写 settings 之前也检查。通用 C ABI 请求入口拒绝失效或已取消任务。local open 的可恢复配置错误 catch 也重查生命周期，不能把删除/取消误包装成可恢复成功返回。
- 普通 `resetForAccountSwitch()` 不调用永久失效；已有发送收据的 stop/确认逻辑未更改。旧 settings 页面迟到返回不刷新新账号状态。
- 同轮小 UX 修复：本地目录加载完成后，仅当用户已选本地且 `localModelID == nil` 才补选已存在 Qwen/首个模型。已有非空但失效的 ID 保留供用户管理修复，不悄悄替换已存选择。
- Swift 源 parse 和差异检查通过。这里的回归明确是实际方法体与文件生命周期验证，native configure 是副作用替身，不作为完整 App/C ABI/真机删除流程验收；最后构建回归由主任务完成。
