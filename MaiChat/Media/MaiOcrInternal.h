// 文件作用：声明 OCR 内部几何、切分判定和 CTC 后处理，供推理及确定性回归共用。
#pragma once

#include <string>
#include <array>
#include <vector>

#include "MaiOcr.h"

struct MaiOcrDecodedLine {
    std::string text;
    float confidence = 0;
};

// Crop projection and fragment output share the same oriented source geometry.
struct MaiOcrLineLayout {
    std::array<MaiOcrPoint, 4> quad{};
    double width = 0;
    double height = 0;
    double recognitionWidth = 0;
};
MaiOcrStatus maiOcrMeasureLine(const std::array<MaiOcrPoint, 4>& quad, MaiOcrLineLayout& layout);
std::array<MaiOcrPoint, 4> maiOcrSliceLine(const MaiOcrLineLayout& layout, double begin, double end);

// Returns a candidate column or -1 when no defensible whitespace cut exists.
// The caller owns this bounded grayscale patch for the duration of the call.
int maiOcrFindWhitespaceCut(const unsigned char* gray, int width, int height, int stride);

// dictionary[0] 必须为空白标签。严格校验所有概率和维度，失败不返回部分文本。
MaiOcrStatus maiOcrDecodeCtc(const float* probabilities, size_t steps, size_t classes,
                             const std::vector<std::string>& dictionary, MaiOcrDecodedLine& output);
