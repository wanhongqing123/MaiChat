# Windows focused native test stderr 误报

2026-11-17，Windows PowerShell 5.1，源码基线 `8aefd68b3097fb37a495846d680d25fc45b5e886`，保留本轮其他未提交工作。问题位于 `MaiChat/desktop/scripts/run-tests-windows.ps1` 的 `TestFunction` 执行分支，不是输入实现或浏览器失败。

## 现场与最小复现

普通 owned Edge 滑块回执 `browser-slider-workflow-receipt.json` 记录 `exit_code=0`、`failures=[]`、页面 `verdict=success`，但程序向 stderr 输出 `native private browser evidence=...`。调用方合并捕获输出时，包装器的全局 `$ErrorActionPreference='Stop'` 将这条正常诊断转换为 `NativeCommandError`，在保存真实退出码之前结束脚本。

用缓存夹具调用实际包装器，CTest 目录查询替换为一条固定的私有测试目录记录；真正运行 Python 原生进程，向 stderr 写相同前缀后分别退出0、7。仍走包装器原有 CMakeCache、真实已安装 `Qt6Test.dll` 和系统错误对话框抑制门禁；调用方采用 `2>&1` 合并流。没有运行 GUI、浏览器或模型请求。

修复前，相同两例分别得到包装器退出1、1，报错位置为原生执行行，错误类型 `NativeCommandError`。不合并流的对照本来正确返回0、7。最初夹具的 PowerShell script-scope 定位错误已单独保留，不作为产品 RED。

## 修复与验证

仅在 focused 原生执行期间把错误偏好设为 `Continue`，在 `finally` 恢复原值并还原工作目录。真实 `$LASTEXITCODE` 决定脚本退出；执行前清除其全局值，避免没有启动进程时复用目录查询的旧退出码。没有修改 CTest 常规分支、Qt 运行时门禁或 native 输入实现。

同一实际包装器夹具验证：

- 原生 stderr、退出0 → 包装器退出0。
- 原生 stderr、退出7 → 包装器退出7。
- 测试路径不存在 → 包装器退出1，保留启动错误。

三例均通过，`git diff --check` 通过。证据保存在 `.cache/run-tests-windows-native-stderr-20261117/` 的 `red-receipt.json`、`green-receipt.json` 及原始输出；修复后脚本原始 SHA256 为 `149503492ea3d313badbbbe10b77edb94ba042e07790ecf3c1ff1820509a0df0`。

此验证覆盖 Windows 包装器的退出码行为；没有据此追加浏览器实测结论。之前 owned 浏览器回执只代表项目自有页面输入工作流，不代表外部验证码验收。
