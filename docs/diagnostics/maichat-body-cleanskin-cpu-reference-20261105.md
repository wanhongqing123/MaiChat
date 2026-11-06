# 身体去毛净肤与原版 GPUPixel CPU 参考核验

时间：2026-11-05 00:14–00:19 CST。平台：macOS 27.2 arm64，Apple M3 Pro。
源码基线：`eee43402aff22caeb4b0c2688e381ced569f7e63`，`main` 上新增模块，保留现场已有未提交修改。
本次仅运行独立命令行 QA；没有构建、安装、启动、停止 App，也没有操作桌面进程或用户工作区。

## 可复现操作及预期

用户目标是身体细毛和毛囊暗点衰减、平滑匀肤，再从净肤后的新像素做美白。
原 v6 的 Lab 低频曝光/匀肤保留体表高频毛发，不能满足这个目标。

将同一原生 RGBA8 身体 ROI 和已验证的最终二值 body scope 传入普通 C++ 核心。
强度 1 应与已有 v2 去毛、v3 净肤私有原型逐像素相同；零强度应逐字节返回原图；
任何强度都不得改变 scope 外像素或 alpha。然后使用原 GPUPixel 的 shader 参数 `.25`，
对净肤新源美白，再按同一 body scope 内侧 6px 合成，与实际 GPU 参考比较。

## 核心与边界

新增 `MaiChat/Portrait/MaiBodySkinRetouch.{h,cpp}`：

- 9px 四向 black-hat，阈值 8；沿用 v2 的连通组件面积、长宽及长短轴方差判断。
- 接受细长毛与紧凑毛囊暗点；按测量宽度膨胀 1–2px 后与原 scope 相交。
- OpenCV Telea 半径 3 修复；净肤使用 masked-normalized Gaussian 低通 L 引导（σ3），
  联合滤波空间 σ3、半径 9、L 差 σ6，保留 20% 细节、scope 内侧 6px 渐变。
- 输入 scope 是宿主已经完成身体归属和保护后的最终二值范围。本模块不重新阈值、闭运算或扩张它。
  私有 v2 曾从软分析 mask 经阈值 96、7px 闭运算、9px 腐蚀生成最终 scope；本次直接复用该最终产物。
- 强度 1 保持原型效果，其他正强度仅在该固定效果和原图间连续混合；0 在颜色转换前退出。
- 组件扫描限制在带 2px 扩边的组件包围框，避免每根毛重扫全图；此次优化后重新通过像素完全一致核验。

完整场契约：同一分析 ROI 必须一次提供，使用同一原生源及最终 scope。
连通组件判断及 Telea 传播不能独立在旧 8px halo tile 内重建；低通引导与联合滤波的
18px 有限依赖也不是整个流程的 halo 保证。允许将完整结果分块写出；当前 API 不支持
独立 tile 求值。不能将 32MP 全图直接视为移动端已验证的内存方案；Lab/guide 场占用
多份浮点图，生产接入仍需完整身体 ROI 或全局修复场的内存设计与实测。

宿主须排除脸、头发、眉眼、胡须、衣服、其他人物和不可靠边缘。细纹身与细毛暗点
可能具有相似局部结构，本算法没有可靠的自动区分保证。`mask` 是已合成结果的诊断
作用域，不能再乘一次；`repairMask` 是暗细结构支持域，不是毛发语义分割。

新增 `MaiPortraitWhiteningLut.{h,cpp}` 忠实移植 GPUPixel 的 whitening-only shader，
包括 levels、gray、origin、skin、light 四 LUT 及纹理坐标和双线性采样方向。
上游固定提交 `ef552bf8ce2d0d41fa9b979bfb5c7cf79374ca88`，四 LUT 无损内嵌，
来源、生成方法和 Apache-2.0 许可位于 `Portrait/whitening/`，不依赖 Apple 或 GL。
接口接受实际 shader uniform `[0,1]`，不会应用 macOS `SetWhite` 的 `/10` API 缩放。
原 shader 的 `whiten > 0` 会先运行完整固定前置 stages，因此 `0 → tiny` 不连续；
本次有意保留以验证忠实性，尚未解决生产公共参数映射及零点连续性。

## 实际运行产物及结果

