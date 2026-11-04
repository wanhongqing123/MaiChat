// 文件作用：用有界字节游标预检 GGUF 结构和模型自带的聊天/工具模板，不装载张量权重。
#include "MaiLlamaModelValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <chat.h>
#include <ggml.h>
#include <openssl/evp.h>

#include "MaiFileSystem.h"
#include "MaiLlamaModelClient.h"

namespace {
constexpr std::uint64_t kMaximumPrefixBytes = 64 * 1024 * 1024;
constexpr std::uint64_t kMaximumModelBytes = 32ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaximumMetadata = 4096;
constexpr std::uint64_t kMaximumTensors = 100000;
constexpr std::uint64_t kMaximumArrayItems = 1000000;
constexpr std::uint64_t kMaximumStringBytes = 1024 * 1024;

// 只执行已审阅的实际模型模板，不能把输入大小限制当成 Jinja 执行预算。
// Qwen2.5 原作者权重：https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/tree/
// 9217f5db79a29953eb74d5343926648285ec7e67；q4_k_m.gguf 中完整模板为 2509 字节，
// SHA256 d5495a1e5db0611132a97e46a65dbb64a642a499421228b9c8b93229097fa9a4。
constexpr std::array<unsigned char, 32> kQwen25TemplateDigest{
    0xd5, 0x49, 0x5a, 0x1e, 0x5d, 0xb0, 0x61, 0x11, 0x32, 0xa9, 0x7e, 0x46, 0xa6, 0x5d, 0xbb, 0x64,
    0xa6, 0x42, 0xa4, 0x99, 0x42, 0x12, 0x28, 0xb9, 0xc8, 0xb9, 0x32, 0x29, 0x09, 0x7f, 0xa9, 0xa4};

// Qwen/Qwen3.5-2B revision 15852e8c16360a2fea060d615a32b45270f8a8fc 的完整原始模板，
// 共 7755 字节；bartowski GGUF revision 7d26695454df6de5fbcce2e58681e62dae06ce43
// 保留同一原文。这里只核准已审阅的执行/解析契约，不代表候选权重质量或设备验收通过。
constexpr std::array<unsigned char, 32> kQwen35TemplateDigest{
    0x27, 0x3d, 0x8e, 0x0e, 0x68, 0x3b, 0x88, 0x50, 0x71, 0xfb, 0x17, 0xe0, 0x8d, 0x71, 0xe5, 0xf2,
    0xa5, 0xdd, 0xfb, 0x53, 0x09, 0x75, 0x61, 0x81, 0x68, 0x1d, 0xe4, 0xf5, 0xa1, 0x82, 0x2d, 0x80};

MaiError validateApprovedTemplate(const std::string& source) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(source.data(), source.size(), digest.data(), &length, EVP_sha256(), nullptr) !=
        1)
        return {MaiErrorCode::Internal, "Local chat template verification is unavailable."};
    const bool qwen25 =
        source.size() == 2509 && length == kQwen25TemplateDigest.size() &&
        std::equal(kQwen25TemplateDigest.begin(), kQwen25TemplateDigest.end(), digest.begin());
    const bool qwen35 =
        source.size() == 7755 && length == kQwen35TemplateDigest.size() &&
        std::equal(kQwen35TemplateDigest.begin(), kQwen35TemplateDigest.end(), digest.begin());
    if (!qwen25 && !qwen35)
        return {MaiErrorCode::NotSupported,
                "This local Agent version requires a verified Qwen2.5 or Qwen3.5 tool template."};
    return {};
}

// 参照 Chromium SpanReader：先比较 remaining，再消费前缀；不以 offset+length 比较，
// 避免不可信 GGUF 中的 uint64 长度先发生回绕。游标失败时不分配声明的长度。
class MaiGgufPrefixReader {
public:
    explicit MaiGgufPrefixReader(std::string_view bytes, std::size_t position = 0)
        : mBytes(bytes), mPosition(position) {}

