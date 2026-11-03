# iOS 手动拍照误入定时模式

时间：2026-11-03（Asia/Shanghai）。基线提交 `2c823e74a03e17acea5f00baa8ad7dadd77d67c3`，main 协作工作区；修复尚未单独提交。

## 可复现操作和预期

1. 正常 Root App 底部进入“创作中心”，点输入框旁“＋”→“拍摄”。应显示手动快门和取消，不显示“每 2 秒拍摄一张”、自动采样数量或定时停止按钮。
2. 保持超过两秒，应仍等待用户按快门；取消、重新打开后仍是手动模式。
3. 只有显式 Agent 相机请求才进入定时模式；停止、封存并关闭选图页后，“＋”→“拍摄”仍恢复为手动快门。
4. 点击一次手动快门后回到原会话草稿，只新增一张附件；取消不创建附件，也不更改其他 Agent 请求。

## 实际红证据

- 正常 App `2.2.4 (178)`，Debug iOS Simulator，目标 UDID `19D02616-576C-43A6-8D92-2AA160F6F48E`；冻结产物目录 `/private/tmp/maichat-image-spinner-camera-red178-20261103/`，xctestrun 为 `MaiChat_iphonesimulator27.1-arm64.xctestrun`。
- `2026-11-03 22:12:46` 开始运行 `ScreenFrameArchiveUITests/testManualCameraHasShutterAndDoesNotReuseTimedCapture`，确实从正常 Root 点“创作中心”→“展开更多功能”→“拍摄”。首次进入等待四秒后仍无可点击的 `ai-manual-camera-shutter`。
- 断言在 `ScreenFrameArchiveUITests.swift:30` 失败：“手动拍摄必须呈现真实手动分支，不能将 nil target 当成定时模式”；单 case 运行 `16.821` 秒。日志尾部重启 runner 后的“0 tests”不是通过回执；整个测试执行为失败。
- 原始回执：`/private/tmp/maichat-camera-red178-20261103.log`、`/private/tmp/maichat-camera-red178-20261103.xcresult`；截图附件 `manual-camera-first-open` 随 xcresult 保存。
- 已导出并实际查看该截图：顶部错误显示“每 2 秒拍摄一张 · 已拍摄 0 张”和“停止拍摄”，正文是已 opt-in 的蓝色“模拟相机画面”，没有手动快门。导出文件 `/private/tmp/maichat-camera-red178-attachments-20261103/93FA1897-1A91-473E-AA91-0923B5F37045.png`。模拟画面不复现用户硬件上的黑色预览，但确认进入了错误的定时分支。
- 这次回归显式 opt-in Debug 模拟器的合成快门与合成 Agent 帧。未调用真实相机，未请求主模型、云端模型或付费任务。

## 根因与修复

源码根因：`openCamera()` 先写独立 `cameraTarget`，再写 `isCameraPresented`；`fullScreenCover(isPresented:)` 用 `cameraTarget == nil` 推断定时模式。首次 presentation 可读到旧的 nil，于是普通手动入口出现定时 UI，却没有对应工具请求、预览 session 或可工作的停止动作。真实红用例证明该入口没有手动快门；SwiftUI 首次 presentation 的状态读取解释来自调用链与数据生命周期分析。

另一个独立的所有权问题在手动回调：它按当时的全局 `cameraRequestID` 决定照片交给 Agent 或草稿，取消也会失败掉全局 Agent 请求。这样晚到的请求可能接管既有手动操作。

修复以单一 `AICameraPresentation: Identifiable` 同时保存模式及所有者：`.manual(originalTarget)` 或 `.timed(requestID)`，改用 `fullScreenCover(item:)`。手动快门/取消捕获本次 item 身份与原 target，绝不通过全局请求重新选路；旧的手动转交 Agent 图片分支已删除。手动授权期间也保留独占状态，Agent 请求不能覆盖当前手动画面；定时入口必须来自有效请求，停止/异常回调只关闭匹配 requestID 的 presentation。取消后 item 清空，下次打开使用新身份与明确模式。

## 验证边界

