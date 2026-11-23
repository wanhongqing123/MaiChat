# Windows 扫码登录后无窗口：旧对象文件与新公开结构混链

## 复现与预期

- 平台：Windows 11，MSVC 2022 14.44，仓库 Qt 6.12.0。
- 用户操作：打开 3.2.1，扫码并在手机确认；登录窗口关闭，主窗口未出现。
- 预期：登录成功后显示主窗口，进程保持运行。
- 排查时间：2026-11-23 晚，源码基线 `b69666e5ec1295dce507b87262082a65527c85eb`。
- 现场产物：`D:/Program Files/MaiChat/maichat.exe`，日志报告版本 3.2.1；进程 73236、75796 已退出。
- 系统 Application 日志分别在 23:17:30、23:18:20 记录 `0xc0000374`（堆损坏）、`0xc0000005`（访问冲突）。只查看崩溃元数据，未读取用户内存转储、令牌或聊天数据库。

## 根因证据

1. 同源正常构建中的 `login_dialog_test approvedQrUsesServerSignedCredential` 通过；二维码批准及凭据交接本身能完成。
2. 使用隔离测试用户目录和 FakeRemoteIMClient，运行 `main_window_layout_test mainWindowUsesRoomierIconFreeTitleArea`，无需扫码及真实账号即可复现 `0xc0000005`。
3. 最初崩溃栈位于 `MainWindow::buildUi` 添加消息页面时，Qt 遍历已经损坏的 QObject 子对象。仅据此不能把原因归于 Qt。
4. 对该测试程序临时启用 full page heap，调试器更早捕获 `DesktopScreenCaptureControl` 构造函数写入已分配对象尾部之外。通过 owned 链接 MAP 将地址对应到 `DesktopScreenCaptureControl.cpp.obj`。测试结束后已关闭该测试程序的 page heap。
5. 2026-11-23 21:30:29 +08:00 的 `9d886135cb03edeeb20bcdead95e059ac2b4ca16`（Merge pending tool and context improvements into main）为 `MaiScreenshotCaptureState` 追加了 `failed`、`nativeStreamPresent` 及 availability history；`git blame` 与相对第一父提交的 diff 均将这些字段定位到该批次。该结构是 `DesktopScreenCaptureControl` 的值成员，因此分配该控件的代码和构造函数必须同时重编。这是触发旧对象混链的结构变化；底层漏记依赖及只覆盖登录窗口的验收缺口共同使其进入 3.2.1 产物。
6. 原 MainWindow 对象文件时间为 13:44:39，控件构造函数对象文件为 20:54:05。Ninja 查询 MainWindow 对象显示 `#deps 0`，漏掉了间接引用的公开头文件。
7. 原 Ninja 规则中的本地化 MSVC `/showIncludes` 前缀为 CP936 字节，而编译器实际输出 UTF-8。构建看似成功，但 Ninja 未能识别头文件依赖。
8. 不修改任何产品 C++ 逻辑，只重新编译 MainWindow，原失败用例即可通过；这验证了旧分配代码与新构造代码混链的因果关系。

## 修复

- Windows MSVC/Ninja 使用小型编译启动器：以 owned 头文件探测编译器实际输出的前缀原始字节，将 include marker 统一为 ASCII；保留路径、诊断及退出状态。
- 在桌面目录的 CMake 配置结束时恢复 ASCII marker，避免 Qt/第三方工程或语言配置重载旧编译器元数据。其他平台及非 Ninja 生成器不使用此启动器。
- 首次切换启动器后，编译命令改变，重新编译正常 App 的完整依赖闭包，消除已有旧对象文件。
- 增加真实 MSVC/Ninja 回归：带空格的工作路径，C/C++ 两种消费方，仅修改间接头文件后检查两者均重编并体现新值；另覆盖配置后续重载 marker 的情况。
- 增加扫码批准后进入实际 MainWindow 的回归：本地 HTTP 夹具、FakeRemoteIMClient、真实 QWidget，镜像 `LoginDialog::exec()` 返回后构建并显示主窗口的顺序。
- README 明确：`--smoke` 只取消登录，不能作为登录后主窗口已验证的证据。

## 验证记录与边界

