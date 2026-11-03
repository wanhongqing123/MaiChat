# 本地美颜、美体与个人训练接入调查（2026-11-02）

## 状态与用户操作

后续明确新增：用户实测发现只有提亮／冷暖，要求实际补独立肤色美白。当前实现确为
皮肤遮罩中的 RGB 增益和红蓝偏移，没有隐藏的降饱和能力；不得称其已有独立美白。
复现与验收：同一原图固定其余参数、关闭个人模型，分别设 whitening=0、0.3、0.6；
选定脸部皮肤应明度升高、色度温和降低，保持色相方向和皮肤细节，眼眉唇及远处背景
不改动。whitening 缺省为 0，提亮／暖度含义不变；已保存的七属性反馈和模型继续
可读，新属性可以单独评价和训练。移动端目标升级为 iOS 169 / Android 243。

最新交互修正：创作中心空态是能力展示页。iOS 167 / Android 241 新增的美颜、
美体、个人模型训练被错误做成了按钮。复现：进入空白创作会话，点击个人模型训练，
输入框会填入草稿；点击美颜／美体还会打开选图。用户要求这些项目与原有功能说明
一致，仅作静态展示。预期：三项仍可见，点击不改草稿、不打开选图、不执行工具；
现有聊天和普通代码的工具能力继续保留。此修正目标构建为 iOS 168 / Android 242。

最新执行顺序：用户要求先实际接入 iOS、Android，桌面端后续处理，并准备测试。
从本节更新起进入移动端实现；下文原有“尚未实施”描述是此前调查阶段的状态，
最终交付以新增的实现与验证记录为准。移动端首版做照片处理及真实个人偏好训练，
不扩大到视频或照片生成模型全量微调。验收仍按正常 iOS 模拟器、真机、Android 顺序。

下文保留实施前调查；实际完成的能力及验收见末尾更新。源码为 main
`1997947219596e6520be27ed892bd6ea1820ff2e` 及当前未提交工作区；保留此前媒体、
浏览器、短信等修改。未提交也未新建分支；真机更新使用用户此前持续授权。

用户确认两类个人学习场景：相册检索，以及美颜、美体。美颜需要自然肤质、保留细节、
背景清晰、减少泛白偏色、自然提亮、瘦脸和大眼；美体需要瘦身和调整腿部比例。
基础模型可以由用户提供或 Agent 获取；优先冻结基础模型，在设备上训练每用户小模型。
用户后续明确要求覆盖桌面端。因此正式接入范围是 iOS、Android，以及现有 Qt macOS
和 Windows 客户端；不能把移动端联通当作桌面也已具备相同能力。

可复现的目标操作：

1. 用户在 IM 或创作中心提供图片，要求自然美颜、局部瘦身或调整腿部比例。
2. Agent 调用本地分析和处理能力，预览新图；普通代码可以直接调用同一处理核心。
3. 用户指出“肤质可以，但太白／脸太瘦／腰太细／腿太长”等具体问题。
4. 从原图重新处理，直到用户明确认可；记录被评价的部位与属性，不推断其他属性。
5. 本地训练任务读取真实反馈，生成并验证个人模型版本；下次处理相似照片时使用。

预期：原图不被覆盖；肤质与几何形变可以分别控制；未选择的人物不被误改；背景、衣服
纹理和关节连续性受到检查。失败或取消不交付半成品，不把“模型不可用”报告成成功。
个人训练必须实际更新权重，不能把保存固定强度、搜索改词或重新执行滤镜称为训练。

## 实施前代码路径（历史快照）

