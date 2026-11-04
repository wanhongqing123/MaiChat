# 拍照成功但对话不显示照片（2026-11-04）

## 现场与预期

用户在 iPhone 14 Pro Max、iOS 27.2、MaiChat 2.2.4/186 的创作中心输入“打开相机进行拍照”，
按快门完成后，`mobile_take_photo` 显示 completed，回复只有 `photo-*.jpg` 文件名。
再次输入“看下拍摄的照片”后，`view_image` 显示 completed，但没有图片卡片和后续回答。
用户 13:55 截图保留在远程 IM 原始记录；没有读取该照片、用户会话数据库或凭据。

预期：完成单张拍摄后立即在当前助手消息显示缩略图，不依赖模型再调用投递工具；只有
用户点击缩略图才进入查看器，退出回到同一条消息。读取历史回执也显示同样的卡片。
未完成和取消的拍摄不显示成功图片；输入草稿不增加附件；切换会话不串图。

修改前 HEAD 为 `0c610bcc`，主分支 `main`。本记录涉及工作区的 187 候选，尚未将构建或
安装当作已完成，后续追加实际产物、运行进程与验证结果。

## 根因证据

1. `AIMobileHostToolProvider.takeManualPhoto` 已保存工作区 JPEG，返回普通工具回执
   `{path,mime_type}`。C ABI `serializeMessages` 与 Swift `AIPart` 均保留该 output。
2. `AIMessageRow.mediaArtifact(for:)` 修改前只接受 `agent_send_media` 的
   `delivery=current_ai_session`，不识别完成的 `mobile_take_photo`，因此没有图片卡片。
   这是展示接线遗漏，不是照片未保存或相机再次失效。
3. `view_image` 的 `successWithImages` 用于模型视觉观察，不等于用户已经看到图片。
   当前本地主模型没有视觉投影器，`chatInputs` 对非空 images 明确返回 NotSupported。
   旧注册表仍向该模型公开 `view_image`，使成功读取文件后的下一轮本地请求无法进行。
   独立有效 PNG 与预览回调复现了旧代码返回图片输入；针对性测试在旧实现出现 9 条失败。
4. 用户 14:16 又提供了明确红字 `Local model cannot render the complete chat and tool history`。
   新用户轮次已经不携带旧像素，不能继续把这个报错归因为视觉输入。独立合成历史经真实
   `MaiContextBuilder` 后，旧图片省略说明成为第二条 System；精确 GGUF 的 Qwen3.5 模板
   在第 85 行抛出 `System message must be at the beginning`。完整 129 工具目录和只有
   `tool_search` 的发现目录均能在没有该说明时渲染同一段历史，加入说明后两者都失败，
   排除了“历史工具定义缺失”的初步怀疑。这个对照只加载词表，没有 context、decode 或
   工具执行；没有访问用户会话。证据位于
   `/private/tmp/maichat-qwen35-history-render-20261104-_b6294se/red-result.json`。

## 修复边界

- 复用已有 `AIWorkspaceImage` 卡片和点击查看器，直接从完成的拍照回执渲染，不给本地模型
  附加视觉输入，不自动进入全屏。相机工具执行详情仍然保留。
- 回执仅接受工作区内的相对 JPEG 路径，路径及符号链接经过同一解析策略；相同规范路径
  在拍照和显式 `agent_send_media` 同时出现时只显示一张，优先保留显式投递的说明。
- 工具可用性按实际 Session 模型同时过滤声明和执行入口。`view_image` 对当前 `local:`
  文本配置和 `glm-5.3` 不公开，直接调用也在文件读取前明确拒绝。图片编辑和展示能力保留。
- 本地基础指令明确“展示不等于已经理解图片”。历史图片省略提示不再要求调用可能不可用
  的 `view_image`。不伪造照片分析，不自动切换云端，不修改旧工具回执或用户图片。
- 本地模板适配将真实 System 按原顺序合并为一条前置 System，新增分隔符计入原有 2 MiB
  输入预算；其余角色、调用 ID、参数和回执完全保持。这个修改只作用于请求副本，
  不删除用户历史，也不通过补造工具定义或忽略错误来绕过模板。
- 跨设备历史同步现有规则会清空工具 output；本次只修所属设备上的实时与历史显示，
  没有增加照片上传或跨设备同步路径。

## 验证记录

Swift 六个文件语法解析通过；独立 Foundation 检查运行了四项路径和回执用例。这不等于
iOS XCTest 或正常 App UI 验收。

第一轮 187 正常 Root 的模拟器验收已通过 **7/7 UI + 4/4 iOS XCTest**。UI 执行 165.426s、
xcodebuild 174.593s；路径单测 0.015s。新 UI 用例经正式 Swift 宿主等待合成快门，实际保存
JPEG 后把真实回执放入普通消息行，Codable 重载同一回执，再点击图片查看、退出。没有
编造模型回答，也没有传感器或付费调用。根任务已查看缩略图、显式查看、返回三张截图及
最后 8 秒录屏抽帧，图片在交互过程中持续存在，未出现空白或自动全屏。

UI 日志 `/private/tmp/maichat-photo187-regressions-ui-20261104.log`，对应 xcresult、mp4 同前缀；
附件目录 `/private/tmp/maichat-photo187-ui-attachments-20261104`。iOS 单测记录
`/private/tmp/maichat-photo187-policy-unit-20261104.xcresult`。最初将单测放入主 UI scheme 的
启动退出 70，没有执行测试；保留失败日志后改用既有 `MaiChatCoreTests` scheme，4 项确实
执行通过，不将退出码或零测试当作验收。

