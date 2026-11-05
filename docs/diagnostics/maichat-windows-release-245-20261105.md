# Windows 2.2.45 完整发布构建：回复建议测试缺少 UiZoom 实现

## 现场与预期

- 时间：2026-11-05，Asia/Shanghai。
- 现场源码：`28c49ec6896098f5fef418ee0e61f135c502aab0`，`main`，开工时工作区干净。
- 产品：MaiChat Desktop `2.2.45`；Windows x64、VS2019 Community/MSVC 14.29、Qt 5.15.2、
  CMake 4.4.4、Ninja Release。FFmpeg、OpenCV、RemoteDesktop 均启用。
- 构建目录：`E:\OpenSource\MaiChat\MaiChat\desktop\.cache\build-release-62111144`。
- 操作：在当前检出重新执行 `cmake -S MaiChat/desktop -B <build>`，然后执行
  `cmake --build <build> --config Release --parallel 2`，构建默认全部目标。
- 预期：默认全部目标完整构建成功，随后才运行七项桌面回归、完整 staging、真实 ZIP 解压和裸 PATH
  `--smoke`。不执行安装器、不替换现装 App、不操作其它应用进程。

## RED 观察与根因证据

09:36:37 开始重新配置，配置退出 0；09:36:43 开始默认全部目标构建。
`maichat.exe` 已完成重新链接，正常 POST_BUILD 的 `windeployqt` 和 **95 项 PE 导入/Qt 插件检查通过**。
默认构建随后在约第 300/353 步链接 `reply_suggestion_controller_test.exe` 时失败，尚未进入测试和打包。

链接器报告：

```text
maichat_agent.lib(DesktopPdfPasswordPrompt.cpp.obj): error LNK2019:
unresolved external symbol int UiZoom::s(int)
bin\reply_suggestion_controller_test.exe: fatal error LNK1120: 1 unresolved external
```

默认全构建的实际退出值为 `-1`（Ninja 输出子命令值 `4294967295`；底层链接器退出 1120）。
未将仅 `maichat` 成功当作完整发布构建通过。

调用链已核对：

1. `reply_suggestion_controller_test` 原先只提供 `tests/ReplySuggestionControllerTest.cpp`，链接
   `maichat_agent` 和 `Qt5::Test`。
2. `maichat_agent` 包含 `src/MaiChatTools/DesktopPdfPasswordPrompt.cpp`。
   该实现为密码窗尺寸调用 `UiZoom::s(420)` 和 `UiZoom::s(12)`。
3. `UiZoom::s(int)` 的实现位于 `src/ui/UiZoom.cpp`，原先只在上层
   `remote_im_desktop_ui` 中编译，因此完整 App 能链接，该独立测试却没有得到实现。

本轮独立输出根：

```text
.cache/windows-release-245/out/v2.2.45-28c49ec6896098f5fef418ee0e61f135c502aab0-20261105T093636-006e188b/
```

其 `local-only-logs/configure.log` 和 `local-only-logs/all-target-build.log` 保存原始 RED。
这些仅为本机编译日志，不作为发布资产，不含用户聊天或账号日志。

## 修复范围与定点 GREEN

本次仅为 `reply_suggestion_controller_test` 补入已有 `src/ui/UiZoom.cpp`，复用相同尺寸实现；
不修改产品逻辑、版本或其它测试目标。

在同一 Windows 环境重新配置退出 **0**，然后执行
`cmake --build <build> --config Release --target reply_suggestion_controller_test --parallel 2`。
构建明确编译了 `reply_suggestion_controller_test.dir/src/ui/UiZoom.cpp.obj`，随后原失败测试目标链接成功，
退出 **0**，不再有 `UiZoom::s(int)` 的 LNK2019。

定点 GREEN 日志为 `.cache/windows-release-245/link-fix-green-configure.log`、
`link-fix-green-build.log`，结构化退出码与本地 CMake 文件指纹保存在
`link-fix-green-evidence.json`。此时仍为基线提交加本文和 CMake 最小修复，尚未分配新的发布提交身份。

## 后续边界

最终发布仍须绑定新的干净提交，再完成默认全部目标、七项 CTest、打包及实际 ZIP 解压 smoke；
当前记录不宣称这些后续门禁已经通过。

## 共享核心变更后的测试阶段（尚非发布提交）

