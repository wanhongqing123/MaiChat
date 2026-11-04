# iOS 修图结果留在聊天中（2026-11-04）

## 操作与预期

用户反馈：美颜完成后只需在聊天中展示结果，不要自动打开全屏；用户点击图片时再预览。
用户明确请求的其他预览、播放工具应继续可用。

产品复现步骤：在 iOS 创作中心提供一张照片，要求美颜，等待处理和交付完成。
预期仍在原聊天，能看到成品图片卡片；点击成品才进入全屏，关闭后回到原消息。
随后明确要求打开图片预览，应仍能执行独立预览工具。

源码审查时间：2026-11-04 00:48 CST 起。基线提交：
`5216279acbfdf5a05944c6f0d00ff0de8a142b63`，工作区同时存在其他未提交任务。
本记录只涵盖美颜交付指引、对应 iOS 结果和 Portrait 测试；未修改主模型、版本或构建。

## 定位证据

美颜调用链：`AIMobileHostToolProvider.execute` 的 `mobile_beautify_image` 分支将
`operation` 设为 `beautify`，经 `MobilePhotoTools.transformImage`、`beautifyImage`
调用普通 `MaiPortraitPhotoProcessor.prepare` 和 `publish`。处理器写出完整新图和蒙版，
登记结果，再返回路径及尺寸。它本身不设置预览状态。

旧指引却要求模型继续打开预览：

- `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` 的 `mobile_transform_image`、
  `mobile_beautify_image`、`mobile_reshape_body` 描述要求处理后预览；
  `mobile_preview_image` 描述也要求在读取或变换图片后使用。
- `MaiPortraitPhotoProcessor.publish` 返回 `next_tool=mobile_preview_image`；
  `MobilePhotoTools.transformImage` 的其他图片变换及美体成功结果同样返回该值。
- `mobile_preview_image` 进入 `MobileImageTools.previewImage`，直接设置
  `AIAssistantModel.previewImage`；`AIAssistantImagePreviewLayer` 随即创建全屏
  `MaiLocalMediaPager`。这才是实际 presentation 入口。

未发现消费 `next_tool` 并自动执行工具的宿主代码。以上证明源码存在强制预览式引导，
并明确实际全屏入口；“模型在用户该次会话中遵循引导后调用预览”仍是调用链推断，
本轮未读用户数据库、请求日志，也未运行用户会话复现。

正常 `agent_send_media` 成功结果由 `AIAssistantView.mediaArtifact` 解析为聊天图片卡片。
只有卡片的点击回调进入 `openImage` 才设置预览状态。`view_image` 则向模型返回有界
视觉输入，不设置 iOS 全屏状态；二者用途独立。

## 本次修改

四项共享工具说明统一改为：成品用 `agent_send_media(file_path=path,type=image)`
交付当前 AI 聊天，用户点击图片查看；只有用户明确请求时调用全屏预览工具。
iOS 美颜发布和其他图片变换／美体成功结果改为 `next_tool=agent_send_media`，并返回
同义 `delivery_hint`。未改图片算法、文件提交、训练记录或任何预览／播放实现。

## 真实处理结果与测试边界

新增 `PortraitUITests.testPortraitResultStaysInChatUntilImageIsTapped`。
使用既有 Debug 模拟器 fixture：公开 `portrait-test-face.jpg` 被复制到独立临时工作区，
真实调用 `mobile_beautify_image`，参数为 `smoothing=0,brightening=0,warmth=0,
whitening=0.6,use_personal_model=false`；继续走宿主、C ABI、C++ 与实际图片编码。

fixture 的 `resultNextTool` 直接读取上述真实工具响应中的 `next_tool`，通过
`portrait-fixture-next-tool` 暴露给 AX，未填充测试期望值。测试先断言该值为
`agent_send_media`、全屏不存在、成品缩略图存在；随后点击缩略图，断言进入预览并可
关闭回原消息；最后点击既有“预览修图结果”按钮，验证明确请求时仍可用宿主预览工具。
旧结果的 `next_tool` 会使新测试失败，但这不是旧 App 已实际运行的红测记录。

既有 `installImageZoomUITestFixture` 会手拼 completed `agent_send_media` 媒体消息，
直接注入 UI 模型。本轮保留该机制，并纠正其调用点原先“真实交付片段”的误导注释。
图片产物来自真实美颜处理，缩略图和预览使用生产视图；该消息边界不覆盖真实
`agent_send_media` 执行、Agent 持久化、模型下一步工具选择或真实账号聊天。
本轮没有新造工具业务结果、没有新增工具或 C ABI，也没有启动模型服务或付费任务。

## 验证状态

- 四个修改过的 Swift 文件完成语法 parse；定点 diff 和空白检查通过。
- 尚未构建、安装或启动本次 App；没有实际运行新增 UI case，也没有查看其截图／录屏。
- 待 root 统一冻结构建后，在 iOS 模拟器运行新增 case 并查看点击前、预览中和关闭后的
  画面；确认产物和运行进程对应上述源码。实际 Agent 对话、真机和 Android 均待验证。

共享工具说明也影响 Android，但其现有成功响应仍可能返回旧预览指引；本次未修改
Android 宿主，不能据此宣称 Android 行为已修复。

## Root 统一构建后的实测追加（02:14 CST）

2.2.4/179 正常模拟器 App 的 `testPortraitResultStaysInChatUntilImageIsTapped`
已实际运行通过（16.931 秒）。结果包为
`/private/tmp/maichat-primary179-composer-portrait-regression-20261104.xcresult`，
录屏同名前缀 `.mp4`。Root 已查看导出的 `portrait-result-in-chat-before-tap` 和
`portrait-result-preview-after-tap` 原始截图：点击前仍在创作中心、成品卡片在正文内，
点击后才出现图片预览。没有以 fixture 合成的媒体消息冒称真实 Agent 交付闭环。

同批忙碌输入栏和连续五条消息、手动相机三项均通过。独立引擎自测失败在测试滚动容器
定位：Picker 离屏被系统回收后 descendant query 失效；截图 AX 树实际已经显示真实
生成完成。本轮给 Form 增加稳定 AX 标识并定点回归，不能把该失败写成 App 推理失败。

179 已构建、严格签名检查并安装到 iPhone 14 Pro Max，但启动实际被设备 Locked 拒绝，
尚未进行真机美颜任务或全屏交付验收。后续账号删除屏障和远端合并后的最终产物与检查
由 Root 另记；本段只记录明确对应 179 的既有结果。