    bool readUnsigned(std::size_t width, std::uint64_t& value) {
        if (width > remaining()) return false;
        std::uint64_t parsed = 0;
        for (std::size_t index = 0; index < width; ++index)
            parsed |=
                static_cast<std::uint64_t>(static_cast<unsigned char>(mBytes[mPosition + index]))
                << (8 * index);
        mPosition += width;
        value = parsed;
        return true;
    }

    bool readString(std::string_view& value, std::uint64_t maximum = kMaximumStringBytes) {
        std::uint64_t length = 0;
        if (!readUnsigned(8, length) || length > maximum || length > remaining()) return false;
        value = mBytes.substr(mPosition, static_cast<std::size_t>(length));
        mPosition += static_cast<std::size_t>(length);
        return true;
    }

    bool skip(std::uint64_t count) {
        if (count > remaining()) return false;
        mPosition += static_cast<std::size_t>(count);
        return true;
    }

    std::size_t position() const {
        return mPosition;
    }

    std::size_t remaining() const {
        return mPosition <= mBytes.size() ? mBytes.size() - mPosition : 0;
    }

private:
    std::string_view mBytes;
    std::size_t mPosition;
};

std::size_t scalarWidth(std::uint64_t type) {
    switch (type) {
        case 0:
        case 1:
        case 7: return 1;
        case 2:
        case 3: return 2;
        case 4:
        case 5:
        case 6: return 4;
        case 10:
        case 11:
        case 12: return 8;
        default: return 0;
    }
}

// GGUF 数组不接受数组元素；数值数组整体跳过，字符串数组逐项受长度和前缀预算限制。
bool skipValue(MaiGgufPrefixReader& reader, std::uint64_t type) {
    if (type == 8) {
        std::string_view unused;
        return reader.readString(unused);
    }
    if (type == 9) {
        std::uint64_t elementType = 0;
        std::uint64_t count = 0;
        if (!reader.readUnsigned(4, elementType) || !reader.readUnsigned(8, count) ||
            count > kMaximumArrayItems)
            return false;
        if (elementType == 8) {
            for (std::uint64_t index = 0; index < count; ++index)
                if (!skipValue(reader, elementType)) return false;
            return true;
        }
        const auto width = scalarWidth(elementType);
        return width != 0 && count <= reader.remaining() / width && reader.skip(count * width);
    }
    const auto width = scalarWidth(type);
    return width != 0 && reader.skip(width);
}

struct MaiGgufTemplateMetadata {
    std::string primary;
    std::string toolUse;
    std::uint64_t alignment = 32;
};

struct MaiGgufTensorRange {
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

bool readMetadata(MaiGgufPrefixReader& reader, std::uint64_t count,
                  MaiGgufTemplateMetadata& templates) {
    std::set<std::string> keys;
    bool hasArchitecture = false;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string_view key;
        std::uint64_t type = 0;
        if (!reader.readString(key, 1024) || key.empty() || key.find('\0') != key.npos ||
            !keys.emplace(key).second || !reader.readUnsigned(4, type))
            return false;
        if (key == "tokenizer.chat_template" || key == "tokenizer.chat_template.tool_use") {
            std::string_view source;
            if (type != 8 || !reader.readString(source) || source.find('\0') != source.npos)
                return false;
            (key == "tokenizer.chat_template" ? templates.primary : templates.toolUse)
                .assign(source);
        } else if (key == "general.alignment") {
            if (type != 4 || !reader.readUnsigned(4, templates.alignment) ||
                templates.alignment == 0 || templates.alignment > 4096 ||
                (templates.alignment & (templates.alignment - 1)) != 0)
                return false;
        } else if (key == "general.architecture") {
            std::string_view architecture;
            if (type != 8 || !reader.readString(architecture, 256) || architecture.empty() ||
                architecture.find('\0') != architecture.npos)
                return false;
            hasArchitecture = true;
        } else if (!skipValue(reader, type)) {
            return false;
        }
    }
    return hasArchitecture;
}