Android CPU 分派和 `streamChat` 取消回调修复冻结后，按授权对当前已知 dirty 源码执行测试，
仍未打包。现场基线提交保持 `28c49ec6896098f5fef418ee0e61f135c502aab0`，实际测试输入另记录
三份源码文件 SHA，不将 dirty 结果归给一个未构建的新提交。CMake 的 Android 库输出目录修复
曾在首轮配置时发生变化，指纹门禁因此停止；重新获取冻结指纹后再完整运行。

- 当前 CMake 依赖文件 SHA：`88019f5ea843ddc5dc7406679c5245ee9b0267cfca882d054ccd9dcde146bf39`。
- `MaiLlamaTools.cpp` SHA：`f5dfb435e8fb09cf466ea27288fc284b54173201a21226bbb35a87e02686f0c1`。
- Desktop 重新配置、默认全部目标构建、显式 95 PE/Qt 插件门禁均退出 **0**。
- MaiAgent 已重新编译本次变更的 `MaiLlamaTools.cpp` 并重链八个核心测试。八项 CTest
  **8/8 PASS**，退出 **0**，总计 `169.67s`；相对原已验证缓存，公共头文件没有变化。
- 七项 GUI CTest 此时 **5/7 PASS**，整体退出 **8**，不能宣称桌面回归全部通过。

测试阶段证据分别位于：

```text
.cache/windows-release-245/test-phase-desktop-20261105T100848-9e8ca406/
.cache/windows-release-245/test-phase-core-20261105T100848-a5b2540a/
```

每份 `phase-manifest.json` 记录已测试源码 SHA、每步退出码及“仅测试、无打包”的边界。

### 欢迎卡片测试的旧数量预期

通过正常 `run-tests-windows.ps1` 包装器为 AgentPanel 单独指定 Qt 文本输出，实测结果为
**44 passed / 1 failed**。唯一失败是 `emptySessionShowsWelcomeAndFirstMessageHidesIt`：
实际欢迎卡片 `19`，旧断言 `18`，位置 `AgentPanelSessionTest.cpp:375`。

`git show fd67e6c20b0ff8f653f85220a882cb27f533a976` 明确证明此前正式功能提交已经新增
“账号资料与头像”展示卡片，而测试数量未同步。修复仅将旧数量改为 `19` 并增加对该明确功能
条目的断言；不删除卡片、不修改欢迎页或产品行为。

原始 RED 保留在 `.cache/windows-release-245/agent-panel-focused.txt`，原七项回归日志同样保留。
最小测试修复后，重建 `agent_panel_session_test` 退出 **0**，同一包装器重跑完整 AgentPanel
套件为 **45 passed / 0 failed / 0 skipped**（`3661ms`），退出 **0**；原数量用例也明确通过。
GREEN 证据为 `.cache/windows-release-245/agent-panel-green-build.log` 和 `agent-panel-green.txt`。
这只说明该套件修复后通过，不能将之前的七项整体结果改写为全部通过。

### computer 前台激活受控拒绝与测试身份边界

该测试在七项回归和单独重跑中均复现：Windows 拒绝将测试自己的窗口变为前台，
平台层 `maiDesktopActivateWindow` 返回既有 `MaiErrorCode::Busy`，消息为
`Windows did not allow the selected window to become foreground`。旧测试在不可用分支只接受
`NotConfigured`，因此失败。调用链在激活失败时直接返回，尚未进入鼠标发送。

这与本轮推理内核改动无关；平台层从仓库初始提交就使用上述返回值。通过原有截图环境变量
保留了 `.cache/windows-release-245/computer-capture-red.png`，已亲看为真实测试窗口：
`Waiting`、空输入框和 `Confirm`，不是黑屏。此截图证明捕获成功，不证明点击成功。

独立只读复核确认：Windows 与 macOS 平台的前台激活失败均可返回 `Busy`；不能为让测试通过
把产品返回值改成成功，也不能对用户进程或系统前台限制做绕过。

按授权，后续最小修复仅调整测试 harness：标题加入自己的 PID，Windows 下核对真实 `windows` QPA、
枚举结果的 HWND/PID 与本 fixture 的原生 HWND/PID 一致；只请求激活自己的窗口，并有界处理事件。
真正成为前台后才执行原来的鼠标、输入和 `computer OK` 断言。如果仍然遭到操作系统精确的前台
拒绝，则测试返回 **77** 并由 CTest `SKIP_RETURN_CODE` 明确标为未验收；不把任意 `Busy` 当成通过。
本节记录方案与原始 RED，实际修后结果另行追加。

