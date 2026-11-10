# Windows 电脑操作：原生保存与重新打开

## 操作与预期

2026-11-10，Windows，main 基线 `7dffeb5bf8b45a1ebec6a780106b86dc619dabde`。
本轮仅增加 `DesktopComputerDisplayNativeTest.cpp` 的 `--workflow` 验收模式；生产工具、
平台后端、Core、CMake、版本和现装程序不改。原默认 click/type/Enter 用例保留。

自行启动一个带真实 `QPlainTextEdit` 的独立进程，通过普通 desktop computer API：

1. 原生点击编辑区、输入两行中英文。
2. Ctrl+Shift+S 打开真实 Windows Save As，输入全新临时文件路径，确认。
3. 独立读取磁盘 UTF-8 字节并核对，不只相信 UI 状态。
4. 用 Ctrl+A/Backspace 清空编辑器，确认磁盘文件没有改变。
5. Ctrl+O 打开真实 Windows Open，输入同一完整路径，再次打开。
6. 核对实际 UIA 文本、磁盘字节、保存/打开次数与路径；保留逐步新帧回执。

模型和该普通 API 共用生产实现，但本轮执行器是确定性序列，`visual_model=false`。
不能将其称为真实视觉模型自主完成任务，也不能扩大成任意应用均已验收。

## 归属与失败证据

每次采集和输入都要求本次 QProcess、真实根 HWND、前台和原生 modal owner 链有效，
物理输入已释放；覆盖整块显示器。其它进程的可见遮挡会取消。只在本次临时目录新建指定文件，
拒绝覆盖已有文件，不改用户编辑器、浏览器或聊天。

首次几次运行在输入和采集前返回 77，不能当作 native PASS：安全元数据显示唯一阻挡窗口为
`ThumbnailDeviceHelperWnd`、1×1、`DWMWA_CLOAKED=1`。它虽然 IsWindowVisible，但对用户不可见；
仅排除 DWM 查询成功且 cloaked 非零的窗口，未知或查询失败仍严格阻挡。
依据为 [Microsoft DWMWINDOWATTRIBUTE 文档](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)。

后续真实运行已保存并清空，但 Open 文件名输入被取消。同步取消回执证明阻挡来自本次
子进程自身的 `SysShadow`（同 PID、无 GW_OWNER 链），不是另一个应用。修正验收夹具的像素
归属：本次仍运行子进程的装饰窗口可视为自有像素；前台、modal、点击与输入的严格 owner 链
检查保持不变。没有类名、尺寸或任意进程白名单。生产取消逻辑没有修改。

文件名使用有界条件轮询，期间不追加输入，要求原 modal 未变且 UIA 读回完整路径精确匹配。
输入/取消诊断仅记录已验证窗口的结构信息，不读取外来标题。原失败回执完整保留。

## 实际结果

- 同 normal MSVC target 构建 exit 0；编译需原 `vcvars64.bat` 环境，首次裸命令缺标准库
  `type_traits` 的环境失败另存，不计代码 RED。
- run8 实际完成 Save As/clear/reopen，exit 0；一份最终截图处于 Windows 对话框正常淡出过程。
- run9 保持原功能断言，仅在最后通过既有 `observe.wait_ms=500` 再取正常画面，exit 0，约 4.44s。
- 实际子 PID **15376**，保存一次、重新打开一次；两个真实 modal 都是 `#32770` 且 owner 链相同。
- **1 resident worker / 1 capture session / 95 frames**。没有启动第二条截图采集链。
- 实际 UIA 两行文本和磁盘字节相同，SHA256：
  `f898e00a0a9a6dd94a2dbe6da5f1f95a4759eb6888354f9ad55934d5bc08477f`。
- 最新同 binary 的原默认 click/type/Enter/native readback 再跑 exit 0，约 1.14s。
- 开发者已亲自查看输入完成、清空、重新打开与最终转场结束画面，文本与控件正常。

运行 binary SHA256：`50e1e99fb71db95a0879dc21266ad36c9565c922720460c7e35dc2fdce48905c`。
源文件 raw SHA256：`92ae9cd9b1f05c5f5d86e0480f8776cfc3d03c82aa765de726991aaf8f8b7648`。
原日志和 JSON 在 `.cache/computer-use-20261110/`；最终证据目录 `document-owned-9`。
原生文件对话框可能显示宿主 Windows Quick Access 名称，因此原始截图仅留私有 cache，未公开上传。

下一步仍是受控浏览器会话与真实视觉模型的任务验收；长按状态、剪贴板、接管/恢复等通用能力
按既有计划继续。本结果证明原生跨窗口文件工作流，不代表总体“像人自由工作”已全部完成。
