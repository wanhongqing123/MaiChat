// 文件作用：以 llama.cpp/mtmd 库 API 提供进程驻留、串行且可取消的本地推理。
#include "MaiLlamaTools.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>
#include <chat.h>
#include <sampling.h>
#include <ggml-cpp.h>
#include <llama-cpp.h>
#include <llama.h>
#include <llama-model.h>
#include <mtmd-helper.h>
#include <mtmd.h>
#include <stb/stb_image.h>

#include "MaiBlockingCheck.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiLlamaChatSupport.h"
#include "MaiLlamaModelCapabilities.h"
#include "MaiLlamaModelClient.h"
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
constexpr std::size_t kMaximumChatBytes = 2 * 1024 * 1024;

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

// 使用中立消息映射上游结构；工具结果必须保留原始调用 ID 和名称，不能摊平成 user 文本。
MaiResult<common_chat_templates_inputs> chatInputs(const MaiModelRequest& request) {
    common_chat_templates_inputs input;
    input.use_jinja = true;
    input.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    // 本地主模型固定为非思考模式，与模板预检和验收配置一致；保留原生 parser 的思考/工具
    // 分流。远端 reasoningEffort 目前不映射到本地模板，不能借同名设置悄悄打开另一种模式。
    input.enable_thinking = false;
    input.parallel_tool_calls = true;
    std::size_t bytes = 0;
    const auto valid = [&](const std::string& value) {
        if (value.size() > kMaximumChatBytes - bytes || value.find('\0') != std::string::npos ||
            !isValidUtf8(value))
            return false;
        bytes += value.size();
        return true;
    };
    const auto appendSystemText = [&](std::string& target, const std::string& source) {
        if (source.empty()) return true;
        if (!target.empty()) {
            if (!valid("\n\n")) return false;
            target += "\n\n";
        }
        target += source;
        return true;
    };
    if (!valid(request.baseInstructions) || request.messages.empty() ||
        request.messages.size() > 4096 || request.tools.size() > 512)
        return {MaiErrorCode::InvalidInput, "Local chat requires bounded UTF-8 messages"};
    if (!request.baseInstructions.empty()) {
        common_chat_msg system;
        system.role = "system";
        system.content = request.baseInstructions;
        input.messages.push_back(std::move(system));
    }
    std::map<std::string, std::string> callNames;
    std::set<std::string> results;
    for (const auto& message : request.messages) {
        if (!message.images.empty())
            return {MaiErrorCode::NotSupported, "Local primary model image input is not supported"};
        if (!valid(message.content) || !valid(message.reasoning))
            return {MaiErrorCode::InvalidInput, "Local chat requires bounded UTF-8 messages"};
        common_chat_msg mapped;
        switch (message.role) {
            case MaiModelRole::System: mapped.role = "system"; break;
            case MaiModelRole::User: mapped.role = "user"; break;
            case MaiModelRole::Assistant: mapped.role = "assistant"; break;
            case MaiModelRole::ToolResult: mapped.role = "tool"; break;
            default: return {MaiErrorCode::InvalidInput, "Local chat message role is invalid"};
        }
        mapped.content = message.content;
        mapped.reasoning_content = message.reasoning;
        if (message.invocations.size() > 256)
            return {MaiErrorCode::InvalidInput, "Local chat history has too many tool calls"};
        for (const auto& call : message.invocations) {
            if (message.role != MaiModelRole::Assistant || call.id.empty() || call.name.empty() ||
                !valid(call.id) || !valid(call.name) || !valid(call.arguments) ||
                !Json::parse(call.arguments, nullptr, false).is_object() ||
                !callNames.emplace(call.id, call.name).second)
                return {MaiErrorCode::InvalidInput, "Local chat history has an invalid tool call"};
            mapped.tool_calls.push_back({call.name, call.arguments, call.id});
        }
        if (message.role == MaiModelRole::ToolResult) {
            const auto found = callNames.find(message.toolCallId);
            if (found == callNames.end() || !results.insert(message.toolCallId).second)
                return {MaiErrorCode::InvalidInput, "Local chat tool result has no matching call"};
            mapped.tool_call_id = message.toolCallId;
            mapped.tool_name = found->second;
        }
        // 严格模板只允许首条 System；基础指令、历史裁剪说明和本轮收尾指令仍同属可信
        // System，按原先顺序并入同一首条消息。其余角色及调用/回执的顺序和内容完全保留，
        // 不能为修复模板渲染而丢历史工具、补造声明或把工具结果改写成新用户请求。
        if (mapped.role == "system") {
            if (input.messages.empty() || input.messages.front().role != "system") {
                input.messages.insert(input.messages.begin(), std::move(mapped));
            } else if (!appendSystemText(input.messages.front().content, mapped.content) ||
                       !appendSystemText(input.messages.front().reasoning_content,
                                         mapped.reasoning_content)) {
                return {MaiErrorCode::InvalidInput, "Local chat requires bounded UTF-8 messages"};
            }
        } else {
            input.messages.push_back(std::move(mapped));
        }
    }
    std::set<std::string> names;
    for (const auto& tool : request.tools) {
        if (tool.name.empty() || !valid(tool.name) || !valid(tool.description) ||
            !valid(tool.parametersJson) || !names.insert(tool.name).second ||
            !Json::parse(tool.parametersJson, nullptr, false).is_object())
            return {MaiErrorCode::InvalidInput, "Local chat tool definitions are invalid"};
        input.tools.push_back({tool.name, tool.description, tool.parametersJson});
    }
    return input;
}

