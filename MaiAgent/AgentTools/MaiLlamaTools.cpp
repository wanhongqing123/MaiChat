// 文件作用：以 llama.cpp/mtmd 库 API 提供进程驻留、串行且可取消的本地推理。
#include "MaiLlamaTools.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>
#include <llama-cpp.h>
#include <llama.h>
#include <mtmd-helper.h>
#include <mtmd.h>
#include <stb/stb_image.h>

#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiTool.h"

namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaximumPromptBytes = 64 * 1024;
constexpr std::size_t kMaximumOutputBytes = 64 * 1024;
constexpr std::size_t kMaximumImageBytes = 20 * 1024 * 1024;
constexpr std::uint64_t kMaximumImagePixels = 8'000'000;
constexpr std::uint64_t kMaximumModelBytes = 32ULL * 1024 * 1024 * 1024;
constexpr std::uint32_t kMaximumContext = 32768;
constexpr std::uint32_t kMaximumTokens = 4096;

bool isCanceled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

MaiError canceledError() {
    return {MaiErrorCode::Canceled, "Local inference was canceled"};
}

void quietLog(ggml_log_level, const char*, void*) {}

// 与已有文件工具一致地拒绝过长、过短编码和代理项。返回 0 表示无效，负数表示字节尚未齐全。
int utf8Width(const std::string& text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return 1;
    const int width = first >= 0xc2 && first <= 0xdf   ? 2
                      : first >= 0xe0 && first <= 0xef ? 3
                      : first >= 0xf0 && first <= 0xf4 ? 4
                                                       : 0;
    if (!width) return 0;
    const std::size_t remaining = text.size() - offset;
    for (std::size_t index = 1; index < std::min<std::size_t>(width, remaining); ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if (next < 0x80 || next > 0xbf) return 0;
        if (index == 1 && ((first == 0xe0 && next < 0xa0) || (first == 0xed && next > 0x9f) ||
                           (first == 0xf0 && next < 0x90) || (first == 0xf4 && next > 0x8f)))
            return 0;
    }
    return remaining < static_cast<std::size_t>(width) ? -width : width;
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const int width = utf8Width(text, offset);
        if (width <= 0) return false;
        offset += static_cast<std::size_t>(width);
    }
    return true;
}

// 一个 tokenizer token 可以只含半个 UTF-8 字符。只交付完整字符，非法字节用替代字符表示，
// 不把损坏的 UTF-8 放进模型工具 JSON，也不在字节预算处切出半个字符。
bool appendOutput(std::string& output, std::string& pending, const std::string& piece,
                  std::size_t maximum, bool final) {
    pending.append(piece);
    std::size_t offset = 0;
    while (offset < pending.size()) {
        const int width = utf8Width(pending, offset);
        if (width < 0 && !final) break;
        const std::string replacement = "\xef\xbf\xbd";
        const std::size_t count = width > 0 ? static_cast<std::size_t>(width) : replacement.size();
        if (output.size() + count > maximum) {
            pending.clear();
            return false;
        }
        if (width > 0)
            output.append(pending, offset, count);
        else
            output.append(replacement);
        offset += width > 0 ? count : 1;
    }
    pending.erase(0, offset);
    return true;
}

struct MaiLlamaFileIdentity {
    MaiFilePath path;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};

bool sameIdentity(const MaiLlamaFileIdentity& first, const MaiLlamaFileIdentity& second) {
    return first.path == second.path && first.bytes == second.bytes &&
           first.modified == second.modified;
}

MaiResult<MaiLlamaFileIdentity> inspectFile(const MaiFilePath& input, bool model) {
    if (input.isEmpty() || !input.isAbsolute() || input.toUtf8().find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput,
                "Local inference requires authorized absolute file paths"};
    MaiLlamaFileIdentity identity;
    identity.path = MaiFileSystem::resolve(input);
    if (identity.path.isEmpty() || !identity.path.isAbsolute() ||
        !MaiFileSystem::exists(identity.path) || MaiFileSystem::isDirectory(identity.path) ||
        !MaiFileSystem::fileSize(identity.path, identity.bytes) || identity.bytes == 0 ||
        !MaiFileSystem::modifiedTime(identity.path, identity.modified))
        return {MaiErrorCode::NotFound, "A required local inference file is unavailable"};
    if (identity.bytes > (model ? kMaximumModelBytes : kMaximumImageBytes))
        return {MaiErrorCode::InvalidInput,
                "A local inference file exceeds the supported size limit"};
    std::string header;
    if (MaiFileSystem::readFile(identity.path, header, 4).hasError())
        return {MaiErrorCode::NotFound, "A required local inference file cannot be read"};
    if (model && header != "GGUF")
        return {MaiErrorCode::InvalidInput, "Local model and projector files must use GGUF format"};
    return identity;
}

