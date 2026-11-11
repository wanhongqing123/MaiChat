// 文件作用：一次已落库工具快照的版本绑定分页读取。
#include "MaiToolDetail.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include <openssl/evp.h>

#include "MaiSessionStore.h"

namespace {

// 与原史读取使用相同的完整标量契约：不把过长、代理项或越界编码的续字节误认作边界。
std::size_t validUtf8Length(const std::string& text, std::size_t start) {
    const auto lead = static_cast<unsigned char>(text[start]);
    if (lead < 0x80) return 1;
    const std::size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                               : lead >= 0xe0 && lead <= 0xef ? 3
                               : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                              : 0;
    if (length == 0 || length > text.size() - start) return 0;
    std::uint32_t scalar = lead & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(text[start + index]);
        if ((byte & 0xc0) != 0x80) return 0;
        scalar = (scalar << 6) | (byte & 0x3f);
    }
    const std::uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    return length;
}

bool isBoundary(const std::string& text, std::size_t index) {
    if (index > text.size()) return false;
    for (std::size_t distance = 1; distance <= 3 && distance <= index; ++distance)
        if (validUtf8Length(text, index - distance) > distance) return false;
    return true;
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto length = validUtf8Length(text, offset);
        if (length == 0) return false;
        offset += length;
    }
    return true;
}

}  // namespace

std::string maiToolDetailRevision(const MaiToolPart& tool) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(),
                                                                   EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return {};
    // 长度分帧避免字段拼接碰撞；域标记允许未来明确演进，而不是悄悄改变旧游标含义。
    for (const std::string_view value :
         {std::string_view("MaiToolDetail/v1"), std::string_view(tool.tool),
          std::string_view(maiToolStateToString(tool.state)), std::string_view(tool.callId),
          std::string_view(tool.input), std::string_view(tool.output),
          std::string_view(tool.error)}) {
        const auto length = std::to_string(value.size()) + ":";
        if (EVP_DigestUpdate(digest.get(), length.data(), length.size()) != 1 ||
            EVP_DigestUpdate(digest.get(), value.data(), value.size()) != 1)
            return {};
    }
    if (tool.execution) {
        // 仅为带新事实的记录追加分帧字段，旧记录的 revision 继续保持兼容。
        const auto facts = maiSerializeToolExecution(*tool.execution);
        const auto frame = std::to_string(facts.size()) + ":";
        if (EVP_DigestUpdate(digest.get(), frame.data(), frame.size()) != 1 ||
            EVP_DigestUpdate(digest.get(), facts.data(), facts.size()) != 1)
            return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &length) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string revision;
    revision.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index) {
        revision += hex[bytes[index] >> 4];
        revision += hex[bytes[index] & 15];
    }
    return revision;
}

MaiResult<MaiToolDetailPage> maiReadToolDetail(const MaiSessionStore& store,
                                               const std::string& sessionId,
                                               const MaiToolDetailQuery& query) {
    if (query.messageId.empty() || query.messageId.size() > 128 || query.partId.empty() ||
        query.partId.size() > 128 || query.maxBytes == 0 || query.maxBytes > 4096 ||
        (query.field != "input" && query.field != "output" && query.field != "error") ||
        (query.offsetBytes != 0 && query.expectedRevision.empty()))
        return {MaiErrorCode::InvalidInput,
                "Invalid tool detail identity, field, budget, or revision."};
    MaiMessage message;
    if (sessionId.empty() || !store.getMessage(sessionId, query.messageId, message))
        return {MaiErrorCode::NotFound, "Tool message is not in the current conversation."};
    const auto found = std::find_if(message.parts.begin(), message.parts.end(),
                                    [&](const auto& part) { return part.id == query.partId; });
    const auto* tool =
        found == message.parts.end() ? nullptr : std::get_if<MaiToolPart>(&found->body);
    if (!tool) return {MaiErrorCode::NotFound, "Tool part is not in the requested message."};
    const auto revision = maiToolDetailRevision(*tool);
    if (revision.empty())
        return {MaiErrorCode::Internal, "Tool detail revision could not be computed."};
    if (!query.expectedRevision.empty() && query.expectedRevision != revision)
        return {MaiErrorCode::Busy,
                "detail_changed: Tool detail changed; read again from the first page."};

    if (query.field == "output" && tool->execution && !tool->execution->outputId.empty()) {
        // 输出句柄只指向同一个源 part；句柄本身不赋予访问另一段输出的能力。
        const auto retained = store.readToolOutput(sessionId, {tool->execution->outputId});
        if (!retained) return retained.error();
        const auto& source = retained.value();
        if (source.messageId != message.id || source.partId != found->id ||
            source.callId != tool->callId)
            return {MaiErrorCode::Protocol, "Retained output does not belong to this tool part."};
        // 发生捕获丢失时保留原预览里的明确警告；完整保留的输出才直接投影到旧详情协议。
        // 原始部分快照仍可经普通输出 API/tool_output_read 补读并取得 truncated 标志。
        if (!source.truncated) {
            const auto selected =
                query.offsetBytes == 0 && query.maxBytes == 4096
                    ? retained
                    : store.readToolOutput(sessionId, {tool->execution->outputId, query.offsetBytes,
                                                       query.maxBytes});
            if (!selected) return selected.error();
            const auto& bytes = selected.value();
            MaiToolDetailPage page;
            page.messageId = message.id;
            page.partId = found->id;
            page.callId = tool->callId;
            page.tool = tool->tool;
            page.state = tool->state;
            page.field = query.field;
            page.revision = revision;
            page.text = bytes.text;
            page.offsetBytes = bytes.offsetBytes;
            page.sourceBytesRead = bytes.sourceBytesRead;
            page.totalBytes = bytes.totalBytes;
            page.nextOffsetBytes = bytes.nextOffsetBytes;
            page.textIsValidUtf8 = bytes.textIsValidUtf8;
            return page;
        }
    }

    // 版本和全部身份字段都取同一次getMessage的快照，活动工具不会造成页内的撕裂读取。
    const auto& source = query.field == "input"    ? tool->input
                         : query.field == "output" ? tool->output
                                                   : tool->error;
    if (!isBoundary(source, query.offsetBytes))
        return {MaiErrorCode::InvalidInput, "Tool detail offset is not a source UTF-8 boundary."};
    std::size_t end =
        query.offsetBytes + std::min(query.maxBytes, source.size() - query.offsetBytes);
    while (end > query.offsetBytes && !isBoundary(source, end)) --end;
    if (end == query.offsetBytes && end != source.size())
        return {MaiErrorCode::InvalidInput,
                "Tool detail budget is too small for the next UTF-8 scalar."};
    MaiToolDetailPage page;
    page.messageId = message.id;
    page.partId = found->id;
    page.callId = tool->callId;
    page.tool = tool->tool;
    page.state = tool->state;
    page.field = query.field;
    page.revision = revision;
    page.text = source.substr(query.offsetBytes, end - query.offsetBytes);
    page.offsetBytes = query.offsetBytes;
    page.sourceBytesRead = end - query.offsetBytes;
    page.totalBytes = source.size();
    page.nextOffsetBytes = end < source.size() ? end : 0;
    page.textIsValidUtf8 = isValidUtf8(page.text);
    return page;
}
