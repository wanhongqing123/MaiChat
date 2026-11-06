// 文件作用：宿主原生人脸定位薄适配，不选择人像参数或复制皮肤处理算法。
#pragma once

#include <functional>
#include <QImage>
#include <QJsonArray>

// 输入是已经转正的 sRGB 原图；返回左上原生像素坐标的人脸/眼部提示。
// 全图及重叠原生窗口都参与检测，避免大图中的远处人物被缩略图丢失。
// 仅后台调用，失败抛异常；结果仍由共用 C++ 核心去重和确定目标。
QJsonArray desktopPortraitFaces(const QImage& rgba, const std::function<bool()>& isCanceled);