| 现有模块 | 当前能力与接入意义 |
| --- | --- |
| `MaiChat/MaiChatTools/MaiImageFilter.{h,cpp}` | 普通 C ABI，RGBA 输入，新 RGBA 输出；`beautify` 是整图高斯模糊、提亮、对比度与饱和度绑定的 FFmpeg 滤镜，无可训练美颜网络 |
| `MaiChat/MaiChatTools/MaiMobileHostTools.cpp` | 注册 `mobile_beautify_image`、`mobile_transform_image`、人脸检测、人物分割与预览保存工具；美颜和 transform 的 beautify 最终复用同一滤镜 |
| `MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MobilePhotoTools.swift` | `transformImage` 校验工作区、方向与 1200 万像素上限，调用 C ABI，写新 PNG；可复用图片读写与返回路径流程 |
| `MaiChat/ios/MaiChat/MaiChat/MaiChatTools/MobileImageTools.swift` | Vision 人脸框和眼口中心、人物分割；现有输出不是稠密人脸网格、皮肤分类或人体姿态 |
| `MaiChat/android/MaiChat/app/src/main/java/MaiChatTools/com/kongshang/maichat/tools/MobileImageVision.java` | ML Kit 人脸框、眼口坐标和人物分割；同样缺少上述稠密分析结果 |
| `MaiChat/android/MaiChat/app/src/main/cpp/MaiAgentJni.cpp` | `nativeFilterBitmap` 把 RGBA 位图交给相同 C++ 滤镜；JNI 只作平台边界 |
| `MaiChat/desktop/src/MaiChatTools/MaiImageVision.{h,cpp}` | 桌面 YuNet 五点定位及 PPHumanSeg 二分类模型；人物遮罩不能当成皮肤或各身体部位遮罩 |
| `MaiAgent/AgentTools/MaiRvmMatting.cpp` | 已有 ONNX Runtime C API 的模型会话与运行库注入方式；RVM 抠像本身不提供美颜或人体关节 |
| `MaiChat/MobileAgentAdapter/CMakeLists.txt`、桌面 CMake | 已接入 OpenCV、FFmpeg；可以增加同一份可移植视觉／处理核心 |
| `MaiAgent/scripts/build-onnxruntime*.sh` 与 iOS build-settings | 当前用于本地推理，未开启端侧训练 API；不能直接假定任意模型文件可在 App 中继续训练 |
| `MaiChat/Network/MaiCurlDownload.{h,hpp,cpp}` | 三端共用 HTTPS 下载、限制、取消、临时文件及完整发布；模型获取应复用它 |

## 模型格式与候选

MediaPipe 框架具有跨平台 C++ 核心，Tasks 也提供 C++ 接口，例如
`mediapipe/tasks/cc/vision/face_landmarker/face_landmarker.h`。iOS、Android 的封装
不意味着核心必须各写一套。模型权重与 C++ 框架是两种产物，端侧推理与继续训练也是
不同能力，不能因框架是 C++ 就认为模型文件自动支持动态训练。

MediaPipe 官方 `.task`／`.tflite` 文件不是 ONNX，不能改扩展名后交给当前运行库。
优先验证来源可追溯的 ONNX 转换，保留原始权重、转换工具版本、输入输出契约、校验值和
模型许可。若关键模型转换无法保证结果一致，再评估增加共用 LiteRT 后端；不在两端
分别维护不同的主要处理算法，也不一次性同时引入两套推理后端。

| 分工 | 候选 | 需要验证的边界 |
| --- | --- | --- |
| 稠密人脸定位 | MediaPipe Face Mesh／Face Landmarker 的 ONNX 转换 | 468／478 点版本不能混用；旋转对齐、裁剪、关键点索引、置信度和坐标回映射须完整实现 |
| 皮肤、头发、衣服与背景分类 | MediaPipe Selfie Multiclass 的 ONNX 转换 | 六分类、256×256，输出 logits 需 softmax；低分辨率和自拍场景限制不能忽略，不能承诺单靠它保护所有细节 |
| 人体定位与姿态 | OpenCV Zoo MP-PersonDet + MP-Pose | 检测与姿态是两个模型；姿态输出包括人体关键点和人物遮罩，仍不等于精确衣物／部位轮廓 |
| 神经网络皮肤修复 | ModelScope skin retouching 链路 | 官方实现组合多个 PyTorch／TensorFlow 模型，不能视作已经有可直接打包的手机小模型；先离线比较质感，再决定是否移植／裁剪 |

本轮探针只检验候选能否被仓库打包的 macOS ONNX Runtime 创建会话、执行推理，以及
输入输出形状、文件体积和冷／热运行耗时。它不验证美颜效果、检测精度、转换等价性、
手机速度或端侧训练；不能据此宣布模型已选型或产品已接入。

作为首个候选的原因是人脸、人体和皮肤等感知任务比较齐全，存在面向端侧的模型规格，
且已有可追溯的 ONNX 转换可验证能否复用当前运行库。它不直接生成自然美颜／美体
效果，也不是此次选型中的已证明优胜者。与继续扩展现有 Vision／ML Kit 的方案相比，
共用相同权重和预后处理有利于跨端一致性，但增加模型导入、转换对齐及打包维护成本。
模型效果、低置信度场景与手机资源开销决定最终选型，不能按框架知名度代替比较。

## 建议的正式接口与唯一实现路径