这轮 UI 所用冻结包先于新增历史模板修复及 Debug 探针模型标识修正；并未安装到手机。
最终源码需要重新构建、再验新图片路径。真机安装及最终核心回归结果随后追加。

最终源码冻结于 `source-freeze-final.json`；相比第一轮 UI 仅增加本地 System 适配、对应
测试和 Debug 探针使用正式 `local:` 标识三项变化。相同原生注册目录的公开 C ABI 证明
本地文本配置为 128 项，排除 `view_image`，保留展示与相机工具；支持视觉的云端测试配置
仍为 129 项。该目录验证不执行模型或工具。云端配置有一次被测试桩明确拒绝的
`mobile_ark_service_config` 控制面查询，不能称为“零宿主回调”。

历史模板独立回归使用精确 GGUF 词表，修复前两个略图场景都抛同一 Jinja 错误，修复后
**4/4 可渲染**；逐项比较确认非 System 数据完全一致。真实 32K/Metal 推理五组场景通过，
覆盖首部、中段、尾部、空 System、无 base 及旧读图后追问。完整 128 项工具和正式基础
指令下的自然追问生成完成，耗时 5.545s、无渲染错误。保留原文中“网络延迟”的未经验证
猜测，不将“返回非空正文”算作图片展示或图片理解通过。原始证据和所有哈希见
`/private/tmp/maichat-qwen35-history-render-20261104-_b6294se/evidence.json`。

历史修复后的最终模拟器正常 App 再次完成同一拍照卡片、历史恢复、点击查看、返回用例，
**1/1 通过**，执行 21.820s、xcodebuild 25.257s。根任务已查看该构建的三张实际截图；
证据 `/private/tmp/maichat-photo187-regressions-history-20261104.{log,xcresult,mp4}` 及
`/private/tmp/maichat-photo187-ui-history-attachments-20261104`。原 7 项 UI 与 4 项 iOS
路径单测也保留，不将不同冻结构建冒称同一个包。

最终核心重新编译并通过 Core8 加三项针对性套件 **11/11**（64.57s）；完整 Mobile C ABI
iOS 宏和通用配置分别 10.51s、10.38s，通过新的虚表 ABI 重编译、最终 System 归档重链接。
证据目录 `/private/tmp/maichat-local-view-image-capability-20261104-wh4u8n7e/post-system-normalization`。

同轮还收紧了 `mobile_screen_frames`、`mobile_browser` 的按操作必填字段，普通宿主调用
保持原平铺参数；实际 129 工具 XML 语法验证 22/22。DeepSeek 的可选 `localization` 明确
为已知 IPv4/IPv6，不是语言或地区标签，非法值在 HTTP 和凭据读取前拒绝。IP 格式仍由
运行时校验，不宣称当前 XML 语法实现已强制 IP 格式。这些证据分别保留于
`/private/tmp/maichat-screen-browser-grammar-20261104-rynv2v9f` 和
`/private/tmp/maichat-deepseek-localization-20261104-6q8zxl4v`。

## 最终安装与运行绑定

最终 iOS **2.2.4/187** 已安装到上述 iPhone，安装回执退出 0；设备应用查询再次确认版本，
正常启动且无 QA 参数，进程 **28156**。安装目录 UUID
`AB8E22D0-4616-480C-BE7F-06DDDC786CB8` 与启动及设备进程路径相同。
冻结包 11 项 Mach-O 独立严格签名、外层 deep/strict 均通过；debug dylib UUID
`0FBCA499-BF69-3286-B0FC-80AF20FAEC7B`。随包 Qwen3.5 2B 仍为 1396198496 bytes、
SHA256 `57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。

回执位于 `/private/tmp/maichat-photo187-device-final-artifacts-20261104/{evidence,runtime-evidence}.json`，
原始设备安装、版本查询、正常启动和进程 JSON 以 `/private/tmp/maichat-photo187-device-` 为前缀。
本次没有删除用户会话或照片。未代替用户在手机上按快门，也未读取其实际照片或数据库；
图片展示在正常模拟器实测，历史模板在真实 Mac/Metal 推理实测，不能冒称完整真机传感器
和旧用户会话的端到端验收。此前 186 的 GPU 实机 QA 仍是已有证据，不重标为 187 实测。

Android **2.2.24/261** 已在自有 emulator-5580 更新、正常启动，PID6981，设备 base.apk
SHA 与冻结包 `80b3b6013a9f540f82d5e815a52d91d1f359614f385a9f4120edfc1f428b10b2` 一致。
空登录页正常，没有登录或验证登录后相机/模型。Desktop **2.2.44** 的 9 项 Mach-O、
外层签名及独立无网络 smoke 通过（3.286s），用户 `/Applications` 的 PID43461 未动。
平台证据：`/private/tmp/maichat-photo-platform261244-187-final-20261104-jkowjd0l/final-evidence-manifest.json`。

最终审计发现两项新增测试 fixture 用了旧式文件写入，随后仅将其改为 `MaiFileSystem`，
断言不变。原 as-built 717 输入中正好这两项测试变化，其余 715 项不变，App 产物没有
重写。新旧哈希及两目标重跑记录独立保留于
`/private/tmp/maichat-test-style-only-20261104-wi68syj5`，不得冒称测试源码与旧输入完全相同。

证据目录：`/private/tmp/maichat-photo187-20261104`；原始输入冻结于 `source-freeze.json`。
Swift 针对性检查位于 `/private/tmp/maichat-photo-artifact-187-20261104`；核心旧实现红灯位于
`/private/tmp/maichat-local-view-image-capability-20261104-wh4u8n7e/red.log`。