MaiError validateRequest(const MaiLlamaRequest& request) {
    if (request.prompt.empty() || request.prompt.size() > kMaximumPromptBytes ||
        request.prompt.find('\0') != std::string::npos || !isValidUtf8(request.prompt))
        return {MaiErrorCode::InvalidInput, "Prompt must be nonempty UTF-8 within 64 KiB"};
    if (request.contextSize < 128 || request.contextSize > kMaximumContext ||
        request.maximumTokens < 1 || request.maximumTokens > kMaximumTokens ||
        request.maximumTokens >= request.contextSize || request.threads < 1 ||
        request.threads > 64 || !std::isfinite(request.temperature) || request.temperature < 0 ||
        request.temperature > 2 || request.maximumOutputBytes < 1 ||
        request.maximumOutputBytes > kMaximumOutputBytes)
        return {MaiErrorCode::InvalidInput,
                "Local inference parameters exceed their supported limits"};
    if (request.imagePath.isEmpty() != request.projectorPath.isEmpty())
        return {MaiErrorCode::InvalidInput,
                "Image inference requires both an image and a matching local GGUF projector"};
    return {};
}

MaiResult<std::string> prepareImageBytes(const MaiLlamaFileIdentity& file) {
    std::string bytes;
    bool truncated = false;
    const MaiError read = MaiFileSystem::readFile(file.path, bytes, kMaximumImageBytes, &truncated);
    if (read.hasError() || truncated || bytes.empty())
        return {MaiErrorCode::InvalidInput, "Image cannot be read within the 20 MiB input limit"};
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                               static_cast<int>(bytes.size()), &width, &height, &channels) ||
        width < 1 || height < 1 || width > 8192 || height > 8192 ||
        static_cast<std::uint64_t>(width) * height > kMaximumImagePixels)
        return {MaiErrorCode::InvalidInput,
                "Image must be a supported static bitmap within 8192 pixels per side and 8 MP"};
    return bytes;
}

MaiResult<std::string> formatPrompt(llama_model* model, const MaiLlamaRequest& request) {
    std::string prompt = request.prompt;
    const std::string marker = mtmd_default_marker();
    const auto markerOffset = prompt.find(marker);
    if (!request.imagePath.isEmpty()) {
        if (markerOffset == std::string::npos)
            prompt = marker + "\n" + prompt;
        else if (prompt.find(marker, markerOffset + marker.size()) != std::string::npos)
            return {MaiErrorCode::InvalidInput,
                    "Single-image inference accepts exactly one media marker"};
    } else if (markerOffset != std::string::npos) {
        return {MaiErrorCode::InvalidInput, "A media marker requires an image input"};
    }
    const char* chatTemplate =
        request.useChatTemplate ? llama_model_chat_template(model, nullptr) : nullptr;
    if (!chatTemplate || !*chatTemplate) return prompt;
    const llama_chat_message message{"user", prompt.c_str()};
    const int32_t required = llama_chat_apply_template(chatTemplate, &message, 1, true, nullptr, 0);
    if (required < 0)
        return {MaiErrorCode::NotSupported,
                "Model chat template is unsupported; provide a prepared prompt with chat "
                "templating disabled"};
    if (required == 0 || required > static_cast<int32_t>(kMaximumPromptBytes * 4))
        return {MaiErrorCode::InvalidInput, "Formatted prompt exceeds the supported input limit"};
    std::string formatted(static_cast<std::size_t>(required), '\0');
    const int32_t written =
        llama_chat_apply_template(chatTemplate, &message, 1, true, formatted.data(), required);
    if (written != required)
        return {MaiErrorCode::Internal, "Model chat template did not produce a complete prompt"};
    return formatted;
}