新增可移植 C++ 能力，建议按职责放在 `MaiChat/Portrait/` 与
`MaiChat/Personalization/`，不改变 MaiChat／MaiAgent 的兄弟布局。下列名称为设计名，
本轮不注册空工具或交付空接口：

- 分析会话：读取固定版本的模型包，对图像输出稠密人脸、人体姿态及分类遮罩。
- 图像处理核心：接收类型明确的分析结果与独立参数，执行局部肤质、肤色和网格形变。
- 个人模型：用图像特征、处理属性及用户反馈预测参数／评价候选；与处理算法分离。
- 普通 C++ API → 薄 C ABI → Swift／JNI／Qt；Agent 工具包装相同普通 API。

模型会话应缓存而非每次调用重新加载。解码、方向修正、缩放与坐标回映射统一；颜色空间
显式约定，避免 HDR、广色域输入在 RGBA8 边界被错误解释。分析可缩小，正式输出保持
原始分辨率；内存峰值和推理队列有界。按基础模型与个人模型版本形成可复现处理记录。

处理参数独立区分皮肤平滑、细节保留、提亮、色调、瘦脸、眼部、腰部、身体宽度和腿部
比例。具体范围由效果验证确定，不预先把所有参数定义成通用百分比。

肤质处理保护眼、眉、唇、头发等区域；肤色处理只作用于可信皮肤区域。瘦脸、大眼、
美体使用局部逆向网格映射并检查折叠、越界和关节连续性。形变会暴露新背景时，需要
填补或限制形变量；不能承诺仅凭人物遮罩就让背景完全不变。多人、遮挡、侧身、坐姿、
裁断的肢体需要独立验收，置信度不足的部位不套用默认人体比例。

`mobile_beautify_image` 扩展为该正式人像处理核心的入口，保留明确的旧参数映射，
`mobile_transform_image` 中 beautify 也转到同一核心，不长期保留旧整图模糊业务路径。
美体建议单独提供 `mobile_reshape_body`，避免将面部与身体参数混在一份庞大 schema 中。
分析、预览、保存继续复用现有能力；非模型调用方不经过工具协议。

## 本地训练与模型管理

第一版训练目标是个人参数选择／结果偏好，不反向传播整个视觉基础网络。可以使用很小
的独立 C++ 排序模型与明确的梯度更新；它不要求把所有感知模型重建为 ORT 训练图。
若采用通用 ONNX 训练包，则需额外生成 training／eval／optimizer 图、checkpoint，
并打包启用训练 API 的运行库；普通推理 ONNX 文件不自动满足这些条件。

训练数据包含原始任务、输入特征及模型版本、候选参数、用户明确评价的属性、正负结果
与反馈来源。评价“腰太细”不能推成腿部或肤色也错误；未评价项保持未知。按原图／
搜索任务分组划分训练与验证，避免同图多个版本分别落入两组产生虚假提升。

训练根据认可与否定方案的得分差计算损失，加入历史反馈与正则约束，限制迭代与资源。
缺反馈、样本不足、未改善、取消和训练失败要有真实状态；验证不充分时不自动替换正式
个人模型。反馈可以立即影响当前结果，但不能用同一训练样本证明泛化效果。

个人模型、反馈和任务以宿主认证的当前账号隔离，不能让模型参数中的 user_id 指定
其他用户。支持版本查询、取消、回滚与清除；账号切换后不得发布旧账号迟到的训练结果。
模型按基础模型摘要、特征维度及 schema 版本校验。模型更新造成特征空间变化时，相册
索引和个人模型须重新构建或通过验证迁移，不能直接混用。

模型包清单固定来源、摘要、尺寸、输入输出、预处理、许可及任务类型。用户附件或
Agent 下载进入同一导入与兼容性检查流程；不加载其中脚本、动态库或自定义执行代码。
模型文件下载可联网，用户照片与训练反馈默认留在设备；本方案不增加模型代理服务。

相册检索的个人排序与美颜／美体偏好可共用任务、存储、版本和训练管理，但分别配置
特征与目标。排序层未见到的照片不能靠训练补出：相册权限、索引、候选检索先完成。

## App 接入清单：移动端与桌面端

本节按当前调用链列出待实现模块；名称和 schema 是接入契约草案，尚未添加到产品。

### 共用构建目标与普通 API

