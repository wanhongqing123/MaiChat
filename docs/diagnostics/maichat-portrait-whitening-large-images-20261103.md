# 本地美白：大图、小脸、蒙版、参数及文件依赖问题（2026-11-03）

## 范围与验收目标

合并用户对 `mobile_beautify_image` 的主报告及后续补充，作为可审阅的问题记录，
暂不重复创建 GitHub issue。记录不包含私人原图、账号、图片 UUID、原图路径或签名地址。
用户原图的格式信息按其报告记录；不据此推定未提供的 ICC、色彩范围或 EXIF 信息。

操作：从相册导出原图，调用人脸检测及独立美白；必要时比较同一图的整图与局部构图，
并检查输出尺寸、参数、蒙版及文件错误。预期：保留原图尺寸和背景，自动完成可靠定位和
局部修肤；原图不覆盖。无法处理时返回可区分、符合实际失败阶段的结果，不能让 Agent
依赖手动裁图、硬边覆盖、反复提高强度或猜测文件何时生成。

调查基线为 `ecef0b19` 加本轮未提交界面工作。先以旧程序复现；随后经授权修改
Portrait 核心及移动宿主，具体实现和新回归见文末，不能把基线行号当成修改后状态。
先检查根目录及 `MaiChat/` 的 AGENTS、MaiAgent 编码规范、Portrait README 和模型说明。
没有操作真实账号或设备，没有调用付费模型。后续验证仅在独立临时目录构建人像
原生测试，不修改产品/vendor staging；主任务负责实际移动构建和设备回归。

## 用户报告与证据分类

| 项目 | 用户报告 | 已确认及尚缺证据 |
| --- | --- | --- |
| 大图门禁 | 5712×4284，24,470,208 像素，3,863,271 字节；MJPEG/yuvj420p，range=pc、matrix=bt470bg、SAR=300:300、DAR=4:3；超过 1200 万像素被拒绝 | iOS 宿主和 C++ 核心均有明确 12,000,000 像素上限。不能仅提高这个数值；当前数据流持有多份整图 RGBA。 |
| 小脸定位 | 整图缩至 3996×2997，后续附录另有 3840×2880，仍返回 `faces:[]`；1200×1400 裁图检出约 201×201 的脸，位置约 `(580,341)`，报告 confidence=1 | 公开素材已复现“整图/缩整图失败，局部构图成功”。这是构图/检测尺度问题的证据；尚未在用户设备重测其具体 confidence。两个缩放尺寸属于不同试验，不应混成同一次调用。 |
| 美白差异 | whitening=0.5 的 changed_fraction=0，同时 mean_abs=0.00513、PSNR=41.5968；1.0 的 changed_fraction=0.0171、mean_abs=0.00589、PSNR=38.4009 | 已查明比较工具默认统计灰度差大于 16 的像素比例，不是逐字节差异。非零 mean_abs 和有限 PSNR 已证明 0.5 的结果不是完全相同。核心也没有 0.5 强度开关。 |
| 蒙版及合成 | 未返回 mask；为绕过大图/失检而手裁再覆盖，希望减少手工操作并保持原尺寸 | 用户补充直接 overlay 自己那张裁图也没有接缝，因为边界未改；不能记录成必然有接缝。两次 alphamerge 失败是 5712×4284 与 1900×2500 输入尺寸不一致，属于调用方滤镜链错误。 |
| 未知参数 | Schema 无 `action`，传 `action:"delegate"` 仍被忽略 | Schema 写了 `additionalProperties:false`，但通用宿主工具不进行运行时字段验证，Swift/C++ 只取已知字段。需要实际调用回归后将“静态确认”提升为“动态确认”。不能把该字段视为已有委派能力。 |
| 计数语义 | `face_count=1`、`person_count=0` 看起来矛盾 | 美颜只使用宿主人脸提示；人体检测仅在美体分支运行。当前 0 兼作“未分析”，不是“照片中无人”。 |
| 文件 I/O 与顺序 | FFmpeg 生成与美颜读取同一路径被并行提交；比较输出文件与生成同批并行却得到数值，因此怀疑跨文件或缓存 | 调用序列本身有依赖错序。尚未取得对“跨文件/缓存错误”的决定性证据，不能归因引擎缓存。必须动态验证并如实返回当前 I/O 状态。 |
| 其他工具约束 | `cv_register_translation` 对 1900×2500 返回 4M 上限；FFmpeg 封装拒绝 `-y`/`-stdin` 等参数 | 与美颜算法分开记录。前者应给尺寸/缩放建议；后者是已明示的覆盖/交互安全边界，不能为便于试验直接放开覆盖。 |

