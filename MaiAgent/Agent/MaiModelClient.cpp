// 文件作用：提供可核对的官方容量目录和明确标为估计的中立上下文计数。
#include "MaiModelClient.h"

#include <algorithm>
#include <limits>
#include <string_view>

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

}  // namespace

MaiModelContextLimits maiOfficialModelContextLimits(const std::string& baseUrl,
                                                    const std::string& model) {
    std::string endpoint = baseUrl;
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    // 官网 2026-11-10：GLM-5.3 文本页及 GLM-5.3-Flash 视觉页均列 1M/128K。
    // https://docs.bigmodel.cn/cn/guide/models/text/glm-5.3
    // https://docs.bigmodel.cn/cn/guide/models/vlm/glm-5.3-flash
    if ((endpoint == "https://open.bigmodel.cn/api/paas/v4" ||
         endpoint == "https://open.bigmodel.cn/api/v1") &&
        (model == "glm-5.3" || model == "glm-5.3-flash"))
        return {1048576, 131072};
    // 官网 2026-11-10：deepseek-flash 当前别名为 V4.1-Flash-1017，1M/384K。
    // https://api-docs.deepseek.com/quick_start/pricing/
    // https://api-docs.deepseek.com/api/list-models/
    if ((endpoint == "https://api.deepseek.com" || endpoint == "https://api.deepseek.com/v1") &&
        model == "deepseek-flash")
        return {1048576, 393216};
    return {};
}

MaiModelTokenEstimate maiEstimateModelInputTokens(const MaiModelRequest& request) {
    std::size_t tokens = addSaturated(16, estimateText(request.baseInstructions));
    for (const auto& message : request.messages) {
        tokens = addSaturated(tokens, 16);
        tokens = addSaturated(tokens, estimateText(message.content));
        tokens = addSaturated(tokens, estimateText(message.reasoning));
        tokens = addSaturated(tokens, estimateText(message.toolCallId));
        for (const auto& call : message.invocations) {
            tokens = addSaturated(tokens, 16);
            tokens = addSaturated(tokens, estimateText(call.id));
            tokens = addSaturated(tokens, estimateText(call.name));
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
        tokens = addSaturated(tokens, estimateText(tool.parametersJson));
    }
    return {tokens, false};
}

MaiModelContextLimits MaiModelClient::contextLimits(const std::string&) const {
    return {};
}

MaiModelTokenEstimate MaiModelClient::estimateInputTokens(const MaiModelRequest& request) const {
    return maiEstimateModelInputTokens(request);
}
