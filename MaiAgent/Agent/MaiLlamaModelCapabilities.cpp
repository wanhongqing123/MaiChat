// 文件作用：从已校验模型的实际架构、张量形状和类型构造无数据的后端能力探针。
#include "MaiLlamaModelCapabilities.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <tuple>

#include <ggml-cpp.h>
#include <gguf.h>

#include "MaiFileSystem.h"

namespace {
constexpr std::size_t kMaximumPrefixBytes = 64 * 1024 * 1024;

bool hasMatrixVectorKernel(ggml_type type) {
    // 固定上游 ggml-metal-device.cpp 的 get_pipeline_mul_mv 类型分派范围。
    // supports_op 对 F32 输入会放过 I32 等没有 dot kernel 的类型，随后分派会直接 abort；
    // 容器校验只保证类型/布局合法，不能代替这道执行类型门禁。
    switch (type) {
        case GGML_TYPE_F32:
        case GGML_TYPE_F16:
        case GGML_TYPE_BF16:
        case GGML_TYPE_Q1_0:
        case GGML_TYPE_Q2_0:
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_1:
        case GGML_TYPE_Q5_0:
        case GGML_TYPE_Q5_1:
        case GGML_TYPE_Q8_0:
        case GGML_TYPE_MXFP4:
        case GGML_TYPE_Q2_K:
        case GGML_TYPE_Q3_K:
        case GGML_TYPE_Q4_K:
        case GGML_TYPE_Q5_K:
        case GGML_TYPE_Q6_K:
        case GGML_TYPE_IQ2_XXS:
        case GGML_TYPE_IQ2_XS:
        case GGML_TYPE_IQ3_XXS:
        case GGML_TYPE_IQ3_S:
        case GGML_TYPE_IQ2_S:
        case GGML_TYPE_IQ1_S:
        case GGML_TYPE_IQ1_M:
        case GGML_TYPE_IQ4_NL:
        case GGML_TYPE_IQ4_XS:
        case GGML_TYPE_TQ2_0: return true;
        default: return false;
    }
}

bool unsignedValue(const gguf_context* metadata, const std::string& key, std::uint32_t& value) {
    const auto index = gguf_find_key(metadata, key.c_str());
    if (index < 0 || gguf_get_kv_type(metadata, index) != GGUF_TYPE_UINT32) return false;
    value = gguf_get_val_u32(metadata, index);
    return value > 0 && value <= 32768;
}

bool probeOperations(const gguf_context* metadata, std::uint32_t contextSize,
                     const std::function<bool(const ggml_tensor*)>& supports) {
    const auto architectureIndex = gguf_find_key(metadata, "general.architecture");
    if (architectureIndex < 0 || gguf_get_kv_type(metadata, architectureIndex) != GGUF_TYPE_STRING)
        return false;
    const std::string architecture = gguf_get_val_str(metadata, architectureIndex);
    if (architecture != "qwen2" && architecture != "qwen35") return false;
    const auto key = [&](const char* suffix) { return architecture + "." + suffix; };
    std::uint32_t embedding = 0, heads = 0, kvHeads = 0, keyLength = 0, valueLength = 0;
    if (!unsignedValue(metadata, key("embedding_length"), embedding) ||
        !unsignedValue(metadata, key("attention.head_count"), heads) ||
        !unsignedValue(metadata, key("attention.head_count_kv"), kvHeads) || heads % kvHeads != 0)
        return false;
    // Qwen2 的旧 GGUF 省略独立 head length；Qwen3.5 的 head256 不能由 embedding/heads 猜。
    if (architecture == "qwen2" &&
        gguf_find_key(metadata, key("attention.key_length").c_str()) < 0) {
        if (embedding % heads != 0) return false;
        keyLength = embedding / heads;
    } else if (!unsignedValue(metadata, key("attention.key_length"), keyLength)) {
        return false;
    }
    if (architecture == "qwen2" &&
        gguf_find_key(metadata, key("attention.value_length").c_str()) < 0)
        valueLength = keyLength;
    else if (!unsignedValue(metadata, key("attention.value_length"), valueLength))
        return false;

    std::uint32_t convKernel = 0, stateSize = 0, groups = 0, valueHeads = 0, inner = 0;
    if (architecture == "qwen35" &&
        (!unsignedValue(metadata, key("ssm.conv_kernel"), convKernel) ||
         !unsignedValue(metadata, key("ssm.state_size"), stateSize) ||
         !unsignedValue(metadata, key("ssm.group_count"), groups) ||
         !unsignedValue(metadata, key("ssm.time_step_rank"), valueHeads) ||
         !unsignedValue(metadata, key("ssm.inner_size"), inner) || valueHeads % groups != 0 ||
         inner != stateSize * valueHeads))
        return false;

    // 同一类型/形状只查询一次，但保留 Q4_K_M 实际混用的 Q5_K、Q6_K、Q8_0 和 F32。
    // 不能用文件名里的量化后缀或某一层替代整个模型；SSM 卷积核另按真实卷积算子检查。
    std::set<std::tuple<ggml_type, std::int64_t, std::int64_t>> matrices;
    const auto count = gguf_get_n_tensors(metadata);
    if (count <= 0 || count > 100000) return false;
    bool hasConvolution = false;
    for (std::int64_t index = 0; index < count; ++index) {
        const auto* shape = gguf_get_tensor_ne(metadata, index);
        if (shape[2] != 1 || shape[3] != 1) return false;
        if (shape[1] == 1) continue;
        const std::string name = gguf_get_tensor_name(metadata, index);
        const auto type = gguf_get_tensor_type(metadata, index);
        if (name.find(".ssm_conv1d.weight") != std::string::npos) {
            if (architecture != "qwen35" || type != GGML_TYPE_F32 || shape[0] != convKernel ||
                shape[1] != inner + 2LL * groups * stateSize)
                return false;
            hasConvolution = true;
            continue;
        }
        if (!hasMatrixVectorKernel(type) || shape[0] <= 0 || shape[0] > 1048576 || shape[1] <= 0 ||
            shape[1] > 1048576)
            return false;
        matrices.emplace(type, shape[0], shape[1]);
    }
    if (matrices.empty() || (architecture == "qwen35" && !hasConvolution)) return false;

    for (const std::int64_t batch : {std::int64_t{1}, std::min<std::int64_t>(512, contextSize)}) {
        // 每组探针单独释放小型元数据 arena，避免模型张量数量增加时累积临时 tensor。
        for (const auto& matrix : matrices) {
            ggml_context_ptr context(ggml_init({3 * ggml_tensor_overhead(), nullptr, true}));
            if (!context) return false;
            auto* weights = ggml_new_tensor_2d(context.get(), std::get<0>(matrix),
                                               std::get<1>(matrix), std::get<2>(matrix));
            auto* input = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, weights->ne[0], batch);
            if (!supports(ggml_mul_mat(context.get(), weights, input))) return false;
        }
        ggml_context_ptr context(ggml_init({20 * ggml_tensor_overhead(), nullptr, true}));
        if (!context) return false;
        auto* queries =
            ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, keyLength, batch, heads, 1);
        auto* keys =
            ggml_new_tensor_4d(context.get(), GGML_TYPE_F16, keyLength, contextSize, kvHeads, 1);
        auto* values =
            ggml_new_tensor_4d(context.get(), GGML_TYPE_F16, valueLength, contextSize, kvHeads, 1);
        if (!supports(ggml_flash_attn_ext(context.get(), queries, keys, values, nullptr,
                                          1.0f / std::sqrt(static_cast<float>(keyLength)), 0, 0)))
            return false;
        if (architecture != "qwen35") continue;
        const std::int64_t channels = inner + 2LL * groups * stateSize;
        auto* convolutionInput =
            ggml_new_tensor_3d(context.get(), GGML_TYPE_F32, convKernel - 1 + batch, channels, 1);
        auto* kernel = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, convKernel, channels);
        if (!supports(ggml_ssm_conv(context.get(), convolutionInput, kernel))) return false;
        auto* q = ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, stateSize, groups, batch, 1);
        auto* k = ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, stateSize, groups, batch, 1);
        auto* v = ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, stateSize, valueHeads, batch, 1);
        auto* gate = ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, 1, valueHeads, batch, 1);
        auto* beta = ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, 1, valueHeads, batch, 1);
        auto* state =
            ggml_new_tensor_4d(context.get(), GGML_TYPE_F32, stateSize, stateSize, valueHeads, 1);
        if (!supports(ggml_gated_delta_net(context.get(), q, k, v, gate, beta, state, 1)))
            return false;
    }
    return true;
}
}  // namespace

bool supportsMaiLlamaModelOperations(
    const MaiFilePath& modelPath, std::uint32_t contextSize,
    const std::function<bool(const ggml_tensor*)>& supportsOperation) {
    if (!supportsOperation || contextSize < 128 || contextSize > 32768) return false;
    try {
        std::string prefix;
        if (MaiFileSystem::readFile(modelPath, prefix, kMaximumPrefixBytes)) return false;
        // 调用前的严格校验负责不可信容器/模板边界；这里仍给重复读取明确的内存上限。
        // no_alloc 且 ctx=nullptr 只解析描述，不读取张量 blob，也不建立第二个 CPU 模型。
        gguf_context_ptr metadata(
            gguf_init_from_buffer(prefix.data(), prefix.size(), {true, nullptr}));
        return metadata && probeOperations(metadata.get(), contextSize, supportsOperation);
    } catch (const std::exception&) {
        return false;
    }
}
