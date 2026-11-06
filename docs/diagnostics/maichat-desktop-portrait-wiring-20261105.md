# Qt Desktop 普通人像服务与共享工具接线

时间：2026-11-05（本机 Asia/Shanghai）。源码：main，起点 `eee43402`，保留开工前全部未提交工作。
平台：macOS arm64，AppleClang 21，Qt 5。未停止或替换 `/Applications/MaiChat.app` PID 43461，未安装设备。

## 可复现操作与预期

当前 Qt 产品没有 Portrait target、普通服务或四个移动端同名人像工具。预期由当前账号的
AgentController 注册共享 factory；普通 C++/Qt 调用可直接使用 DesktopPortraitService，
不需要构造 Tool。已有工作区图片经授权后以原生尺寸编辑，输出新的 PNG 和同尺寸灰度 mask，
完整编码校验和无覆盖发布后，才在当前账号 Portrait/portrait.sqlite3 登记结果。
原图不修改，生成图再次调整追溯原图，取消/失败不发布半成品，不自动打开全屏。

正式目标的独立复现命令（不安装或替换 App）：

```sh
cmake -S MaiChat/desktop -B /tmp/maichat-portrait-desktop-build -DQt5_DIR=/opt/homebrew/opt/qt@5/lib/cmake/Qt5
cmake --build /tmp/maichat-portrait-desktop-build --target desktop_portrait_service_test -j4
ctest --test-dir /tmp/maichat-portrait-desktop-build -R '^desktop_portrait_service_test$' --output-on-failure
```

## 接线与边界

- `MaiChat/Portrait/CMakeLists.txt` 为移动 CMake 和桌面共用唯一 `maichat_portrait` target。
  原四源及普通身体净肤/LUT 两独立模块一起编入。净肤/LUT 主路径 v7 接入由核心任务同步完成，宿主不复制算法。移动端归档输出位置保留
  原 CMake 根目录，避免 iOS 递归合并归档时纳入缓存内的新旧两份 Portrait archive。
- 四个工具集中于 `MaiPortraitHostTools.cpp`，原名称、原 schema 字段次序逐字对比 HEAD 一致；之后依 v7 明确需求仅追加
  可选 bool remove_body_hair（missing=AUTO，true 显式净肤，false 保留体毛），不扩八属性训练数组。
  MobilePhotoTools 在原位置调用该 factory；桌面回调委托普通 service。
- service 绑定当前账号数据库所在目录，工作区来自每次普通请求或 ToolContext，文件策略复用
  调用者的 resolvePath。模型不能指定 profile、ORT、模型目录或宿主面部提示。
- Qt 只做 EXIF 转正、sRGB RGBA8 I/O、原生窗口取样和编码。macOS Vision/Windows 已有 YuNet
  只定位人脸；参数、归属、ROI、partial 和保护仍由同一核心决定。Analyze 全图仍 <=512；
  v7 请求的原图人物区域 Refine 代理允许 <=1024，不接受模型指定区域。
- beautify 走 Analyze → 可选原图区域 Refine → 原生脸 Protect → 顺序 Tiles → Finish，支持
  3200 万像素；保存完整原图尺寸。普通 body geometry 沿用核心 1200 万像素上限并明确拒绝超限。
- 可选脸部几何处理只在有可信 face_plan 时应用于原生 ROI；不放大全图 float 图层。
- 失败会清理本次拥有的临时/未登记文件，不删除发布前已存在的路径；取消在队列等待、检测、
  指纹、tile 和两张 PNG 发布之间检查。模型推理内部仍沿用核心单次调用的取消粒度。
- 模型/ORT 打包不依赖 RVM 或 FFmpeg 是否启用；配置校验固定模型 SHA，构建后再次校验包内文件，
  同时携带 MediaPipe、ORT、GPUPixel 许可与说明。Windows ZIP/安装器的显式 staging 清单也
  独立按 MAICHAT_ENABLE_PORTRAIT 携带四模型/ORT/许可，逐文件核对源与 staging SHA；
  不再只依靠原有 RVM/FFmpeg 条件下的 ORT 复制分支。PE 检查同时覆盖动态加载的 ORT/provider 自身及其 delay-load imports。
  这些 PowerShell 分支尚未在 Windows 执行。

## 实测证据

原有 desktop 缓存的 CMAKE_HOME_DIRECTORY 指向另一真实仓库
`/Users/hongqingwan/OpenSource/multi-ai-code/MaiChat/desktop`；直接执行旧缓存 build 会验证错树。
没有修改或清理该缓存。独立验证目录：`/tmp/maichat-desktop-portrait-20261105/`。

