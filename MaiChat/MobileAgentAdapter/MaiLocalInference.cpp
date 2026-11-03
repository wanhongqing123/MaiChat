// 文件作用：把普通移动端本地推理请求交给全进程共享 runtime，并管理跨线程取消生命周期。
#include "MaiLocalInference.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "MaiLlamaTools.h"

namespace {

constexpr std::size_t kMaximumJobs = 64;
constexpr std::size_t kMaximumPathBytes = 16 * 1024;
constexpr std::size_t kMaximumPromptBytes = 64 * 1024;

struct Job {
    std::atomic<bool> started{false};
    std::atomic<bool> canceled{false};
};

struct JobRegistry {
    std::mutex mutex;
    std::unordered_map<MaiLocalInferenceJob, std::shared_ptr<Job>> jobs;
    MaiLocalInferenceJob nextId = 1;
};

JobRegistry& registry() {
    // 进程退出时不抢先销毁正在收尾的宿主调用；所有权由各 job 的显式 destroy 管理。
    static auto* value = new JobRegistry;
    return *value;
}

std::shared_ptr<Job> findJob(MaiLocalInferenceJob id) {
    auto& values = registry();
    std::lock_guard<std::mutex> lock(values.mutex);
    const auto found = values.jobs.find(id);
    return found == values.jobs.end() ? nullptr : found->second;
}

char* copyText(const char* text, std::size_t size) {
    auto* bytes = static_cast<char*>(std::malloc(size + 1));
    if (bytes) {
        std::memcpy(bytes, text, size);
        bytes[size] = 0;
    }
    return bytes;
}

const char* safeError(MaiLocalInferenceStatus status) {
    switch (status) {
        case MAI_LOCAL_INFERENCE_INVALID_INPUT: return "Invalid local inference request";
        case MAI_LOCAL_INFERENCE_NOT_FOUND: return "Local inference job or model file was not found";
        case MAI_LOCAL_INFERENCE_BUSY: return "This local inference job was already started";
        case MAI_LOCAL_INFERENCE_NOT_CONFIGURED: return "Local inference runtime is not configured";
        case MAI_LOCAL_INFERENCE_CANCELED: return "Local inference was canceled";
        case MAI_LOCAL_INFERENCE_NOT_SUPPORTED: return "This local inference operation is not supported";
        default: return "Local inference failed";
    }
}

MaiLocalInferenceResult failure(MaiLocalInferenceStatus status) {
    MaiLocalInferenceResult result{};
    result.status = status;
    const char* message = safeError(status);
    result.error_utf8 = copyText(message, std::strlen(message));
    return result;
}

MaiLocalInferenceStatus statusFor(MaiErrorCode code) {
    switch (code) {
        case MaiErrorCode::InvalidInput: return MAI_LOCAL_INFERENCE_INVALID_INPUT;
        case MaiErrorCode::NotFound: return MAI_LOCAL_INFERENCE_NOT_FOUND;
        case MaiErrorCode::Busy: return MAI_LOCAL_INFERENCE_BUSY;
        case MaiErrorCode::NotConfigured: return MAI_LOCAL_INFERENCE_NOT_CONFIGURED;
        case MaiErrorCode::Canceled: return MAI_LOCAL_INFERENCE_CANCELED;
        case MaiErrorCode::NotSupported: return MAI_LOCAL_INFERENCE_NOT_SUPPORTED;
        default: return MAI_LOCAL_INFERENCE_INTERNAL;
    }
}

bool readUtf8(MaiLocalInferenceUtf8 input, std::size_t limit, bool required, std::string& output) {
    if (input.size > limit || (input.size && !input.bytes) || (required && !input.size)) return false;
    // 与既有文件/PDF 边界相同：拒绝 overlong、surrogate、超出 Unicode 的码点，
    // 避免 Windows 转路径时悄悄丢字；明确长度也防止 NUL 截断不同请求。
    for (std::size_t index = 0; index < input.size;) {
        const auto first = static_cast<unsigned char>(input.bytes[index++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        unsigned remaining = 0;
        std::uint32_t point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) {
            remaining = 1;
            point = first & 0x1F;
            minimum = 0x80;
        } else if (first >= 0xE0 && first <= 0xEF) {
            remaining = 2;
            point = first & 0x0F;
            minimum = 0x800;
        } else if (first >= 0xF0 && first <= 0xF4) {
            remaining = 3;
            point = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > input.size - index) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(input.bytes[index++]);
            if ((next & 0xC0) != 0x80) return false;
            point = (point << 6) | (next & 0x3F);
        }
        if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    }
    output.assign(input.bytes ? input.bytes : "", input.size);
    return true;
}

bool decodeRequest(const MaiLocalInferenceRequest& input, MaiLlamaRequest& output) {
    std::string model, projector, image;
    if (!readUtf8(input.model_path, kMaximumPathBytes, true, model) ||
        !readUtf8(input.projector_path, kMaximumPathBytes, false, projector) ||
        !readUtf8(input.image_path, kMaximumPathBytes, false, image) ||
        !readUtf8(input.prompt, kMaximumPromptBytes, true, output.prompt) ||
        (input.use_chat_template != 0 && input.use_chat_template != 1))
        return false;
    output.modelPath = MaiFilePath::fromUtf8(model);
    output.projectorPath = MaiFilePath::fromUtf8(projector);
    output.imagePath = MaiFilePath::fromUtf8(image);
    if (!output.modelPath.isAbsolute() || (!projector.empty() && !output.projectorPath.isAbsolute()) ||
        (!image.empty() && !output.imagePath.isAbsolute()) || projector.empty() != image.empty())
        return false;
    output.contextSize = input.context_size;
    output.maximumTokens = input.maximum_tokens;
    output.threads = input.threads;
    output.temperature = input.temperature;
    output.seed = input.seed;
    output.maximumOutputBytes = input.maximum_output_bytes;
    output.useChatTemplate = input.use_chat_template != 0;
    // 数值上限由共用 runtime 唯一校验，普通入口和工具不维护不同规则。
    return true;
}

}  // namespace

