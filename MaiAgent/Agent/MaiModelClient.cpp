// 文件作用：提供可核对的官方容量目录和明确标为估计的中立上下文计数。
#include "MaiModelClient.h"
#include "MaiCloudModelCatalog.h"

#include <algorithm>
#include <limits>
#include <string_view>

#include <json.hpp>

namespace {

std::size_t addSaturated(std::size_t left, std::size_t right) {
    return right > std::numeric_limits<std::size_t>::max() - left
               ? std::numeric_limits<std::size_t>::max()
               : left + right;
}

// ASCII 连续段按每 3 字符约 1 token 估计；非 ASCII 标量按 UTF-8 编码长度预留
// 2--4 token，覆盖中文/罕见字/emoji 的较高分词成本。它仍不是供应商 tokenizer，
// 不能作为实际 usage。压缩触发另留安全余量，真实 usage 可校正后续请求的估计下界。
std::size_t estimateText(std::string_view source) {
    std::size_t tokens = 0;
    std::size_t ascii = 0;
    for (std::size_t index = 0; index < source.size();) {
        const unsigned char lead = static_cast<unsigned char>(source[index]);
        if (lead < 0x80) {
            ++ascii;
            ++index;
            continue;
        }
        tokens = addSaturated(tokens, (ascii + 2) / 3);
        ascii = 0;
        std::size_t length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        if (index + length > source.size()) length = 1;
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((static_cast<unsigned char>(source[index + offset]) & 0xc0) != 0x80) {
                length = 1;
                break;
            }
        }
        tokens = addSaturated(tokens, length);
        index += length;
    }
    return addSaturated(tokens, (ascii + 2) / 3);
}

std::size_t estimateJsonText(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string()
               ? estimateText(found->get_ref<const std::string&>())
               : 0;
}

// 只估模型读取的内容；签名、ID、外层 JSON 转义和 data URL 编码均不是自然语言 token。
// 媒体沿用普通请求的估计余量，不读取文件，也不把编码字节当作实际视觉用量。
std::size_t estimateResponseContent(const nlohmann::json& content) {
    if (content.is_string()) return estimateText(content.get_ref<const std::string&>());
    if (!content.is_array()) return 0;
    std::size_t tokens = 0;
    for (const auto& part : content) {
        if (!part.is_object()) continue;
        tokens = addSaturated(tokens, estimateJsonText(part, "text"));
        tokens = addSaturated(tokens, estimateJsonText(part, "refusal"));
        const auto type = part.find("type");
        if (type != part.end() && (*type == "input_image" || *type == "output_image"))
            tokens = addSaturated(tokens, 4096);
    }
    return tokens;
}

// 对齐 Codex rust-v0.172.0 history.rs 的加密 reasoning 兜底：base64 长度还原后
// 扣除固定封装开销，再按 4 bytes/token 估计。它不是解密、实际 usage 或精确 tokenizer；
// 真实 usage 覆盖的前缀应由宿主使用其测量基线，不再重复估计密文字节。
// https://github.com/openai/codex/blob/3d86e43b075adeba2193ec5c22ac92f55afd82e6/
// codex-rs/core/src/context_manager/history.rs
std::size_t estimateEncryptedReasoning(std::size_t encodedBytes) {
    const auto decodedBytes = encodedBytes / 4 * 3 + encodedBytes % 4 * 3 / 4;
    const auto contentBytes = decodedBytes > 650 ? decodedBytes - 650 : 0;
    return contentBytes / 4 + (contentBytes % 4 != 0 ? 1 : 0);
}

