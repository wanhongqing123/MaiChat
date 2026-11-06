# 本地美颜、美体与个人偏好

iOS、Android 与 Qt 桌面共用本目录的普通 C++ 服务及工具定义。平台只负责照片授权、
方向与 sRGB RGBA8 解码、初始人脸定位、账号私有目录、写新 PNG 和展示；普通服务与
Agent 工具调用同一处理路径。桌面运行与 Windows 发布验证的实际边界见下方验收记录。

## 美颜参数

`mobile_beautify_image` 的 `path` 指向当前 Agent 工作区内的原图。结果返回新 PNG 路径、
`result_id`、实际参数和个人模型版本。输入图不覆盖；再次调整生成图时先追溯原图。

| 参数 | 范围 | 默认值 | 含义 |
| --- | --- | --- | --- |
| smoothing | 0–1 | 0.28 | 可靠归属的同一人物可见皮肤平滑 |
| brightening | 0–1 | 0.12 | 局部 RGB 增益，最大约 22%，不等同于美白 |
| warmth | −1–1 | 0 | 红蓝通道平衡，负值偏冷 |
| whitening | 0–1 | 0 | 皮肤范围内的四级 LUT 美白，保留背景与衣服 |
| remove_body_hair | boolean | 未传时自动 | 身体细毛、毛孔暗点净肤；false 保留，true 明确启用 |
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

当前 v8 的正式顺序是 **身体净肤／匀肤 → 可选平滑、提亮和暖度 → LUT 明度参考及中性色彩保护／暗部补偿 → 皮肤范围合成 → 可选脸部几何**。
用户确认的效果参考是 `calf-cleanskin-v3-body-scoped-comparison-zh.png`；不能将手绘局部蒙版的
原型审美确认，等同于所有真实照片及所有平台已经完成验收。

`MaiBodySkinRetouch` 提供不依赖工具协议、界面或平台的普通 C++ 接口：在可靠归属的身体皮肤
内区，方向黑帽和连通区域筛选定位细毛／小暗点，使用 OpenCV Telea 修补，再以低频亮度为
引导作有界联合双边匀肤，并保留 20% 高频细节。不能直接以原始亮度作引导，否则细毛也会被
当作需要保护的边缘。身体软边从语义类别 2 和人物归属生成，避免以黑色毛发的 RGB 再挖孔；
衣服、背景、其他人物与头发不进入该范围。脸部、眉眼、嘴唇和胡须不执行身体去毛。
这是细毛与细小暗点清理，不能承诺任意浓密毛发或纹身均可自动无痕移除。

可选 `remove_body_hair` 严格接受 boolean；false 关闭身体净肤，true 明确启用，未传时根据
`max(smoothing, whitening)` 在 0–0.2 之间连续过渡。不会在全零参数时突然开启净肤。
`body_cleanup_request_mode` 单独报告 auto／enabled／disabled，不能把字符串 auto 写入
boolean 参数。个人偏好训练仍是原八属性 72 权重，不把这个可选开关伪装成已训练属性。

`MaiPortraitWhiteningLut` 是 GPUPixel 原版四级美白 LUT 的共享 CPU 实现，固定数据与来源、
许可证见 `whitening/`；没有新增 GPU 专属业务分支或生成式美肤模型。界面 whitening 0.6
对应实际 shader 强度 0.25，与本次审阅参考一致；零附近再以连续门控渐入，避免原 shader
低强度分段的跳变。完整映射为 `shader = whitening × 5/12`，整个 LUT 效果乘以
`smoothstep(0, 0.05, whitening)` 后进入皮肤范围合成；不是只对最后一张 LUT 渐入。
`whitening_shader_strength` 与 `whitening_blend_gate` 如实返回实际值。
脸部与身体都只在各自受保护的皮肤范围内调色，不把原版全图滤镜直接套到背景。

