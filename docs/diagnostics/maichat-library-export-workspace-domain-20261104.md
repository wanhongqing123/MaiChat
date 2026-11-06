# 相册导出 ID 与工作区路径的参数域校验

日期：2026-11-04。定位提交：`eee43402aff22caeb4b0c2688e381ced569f7e63`，改动尚未提交。
所有执行仅使用自建临时工作区与仓库公开 512×512 JPEG；未访问系统相册、用户工作区或账号数据。

## 复现与定位

原单次本地模型选择基线的用户输入为“工作区照片：portrait-source.jpg。美白一下”，模型生成
`mobile_export_photo_original({"id":"portrait-source.jpg"})`。更新工具描述后，该单次选择仍如此；
这只能说明首步错选，不能单独据此判定完整 Agent 会话无法恢复。

原 `MaiMobileHostTool::execute` 对两个 export-original 别名直接分发，未校验已知工作区文件。
隔离普通入口探针确认：即使该 JPEG 确实存在，未注册平台处理器时仍返回真实的
`not_configured: the MaiChat host tool handler is not registered`。
iOS 的 `MobilePhotoTools.exportPhotoOriginal` 则先请求相册授权，再用 `PHAsset.fetchAssets`
查询资产，最后才可能报告授权库中不存在该 ID。不能在 Mac 测试中伪造这个平台错误或预览成功，
声称已经验证模型恢复。

实际边界证据、公开 JPEG 摘要与命令：
`/private/tmp/maichat-beauty-export-boundary-20261104-5sqw2drp/evidence.json`。
源文件和工作区副本的 SHA-256 均为
`7de7ed51a1594fff247f4cae2301eceacf5313d6011e37b4a4c8733f7bb72c07`。

## 最小修复

两个导出别名在进入分发器之前，共用现有 `maiResolvePathWithinRoot` 解析 `id`。
只有可证明位于当前工作区内的已有普通文件才返回 `InvalidInput`，说明应将同一个准确路径
交给适合的工作区图像/视频操作，该文件不需要再次导出或请求相册权限。
不扫描目录、不探测越界文件内容、不自动调用替代工具，也不改写模型的选择。

合法不透明资产 ID、缺失文件名、目录和不能证明位于工作区内的参数仍原样分发。
工具名称、描述、Schema、完整工具目录和审批策略均未改变。普通文件身份查询沿用
`MaiFileSystem`，不使用 `std::filesystem`。

## 红绿与维护回归

私有对象替换对照保留相同断言：原实现 8 项失败，两个错用文件名的请求均到达分发器一次；
新实现全部通过，两个别名均在分发前给出参数错误，回调数为零。记录型宿主仅用于单元测试，
不是端到端平台结果的替身。合法不透明 ID、缺失文件名及目录的参数保持原样。

对应断言已合入现有 `MaiMobileAgentTests.cpp` 的 `libraryExportDomainContract`，并补充工作区
外自建测试文件的透传与原文件保持检查。完整移动适配测试在 iOS 宏及通用配置下均通过。
编译命令、输出和二进制摘要位于同目录的 `mobile-tests-results.json`、
`mobile-tests-build-commands.json`。最终输入绑定：`product-guard-freeze.json`。

## 真实 MainAgent 闭环尝试及边界

只运行一次真实 C ABI / SQLite / MainAgent / 本地工厂客户端，保留实际完整 128 工具，
32K、温度 0、seed 0；上限为 6 次工具调用或 240 秒。没有预置工具结果或修改提示词。
诊断宿主具备真实 Vision 检测、C ABI 修图、PNG 发布和结果登记适配；媒体交付使用核心自身的
`agent_send_media`。超出本次授权范围的外部、系统库或平台 UI 操作会在执行前取消，不能伪造成功。

该实际会话首步生成 `mobile_preview_image({"path":"portrait-source.jpg"})`，在平台 UI 边界取消。
实际模型耗时 32.39 秒，Metal 权重占用 1320574208 字节；没有执行预览、相册查询、网络、修图
或交付，原 JPEG 保持不变。因此该用例未验证“错导出之后恢复修图和交付”，也未证明自然会话
最终失败；不能把它标成端到端通过。
记录位于 `/private/tmp/maichat-whitening-real-agent-20261104-zkh9ygsd/` 的
`final-run-evidence.json`、`run/requests-and-host-results.json`、`run/snapshot.json`。

单次选择助手与真实 MainAgent 的基础指令、用户文本、工具顺序及解析后的 Schema 相同，但前者
经 nlohmann JSON 重建会排序参数对象键，真实注册表保留原始 Schema 顺序。上游 `common_json`
保留插入顺序，所以二者不等于逐 token 相同的 MainAgent 重放；不据此宣称随机性或修复效果。
本次未追加提示词、改变工具目录或重试寻找通过结果，也未进行 App/设备验收或提交、合并。
