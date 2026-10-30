# Windows 新仓库干净构建缺少 TRTC 加载期 DLL

- 时间与平台：2026-10-30，Windows 11 x64、VS2019、Qt 5.15.2。
- 基线源码：新仓库 `621111444a34f4f450d73dc0df4faca0b73e472b`，Desktop 2.2.32。
- 工作区：`E:\OpenSource\MaiChat`，独立 Release 构建位于
  `MaiChat/desktop/.cache/build-release-62111144`；原 `multi-ai-code` 工作区未修改。

## 复现与定位

从新仓库配置 FFmpeg、OpenCV、RemoteDesktop 均为 ON 的 VS2019/Qt Release，构建
`maichat`。源码编译和链接完成，但构建后的 `verify-windows-runtime.ps1` 报
`Missing MaiChat runtime dependencies ... liteav.dll`，因此不能启动裸 `build/bin` 程序。
四个 TRTC DLL 都已在新仓库的 `vendor/tencent-trtc/windows/lib/x64` 中，并非素材缺失。

`MaiChat/desktop/CMakeLists.txt` 先为 `maichat` 添加 `windeployqt` 和 PE 依赖检查，
随后才添加 TRTC DLL 的 `POST_BUILD` 复制。Ninja 按注册顺序执行，检查在复制前失败。
旧构建目录中残留的 DLL 曾掩盖这个顺序问题；全新克隆的构建目录稳定复现。

## 修复与验证

修复提交：本文所在提交。启用 Windows TRTC 时，显式检查四个运行时 DLL，在
`maichat` 的 PE 检查之前复制到可执行文件目录；TRTC 测试目标保留自己的复制命令。
这样缺素材会在配置阶段失败，缺其它 DLL 仍由 PE 检查拦截。

- 同一构建在修改前因 `liteav.dll` 缺失失败；修改后 `maichat` 构建返回 0，
  `verify-windows-runtime.ps1` 确认 90 项 PE 导入与 Qt 插件。
- 生成的 `post-build.bat` 中 TRTC 复制在第 24 行，运行时验证在第 27 行。
- `liteav.dll`、`liteav_screen.dll`、`txffmpeg.dll`、`txsoundtouch.dll`、
  `Qt5WebSockets.dll` 及 QPDF LICENSE/NOTICE 都在新构建目录；两份 QPDF 许可文件
  与仓库源文件的 SHA-256 各自一致。
- Host、Windows computer、Agent 面板、登录、布局、PDF 密码提示与 TRTC 引擎
  共 7/7 项定向 CTest 通过。computer 测试截图亲看为真实可见窗口，无黑屏。
- 从新仓库完整 `bin` 目录，以仅含 Windows 系统目录的 PATH 执行 `--smoke`，
  退出 0；日志回读版本 2.2.32、Qt 5.15.2、OpenSSL 3.5.9 和新仓库 exe 实际路径。

## 构建环境与边界

该 Windows 检出使用本地 `.ps1` CRLF 规则，避免 PowerShell 5.1 对无 BOM UTF-8
加 LF 行尾的脚本解析失败；C/C++ 编译使用 `/utf-8`，C++ 另用 `/EHsc`。
Qt 桌面端 ZLIB 使用本机 QtZlib 头和 Qt5Core 导入库，LLVM、Meson、MSYS2 取本机工具。
ONNX Runtime 1.26.0 DLL 和 OpenSSL 3.5.9 静态库/生成头从旧工作区**复制**到新仓库
独立构建缓存；ONNX 的 `VERSION_NUMBER` 与 DLL 版本均为 1.26.0，OpenSSL 的
`VERSION.dat`、`Configure` 和构建脚本 Git blob 与旧工作区一致。复制后产物哈希一致，
未移动旧文件，也未在新仓库重新编译这两项第三方依赖。

本次只验证新仓库 Windows 构建目录与短暂启动；没有运行安装器、制作正式 EXE/ZIP、
上传 Release、替换现装 App 或进行真实账号/付费任务验收。
