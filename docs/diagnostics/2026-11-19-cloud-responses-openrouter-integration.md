# 云端主模型统一 Responses 与 OpenRouter 接入

## 复现目标与源码边界

2026-11-18 至 19 日，Windows 主工作区以
`2a2c7c537bfa059f042972c11450a5b48a3e3b86` 加本次待提交源码验证。
用户要求保留原默认模型，新增 OpenRouter `anthropic/claude-fable-5.2`，
并让 GLM、DeepSeek、OpenRouter 的云端主模型统一使用 Responses。
主模型请求仍由客户端直连供应商，后台只下发当前账号授权的凭据。

操作与预期：登录后打开主模型菜单，五个型号与思考档位分别选择；
旧 GLM 会话恢复后也应使用 `/api/v1/responses`，不能把旧 Chat 路由直接拼上
`/responses`。切换账号、型号或恢复会话时不得借用其他供应商的 Key。
工具响应的完整 output 应按原顺序续接，不能重复调用工具或丢掉已显示正文。

## 实现与失败证据

- 云端目录统一 Responses，OpenRouter 使用独立凭据与 URL；新旧凭据响应均按字段类型验证。
- 新的普通 continuation 数据保存 provider、model、格式及完整 output；SQLite 增量迁移至
  v13，原无 continuation 的记录和指纹保持兼容。保存后再广播，包括只有加密状态的响应。
- 续接只匹配原 provider/model，保留未知元数据；切换供应商时不重建或转发旧签名。
  上下文预算与压缩按完整响应及其整批工具结果处理。
- 已实际观察协议 RED 78 项、存储 RED 23 项、上下文/压缩各 RED 2 项、
  Runner 状态保存 RED 24 项、取消边界 RED 11 项；对应修复后原用例通过。
- 最终审查补到两个真实协议漏洞：显式空终态被旧 item.done 回填，会复活旧工具；
  不完整 done 前缀被保存为完整历史，会漏掉已显示答案。两条本地 SSE 最小用例实际
  RED 共 9 个断言，修复在发布状态/工具之前检查终态权威性与快照完整性。
- 新供应商进入目录后，原生网页工具旧的二元分派会把 Fable 错送给 DeepSeek。
  发现与直接执行现共用 GLM/DeepSeek 明确白名单；实际 RED 31 项到 GREEN，
  Fable 直接调用也不读取其他供应商的 Key、不发 HTTP。

## 已执行验证

Windows Core 使用实际 `MaiAgent/` 源码的新构建目录
`.cache/openrouter-fable52-20261118/core-verified-build`。
首个独立构建发现 MSVC 中文 showIncludes 前缀被 CMake 误识别，依赖记录为空；
已改用项目现有 `msvc-showincludes.cmake`，标准 clean 后重编。
90 个 Core 对象均有实际依赖记录，新 ABI 头与相关消费者依赖均可查，未伪造缓存身份。

- Core、输入队列、会话改名、路径、Store、ModelClient、Runner、Tool、
  ContextBuilder、Compactor 十套回归通过；末端修复后完整 ModelClient 与原生网页
  测试再次通过。原生网页套件中旧 Seedream 用例仍期待暂停动作请求付费审批，
  已按现有生产暂停合同改为验证无审批、无取 Key、明确错误落库及向模型回灌，
  未改其他付费审批合同。控制台端到端的工具、权限、会话恢复和崩溃后落库检查全部通过。
- Windows 正常 `maichat` 构建、部署检查与 PE 依赖门禁通过；路由 17/0、
  主模型菜单 3/0、账号凭据 3/0，使用本地假 HTTP 与假 Key。
- Android Java 七套 40/0；实际 ARM64 与 ARMv7 两 ABI 原生构建成功，
  最后两件共享实现增量也已重新构建通过（12 秒）。
  没有安装或宣称 Android 真机 UI 验收。
- Apple 六件源码按 before/after Git blob 与 LF SHA256 逐项核对、只应用一次。
  Apple owner 使用完整新 ABI 验证 iOS 两项业务 2/0，正常 build-for-testing 成功；
  原横屏观察未实际旋转，保留失败，不能写成横屏已验。
  PBX 两个 build 字段从 228 改为 229，marketing 仍 3.0.1。

真实供应商探针与线上两 JS/受限 env 的部署结果见同目录
`2026-11-19-cloud-responses-credentials-deployment.md`。
OpenRouter 两次真实工具往返没有返回 reasoning/encrypted item，
真实签名续接未实测；该路径由本地对抗 fixture 覆盖，不能混称为真实供应商验收。

## 运行与交付边界

Windows 原已安装进程未停止、未替换；其公有 3.0.1 包不包含本轮功能。
本次没有修改公开 release/tag 或资产，也未触发真实模型 UI 任务。
iOS 229 正常 Release、安装和独立启动由 Apple owner 在源码提交后执行，
此记录不预先宣称手机更新完成。测试日志与精确源包保留于任务私有 cache。

## 提交后的 Windows 候选

实现提交 `70ee22f62251740bf53fd684a4aa6baef3409734` 已正常推送并独立核对远端 main。
正常脚本生成私有候选 `MaiChat-win64-v3.0.1-20261119-70ee22f6.zip`：
242,401,070 字节，SHA256
`31d9e671fa8212399af8888a6ebc686b6f24acb4ba9fb0125c6addc968f4d2bb`。
包内 exe SHA256 为
`55f9c5eacc00f397c20fdeceedf8f94a5032c5ea8a04eb6723e98f4cde176dc0`。

staging 和实际 ZIP 提取目录各运行一次 `--smoke`，退出均为 0
（1.947 秒、3.890 秒）；两次 PATH 只有 Windows/System32，已清除 Qt 插件环境变量。
三产物的 114 个 PE 导入和 Qt 插件检查通过。
这是正常启动/依赖检查，不等于登录后真实供应商任务验收。
现装 `D:\Program Files\MaiChat\maichat.exe` 的 PID 59396 保持运行，未用候选替换。
候选留在 `.cache/openrouter-fable52-20261118/windows-candidate-70ee22f6/`，未上传发布。