MaiResult<std::vector<llama_token>> tokenize(llama_model* model, const std::string& prompt) {
    const auto* vocabulary = llama_model_get_vocab(model);
    const int32_t queried = llama_tokenize(
        vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    if (queried >= 0 || queried == std::numeric_limits<int32_t>::min())
        return {MaiErrorCode::InvalidInput, "Prompt cannot be tokenized"};
    const int32_t count = -queried;
    if (count > static_cast<int32_t>(kMaximumContext))
        return {MaiErrorCode::InvalidInput,
                "Prompt token count exceeds the supported context limit"};
    std::vector<llama_token> tokens(static_cast<std::size_t>(count));
    const int32_t written =
        llama_tokenize(vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()),
                       tokens.data(), count, true, true);
    if (written != count)
        return {MaiErrorCode::Internal, "Prompt tokenization did not produce complete input"};
    return tokens;
}

MaiResult<std::string> tokenPiece(const llama_vocab* vocabulary, llama_token token) {
    std::string piece(256, '\0');
    int32_t length = llama_token_to_piece(vocabulary, token, piece.data(),
                                          static_cast<int32_t>(piece.size()), 0, false);
    if (length < 0 && length >= -static_cast<int32_t>(kMaximumOutputBytes)) {
        piece.resize(static_cast<std::size_t>(-length));
        length = llama_token_to_piece(vocabulary, token, piece.data(),
                                      static_cast<int32_t>(piece.size()), 0, false);
    }
    if (length < 0 || static_cast<std::size_t>(length) > piece.size())
        return {MaiErrorCode::Internal, "Generated token exceeds the bounded output limit"};
    piece.resize(static_cast<std::size_t>(length));
    return piece;
}

const char* stopReasonName(MaiLlamaStopReason reason) {
    switch (reason) {
        case MaiLlamaStopReason::EndOfSequence: return "end_of_sequence";
        case MaiLlamaStopReason::TokenLimit: return "token_limit";
        case MaiLlamaStopReason::OutputLimit: return "output_limit";
    }
    return "token_limit";
}
}  // namespace

class MaiLlamaRuntime::Implementation {
public:
    ~Implementation() {
        resetResident();
    }

    MaiResult<MaiLlamaResponse> generate(const MaiLlamaRequest& request,
                                         const std::atomic<bool>* cancel) {
        maiAssertBlockingAllowed("MaiLlamaRuntime::generate");
        const MaiError invalid = validateRequest(request);
        if (invalid.hasError()) return invalid;
        std::unique_lock<std::timed_mutex> operation(mOperationMutex, std::defer_lock);
        while (!operation.try_lock_for(std::chrono::milliseconds(10)))
            if (isCanceled(cancel)) return canceledError();
        if (isCanceled(cancel)) return canceledError();
        RequestScope scope(*this);
        mActiveCancel = cancel;
        try {
            const auto modelFile = inspectFile(request.modelPath, true);
            if (!modelFile) return modelFile.error();
            MaiLlamaFileIdentity projector;
            if (!request.projectorPath.isEmpty()) {
                const auto inspected = inspectFile(request.projectorPath, true);
                if (!inspected) return inspected.error();
                projector = inspected.value();
            }
            std::string imageBytes;
            if (!request.imagePath.isEmpty()) {
                const auto imageFile = inspectFile(request.imagePath, false);
                if (!imageFile) return imageFile.error();
                auto prepared = prepareImageBytes(imageFile.value());
                if (!prepared) return prepared.error();
                imageBytes = std::move(prepared.value());
            }
            if (isCanceled(cancel)) return canceledError();
            if (!mContext || !sameIdentity(mModelIdentity, modelFile.value()) ||
                !sameIdentity(mProjectorIdentity, projector) ||
                mContextSize != request.contextSize || mThreads != request.threads) {
                const MaiError loaded = load(request, modelFile.value(), projector);
                if (loaded.hasError()) return loaded;
            }
            llama_set_abort_callback(mContext.get(), abortEvaluation, this);
            const auto formatted = formatPrompt(mModel.get(), request);
            if (!formatted) return formatted.error();
            MaiLlamaResponse response;
            const MaiError evaluated =
                request.imagePath.isEmpty()
                    ? prefillText(formatted.value(), request, response)
                    : prefillImage(formatted.value(), imageBytes, request, response);
            if (evaluated.hasError()) return evaluated;
            return sample(request, response);
        } catch (const std::exception&) {
            resetResident();
            return isCanceled(cancel) ? MaiResult<MaiLlamaResponse>(canceledError())
                                      : MaiResult<MaiLlamaResponse>(MaiErrorCode::Internal,
                                                                    "Local inference failed");
        } catch (...) {
            resetResident();
            return {MaiErrorCode::Internal, "Local inference failed"};
        }
    }

