// 文件作用：通过真实共享 runtime 验证无模型失败、重复普通调用与进程存活，不加载权重。
#include "MaiLocalInference.h"

#include <cstdio>
#include <cstring>

int main() {
    const auto initial = mai_local_inference_status();
    if (initial.status != MAI_LOCAL_INFERENCE_OK || initial.busy || initial.has_model) {
        std::fputs("FAIL initial real-runtime status\n", stderr);
        return 1;
    }
    for (int index = 0; index < 3; ++index) {
        auto input = mai_local_inference_request_defaults();
        constexpr const char model[] = "/maichat-missing-local-inference-fixture/model.gguf";
        constexpr const char prompt[] = "Describe the local test image";
        input.model_path = {model, sizeof(model) - 1};
        input.prompt = {prompt, sizeof(prompt) - 1};
        const auto job = mai_local_inference_job_create();
        auto result = mai_local_inference_generate(job, &input);
        const bool correct = job != 0 && result.status == MAI_LOCAL_INFERENCE_NOT_FOUND &&
                             result.text_utf8 == nullptr && result.text_size == 0 && result.error_utf8 != nullptr &&
                             std::strstr(result.error_utf8, "model.gguf") == nullptr;
        mai_local_inference_result_free(&result);
        mai_local_inference_job_destroy(job);
        if (!correct) {
            std::fprintf(stderr, "FAIL real-runtime missing model attempt %d\n", index + 1);
            return 1;
        }
    }
    const auto final = mai_local_inference_status();
    if (final.status != MAI_LOCAL_INFERENCE_OK || final.busy || final.has_model ||
        final.model_generation != initial.model_generation) {
        std::fputs("FAIL missing model changed resident runtime\n", stderr);
        return 1;
    }
    if (mai_local_inference_unload() != MAI_LOCAL_INFERENCE_OK ||
        mai_local_inference_unload() != MAI_LOCAL_INFERENCE_OK) {
        std::fputs("FAIL real-runtime empty unload is not idempotent\n", stderr);
        return 1;
    }
    const auto unloaded = mai_local_inference_status();
    if (unloaded.status != MAI_LOCAL_INFERENCE_OK || unloaded.busy || unloaded.has_model || unloaded.has_projector ||
        unloaded.context_size != 0 || unloaded.model_generation != initial.model_generation) {
        std::fputs("FAIL real-runtime unload state\n", stderr);
        return 1;
    }
    std::puts("PASS real-runtime missing model repeated 3 times; process remains alive");
    return 0;
}