建议增加 `maichat_portrait` 与 `maichat_personalization` 两个 CMake 目标，由
`MaiChat/MobileAgentAdapter/CMakeLists.txt` 和 `MaiChat/desktop/CMakeLists.txt`
共同链接，源文件只保留一份。C++ 公开头使用类型明确的结构体，不暴露 Qt、Swift、
JNI、HTTP 或模型工具 JSON；C ABI 仅承担内存所有权和语言边界转换。

| 拟新增模块 | 普通代码职责 | Agent 入口职责 |
| --- | --- | --- |
| `Portrait/MaiPortraitModelSession.{h,cpp}` | 缓存已校验的推理会话，执行人脸／人体／分割模型与完整预后处理 | 不要求模型逐个拼接原始网络张量 |
| `Portrait/MaiPortraitProcessor.{h,cpp}` | 从原图、选中人物、明确参数与分析结果生成新像素，响应取消 | 美颜／美体工具只校验参数和组织返回结果 |
| `Portrait/MaiPortraitService.{h,cpp}` | 编排读取、分析、个人参数推荐、处理、完整文件发布，生成结果 ID | 保留移动端 `mobile_beautify_image`；增加美体业务入口，复用服务 |
| `Personalization/MaiPersonalizationStore.{h,cpp}` | 保存当前账号的反馈、个人模型版本及结果引用，保证事务和账号隔离 | 反馈工具只引用已存在且属于当前账号的结果 |
| `Personalization/MaiPersonalizationTrainer.{h,cpp}` | 训练小型偏好模型、验证、生成 checkpoint、取消和回滚 | “模型继续训练”工具启动任务并查询真实进度和验证结论 |
| `Personalization/MaiLocalModelCatalog.{h,cpp}` | 导入模型包，校验角色、摘要、张量和版本；复用共用下载器 | 用户附件和 Agent 获取的文件进入同一导入路径 |

第一版为照片处理；视频必须另验跨帧姿态、掩码、颜色和形变一致性，不能因为单帧处理
成功就将图片工具标为已支持视频美颜／美体。算法、普通 API、工具与测试分别存放。

### 各端具体适配点

| 平台 | 当前真实入口 | 待接入职责 |
| --- | --- | --- |
| iOS | `MobilePhotoTools.swift::transformImage`、`AIMobileHostToolProvider.swift`；`AIAssistantBackend.init` 已取得 `OrtGetApiBase` | 将运行库接口提供给新服务；复用方向修正、工作区图片读写与现有预览／保存；后台执行，主线程只更新结果 |
| Android | `MobilePhotoTools.java` → `AIAssistantController.transformImageForHost` → `MaiAgentJni.cpp::nativeFilterBitmap` | 加薄 JNI 服务桥接；复用 `prepareBundledModel` 的模型落盘思路并校验摘要；打包的本地 ORT 路径传给服务；Java 不重写美颜或训练算法 |
| Qt macOS | `AgentController.cpp` 建立工具注册表；`MainWindow.cpp` 用当前账号创建 controller，已有图片预览 | 在注册表中接入相同业务服务；QImage／FFmpeg 解码至明确像素格式，结果回到已有预览；模型随 App 资源打包，运行库 rpath 与签名通过正常构建处理 |
| Qt Windows | 同一 `AgentController.cpp`；已有 `DesktopVisionTools.cpp` 和内存加载视觉模型接口 | 链接相同服务，使用平台原生路径读模型；打包 ORT DLL、模型与许可证；完整 staging 及 ZIP 解压环境验证依赖 |

重要差异：当前 `MaiChat/desktop/CMakeLists.txt` 中 `maichat_image_filter` 的创建位于
Windows 条件块；`MAICHAT_DESKTOP_VISION` 及 `DesktopVisionTools.cpp` 的编译注册也有
`WIN32` 条件。因此“仓库存在 C++ 滤镜／视觉代码”不等于 macOS 桌面已经联通该能力。
正式实现应把新的共用目标在两个桌面平台正确链接、打包并注册，不能只给移动端加符号。

桌面工具不强制照搬移动端的名字前缀，但一个平台只暴露一个美颜业务入口，schema、
处理器和结果结构共用。旧记录需要识别历史名称时只作名称映射，不保留第二套处理算法。

### 用户在现有 App 中如何使用

1. 在现有 IM 或创作中心选择照片，直接说美颜／美体要求；无需先打开模型设置页面。
2. 后台分析并生成新图，沿用现有媒体预览。结果携带 `result_id`、原图引用、实际执行
   的参数、基础模型与个人模型版本。再次调整始终回到该原图，不对上次结果叠加处理。