bool readTensorRanges(MaiGgufPrefixReader& reader, std::uint64_t count,
                      std::vector<MaiGgufTensorRange>& ranges) {
    std::set<std::string> names;
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string_view name;
        std::uint64_t dimensions = 0;
        if (!reader.readString(name, GGML_MAX_NAME - 1) || name.empty() ||
            name.find('\0') != name.npos || !names.emplace(name).second ||
            !reader.readUnsigned(4, dimensions) || dimensions == 0 || dimensions > GGML_MAX_DIMS)
            return false;
        std::uint64_t elements = 1;
        std::uint64_t rowElements = 0;
        for (std::uint64_t dimension = 0; dimension < dimensions; ++dimension) {
            std::uint64_t size = 0;
            if (!reader.readUnsigned(8, size) || size == 0 ||
                size >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / elements)
                return false;
            elements *= size;
            if (dimension == 0) rowElements = size;
        }
        std::uint64_t type = 0;
        std::uint64_t offset = 0;
        if (!reader.readUnsigned(4, type) || type >= GGML_TYPE_COUNT ||
            !reader.readUnsigned(8, offset))
            return false;
        const auto blockSize = ggml_blck_size(static_cast<ggml_type>(type));
        const auto typeBytes = ggml_type_size(static_cast<ggml_type>(type));
        if (blockSize <= 0 || typeBytes == 0 || rowElements % blockSize != 0 ||
            elements / blockSize > std::numeric_limits<std::uint64_t>::max() / typeBytes)
            return false;
        ranges.push_back({offset, (elements / blockSize) * typeBytes});
    }
    return true;
}

MaiError invalidGguf() {
    return {MaiErrorCode::InvalidInput,
            "Local GGUF structure is invalid, truncated, or exceeds the mobile validation limits."};
}

// 只使用文件里的完整模板；不给空模板或 chatml 别名进入上游默认模板分支。
// 以固定的非用户工具及完整调用/结果历史构造真正的上游解析会话，不能靠查找标签猜兼容。
MaiError validateNativeTemplate(const std::string& source, bool requireTools) {
    if (source.empty() || source == "chatml")
        return {MaiErrorCode::NotSupported,
                "Local primary models require an explicit native chat and tool template."};
    const auto approved = validateApprovedTemplate(source);
    if (approved) return approved;
    try {
        const auto templates = common_chat_templates_init(nullptr, source, "<s>", "</s>");
        if (!templates || !common_chat_templates_was_explicit(templates.get()))
            return {MaiErrorCode::NotSupported, "The native local chat template is unavailable."};
        common_chat_templates_inputs inputs;
        inputs.enable_thinking = false;
        common_chat_msg user;
        user.role = "user";
        user.content = "Read the validation marker.";
        inputs.messages.push_back(std::move(user));
        if (requireTools) {
            const auto capabilities = common_chat_templates_get_caps(templates.get());
            if (!capabilities.value("supports_tools", false) ||
                !capabilities.value("supports_tool_calls", false))
                return {MaiErrorCode::NotSupported,
                        "The native local chat template does not support Agent tools."};
            inputs.tools.push_back(
                {"mai_validation_read", "Read a validation marker.",
                 R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})"});
            common_chat_msg call;
            call.role = "assistant";
            call.tool_calls.push_back(
                {"mai_validation_read", R"({"path":"marker.txt"})", "call_validation"});
            inputs.messages.push_back(std::move(call));
            common_chat_msg result;
            result.role = "tool";
            result.tool_name = "mai_validation_read";
            result.tool_call_id = "call_validation";
            result.content = "MAI_VALIDATION_TOOL_RESULT";
            inputs.messages.push_back(std::move(result));
        }
        const common_chat_session session(templates.get(), nullptr, inputs);
        const auto prompt = session.prompt().str();
        if (!session.has_template() || prompt.empty() ||
            (requireTools &&
             (session.format() == COMMON_CHAT_FORMAT_CONTENT_ONLY || session.parser().empty() ||
              prompt.find("mai_validation_read") == std::string::npos ||
              prompt.find("MAI_VALIDATION_TOOL_RESULT") == std::string::npos)))
            return {MaiErrorCode::NotSupported,
                    "The native local chat template cannot preserve Agent tool calls and results."};
    } catch (const std::exception&) {
        return {MaiErrorCode::NotSupported,
                "The native local chat template is not supported by the embedded parser."};
    }
    return {};
}
}  // namespace

