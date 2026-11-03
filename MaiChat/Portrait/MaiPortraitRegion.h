// 文件作用：规划原图坐标中的有界人脸区域，避免为大照片复制多份整图像素。
#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include "MaiPortraitEngine.h"

// 稳定业务错误码供 C ABI/宿主分类；message 不含用户路径或任意底层错误原文。
class MaiPortraitFailure final : public std::runtime_error {
public:
    MaiPortraitFailure(std::string code, const std::string& message);
    const std::string& code() const;

private:
    std::string mCode;
};

struct MaiPortraitRegion {
    int x = 0, y = 0, width = 0, height = 0;
};

struct MaiPortraitRegionPlan {
    int sourceWidth = 0, sourceHeight = 0;
    int faceCount = 0, subjectIndex = 0;
    MaiPortraitRegion region;
    MaiPortraitFaceHint selectedFace;  // 全图坐标，EXIF 已转正，原点在左上。
};

// 只有宿主能创建本上下文；不能把模型参数直接当作坐标或特征使用。
// ROI 像素保持原生尺寸；features 仍由整图 proxy 生成，不污染已有个人模型特征域。
struct MaiPortraitRegionContext {
    MaiPortraitRegionPlan plan;
    std::array<float, 8> globalFeatures{};
    std::string sourceFingerprint;
};

class MaiPortraitRegionPlanner {
public:
    // 输入框在同一原图坐标中；重叠检测合并后按从左到右编号。无法选择时明确失败。
    // 原图不在这里解码。单 ROI 最长边 2048、至多 4 Mi 像素，不能隐式降低原图细节。
    static MaiPortraitRegionPlan faceRegion(int sourceWidth, int sourceHeight,
                                            const std::vector<MaiPortraitFaceHint>& faces, int subjectIndex);
    static std::vector<MaiPortraitFaceHint> mergeFaces(const std::vector<MaiPortraitFaceHint>& faces);
    // 缩略图为宿主按 EXIF 转正的 RGBA8，最长边 <=256；返回现有八维全局特征。
    static std::array<float, 8> globalFeatures(const unsigned char* rgba, int width, int height, int stride,
                                               const MaiPortraitRegionPlan& plan);
};
