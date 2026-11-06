# Windows / Mac 共同发布 2.2.52

2026-11-06。用户要求拉取最新代码，与 house-maichat 一起构建 Win 和 Mac 包并发布 GitHub Release。
验收操作：在同一份 main 提交统一桌面版本号为 2.2.52；从该完整 SHA 构建 Mac arm64 与
Windows x64；各自在完整发布目录及解压后的包中验证运行依赖和 smoke，再把通过的产物
上传同一个草稿 v2.2.52。核对版本、提交、SHA256 和各自测试证据后统一公开。

远端基线是 3b4906a3afc43f4f2c4285095aede5662bfb8821，已含 v2.2.51。
此前本机 9459e7ec 的未提交工作逐文件备份到
`/private/tmp/maichat-win-mac-release-20261106/before/`，清单为 before-manifest.json。
保留字体/字号、云盘查询下载、普通输入队列及逐工具重评、Mac 原生 HDR 等已完成工作；
新上游提问普通输入、includeMessageUpdates opt-in、独立远端更新、分页与显示旁路不能丢失。
用户仍在运行的正式 Mac 2.2.34 不停止或替换；发布安装包不代表已更新运行中的程序。

分工已通过 IM 与 house-maichat 对齐：本机负责合并、统一源码/版本、Mac 构建验收与
Release 汇总发布；house-maichat 在收到最终完整 SHA 后重新执行 Windows 全目标构建、
Core 门禁、staging/ZIP 双份裸 PATH smoke 和 PE 检查，上传同一草稿，最后由本机公开。
不复用 v2.2.51 的验证结论，也不把此前未发布的本地 2.2.54 验证包直接改名发布。

## 合并与冻结前验证

fast-forward 到上述远端基线后，恢复安全 stash
`62250d381409d04b640e677df2270934931cb1b8`，处理 14 个冲突文件。
问题回答保持独立 answer / question_id 绑定，普通远端补充使用 prompt + 持久 requestId，
不依赖观察端 busy/turnId；iOS 队列同时保留 answer 和 enqueue 语义及各自重试边界。
窗口外消息精读、remote_snapshot 的 includeMessageUpdates opt-in、两套独立更新队列
及分页/展示旁路均保留。所有恢复前仍存在的路径均未丢失，备份及 stash 不删除。

本机 Release 主目标和相关 Qt 测试目标构建通过，已核对版本为 2.2.52。
最初调用的最后一个媒体测试目标名有误；此前主程序和 UI 测试已成功构建，随后改用
实际 CMake 目标 MaiFfplayVideoDialogTest，补齐媒体/云盘/人像测试目标，未把失败调用算通过。

- 链接实际生产 Core 的十套回归全部通过，包括新增逐工具队列、原权限和工具循环门禁。
- 桌面同步、Agent 页面、主窗口三组完整回归最终全部通过；控制器、MarkdownView、
  UiZoom 三组在初轮通过，没有生产源码变化后无理由重复执行。
  初轮中的旧脚本预期“补充后仍执行原问题工具”和“忙时发送 steer”，已按用户明确的
  重规划要求更新：原问题确实取消，模型看见补充后重新提问；额外断言原调用的取消状态、
  新模型请求中的用户输入，以及正常 prompt 不含 turnId / 含稳定 requestId。
  主窗口断言改为核对统一正文偏好，而非已废弃的固定 14px；未降低字形清晰度要求。
- 云盘 Agent 本地 TLS 集成通过；桌面人像普通服务回归通过。
- 同步服务 24 项通过；Android Java / 正常问题输入回归共 476 项、0 失败、1 跳过。
- iOS 队列 17 项通过，普通 App 与移动 C++ 适配层 build-for-testing 通过，未把新合并
  的移动端源码自动安装到用户手机；此前手机 202 的安装证据属于上一轮交付。

播放器完整扩展测试在当前 Mac 锁屏时运行：44 通过、13 个原生交互断言失败、5 跳过。
原生诊断明确显示前台为 com.apple.loginwindow、active=0 / space=0，菜单、拖动和全屏
输入无法命中新窗口。已请求用户解锁并继续其它验证；该组尚未作为通过门禁，未据此
修改生产播放器，也不宣称实际 HDR 全屏亮度问题已完全验收。

冻结后的完整 SHA 由 Release 目标提交及各平台 manifest 记录；实际打包、解压/挂载、
签名和公开发布回读的最终证据保存在验收目录，并随 Release 提供脱敏后的平台清单。

## 发布包审计及 Windows 编译补充

首个候选 0277cda6 已由双方通过 Git bundle 校验同一 SHA；本机大包 Git 推送遇到 TLS
传输中断，house-maichat 从 bundle fast-forward 后代为成功推送，双方均回读 origin/main。
共同草稿 Release ID 436491882，没有提前公开或上传旧候选安装包。

Mac 完整 staging、只读 DMG 挂载、ZIP 实际解压的签名和纯系统 PATH smoke 均通过，
但真实 Mach-O 依赖审计发现 libwebp 的 @rpath/libsharpyuv.0.dylib 未随 Qt 5 部署器带入。
不能把主程序启动成功当作所有图片插件可用。新打包流程从同一 WebP 安装目录复制
匹配 arm64 的 SharpYUV，改用同目录 loader_path，并在签名前检查实际加载命令、依赖
闭包及 App 架构。原包验证器退出 1，新包退出 0，缺口为三个 WebP 消费者；
LC_ID_DYLIB 和 universal 架构标题均不算运行时加载依赖。
SDK 附带而未被 App 加载的另一架构 CoreAudio driver 资源单独记录，不宣称它支持 arm64。

Windows 在真实 MSVC2019 编译发现 windows.h 的 min/max 及 RPC small 宏污染图像代码。
house-maichat 使用冻结源码和原 cl 命令作 RED/GREEN：原文件 exit 2 / 102 错误，
仅增加 NOMINMAX、WIN32_LEAN_AND_MEAN 后 exit 0 / 0 错误。收到并核对补丁 SHA256 后
合入同一 main；这不是美颜算法调整。另给 Windows 人像测试链接与产品同一份 qrc，
使 YuNet / 人体分割资源确实存在，Mac Vision 的通过结论不能替代 Windows 资源入口。
上述修正统一进入最终冻结提交；Windows 保留输入未变化的编译缓存，在新 HEAD 下继续
增量构建及首次完整门禁，正式安装包、草稿目标与两端 manifest 均采用最终完整 SHA。

Windows 继续完整目标构建又实际复现独立 PDF 测试的 UiZoom::setStyle 链接缺失。
只加 UiZoom 对象仍缺六个 Markdown 符号；house-maichat 用冻结目标的原参数加
UiZoom + maichat_markdown + 其传递依赖 mai_md4c 后链接 exit 0。收到 673B 补丁并
核对 SHA256 6559a2383eb446065f3c2465c61b731dabc6c819f97c0139ea3e88c4fa29361c 后，
仅在 WIN32 分支补全独立测试依赖；运行结果仍以最终 Windows 门禁为准。
图像引擎恢复无关格式，最终相对 0277 仅保留顶部 Windows 宏保护，不改变处理算法。

Mac 另用私有 Qt 解码探针读取合成 2×2 WebP，在纯系统 PATH 下成功；实际动态加载记录
确认 Qt imageformats/WebP/SharpYUV 全从部署 App 内加载，未加载 Homebrew 库。该探针
只用于打包验收，不进入产品，也不使用用户素材；此前 ffmpeg 缺编码器及探针首次头文件
路径错误已修正，未将这两次准备失败算作成功验证。