净肤与顺序合成可复查产物：`/private/tmp/maichat-body-skin-core-20261105-final/summary.json`。
该报告记录编译命令、基线提交、源码 SHA-256、实际执行二进制 SHA-256、每次参数与退出码。
二进制 SHA-256：`5b08b6ce607c1340d462a414c5bc2e652dadb4e1a9864e6de952cb12cb38d06f`。
参考现场位于 `/private/tmp/maichat-calf-hair-prototype-20261104-54w_robg/`，原图 SHA-256：
`10cc1d5b82c160991ea58e1033e1cb22aa1702c2dd68647c21c4b428ea4cb6d5`；原图 ROI 为 `(4160,3740,160,290)`。

| 核验 | 实际结果 |
| --- | --- |
| 净肤 vs v2 Telea → v3 私有原型 | RGB 最大差 0，全部通道相同，修复 mask 差 0 |
| 固定组件结果 | 75 候选，53 接受，2245 修复像素，与参考相同 |
| 作用范围 | 15479 scope 像素，14679 变化像素，范围外变化 0，alpha 变化 0 |
| 净肤新源 → CPU 原版美白 → 身体范围合成 | 对实际 GPU 最终 crop 最大 RGB 差 1 LSB，MAE 0.005316092，740 通道差 1 |
| 合成保护 | scope 外变化 0，alpha 逐字节相同 |
| 合成图与源 | 结果为新像素计算，没有拼贴旧美白输出 |
| 普通 API 合成测试 | 零强度、空 scope、行跨度、输入不变、半强度连续混合、非法值拒绝通过 |

独立美白 QA 对已有同图 GPU 产物（全部 alpha 相同）：

| 输入 | shader 强度 | RGB 最大差 | RGB MAE | CPU 时间 |
| --- | --- | --- | --- | --- |
| 1000×667 | .025 | 1 | .01060070 | .115s |
| 1000×667 | .25 | 1 | .01629685 | .103s |
| 5712×4284 | .025 | 1 | .00116544 | 3.351s |
| 5712×4284 | .25 | 1 | .00178520 | 3.158s |

另将全部 16,777,216 个 RGB8 颜色组成 4096² 色立方，经过未改的原版 GPU shader `.25`，
CPU 对照最大差 1 LSB、MAE .01563805、差值大于 1 的通道数为 0。
私有证据：`/private/tmp/maichat-whitening-parity.json`、
`/private/tmp/maichat-whitening-rgb-cube/report.json`。这仅是本机 OpenGL 与 CPU 的实测，
不自动代表所有平台 GPU 的误差界限。独立美白 QA 同时通过 `-Wall -Wextra -Werror`、
AddressSanitizer 和 UndefinedBehaviorSanitizer。净肤及顺序合成同样通过 ASan/UBSan，
并保持相同像素核验结果。四张内置 LUT 经生产解码器与原 PNG 的 RGB 字节完全一致；
独立报告位于 `/private/tmp/maichat-whitening-lut-integrity.json`。

## 重跑

无需 App、ORT 或网络：

```sh
python3 MaiChat/Portrait/tests/run-body-skin-tests.py \
  --build-cache /private/tmp/maichat-desktop-agent-window-build-20261102 \
  --opencv-config /private/tmp/maichat-desktop-agent-window-build-20261102 \
  --eigen-include MaiAgent/build/vendor/onnxruntime-ios-build/intermediates/iphoneos_arm64/Release/_deps/eigen3-src \
  --output /private/tmp/maichat-body-skin-fresh-qa

python3 MaiChat/Portrait/tests/run-whitening-tests.py
```

净肤 runner 默认重新编译仓库既有 `inpaint.cpp`；可用 `--inpaint-object` 复用同版本的既有对象。
`--compare source scope expected_clean expected_repair` 和
`--compare-whitening clean scope expected_body_whiten` 可重跑固定参考，完整实参见上述 summary。
美白 runner 的 `--baseline-root` 可读取现有 GPU 参考，`--exhaustive` 可重跑全 RGB8 色域。

未改 Engine、Bridge、个性化、工具 schema 或参数含义；这不是正式主路径接入。
未在 iOS、Android、Windows/Linux 编译或执行；未运行正常 App、安装包、UI 或设备验收。
身体全范围覆盖、多人/五官/衣服保护及完整生产路径需宿主接入后按原场景另验。
