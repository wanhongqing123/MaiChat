# 本地美颜、美体与个人偏好

iOS 和 Android 共用本目录的普通 C++ 服务。平台负责照片授权、方向与 sRGB RGBA8
解码、初始人脸定位、账号私有目录、写新 PNG 和展示；工具入口调用同一处理路径。
当前处理照片，桌面宿主接入另行完成。

## 美颜参数

`mobile_beautify_image` 的 `path` 指向当前 Agent 工作区内的原图。结果返回新 PNG 路径、
`result_id`、实际参数和个人模型版本。输入图不覆盖；再次调整生成图时先追溯原图。

| 参数 | 范围 | 默认值 | 含义 |
| --- | --- | --- | --- |
| smoothing | 0–1 | 0.28 | 选定脸部皮肤的局部平滑 |
| brightening | 0–1 | 0.12 | 局部 RGB 增益，最大约 22%，不等同于美白 |
| warmth | −1–1 | 0 | 红蓝通道平衡，负值偏冷 |
| whitening | 0–1 | 0 | 独立美白：提高皮肤明度，温和降低色度 |
| slim_face | 0–1 | 0 | 有界瘦脸 |
| enlarge_eyes | 0–1 | 0 | 有界大眼 |

美体使用 `mobile_reshape_body`，参数为 `slim_waist`、`leg_length`；只接受可可靠定位关节的
直立站姿。归一化强度不是尺寸变化的百分比。多人照片须用 `subject_index` 选择人物。

独立检验美白的调用：

```json
{
  "path": "original.jpg",
  "smoothing": 0,
  "brightening": 0,
  "warmth": 0,
  "whitening": 0.4,
  "use_personal_model": false
}
```

美白在选定脸部的皮肤遮罩内使用 float CIELAB：按遮罩强度 `w` 执行
`L' = L + 10w(1 − L/100)`、`a' = a(1 − 0.3w)`、`b' = b(1 − 0.3w)`。
相同比例收缩色度，避免用负暖度来近似美白；高亮处提升自然收敛，保留局部纹理。
眼、眉、唇排除在遮罩外，背景不参与调色。此版本是局部调色算法，不是生成式修肤网络，
也不对身体全身皮肤作美白。Lab 按 64 行分块处理以限制临时内存。

移动端美颜通过全图分析与重叠窗口定位小脸，共用核心规划最长边不超过 2048 的原生
人脸 ROI，在原图中局部处理后输出同尺寸新 PNG，不要求用户裁图再贴回。完整光栅的旧
核心入口及美体仍限制 1200 万像素；iOS 原尺寸文件解码／编码暂限制 3200 万像素，
超限明确报 `output_budget_exceeded`，不隐式缩小图片。单个近距离大脸使原生 ROI 超限时
报 `roi_budget_exceeded`，此时需要明确选择更小输入。普通照片处理同进程只允许一个请求，
并发请求返回 `busy`，避免原尺寸编码工作内存叠加。

美颜额外返回同尺寸灰度 `mask_path`、原图坐标 `roi`／`processing_bbox`、实际参数、
mask 内加权 `skin_L_before/after` 和 `skin_C_before/after` 以及改变的 RGB 像素数。
这个 mask 是皮肤调色覆盖诊断；RGBA 结果已经完成软边合成，不得再乘 mask 混一次，
几何修饰也不能只靠皮肤 mask 合回原图。美颜不执行人体检测，因此返回
`person_count:null`、`person_analysis_performed:false`；这不表示图片里没有人。
全图阈值差异占比小也不表示皮肤没有美白，应优先看同一 mask 内的 L*/C*。

失败诊断使用 `code`、`stage`、`message`、`hint` 和实际尺寸。不存在／空／未写完文件、
定位失败、mesh 失败和皮肤分割为空分别报告，不把 I/O 失败包装成 `faces:[]`。
`action:delegate` 不属于修图参数，未知键和错误类型会被运行时拒绝。
生产文件和消费文件必须有明确先后依赖，不能将 FFmpeg 与读取其输出的工具并行提交。

`whitening=0` 完全跳过新增调色；所有参数均为 0 时输出像素与原图一致。
`strength` 是旧调用的磨皮／提亮映射，不控制美白。已验证的个人模型只补充未显式指定的
参数，显式参数始终优先；做同图对照时应关闭个人模型并固定其余参数。

## 反馈与训练

`mobile_portrait_feedback` 接受同一原图两个结果的 `result_id`，只标注用户明确评价的
属性，例如 `attributes: ["whitening"]`。Agent 不可代替用户决定哪张更好。

`model_continue_training` 支持 `train`、`status`、`versions`、`rollback`。训练更新每属性
九个权重、共 72 个权重的偏好排序器，感知基础模型保持不变。按原图分组验证；每个被
评价属性至少三个独立原图并通过验证改善后才启用。样本不足保存 `candidate_only`，
不会修改当前版本；候选不能通过回滚强行启用。`rollback` 的版本 0 恢复基础参数。

账号间的反馈、输出和模型隔离。旧七属性记录及 63 权重版本可继续读取和回滚，新增美白
属性从 0 开始，不凭旧磨皮或提亮反馈推断美白偏好。当前不提供任意模型导入或相册 RAG。

## 构建与验证

先运行 `python3 MaiChat/scripts/fetch-portrait-models.py`；构建前使用 `--check` 验证固定
权重。来源和摘要见 [models/MODELS.md](models/MODELS.md)。模型二进制不进入 Git。

运行人像/美体测试前需额外准备公开站姿素材：

```bash
python3 MaiChat/scripts/fetch-portrait-models.py --test-fixtures
```

该命令将 `pose.jpg` 保存到本目录的 `tests/fixtures/`，Android 测试资源任务会将其复制为
`portrait-test-body.jpg`；测试图片不提交到 Git。普通 App 构建只需要上面的固定权重。

共享核心回归在 `tests/MaiPortraitTests.mm`，iOS 宿主与界面回归在
`ios/MaiChat/MaiChatUITests/PortraitUITests.swift`，Android JNI 与真实模型回归在
`android/MaiChat/app/src/androidTest/java/com/kongshang/maichat/PortraitInstrumentedTest.java`
（后两路径相对于 `MaiChat/`）。实际构建、设备和验证边界记录在仓库
`docs/diagnostics/maichat-local-retouch-integration-20261102.md`。
`MaiPortraitTests.mm` 当前由本机原生 QA runner 编译执行，尚未注册为 CMake 测试目标；
iOS UI 与 Android instrumented 测试使用各平台工程中的正式测试 target。

可复现的 macOS 普通服务回归入口为 `tests/run-native-tests.py`：

```bash
python3 MaiChat/Portrait/tests/run-native-tests.py \
  --build-cache <native-desktop-build> --opencv-config <matching-opencv-generated-headers> \
  --runtime <bundled-macos-ort-library> --output <new-temporary-directory>
```

该脚本只读取已经构建的原生 MaiAgent/OpenCV 静态库，在新的临时目录编译人像核心、
已有真实模型回归和 `MaiPortraitRegionTests.swift`。新回归实际调用 iOS 共用的普通
`prepare/detectFaces/publish` 服务，以公开人脸图生成 5712×4284 的角落小脸构图，
验证同尺寸 image/mask、遮罩外 RGB 保持、皮肤 L*/C*、未知参数、缺失／未完成输入与取消。
这是 macOS Vision/CoreImage 验证，不能替代 iOS 安装产物和真机验收。