    MaiLlamaStatus status() const {
        std::lock_guard<std::mutex> lock(mStatusMutex);
        return mStatus;
    }

    MaiError unload() {
        maiAssertBlockingAllowed("MaiLlamaRuntime::unload");
        std::lock_guard<std::timed_mutex> operation(mOperationMutex);
        resetResident();
        return {};
    }

private:
    static bool modelProgress(float, void* opaque) {
        return !isCanceled(static_cast<Implementation*>(opaque)->mActiveCancel);
    }
    static bool abortEvaluation(void* opaque) {
        return isCanceled(static_cast<Implementation*>(opaque)->mActiveCancel);
    }
    static bool visionEvaluation(ggml_tensor*, bool ask, void* opaque) {
        // ask=true 请求节点完成后的回调；此阶段返回 false 只会跳过观察。
        // 在后续回调中返回 false，后端才会合作停止计算。
        return ask || !isCanceled(static_cast<Implementation*>(opaque)->mActiveCancel);
    }

    struct RequestScope {
        explicit RequestScope(Implementation& owner) : mOwner(owner) {
            std::lock_guard<std::mutex> lock(mOwner.mStatusMutex);
            mOwner.mStatus.busy = true;
        }
        ~RequestScope() {
            if (mOwner.mContext) {
                llama_synchronize(mOwner.mContext.get());
                llama_set_abort_callback(mOwner.mContext.get(), nullptr, nullptr);
                // 独立任务和账号不能共享 KV 内容；清除元数据与数据，保留唯一驻留分配，
                // 直到显式 unload，避免每次请求重新分配整块工作内存。
                auto memory = llama_get_memory(mOwner.mContext.get());
                if (memory) llama_memory_clear(memory, true);
            }
            mOwner.mActiveCancel = nullptr;
            std::lock_guard<std::mutex> lock(mOwner.mStatusMutex);
            mOwner.mStatus.busy = false;
        }
        Implementation& mOwner;
    };

    void resetResident() {
        // model_free 与 mtmd_free 均不释放 llama_context；后两者还引用 model。
        // 必须先销毁投影器、decoder 及其 KV/scheduler，再释放权重。
        mProjector.reset();
        mContext.reset();
        mModel.reset();
        mModelIdentity = {};
        mProjectorIdentity = {};
        mContextSize = 0;
        mThreads = 0;
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.hasModel = false;
        mStatus.hasProjector = false;
        mStatus.contextSize = 0;
    }

