# 桌面图片预览缩放与平移

> 本文保留 Mac 侧当时的复现、候选和验收记录；其中“等待/未部署”等状态以各小节的时间和证据范围为准。后续共同结论见 [2026-11-16-image-preview-zoom-pan.md](2026-11-16-image-preview-zoom-pan.md)。


2026-11-16。用户要求 Mac 两指张开放大，Windows 使用鼠标操作放大，并能查看放大图各处。普通 IM、创作中心和工具打开图片复用 ImagePreviewDialog，没有创建另一套预览。

## 行为

原生 ZoomNativeGesture 围绕手势位置缩放；普通角度滚轮围绕指针缩放；精确触控板滚动平移。放大后左键拖动平移，双击回到适应窗口，Esc/Enter/窗口关闭保留原关闭入口；适应窗口时保留原点击关闭。

同一画布保留原始 QPixmap，通过 QPainter 绘制当前视口，不随每次缩放分配巨大新位图。缩放限制为 fit 到 32 倍；平移不露出图外空白。原图、透明衬底、原 native window 身份不变。

## 验证与独立审阅

旧生产源配新手势用例：2 pass / 3 fail。R1 普通 Cocoa 目标 build 0，受影响组 11/0/0；增加 DPR1 滚轮、DPR2 原生手势彩色象限后 5/0/0。实际 fit/zoom/pan/reset widget 帧已亲看。

独立 Windows 审阅指出两个缺口：放大状态直接关闭时仍裁切，末尾切回完整缩略图；精确滚动零 pixel 但非零 angle 被误判缩放。同例实际 R1 2 pass / 4 fail：三种关闭的归一化象限边界始终约 0.774，未回到 0.5；零像素滚动破坏 fit 点击关闭。

R2 使用几何动画 valueChanged 同步图像变换，15/0/0；三种关闭 28–30 个实际 widget 帧约 0.774→0.501。随后独立审阅发现整数几何 plateau 会漏进度：Root 接管四文件 R3，采用连续 qreal 时钟同时驱动几何与内容，同窗生命周期不变。本侧冻结 R2 后没有并写。

R3 只对精确 R2 应用一次 13628 B delta，SHA256 `ea243e40a025b2e2f0e50aac15c0f6ea7784eba3820fb983f7c1067a413bbeb5`。四个规范化 Git blob 核等：cpp db781785、h 98dd144b、Main 2fc63ca2、test c646dadc。Windows Root 同例 RED2→GREEN17/0/0；本侧正常 Mac consumer build 0，实际 Cocoa widget 受影响组也 17/0/0，包含恒定几何和向左上收缩、最大缩放、透明、同窗、精确滚动和回源。

产物与日志：`/private/tmp/maichat-image-preview-zoom-20261116-z2_z7e9_`。最终源由 Root c1142d80 及后继 main 整合；没有替换用户安装或发布新 release。

验证边界：本侧在真实 Cocoa window 中发送 Qt 原生手势契约事件并检查实际 widget 绘制；没有冒称物理触控板输入或连续 compositor 录屏验收。Windows 实际运行结果由 Root 的独立记录负责。
