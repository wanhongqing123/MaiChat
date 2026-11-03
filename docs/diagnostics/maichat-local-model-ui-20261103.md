# iOS 本地模型选择与普通推理测试

时间：2026-11-03。源码基于 main 的本轮协作工作区；实际 App 构建号、安装与画面由主任务的最终构建回执记录。

## 可复现操作和预期

1. 正常创作中心右上角“•••”菜单进入“本地模型”。沿用 Form、Picker 与导航栏“完成”；功能展示卡片没有新增点击行为。
2. Debug 包默认可选“内置英文测试模型 · 1.2 MB”。页面明确它是英文引擎测试，非聊天/OCR 模型，并展示上游 CI 警示、文件大小和 SHA。
3. 直接点击“运行本地测试”。普通 Swift API 使用固定本地文件、原始英文补全文本、context 256、2 个 CPU 线程、最多 32 个输出 token；展示真实文本与 token 数。过程不调用 Agent、云端推理、下载或训练。
4. 完成后再次运行；共享 runtime 可以复用模型，但每次生成清空上下文 KV。点击“卸载驻留模型”后状态应变成“未加载模型”。取消/关闭页面只取消本页自己的生成 Task，不停止 Agent 请求。
5. 从“文件”导入用户 GGUF。二进制类型允许 `UTType.item`，不用文件扩展名作为格式证明。可选择文本模型和可选 mmproj；图片测试还需匹配的 mmproj/主模型及已支持架构，纯文本不加载 mmproj。主聊天模型不会随此处选择而切换。

## 文件与资源边界

- `AILocalModelStore` 是 MaiChatCore 的普通 Foundation actor。导入开启/平衡 security scope，在后台以 64 KiB 块复制并计算 SHA，保存到 `Documents/MaiLocalModels/<UUID>.gguf`。源文件不改动；新文件不覆盖已有路径。没有模型替换/删除 API，避免改写正在 mmap 的文件。
- 先检查普通文件、GGUF v2/v3 和有界元数据，再复制后复查。最大文件 16 GiB、目录 128 项、catalog 256 KiB、容器元数据/描述扫描 64 MiB。目录或源文件的叶子符号链接被拒绝；catalog 中只接受 UUID 相对文件名。GGUF 容器通过不等于架构/内存适配通过，完整模型由共享 runtime 再验证。
- catalog 只保存本机显示名、相对文件名、用途、字节数和导入时 SHA/选择，不保存外部 security-scope URL、提示词、账号令牌或聊天内容。损坏目录失败保留现有文件；导入失败清理本次临时文件。
- `ensureBundledTestModel(bundle:) -> URL?` 把固定测试资源安装到 App Documents，以便普通 Swift 与受 NSHomeDirectory 边界限制的 Agent 使用同一文件。校验固定 SHA；存在正确文件时复用 inode，存在错误内容时拒绝覆盖。并发 ensure 只允许原子发布/复用正确文件。
- 原始 `karpathy/tinyllamas` 官方模型卡为 MIT；GGUF 来自固定 ggml-org CI 仓库，原 README 明确不要用于生产。完整来源、MIT notice 和 CI 警示在 `MaiAgent/models/local-test/`。仅 `CONFIGURATION=Debug` 复制这个测试资源，Release 不默认包含 CI 权重，仍保留用户导入功能。没有 App/构建时下载。
- 可选图片只处理用户选取的单图：20 MB/8 MP 上限，在后台按 EXIF 转正为标准 PNG，不改原图。临时图片只保留到页面关闭/本页正在执行的请求返回，不将其写入模型 catalog。

## 定点证据

