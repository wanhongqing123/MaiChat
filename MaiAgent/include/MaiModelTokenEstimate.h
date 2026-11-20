// 文件作用：定义不依赖模型客户端或传输接口的输入容量计数快照。
#pragma once

#include <cstddef>

// 本地估算与供应商 usage 分开。exact=false 时不能保证已包含供应商模板/媒体的精确成本，
// 也不能把此值当实际计费 Token。模型实现有真实 tokenizer 时可返回精确计数。
struct MaiModelTokenEstimate {
    std::size_t tokens = 0;
    bool exact = false;
};
