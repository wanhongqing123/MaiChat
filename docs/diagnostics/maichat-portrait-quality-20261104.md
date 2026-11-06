# 人像修饰实际效果与全身皮肤范围排查

## 复现与预期

- 时间：2026-11-04，macOS 原生正式引擎；起始源码 `main` / `eee43402`。
- 已安装 iPhone 版本为 2.2.4 (188)，本次尚未将效果修改安装到设备。
- 现场：24 MP 逆光全身人像，分别仅设置 `whitening=0.6`、`1.0`，观察脸、脖子、手臂、腿及发际线。用户测得暗脸 L* 约 25，满强度后 L* 增加约 7、色度下降约 29%，出现面膜感；身体皮肤未改变。
- 预期：同一主体可见皮肤获得自然、连续的肤色改善，保留暗部层次、眼眉嘴唇、头发和背景；显式零强度不改变像素。处理结果仍是原尺寸图片和诊断蒙版，通过聊天附件交付，不自动打开全屏。
- 本次不访问用户相册、既有工作区或个人照片；使用已有公开测试照片和隔离产物复现。合成大图只验证内存和合成契约，不用作真实 24 MP 审美验收。

## 已确认的根因证据

1. `MaiPortraitPhotoProcessor.prepareImage` 先规划最大 2048 边长的人脸上下文，只将该 ROI 的原生像素交给 C++。ROI 外身体从未进入算子。引擎内另有 1.3 倍脸框、面部轮廓与眼眉唇排除，单独扩大蒙版无法突破宿主边界。
2. 实际固定版本 `selfie_multiclass.onnx` 输出为六类 logits，当前 softmax 正确；代码仅使用类别 3（脸部皮肤），舍弃类别 2（身体皮肤）。Google 官方类别表：<https://developers.google.com/edge/mediapipe/solutions/vision/image_segmenter>。公开全身测试图的 256² 输出，body-skin argmax 为 2037 像素、face-skin 为 169 像素，缺失身体覆盖可直接量化。
3. 已存在概率软蒙版、面部轮廓高斯羽化及 ROI 边缘衰减，不能将根因写成“未羽化”。旧白化公式 `L += 10*w*(1-L/100)` 对暗部提升最大；`a,b *= 1-0.3*w` 统一减色，无法保留逆光皮肤的自然色度。
4. 默认参数 whitening 为 0；旧 `strength` 只映射磨皮和提亮。这与显式 whitening 调用的算子效果是不同问题。真实完整工具模型的两次自然指令基线先误选了相册导出工具，未执行美颜，因此不能据此声称用户本次漏传 whitening。

## 基线与验证边界

- 固定模型 SHA、原生实测及图像对照保存在 `/private/tmp/maichat-portrait-quality-20261104-6r5z3z5t/`；实际 ONNX 输出域记录为 `segmentation-output-domain.json`。
- 修复前普通 Swift 服务及原生 Engine/Bridge/Region 回归已运行；这些既有测试验证强度单调、蒙版外不变和原尺寸发布，不能证明自然程度已达到商业产品。
- 不以改变全图差异阈值、重复调参或把调用成功计作效果合格。后续修改必须对照同一照片、同一参数的修复前后图像和皮肤区域指标。

## 修复与最终验收

已实现的候选修改：

- 共用的曝光肩部曲线及选择性色度修正，最大色度收缩由 30% 改为 6%，低色度肤色衰减更少。实际同图、同蒙版的原生放大对照中，原图 C* 42.03，旧满强度输出 29.74，新候选 39.55；这组对照只证明本次颜色修正，不能推导商业产品等效。
- 独立的同一主体皮肤分析、原生 tile 颜色处理和单独五官保护；iOS 普通照片服务与工具复用同一处理与发布。人物归属失败只能声明 `face_skin_only`；五官保护失败明确 `face_color_skipped`。重叠人物排除须在面部蒙版合成后再次保留。
- 完整光栅普通 C++ 入口也复用相同皮肤流程，仍保留原有 12 MP 限制。iOS 继续原尺寸文件处理上限 32 MP；没有将上限直接放大为全图 float 工作内存。
- 仅美白时跳过双边滤波；空蒙版块不做 Lab 转换，非空区域按 64 行带处理。保存的 12 份原生输出与优化前逐字节一致，见 `fastpaths-equivalence.json`。

公开全身图的真实原生结果：满强度覆盖 22737 个皮肤像素，皮肤 L* 54.19→60.16，
C* 34.54→32.50。手臂、腿进入蒙版，选定衣服、天空、海水、沙地点逐字节不变。
该图侧脸保护失败，结果明确标为 `face_color_skipped=true`；不能声称脸部也已处理。

曾失败的用例：正常公开全身图经 Swift 普通服务已通过；真正失败的是将该图放入
5712×4284 灰底的远处人物用例。全图 512 代理再缩入 224 人体检测器，主体占比过小，
退回仅脸部处理。该 RED 已保留并定位，没有把正常图的原生结果当成该用例通过，
也不能将这种失败误记为所有 Swift 输入方向错误。