MaiLocalInferenceRuntimeStatus mai_local_inference_status(void) {
    MaiLocalInferenceRuntimeStatus result{};
    try {
        const auto state = getSharedMaiLlamaRuntime()->status();
        result.status = MAI_LOCAL_INFERENCE_OK;
        result.busy = state.busy;
        result.has_model = state.hasModel;
        result.has_projector = state.hasProjector;
        result.context_size = state.contextSize;
        result.model_generation = state.modelGeneration;
    } catch (...) {
        result.status = MAI_LOCAL_INFERENCE_INTERNAL;
    }
    return result;
}

MaiLocalInferenceStatus mai_local_inference_unload(void) {
    try {
        const auto error = getSharedMaiLlamaRuntime()->unload();
        return error.hasError() ? statusFor(error.code()) : MAI_LOCAL_INFERENCE_OK;
    } catch (...) {
        return MAI_LOCAL_INFERENCE_INTERNAL;
    }
}

MaiLocalInferenceJob mai_local_inference_job_create(void) {
    try {
        auto& values = registry();
        std::lock_guard<std::mutex> lock(values.mutex);
        if (values.jobs.size() >= kMaximumJobs || values.nextId == std::numeric_limits<MaiLocalInferenceJob>::max())
            return 0;
        const auto id = values.nextId++;
        values.jobs.emplace(id, std::make_shared<Job>());
        return id;
    } catch (...) {
        return 0;
    }
}

int mai_local_inference_job_cancel(MaiLocalInferenceJob id) {
    try {
        auto job = findJob(id);
        if (!job) return 0;
        job->canceled.store(true);
        return 1;
    } catch (...) {
        return 0;
    }
}

int mai_local_inference_job_destroy(MaiLocalInferenceJob id) {
    try {
        auto& values = registry();
        std::lock_guard<std::mutex> lock(values.mutex);
        const auto found = values.jobs.find(id);
        if (found == values.jobs.end()) return 0;
        found->second->canceled.store(true);
        values.jobs.erase(found);
        return 1;
    } catch (...) {
        return 0;
    }
}

MaiLocalInferenceRequest mai_local_inference_request_defaults(void) {
    const MaiLlamaRequest defaults;
    MaiLocalInferenceRequest result{};
    result.context_size = defaults.contextSize;
    result.maximum_tokens = defaults.maximumTokens;
    result.threads = defaults.threads;
    result.temperature = defaults.temperature;
    result.seed = defaults.seed;
    result.maximum_output_bytes = defaults.maximumOutputBytes;
    result.use_chat_template = defaults.useChatTemplate;
    return result;
}

MaiLocalInferenceResult mai_local_inference_generate(MaiLocalInferenceJob id, const MaiLocalInferenceRequest* request) {
    try {
        auto job = findJob(id);
        if (!job) return failure(MAI_LOCAL_INFERENCE_NOT_FOUND);
        if (job->started.exchange(true)) return failure(MAI_LOCAL_INFERENCE_BUSY);
        if (job->canceled.load()) return failure(MAI_LOCAL_INFERENCE_CANCELED);
        MaiLlamaRequest decoded;
        if (!request || !decodeRequest(*request, decoded)) return failure(MAI_LOCAL_INFERENCE_INVALID_INPUT);
        const auto generated = getSharedMaiLlamaRuntime()->generate(decoded, &job->canceled);
        if (!generated) return failure(statusFor(generated.error().code()));
        const auto& output = generated.value();
        MaiLocalInferenceResult result{};
        result.text_utf8 = copyText(output.text.data(), output.text.size());
        if (!result.text_utf8) return failure(MAI_LOCAL_INFERENCE_INTERNAL);
        result.status = MAI_LOCAL_INFERENCE_OK;
        result.text_size = output.text.size();
        result.prompt_tokens = output.promptTokens;
        result.generated_tokens = output.generatedTokens;
        result.stop_reason = output.stopReason == MaiLlamaStopReason::EndOfSequence
                                 ? MAI_LOCAL_INFERENCE_END_OF_SEQUENCE
                             : output.stopReason == MaiLlamaStopReason::OutputLimit ? MAI_LOCAL_INFERENCE_OUTPUT_LIMIT
                                                                                    : MAI_LOCAL_INFERENCE_TOKEN_LIMIT;
        result.truncated = output.truncated;
        return result;
    } catch (...) {
        return failure(MAI_LOCAL_INFERENCE_INTERNAL);
    }
}

void mai_local_inference_result_free(MaiLocalInferenceResult* result) {
    if (!result) return;
    std::free(result->text_utf8);
    std::free(result->error_utf8);
    *result = {};
}