初始调查未把未知参数或某条具体 I/O 错误冒称为本机已复现；后续的公开基线 probe
和修复回归见文末。`changed_fraction=0` 已能由比较工具的阈值定义解释，不能继续称为
“结果逐字节没变”。

### 附录调用顺序（匿名文件名）

1. 原图独立美白：`whitening=0.5, smoothing=0, brightening=0, warmth=0,
   use_personal_model=false`，收到中文超 1200 万像素错误。
2. FFmpeg `scale=3840:2880:lanczos -frames:v 1` 生成 `scaled.png`，与读取该文件的
   美颜在同批并行；用户承认读取时文件可能未生成。后一次独立 `detect_faces`
   对完整的 3840×2880 文件返回空 faces。
3. 完成 `crop=1200:1400:1500:1900` 后再检测：face box `(580,341,201,201)`、
   confidence=1；left_eye `(636,408)`、right_eye `(716,399)`、mouth `(686,483)`。
   对该裁图美白 0.5 返回 face_count=1、person_count=0。
4. 0.5 / 1.0 的比较数值如上表；其他强度未提供足够完整数值，不补造序列。
   有些比较调用与生产所比较文件的任务并行，单独归为未确证的依赖/文件状态问题。
5. 另取 `crop=1900:2500:1250:1800`，美白 0.6；差分阈值 2、gblur 18、alpha
   与 overlay 合回原图并输出 JPEG。用户明确补充其简化 overlay 也没有接缝。
   alphamerge 的两次尺寸错误不作为本工具输出尺寸异常的证据。

## 源码定位（修改前基线）

- `MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MobilePhotoTools.swift`：
  `transformImage` 约 370–397 行先检查文件，再恢复已登记结果的原图路径、读取 CIImage，
  并以 12,000,000 像素拒绝大图。文件类型不符、空文件和超限共用一条大小错误；
  末尾约 519 行又把其他异常统一映射为 `internal`，错误分类需要拆开。
- 同文件约 405–438 行的美颜人脸定位直接使用当前 CIImage。独立
  `MobileImageTools.swift` 约 56–60 行的 `detectFaces` 却先把整图最长边缩至 2048。
  两个入口没有共用多尺度、局部窗口和回到原图坐标的策略。
- `MaiChat/Portrait/MaiPortraitEngine.cpp:286`：再次以 12 MP 检查 RGBA；随后 clone
  整图。约 314 行的 `faceCount` 是宿主提示数，约 418 行才在美体分支填写 `personCount`。
- `MaiPortraitEngine.cpp:137`：`skinMask` 把整张照片直接缩成 256×256；约 323 行
  的 mesh 使用选中脸局部 crop。于是“局部 mesh 成功、整图皮肤分割过弱”可以同时发生。
  这是需要测 mask 覆盖率的候选机制，不等于已经测得用户照片的 mask 为零。
- 同文件约 337–391 行：只在选中脸附近应用皮肤处理。whitening 使用浮点 Lab，
  `amount = strength × mask / 255`，提高 L*、同比收缩 a*/b*，再量化为 8 位。
  弱 mask 与量化可能影响小强度的可见差异；不存在硬编码的 0.5 阈值。
- `MaiPortraitBridge.h` / `.cpp`：目前只返回 RGBA、元数据和错误字符串，没有 mask；
  commit 输出参数和计数。普通图片也会查询原图追溯表；“没有登记记录”应继续使用
  已验证的原文件，不能被解释成需要还原某个不存在的缓存。
- `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` 约 454 行：通用 `MaiMobileHostTool`
  原样转发参数。约 1235 行的美颜 Schema 虽禁止未知键，运行层和 C ABI 未据此拒绝
  `action` 等未知字段。模型端 Schema 不能代替宿主校验。
- `MaiAgent/include/MaiCvImageAnalysis.h:30` 的默认 `changedPixelThreshold=16`；
  `MaiCvImageAnalysis.cpp` 约 289–302 行分别计算逐通道 mean_abs、逐字节 identical、
  PSNR 和“灰度差 > threshold”的 changed_fraction。它们不是同一个指标；
  工具还明确返回 `color_managed:false`，不执行 ICC/HDR 色彩管理。

