# Android 原尺寸人脸 ROI、检测生命周期和回归（2026-11-03）

## 操作和交付边界

基线提交 `9959488832f2d6f1ab8831fd6b0029f68180ac01` 加本轮未提交修改。
输入公开 OpenCV Lena 与合成背景的 5712×4284 JPEG，分别标记 EXIF 1、6；
普通 `AIAssistantController.transformImageForHost` 和美颜工具复用同一普通服务。
预期原文件 SHA 不变，输出保持 EXIF 转正后的尺寸，选中脸局部修肤、背景不重采样，
完成临时编码和检查后原子发布、登记原图追溯；不需要用户手动裁图或贴回。

Android 输出原尺寸 PNG 及原图坐标 ROI、皮肤覆盖和 L*/C* 指标，**不导出 mask_path**。
共享核心返回的诊断 mask 在 JNI 释放；RGBA patch 已完成混合，直接替换源 Bitmap 的
ROI，不能再乘一次 mask。iOS 的完整 L8 mask 文件不能算作 Android 已提供的能力。

所有本机素材均为公开 Lena 和合成画布，没有使用用户私人原图、真实账号或网络模型。
用户仅报告原尺寸；本机同尺寸测试不等于其原照、ICC 或具体设备的验收。

## Android 数据流

- `MaiPortraitBitmap` 先读取原始尺寸、EXIF；软件解码归一 sRGB。检测使用最长边
  2048 的全图代理，加逐个 2048 原生重叠窗口，最多 24 窗口。
- `MobileImageVision` 把框、眼睛及姿态回到转正后的原图左上坐标，共用 C++ NMS。
  保留原有 `yaw_degrees`、`roll_degrees`、`coordinate_origin:top_left` 输出。
- 全局代理未支持的窗口候选还需一次以自身为中心、2.5 倍面框边长的局部复核，
  最长边仍不超过 2048、最多 32 候选。必须重检到重叠的同一面框，不能用裁图内其他
  人或数组第一项代替该候选。全局已支持的脸保持不变。
- `MaiPortraitImage` 调共享 plan，使用原图整体代理生成原个人模型的八维特征。
  完整原图仅保留一张可变 Bitmap；EXIF 旋转按有界原始条带写入转正目标，
  不建立第二张完整 Bitmap 或完整 Java RGBA 数组。指纹逐行计算。
- JNI 锁定这张 Bitmap，只把原生不超过 2048² 的 ROI 和真实 stride 交共享核心。
  半透明 ROI 先去预乘，回写时重新预乘；不改变像素格式标志来重新解释内存。
- PNG 直接压缩到同目录唯一临时文件，关闭、同步并检查尺寸/字节后，经共享发布 ABI
  原子发布，随后登记结果。账号/工作区失效阻止发布或登记；失败清理本请求文件。
  单进程 admission 持续到编码及登记完成，避免并发完整图片峰值。

原尺寸 I/O 硬上限 32 MP；还按设备实际可用 Java 堆核算完整图片、条带及 48 MiB
预留。若暂存检测转换缓冲令预检不足，先给 VM 一次回收机会再按相同预算重算；
不足继续拒绝，不升 heap、不降低原图尺寸。32 MP 是上限，不保证每台设备都能处理。
旧美体、普通完整光栅转换仍保留原 12 MP 门禁，不宣称本轮覆盖全身或所有图片操作。

## 两个失败反事实及定位

初版每张照片创建并 close 一个 ML Kit client，第一张 24 MP 可完成，但连续调用
原生分配约每轮增加 229 MB。仅等待 SDK 的 `closeWithTask` 不能消除增长。
最小分离 probe 表明：

| 路径 | 实测 |
| --- | --- |
| 同一组 2048 窗口只解码、旋转、释放，不推理 | 连续三轮原生分配约 5.97→6.05→6.13 MB，PSS 约 48 MB |
| 每张新 detector、等待真实 close | 三轮约 235.94→465.49→695.07 MB |
| 同一 detector 连续 13 次，同 Bitmap、直接 NV21、byte[] NV21 三组 | 三组均约 249.95→249.97 MB，格式切换不能修复 |
| 共享同一 detector 跨照片 | 三轮 235.932→235.925→235.925 MB，PSS 约 295 MB |

SDK 字节码确认相同选项已命中同一个缓存 MLTask；最后 client close 会 unpin/release，
下一张再 load。产品保留一个进程级匿名模型 client，输入 Bitmap 仍逐帧释放，不保存
图片、账号或个人偏好。推理超时/异常不能立即 recycle 尚被 native 使用的输入：
关闭真实任务队列，保留输入直到 release 成功；不能确认释放时阻止该进程新批次，
提示重新打开 App，而不是继续堆积新 client。

另一个独立问题是 EXIF6 合成图的窗口边缘把 Lena 肩部检成第二张脸。
原图 display 4284×5712：真实框约 `(2136,2520,492,660)`，误框约
`(2652,3113,268,293)`，两套眼睛不同，不是应扩大 NMS 合并的重复框。
同参数中心局部复核对真实框重新检出约 `(2126,2546,478,638)`，肩部为零。
据此增加上述局部复核，不强制 `subject_index:0`，也不修改多人选择语义。