    MaiError load(const MaiLlamaRequest& request, const MaiLlamaFileIdentity& modelFile,
                  const MaiLlamaFileIdentity& projectorFile) {
        resetResident();
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            // 固定日志接收器避免上游元数据和路径进入宿主 stderr 或工具结果。
            // 静态 CPU 后端自行注册，不扫描任意插件目录。
            llama_log_set(quietLog, nullptr);
            mtmd_helper_log_set(quietLog, nullptr);
            llama_backend_init();
        });
        auto modelParameters = llama_model_default_params();
        modelParameters.n_gpu_layers = 0;
        modelParameters.progress_callback = modelProgress;
        modelParameters.progress_callback_user_data = this;
        mModel.reset(llama_model_load_from_file(modelFile.path.toUtf8().c_str(), modelParameters));
        if (!mModel || isCanceled(mActiveCancel)) {
            resetResident();
            return isCanceled(mActiveCancel) ? canceledError()
                                             : MaiError(MaiErrorCode::NotConfigured,
                                                        "Local GGUF model could not be loaded");
        }
        const auto batchSize = std::min<std::uint32_t>(512, request.contextSize);
        if (!projectorFile.path.isEmpty()) {
            auto parameters = mtmd_context_params_default();
            parameters.use_gpu = false;
            parameters.warmup = false;
            parameters.n_threads = static_cast<int>(request.threads);
            parameters.image_max_tokens = static_cast<int>(batchSize);
            parameters.progress_callback = modelProgress;
            parameters.progress_callback_user_data = this;
            parameters.cb_eval = visionEvaluation;
            parameters.cb_eval_user_data = this;
            mProjector.reset(
                mtmd_init_from_file(projectorFile.path.toUtf8().c_str(), mModel.get(), parameters));
            if (!mProjector || !mtmd_support_vision(mProjector.get()) ||
                isCanceled(mActiveCancel)) {
                resetResident();
                return isCanceled(mActiveCancel)
                           ? canceledError()
                           : MaiError(MaiErrorCode::NotConfigured,
                                      "A compatible vision projector could not be loaded");
            }
        }
        auto contextParameters = llama_context_default_params();
        contextParameters.n_ctx = request.contextSize;
        contextParameters.n_batch = batchSize;
        contextParameters.n_ubatch = batchSize;
        contextParameters.n_threads = static_cast<int32_t>(request.threads);
        contextParameters.n_threads_batch = static_cast<int32_t>(request.threads);
        contextParameters.type_k = GGML_TYPE_F16;
        contextParameters.type_v = GGML_TYPE_F16;
        // 仅设 n_gpu_layers=0 仍保留上游默认的运算/KQV offload。
        // 首版让 decoder 全部使用 CPU，确保公开的 abort 取消边界成立。
        contextParameters.offload_kqv = false;
        contextParameters.op_offload = false;
        contextParameters.abort_callback = abortEvaluation;
        contextParameters.abort_callback_data = this;
        mContext.reset(llama_init_from_model(mModel.get(), contextParameters));
        if (!mContext || isCanceled(mActiveCancel)) {
            resetResident();
            return isCanceled(mActiveCancel)
                       ? canceledError()
                       : MaiError(MaiErrorCode::NotConfigured,
                                  "Local decoder context could not be allocated");
        }
        mModelIdentity = modelFile;
        mProjectorIdentity = projectorFile;
        mContextSize = request.contextSize;
        mThreads = request.threads;
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.hasModel = true;
        mStatus.hasProjector = static_cast<bool>(mProjector);
        mStatus.contextSize = request.contextSize;
        ++mStatus.modelGeneration;
        return {};
    }

    MaiError decodeFailure(int status) {
        if (isCanceled(mActiveCancel)) return canceledError();
        if (status < -1) resetResident();
        return {status == 1 ? MaiErrorCode::InvalidInput : MaiErrorCode::Internal,
                status == 1 ? "Local decoder context has insufficient capacity"
                            : "Local decoder evaluation failed"};
    }

    MaiError prefillText(const std::string& prompt, const MaiLlamaRequest& request,
                         MaiLlamaResponse& response) {
        auto tokens = tokenize(mModel.get(), prompt);
        if (!tokens) return tokens.error();
        if (tokens.value().size() + request.maximumTokens > llama_n_ctx(mContext.get()))
            return {MaiErrorCode::InvalidInput,
                    "Prompt and output token budgets exceed the decoder context"};
        response.promptTokens = static_cast<std::uint32_t>(tokens.value().size());
        for (std::size_t offset = 0; offset < tokens.value().size();) {
            if (isCanceled(mActiveCancel)) return canceledError();
            const auto count = static_cast<int32_t>(std::min<std::size_t>(
                llama_n_batch(mContext.get()), tokens.value().size() - offset));
            const int decoded = llama_decode(
                mContext.get(), llama_batch_get_one(tokens.value().data() + offset, count));
            if (decoded != 0) return decodeFailure(decoded);
            offset += static_cast<std::size_t>(count);
        }
        return {};
    }

    MaiError prefillImage(const std::string& prompt, const std::string& bytes,
                          const MaiLlamaRequest& request, MaiLlamaResponse& response) {
        auto decoded = mtmd_helper_bitmap_init_from_buf(
            mProjector.get(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
            false, mtmd_helper_init_opt_default());
        mtmd::bitmap_ptr bitmap(decoded.bitmap);
        mtmd_helper::video_ptr video(decoded.video_ctx);
        if (!bitmap || video || mtmd_bitmap_is_audio(bitmap.get()))
            return {MaiErrorCode::InvalidInput, "Image bitmap could not be decoded"};
        mtmd::input_chunks chunks;
        const mtmd_input_text text{prompt.data(), prompt.size(), true, true};
        const mtmd_bitmap* input = bitmap.get();
        const int tokenized = mtmd_tokenize(mProjector.get(), chunks.ptr.get(), &text, &input, 1);
        if (tokenized != 0)
            return {MaiErrorCode::InvalidInput,
                    tokenized == 1 ? "Image markers do not match the supplied bitmap"
                                   : "Image media preprocessing failed"};
        const std::size_t count = mtmd_helper_get_n_tokens(chunks.ptr.get());
        const llama_pos positions = mtmd_helper_get_n_pos(chunks.ptr.get());
        if (count == 0 || count + request.maximumTokens > llama_n_ctx(mContext.get()) ||
            positions < 0 ||
            static_cast<std::uint64_t>(positions) + request.maximumTokens >
                llama_n_ctx(mContext.get()))
            return {MaiErrorCode::InvalidInput,
                    "Image and output token budgets exceed the decoder context"};
        if (isCanceled(mActiveCancel)) return canceledError();
        for (std::size_t index = 0; index < mtmd_input_chunks_size(chunks.ptr.get()); ++index) {
            const auto* chunk = mtmd_input_chunks_get(chunks.ptr.get(), index);
            if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE &&
                mtmd_decode_use_non_causal(mProjector.get(), chunk) &&
                mtmd_input_chunk_get_n_tokens(chunk) > llama_n_ubatch(mContext.get()))
                return {MaiErrorCode::InvalidInput,
                        "Vision model requires an unsplit image larger than the decoder batch"};
        }
        response.promptTokens = static_cast<std::uint32_t>(count);
        llama_pos next = 0;
        const int status = mtmd_helper_eval_chunks(
            mProjector.get(), mContext.get(), chunks.ptr.get(), 0, 0,
            static_cast<int32_t>(llama_n_batch(mContext.get())), true, &next);
        if (isCanceled(mActiveCancel)) return canceledError();
        return status == 0 ? MaiError{}
                           : MaiError(MaiErrorCode::Internal,
                                      "Image encoding or decoder evaluation failed");
    }

    MaiResult<MaiLlamaResponse> sample(const MaiLlamaRequest& request, MaiLlamaResponse response) {
        llama_sampler_ptr sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()));
        if (request.temperature == 0)
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
        else {
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(request.temperature));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(request.seed));
        }
        const auto* vocabulary = llama_model_get_vocab(mModel.get());
        std::string pending;
        for (std::uint32_t index = 0; index < request.maximumTokens; ++index) {
            if (isCanceled(mActiveCancel)) return canceledError();
            if (!llama_get_logits_ith(mContext.get(), -1))
                return {MaiErrorCode::Internal, "Local decoder returned no sampling logits"};
            const llama_token token = llama_sampler_sample(sampler.get(), mContext.get(), -1);
            if (token < 0 || token >= llama_vocab_n_tokens(vocabulary))
                return {MaiErrorCode::Internal, "Local decoder returned an invalid token"};
            if (llama_vocab_is_eog(vocabulary, token)) {
                response.stopReason = MaiLlamaStopReason::EndOfSequence;
                if (!appendOutput(response.text, pending, {}, request.maximumOutputBytes, true)) {
                    response.stopReason = MaiLlamaStopReason::OutputLimit;
                    response.truncated = true;
                }
                return response;
            }
            ++response.generatedTokens;
            const auto piece = tokenPiece(vocabulary, token);
            if (!piece) return piece.error();
            if (!appendOutput(response.text, pending, piece.value(), request.maximumOutputBytes,
                              false)) {
                response.stopReason = MaiLlamaStopReason::OutputLimit;
                response.truncated = true;
                return response;
            }
            if (index + 1 < request.maximumTokens) {
                llama_token next = token;
                const int decoded = llama_decode(mContext.get(), llama_batch_get_one(&next, 1));
                if (decoded != 0) return decodeFailure(decoded);
            }
        }
        response.truncated = true;
        response.stopReason = MaiLlamaStopReason::TokenLimit;
        if (!appendOutput(response.text, pending, {}, request.maximumOutputBytes, true))
            response.stopReason = MaiLlamaStopReason::OutputLimit;
        return response;
    }

    std::timed_mutex mOperationMutex;
    mutable std::mutex mStatusMutex;
    MaiLlamaStatus mStatus;
    const std::atomic<bool>* mActiveCancel = nullptr;
    MaiLlamaFileIdentity mModelIdentity;
    MaiLlamaFileIdentity mProjectorIdentity;
    std::uint32_t mContextSize = 0;
    std::uint32_t mThreads = 0;
    llama_model_ptr mModel;
    llama_context_ptr mContext;
    mtmd::context_ptr mProjector;
};