std::size_t estimateResponseOutput(std::string_view payload) {
    const auto output = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
    // 损坏/未知协议仍保守估计，发送层负责拒绝；计数器不能修复或改写续接状态。
    if (!output.is_array()) return estimateText(payload);
    std::size_t tokens = 0;
    for (const auto& item : output) {
        if (!item.is_object()) return estimateText(payload);
        const auto type = item.find("type");
        if (type == item.end() || !type->is_string()) return estimateText(payload);
        std::size_t itemTokens = 16;
        if (*type == "message") {
            const auto content = item.find("content");
            if (content != item.end())
                itemTokens = addSaturated(itemTokens, estimateResponseContent(*content));
        } else if (*type == "reasoning") {
            const auto encrypted = item.find("encrypted_content");
            if (encrypted != item.end() && encrypted->is_string()) {
                itemTokens = addSaturated(
                    itemTokens,
                    estimateEncryptedReasoning(encrypted->get_ref<const std::string&>().size()));
            } else {
                // 兼容供应商会真实回传签名的明文思考；仅计明文字段，绝不重复计签名或摘要。
                const auto content = item.find("content");
                if (content != item.end())
                    itemTokens = addSaturated(itemTokens, estimateResponseContent(*content));
            }
        } else if (*type == "function_call" || *type == "custom_tool_call") {
            for (const char* key : {"name", "namespace"})
                itemTokens = addSaturated(itemTokens, estimateJsonText(item, key));
            itemTokens = addSaturated(
                itemTokens,
                estimateJsonText(item, *type == "custom_tool_call" ? "input" : "arguments"));
        } else if (*type == "function_call_output" || *type == "custom_tool_call_output") {
            const auto content = item.find("output");
            if (content != item.end())
                itemTokens = addSaturated(itemTokens, estimateResponseContent(*content));
        } else if (*type == "image_generation_call") {
            itemTokens = addSaturated(itemTokens, estimateJsonText(item, "revised_prompt"));
            const auto result = item.find("result");
            if (result != item.end() && result->is_string() &&
                !result->get_ref<const std::string&>().empty())
                itemTokens = addSaturated(itemTokens, 4096);
        } else {
            // 新的模型可见 item 尚无字段契约，不能猜测其内容为空。
            itemTokens = addSaturated(itemTokens, estimateText(item.dump()));
        }
        tokens = addSaturated(tokens, itemTokens);
    }
    return tokens;
}

}  // namespace

const char* maiWireApiToString(MaiWireApi wire) {
    switch (wire) {
        case MaiWireApi::Responses: return "responses";
        case MaiWireApi::Local: return "local";
        case MaiWireApi::Mixed: return "mixed";
    }
    return "unknown";
}

const char* maiUsageValueSourceToString(MaiUsageValueSource source) {
    switch (source) {
        case MaiUsageValueSource::Missing: return "missing";
        case MaiUsageValueSource::Provider: return "provider";
        case MaiUsageValueSource::Derived: return "derived";
    }
    return "missing";
}

MaiModelContextLimits maiOfficialModelContextLimits(const std::string& baseUrl,
                                                    const std::string& model) {
    std::string endpoint = baseUrl;
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    // 官网 2026-11-27：GLM-5.4 与保持不变的 GLM-5.3-Flash 均列 1M/128K。
    // https://docs.bigmodel.cn/cn/guide/models/glm-5.4
    // https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash
    if ((endpoint == "https://open.bigmodel.cn/api/paas/v4" ||
         endpoint == "https://open.bigmodel.cn/api/v1") &&
        (model == "glm-5.4" || model == "glm-5.3-flash"))
        return {1048576, 131072};
    // 官网 2026-11-10：deepseek-flash 当前别名为 V4.1-Flash-1017，1M/384K。
    // https://api-docs.deepseek.com/quick_start/pricing/
    // https://api-docs.deepseek.com/api/list-models/
    if ((endpoint == "https://api.deepseek.com" || endpoint == "https://api.deepseek.com/v1") &&
        (model == "deepseek-flash" || model == "deepseek-v4-pro"))
        return {1048576, 393216};
    if (endpoint == "https://openrouter.ai/api/v1" &&
        (model == "anthropic/claude-fable-5.2" || model == "anthropic/claude-opus-5.5"))
        return {1000000, 128000};
    // https://docs.particle.ai/models；不把收费同名或其它代理地址视为此免费目录容量。
    if (endpoint == "https://openrouter.ai/api/v1" && model == "particle-ai/particle-1:free")
        return {1048576, 131072};
    // 2026-11-25 官方 Sol 目录；代理同名型号和未核对别名都不继承容量或生成预留。
    // https://developers.openai.com/api/docs/models/gpt-6.1-sol
    if (endpoint == "https://api.openai.com/v1" && model == "gpt-6.1-sol") return {1050000, 128000};
    return {};
}

