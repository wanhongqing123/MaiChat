// 文件作用：在加载权重前，以真实 GGUF 的元数据检查主模型所需的后端算子。
#pragma once

#include <cstdint>
#include <functional>

#include "MaiFilePath.h"

struct ggml_tensor;

// 内部后端选择辅助接口，不是模型兼容性或质量验收入口。调用前必须已通过
// validateMaiLlamaModelConfig；文件在校验、探测和随后的加载之间必须保持不变。
// 同步读取最多 64 MiB 前缀，只分配元数据和无数据 tensor，不加载权重、KV 或执行 kernel。
// 目前识别 qwen2/qwen35；每个真实矩阵的类型/形状，以及注意力和循环状态算子都须支持。
// 不认识的架构、缺失/异常元数据、读取失败或任一算子不支持均返回 false，宿主须使用纯 CPU。
// supportsOperation 在调用线程同步执行；tensor 仅在回调期间有效，不能保存指针或执行计算。
// 回调只供查询后端能力及隔离测试观察，不替换实际模型推理。函数不保留跨请求状态。
bool supportsMaiLlamaModelOperations(
    const MaiFilePath& modelPath, std::uint32_t contextSize,
    const std::function<bool(const ggml_tensor*)>& supportsOperation);
