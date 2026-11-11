// 文件作用：统一工具原始输出的编号校验及保留 UTF-8 标量的字节分页。
#include "MaiToolOutput.h"

#include <algorithm>
#include <cstdint>

namespace {

// 按 Chromium ReadUnicodeCharacter 的原则先解码再校验标量；这里不替换非法源字节。
// 0 表示当前单字节非法，调用方逐字节消耗，不能吞掉后续可能有效的标量。
std::size_t scalarBytes(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) return 0;
    const auto lead = static_cast<std::uint8_t>(text[offset]);
    if (lead <= 0x7F) return 1;
    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    }
    if (length == 0 || length > text.size() - offset) return 0;
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
        if ((continuation & 0xC0) != 0x80) return 0;
        value = (value << 6) | (continuation & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
    return length;
}

bool validIdentity(const std::string& id) {
    if (id.empty() || id.size() > 128 || id.find('\0') != std::string::npos) return false;
    for (std::size_t offset = 0; offset < id.size();) {
        const auto length = scalarBytes(id, offset);
        if (length == 0) return false;
        offset += length;
    }
    return true;
}

}  // namespace

MaiError maiValidateToolOutputRecord(const MaiToolOutputRecord& output) {
    if (!validIdentity(output.id) || !validIdentity(output.sessionId) ||
        !validIdentity(output.messageId) || !validIdentity(output.partId) ||
        !validIdentity(output.callId))
        return {MaiErrorCode::InvalidInput, "Tool output identity is invalid."};
    if (output.text.size() > kMaiToolOutputMaxBytes)
        return {MaiErrorCode::InvalidInput, "Tool output exceeds the 16 MiB capture limit."};
    return {};
}

MaiError maiValidateToolOutputQuery(const std::string& sessionId, const MaiToolOutputQuery& query) {
    if (!validIdentity(sessionId) || !validIdentity(query.outputId) || query.maxBytes == 0 ||
        query.maxBytes > 4096 || query.offsetBytes > kMaiToolOutputMaxBytes)
        return {MaiErrorCode::InvalidInput, "Tool output query is invalid."};
    return {};
}

MaiResult<MaiToolOutputPage> maiPageToolOutput(const MaiToolOutputRecord& output,
                                               const MaiToolOutputQuery& query,
                                               std::string_view bytes, std::size_t windowOffset,
                                               std::size_t totalBytes) {
    if (const auto error = maiValidateToolOutputQuery(output.sessionId, query)) return error;
    if (query.offsetBytes > totalBytes)
        return {MaiErrorCode::InvalidInput, "Tool output offset exceeds the retained bytes."};
    const std::size_t before = std::min<std::size_t>(3, query.offsetBytes);
    const std::size_t after = std::min(totalBytes - query.offsetBytes, query.maxBytes + 3);
    if (windowOffset > query.offsetBytes - before || windowOffset > totalBytes ||
        bytes.size() > totalBytes - windowOffset ||
        query.offsetBytes - windowOffset > bytes.size() ||
        after > bytes.size() - (query.offsetBytes - windowOffset))
        return {MaiErrorCode::Internal, "Tool output paging window is incomplete."};
    const std::size_t start = query.offsetBytes - windowOffset;
    for (std::size_t index = start - before; index < start; ++index) {
        if (scalarBytes(bytes, index) > start - index)
            return {MaiErrorCode::InvalidInput, "Tool output offset splits a UTF-8 scalar."};
    }
    MaiToolOutputPage page;
    page.outputId = output.id;
    page.messageId = output.messageId;
    page.partId = output.partId;
    page.callId = output.callId;
    page.offsetBytes = query.offsetBytes;
    page.totalBytes = totalBytes;
    page.truncated = output.truncated;
    while (page.sourceBytesRead < totalBytes - query.offsetBytes) {
        const std::size_t length = scalarBytes(bytes, start + page.sourceBytesRead);
        const std::size_t consumed = length == 0 ? 1 : length;
        if (consumed > query.maxBytes - page.sourceBytesRead) break;
        page.sourceBytesRead += consumed;
        if (length == 0) page.textIsValidUtf8 = false;
        if (page.sourceBytesRead == query.maxBytes) break;
    }
    if (page.sourceBytesRead == 0 && query.offsetBytes < totalBytes)
        return {MaiErrorCode::InvalidInput, "Page budget cannot hold the next UTF-8 scalar."};
    if (page.sourceBytesRead != 0) page.text.assign(bytes.data() + start, page.sourceBytesRead);
    if (page.sourceBytesRead < totalBytes - query.offsetBytes)
        page.nextOffsetBytes = query.offsetBytes + page.sourceBytesRead;
    return page;
}