原版 LUT 在低照度皮肤上可能额外增加红色色度。v8 保留该 LUT 作为明度参考，目标 a*/b*
沿美白阶段输入的方向，色度取输入与 LUT 目标的较小值；该输入已经包含用户显式要求的
平滑、提亮和暖度，不会回看原片颜色抵消暖度。目标超出 sRGB 时沿同一 Lab 色相降低色度，
保留目标 L*。这些是浮点目标性质；最终 RGB8 量化及软边 RGB 合成仍可能产生小幅 Lab 误差。
`whitening_color_policy=source_hue_chroma_cap` 说明实际使用了该业务颜色策略；底层原版
`MaiPortraitWhiteningLut` 数据、算法及 CPU/GPU 参考一致性测试保持独立。

暗部补偿仅依赖用户的美白强度 `s`：`D=25*s*(2-s)`，并作用于 LUT 的目标 L*：
`L'=L+D*smoothstep(3,15,L)*max(0,1-L/50)^2`。D 为 `[0,25]` 的 L* 曲线幅度参数，
由 `whitening_shadow_amount` 返回，不是每个像素实际增加的 L*。曲线在 L*≤3 和 L*≥50
处不额外补偿，保留黑白端点；固定强度下为 C1 严格递增曲线，最小导数不小于 0.3。
这不代表整个感知、LUT、合成流程对跨输入变化全局单调。不会按全图或皮肤均值改变 D，
因此亮图中的局部暗肤也可能得到补偿，不能把“亮图整体无变化”当成契约。

人物工作平面直接来自真实原图区域，最长边不超过 1024；不会将已有代理放大，也不放大
本身较小的原图。Protect 后、首 Tile 前，仅在启用身体净肤且有足够身体范围时，一次生成
共享原始场 W、未合成目标场 T 与身体软边。原生目标为 T + 0.2 × (原生原图 − W)，
恢复有限细节，再与原生原图按 A 合成一次；A 为 `clean_strength` 乘以采样身体 alpha
和最终受保护皮肤 alpha 的较小值。关闭或跳过净肤时不分配 W/T，仍准备身体软边供美白使用。
所有 Tile 采样同一原图坐标场，8 像素 halo 内不独立执行
连通区域或大半径修补，避免接缝与块间不一致。旧 v6 Lab 曝光／局部校正业务路径已删除，
不能再把其 0.65 EV、去色上限或响应公式描述为当前效果。

感知使用同一六类模型的身体皮肤（类别 2）与脸部皮肤（类别 3），再通过人物检测与人物
分割限定目标归属，不能给其他人物一起调色。原生脸部关键点保护眼、眉、唇；面部软轮廓
与身体蒙版连续混合。成熟身体内区直接使用稳定软边；其余范围上采样用原生颜色引导并
检查绝对颜色支持，防止单纯拉伸低分辨率蒙版覆盖衣服或背景。其他人物的排除区在面部混合后仍有效。

iOS 通过全图分析与重叠窗口定位小脸，并单独准备最长边不超过 2048 的原生五官保护
ROI；皮肤颜色覆盖不再被限制在人脸框。全图先由最长边 512 的代理确定人物归属；
需要细化时，核心返回原图左上角坐标的 `skin_refinement_request`，宿主直接从已校正
方向的原始图像生成该人物区域的 1024 边内代理，不放大全图旧代理。新的皮肤分析保留
自己的原图坐标域，逐块采样时不再缩回全图 512，也不重复乘旧的软人物覆盖；其他
人物的硬排除及原始五官保护仍有效。普通 C++ 完整光栅入口执行同一核心细化。
随后处理原生 1024 像素写入块及 8 像素 halo，输出同尺寸新 PNG，不要求用户裁图再贴回。
完整光栅普通核心入口及美体仍限制 1200 万像素；iOS、Android 与桌面原尺寸文件宿主暂限制 3200 万像素，
超限明确报 `output_budget_exceeded`，不隐式缩小图片。单个近距离大脸使原生 ROI 超限时
报 `roi_budget_exceeded`，此时需要明确选择更小输入。普通照片处理同进程只允许一个请求，
并发请求返回 `busy`，避免原尺寸编码工作内存叠加。