实际失败代理为 512×384，其人物检测最高置信度 0.260556，低于原有 0.6 门槛。
同一 RGBA 的分窗口检测恢复到 0.934692，pose presence 为 1；没有降低置信度门槛。
跨尺度重复结果以模型实际输出的 hip/extent 虚拟裁剪几何去重，其他人物的排除继续生效。
随后对实际归属的人物区域做皮肤分类，不以猜测的人体矩形替代皮肤蒙版。

最终普通 Swift 服务同一用例通过：5712×4284 原尺寸，30 块、60 个有界检测窗口，
`person_count=1`、`person_ownership_reliable=true`、`face_color_skipped=false`，
身体皮肤 92997 像素。四肢正区改变数为 `[6269,3465,7627,3502]`，选定保护区改变数
为 `[0,0]`，蒙版外改变为 0；L* 56.43→59.54，C* 35.79→34.75。
该图仍是公开素材放入大画布的构图测试，不是用户原始逆光照片，也不是商业 SDK 对照。
最终图的原生大小上身与双腿裁图已查看，未观察到该组裁图的矩形拼接或普遍抽灰。

同一原生/普通服务完整回归通过，证据为 `final-evidence-summary.json`、
`ordinary-final-summary.json`、`native-source-freeze.json`。仅普通 prepare+publish 的
macOS 热运行记录：全身图 0.6 为 0.242 秒，24 MP 角落人物为 2.430 秒；完整 Swift
测试进程最大 RSS 为 800391168 字节。它们不代表 iPhone 性能或本地语言模型同时驻留的峰值。

## 安装产物与 Agent 边界

- `main` / `eee43402` 基础上的未提交改动，iOS 构建号 189。源码冻结记录在
  `/private/tmp/maichat-beauty189-20261104/source-freeze.json`。
- iOS 模拟器 26.5（`19D02616-576C-43A6-8D92-2AA160F6F48E`）实际运行正常 App，
  两项 Portrait UI 回归通过，0 失败、0 跳过，36.431 秒；录屏逐段及原尺寸/聊天缩略图、
  主动点按全屏截图已查看。处理后留在聊天，点按才全屏。两项仍使用已注明的 UI fixture，
  真实宿主处理路径通过不等于真实模型交付闭环通过。
- 模拟器记录位于 `/private/tmp/maichat-beauty189-20261104/portrait-ui.xcresult` 与
  `sim-portrait-flow.mp4`。真机 189 构建已成功，安装和正常进程绑定尚待最终补齐。
- 完整 128 工具、实际本地 2B 的相同两条自然指令单次重测中，“美颜一下”选中
  `mobile_beautify_image(path=portrait-source.jpg,strength=0.8)`；“美白一下”仍误把工作区
  文件名作为 `mobile_export_photo_original.id`。该观测没有执行工具，不是交付闭环。
  证据 `/private/tmp/maichat-beauty-natural-args-current-20261104-auh0h74o/final-results.json`。
  不能因算子或 UI 回归通过而宣称本地模型已无误选。

当前不宣称用户原始逆光照片、全部构图或商业产品同等质量已验收。

### 最终参数边界与产物记录

相册导出的 `id` 若实际指向当前工作区已有的普通文件，两个原图导出工具现在在调用
平台前返回 `invalid_input`，明确告知直接使用该路径处理，无须相册导出或授权。
这只是参数域校验，不替模型选择工具、不隐藏工具、不新增 UI 设置。维护的移动端测试
覆盖两个别名、真实文件不触发宿主、正常素材 ID 原样透传、其他范围不误判与原图不变；
相同用例在旧实现有 8 项失败，新实现通过。iOS 宏与通用移动目标的完整测试通过，
证据 `/private/tmp/maichat-beauty-export-boundary-20261104-5sqw2drp/mobile-tests-results.json`。

实际生产 MainAgent 的单次完整 128 工具、本地 2B 调用选择了 `mobile_preview_image`，
在隔离 QA 中按不执行未授权 UI 的规则于宿主调用前停止，未执行修图或交付，不能算通过。
证据 `/private/tmp/maichat-whitening-real-agent-20261104-zkh9ygsd/run/result.json`。
先前直接模型 helper 会二次序列化参数 Schema 并排序键，而真实 MainAgent 保留原始字段
顺序；两者不能当作逐字节相同的模型请求。未为得到绿色结果而修改顺序、强制工具或补造
平台成功回执。因此本地模型的自然语言选择、修图、交付闭环仍未验收。

含最终参数校验的 iOS 189 再次构建完成；同一正常 App 的聊天缩略图／主动全屏流程回归
1/1 通过，0 跳过，18.031 秒，录屏已查看。最终源码绑定为
`/private/tmp/maichat-beauty189-20261104/source-freeze-guard.json`，结果为
`portrait-ui-final.xcresult`、`sim-final-flow.mp4`。美颜及 Swift 像素处理源码与此前完整
24 MP 回归保持一致。

