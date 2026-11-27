# computer 工具命名与采集边界

时间：2026-11-26；源码为 main 基线 `be9711aeac49e2be0c1c2f37069cc910ff90fc90` 加当前工作区。用户要求改掉 `desktop_computer` 名称，并指出模型频繁另调 screenshot、顶部一直显示屏幕采集；后续要求先确认 Codex 的截图与模型工具链路。

## 事实与范围

本机 Codex Windows 插件 `computer-use/26.1007.71326` 的 SKILL/API/guidance 明确使用 `node_repl` 中的 `@oai/sky`。调用方通过窗口状态接口选择截图、UIA 文字或两者，再调用鼠标键盘等方法。这是客户端执行的工具流程；OpenAI Responses API 的原生 `computer_call` 是另一种接入方式。详细证据见 `2026-11-26-codex-computer-use-research.md`。

插件说明 Windows 截图使用 Windows.Graphics.Capture，但没有完整公开原生后端。本轮未操作 Codex 桌面或对其采集进程做运行态计量，因此不能断言 Codex 每次截图后销毁会话，也不能断言所有截图会话始终常驻。按需输出截图与底层采集会话常驻不矛盾。

MaiChat 的 `NativeComputerBackend::capture` 和 `captureDisplay` 都调用普通 `maiCaptureScreenshot`，与独立 screenshot 工具共用同一个采集核心。Windows 相同目标复用 DXGI/WGC 会话，macOS 相同目标复用 ScreenCaptureKit 流。顶部提示条反映这个真实状态；原生采集帧不会仅因持续产生就自动逐帧发送给模型，模型请求中的图像来自明确的观察/工具结果。

因此，本轮没有擅自关闭原生采集、隐藏提示条或在一个会话结束时停止所有会话共用资源。持续采集生命周期尚未更改，不能将本轮命名与提示修复描述为“常驻采集已取消”。

## 复现、根因与改动

1. 注册电脑操作工具后按 `computer` 查找，应得到唯一入口，旧 `desktop_computer` 不再作为活动入口。
2. 只注册 computer，不注册 screenshot，用自有记录后端调用窗口 observe/include_image=true，应返回窗口图片、保存路径及快照；同一工具调用 include_image=false/include_elements=true 时不再捕获图片。
3. 系统提示此前仍声称窗口观察不带图片，虽然普通核心及适配器已经支持 include_image。这会把模型引向独立整屏截图。现在明确单窗口任务直接选择返回的 window_id，再按实际需要获取图片或控件信息，不能拿截图工具的无绑定图像代替输入快照。
4. 工具目录原文不可变；仅改注册名称会让已有 v2 目录继续暴露已不存在的旧名称。当前快照键升级到 v3，从 v2 或 v1 追加一次 configuration_update，并保留旧目录、正文及原有工具调用记录。新发现结果使用 computer，不添加第二个执行别名。
5. 活动源码、欢迎页能力卡及测试调用名同步更新。C++ 类名和测试构建目标仍按既有宿主命名，不把名称更改误当作 API 原生 computer 协议接入。

## 验证与证据

证据目录 `.cache/computer-name-20261126/`。RED 阶段先只修改测试并用正常目标构建：目录升级测试 9 个断言失败，正式名称测试 2 个断言失败；日志保留。随后实现正式名称、v3 迁移和提示修正，再使用 `.cache/r311/src`、`.cache/r311/build-msvc` 正常依赖构建应用及相关测试。

最终执行结果以 `green/results.json` 为准，涵盖工具目录、发现运行态、rollout 原文、模型协议、工具循环、JSONL 运行态、电脑工具纯合同、欢迎页及 Controller。正常应用的构建后部署仍执行 Qt 和 PE 依赖校验。`final-normal-source-manifest.json` 记录编译副本输入，不将 main 基线冒充已提交的完整源码。

本轮只运行自有数据及记录后端测试，不向用户桌面注入输入、读取用户像素、改变剪贴板或访问真实供应商。没有停止/安装/替换正在运行的体验包，也没有重新发布 GitHub Release。持续采集生命周期和 Codex 私有后端的资源保留策略仍是明确的验证边界。