## 已运行的公开素材最小复现

只使用仓库自带 OpenCV 公开测试图，不使用用户照片。把原有 512×512 图像原样贴到
5712×4284 中性灰画布的 `(104,3684)`，得到位于左下角的小脸构图。

三份本机临时工件：

1. `corner-face512-24mp.png`：5712×4284，24,470,208 像素。
2. `corner-face512-12mp.png`：整图等比例缩至 3996×2997，11,976,012 像素。
3. `corner-face512-crop.png`：原画布区域 `(0,2884,1200,1400)`，1,680,000 像素。

使用现成 `MaiPortraitTests` 原生 QA 可执行文件，传入相同固定模型、运行库及公开站姿
素材，没有重编译。该程序实际调用 macOS Vision 和 ONNX 核心：

- 24 MP 原构图：`FAIL Fixture face not found`。
- 11.976 MP 整图缩小：`FAIL Fixture face not found`。
- 1200×1400 局部图：exit 0，人脸、局部修肤、美白和已有美体回归通过。
  whitening 0 / 0.3 / 0.6 的原生同皮肤采样结果：
  L* `58.7772 → 59.696 → 60.7283`，C* `42.8261 → 39.4665 → 36.1436`，
  HSV 饱和度 `49.9849 → 46.7422 → 43.5404`，平均色相角变化约 `0.315°`。

这个现成程序先执行 Vision 再调用核心，所以 24 MP 用例未到达核心门禁；
门禁本身有双层源码证据。它的 Vision 版本和设置也不能替代 iOS 真机验收。
原生 QA 程序与模型的调用形式：

```text
MaiPortraitTests <models-directory> <bundled-ort-library> <public-face-image>
                 <public-standing-image> <new-isolated-output-directory>
```

仓库测试入口是 `MaiChat/Portrait/tests/MaiPortraitTests.mm`；目前 runner 属于本机 QA，
尚未注册为可直接在干净 checkout 执行的 CMake target。后续修复需要补正式的回归入口，
而不是把本机旧二进制当成新代码的验收。

## 差异指标必须按固定 ROI 和颜色解释比较

对上述局部测试图，whitening 0.3 / 0.6 的全图逐字节 RGB 差异占比分别约
`0.8523% / 0.8688%`，但固定皮肤采样的 L* 提高、C* 降低均明确成立。
这直接说明“全图改动少”不等同于“美白无效”。原图未翻转；与输出比较时其他区域不变。

用户提供的 201×201 面框在 1200×1400 图中占约 `2.40%`，可见皮肤本来更少；
在这个输入域中 1.7% 的全图像素发生变化并不反常。不得拿裁图分母和 24 MP 原图分母
混用。也不得用模型只找到脸这一事实，推定皮肤分割一定可靠。
用户 0.5 的 mean_abs=0.00513 对应按 255 归一化的非零平均通道差；PSNR 41.5968
也不是 identical 时的 null。该组报告不能支持“美白完全未生效”，不应据此盲调曲线。

后续回归至少同时保存：

- 同一方向、相同尺寸、相同颜色解释下的基线解码像素；分别记录源矩阵/原色/传递函数/
  范围和 ICC，不能只凭输出同为 PNG 就认定比较可比。
- 独立于强度的固定皮肤 mask、选中脸 ROI、眼眉唇排除区域及背景对照区域。
- mask 内平均/分位 L*、C*、色相角变化；阈值明确的 changed_fraction；背景逐字节差异。
- 请求参数、实际生效参数、个人模型是否启用和原图追溯关系的匿名标识。

## 修复分层

### P0：可解释的失败、上下文和蒙版

1. 把文件不存在、目录、空文件、超字节限制、无效/未写完图片、超像素限制、未检出脸、
   mesh 不可靠、皮肤 mask 无效、量化后无可见效果和输出写入失败分开。
   返回稳定的机器错误码、实际阶段、输入尺寸和已生效参数；不用私人路径或文件名诊断。
2. 显式非零修肤却没有可靠皮肤区域时，不登记“已美白”结果。全部参数为零仍可作为
   明确的 identity/no-op 调用，不能和定位失败混淆。