最终真机包冻结于 `/private/tmp/maichat-beauty189-20261104/device-final/MaiChat.app`。
11 项 Mach-O 和外层严格签名验证通过，新增皮肤 C ABI 存在，内置 2B 权重 SHA 仍为
`57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`。
实际安装失败：CoreDeviceError 4016，设备 `00008120-001E0DEC0263C01E` 为 unavailable。
未安装或启动 189，不能宣称真机验收。安装输出、设备状态及产物 SHA 均留在上述目录；
已向用户说明并等待设备重新连接，没有将此错误猜测为用户未解锁。

Android 263 / 2.2.26 已在唯一自有 emulator-5580 安装、正常启动并查看，最终 PID11628。
JNI 人像回归两次 1/1 通过、0 跳过，实际 PNG 已查看；最后补拍无遮挡预览时外部点击
干扰导致一次测试失败，记录保留，不把三次都记为绿色。系统全屏教学层遮挡的预览未通过
完整视觉验收，也没有用图片合成替代真实 UI。证据
`/private/tmp/maichat-beauty-android263-guard-20261104-43a41iad/runtime/final-runtime-evidence.json`。
Android 仍保留完整光栅 12 MP 边界，没有声称同步 iOS 的 32 MP 宿主编排。

Desktop 2.2.46 构建、隔离正常冒烟启动及 11 项宿主工具测试通过，用户现有 PID43461
未停止或替换；桌面工程尚未接入 Portrait 宿主，此验证不能算桌面美颜验收。
存储普通服务及工具另有 15/15 核心回归和 console e2e 通过，未扫描或清理用户 App 工作区。

实际原生对照图 `final-24mp-body-native-crops.png` 已通过 `imcli send-image whq-iphone`
发送，仅展示公开测试素材与确定性引擎真实输出，没有发送用户私人照片或再次发送源码 ZIP。
全部改动仍在 `main` 工作区，未提交、推送或发布新 Release。

## 用户视觉反馈：0.6 尚未达到可感知要求

2026-11-04 用户查看实际发送的 0.6 对照后反馈“感觉没啥，跟原来的图片也没啥区别”。
这使该强度的视觉验收重新开放；不能用 L*/C* 的方向正确替代用户可见改善，也不能把
上述通过的像素／范围／发布回归解释成该强度已经效果合格。

保留 v4 原始输入、0.6 输出、mask 及已发送对照作为基线。后续先在隔离目录比较同一
实际引擎的强度响应候选，不混入提亮、磨皮或暖度参数；原图、当前 0.6 与候选 0.6 使用
相同裁剪，阴影、肤色、高亮及五官／衣物保护一起检查，观察后再决定产品修改。
已构建的 189 仍未安装，不能因硬件恢复就把用户已否定的视觉版本当作本轮最终交付。

### 0.6 偏弱的定点定位

已发送的九块裁图与 v4 实际输入／输出逐像素相同，未拿错结果。原拼图宽 2730 像素，
含诊断 mask 及较多空白；在 390 pt 屏幕上脸部仅约 11 pt。这影响比较，但接收端是否
转码尚未测量，不能把压缩当成原因。原图 0.6 在所有非零皮肤 mask 内的平均 ΔL* 为
2.17，按 alpha 加权后为 3.10；不能以全画面背景比例大解释掉这个偏弱结果。

同一 61,940 个皮肤像素固定坐标的因果对照：仅将人物原生区域重新送分析，源 RGB
保持逐像素相同，旧曲线不变。全幅缩小分析的平均 alpha 为 0.671，原生区域分析为
0.969；ΔL* 从 2.888 增至 4.157。宿主没有二次乘 alpha，问题发生在分析分辨率降低
后的软覆盖权重，而非 PNG 合成。证据分别为：

- `/private/tmp/maichat-beauty-comparison-audit-20261104-j7w3c1gm/comparison-audit.json`
- `/private/tmp/maichat-skin-alpha-audit-20261104-m2ec2p1v/audit.json`
- `/private/tmp/maichat-skin-resolution-control-20261104-t7c0hiuu/pixel-comparison.json`
- `/private/tmp/maichat-skin-fixed-cohort-20261104-wmtn0eqo/manifest.json`

候选先固定同一源、其余参数为零做有限对照。单纯将曝光峰值提至 1 EV 虽更亮，却使
额头／鼻部更多像素到达通道上限，已否决；额外抬蒙版内区权重的收益不足，也已否决。
当前产品方向是修复分析尺度损失、改善中档响应，同时保留 0.65 EV 峰值并处理色域
边界。公开低分辨率站姿图嵌入大画布仅验证尺度与坐标，不冒充用户原生 24 MP 照片。

### v5 实现、验证及再次否决的视觉结果

