# 2026-11-16：图片预览通用缩放与平移

用户要求 Mac 双指缩放、Windows 滚轮缩放，放大后可查看不同位置。生产变更限
ImagePreviewDialog cpp/h 和 MainWindow 的既有图片关闭动画接线，测试在原
MainWindowLayoutTest，复用同一原图与原生窗口，不创建巨幅缩放 pixmap。

支持锚点缩放、放大后拖动/像素滚动平移、双击回 fit、32 倍上限及位置钳制，保留原
fit 轻点关闭、透明图片衬底和关闭回源。精确滚动流的零像素增量不误作普通滚轮缩放。

独立 review 先发现放大裁切状态直接收缩窗口、关闭末帧跳变，以及 Cocoa 小增量截断
导致零 pixel 被误当 zoom。Mac R2 同例实际 RED4 → GREEN15，但后继独立复审继续发现
整数 QRect valueChanged 漏 tick：恒定起止矩形全程不更新，向左上收缩也可能提前到整数
终点，32 倍缩放无法连续收敛。

Windows 新 case 最初停在 150% DPI 的 native-frame 终点前提不匹配，明确计为 fixture
前提失败，未当产品 RED。按真实生产终点修正后，两行实际产品 **RED2 → GREEN**。
R3 从同一动画的每 tick 真实进度驱动图片变换，保留原 geometry 动画曲线与同窗生命周期，
不依赖整数值恰好变化，也不在 finished 时用瞬时 reset 遮盖缺口。

Windows 普通目标全影响 **17/0/0**，覆盖恒定矩形、左上收缩+zoom32、精确零像素、
Esc/Enter/close、滚轮/NativeGesture、DPR/原图、透明、同窗与背景。Mac 仅消费 R2→R3
精确增量一次，四源 blob 全等，普通 Cocoa 消费者构建 exit 0，同组 **17/0/0**。

Windows 证据：`.cache/desktop-input-primitives-20261116/image-close-r2-product-red.txt`、
`image-close-r3-green.txt`、`image-close-r3-evidence/`。四源及两种补丁入口的摘要见
`image-preview-r3-windows-receipt.json`；R1/R2 为历史候选，不叠加消费。
验证包括真实 widget 绘制和已查看截图，不宣称硬件触控板事件注入或 compositor 连续性验收。

iOS 另外的授权变更仅为 ChatView.swift 的 navProfile 从灰蓝改为既有 blue。Mac 普通
私有 bundle 模拟器构建/视觉通过，用户已接受截图，源码消费 blob 为 `5c256b4a`。
正常 deviceRelease 213 构建/签名通过，用户已授权余项完成后的保数据真机安装，由 Mac
协作者执行；本机未安装，不将该许可外推到 Windows 更新、Mac TCC 或 Sync 部署。

后继用户明确撤销等待 PC 的条件，要求先安装 iOS 回归。协作者报告 213 保数据覆盖
exit 0（61.485秒），设备 metadata 为2.2.4/213，正常启动 PID66929；随后用户又要求
底部各入口统一选中颜色，214 由 Mac 按同一明确安装许可独立推进。本机未执行这些设备操作。