MaiModelTokenEstimate maiEstimateModelInputTokens(const MaiModelRequest& request) {
    const auto capability = maiCloudModelInfo(request.model);
    const bool nativePatch = capability && capability->provider == "openai";
    std::size_t tokens = addSaturated(16, estimateText(request.baseInstructions));
    for (const auto& message : request.messages) {
        tokens = addSaturated(tokens, 16);
        if (message.role == MaiModelRole::Assistant && message.continuation && capability &&
            message.continuation->format == "responses.output.v1" &&
            message.continuation->provider == capability->provider &&
            message.continuation->model == request.model) {
            // wire 原样重放完整输出；预算只估其中模型可见内容，显示投影不再重复计数。
            // 不解析签名、不修改密文，HTTP 413 仍以真实编码请求体字节另行处理。
            tokens = addSaturated(tokens, estimateResponseOutput(message.continuation->payload));
            continue;
        }
        tokens = addSaturated(tokens, estimateText(message.content));
        tokens = addSaturated(tokens, estimateText(message.reasoning));
        tokens = addSaturated(tokens, estimateText(message.toolCallId));
        for (const auto& call : message.invocations) {
            tokens = addSaturated(tokens, 16);
            tokens = addSaturated(tokens, estimateText(call.id));
            tokens = addSaturated(tokens, estimateText(call.name));
            // OpenAI 原生补丁只发送 patch 原文；中立调用的 JSON 包装和转义不属于模型输入。
            const auto arguments = nativePatch && call.name == "apply_patch"
                                       ? nlohmann::json::parse(call.arguments, nullptr, false)
                                       : nlohmann::json();
            if (arguments.is_object() && arguments.size() == 1 && arguments.contains("patch") &&
                arguments["patch"].is_string())
                tokens = addSaturated(tokens, estimateJsonText(arguments, "patch"));
            else
                tokens = addSaturated(tokens, estimateText(call.arguments));
        }
        // 像素分词依赖尺寸和供应商，不读取文件，也不把 base64 大小当作 token。
        // 未知 tokenizer 时为每张保留较高媒体余量；这是估计，不是视觉用量保证。
        for (const auto& image : message.images) {
            tokens = addSaturated(tokens, 4096);
            tokens = addSaturated(tokens, estimateText(image.path));
        }
    }
    for (const auto& tool : request.tools) {
        tokens = addSaturated(tokens, 24);
        tokens = addSaturated(tokens, estimateText(tool.name));
        tokens = addSaturated(tokens, estimateText(tool.description));
        tokens = addSaturated(tokens, estimateText(nativePatch && tool.name == "apply_patch" &&
                                                           !tool.inputGrammar.empty()
                                                       ? tool.inputGrammar
                                                       : tool.parametersJson));
    }
    return {tokens, false};
}

MaiModelContextLimits MaiModelClient::contextLimits(const std::string&) const {
    return {};
}

std::string MaiModelClient::usageProviderForModel(const std::string&) const {
    return {};
}

MaiModelTokenEstimate MaiModelClient::estimateInputTokens(const MaiModelRequest& request) const {
    return maiEstimateModelInputTokens(request);
}

MaiResult<std::size_t> MaiModelClient::requestBodyBytes(const MaiModelRequest&) {
    return std::size_t{0};
}

std::optional<MaiWireApi> MaiModelClient::wireApiForModel(const std::string&) const {
    return wireApi();
}
