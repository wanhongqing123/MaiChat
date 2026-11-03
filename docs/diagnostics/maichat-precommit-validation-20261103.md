# 当前批改动提交前核验（2026-11-03）

用户要求提交并推送已有改动。基于 main
`1997947219596e6520be27ed892bd6ea1820ff2e` 的当前工作区，保留既有未提交实现，
未新建分支。此批包括跨端媒体与 Seek 修复、移动端美颜/美体/偏好训练、独立浏览器与
短信工具、Swift 测试宿主分离以及 C++ 索引生成。后续追加的账号资料查询和头像工具
另行实现，不以本记录宣称已接入。

提交前检查 123 个候选文本文件，约 3.87 MB，没有二进制构建产物或大文件。
模型、测试照片、App/APK、日志、编译数据库和临时构建目录保持忽略；模型固定版本、
摘要、下载脚本和许可文件随代码提交。公开诊断说明中的永久设备标识及用户媒体文件名
已脱敏；运行容器 UUID、PID 和构建摘要保留为当时产物证据。未发现真实凭据或私钥。

按 MaiAgent 约定重新构建并运行八套核心回归，全部退出 0，失败检查 0：

| 套件 | 秒 |
| --- | ---: |
| MaiCoreTests | 1.830 |
| MaiModelClientTests | 27.250 |
| MaiTurnRunnerTests | 46.550 |
| MaiToolTests | 0.480 |
| MaiToolLoopTests | 0.426 |
| MaiPermissionTests | 46.448 |
| MaiStoreTests | 0.644 |
| MaiFilePathTests | 0.346 |

使用原生 macOS arm64 构建目录 `/private/tmp/maichat-pdf-core-gate`，最多四个构建任务。
现有缓存缺失 OpenSSL 生成头及 zlib 配置中间文件，已仅在该构建目录恢复并重新配置；
源码没有为通过测试而改动。正常构建 75.18 秒。日志与元数据
`/private/tmp/maichat-precommit-core-20261103-*`，未请求付费模型。

Xcode 新增 Swift 文件、DebugSupport、单元/UI 测试引用及 Android/CMake 共享核心和
资源接线已检查；Android 人像测试的公开站姿素材准备命令补入 Portrait README。
已有产品构建、设备、图像/视频及工具回归详见各专项诊断记录，不在本次 Git 操作中
重新宣称全平台实机验收。桌面原生全屏鼠标路径仍受独立测试进程前台 Space 限制，
该两项没有计入通过；iPhone 已安装启动与真实手势验收仍分别记录。