3. 暴露与输出同一坐标系的 mask/作用区域，以及 mask 覆盖率、处理 bbox、实际改变
   像素数。说明 mask 是编辑覆盖诊断还是未混合 patch 的合成 alpha；不能对已经混合
   完成的结果再乘一次 mask。
4. 美颜返回人体检测未执行的状态，例如 `person_count:null` 和
   `person_analysis_performed:false`；不能继续把默认 0 宣称为真实人数。
5. 对同一路径的生产和消费显式串行等待；FFmpeg 必须成功退出并提交完整文件后才能
   开始美颜。失败按当前文件实际状态报告，不从“可能存在的缓存”构造成功。

### P1：内部自动 ROI、原尺寸和有界内存

新增共享普通 ROI/patch 服务，保留小图 `apply` 入口，不能只是把 12 MP 常量改大。

数据流：原始文件 → 有界全图分析 proxy → 重叠窗口补检及全图坐标 NMS → 选中目标的
上下文 ROI → mesh/皮肤分割/局部修饰/羽化 → 原始 lazy 图像上合成 → 原尺寸新文件。

- 人脸 box、眼部位置和 ROI 都以 EXIF 转正后的原图左上像素为契约。每个窗口保留
  crop/scale/rotation 的正逆变换；去重后才按全图从左到右选择 `subject_index`。
  旋转、镜像和临近边缘的 ROI 必须用已知像素图验证坐标，不能依赖肉眼猜偏移。
- skinMask 针对选中脸及足够上下文进行 256 模型输入，不把原图小脸挤成少数像素。
  保留眼眉唇排除及软边；采样和滤波保留 halo，羽化在 ROI 外边缘归零。
- 模型偏好 features 仍来自全图小缩略图，面框面积占比也按原图计算；不能把 ROI
  代替整图特征后继续无说明地使用旧个人模型。
- 全图 proxy 最长边不超过 2048；处理 ROI 初始预算不超过 2048×2048，逐窗口处理。
  大图最终编码最多保留一份原尺寸 RGBA，5712×4284 约 93.35 MiB；禁止当前
  source、destination、vector、C ABI malloc、Swift Data 多份大图副本同时存在。
  额外工作内存以约 256 MiB 为初始工程预算，须测峰值，不能把估算当成设备通过。
- iOS 只适配 ImageIO/CoreImage 的有界区域读取、Vision 和最终编码；Android 只适配
  SDK/区域解码及最终编码。ROI 选择、坐标、mask、羽化、参数和输出语义共用 C++。
  后续需要完全流式时，来源读取/编码采用条带回调，不给平台各写一套修肤算法。
- 结果保持原尺寸，用户不再手裁、硬边覆盖；可选输出 mask 供审查，不要求用户自己合成。

输出提交：同目录唯一临时文件 → 编码结束并关闭 → 校验完整性/原尺寸 → 原子重命名，
随后在账号代际和取消状态仍有效时登记结果/偏好元数据。输入不覆盖；任何阶段失败只清理
本请求拥有的临时文件，不能留下可被当成完整结果使用的半文件。

### P2：严格字段及协议测试

使用共用参数验证器校验美颜、美体及兼容 transform 的各自字段集合、类型和数值范围；
拒绝 `action:"delegate"`、模型伪造 `_face_hints` 和其他未知字段，不静默丢弃。
内部工作区、账号及定位提示只能由宿主附加。未知字段、I/O 分类、并行读半文件的现象
必须补实际失败用例；不能仅因 Schema 写了禁止就认为运行时已经禁止。

## 本轮实现（未提交源码，2026-11-03）

- 新普通 `MaiPortraitRegionPlanner` 合并全图及窗口提示，统一原图左上坐标、人脸编号、
  subject 选择和最长边 2048 的原生 ROI 预算。原图元数据和实际 ROI 像素是不同预算，
  旧 12 MP 完整光栅入口没有放宽。`MaiPortraitBridge.h` 只新增 C 兼容 plan/merge/
  process-region/publish 函数，旧完整光栅 ABI 继续可用。
- Engine 在选中脸及上下文运行皮肤分割，叠加密集点脸部轮廓、眼眉唇排除和羽化。
  非零调色请求但皮肤 mask 为空，或量化后完全没有 RGB 改动，明确返回失败；零参数
  identity 仍可通过。mask=0 的像素直接保留原 RGBA，避免 Lab 往返量化误改背景。
