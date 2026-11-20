# Mac execution-window 守卫与 OCR 消费者验证

## 范围与供源

基线 `12bad690dddec511b00308b046c7ef69d92214fe`。Parent 独占 `DesktopComputerMac.mm`；Root 独占共享 Controller/Ownership/Windows/头文件与 OCR lane，并负责最后串行合源。全部修改与构建留在私有目录，Parent 未改 live 源码、索引、main、现装或 release/tag。

Root shared v1：29 个完整 postimage，ZIP 18,902,904 字节，SHA256 `5343bcac4d99741491deb9347cf6f5e5f373177afdb7c2d9548ec658e177f64b`。逐 before blob、raw SHA 和 Git LF post blob 核对后消费一次；九个 Windows CRLF 文本的 raw SHA 与 LF Git blob 分别核对，没有把行尾差异冒充内容差异。v2：16 件相对 v1 的完整 postimage，ZIP 164,333 字节，SHA256 `f3dc014e5fd747910a4f561249de1e2c07cbdc228efeb42074dcfd76a54d1042`，逐 before/raw SHA 守卫消费一次，没有重叠旧 v1 或重发模型。两版头的布局相同，v2 仅注释变化。另按 Root 明确的一行元数据补 `LICENSE text eol=lf`；权重、字典、原 LICENSE 字节均保持原 SHA，不为该元数据重复 OCR 测试。

MM 基线 blob `85a10e6ff14140098c842a3fe54b7c71699ef38f`，首供 blob `91130a0ede7fb28c6b12a601f177a4e4b463b225`，LF SHA256 `8959f6673f05a95b16d39e3b01635ff63aee6fe4ac065ff0d0a0abe9eb78b93d`。后继局部资源 RAII 续件见文末。要求最终 Platform 头 blob `fdfb71259c19608762335266d5d8a047dba8d642`、Tools 头 blob `63131e894323e9f782dde19d708ce99614bcc807` 及共享普通 `maiDesktopValidateExecutionPoint` 实现。

## 实现行为

- 新可选接收窗口独立于 display 坐标空间。每次新 effect 就近核 AX 权限、CGWindowID/PID/可见性、冻结的原生 frame、强 AX 引用、前台及实际 focused AX element/window 的归属；不能靠同 PID 或标题猜窗口。旧无约束 display 和旧 window scope 的能力边界保持。
- 鼠标矩形判断共用共享普通 predicate，保留 `point_outside_target`、bounds 与 `screen_points` JSON 诊断。主 receiver 矩形外一律拒绝，即使是 owned popup；拖动终点在初始 down 前也预检。矩形内再使用 system-wide AX hit 按真实 z-order 核 selected/owned family，不用 app-only hit 或会过滤小窗的窗口列表代替。
- 键盘接受可证明的 parent/sheet/window owner 链，不接受同 PID 独立 sibling；无法证明时拒绝，不自动激活。既有 secure-input 与硬件冲突策略保留。native 坐标仍为 top-left 全局 points，不翻 Y、不乘 DPR。
- 所有新 CG effect 集中经过同一个 `post`；拖动每步、双击第二次、Unicode 分块重复校验。AX 远程读取之后、真正 post 前再次采取消，避免读取期间取消仍发送新输入。公有 API 没有原子 validate-and-post 保证，不能宣称消除最后瞬间竞态。
- 有 holds 时，接收窗口 optional 清空、换 ID/PID/frame 或可见性证据变化均拒绝重绑；重复 down 的快捷路径也先检查 binding。scope 按值保留完整接收窗口，finish 清理每次回调。targeted up、releaseAll、析构清理继续旁路新 guard，硬件重叠时也不制造用户硬件的 up。
- 未发送 down 的 provisional ledger 在普通失败或 C++ 异常时都会撤回，避免清理为不存在的 down 制造 up；构造期 resolver 抛异常也清本 scope 已持有的 CF/source。上述异常/AX 延迟边界为静态审查修正，不冒称已实际注入该类异常。
- Mac 原生 session 仍未实现 typed RawKey repeat/touch/pen，保持 NotSupported，不新增后台报告能力。共享调度器按值保留 scope、清 caller callback，再由 native 下游校验；Mac 同步手势/文字循环按每个 effect 检查。没有新增自动 focus。

## 同 ABI 普通 Mac 消费者

既有正常 producer 命令和 `.d` 覆盖中，Platform/Tools/Ownership 新布局影响五个产品 TU：AgentController、DesktopComputerTools、InputOwnership、InputSession、Mac MM，均新头重编；另编译新增 OCR adapter、重生成并编译 qrc，共七 TU，通过。Core 不包含该原生 DTO，沿当前同布局 Core 归档复用；没有混用旧 Controller/原生输入对象。

agent 归档最终 37 成员，UI 资源归档 45 成员，所有新/复用成员逐字节核对。v2 只增编 Tools.cpp，0/3.772 秒，完整普通 App 链接 0/0.869 秒；最终 agent SHA256 `1fedeabae5df28996e11ee307e61a0c0d0bea238a41b6dc4c291bf6252466d69`，可执行文件 180,275,016 字节，SHA256 `1fd432746ea25b9f3cff478b5dae3102c9e59c4c7e16aa02329d0539feed2356`。

此验证使用既有普通 target 的编译/链接参数手工增编和完整 App 链接，不冒称重新配置了整个 CMake target、全对象重编、打包或运行了产品 GUI。正常配置 OpenCV=ON；只启用新 CMake 对应的独立 OCR 注册，不借 Windows Vision 宏。没有修改 SDK stamp/CMAKE_HOME、冷重建 vendor 或替换现装。