v5 已接入真正的原图区域细化；新的代理不从旧全图代理放大，也不再缩回全图代理。
曲线改为 `response(s)=1−(1−s)²`，峰值仍 0.65 EV。颜色转换使用未截断的线性 RGB
及连续色域肩部。审查发现固定肩宽使极小强度产生去色跳变：例如暖色 RGB(250,100,70)
在 0.0001 强度下 ΔE 达 4.80，已保留失败并改为肩宽随实际强度／alpha 同时趋零。
维护测试覆盖该极小强度、梯度、分块连续性及细化遗漏／重复／过晚调用失效。Refine、
Protect、Finish 分配返回对象失败时使上下文失效，不能继续发布成功。

最终普通服务与原生回归退出 0，证据为
`/private/tmp/maichat-portrait-response-20261104-8xqkjijt/product-v5-final-summary.json`，
源码冻结为同目录 `native-v5-final-source-freeze.json`。独立固定 61,940 像素对照中，
全幅 ΔL* 5.9645、同新曲线原生裁剪 5.9446，响应比 100.33%；四肢均超过 100%。
两个实际眼点、衣服和四条原图外边框均零改色。证据为
`/private/tmp/maichat-skin-v5-final-cohort-20261104-e5217yn3/comparison-final.json`。
这仅说明分析尺度响应已恢复，不能证明皮肤观感均匀。

实际工具最终输出的上身、小腿紧凑对照已通过 IM 发送，图片区域逐像素与真实输出
相同、同 sRGB、未改色／放大。记录为
`/private/tmp/maichat-v5-final-phone-comparison-20261104-s6_r6xja/images/manifest.json`。
用户再次明确否决：“美白效果还是不行，美白的不太均匀”。因此 v5 作为新的失败视觉
基线保留，不安装给用户当作完成版本，也不以增大强度或全区域平均值回应不均匀问题。
接下来按实际裁图定位新增斑驳，区分原有光影、语义覆盖起伏及原生颜色支持权重。

iOS 190 模拟器构建及两项既有人像 UI 回归已通过（2/2，0 失败、0 跳过，42.788 秒），
实际录屏及聊天／主动全屏截图已查看。原始 24 MP 构图仍有已注明的灰底、角落小脸，
不是用户原始照片。证据在 `/private/tmp/maichat-beauty190-20261104/`。此运行只能验
处理和展示流程，不能推翻用户对不均匀的反馈。未构建／安装 190 真机版，Android 264
也未开始，待这次质量问题有实际修复后再继续平台验收；原真机连接仍为 unavailable。

### 不均匀的空间诊断

复查采用刚发给用户的真实 v5 小腿／上身裁图，不换素材或强度。原始侧面长阴影、
肌肉明暗及部分毛发本来存在；最终 alpha 内部也有细密浅灰纹路，对齐 ΔL* 可见同位
小幅提亮差异，因此不能把全部不均匀都解释为原有光影。图像诊断记录在
`/private/tmp/maichat-v5-unevenness-visual-20261104-j09wr04m/manifest.json`，诊断图明确
标为非成品，原图／结果／alpha 面板保留原生像素，没有用伪彩诊断冒充美白成品。

仅取高置信度皮肤内缩区会遗漏具体问题：该小腿 8,024 点 alpha P5 为 0.9647，
归一化提亮 P5 为 0.9448，看不出严重随机漏涂；但固定原图区域
`[4190,3888,25,27]` 的同明度归一化响应 P5 为 0.8836、最低 0.7986，与 alpha 的
相关系数为 0.919。源码确认原生 RGB 与分析代理的色差绝对支持度会直接影响 alpha，
并继续影响逐像素提亮。该细纹问题有空间证据，不能靠提高全区域平均 ΔL* 掩盖。

目前 whitening 只做逐像素 Lab 曝光／色度变换；本次 smoothing 为零，没有单独的
局部肤色均匀化。修复方向先在仓库外验证一组有界原生候选：稳定可靠皮肤内部力度，
在绑定的分析域计算两尺度低频 Lab 校正场，温和减轻肤色色块，保留原生高频细节和
大范围光影。场跨写入块共享，不能把大半径滤波塞进 8 像素 halo 后留下接缝，也不能
归一化所有颜色支持而使衣物获得满强度。候选未接入产品前，v5 保持失败视觉基线。
固定测量／剖线／阴影对照／纹理及负例合同在
`/private/tmp/maichat-v5-local-uniformity-20261104-kwosba9w/candidate-measurement-contract.json`。

### v6：有界均匀化及必要的端点保护

只做一组仓库外原生候选，调用未修改的生产 Swift prepare/publish，以同字节原图、
同显式参数、完整同 header 的 C++ 对象做对照。先证明薄测试入口可逐像素重放 v5，
再替换核心对象；没有用 Python 调色图冒充实际结果。原始候选的固定局部色块和细增益
纹有改善，但暗部／低色度边界未满足旧保护，因此没有直接接入。