美颜额外返回同尺寸灰度 `mask_path`、原图坐标 `roi`／`processing_bbox`、实际参数、
mask 内加权 `skin_L_before/after` 和 `skin_C_before/after` 以及改变的 RGB 像素数。
这个 mask 是皮肤调色覆盖诊断；RGBA 结果已经完成软边合成，不得再乘 mask 混一次，
几何修饰也不能只靠皮肤 mask 合回原图。新皮肤流程实际执行人物分析，返回对应的
`person_count`、`person_analysis_performed`、`person_ownership_reliable`。
`skin_scope=face_skin_only` 表示无法可靠归属人体，仅处理了已保护的脸部皮肤；
`face_color_skipped=true` 表示五官保护失败、脸部未调色，仅可能保留可靠身体皮肤结果。
调用方必须说明部分覆盖，不能宣称所有可见皮肤都已处理。可靠皮肤不足时返回失败。
同一唯一人脸可能对应多个重叠的人体候选；覆盖率竞争仍拒绝身体处理，
但原生 mesh／五官保护确认有效后可返回 `face_skin_only`，并报告
`body_ownership_skip_reason=overlapping_person_seeds`。此时仅原生授权脸部 alpha 可写入，
不保留全图粗蒙版残量；多脸或无人脸的归属歧义仍失败，显式身体净肤请求也不伪装成功。
新增 `body_skin_pixel_count` 与处理版本 `native-owned-skin-tiles-20261105-v8` 便于核对；
`analysis_version` 仍表示原感知模型版本，不跟随颜色策略升号。
`clean_applied`、`clean_strength`、`clean_scope`、`clean_skip_reason` 说明身体净肤实际是否执行；
AUTO 没有可靠身体范围时说明跳过，显式 `remove_body_hair=true` 则返回
`body_skin_unavailable`，不能把成功的脸部美白描述为全身净肤。
`skin_refinement_applied`、`skin_analysis_source_rect` 及分析代理宽高说明是否执行原图
区域细化。嵌入方的次序为 Analyze → 若请求则 Refine 恰好一次 → Protect → Tile →
Finish/free；准备共享净肤场由 C ABI 在 Protect 后自动执行；省略必需细化、重复或过晚细化均失败，不返回伪造的完整处理成功。
全图阈值差异占比小也不表示皮肤没有美白，应优先看同一 mask 内的 L*/C*。

失败诊断使用 `code`、`stage`、`message`、`hint` 和实际尺寸。不存在／空／未写完文件、
定位失败、mesh 失败和皮肤分割为空分别报告，不把 I/O 失败包装成 `faces:[]`。
`action:delegate` 不属于修图参数，未知键和错误类型会被运行时拒绝。
生产文件和消费文件必须有明确先后依赖，不能将 FFmpeg 与读取其输出的工具并行提交。

`whitening=0` 完全跳过 LUT 美白；所有数值参数均为 0 且身体净肤为 AUTO 或 false 时，
输出像素与原图一致。显式 `remove_body_hair=true` 可以在零数值参数下单独执行身体净肤。
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

共享核心回归在 `tests/MaiPortraitTests.mm`、`tests/MaiPortraitSkinTests.mm`，iOS 宿主与界面回归在
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
已有真实模型回归、皮肤分块与颜色算子回归以及 `MaiPortraitRegionTests.swift`。新回归实际调用 iOS 共用的普通
`prepare/detectFaces/publish` 服务，以公开人脸图生成 5712×4284 的角落小脸构图，
验证同尺寸 image/mask、遮罩外 RGB 保持、皮肤 L*/C*、未知参数、缺失／未完成输入与取消。
另用公开全身素材验证身体皮肤覆盖和明确的衣服、天空、海水、沙地点；不能把原有灰底拼接
大图当成真实 24 MP 审美质量验证。这是 macOS Vision/CoreImage 验证，不能替代 iOS 安装
产物和真机验收。本次证据及边界见仓库 `docs/diagnostics/maichat-portrait-quality-20261104.md`。


净肤及原版 CPU LUT 参考证据见 `docs/diagnostics/maichat-body-cleanskin-cpu-reference-20261105.md`、
`docs/diagnostics/maichat-desktop-portrait-wiring-20261105.md` 与
`docs/diagnostics/maichat-portrait-quality-20261104.md`。普通完整光栅、各平台文件宿主以及
Windows 发布包是不同验收边界，不能用其中一项通过代替其余平台实际运行。