// 仅保留上游采样器的工具语法部分；不引入 common_sampler 的 CLI、下载或进程配置。
llama_sampler_ptr chatSampler(const llama_vocab* vocabulary, common_params_sampling& parameters,
                              float temperature, std::uint32_t seed) {
    llama_sampler_ptr chain(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    const auto& grammar = parameters.grammar.grammar;
    if (!grammar.empty()) {
        std::vector<std::string> patterns;
        std::vector<llama_token> tokens;
        for (const auto& trigger : parameters.grammar_triggers) {
            switch (trigger.type) {
                case COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN: tokens.push_back(trigger.token); break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_WORD:
                    patterns.push_back(escapeMaiLlamaGrammarWord(trigger.value));
                    break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN: patterns.push_back(trigger.value); break;
                case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL:
                    patterns.push_back(
                        (trigger.value.empty() || trigger.value.front() != '^' ? "^" : "") +
                        trigger.value +
                        (trigger.value.empty() || trigger.value.back() != '$' ? "$" : ""));
                    break;
            }
        }
        std::vector<const char*> expressions;
        for (const auto& pattern : patterns) expressions.push_back(pattern.c_str());
        llama_sampler* sampler =
            parameters.grammar_lazy
                ? llama_sampler_init_grammar_lazy_patterns(vocabulary, grammar.c_str(), "root",
                                                           expressions.data(), expressions.size(),
                                                           tokens.data(), tokens.size())
                : llama_sampler_init_grammar(vocabulary, grammar.c_str(), "root");
        if (!sampler) throw std::runtime_error("Local tool grammar initialization failed");
        llama_sampler_ptr owned(sampler);
        if (!parameters.grammar_lazy && common_grammar_needs_prefill(parameters.grammar)) {
            const auto prefill =
                common_tokenize(vocabulary, parameters.generation_prompt, false, true);
            for (std::size_t index = 0; index < prefill.size(); ++index) {
                const auto piece = common_token_to_piece(vocabulary, prefill[index], true);
                // 某些 tokenizer 自动在首个特殊 token 前增加空格；与上游 common_sampler 一致。
                if (index == 0 && !piece.empty() && !parameters.generation_prompt.empty() &&
                    std::isspace(static_cast<unsigned char>(piece.front())) &&
                    !std::isspace(static_cast<unsigned char>(parameters.generation_prompt.front())))
                    continue;
                llama_sampler_accept(sampler, prefill[index]);
            }
        }
        llama_sampler_chain_add(chain.get(), owned.release());
    }
    if (temperature == 0) {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(seed));
    }
    return chain;
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
                mContextSize != request.contextSize || mThreads != request.threads ||
                mPreferHardwareAcceleration) {
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

    MaiError streamChat(const MaiLlamaModelConfig& config, const MaiModelRequest& request,
                        const MaiStreamSink& sink, const std::atomic<bool>& cancel) {
        maiAssertBlockingAllowed("MaiLlamaRuntime::streamChat");
        if (isCanceled(&cancel)) return canceledError();
        if (!config.projectorPath.isEmpty())
            return {MaiErrorCode::NotSupported, "Local primary model vision is not supported"};
        if (config.contextSize < 128 || config.contextSize > kMaximumContext ||
            config.maximumTokens >= config.contextSize || config.threads < 1 ||
            config.threads > 64 || !std::isfinite(config.temperature) || config.temperature < 0 ||
            config.temperature > 2 || config.maximumOutputBytes < 1 ||
            config.maximumOutputBytes > 4 * 1024 * 1024)
            return {MaiErrorCode::InvalidInput,
                    "Local primary model inference limits are out of range"};
        const bool automaticTokens = config.maximumTokens == 0;
        MaiLlamaRequest generation;
        generation.modelPath = config.modelPath;
        generation.prompt = "chat";
        generation.contextSize = config.contextSize;
        generation.maximumTokens = automaticTokens ? 1 : config.maximumTokens;
        generation.threads = config.threads;
        generation.temperature = config.temperature;
        generation.seed = config.seed;
        generation.maximumOutputBytes = config.maximumOutputBytes;
        if (!std::isfinite(request.temperature) || request.temperature > 2)
            return {MaiErrorCode::InvalidInput, "Local chat temperature is invalid"};
        if (request.temperature >= 0)
            generation.temperature = static_cast<float>(request.temperature);
        const auto input = chatInputs(request);
        if (!input) return input.error();
        std::unique_lock<std::timed_mutex> operation(mOperationMutex, std::defer_lock);
        while (!operation.try_lock_for(std::chrono::milliseconds(10)))
            if (isCanceled(&cancel)) return canceledError();
        if (isCanceled(&cancel)) return canceledError();
        RequestScope scope(*this);
        mActiveCancel = &cancel;
        try {
            const auto identity = inspectFile(config.modelPath, true);
            if (!identity) return identity.error();
            const bool needsLoad = !mContext || !sameIdentity(mModelIdentity, identity.value()) ||
                                   mProjector || mContextSize != config.contextSize ||
                                   mThreads != config.threads ||
                                   mPreferHardwareAcceleration != config.preferHardwareAcceleration;
            if (needsLoad || !mChatTemplates) {
                // 普通入口与宿主配置复用同一预检；不能通过先运行 llama 工具绕过主模型门禁。
                const auto checked = validateMaiLlamaModelConfig(config);
                if (checked) return checked;
            }
            if (needsLoad) {
                const auto loaded =
                    load(generation, identity.value(), {}, config.preferHardwareAcceleration);
                if (loaded) return loaded;
            }
            if (!mChatTemplates) {
                // 上游允许缺模板时默认为 ChatML；主模型不能把原始续写权重伪装为聊天模型。
                const char* ordinary = llama_model_chat_template(mModel.get(), nullptr);
                const char* tools = llama_model_chat_template(mModel.get(), "tool_use");
                if ((!ordinary || !*ordinary) && (!tools || !*tools))
                    return {MaiErrorCode::NotSupported,
                            "Local primary model requires an explicit GGUF chat template"};
                try {
                    mChatTemplates = common_chat_templates_init(mModel.get(), {});
                } catch (...) {
                    return {MaiErrorCode::NotSupported,
                            "Local model chat template cannot be initialized"};
                }
            }
            if (!input.value().tools.empty()) {
                const auto caps = common_chat_templates_get_caps(mChatTemplates.get());
                if (!caps.at("supports_tools").get<bool>() ||
                    !caps.at("supports_tool_calls").get<bool>())
                    return {MaiErrorCode::NotSupported,
                            "Local model template does not support native tool calls"};
            }
            const auto* vocabulary = llama_model_get_vocab(mModel.get());
            std::unique_ptr<common_chat_session> chat;
            try {
                chat = std::make_unique<common_chat_session>(mChatTemplates.get(), vocabulary,
                                                             input.value());
            } catch (...) {
                return {MaiErrorCode::NotSupported,
                        "Local model cannot render the complete chat and tool history"};
            }
            if (!input.value().tools.empty() &&
                (chat->parser().empty() || chat->format() == COMMON_CHAT_FORMAT_CONTENT_ONLY))
                return {MaiErrorCode::NotSupported, "Local model tool parser is unavailable"};
            if (isCanceled(&cancel)) return canceledError();
            std::vector<llama_token> tokens;
            if (llama_vocab_get_add_bos(vocabulary)) tokens.push_back(llama_vocab_bos(vocabulary));
            std::size_t promptBytes = 0;
            for (const auto& part : chat->prompt().parts) {
                if (part.text.size() > kMaximumChatBytes - promptBytes)
                    return {MaiErrorCode::InvalidInput,
                            "Formatted local chat exceeds the byte budget"};
                promptBytes += part.text.size();
                // Jinja 区分模板控制文本与用户/工具数据；数据中的特殊 token 不能变成角色边界。
                const auto encoded = common_tokenize(vocabulary, part.text, false, !part.is_input);
                tokens.insert(tokens.end(), encoded.begin(), encoded.end());
                if (tokens.size() + generation.maximumTokens > generation.contextSize)
                    return {MaiErrorCode::InvalidInput,
                            "Local chat needs at least " +
                                std::to_string(tokens.size() + generation.maximumTokens) +
                                " tokens including " + std::to_string(generation.maximumTokens) +
                                " output tokens; configured context is " +
                                std::to_string(generation.contextSize) +
                                ". Increase context or shorten the conversation."};
            }
            if (llama_vocab_get_add_eos(vocabulary)) tokens.push_back(llama_vocab_eos(vocabulary));
            if (automaticTokens) {
                if (tokens.size() >= generation.contextSize)
                    return {MaiErrorCode::InvalidInput,
                            "Complete local chat has no remaining context for a response"};
                generation.maximumTokens =
                    generation.contextSize - static_cast<std::uint32_t>(tokens.size());
            }
            MaiLlamaResponse bookkeeping;
            const auto evaluated = prefillTokens(tokens, generation, bookkeeping);
            if (evaluated) return evaluated;
            return sampleChat(generation, request, *chat, sink, automaticTokens);
        } catch (...) {
            resetResident();
            return isCanceled(&cancel)
                       ? canceledError()
                       : MaiError(MaiErrorCode::Internal, "Local chat inference failed");
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
            std::uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(mOwner.mStatusMutex);
                mOwner.mStatus.busy = false;
                if (mOwner.mStatus.hasModel) generation = mOwner.mStatus.modelGeneration;
            }
            if (generation && generation != mOwner.mExitCleanupGeneration) {
                // Metal 的静态设备清理要求权重资源已释放。首次实际使用新驻留配置后才
                // 登记，使最新清理先于本配置初始化的后端静态对象；旧处理器幂等清空。
                // 所有者本身不参与静态析构，避免 CPU registry 先析构的另一种退出崩溃。
                if (std::atexit([] { getSharedMaiLlamaRuntime()->unload(); }) == 0)
                    mOwner.mExitCleanupGeneration = generation;
                else
                    mOwner.resetResident();
            }
        }
        Implementation& mOwner;
    };

    void resetResident() {
        // model_free 与 mtmd_free 均不释放 llama_context；后两者还引用 model。
        // 必须先销毁投影器、decoder 及其 KV/scheduler，再释放权重。
        mChatTemplates.reset();
        mProjector.reset();
        mContext.reset();
        mModel.reset();
        mModelIdentity = {};
        mProjectorIdentity = {};
        mContextSize = 0;
        mThreads = 0;
        mPreferHardwareAcceleration = false;
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.hasModel = false;
        mStatus.hasProjector = false;
        mStatus.contextSize = 0;
        mStatus.gpuSelected = false;
        mStatus.gpuModelBytes = 0;
        mStatus.gpuBackend.clear();
    }

    MaiError load(const MaiLlamaRequest& request, const MaiLlamaFileIdentity& modelFile,
                  const MaiLlamaFileIdentity& projectorFile,
                  bool preferHardwareAcceleration = false) {
        resetResident();
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            // 固定日志接收器避免上游元数据和路径进入宿主 stderr 或工具结果。
            // 仓库静态后端自行注册，不扫描任意插件目录。
            llama_log_set(quietLog, nullptr);
            mtmd_helper_log_set(quietLog, nullptr);
            llama_backend_init();
        });
        auto modelParameters = llama_model_default_params();
        modelParameters.n_gpu_layers = 0;
        ggml_backend_dev_t selectedDevices[] = {nullptr, nullptr};
        // nullptr 表示上游自动枚举 GPU，不能用于纯 CPU 回退；显式空列表才禁止 GPU 后端。
        modelParameters.devices = selectedDevices;
        bool useGpu = false;