真实 C++ 受控场对照发现：原生 L* 约 4 时旧实现 ΔL* 为 0，较亮底色可使新方案增加
0.616，局部补偿可增加 1.947；C* 10.235 可抽至 7.524。仅给局部分量限幅不足以保护
完整新增校正。后续同类高光检查又发现 L* 97.766 会由旧输出灰值 251 顶到 255。
失败候选、探针和输出均保留在 `/private/tmp/maichat-native-uniformity-private-20261104/`
及 guarded 目录的 before-highlight 记录，没有覆盖失败证据。

同一方案仅补必要护栏：完整 ΔL* 受实际原生像素的黑／白端曝光预算约束，在
18–33、75–90 连续过渡；完整 Δa*/b* 向量按 `min(3, 0.1*C_native)` 限幅，避免越过
中性点。极小强度和零强度仍连续／原样。亮端 90／98／99 与暗端探针均恢复旧实现的
RGBA；量化前的 10% 不是 RGB8 成品严格下限（C* 10.235 的实测输出为 8.848），此
限制如实保留。冻结和红→绿证据在
`/private/tmp/maichat-native-uniformity-guarded-20261104/boundary-review/guard-review.md`。

补完护栏后，仅重跑既有全身图和原始 512 人脸图；同图三联原生裁图已亲自查看。
结果为 `/private/tmp/maichat-uniformity-guarded-host-20261104-tbiq1afv/`。源图、方向、
尺寸、参数、特征和指纹保留；新旧 mask 支持范围未扩大，范围外 RGB 改动为零。
眼／唇和选定发色区域保持；眉边原有少量非零范围仍记录，不宣称整个眉区逐像素不变。

Root 使用先前独立编写的测量脚本逐字节副本复测，选区与阈值未改。新数据在
`/private/tmp/maichat-uniformity-guarded-metrics-20261104-wbkev5hh/candidate-results.json`：
固定小腿 3 像素局部增益细纹 RMS 0.02867→0.01529，约减轻 46.7%；5 像素约减轻
16.6%，小腿中频 a*/b* 色块约减轻 12.2%。宽阴影对照为 33.513，原图 33.039，
v5 为 34.239，保留了真实大范围明暗。两个眼点、衣物、四条边框及 mask 外均零改色。
这里没有重新声称独立 Agent 已完成第二轮复测。

测量仍有混合结果：整体归一化增益范围增加、脸部 3 像素纹理 RMS 比为 1.1019，略高
于原提议 1.1；左臂相关 0.946，低于原提议 0.95，但高于 v5 的 0.884。未修改阈值
或换选区把这些标成全绿。允许接入这次有界修正，以减轻新增细纹／色块；不把数字改善
解释为用户已经接受、所有原有斑点已去除或商业 SDK 同等效果。原有光影、胡须和皮肤
纹理保留，均匀肤色不等于每块皮肤同色。

产品核心与普通／工具入口收敛为同一处理路径，处理版本升 v6；iOS 构建号预留 191，
Android 265／2.2.28。当前正在维护回归与构建，尚不能宣称这两个安装产物已验收。

### 用户再次否决及需求澄清：全平台、身体去毛

v6 最终普通流程回归已通过，53.03 秒；源码冻结位于
`/private/tmp/maichat-native-uniformity-product-final-20261104/product-v6-source-freeze.json`。
iOS 191 已构建并验证 11 个 Mach-O 签名、内置 2B 权重及皮肤 ABI。模拟器两项 UI
测试实际各 19.016／16.857 秒通过，已看录屏与截图；整次 Xcode 命令另有较长启动前
墙钟间隔，不把它作为 App 图像处理耗时。Android 265 构建通过，未安装／运行。

真机的两次安装分别在 60／300 秒超时；后续精确 bundle 查询仍为 188。设备详情为
`localNetwork`、paired、DDI 和开发者模式可用，不能解释成用户没有解锁。曾请求数据线
连接，随后用户再次否决 v6，已明确先不安装 191、连接请求先不用处理。证据都保存在
`/private/tmp/maichat-beauty191-20261104/`；未安装成功，不存在 191 正常真机进程验收。

用户明确反馈仍“白一块黑一块”，并要求查论文／成熟美颜模型，补充要的是全平台；
后来进一步点明“腿上的毛还在，没有剃掉”。这澄清了此前错误假设：**身体细毛不能继续
当作本任务必须保留的纹理**。目标为身体去毛／局部修复、平滑匀肤，再美白；大范围
光影与轮廓保护仍有意义。不能继续只交付提亮／去色图或以纹理保留指标声称符合目标。

已按原始来源核查并在仓库外实测：