- ROI patch 已完成软边调色，宿主原位替换，不能再乘导出的 mask。新结果附原图坐标
  bbox、皮肤覆盖、实际参数、固定 mask 权重下 L*/C* 前后值和精确 RGB 改变数。
  人体检测未执行时用 null 加 performed 标志，不再用 0 伪装已统计。
- 全局 64×64 proxy 和原图脸面积比继续生成个人模型八维特征，`analysis_version`
  保持 `mediapipe-onnx-20261102-v2`；新增独立
  `processing_version:native-face-roi-20261103-v3` 标明处理算法变化。
- iOS 新普通 `MaiPortraitPhotoProcessor` 被 detect_faces 和 beautify 宿主共用。
  全图最长边 2048，加 1400 原生重叠窗口，逐个检测、映射、NMS；Vision 真正抛错时
  返回 `vision_failed`，不能冒充成功的无脸结果。全图指纹按 128 行拼接 RGBA SHA256，
  尺寸前缀和方向保持已有定义；CoreImage 的坐标换算经已知四行色块 probe 验证。
- iOS 不建立多份整图 Swift Data；只渲染有界 ROI。ImageIO/CoreImage 解码和最终
  编码仍有框架内部完整光栅工作内存，因此不宣称“没有整图内存”。先明确限制原尺寸
  I/O 为 32 MP，超限报告 `output_budget_exceeded` 和实际尺寸，禁止静默缩图。
  同进程照片服务 single-flight，第二项请求返回 `busy`，不叠加多个完整编码峰值。
- stat 不存在、目录、空文件、超 50 MB、JPEG/PNG 尾不完整、ImageIO 状态／真实 proxy
  解码失败、处理中输入被改写各有分类。标准 JPEG 当前要求最后 EOI，MotionPhoto 或
  合法 trailing data 尚未验证，列为格式边界，尾标记不代替 ImageIO 完整性检查。
- 完整 image 与 mask 写到同目录唯一临时文件，校验尺寸及完整状态；宿主核对账号域和
  取消后才原子发布、登记偏好账本。失败清理本请求产物，不覆盖原图。工作区切换会永久
  取消旧请求；Swift Task 取消会取消 Vision 并阻止发布。现有 Agent 停止若没有传播
  Swift Task 取消，本修复不宣称已覆盖该控制链；同步 ORT 调用也不承诺中途可打断。
- 共用运行时校验拒绝未知键、错误类型和越界值；宿主附加的 operation、face hints、
  workspace/session 内部字段有独立白名单，模型不能伪造这些提示。

## 新公开回归及实际边界

独立编译的旧核心 probe 重复测试 0/.3/.5/.6/.7/1。固定扩展脸框用于比较，不能把它
冒称精确皮肤 mask：0.5 时全图逐字节 RGB 改动约 0.86494%，灰度差>16 的占比约
0.0000595%，mean_abs=0.00022954，PSNR=50.7664；固定脸框 L* 47.2588→47.7671、
C* 37.3721→35.6131。这再次证明“阈值比例约零”与“完全没改”不同。
旧核心还实际接受 action:delegate 和未知 probe 字段，输出与同参数 0.5 完全相同；
新普通服务对该请求返回 `unknown_parameter`。用户具体图片的像素和缓存怀疑没有重测，
不能把公开构图的结论冒称其私人图片实测结果。

新的 `MaiPortraitRegionTests.swift` 直接编译正式 iOS 普通服务和真实共享核心，使用
公开 512 图块在 5712×4284 灰画布左下角生成小脸，结果如下：

| 验收项 | 真实结果 |
| --- | --- |
| 角落小脸检测 | 1 张，原图 box 约 `(322.38,3911.32,151.09,151.09)` |
| ROI / 输出 | ROI `(208,3797,378,378)`；image 与 mask 均 5712×4284 |
| whitening=0.5 | 固定 mask 加权 L* `61.4534→63.2242`，C* `42.3581→36.1533` |
| whitening=1.0 | L* `61.4534→65.0993`，C* `42.3581→29.9546` |
| 背景完整性 | 逐带比较完整编码后输出，mask=0 的 RGB 改变数 **0** |
| 实际变化 | 0.5 / 1.0 分别改变 13,746 / 13,893 像素；不可与用户图或不同分母混用 |
| 计数 | face_count=1，person_count=null，face/person performed 分别 true/false |
| 输入及字段 | 不存在=`file_not_found`；未写完 PNG=`input_not_ready`；action=delegate 为 `unknown_parameter`；字符串强度为 `invalid_parameters`；预取消不产生结果 |
| 旧回归 | 4 个真实感知模型、零参数 identity、局部背景保护、脸部几何、美体、个人偏好／旧七属性兼容均通过 |