#if defined(__APPLE__)
        if (preferHardwareAcceleration) {
            for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
                auto* device = ggml_backend_dev_get(index);
                const auto type = ggml_backend_dev_type(device);
                const auto registry = ggml_backend_dev_backend_reg(device);
                if ((type == GGML_BACKEND_DEVICE_TYPE_GPU ||
                     type == GGML_BACKEND_DEVICE_TYPE_IGPU) &&
                    registry && registry == ggml_backend_reg_by_name("MTL") &&
                    supportsMaiLlamaModelOperations(
                        modelFile.path, request.contextSize, [device](const ggml_tensor* op) {
                            return ggml_backend_dev_supports_op(device, op);
                        })) {
                    selectedDevices[0] = device;
                    modelParameters.n_gpu_layers = -1;
                    useGpu = true;
                    break;
                }
            }
        }
#endif
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
        // 独立 generate 保持固定 CPU；主模型的长上下文注意力可随完整权重层走 Metal。
        // Flash Attention 仍用上游 AUTO 探测；GPU 的取消边界是批次完成/同步之后。
        contextParameters.offload_kqv = useGpu;
        contextParameters.op_offload = useGpu;
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
        mPreferHardwareAcceleration = preferHardwareAcceleration;
        std::uint64_t gpuBytes = 0;
        std::string gpuBackend;
        for (const auto& allocation : mModel->memory_breakdown()) {
            const auto device = ggml_backend_buft_get_device(allocation.first);
            if (!device) continue;
            const auto type = ggml_backend_dev_type(device);
            if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU)
                continue;
            gpuBytes += allocation.second;
            const auto registry = ggml_backend_dev_backend_reg(device);
            if (registry) gpuBackend = ggml_backend_reg_name(registry);
        }
        std::lock_guard<std::mutex> lock(mStatusMutex);
        mStatus.hasModel = true;
        mStatus.hasProjector = static_cast<bool>(mProjector);
        mStatus.contextSize = request.contextSize;
        mStatus.gpuSelected = gpuBytes != 0;
        mStatus.gpuModelBytes = gpuBytes;
        mStatus.gpuBackend = gpuBytes ? std::move(gpuBackend) : std::string{};
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
        return prefillTokens(tokens.value(), request, response);
    }

    MaiError prefillTokens(const std::vector<llama_token>& tokens, const MaiLlamaRequest& request,
                           MaiLlamaResponse& response) {
        if (tokens.empty() || tokens.size() + request.maximumTokens > llama_n_ctx(mContext.get()))
            return {MaiErrorCode::InvalidInput,
                    "Local inference needs " +
                        std::to_string(tokens.size() + request.maximumTokens) +
                        " tokens including output; configured context is " +
                        std::to_string(llama_n_ctx(mContext.get())) +
                        ". Increase context or shorten the conversation."};
        response.promptTokens = static_cast<std::uint32_t>(tokens.size());
        for (std::size_t offset = 0; offset < tokens.size();) {
            if (isCanceled(mActiveCancel)) return canceledError();
            const auto count = static_cast<int32_t>(
                std::min<std::size_t>(llama_n_batch(mContext.get()), tokens.size() - offset));
            const int decoded = llama_decode(
                mContext.get(),
                llama_batch_get_one(const_cast<llama_token*>(tokens.data() + offset), count));
            if (decoded != 0) return decodeFailure(decoded);
            // Metal 无公开的 kernel 抢占 abort；等待当前有界批次结束再报告取消。
            llama_synchronize(mContext.get());
            if (isCanceled(mActiveCancel)) return canceledError();
            offset += static_cast<std::size_t>(count);
        }
        return {};
    }

    MaiError sampleChat(const MaiLlamaRequest& generation, const MaiModelRequest& request,
                        common_chat_session& chat, const MaiStreamSink& sink,
                        bool automaticTokens) {
        const auto* vocabulary = llama_model_get_vocab(mModel.get());
        common_params_sampling parameters;
        chat.apply_sampling(parameters);
        auto sampler = chatSampler(vocabulary, parameters, generation.temperature, generation.seed);
        common_chat_msg previous;
        previous.role = "assistant";
        common_chat_input pending;
        std::size_t producedBytes = 0;
        std::size_t stopHold = 0;
        for (const auto& stop : chat.additional_stops()) {
            if (stop.empty() || stop.size() > 4096)
                return {MaiErrorCode::NotSupported, "Local model stop sequence is unsupported"};
            stopHold = std::max(stopHold, stop.size() - 1);
        }
        const auto emit = [&](const common_chat_msg& message) {
            const auto differences = common_chat_msg_diff::compute_diffs(previous, message);
            for (const auto& difference : differences) {
                if (!difference.content_delta.empty() && sink.onText)
                    sink.onText(difference.content_delta);
                if (!difference.reasoning_content_delta.empty() && sink.onReasoning)
                    sink.onReasoning(difference.reasoning_content_delta);
            }
            previous = message;
        };
        const auto finish = [&](const common_chat_input& final) -> MaiError {
            try {
                // 整个响应先严格完成解析和调用校验，再一次性交付调用，防止半批工具执行。
                const auto& message = chat.finish(final);
                std::vector<MaiToolInvocation> calls;
                std::set<std::string> ids;
                for (const auto& call : message.tool_calls) {
                    const auto known =
                        std::find_if(request.tools.begin(), request.tools.end(),
                                     [&](const auto& tool) { return tool.name == call.name; });
                    if (known == request.tools.end() ||
                        !Json::parse(call.arguments, nullptr, false).is_object())
                        return {MaiErrorCode::Protocol,
                                "Local model returned an invalid tool call"};
                    std::string id = call.id.empty() ? MaiIdGenerator::generate("call") : call.id;
                    if (!ids.insert(id).second)
                        return {MaiErrorCode::Protocol,
                                "Local model returned duplicate tool call IDs"};
                    calls.push_back({std::move(id), call.name, call.arguments});
                }
                emit(message);
                if (isCanceled(mActiveCancel)) return canceledError();
                for (const auto& call : calls)
                    if (sink.onToolCall) sink.onToolCall(call);
                return isCanceled(mActiveCancel) ? canceledError() : MaiError{};
            } catch (...) {
                return {MaiErrorCode::Protocol,
                        "Local model response does not match its chat template"};
            }
        };
        for (std::uint32_t index = 0; index < generation.maximumTokens; ++index) {
            if (isCanceled(mActiveCancel)) return canceledError();
            if (!llama_get_logits_ith(mContext.get(), -1))
                return {MaiErrorCode::Internal, "Local decoder returned no sampling logits"};
            const auto token = llama_sampler_sample(sampler.get(), mContext.get(), -1);
            if (token < 0 || token >= llama_vocab_n_tokens(vocabulary))
                return {MaiErrorCode::Internal, "Local decoder returned an invalid token"};
            if (llama_vocab_is_eog(vocabulary, token)) {
                if (!isValidUtf8(pending.text))
                    return {MaiErrorCode::Protocol, "Local model ended inside a UTF-8 character"};
                return finish(pending);
            }
            // 工具/思考控制 token 必须交给原生 parser，而不能先过滤或直接显示给用户。
            const auto piece = common_token_to_piece(vocabulary, token, true);
            if (piece.size() > generation.maximumOutputBytes - producedBytes)
                return {MaiErrorCode::Protocol, "Local model reached its output byte limit"};
            producedBytes += piece.size();
            pending.append(piece, token);
            std::size_t stopAt = std::string::npos;
            for (const auto& stop : chat.additional_stops())
                stopAt = std::min(stopAt, pending.text.find(stop));
            if (stopAt != std::string::npos) {
                const auto final = pending.substr(0, stopAt);
                if (!isValidUtf8(final.text))
                    return {MaiErrorCode::Protocol, "Local model stop split a UTF-8 character"};
                return finish(final);
            }
            const auto available = pending.size() > stopHold ? pending.size() - stopHold : 0;
            std::size_t complete = 0;
            while (complete < available) {
                const auto width = utf8Width(pending.text, complete);
                if (width == 0)
                    return {MaiErrorCode::Protocol, "Local model produced invalid UTF-8"};
                if (width < 0 || complete + static_cast<std::size_t>(width) > available) break;
                complete += static_cast<std::size_t>(width);
            }
            if (complete) {
                try {
                    emit(chat.feed(pending.substr(0, complete)));
                } catch (...) {
                    return {MaiErrorCode::Protocol,
                            "Local model stream does not match its chat template"};
                }
                pending = pending.substr(complete);
            }
            if (index + 1 < generation.maximumTokens) {
                llama_token next = token;
                const int decoded = llama_decode(mContext.get(), llama_batch_get_one(&next, 1));
                if (decoded) return decodeFailure(decoded);
            }
        }
        // 预算耗尽时即使已看见完整 JSON 也不能猜测模型结束，更不能运行工具。
        return {automaticTokens ? MaiErrorCode::InvalidInput : MaiErrorCode::Protocol,
                automaticTokens
                    ? "Local model reached context capacity before ending its response. Start a "
                      "new conversation or choose a model with a larger context."
                    : "Local model reached its explicit output token limit"};
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
    bool mPreferHardwareAcceleration = false;
    std::uint64_t mExitCleanupGeneration = 0;
    llama_model_ptr mModel;
    llama_context_ptr mContext;
    mtmd::context_ptr mProjector;
    common_chat_templates_ptr mChatTemplates;
};