MaiLlamaRuntime::MaiLlamaRuntime() : mImplementation(std::make_unique<Implementation>()) {}
MaiLlamaRuntime::~MaiLlamaRuntime() = default;
MaiResult<MaiLlamaResponse> MaiLlamaRuntime::generate(const MaiLlamaRequest& request,
                                                      const std::atomic<bool>* cancel) {
    return mImplementation->generate(request, cancel);
}
MaiLlamaStatus MaiLlamaRuntime::status() const {
    return mImplementation->status();
}
MaiError MaiLlamaRuntime::unload() {
    return mImplementation->unload();
}
std::shared_ptr<MaiLlamaRuntime> getSharedMaiLlamaRuntime() {
    static const auto runtime = std::shared_ptr<MaiLlamaRuntime>(new MaiLlamaRuntime());
    return runtime;
}

namespace {
bool parseUnsigned(const std::string& text, std::uint32_t& value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

class MaiLlamaTool final : public MaiTool {
public:
    explicit MaiLlamaTool(std::shared_ptr<MaiLlamaRuntime> runtime)
        : mRuntime(std::move(runtime)) {}
    std::string name() const override {
        return "llama";
    }
    std::string description() const override {
        // 协议说明：有限参数只映射到普通 CPU API；--status/--unload 是互斥的独立模式。
        // unload 不取消别人的请求；先拒绝已观察到的 busy，检查后的竞态仍可能需要等待。
        // 不接上游 CLI 的下载、网络、标准输入、视频和输出文件能力，避免隐式外部副作用。
        return "Generate bounded local text or analyze one local image using an existing GGUF "
               "model. "
               "Arguments support -m/--model, -p/--prompt, -n/--n-predict, -c/--ctx-size, "
               "-t/--threads, --temp, --seed, --mmproj, --image and --no-conversation. "
               "An image needs a matching local projector. Use [\"--status\"] to inspect "
               "residency, or [\"--unload\"] to release it. Unload rejects an observed busy "
               "runtime; a concurrent request starting just after the check can make it wait. "
               "It never cancels or preempts another request. "
               "This is a supported library-API subset, not the complete upstream CLI. CPU only; "
               "no downloads, networking, stdin, video paths or output files. Input paths must be "
               "accessible.";
    }
    std::string parametersSchema() const override {
        // arguments 必填且仅含字符串；执行层同时拒绝未知根字段、参数、重复别名及越界数值。
        return R"({"type":"object","properties":{"arguments":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":32}},"required":["arguments"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return false;
    }

    MaiToolResult execute(const std::string& encoded, const MaiToolContext& context) override {
        if (encoded.size() > 128 * 1024)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "Local inference arguments are too large");
        const auto parsed = Json::parse(encoded, nullptr, false);
        if (!parsed.is_object() || parsed.size() != 1 || !parsed.contains("arguments") ||
            !parsed["arguments"].is_array() || parsed["arguments"].empty() ||
            parsed["arguments"].size() > 32)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "llama accepts only a bounded arguments array");
        std::vector<std::string> arguments;
        for (const auto& item : parsed["arguments"]) {
            if (!item.is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "Every llama argument must be a string");
            auto value = item.get<std::string>();
            if (value.empty() || value.size() > kMaximumPromptBytes ||
                value.find('\0') != std::string::npos)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "llama arguments must be nonempty bounded strings");
            arguments.push_back(std::move(value));
        }
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Local inference was canceled");
        if (arguments[0] == "--status" || arguments[0] == "--unload") {
            if (arguments.size() != 1)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "--status and --unload must be standalone");
            if (arguments[0] == "--unload") {
                if (mRuntime->status().busy)
                    return MaiToolResult::failure(
                        MaiErrorCode::Busy, "Local inference is busy; unload does not cancel it");
                const auto error = mRuntime->unload();
                if (error) return MaiToolResult::failure(error.code(), error.message());
            }
            const auto state = mRuntime->status();
            return MaiToolResult::success(Json{
                {"busy", state.busy},
                {"has_model", state.hasModel},
                {"has_projector", state.hasProjector},
                {"context_size", state.contextSize},
                {"model_generation",
                 state.modelGeneration}}.dump());
        }
        MaiLlamaRequest request;
        std::set<std::string> seen;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            std::string option = arguments[index];
            if (option == "-m")
                option = "--model";
            else if (option == "-p")
                option = "--prompt";
            else if (option == "-n")
                option = "--n-predict";
            else if (option == "-c")
                option = "--ctx-size";
            else if (option == "-t")
                option = "--threads";
            if (!seen.insert(option).second)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "Duplicate llama options are not supported");
            if (option == "--no-conversation") {
                request.useChatTemplate = false;
                continue;
            }
            if (option != "--model" && option != "--prompt" && option != "--n-predict" &&
                option != "--ctx-size" && option != "--threads" && option != "--temp" &&
                option != "--seed" && option != "--mmproj" && option != "--image")
                return MaiToolResult::failure(
                    MaiErrorCode::NotSupported,
                    "Unsupported llama option; only the documented local subset is available");
            if (++index == arguments.size())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "A llama option is missing its value");
            const std::string& value = arguments[index];
            if (option == "--prompt")
                request.prompt = value;
            else if (option == "--model" || option == "--mmproj" || option == "--image") {
                if (value.find("://") != std::string::npos || value == "-" ||
                    value.rfind("file:", 0) == 0)
                    return MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "Only authorized local inference files are supported");
                const auto path = context.resolvePath(value);
                if (path.empty())
                    return MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "Local inference file is outside the accessible area");
                if (option == "--model")
                    request.modelPath = MaiFilePath::fromUtf8(path);
                else if (option == "--mmproj")
                    request.projectorPath = MaiFilePath::fromUtf8(path);
                else
                    request.imagePath = MaiFilePath::fromUtf8(path);
            } else if (option == "--temp") {
                const auto number = Json::parse(value, nullptr, false);
                if (!number.is_number())
                    return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                                  "Temperature must be a finite number");
                request.temperature = number.get<float>();
            } else {
                std::uint32_t number = 0;
                if (!parseUnsigned(value, number))
                    return MaiToolResult::failure(
                        MaiErrorCode::InvalidInput,
                        "llama numeric options require bounded nonnegative integers");
                if (option == "--n-predict")
                    request.maximumTokens = number;
                else if (option == "--ctx-size")
                    request.contextSize = number;
                else if (option == "--threads")
                    request.threads = number;
                else
                    request.seed = number;
            }
        }
        const auto generated = mRuntime->generate(request, context.cancel);
        if (!generated)
            return MaiToolResult::failure(generated.error().code(), generated.error().message());
        const auto& output = generated.value();
        return MaiToolResult::success(Json{{"text", output.text},
                                           {"prompt_tokens", output.promptTokens},
                                           {"generated_tokens", output.generatedTokens},
                                           {"stop_reason", stopReasonName(output.stopReason)},
                                           {"truncated", output.truncated}}
                                          .dump(),
                                      output.truncated);
    }

private:
    std::shared_ptr<MaiLlamaRuntime> mRuntime;
};
}  // namespace

std::unique_ptr<MaiTool> makeMaiLlamaTool(std::shared_ptr<MaiLlamaRuntime> runtime) {
    return std::make_unique<MaiLlamaTool>(runtime ? std::move(runtime)
                                                  : getSharedMaiLlamaRuntime());
}
void registerMaiLlamaTools(MaiToolRegistry& registry, std::shared_ptr<MaiLlamaRuntime> runtime) {
    registry.add(makeMaiLlamaTool(std::move(runtime)));
}