单独进程只跑一次 24 MP 原图普通 prepare→image/mask 编码→publish，不包含生成素材和
全图比较，耗时约 2.95 秒，最大 RSS **479,428,608 bytes（约 457.2 MiB）**。
阶段 probe 的高水位依次约 202.5 MiB（输入读取／proxy 解码）、244.9 MiB（真实模型）、
373.9 MiB（image 编码）、457.3 MiB（mask 编码）。完整测试同时生成素材、执行多轮
处理和全图比较，曾达到约 973 MiB；不能把该混合峰值当成普通单次值，也不能拿
ROI 自身大小推定整个 App 只用十几 MiB。Mac 数字不能代替 iPhone 的 jetsam 边界，
32 MP 以上输入和重复调用后内存稳定性仍需目标平台测量。

可复现入口 `MaiChat/Portrait/tests/run-native-tests.py` 接受已有原生构建库、匹配的
OpenCV 生成头、固定运行库以及一个全新的输出目录。所有新对象、合成图片及个人偏好
测试库在隔离目录，不写共享 staging。此脚本只是 macOS Vision/CoreImage 正式服务
验证；iOS 模拟器、真机、Android 实际安装产物由主任务补充结果。

尚未验证私人原图、其 ICC/EXIF 的具体色彩观感、MotionPhoto/trailing JPEG、超 32 MP、
全身皮肤美白和任意姿态。极大近脸 ROI 仍明确报预算错误，不默默降细节。诊断记录不
公开原图 UUID、私人路径、账号、图片内容或任意服务响应；用户并行文件调用不据此
升级成引擎缓存故障结论。

模拟器正常 Root/宿主回归入口新增为
`PortraitUITests/testLargePhotoWhiteningReturnsOriginalSizeAndSkinMask`，环境变量
`MAICHAT_PORTRAIT_LARGE_TEST=1`。fixture 仍使用 bundle 公开人脸图，真实调用
`mobile_detect_faces` 与 `mobile_beautify_image`，校验原尺寸 PNG/L8 mask、计数、
源 SHA、皮肤指标、ROI 边和背景采样以及三类失败。成功后仍经正常交付消息打开预览；
XCTest 保留状态/结果缩略图、全屏、返回截图及去路径 `qa.json` 附件，App 测试目录
`tmp/portrait-large-photo-qa/qa.json` 可独立导出。此处只是测试代码已准备，实际174
构建及模拟器/设备结果由主任务记录，不提前记为通过。

## 174 iOS 模拟器累计构建回归

2026-11-03 正常 Root 的 2.2.4 (174) 实际运行，两个人像 UI 用例分别通过：
`testLargePhotoWhiteningReturnsOriginalSizeAndSkinMask` 12.078 秒，
旧 `testLocalPortraitPreviewAndTrainingCandidate` 11.181 秒。
结果包 `/private/tmp/maichat-batch174-ios-ui-20261103.xcresult`；同一批共八例，
另一个任务补充用例被首次通知预提示阻挡，不能把整批说成全部通过。

已导出并人工查看原尺寸状态与缩略图、全屏结果、回到消息的画面。使用的是公开人脸
置于 5712×4284 灰色画布角落的合成素材，不是用户原图。附件 `portrait-large-photo-qa`
实际记录：face_count=1、person_count=null；PNG 和 L8 mask 均 5712×4284；
源 SHA 前后一致，背景采样块相同；whitening=0.5 的固定皮肤权重 L*
`61.123367→62.906334`、C* `42.020535→35.874227`，改变 14616 像素。
`file_not_found`、`input_not_ready`、`unknown_parameter` 三项均通过。
Mac 原生 probe 的全图 mask 外逐字节比较与这里的模拟器背景采样是不同验证，
不得互相冒充。此时真机仍 unavailable，尚未安装或验证真实 iPhone 的峰值内存。