MaiError validateMaiLlamaModelConfig(const MaiLlamaModelConfig& config) {
    if (!config.projectorPath.isEmpty())
        return {MaiErrorCode::NotSupported,
                "Local primary models currently support text and tools only."};
    if (config.contextSize < 128 || config.contextSize > 32768 ||
        config.maximumTokens >= config.contextSize || config.threads == 0 || config.threads > 64 ||
        !std::isfinite(config.temperature) || config.temperature < 0 || config.temperature > 2 ||
        config.maximumOutputBytes == 0 || config.maximumOutputBytes > 4 * 1024 * 1024)
        return {MaiErrorCode::InvalidInput,
                "Local primary model inference limits are out of range."};
    std::uint64_t fileBytes = 0;
    if (config.modelPath.isEmpty() || !MaiFileSystem::fileSize(config.modelPath, fileBytes) ||
        MaiFileSystem::isDirectory(config.modelPath))
        return {MaiErrorCode::NotFound, "Local GGUF file is missing or unavailable."};
    if (fileBytes < 24 || fileBytes > kMaximumModelBytes) return invalidGguf();
    try {
        std::string prefix;
        const auto readError =
            MaiFileSystem::readFile(config.modelPath, prefix, kMaximumPrefixBytes);
        if (readError) return {readError.code(), "Local GGUF file could not be read."};
        if (prefix.size() < 24 || prefix.compare(0, 4, "GGUF") != 0) return invalidGguf();
        MaiGgufPrefixReader reader(prefix, 4);
        std::uint64_t version = 0;
        std::uint64_t tensorCount = 0;
        std::uint64_t metadataCount = 0;
        if (!reader.readUnsigned(4, version) || (version != 2 && version != 3) ||
            !reader.readUnsigned(8, tensorCount) || tensorCount == 0 ||
            tensorCount > kMaximumTensors || !reader.readUnsigned(8, metadataCount) ||
            metadataCount == 0 || metadataCount > kMaximumMetadata)
            return invalidGguf();
        MaiGgufTemplateMetadata templates;
        if (!readMetadata(reader, metadataCount, templates)) return invalidGguf();
        std::vector<MaiGgufTensorRange> ranges;
        if (!readTensorRanges(reader, tensorCount, ranges)) return invalidGguf();
        const std::uint64_t remainder = reader.position() % templates.alignment;
        const std::uint64_t dataStart =
            reader.position() + (remainder == 0 ? 0 : templates.alignment - remainder);
        if (dataStart > fileBytes) return invalidGguf();
        std::sort(ranges.begin(), ranges.end(), [](const auto& first, const auto& second) {
            return first.offset < second.offset;
        });
        std::uint64_t previousEnd = 0;
        for (const auto& range : ranges) {
            if (range.offset % templates.alignment != 0 || range.offset < previousEnd ||
                range.offset > fileBytes - dataStart ||
                range.bytes > fileBytes - dataStart - range.offset)
                return invalidGguf();
            previousEnd = range.offset + range.bytes;
        }
        // 上游初始化可能执行 default 和 tool_use；必须在第一次调用它之前核准每个非空
        // 模板，不能只核准最后被选中的一个，让另一个未知模板仍进入 caps/analysis 执行。
        for (const auto* source : {&templates.primary, &templates.toolUse}) {
            if (source->empty()) continue;
            const auto approved = validateApprovedTemplate(*source);
            if (approved) return approved;
        }
        // 与上游一致，显式 tool_use 可作为缺省模板；从不启用空值/chatml 别名的回退。
        const std::string& primary =
            templates.primary.empty() ? templates.toolUse : templates.primary;
        const auto plainError = validateNativeTemplate(primary, false);
        if (plainError) return plainError;
        return validateNativeTemplate(templates.toolUse.empty() ? primary : templates.toolUse,
                                      true);
    } catch (const std::exception&) {
        return invalidGguf();
    }
}