修后重新配置退出 **0**（`6.675s`），`desktop_computer_tools_test` 构建退出 **0**（`3.578s`）。
正常包装器实测核对自有 `PID=25544`、`HWND=526682`、`QPA=windows` 后，操作系统仍拒绝前台，
测试按设计返回 **77**，CTest 明确显示 **Skipped**（`2.73s`）。日志为
`.cache/windows-release-245/computer-normal-ctest.log`，截图为 `computer-verified-fixture.png`；
该结果没有发送鼠标，也不记作完整交互通过。

为取得正向证据，使用本机已有的 Windows Kits x64 CDB **仅启动本次测试自己的新进程**，
不 attach 用户 App，不调整前台限制。测试仍通过原有 Qt 运行时/错误模式包装器进入，
独立 CTest 目录和全部调试日志只在 `.cache/windows-release-245/`。
在该受支持的前台条件下，原来的鼠标点击、原生文本输入和实际输入框文本断言全部到达，
输出明确成功标记：

```text
computer native input verified: own pid=20756 hwnd=1508442 text=computer OK
```

此次 CTest 退出 **0**，耗时 **2.60s**，无 `SKIP` 或 `check failed`。
结构化证据为 `.cache/windows-release-245/computer-debugger-evidence.json`，完整测试输出在
`computer-debugger-catalog/Testing/Temporary/LastTest.log`；CDB 模块日志单独保留，均不作为发布资产。
正常会话的明确跳过与本次正向输入验证分开记录，不能将前者改写成普通会话交互通过。

## 封版产物与 GitHub Release

最终发布源码为干净 `main` 的 `a2dc0aba88813973ec610879bccb030b22c50428`，已推送到
`wanhongqing123/MaiChat`。最终输出目录为：

```text
.cache/windows-release-245/out/v2.2.45-a2dc0aba88813973ec610879bccb030b22c50428-20261105T102820-9aeb2a42/
```

- 重新配置、默认全部目标、95 项 PE/Qt 插件门禁、打包和安装器编译均退出 **0**。
- 七项桌面 CTest 实际结果为 **6 PASS + 1 SKIP**，computer 仍明确记录普通后台会话的前台限制。
  对最终测试 EXE 另行在已有 CDB 中启动自有进程，原生点击和键入断言通过（`2.61s`，
  `PID=27544`、`HWND=2099570`）；没有把普通会话的跳过改写为通过。
- 同一核心源码指纹的八项核心回归 **8 PASS**，已在 manifest 记录身份关联。
- 完整 portable staging 与真实 ZIP 解压目录分别以
  `PATH=C:\Windows\System32;C:\Windows` 运行 `--smoke`，退出 **0**；进程分别为
  `PID=1568/5580`，均核对实际 EXE 路径、版本 `2.2.45`、Qt `5.15.2`。
- build、portable staging、真实 ZIP 解压和 installer 输入的 App EXE 均为 `100881920` 字节，
  SHA-256 `0125c61811d786829853478de6aaa70082d01b7cb8e6c44511f01bc4a266c785`。
- 安装器仅编译，没有运行，没有替换用户已安装的桌面 App。真实账号和付费模型路径不在此次验收范围。

| 发布文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `MaiChat-win64-v2.2.45-20261105-a2dc0aba.zip` | 170807614 | `a6480524d1ecb95860aeaeb4e3bee929214a7cfff66ef32043ecde3510d7dd07` |
| `MaiChat-Setup-win64-v2.2.45-a2dc0aba.exe` | 140687109 | `c38cbf6d692e6b802f9b9a58db2d2898d855d5f2d9f607c7b4bf25bdc8a2a9b2` |
| `release-manifest.json` | 10308 | `d8de8934e908fddbd3476291dced4a2b1db8f121b4593a0eee51d58ab7f39042` |
| `SHA256SUMS` | 308 | `149a41fd94b3860148ad2ac6fd5d1d0d2381444ed82c106c878c025602061ced` |

先创建 draft 并校验四个远端资产均为 `uploaded`、大小和 GitHub 返回的 SHA-256 digest 与本机一致，
随后按用户明确授权公开发布：
[MaiChat v2.2.45 · Windows x64](https://github.com/wanhongqing123/MaiChat/releases/tag/v2.2.45)。

发布后重新核对 `draft=false`、标签指向上述完整源码提交、四个资产哈希，并用无登录凭据的公开
GitHub API GET 确认 Release 可读。Release ID 为 `434713156`。
证据为最终输出目录的 `github-draft-verification.json`、`github-published-verification.json`；
原始本机账号日志、测试截图和编译日志均未作为 Release 资产上传。