- 原 MainWindow 回归：修复前访问冲突，单独重编 MainWindow 后 3 passed / 0 failed。
- 编译依赖回归：去掉最终 marker 恢复的 owned 对照版本 exit 1、C/C++ 均 `#deps 0`；修复版 exit 0，两者都能对间接头文件变更重新编译。
- 正常 App、LoginDialogTest、MainWindowLayoutTest 完成 1731 步依赖重建及链接，exit 0；正常部署后的 PE/Qt 插件检查通过。
- 新 MainWindow 对象记录 545 项头文件依赖，DesktopScreenCaptureControl 对象记录 155 项，两者均包含 `MaiScreenshot.h`；原 MainWindow 为 0 项。
- 通过仓库 Windows 测试入口运行二维码批准、批准后主窗口显示、拒绝不匹配凭据：5 passed / 0 failed（含初始化及清理）；原 MainWindow 回归 3 passed / 0 failed；注册的编译依赖测试 exit 0。
- 已亲看隔离测试抓取的实际主窗口截图；显示消息页、导航栏及输入区，登录窗口已关闭。
- 补充 UTF-8/CP936 中文用户路径的前缀解析及真实中文/空格临时路径下的 C/C++ 依赖回归，exit 0。审查发现的路径编码假设已修正，复审无剩余重要发现。
- 正常打包生成本地候选 ZIP（261414075 bytes），staging 与实际 ZIP 解压目录分别在隔离用户目录、仅含系统路径的 PATH 下运行 `--smoke`，两者 exit 0。两份 exe 与正常构建的 SHA-256 均为 `2ed0ea62c5584ef6638de27257cd3dce101b027142d4198627e02d1c1baaf762`。这些 smoke 仍只证明产物可启动至登录窗口；登录后的覆盖由前述真实控件回归提供。
- 调查阶段候选产物为 `b69666e5` 加当时未提交的构建修复，源码闭包与 SHA 记录于本地 `fix-source-manifest.json`；该候选本身未作为公开 Release 发布。
- 尚未替换或停止用户安装程序，未用真实账号再次扫码；Windows 夹具通过不等于用户现场安装已验收。此次修改不涉及登录服务或账号数据迁移。

调试及构建原始记录位于本地 `.cache/login-main-handoff-20261123/`，不提交大日志、二进制或用户数据。

## 正式交付：3.2.2 Windows

- 用户明确要求发布后，修复、定点测试、版号和记录共八文件提交为 `13a1784281623618444005fc64d6635f6b3d2084`，已正常推送 `main`。
- 新 annotated tag `v3.2.2` 对象 `d325b8f2bc09569f9746d9017c18983d2bc1a87d`，peeled 精确指向上述产品提交；不移动旧标签、不替换旧资产。
- Release `455667383` 于 `2026-11-23T16:20:30Z`（北京时间 2026-11-24 00:20:30）公开：[MaiChat Desktop 3.2.2](https://github.com/wanhongqing123/MaiChat/releases/tag/v3.2.2)。
- 正常 native-client Release 同源重新配置、编译及部署检查通过；精确发布源的登录测试 5/0、原主窗口测试 3/0、注册的真实 MSVC/Ninja 依赖回归 exit 0。再次亲看本版实际主窗口截图。
- 绿色版 staging、ZIP 实际解压及安装器仅提取的三份程序，均在自有目录、裸系统 PATH 下启动通过，实际运行版本 3.2.2。三份 exe SHA-256 均为 `cd295edb7aab7976974e978579b0e5a3eab50f379ff47c6d79bedbb52c9a295f`；未执行安装器安装。

| 附件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| MaiChat-Setup-win64-v3.2.2-13a17842.exe | 220209561 | bdeede9c4104dcd1728b9bc8ed084021f9ef18388a1f77e3e1605e275cfa1fdc |
| MaiChat-win64-v3.2.2-20261124-13a17842.zip | 261414092 | 513939690799755bdd15c67ee379e52b43dd61d7e3f85834d094e1ff23ef3f98 |
| release-manifest-win64.json | 3534 | 2aa8bf76f5b731b20db3d9b4cf2644d5e4f63bb09c2c4263baaa09e2057e7821 |
| SHA256SUMS-win64.txt | 312 | cda82523da336e0a0d7e0e242da439ded70a61a0d31b13523a649356ea57fa76 |

四件 GitHub 资产大小、服务端摘要和 uploaded 状态已核对；两份校验元数据在公开前经认证下载、公开后经匿名下载，均与本地原字节一致。独立 GitHub 连接器及匿名 API 确认 latest 为 v3.2.2、源提交和四附件一致；旧 3.2.1 四件资产保持原 id/大小/摘要/状态。

此版本只发布 Windows x64，未构建 Mac/移动端，也未用真实用户账号再次扫码、替换现装或访问用户聊天数据。发布与验包证据保留于本地 `.cache/maichat-release-20261124-322/`；该交付补记不改变产品源码、标签或附件。