MaiLlamaRuntime::MaiLlamaRuntime() : mImplementation(std::make_unique<Implementation>()) {}
MaiLlamaRuntime::~MaiLlamaRuntime() = default;
MaiResult<MaiLlamaResponse> MaiLlamaRuntime::generate(const MaiLlamaRequest& request,
                                                      const std::atomic<bool>* cancel) {
    return mImplementation->generate(request, cancel);
}
MaiError MaiLlamaRuntime::streamChat(const MaiLlamaModelConfig& config,
                                     const MaiModelRequest& request, const MaiStreamSink& sink,
                                     const std::atomic<bool>& cancel) {
    return mImplementation->streamChat(config, request, sink, cancel);
}
MaiLlamaStatus MaiLlamaRuntime::status() const {
    return mImplementation->status();
}
MaiError MaiLlamaRuntime::unload() {
    return mImplementation->unload();
}
std::shared_ptr<MaiLlamaRuntime> getSharedMaiLlamaRuntime() {
    // 参照 Chromium NoDestructor 的进程级所有者。后端 registry 在首次加载权重时才创建，
    // 普通静态 shared_ptr 会晚于它析构，导致 model_free 调用已经销毁的后端函数表。
    // 显式 unload 仍正常释放权重；RequestScope 在后端就绪后登记有序退出清理。
    static const auto* runtime = new std::shared_ptr<MaiLlamaRuntime>(new MaiLlamaRuntime());
    return *runtime;
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
                {"gpu_selected", state.gpuSelected},
                {"gpu_model_bytes", state.gpuModelBytes},
                {"gpu_backend", state.gpuBackend},
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