- 独立 Mac SwiftPM harness 编译实际 `AILocalModelStore.swift` 和仓库 `AILocalModelStoreTests.swift`，不链接/调用推理引擎、不发网络。
- 七项真实临时文件测试：跨块复制与同名二次导入不覆盖、持久化与 mmproj 分离、坏 magic/截断/长度炸弹、目录/符号链接与 catalog 穿越、16 GiB+1 稀疏文件/预取消、真实固定模型安装/并发 ensure/已有 inode 保留/错误内容拒绝覆盖。
- 首轮同路径大小变化测试真实失败：Foundation URL 缓存旧 fileSize 导致二次导入误判。改为每次清除 resource values 缓存后，原断言通过。红日志：`/private/tmp/maichat-local-model-store-tests-red-20261103.log`。最终日志：`/private/tmp/maichat-local-model-store-tests-final-20261103.log`。
- iOS 16.0 arm64 device/simulator 独立 typecheck 实际 Store、新 View、原 Swift runtime 和真实 C ABI 头，未构建/安装/启动 App。回执：`/private/tmp/maichat-local-model-ui-typecheck-20261103/final-results.json`。
- 实跑新增资源 shell 块：Debug 复制模型/来源/许可六文件；Release 零文件；错误 SHA exit 1 且不创建输出目录。回执：`/private/tmp/maichat-local-model-resource-validation-20261103.json`。

本记录不代替正常 App 的实际菜单、导入器、屏幕画面和真机测试；该步骤由主任务随后统一构建验收。没有用户 OCR/VLM 大权重，不宣称其质量或手机内存验收通过。16 GiB 是有界导入门禁，不是手机可运行模型大小承诺。

## 最终正常 App 实际回归与画面复核

- 首次正常 App 178 回归 `/private/tmp/maichat-batch178-model-camera-ui-20261103.xcresult` 中，case 实际在默认模型断言失败：Picker 的已选内容在 AX `label` 中，测试错查了 `value`。原始 AX 文件 `/private/tmp/maichat-batch178-model-camera-shots-20261103/A3E47CD6-B132-4779-8883-310C39E61E42.txt` 显示 `文本模型, 内置英文测试模型 · 1.2 MB`。随后只改测试为 `label CONTAINS`，保留默认选择断言，产品 Picker 未为测试变更。
- `2026-11-03 22:53:15–22:53:41`，冻结目录 `/private/tmp/maichat-image-spinner-final178-20261103/` 的正常 App `2.2.4 (178)`，模拟器 UDID `19D02616-576C-43A6-8D92-2AA160F6F48E`。独立 `LocalModelsUITests/testBuiltinModelGeneratesTextAndUnloadsFromCreationCenter` 实际运行 `26.705` 秒并通过，单 case `1 test / 0 failures`。
- 日志、结果、完整录屏分别为 `/private/tmp/maichat-batch178-final-ui-20261103.log`、同名 `.xcresult`、同名 `.mp4`。同批次另有创作中心用例失败；本地模型的具体 case 通过不代表整批测试通过。
- 已实际查看生成与卸载两张 xcresult 截图，并从完整录屏约 `205–231` 秒抽帧核对正常创作中心→“更多”菜单中的“本地模型”→默认内置模型页→滚动到运行按钮→生成结果与驻留状态→卸载→“完成”回创作中心。页面内容与 sheet 衔接在这些阶段保持可读；抽帧中没有停在空白页或加载错误。菜单图 `/private/tmp/maichat-final178-model-menu-20261103.png`，阶段图 `/private/tmp/maichat-final178-model-stages-20261103.png`，返回原页图 `/private/tmp/maichat-final178-model-returned-root-visible-20261103.png`。
- 生成截图 `/private/tmp/maichat-batch178-final-shots-20261103/07FF968B-A71D-4396-9A7E-96A9B7E367EA.png` 显示实际英文补全文本、`输入 11 · 生成 32 token · 达到 token 上限` 和 `运行库空闲 · 模型已驻留 · context 256`。卸载截图 `F464FA5C-D45D-4EEE-847F-B3CF7DDEB9D2.png` 显示 `运行库空闲 · 未加载模型`、已禁用卸载按钮及“驻留模型已卸载”；生成结果仍保留可读。
- 运行按钮附近另外检查了约 `219–223` 秒、每四分之一秒的连续抽帧，保存为 `/private/tmp/maichat-final178-model-run-transition-20261103.png`。小模型很快完成，抽帧未覆盖可辨识的长时间运行/取消状态；这次确认了真实本地生成、结果呈现和卸载退出，不扩大到大模型耗时、外部文件导入器、mmproj 图片推理或真机内存验收。没有触发 Agent、云端模型或付费请求。
