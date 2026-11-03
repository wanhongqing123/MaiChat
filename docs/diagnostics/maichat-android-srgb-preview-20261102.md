# Android 图片预览 sRGB（2026-11-02）

场景：本地美白移动端验收，Android 2.2.6 (243)，API 37 arm64 模拟器，
main `1997947219596e6520be27ed892bd6ea1820ff2e` 加当前工作区。

复现：公开 Lena 照片调用真实 JNI 美白后保存 PNG，在正常 MainActivity 的
showFullScreenImage 中预览，截图同一画面。PNG 较自然，预览却明显变暗、饱和。
预期：前景预览的像素应接近 PNG，允许缩放插值误差，不能将线性颜色直接送显示。

证据：`/private/tmp/maichat-portrait243-android-qa-20261102/` 留存原图、处理 PNG、
实际截图及三图对比。对齐后，预览与 PNG 的 RGB 平均绝对误差为 57.29/255；
将 PNG 做 sRGB 解码再比较，误差降为 0.69/255。这不是美白处理改变了背景，
而是预览缺少输出编码。

调用链：MaiGraphicsImageView → TextureView JNI → 共享 Presenter → GLES。
GS_RGBA 纹理使用 GL_SRGB8_ALPHA8，采样后为线性值。Android EGL 窗口表面创建时
没有设置颜色空间，默认 LINEAR；因此没有与采样解码配对的 sRGB 写出。

对照已阅读的 OBS 实现：`libobs/obs-video.c::draw_mix_texture` 将
gs_effect_set_texture_srgb 与 gs_enable_framebuffer_srgb(true) 配对。
GLES 没有该桌面开关，需创建 sRGB EGL 表面，由硬件完成写出转换。
参照 [EGL_KHR_gl_colorspace 规范](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_gl_colorspace.txt)，
默认 LINEAR，GLES 的 sRGB 表面不需要额外 GL_FRAMEBUFFER_SRGB 开关。

最小失败用例已补在 PortraitInstrumentedTest，直接比较实际 TextureView 与保存的
sRGB PNG：243 的均值误差 56.79，断言失败。尝试原生 sRGB EGL 表面时，模拟器返回
EGL_BAD_ATTRIBUTE，不能把该选项当作所有 Android 都支持的能力。

最终实现保留原有 EGL 创建方式；GLES 查询当前附件实际编码，LINEAR 目标由共享
Presenter 在最终 effect 做 sRGB 编码，sRGB 目标仍由硬件写出，避免重复编码。
新 effect 复用 OBS default.effect 的顶点、采样、色彩辅助函数和相同 tone mapping。
最终红绿验证结果本轮补充。iOS 和桌面的 Metal 路径不选择新增 effect。

最终 Android 2.2.6 (245) 实测：同一实际 TextureView 像素用例通过，平均通道误差
从 56.79 降为 0.665/255。原图追溯、独立美白、实际反馈训练、旧滤镜及 Graphics
直显共五项回归通过（12.338 秒）；日志
`/private/tmp/maichat-portrait245-android-tests-20261102.log`。
已自行查看保存 PNG 与屏幕截图的并排对照，亮度和色彩恢复一致。
产物 `/private/tmp/maichat-portrait245-android-qa-20261102/`，
其中 `png-vs-preview.png` 是最终对照。未在 Android 真机测试。

2026-11-03 同一 Android 245 补跑既有实际视频工具回归
`MaiFfplayPlaybackInstrumentedTest.agentOpensControlsAndClosesGraphicsVideo`，1 项通过
（8.196 秒），日志 `/private/tmp/maichat-portrait245-android-video-regression-20261103.log`。