最终普通安全 harness 64/0：18 项共享 predicate 的纯坐标/结构化诊断，46 项预取消与空账本 state/up/release/reset。v2 仅重链接 0/0.291 秒、运行 0/1.035 秒。进程自身 AX trusted=true 前后未变，因此没有运行旧 14 项 unavailable 夹具，也没有运行真实 receiver/真实 holds/owned popup/foreign overlay/失焦后的实际 up。没有创建或激活窗口、截图、发键鼠、读剪贴板或私人 AX。纯 predicate 不是 native hit 正向验收。

## Mac 普通 OCR 实推

MaiOcr C ABI 使用相同生产源码新编译，现有普通跨平台测试通过；私有 CoreText 自造图三例通过：中英文与 nonce/中文按钮、其 90° 旋转、1920×1080 的 20px 小按钮。文字、原始像素 quad、独立墨迹角点覆盖及结果生命周期/释放检查通过，未修改核心阈值或字典来适配测试。

动态 ORT getter 确认为既有 1.26.0。检测、识别与字典 SHA 与 Root 供源/PROVENANCE 一致；单成员新 OCR 静态库的对象也逐字节核对。仅使用已有 ORT、OpenCV 和其已存在的传递 zlib 依赖，没有新库、下载、远端模型或用户图片。该结果不代替 Mac OCR 工具队列、普通 GUI 或屏幕内容的验收。

## WindowById 图像与原生 frame 的未验边界

仅只读核 `MaiAgent/AgentTools/MaiScreenshotMac.mm`：148–158 按 `filter.contentRect.size * pointPixelScale` 设 surface 尺寸，只显式设置 cursor；697–741 使用 desktopIndependentWindow filter。回调复制完整 surface，未按帧附件 `contentRect/contentScale` 裁切或校正原点。Controller 的窗口 bounds 则来自 CGWindow。宽高比例一致不能证明两个原点一致。

[Apple contentRect](https://developer.apple.com/documentation/screencapturekit/sccontentfilter/contentrect)只定义内容位置和大小；[sourceRect](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/sourcerect)当前说明单窗口捕获取 full bounds、不参考该值，不能盲加 sourceRect 补偿。可证默认是 [preservesAspectRatio=true](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/preservesaspectratio) 与未指定 destinationRect 时使用整个 surface；所查 SDK/官方 [阴影属性](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/ignoreshadowssinglewindow)没有给出可据以保证同原点的阴影/标题栏偏移合同。

因此这里既不证明已有确定偏移，也不证明 PNG(0,0) 等于 CGWindow.frame 原点。Mac window-image 像素/原生 frame 标记校准仍未验；没有调用捕获、请求 ScreenCapture/TCC，也没有盲改 SCK。Windows 的 owned HWND 截图标记实测属于 Root 独立证据，不能替代本项 Mac 验证。

## v3 元数据与局部 RAII 续件

Root v3 五件逐 before/raw SHA/post blob 核等后私有消费；先前已明确消费过的 LICENSE EOL 一行只核对精确 post，不重复应用。该版不改共享布局或模型，Mac window-image 仍可读取，但显式报告 `image_coordinates_usable_for_input=false`，并指导视觉定位使用 display+executionWindow 或 AX element，避免把未经校准的窗口 PNG 当同原点输入坐标。该元数据不改变 native 输入空间，也不禁用窗口图像。

Root 对首供 MM 独立审无 P1/P2，另提出低优异常资源边界。相对 `91130a0e` 的续件仅将局部 CF/CGEvent 引用纳入既有 RAII：`accessibilityWindow` 的 application/value/found 在 stopped 回调抛出时自动释放；attribute 的 CF value 覆盖 UTF-8 分配失败；move/scroll 的事件在 post 抛异常时也能释放，正常释放仍在原位置。没有更改 native effect、矩形、focus、family、取消采样、held lease 或释放策略。异常分配/真实 AX 过程未被注入运行，资源等价性为独立静态审查。

续件 MM 最终 blob `8813017973461a98872fab213d9407464f4e7466`，LF SHA256 `8b07bb688cd376fb36e8bda1e0264b40d9230fd182bc5f4866ea7ee29d50949b`。v3 fresh Tools/MM 普通编译均为 0，分别 5.310/3.633 秒，完整 App 链接 0/0.834 秒。agent37 所有成员 bytes 核等，SHA256 `53429b70c1fc711bd82e40fc854b5707249af3b818a4c0d024a318933d31cbf5`；最终可执行文件 180,275,688 字节，SHA256 `40ff43ff3be901cb0a16f6d00465dd8011b7b340cbd613bd793cb1bb228d4758`。模型与已过 OCR 推理没有重复运行。

最终 v3 安全消费者改用新产品 MM 对象并移除旧显式对象后，普通重链 0/0.273 秒、运行 64/0/1.456 秒、严格签名验证 0；18 纯 predicate 和 46 取消/空账本范围未扩，AX trusted=true 前后不变，真实 receiver/holds 与旧 14 项仍未运行。

## 交付边界

Parent 仅供 MM 单件最小 patch/完整 postimage、本诊断及安全回执，交 Root 串行合源；不携带模型大包，不提交或 push main。现装/私人 App/文件/截图/剪贴板均未操作；上述构建与测试不读取凭据、不调用网络或收费模型。产品实际原生窗口输入、普通 OCR 工具与 window-image 同原点仍按未验标明。