3. 用户自然语言评价上一版；Agent 提交 `result_id`、被评价属性和明确反馈。服务核对
   账号与上下文，未知属性不补标签，多个结果指代不明时先澄清。
4. Agent 调用训练工具，创建本地后台任务；现有工具任务展示进度、成功、无改善、取消
   或失败。训练不阻塞聊天输入；退出预览不等于丢弃用户已经提交的反馈。
5. 后续美颜读取经过验证的当前个人版本；普通代码可直接调用同一服务完成相同操作。
   个人模型管理只需查看版本、回退、清除及导入兼容包，不把训练超参数堆进修图流程。

### 任务与反馈契约草案

美颜／美体结果至少返回结果 ID、原图引用、输出路径、实际参数、处理人物范围及模型
版本。只对完整文件发布成功返回结果；取消或失败保留原图，不把半成品纳入训练。

反馈条目引用服务生成的结果 ID，并记录 `attribute`（肤质、提亮、脸部、眼部、腰部、
腿部等）、用户明确认可／否定或“更强／更弱”的方向及来源消息。账号 ID 由宿主会话
注入，模型不能自行指定其他账号。按相同原图与属性组织比较样本；“腿可以、腰太细”
形成分别有依据的记录，不合成整张照片完全认可或完全否定。

训练工具草案动作是 `start`、`status`、`cancel`、`versions`、`rollback`，任务类型先
包含美颜／美体偏好，再扩展相册排序。`start` 读取已保存且兼容的反馈，不接受一段
任意训练脚本。后台状态区分排队、训练、验证、成功、未改善、数据不足、取消和失败。
成功才返回新权重版本与验证结果；同一原图的多个候选不能充作独立验证集。

基础模型按版本缓存，个人模型和反馈按账号、设备本地保存。跨设备使用可以通过
兼容性校验后的导入／导出实现；本阶段不假定手机训练后会自动同步到所有桌面，也不
新增未经约定的照片或训练数据上传。基础模型升级时重新核对个人模型特征契约。

### 第一版实施与交付边界

接入按四个能分别验收的工作包推进，但最终范围覆盖全部指定平台和美颜／美体要求：

1. 普通 C++ 模型服务及真实照片分析：包括检测、裁剪对齐、mask 后处理、坐标回映射，
   在 iPhone、Android、macOS、Windows 的实际运行库上记录耗时和内存。
2. 局部修肤、独立肤色控制、面部与身体形变：用同图前后对比检查皮肤细节、色偏、
   背景线条、关节和多人选择；原图与零参数输出作为基准，不能以随机张量代替。
3. 反馈、本地真实训练与版本回滚：证明权重变化和独立样本上的效果；缺样本或失败时
   显示真实状态，不临时注册一个总返回“训练成功”的工具。
4. 四个平台的宿主接入和产品操作回归：iOS 模拟器 → iPhone 真机 → Android 正常 App
   → Qt macOS → Qt Windows。没有对应实机／Windows 环境的项目必须标为未验证。

本轮只完成代码路径核对、方案记录及上一节所述 macOS 独立模型兼容性验证，尚未
实施这些工作包。没有新的 App 构建、安装或美颜／训练工具上线。

## 实施顺序与验收

1. 模型兼容性验证：固定候选和完整预处理，跑真实照片的定位／遮罩可视化，确认转换
   等价性、体积、内存、iPhone 与 Android 的耗时，再确定首批模型包。
2. 普通处理能力：先局部肤质与肤色，再面部和身体形变；相同核心同时供工具与普通调用。
   每项用原图、参数和输出形成可重复对比，不以“能返回图片”代替效果验收。
3. 接入自然语言反馈和本地训练：先做美颜／美体任务的真实反馈闭环，再复用管理层接入
   相册语义检索及个人排序。训练任务返回权重版本、样本数和验证结论。
4. 产品验收：按 iOS 正常 App 模拟器录屏及回看 → 真机构建、安装与操作 → Android
   正常 App 的顺序验收；包括 IM 与创作中心、横屏、多人物、失败／取消、后台恢复。
   当前尚未执行这些产品验收步骤。

## 来源