## 独立实际 Android 验证

在 `emulator-5580` 的独立包 `com.kongshang.maichat.portraitprobe` 运行；
Manifest 无网络权限，Java heap 最大值 **201326592 bytes（192 MiB）**，无 largeHeap。
没有停止、卸载或改主 App。Probe 使用正式 Java helper 的逐字节副本，以及正式 JNI
portrait 代码的精确后缀、共享 Portrait 四源和真实 ML Kit/ONNX 模型；不是算法 mock。
最终源码匹配和二进制 SHA 见 `/private/tmp/maichat-android-portrait-roi-validation-20261103.json`。

最终 instrumentation 全部通过：小图、24 MP 同源连续两轮、实际报告尺寸的 EXIF1/6；
五例均 face_count=1、原文件 SHA 不变。EXIF1 输出 5712×4284，EXIF6 输出
4284×5712。附加 8 方向逐像素/ROI 正逆映射、5 项字段/无脸/预算拒绝、并发 admission
拒绝通过；透明预乘 PNG 的零效果逐像素不变、非零效果保留全部 alpha 并改变 13664 像素。

全链连续两轮大图后的原生分配 **418337952→418338368 bytes**，差 416 bytes；
PSS **433614→433670 KiB**。后续两个实际尺寸案例为 418170448、404582992 bytes，
没有每图 229 MB 增长。单次纯 Bitmap 24 MP 方向/编码 probe 峰值约 242.8 MiB；
包含检测器、真实模型、ROI、编码的最终混合五例高水位 **651504 KiB（约 636.2 MiB）**，
不能用纯 Bitmap 数字代表完整服务，也不能宣称满足初步设想的 256 MiB。

原始证据目录 `/private/tmp/maichat-android-portrait-host-20261103`：
`final-instrumentation.log`、`candidate-recheck.log`、`detect-reused-client.log`、
`detect-only-close-task.log`、`crop-only-repeat.log`、三份 `buffer-arm-*.log`。
独立 javac 与 NDK JNI syntax 检查通过，`git diff --check` 通过。

正常 App 回归入口为
`com.kongshang.maichat.PortraitRegionInstrumentedTest`：四项测试涵盖方向、原尺寸
普通 Controller→结果追溯→再次编辑、完整编码后条带像素/Lab 对比、字段及透明边界。
大图夹具明确是 raw 5712×4284、EXIF6、公开人脸肩部误检构图，fixture 完成后释放并
detach Canvas，再走产品处理；含普通 detector 的姿态字段保留断言。
截至本记录，主任务正在构建 247，**正常安装 App 验收仍待主任务结果**；独立 probe
不能替代正常 App 全部服务并存、真机低内存/jetsam 或用户原照的验证。

## 247 正常 App 暴露的 JPEG 区域边缘差异

主任务正常安装的 247 初次四项回归中两项通过，两项失败，不能把前述独立 probe
当作这次正常 App 验收通过：

- 方向测试：EXIF2 子区域首像素 expected=-14735291、actual=-14342338。
- 完整编码比较：远离 ROI 的背景首次在 `(0,127)` 不同，正处 64 行验证条带的边缘。

随后用独立 `app_process -Xmx192m` 最小反事实，未操作主 App：相同高频 37×23
JPEG 的 raw crop `(2,4)-(34,20)`，裸 `BitmapRegionDecoder.decodeRegion` 仅右边
一列 16 个像素不同，首像素 `(33,4)` 的两值与上述断言**完全相同**；EXIF2 翻转后
恰是该区域 `(0,0)`。2049×1031 JPEG 的原生竖条 `[64,128)` 首尾两列共 2062
像素不同，原生横条没有差异。原始结果为
`/private/tmp/maichat-jpeg-region-pixel-20261103/results.json`。

这证明原失败源于 JPEG 裁剪边缘缺少色度插值的邻近样本，不是通过修改容差就能
忽略的 ROI 颜色问题。`MaiPortraitBitmap` 改为先解码 16 像素上下文并对齐 16 像素
网格，再裁取精确区域；方向条带复用同一实现，并扣减有效条带高度为上下文留空间。
完整图片仍只有一张，解码上下文加精确裁图受原有两条带工作预留约束。

修复后的精确正式 helper 在 2049×1031 高频 JPEG 上跑全部 8 种方向，每种均比较
全部约 211 万像素的条带合成与完整解码 oracle，再验证 MCU/非 MCU 区域：
**8 项完整合成与 8 项区域比较均为零像素差异**。证据
`/private/tmp/maichat-jpeg-region-pixel-20261103/fixed-results.json`。
原四项正常 App 测试断言未放宽，方向测试另加完整 JPEG oracle/条带/区域的精确比较；
主任务正在增量构建、复跑相同正常 App 用例，结果尚待记录。
