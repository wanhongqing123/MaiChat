// 文件作用：以独立 GGUF 元数据和后端能力回调验证真实类型/形状探测与纯 CPU 拒绝边界。
// 不加载权重、不执行 kernel；这些测试不代表模型质量、真实 GPU 数值或设备内存验收。
#include "MaiLlamaModelCapabilities.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include <ggml-cpp.h>
#include <gguf.h>

#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiLlamaModelClient.h"
#include "MaiLlamaModelValidation.h"

namespace {
int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

gguf_context_ptr fixture(bool hybrid) {
    gguf_context_ptr metadata(gguf_init_empty());
    gguf_set_val_str(metadata.get(), "general.architecture", hybrid ? "qwen35" : "qwen2");
    const std::string architecture = hybrid ? "qwen35." : "qwen2.";
    const auto set = [&](const char* key, std::uint32_t value) {
        gguf_set_val_u32(metadata.get(), (architecture + key).c_str(), value);
    };
    set("embedding_length", hybrid ? 2048 : 896);
    set("attention.head_count", hybrid ? 8 : 14);
    set("attention.head_count_kv", 2);
    if (hybrid) {
        set("attention.key_length", 256);
        set("attention.value_length", 256);
        set("ssm.conv_kernel", 4);
        set("ssm.state_size", 128);
        set("ssm.group_count", 16);
        set("ssm.time_step_rank", 16);
        set("ssm.inner_size", 2048);
    }
    ggml_context_ptr context(ggml_init({8 * ggml_tensor_overhead(), nullptr, true}));
    const auto matrix = [&](const char* name, ggml_type type, std::int64_t input,
                            std::int64_t output) {
        auto* tensor = ggml_new_tensor_2d(context.get(), type, input, output);
        ggml_set_name(tensor, name);
        gguf_add_tensor(metadata.get(), tensor);
    };
    if (hybrid) {
        // 实际候选 GGUF 的混合量化和投影形状，包含 tied embedding 的输出矩阵。
        matrix("token_embd.weight", GGML_TYPE_Q6_K, 2048, 248320);
        matrix("blk.0.ffn_gate.weight", GGML_TYPE_Q4_K, 2048, 6144);
        matrix("blk.0.ffn_down.weight", GGML_TYPE_Q6_K, 6144, 2048);
        matrix("blk.3.attn_k.weight", GGML_TYPE_Q5_K, 2048, 512);
        matrix("blk.0.ssm_out.weight", GGML_TYPE_Q8_0, 2048, 2048);
        matrix("blk.0.ssm_alpha.weight", GGML_TYPE_F32, 2048, 16);
        matrix("blk.0.ssm_conv1d.weight", GGML_TYPE_F32, 4, 6144);
    } else {
        matrix("blk.0.ffn_down.weight", GGML_TYPE_Q4_K, 4864, 896);
    }
    return metadata;
}

void writeMetadata(const MaiFilePath& path, const gguf_context* metadata) {
    // 只写容器元数据，特意不创建权重 blob；能力探测不得因任何算子构造读取/分配权重。
    std::string bytes(gguf_get_meta_size(metadata), '\0');
    gguf_get_meta_data(metadata, bytes.data());
    CHECK(!MaiFileSystem::writeFile(path, bytes));
}

void test_hybrid_shapes_and_mixed_quantization(const MaiFilePath& path) {
    const auto metadata = fixture(true);
    writeMetadata(path, metadata.get());
    std::set<ggml_type> matrixTypes;
    std::set<std::int64_t> batches;
    int attention = 0, convolution = 0, deltaNet = 0;
    CHECK(supportsMaiLlamaModelOperations(path, 32768, [&](const ggml_tensor* op) {
        CHECK(op->data == nullptr);
        for (const auto* source : op->src)
            if (source) CHECK(source->data == nullptr);
        if (op->op == GGML_OP_MUL_MAT) {
            matrixTypes.insert(op->src[0]->type);
            batches.insert(op->src[1]->ne[1]);
        } else if (op->op == GGML_OP_FLASH_ATTN_EXT) {
            ++attention;
            CHECK(op->src[0]->ne[0] == 256 && op->src[0]->ne[2] == 8);
            CHECK(op->src[1]->type == GGML_TYPE_F16 && op->src[1]->ne[1] == 32768);
            CHECK(op->src[1]->ne[2] == 2 && op->src[2]->ne[0] == 256);
        } else if (op->op == GGML_OP_SSM_CONV) {
            ++convolution;
            CHECK(op->src[1]->ne[0] == 4 && op->src[1]->ne[1] == 6144);
            CHECK(op->src[0]->ne[0] == 3 + op->ne[1]);
        } else if (op->op == GGML_OP_GATED_DELTA_NET) {
            ++deltaNet;
            CHECK(op->src[0]->ne[0] == 128 && op->src[0]->ne[1] == 16);
            CHECK(op->src[3]->ne[0] == 1 && op->src[4]->ne[1] == 16);
            CHECK(op->src[5]->ne[0] == 128 && op->src[5]->ne[2] == 16);
        } else {
            CHECK(false);
        }
        return true;
    }));
    CHECK((matrixTypes == std::set<ggml_type>{GGML_TYPE_F32, GGML_TYPE_Q4_K, GGML_TYPE_Q5_K,
                                              GGML_TYPE_Q6_K, GGML_TYPE_Q8_0}));
    CHECK((batches == std::set<std::int64_t>{1, 512}));
    CHECK(attention == 2 && convolution == 2 && deltaNet == 2);

    // 任一真实量化类型或混合注意力必要算子不支持，都不能只卸载部分层并报告 GPU 可用。
    for (const auto type : matrixTypes)
        CHECK(!supportsMaiLlamaModelOperations(path, 32768, [type](const ggml_tensor* op) {
            return op->op != GGML_OP_MUL_MAT || op->src[0]->type != type;
        }));
    for (const auto operation : {GGML_OP_FLASH_ATTN_EXT, GGML_OP_SSM_CONV, GGML_OP_GATED_DELTA_NET})
        CHECK(!supportsMaiLlamaModelOperations(
            path, 32768, [operation](const ggml_tensor* op) { return op->op != operation; }));
    CHECK(!supportsMaiLlamaModelOperations(path, 32768, [](const ggml_tensor* op) {
        return op->op != GGML_OP_GATED_DELTA_NET || op->src[0]->ne[2] != 512;
    }));
}

void test_legacy_profile_and_invalid_metadata(const MaiFilePath& path) {
    auto metadata = fixture(false);
    writeMetadata(path, metadata.get());
    int attention = 0;
    CHECK(supportsMaiLlamaModelOperations(path, 256, [&](const ggml_tensor* op) {
        if (op->op == GGML_OP_FLASH_ATTN_EXT) {
            ++attention;
            CHECK(op->src[0]->ne[0] == 64 && op->src[0]->ne[2] == 14);
            CHECK(op->src[1]->ne[1] == 256);
        }
        CHECK(op->op != GGML_OP_SSM_CONV && op->op != GGML_OP_GATED_DELTA_NET);
        return true;
    }));
    CHECK(attention == 2);
    const auto accepts = [](const ggml_tensor*) { return true; };
    CHECK(!supportsMaiLlamaModelOperations(path, 127, accepts));
    CHECK(!supportsMaiLlamaModelOperations(path, 32769, accepts));
    CHECK(!supportsMaiLlamaModelOperations(path, 256, {}));
    gguf_set_val_str(metadata.get(), "general.architecture", "unreviewed");
    writeMetadata(path, metadata.get());
    CHECK(!supportsMaiLlamaModelOperations(path, 256, accepts));
    metadata = fixture(true);
    gguf_remove_key(metadata.get(), "qwen35.attention.key_length");
    writeMetadata(path, metadata.get());
    CHECK(!supportsMaiLlamaModelOperations(path, 256, accepts));
    metadata = fixture(true);
    gguf_set_val_str(metadata.get(), "qwen35.ssm.state_size", "128");
    writeMetadata(path, metadata.get());
    CHECK(!supportsMaiLlamaModelOperations(path, 256, accepts));
    metadata = fixture(true);
    gguf_set_val_u32(metadata.get(), "qwen35.ssm.inner_size", 1024);
    writeMetadata(path, metadata.get());
    CHECK(!supportsMaiLlamaModelOperations(path, 256, accepts));
    CHECK(!MaiFileSystem::writeFile(path, "GGUF"));
    CHECK(!supportsMaiLlamaModelOperations(path, 256, accepts));
}

void test_non_matrix_types_are_rejected_before_backend(const MaiFilePath& path) {
    const auto* templatePath = std::getenv("MAI_LLAMA_CHAT_TEST_TEMPLATE");
    std::string approved;
    if (templatePath)
        CHECK(!MaiFileSystem::readFile(MaiFilePath::fromUtf8(templatePath), approved, 1024 * 1024));
    for (const auto type : {GGML_TYPE_I32, GGML_TYPE_F64, GGML_TYPE_Q8_K}) {
        auto metadata = fixture(false);
        ggml_context_ptr context(ggml_init({ggml_tensor_overhead(), nullptr, true}));
        auto* tensor = ggml_new_tensor_2d(context.get(), type, 256, 16);
        ggml_set_name(tensor, "blk.0.invalid_linear.weight");
        gguf_add_tensor(metadata.get(), tensor);
        if (!approved.empty())
            gguf_set_val_str(metadata.get(), "tokenizer.chat_template", approved.c_str());
        writeMetadata(path, metadata.get());
        // 补齐容器的全部数据范围，证明拒绝并非因缺失权重字节；这些合成数据从不加载推断。
        std::string bytes;
        CHECK(!MaiFileSystem::readFile(path, bytes));
        const auto last = gguf_get_n_tensors(metadata.get()) - 1;
        bytes.resize(bytes.size() + gguf_get_tensor_offset(metadata.get(), last) +
                         gguf_get_tensor_size(metadata.get(), last),
                     '\0');
        CHECK(!MaiFileSystem::writeFile(path, bytes));
        if (!approved.empty()) {
            MaiLlamaModelConfig config;
            config.modelPath = path;
            CHECK(!validateMaiLlamaModelConfig(config));
        }
        bool backendCalled = false;
        CHECK(!supportsMaiLlamaModelOperations(path, 256, [&](const ggml_tensor*) {
            backendCalled = true;
            return true;
        }));
        CHECK(!backendCalled);
    }
}
}  // namespace

int main() {
    const auto directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("local_capabilities_")));
    CHECK(!MaiFileSystem::createDirectories(directory));
    // 文件名刻意与架构无关；探针必须由内容选择 Qwen2/Qwen3.5 的真实配置。
    const auto path = directory.append(MaiFilePath::fromUtf8("renamed-model.gguf"));
    test_hybrid_shapes_and_mixed_quantization(path);
    test_legacy_profile_and_invalid_metadata(path);
    test_non_matrix_types_are_rejected_before_backend(path);
    if (const auto* model = std::getenv("MAI_LLAMA_CAPABILITY_TEST_MODEL")) {
        CHECK(supportsMaiLlamaModelOperations(MaiFilePath::fromUtf8(model), 32768,
                                              [](const ggml_tensor*) { return true; }));
    }
    MaiFileSystem::removeRecursively(directory);
    std::printf("MaiLlamaModelCapabilitiesTests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