1. 本树六个 Portrait 源、Qt 普通 service、Mac Vision、共享 factory、桌面注册代码编译通过。
   引用旧缓存已有 OpenCV core/imgproc、MaiAgent/SQLite 静态库；当前 MaiTool.cpp 重新编译，
   在临时复制的 archive 中剔除旧 MaiTool 对象，避免旧接口 ABI 混用；仅在验证目录补编
   OpenCV 的 inpaint.cpp。未修改 vendor 或产品目录。
2. 本树 AgentController（开启 MAICHAT_DESKTOP_PORTRAIT）和移动 MaiMobileHostTools 分别编译通过。
   Windows face adapter 用 Mac Qt 进行 C++ 语法检查通过；这不是 Windows 构建或实机验收。
3. `DesktopPortraitServiceTest` 普通入口/共享工具入口、UI 线程拒绝、越界授权拒绝、未知字段拒绝、
   取消、原图字节不变、生成图追溯原图、账号结果登记、原生脸几何、普通个人模型 status 通过。
4. 18MP 5000×3600 角落人物样例：Refine=true，20 tiles，原生输出及 mask 完整。
   32MP 8000×4000 样例：32 tiles，输出及 mask 完整；核心只确认脸部范围，诚实返回
   `skin_scope=face_skin_only`，未称为全身覆盖。
5. 原始一次运行在所有断言通过后仍 exit134，终端为 `mutex lock failed: Invalid argument`。
   根因边界是 Bridge 全局引擎晚于动态 ORT 内部静态资源析构；普通 service 析构在其确定的
   账号生命周期内串行清空 Bridge 配置并释放会话后，同一回归 exit0。没有忽略异常退出。
6. 完整桌面 CMake 在 `full-config/` 从本树配置/生成通过（远程桌面关闭，无新网络下载）。
   独立最小 bundle probe 执行产品同一 PortraitAssets.cmake：四模型/ORT/许可实际拷贝，
   构建后显示 `Packaged portrait models verified (4/4), ONNX Runtime present`。

最终 32MP 普通服务日志：`service-verified.log`；产物：`results-verified/Workspace/`。
打包 probe：`build/maichat.app/Contents/Resources/PortraitModels`，不是正式 MaiChat App。

## 验证边界

这些是新接线的普通服务/工具与包规则验证，不代表用户否决的旧 v6 美白视觉质量通过。
没有完成正式 Qt 产品整包构建、Windows staging/ZIP、运行 App 的视觉验收或 iOS/Android 设备验收。
没有宣称 Linux 产品支持。模型请求依旧客户端直连，未增加 UI 配置或服务器转发路径。

## v7 最终增量验证（同日）

核心冻结后，采用正式 desktop CMake 目标完成全部当前树依赖构建，不再只依赖临时导入的
旧静态库。首次真实目标编译暴露 OpenSSL 只有生成头目录、缺源码 evp.h 的接线问题；
共享 Portrait target 已补同一仓库 OpenSSL source include，之后正式目标构建成功。

- 正式目标：`full-config/bin/desktop_portrait_service_test`；构建日志 `final-target-build.log`。
- 正式 CTest：`final-ctest.log`，`desktop_portrait_service_test` 1/1 通过，11.73 秒，exit0。
  覆盖普通 service、共享工具、原图追溯、授权/未知字段/UI 线程拒绝、两个完整 PNG、
  32MP 32 tiles、几何、个人模型 status、首张 PNG 发布后取消清理。
- v7 记录 `processing_version=native-owned-skin-tiles-20261105-v7`；AUTO 的
  `parameters` 不携带 remove_body_hair，另 `body_cleanup_request_mode=auto`；显式开关仍为
  boolean，mode=enabled/disabled。三者经普通服务验证；显式 false 的 clean_skip_reason
  为 disabled_by_parameter。旧八项数值收据格式保持原契约，不扩展个人模型数组。
- 从正式目标复制同一测试 binary 到临时 bundle probe，在该真实目录结构下调用
  `registerDesktopPortraitTools`（不是测试自建的宿主回调），读取包内四模型和 ORT，
  跑过实际桌面工具注册与路径/账号/取消/v7 开关，exit0；日志 `final-bundled-adapter.log`。
  此增量 probe 不重复 32MP 阶段，更没有启动正式 MaiChat UI。
- 源码、四模型、内嵌 LUT/许可和最终产物 SHA-256：
  `/tmp/maichat-desktop-portrait-20261105/final-source-artifact-manifest.json`。
  main 源码基点仍为 `eee43402aff22caeb4b0c2688e381ced569f7e63`，包含未提交补丁。
- 最终检查 PID43461 仍为 `/Applications/MaiChat.app/Contents/MacOS/MaiChat`。

