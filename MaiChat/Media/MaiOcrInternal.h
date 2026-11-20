// 文件作用：声明 OCR 内部的纯 CTC 后处理，供推理及最小确定性回归共用。
#pragma once

#include <string>
#include <vector>

#include "MaiOcr.h"

struct MaiOcrDecodedLine {
    std::string text;
    float confidence = 0;
};

// dictionary[0] 必须为空白标签。严格校验所有概率和维度，失败不返回部分文本。
MaiOcrStatus maiOcrDecodeCtc(const float* probabilities, size_t steps, size_t classes,
                             const std::vector<std::string>& dictionary, MaiOcrDecodedLine& output);