- [MediaPipe Face Landmarker](https://ai.google.dev/edge/mediapipe/solutions/vision/face_landmarker)
- [MediaPipe Image Segmenter](https://ai.google.dev/edge/mediapipe/solutions/vision/image_segmenter)
- [MediaPipe Pose Landmarker](https://ai.google.dev/edge/mediapipe/solutions/vision/pose_landmarker)
- [OpenCV Zoo MP-Pose](https://github.com/opencv/opencv_zoo/tree/main/models/pose_estimation_mediapipe)
- [OpenCV Zoo MP-PersonDet](https://github.com/opencv/opencv_zoo/tree/main/models/person_detection_mediapipe)
- [Face Mesh ONNX 实现](https://github.com/yakhyo/mediapipe-face-mesh-onnx)
- [Face Landmarker ONNX 转换记录](https://huggingface.co/naklitechie/face-landmarks-onnx)
- [Selfie Multiclass ONNX 转换记录](https://huggingface.co/senty-au/selfie_multiclass_256x256-ONNX)
- [ModelScope 皮肤修复源码](https://github.com/modelscope/modelscope/blob/master/modelscope/pipelines/cv/skin_retouching_pipeline.py)
- [ONNX Runtime 端侧训练](https://onnxruntime.ai/docs/get-started/training-on-device.html)

## 独立探针结果

以下为 20:33 调查阶段的快照；其中“未接入／未安装”不是本轮最终状态。

2026-11-02 20:33 CST，在本机 macOS arm64 独立进程执行，使用仓库已构建的
`MaiAgent/build/vendor/onnxruntime-macos/libonnxruntime.1.26.0.dylib`，CPU provider，
intra-op 2 线程、inter-op 1 线程。四个模型均成功创建会话并运行六次全零 float32 张量，
首次输出全部为有限值。热运行取后五次中位数；以下只是合成输入的兼容性观测，
不是实际处理全分辨率照片的耗时，不作手机性能或美颜效果承诺。

| 候选 | 文件字节数 | 会话加载 ms | 合成输入热运行中位数 ms | 输出观察 |
| --- | ---: | ---: | ---: | --- |
| face_landmarks | 4,920,995 | 12.56 | 2.75 | `[1,1,1,1434]` 可对应 478×3 坐标，另有存在性等输出 |
| selfie_multiclass | 16,454,560 | 12.82 | 54.69 | `[1,256,256,6]` 六分类 logits |
| pose_estimation | 5,557,238 | 8.54 | 8.92 | `[1,195]` 及 mask、heatmap、world 等输出；不能把 195 个数直接当作 33 点坐标，须依原后处理解释辅助点与属性 |
| person_detection | 11,990,159 | 9.44 | 12.58 | `[1,2254,1]` 与 `[1,2254,12]`，需要锚点解码、阈值及 NMS |

四个候选权重合计 38,922,952 字节（约 37.12 MiB），不包含人脸检测、推理运行库、
神经网络修肤模型或个人训练数据。探针直接调用 `GetTrainingApi(ORT_API_VERSION)`
返回空，确认本次加载的 ORT 1.26.0 运行库不具备训练 API。
该运行库 SHA-256 为 `06b0e29c71927e0408a1caf98c67eeb79de62fa998d9f06bf1a722b056a3049f`；
探针源码、结果摘要同时保存在临时目录的 manifest 中。

固定的权重与来源：

| 文件 | 仓库修订 | SHA-256 |
| --- | --- | --- |
| face_landmarks.onnx | `naklitechie/face-landmarks-onnx@575c33816c840c5b156398adb485e4f8d138adc2` | `f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c` |
| selfie_multiclass.onnx | `senty-au/selfie_multiclass_256x256-ONNX@6db8421a7150ac20558f2c24675078eb3a1a04d0` | `35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0` |
| pose_estimation.onnx | `opencv/opencv_zoo@48b10fa505aeb51fbd127fc60a5c660040e823b7` | `9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f` |
| person_detection.onnx | `opencv/opencv_zoo@48b10fa505aeb51fbd127fc60a5c660040e823b7` | `47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f` |

产物目录 `/private/tmp/maichat-local-retouch-probe-20261102/`：

- `fetch_candidates.py`：固定修订与摘要，每文件限制 64 MiB，只下载模型数据。
- `model_manifest.json`：记录系统、源码提交、来源、修订、字节数与摘要。
- `MaiPortraitModelProbe.cpp`、`MaiPortraitModelProbe`：独立 C++17 探针及本机可执行文件。
- `probe_result.json`、`probe_stderr.log`：完整输入输出、运行库及耗时，stderr 为空。

从仓库根目录可重复运行：

```bash
python3 /private/tmp/maichat-local-retouch-probe-20261102/fetch_candidates.py
clang++ -std=c++17 -O2 \
  -I MaiAgent/third_party/onnxruntime/include/onnxruntime/core/session \
  -I MaiAgent/third_party/nlohmann \
  /private/tmp/maichat-local-retouch-probe-20261102/MaiPortraitModelProbe.cpp \
  -o /private/tmp/maichat-local-retouch-probe-20261102/MaiPortraitModelProbe
/private/tmp/maichat-local-retouch-probe-20261102/MaiPortraitModelProbe \
  "$PWD/MaiAgent/build/vendor/onnxruntime-macos/libonnxruntime.1.26.0.dylib" \
  /private/tmp/maichat-local-retouch-probe-20261102/face_landmarks.onnx \
  /private/tmp/maichat-local-retouch-probe-20261102/selfie_multiclass.onnx \
  /private/tmp/maichat-local-retouch-probe-20261102/pose_estimation.onnx \
  /private/tmp/maichat-local-retouch-probe-20261102/person_detection.onnx
```

所有权重和探针产物均在临时目录；仓库只新增本调查文档，未把这些模型加入产品、未新增
工具、未训练个人模型、未替换或重新安装 App。后续仍需真实图像预后处理对齐、转换
等价性、检测精度、效果、内存峰值和目标手机验证。

## 移动端实际实现与验收更新

以下为上述历史调查之后完成的实现。共用普通 C++ 核心位于 `MaiChat/Portrait/`，
包含四模型推理、局部修肤、肤色调整、面部／身体形变和账号私有 SQLite 偏好学习。
Swift、JNI 仅适配解码、人脸初始定位、原图追溯、后台调用和新 PNG 提交。
构建脚本校验并打包固定权重及许可，工具和普通调用复用同一核心。

工具注册：更新 `mobile_beautify_image`，新增 `mobile_reshape_body`、
`mobile_portrait_feedback`、`model_continue_training`。原有 transform 的 beautify
也走新核心。几何参数默认关闭，原图不覆盖；多目标须显式选择，无法定位时失败。
完整参数、普通接口和调用样例见 `MaiChat/Portrait/README.md`。

### 独立美白

用户后续要求已实际补充 `whitening`，范围 0–1，默认 0。六分类分割及稠密人脸
关键点确定选定脸部皮肤，排除眼、眉、唇；美白本身使用 float Lab 局部调色，
不是生成式修肤模型。`w = whitening × mask`：

```text
L' = L + 10w(1 − L/100)
a' = a(1 − 0.3w)
b' = b(1 − 0.3w)
```

亮部提升收敛、色度最多降低 30%，不通过负暖度偏蓝来近似；不增加磨皮操作。
按 64 行分块处理以限制临时内存，零美白跳过该分支。仍只作用于选定脸部皮肤，
不是全身美白。分析版本为 `mediapipe-onnx-20261102-v2`。

公开 Lena 样图：其余参数全部为 0、个人模型关闭，同一组彩色受影响皮肤像素的测量：

| whitening | Lab L* | Lab C* | HSV S（0–100） |
| --- | ---: | ---: | ---: |
| 0 | 59.2825 | 41.9740 | 49.3059 |
| 0.3 | 60.3026 | 38.3727 | 45.7086 |
| 0.6 | 61.4310 | 34.8153 | 42.1803 |

平均 Lab 色相转角约 0.333°；远处背景字节不变。开发者已查看三图对照及实际 iOS
预览。以上不是用户此前测量的那张原图，也不代表所有光线、肤色或主观效果已验收。
调色公式和强度映射为本项目实现，不宣称等同 Photoshop 内部算法或达到其完整修肤能力。
Adobe 官方说明支持皮肤蒙版和独立局部曝光／曲线／饱和度调整：
[Camera Raw 蒙版](https://helpx.adobe.com/camera-raw/desktop/edit-and-enhance-images/masking-and-local-adjustments/masking.html)，
[肤色调整](https://www.adobe.com/products/photoshop/fix-skin-tone.html)。

### 真实个人学习与兼容

八属性各九个权重，共 72 个双精度权重，使用成对排序损失进行实际梯度更新。
用户必须明确评价同一原图的两个结果，只记录被评价属性；不以 Agent 自己判断充当
用户标签。去重、账号隔离、原图隔离、冲突评价检查，训练读取至多 512 条记录。
每个被评价属性至少三个独立原图且分组验证改善才启用；否则保存真实候选，当前版本
不变。回滚不能启用未验证候选；版本 0 回基础参数。

美白追加为第八项，旧七参数记录补零、旧 63 权重版本补关闭的美白先验，保留旧评价
索引与已学参数。真实旧格式 SQLite 回归覆盖继续训练及回滚，没有删除用户历史数据。
美白偏好可独立标注和训练，基础感知网络未改权重。

### 已执行的定位与验证

- 共享核心真实模型／图像回归通过：零参数恒等、局部肤质和背景保护、人脸形变、
  无脸失败、人体定位和塑形、独立美白明度／色度／色相、真实学习、分组验证、
  账号与原图隔离、持久化、候选保护、旧版本加载及回滚。
  日志 `/private/tmp/maichat-portrait-whitening-final-native-20261102.log`；
  对照图 `/private/tmp/maichat-portrait-whitening-comparison-20261102.png`。
- iOS 模拟器最初 Vision 无法创建推理上下文，定位后仅模拟器使用 CPU 和明确的
  landmark revision 2。真机仍使用系统默认 Vision；未用假人脸提示代替此回归。
- Android 首次回归发现 `/data/data` 与 `/data/user/0` 别名导致生成图不能追溯原图，
  修为共用核心按真实路径保存和查询，使用同一用例回归通过。
- iOS 169 正常 Root 及生产组件中调用真实宿主工具，生成独立美白 PNG，从消息缩略图
  打开全屏、单击返回，记录明确美白反馈并实际训练，单图显示候选而不启用。
  同时点击三个静态功能说明，确认不填草稿、不弹键盘、不打开选图。两项 UI 回归通过，
  录屏和截图已回看：`/private/tmp/maichat-portrait169-ui-20261102.xcresult`、
  `/private/tmp/maichat-portrait169-ui-20261102.mp4`、对应 `ui-shots` 与 contacts 图。
  本地测试账号、公开素材，不请求付费模型。
- iPhone 14 Pro Max / iOS 18.7 已安装并启动 2.2.4 (169)，安装 UUID
  `6192E45E-0D6E-4939-9FCB-F4246967F6FA`，核对 PID 20904 和相同安装目录中的可执行文件。
  `codesign --verify --deep --strict` 通过，安装、apps 与 processes JSON 存于
  `/private/tmp/maichat-portrait169-device-*-20261102.json`。
  真机照片效果与用户操作仍待实际反馈，不以模拟器通过冒充实机验收。
- Android 2.2.6 (243) 在 API 37 arm64、16 KiB page 模拟器安装运行。实际 JNI 的美白
  降饱和、平台 Lab L* 提升、原图追溯、反馈、候选训练和美体通过，旧滤镜两项回归
  同时通过。首次测试错误地使用 HSV V（最大 RGB 通道）作为明度，已改用独立的
  Android ColorSpace Lab L*，没有为测试放宽或改动美白算法。
  正常 MainActivity 登录页、正常全屏图像组件和 Debug 宿主中的实际创作页均已查看；
  创作能力说明是不可点击 TextView。没有登录 Android 真实账号或触发付费模型。
- Android 实际预览验收又发现输出 PNG 正常但显示明显变暗，已用截图像素定位到 EGL
  线性窗口缺失 sRGB 输出编码。详见 `maichat-android-srgb-preview-20261102.md`；
  最终 2.2.6 (245) 使用目标编码检测和最终输出 effect，预览像素误差从 56.79 降为
  0.665/255；五项回归通过，PNG 与截图对照已回看。243 仅为此前阶段结果。

2026-11-03 更新：iOS 2.2.4 (170) 包含以上美白和静态能力说明，并加入视频控制条
自动隐藏。模拟器再次通过两个 Portrait UI 用例（实际宿主 PNG、反馈训练和静态卡片），
结果 `/private/tmp/maichat-controls170-ui-20261103.xcresult`，录屏与截图已回看。
iPhone 14 Pro Max / iOS 18.7 已安装并启动 170，安装 UUID
`58CA16CC-2DFA-4E61-926B-775B0C6CFDD3`，启动和 processes 均核对 PID 21271；
`/private/tmp/maichat-controls170-device-{install,launch,apps,processes}-20261103.json`。
这是安装与启动验证，不替代真机上的照片质量和手势体验验收。

边界：桌面美颜／美体宿主按用户后续顺序处理；桌面 Agent 播放器独立窗口另有修复
和验收记录。没有 Windows 或 Android 真机验收；多人、遮挡、复杂光线、极端参数、
全分辨率性能和用户审美尚未全面覆盖。未实现神经网络修肤基座微调、任意模型导入、
相册 RAG 或视频美颜，不以偏好学习代替这些能力。