正式 MaiChat UI、Windows 部署和移动设备的边界继续如上，不以这些普通服务测试代替视觉验收。

## 唯一脸、身体归属歧义的最小回退增量（同日）

Android 实际 24MP/EXIF6 合成图暴露多个 person seed 同时高覆盖唯一人脸；这不构成身体归属。
本桌面任务仅做只读边界审查，没有并行修改核心。生产补丁的 guard 已核对：

- 仅 `faces.size()==1` 且原竞争阈值命中时回退，身体仍为 unowned；多脸、无脸的原有拒绝保留。
- 仅排除同一脸覆盖率 >.35 的竞争 seed 对该脸形成的假 foreign 证据，其余 foreign 排除保留。
- 回退 tile 在硬排除后仅使用 `faceProtection * faceExclusion`，脸采样 ROI 外为零，
  不走 coarse/body alpha 分支。采样 ROI 本身不被当作授权皮肤域。
- 身体 alpha 和 material scope 仍全零，显式身体去毛请求继续失败，mesh/五官保护失败继续拒绝。
- `body_ownership_skip_reason=overlapping_person_seeds` 由核心说明回退原因；普通桌面服务保留
  commit 完整输出对象，无返回字段白名单。此次普通服务回归也确认该字段已透传（正常案例为空）。

核心宣布冻结后，只增量构建正式 `desktop_portrait_service_test` 目标并运行一次 CTest，未构建
整个 App：`ownership-fallback-build.log` exit0；`ownership-fallback-ctest.log` 1/1 通过，15.71 秒。
已建目录从 453352 KiB 增至 456556 KiB，净增 3204 KiB（约 3.13 MiB），低于本次 500MB 限额。
PID43461 仍为原 `/Applications/MaiChat.app/Contents/MacOS/MaiChat`，未启动第二个 App。

最新源码/产物 SHA-256 与实际服务字段摘要：
`/tmp/maichat-desktop-portrait-20261105/ownership-fallback-source-artifact-manifest.json`。
此前临时 bundle 工具 probe 发生在此 guard 增量之前；本轮不把旧 probe 宣称为新 guard 的 UI 验收。

## 12MP 门禁定点审计关闭（用户更正）

用户随后确认当前 24MP 原始尺寸已能直接处理成功，前述“只能缩到 12MP”说明作废，
本轮不据此修改任何阈值、工具或宿主路径。源码只读核对：三个平台 dedicated
mobile_beautify_image 都先进入 beautify 原尺寸/tiles 路径；iOS/Android 的 generic
transform(operation=beautify) 同样在旧整图 12MP 门禁之前早返。旧门禁仍适用于
reshape_body、非 beautify 整图变换及独立整图核心接口。

保留一个诊断语义线索：MaiPortraitPhotoProcessor.swift 的 MaiPortraitHostFailure
初始化（约45–49行）及 MaiPortraitBridge.cpp diagnostic()（约116–132行）会在
不同阶段/原因的失败中统一附带 full_raster_max_pixels=12000000；该字段并非
32MP tile 路径的实际尺寸门禁，不能脱离 code/stage/message 判断失败原因。
此次仅记录，不扩大修复其它接口；质量任务继续由实际前后图的统一色彩对照验证。

## v8 正式目标清单补落盘（紧急清理后）

清理磁盘后只读重核此前采集的全部 43 个源码指纹及 3 个正式产物指纹，均未变化；
Portrait 冻结源码、桥接、两项 native 测试及桌面服务测试保持一致。没有重复构建或测试。

- v8 正式目标构建日志：`/tmp/maichat-desktop-portrait-20261105/v8-formal-build.log`，exit0。
- CTest：`v8-formal-ctest.log`，1/1 通过，11.90 秒；构建目录净增 3260 KiB（约3.18 MiB）。
- 正式 binary SHA-256：`7cc80e426da0da3ece5967de6a1149256981178df9985c2314ed6ff75867a794`。
- 四个真实 service 响应均为 v8，实际颜色规则为 `source_hue_chroma_cap`，
  请求固定暗部幅度为18.75；该值不是每像素最终亮度差。四项工具字段次序和 schema 约束不变，
  文案描述的更新不构成新参数。候选图仅已发给用户可审阅，尚未收到审美反馈；
  工程性质回归、像素一致性均不表示用户已接受视觉效果。
- 最新源码/正式产物/服务字段/schema 结构清单：
  `/tmp/maichat-desktop-portrait-20261105/v8-final-source-artifact-manifest.json`。
  单独 schema 清单：同目录 `v8-schema-structure.json`。

此前因 ENOSPC 未能创建上述小文件；本轮只补持久化，不重跑旧测试，也未构建整个App、
启动第二个App或替换/结束用户 PID43461。