- 首次修复后正常 App 178 回归保存于 `/private/tmp/maichat-batch178-model-camera-ui-20261103.xcresult` 和同名 `.log`。相机 case 已通过首次手动模式、两秒不自动采样、取消重开、显式 Agent 定时拍摄与封存、定时结束再开手动模式；最终手动快门回草稿断言仍实际失败，没有把前面的局部通过写成整个 case 通过。
- 该次失败已定点为测试环境缺少文件导入根目录：`/private/tmp/maichat-batch178-model-camera-shots-20261103/9F288506-4162-4C18-AFFC-1BB14DA2D3CF.txt:114` 的实际 AX 显示“创作中心尚未准备好”。合成 fixture 仅设置了 model 的 `workspacePath`，而原生产 `backend.importDocument` 依赖尚未设置的 `backend.root`。这不是滚动/托盘 AX 问题，也没有放宽断言。已仅在 Debug 模拟器、两个显式 manual 测试参数同时存在时准备临时 `Workspace` 的文件导入 root，并等准备完成才置 ready；不初始化 Agent、凭证或网络，继续走原 `importDocument` 和捕获的原 target。
- 同次本地模型 case 停在错误的 AX `value` 谓词，实际 Picker 在 `label` 中显示已选内置模型；已把测试谓词定点改为 `label CONTAINS`，未改产品 Picker。两个 case 均须在下一冻结构建中重新实际跑完。
- 修复源码及两个 UI 测试文件已通过 `swiftc -frontend -parse`；定点 `git diff --check` 通过。这是语法/差异检查，不是 App 编译或实际 UI 通过。
- 同一个 camera case 还覆盖两秒等待、取消重开、显式 Agent 定时采样与封存、结束后再手动拍照，并断言原草稿只有一项附件。新增独立 `LocalModelsUITests.swift` 与 camera 产品逻辑无关。
- 修复后的正常 App 构建、安装、实际运行、绿测回执及截图/录屏由主任务在冻结源码后统一记录；本记录此时不宣称修复已通过实际运行验收。
- 未操作用户真机，未验证真实硬件相机权限、硬件预览和实际快门。模拟器合成帧只验证模式与交互所有权。

## 最终正常 App 回归与画面复核

- `2026-11-03 22:53:41–22:54:13`，冻结目录 `/private/tmp/maichat-image-spinner-final178-20261103/` 的正常 App `2.2.4 (178)`，同一模拟器 UDID `19D02616-576C-43A6-8D92-2AA160F6F48E`。`ScreenFrameArchiveUITests/testManualCameraHasShutterAndDoesNotReuseTimedCapture` 实际运行 `31.732` 秒并通过，单 case `1 test / 0 failures`。
- 原始日志、结果与完整录屏分别为 `/private/tmp/maichat-batch178-final-ui-20261103.log`、同名 `.xcresult`、同名 `.mp4`。同批次另有创作中心用例失败；不能把这个 camera case 的通过扩展为整批测试通过。
- 已实际查看 xcresult 的首次手动、取消重开、定时结束再手动、手动照片回原草稿截图，并从完整录屏约 `231–262` 秒逐段抽帧。画面顺序为正常创作中心“＋”→手动快门→取消回原页→重开手动→取消→显式模拟 Agent 入口→定时画面→停止与一张归档选图页→关闭→再开手动→点击快门→原 A 会话待发送附件区显示一张蓝色合成照片。定时计数从 `0` 增至 `1`；普通手动界面始终只有“拍照/取消”，没有误用定时停止按钮。
- 模式衔接复核补充了录屏约 `249–255` 秒的每半秒抽帧，观察到定时全屏与归档 sheet 的正常进入/退出，未停在黑色 spinner 或错误层级。阶段图 `/private/tmp/maichat-final178-camera-stages-20261103.png`，衔接图 `/private/tmp/maichat-final178-camera-timed-transition-20261103.png`。这份抽帧检查覆盖阶段连贯性，不是逐帧无闪烁证明。
- 最终照片证据为导出目录 `/private/tmp/maichat-batch178-final-shots-20261103/8DABC885-D961-46D9-95A1-17A5420C6D8B.png`，能看见“待发送附件 1”、图片缩略图、原 A 历史和可继续编辑的输入框；日志也实际断言了一个移除附件按钮与定时停止按钮不存在。
- 以上仅确认 Debug 模拟器合成相机的模式、请求所有权、真实文件导入和原草稿回填。未拍摄真人画面，未操作用户真机；硬件相机权限、真实预览、实际快门、旋转与后台恢复仍未实机验收。