- [GPUPixel](https://github.com/pixpark/gpupixel)：Apache-2.0、C++、GL／GLES，官方
  列出移动和桌面平台。固定提交原版、原 LUT 离屏运行同图；macOS SetWhite 另除以
  10，基线已明确补偿为同 shader 强度。零参数像素相同，完整上游文件逐一校验。
  原版美白与美白＋磨皮真实图已发 IM，用户仍否决；它没有身体去毛步骤，且会调全图
  背景色，不能当作仅处理皮肤的成品。证据 `/private/tmp/maichat-gpupixel-baseline-20261104-s58_jqjp/`。
- [阿里 ModelScope 美肤](https://www.modelscope.cn/models/damo/cv_unet_skin_retouching_torch/summary)：
  官方 v1.0.4、完整公开权重与未改源码实际 CPU 跑通。该固定 24MP 样例只改
  166×166 人脸区，框外 RGB 完全相同，腿和手臂未处理；人脸学习匀肤、身体分割＋
  曲线美白不等于身体去毛。Lena 人脸有效。证据 `/private/tmp/maichat-modelscope-baseline-20261104/`。
- [1997 DullRazor](https://www.dermweb.com/dull_razor/) 已有“检测毛发再局部修复”的
  成熟思路；作者原软件仅非商业免费，未复制其实现。仓库外 C++ 原型复用 OpenCV
  black-hat／形状筛选及官方 Telea：首版 630 像素不足，按目标澄清纳入短细毛与小
  毛囊暗点后为 2245 像素，mask 外／身体范围外均零改动，但仍未达到明显剃净。
  证据 `/private/tmp/maichat-calf-hair-prototype-20261104-54w_robg/`，未把失败原型作为成品发送。
- [MI-GAN](https://github.com/Picsart-AI-Research/MI-GAN)：作者 28MB ONNX 在同一
  630 像素 mask 上实际推理仍保留明显暗纹。原 pipeline 的洞腐蚀／高斯回贴会削弱
  细毛修复，因此正在区分检测遗漏与回贴流程，不能直接断言模型本体成功／无效。
  证据 `/private/tmp/maichat-migan-hair-20261104-j_8tm9qy/`，新检测 mask 与修复器
  对照仍在进行，未接入产品。

以上各项均为原版／原生算法效果对照，没有通过额外改色或大范围模糊伪造成品；当前
公开站姿素材本来低分辨率，嵌入 24MP 画布不会增加可辨毛发信息，不能以此替代真实
清晰身体照片验收。

### 全平台接线审计纠正

当前共享库存在不等于各端已走新链路。只读审计确认：iOS 用 Analyze／Refine／
Protect／Tiles；**Android 美颜实际仍调用 PlanFaceRegion→ProcessRegionRgba 的
脸部 ROI**，其 Bitmap 可读 32MP，但未接身体 SkinPlan。此前将 Android 概括为
“完整光栅 12MP 美颜”不准确；12MP 门限属于其他变换／美体路径。

Qt Windows／macOS 桌面未接 Portrait 普通服务／工具，OpenCV 模块列表缺 photo，
模型／ORT 打包也未按 Portrait 独立配置；macOS 既有桌面视觉工具还受 Windows 条件
编译限制。Linux 当前不是已正式支持的桌面产品，不能把底层 GL 库支持说成 MaiChat
Linux 已完成。所有后续方案以共享 C++ 普通服务与同一 Agent 工具实现为准，平台只做
图像 I/O、权限和展示；不能选择 iOS 独占实现当全平台交付。


### v7：用户确认净肤 + 身体范围美白（2026-11-05）

复现目标现明确为：对同一人物手臂、小腿等可见身体皮肤，先减轻细毛、毛孔暗点及细碎肤色
差异，再在身体皮肤范围内美白；不再把体毛全部当作应原样保留的纹理。用户审阅
`calf-cleanskin-v3-body-scoped-comparison-zh.png` 后明确回复“这个效果可以，净肤 + 身体范围美白”。
这是固定公开素材的局部参考确认，不扩大为任意原始高分辨率照片或全平台产品验收。

普通 C++ 新增 `MaiBodySkinRetouch` 与 `MaiPortraitWhiteningLut`，Engine 的正式路径复用两者；
旧 v6 Lab 曝光／局部校正业务删除。身体净肤限定可靠归属 class2 内区，脸、头发、眉眼、唇、
胡须不走身体去毛。黑帽／连通区域 + Telea + 低频引导联合双边匀肤及20%高频残差恢复，
随后同一批新像素进入四级 LUT 美白，不拿旧白化结果贴回。UI .6 映射实际 shader .25，
与用户已审参考一致。全图初始分析512不变，从原图取人物区域精化及共享材料平面提高至1024；
所有输出块共享该场，避免把大半径算子塞进8像素halo产生接缝。

`remove_body_hair` 运行时与schema一致严格bool；缺省AUTO按max(smoothing, whitening)连续渐入，
另返body_cleanup_request_mode，AUTO不把字符串塞进boolean参数。参数全零时不执行净肤。
返回clean_applied/clean_strength/clean_scope/clean_skip_reason和whitening_shader_strength/
whitening_blend_gate；没有可靠身体范围必须诚实报告跳过，不能以脸部成功冒充全身净肤。
个人偏好仍使用原八属性，不增加伪训练权重。

本树 main、基准提交eee43402（未提交改动）完整native＋ordinary回归exit0：
`/private/tmp/maichat-portrait-v7-native-20261105-final/ordinary-summary.json`，源码与二进制摘要为
同目录source-and-binary-manifest.json，日志`/private/tmp/maichat-portrait-v7-final.log`。
24MP普通文件路径实际30 tiles、原图ROI[3960,3101,1072,1072]→1024²，prepare/publish约2.37秒；
两臂/两腿测试区域改变像素[6600,3526,7866,3600]，衣服与背景保护区域[0,0]，mask外变化0。
原始公开素材是1000×667、放大置于24MP画布，仅证明原尺寸路由／范围与出图，不冒充原生24MP
毛发细节质量验收。普通原尺寸图四肢外脸范围19364像素变化、保护区域[0,0]。

原版GPUPixel CPU LUT逐个RGB8穷举16,777,216色、最大误差1LSB；身体净肤与v3局部参考逐像素
一致。ASan/UBSan、零/tiny、严格参数、暗线对比减弱、20%残差、跨tile、profile/geometry、
取消与发布回归通过。实际App构建验证单独记录，不能用上述macOS普通服务替代安装产物。


v7 iOS192 实际 App：使用 `MaiChat.xcworkspace`、部署目标16.0构建（独立project缺Pods准备阶段；
workspace旧Aliyun pod目标12.0低于当前SDK支持范围，通过本次命令统一目标16.0）。
3项UI测试0失败，实际测试45.03秒；54.89秒录屏逐段查看，真实身体图在聊天中显示缩略图，
点按后放大、收起后回消息。body fixture实际receipt：clean_applied=true、shader=.25、
body_skin_pixel_count=21568，Simulator Vision未可靠保护该小脸，因此face_color_skipped=true；
诚实跳过脸部，不把此项说成真机脸部效果验收。测试素材公开，未请求付费模型；消息展示沿用
既有fixture，不冒充实际Agent发送与持久化验收。QA：
`/private/tmp/maichat-beauty192-20261105/sim-verification.json`及ui-attachments。

设备192 App构建成功，冻结副本11个Mach-O逐个签名验证、2B GGUF SHA及v7处理标识均核对；
GPUPIXEL_LICENSE/NOTICE随包文本与源码摘要一致。当前iPhone14ProMax为localNetwork连接，
单次安装180秒超时exit2；未据此声称安装成功，已请求用户改用数据线。尚不替换用户Mac App。
具体产物与安装状态：`/private/tmp/maichat-beauty192-20261105/device-artifact.json`和device-install-status.json。

安装超时后独立查询确认手机仍为2.2.4 (188)，并非192；没有启动旧版来冒充本次运行验收。


### Android 实测追加：同一脸的多个身体候选（最终193／267候选）

Android266首轮8项真实回归7通过、1失败：24MP EXIF6合成Lena在Analyze抛
person_ownership_ambiguous。按原始RGBA、同hints／参数／global64／fingerprint做反事实，
268×357的现有power-of-two proxy与精确384×512 proxy都在实际JNI失败，因此分析尺寸不是充分根因，
没有先改helper或调归属阈值。Root在macOS同核心定点副本重现：357有两个person候选对同一脸覆盖
.723／.771；512有三个候选覆盖.788／.838／.882。真实输入与probe日志位于
`/private/tmp/maichat-android266-20261105-ge66p_pm/proxy-evidence/`，不含用户照片。

最小修复保留原归属竞争阈值；仅唯一已定位人脸在身体歧义时拒绝身体、转现有受原生mesh保护的
face_skin_only。未确定身份的同脸竞争seed不再把该脸自身全部当foreign；其余人物／其他脸硬排除保留。
最终alpha仅为原生faceProtection×faceExclusion，授权脸外0，不保留coarse蒙版插值残量。
mesh失败继续失败；多脸／无人脸歧义继续失败；显式remove_body_hair=true仍body_skin_unavailable。
新增body_ownership_skip_reason=overlapping_person_seeds，commit收据透传，不能谎称全身成功。

私有两种proxy及同一完整JPEG检查均由原失败转安全脸部输出：changed126124，skin129428，
body ownership0、净肤false；mesh授权脸外mask0／changed0，zero-mask changed0，显式身体净肤拒绝。
独立桌面代码审查已确认以上边界，旧身体效果配方未改。正式全套回归与平台产物在后续小节追加。
最终另升iOS193、Android267以区分已验证过但有该失败的192／266候选，旧候选不作最终交付。

最终Core全套native/ordinary通过；16个旧身体图与蒙版逐RGBA exact0。正式object对原Android两proxy／JPEG的正反例也通过。iOS193重新构建，3项UI0失败，root查看新录屏与身体图。真机193链接曾因磁盘满失败；仅释放本次ModelScope研究临时venv和已下载公开weights cache，保留源码、正式输出、模型来源/hash与requirements，重新构建成功并冻结11个Mach-O、2B GGUF、safe-partial版本和许可。此时设备已实际wired连接，正在安装。删除研究环境后重跑该外部baseline需依requirements及来源重新准备，不应称依赖仍留存。

最终iOS193通过wired安装exit0，独立系统查询2.2.4(193)；正常启动、1290×2796截图由root查看，实际安装URL与进程30951匹配，后续查询仍存活。证据为`/private/tmp/maichat-beauty193-20261105/device-verification.json`及device-artifact.json。没有扫描或清理用户Workspace/相册，也没有拿用户照片执行真机美颜质量验收；本轮真机边界为真实产物安装和正常启动。


### 真实照片再次否决：逆光肤色增色（2026-11-05，尚未结案）

用户随后提供同一张私有人像的完整前后文件，4284×5712，相同构图；实际参数
smoothing=.35、whitening=.5、brightening=.2、warmth=0，geometry=false。
用户已明确更正12MP说明：原始尺寸直接处理成功，不再修改尺寸门禁。所有私有照片、
派生图及像素分析仅置临时私有QA目录，不加入仓库或发送给外部模型。

本次按同参数、普通文件服务在共同sRGB色彩域复现：L*25.135→30.472，
C*13.534→21.486，色度增加约58.8%；人物归属可靠，身体净肤已执行，body245681px，
changed300982px、30tiles、约2.75秒，原尺寸输出。源JPEG带Display P3配置，
用户结果JPEG没有ICC；比较前先归一色彩，且有损JPEG压缩误差不能当作精确算法差异。
用户反馈仍不满意，所以此前公开小腿参考的接受不能当成当前真实照片整体效果验收。
当前仅私有验证whitening颜色保护候选：保持原明度变化与净肤／覆盖，抑制额外增色及色相偏移，
先证明偏红因果，再决定是否需要独立逆光照明补偿；尚未修改正式处理配方或宣称修复。


私有纠红保护（原参数不变）使全皮肤C*21.486→13.851，L*30.472→30.383；原mask相同，
相对当前版本的mask外变化0。后续唯一暗部补偿候选使L*33.734、C*13.867，脸部额外+1.25L，
双小腿额外+4.42/+5.20L；脸减平均小腿的L差14.96→11.40（原图11.89）。
原版LUT实际.2083333强度已独立用全部16,777,216 RGB8验证，CPU/GPU最大1LSB，排除移植
分段／通道错误。两候选图和最终4k无损PNG已发回用户，审美反馈仍待确认。

产品准备使用固定请求D=25*s*(2-s)的局部L曲线，移除私有初版E(mean)反馈；本图原E=1，
因此去门控后RGBA/mask与已发候选完全相同。保留原输入已生效的提亮／暖度色相，限制白化
额外增色，并在必要时沿色度方向收至sRGB色域内。不能称其为纯原版LUT最终输出；utility
本身继续保持上游parity。新曲线仅对局部L<50及toe生效，D≤25时曲线单调C1，黑白端点保留；
不保证整条含感知／滤波的pipeline跨任意输入全局单调，也不再声称亮图全图无变化。
正式v8集成与平台194／268回归尚在进行，不能据私有图直接结案。

### v8 整 App 验证收尾（2026-11-05）

iOS 194 最终整 App 五项 UI 回归通过，其中三项直接验证人像结果留在当前聊天、点击才
放大、24MP 原尺寸与同尺寸 mask，以及身体净肤输出。开发者查看了最终截图及录屏。
最终设备包 11 个 Mach-O 严格验签、内置 2B 模型校验通过，但安装返回 CoreDevice 4016，
未完成真机安装。证据为 `/private/tmp/maichat-my-markdown194-final-20261105/` 和
`/private/tmp/maichat-beauty194-20261105/device-reviewed-artifact.json`。

Android 268 正式 App 已安装到专用 emulator-5580，原八项 PortraitRegion 回归通过；
正常 MainActivity 的身体预览也已实际截取、读回并查看。普通设置/存储与 Markdown 集成
另行回归，不调整已冻结的 v8 配方。Qt 正式 App 2.2.48 完整构建、打包、原生启动检查通过，
当前正式目标的 desktop_portrait_service_test 也重新构建并以 CTest 13.07 秒通过。
保留旧运行 App，替换等待用户确认；Windows 未运行验收。

原 v8 指纹清单的 43 个源文件中，处理核心及工具 42 个保持一致，只有移动 CMake 因接入
普通 app_storage 桥新增源清单而变化。未根据工程测试宣称用户已认可私有人像的最终审美；
此前候选仍待用户反馈。本次未把私人照片或输出加入仓库。

15:52 安装更新：用户再次要求安装最新 iOS App，设备连接恢复后最终 194 已覆盖安装并
前台启动，设备回读版本及实时 PID 32177 已核对。正式包与此前已验签、已做模拟器回归的
冻结包一致。此次仅安装和启动，没有对用户照片进行真机处理或扩大为效果验收。
