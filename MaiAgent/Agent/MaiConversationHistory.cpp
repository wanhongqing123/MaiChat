// 文件作用：共用会话原史读取/检索逻辑，显式返回未读尾部和精确消息、片段、字段偏移。
#include "MaiConversationHistory.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

struct HistoryField {
    std::string partId;
    std::string name;
    std::string text;
    std::string tool;
    std::string callId;
};

std::vector<HistoryField> readableFields(const MaiMessage& message) {
    std::vector<HistoryField> fields;
    for (const auto& part : message.parts) {
        if (const auto* text = std::get_if<MaiTextPart>(&part.body)) {
            fields.push_back({part.id, "text", text->text, "", ""});
        } else if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
            fields.push_back({part.id, "tool_input", tool->input, tool->tool, tool->callId});
            fields.push_back({part.id, "tool_output", tool->output, tool->tool, tool->callId});
            fields.push_back({part.id, "tool_error", tool->error, tool->tool, tool->callId});
        } else if (const auto* image = std::get_if<MaiImagePart>(&part.body)) {
            fields.push_back({part.id, "media_path", image->path, "", ""});
        } else if (const auto* video = std::get_if<MaiVideoPart>(&part.body)) {
            fields.push_back({part.id, "media_path", video->path, "", ""});
        } else if (const auto* quote = std::get_if<MaiQuotePart>(&part.body)) {
            fields.push_back({part.id, "quoted_message_id", quote->messageId, "", ""});
        }
    }
    return fields;
}

// 只把完整、合法标量的续字节当作内部边界。参照 Chromium Unicode 解码契约：
// 过长编码、代理项、越界标量与截断序列都是非法输入，不能因字节形状像 continuation 就吞掉。
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
    for (std::size_t distance = 1; distance <= 3 && distance <= index; ++distance) {
        if (validUtf8Length(text, index - distance) > distance) return false;
    }
    return true;
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const std::size_t length = validUtf8Length(text, offset);
        if (length == 0) return false;
        offset += length;
    }
    return true;
}

MaiConversationHistoryExcerpt excerpt(const MaiMessage& message, const HistoryField& field,
                                      std::size_t offset, std::size_t limit) {
    MaiConversationHistoryExcerpt result;
    result.messageId = message.id;
    result.role = message.role;
    result.partId = field.partId;
    result.field = field.name;
    result.tool = field.tool;
    result.callId = field.callId;
    result.totalBytes = field.text.size();
    result.offsetBytes = offset;
    std::size_t end = offset + std::min(limit, field.text.size() - offset);
    while (end > offset && !isBoundary(field.text, end)) --end;
    result.text = field.text.substr(offset, end - offset);
    result.sourceBytesRead = end - offset;
    result.textIsValidUtf8 = isValidUtf8(result.text);
    result.nextOffsetBytes = end < field.text.size() ? end : 0;
    return result;
}

}  // namespace

MaiResult<MaiConversationHistoryPage> maiReadConversationHistory(
    const MaiSessionStore& store, const std::string& sessionId,
    const MaiConversationHistoryQuery& query) {
    if (query.maxBytes < 4 || query.maxBytes > 65536 || query.maxMessages == 0 ||
        query.maxMessages > 50 || query.search.size() > 512 || query.messageId.size() > 128 ||
        query.partId.size() > 128 || query.beforeMessageId.size() > 128)
        return {MaiErrorCode::InvalidInput, "Invalid history query budget, ID, or search length."};
    MaiSession session;
    if (sessionId.empty() || !store.getSession(sessionId, session))
        return {MaiErrorCode::NotFound, "Current conversation is unavailable."};
    MaiConversationHistoryPage page;
    if (!query.messageId.empty()) {
        if (!query.search.empty() || !query.beforeMessageId.empty())
            return {MaiErrorCode::InvalidInput,
                    "Exact history reads cannot include search or a message cursor."};
        MaiMessage message;
        if (!store.getMessage(sessionId, query.messageId, message))
            return {MaiErrorCode::NotFound, "Original message is not in the current conversation."};
        if (query.listParts) {
            if (!query.partId.empty() || query.field != "text" || query.offsetBytes != 0)
                return {MaiErrorCode::InvalidInput,
                        "list_parts requires a message ID and a field_offset cursor only."};
            const auto fields = readableFields(message);
            if (query.fieldOffset > fields.size())
                return {MaiErrorCode::InvalidInput,
                        "field_offset is outside the original message."};
            const std::size_t end =
                query.fieldOffset + std::min(query.maxMessages, fields.size() - query.fieldOffset);
            for (std::size_t index = query.fieldOffset; index < end; ++index) {
                auto metadata = excerpt(message, fields[index], 0, 0);
                metadata.metadataOnly = true;
                page.excerpts.push_back(std::move(metadata));
            }
            page.hasMore = end < fields.size();
            page.nextFieldOffset = page.hasMore ? end : 0;
            return page;
        }
        if (query.fieldOffset != 0)
            return {MaiErrorCode::InvalidInput, "field_offset requires list_parts."};
        HistoryField selected;
        if (query.partId.empty()) {
            if (query.field != "text")
                return {MaiErrorCode::InvalidInput, "A non-text history field requires part_id."};
            selected = {"", "text", message.text(), "", ""};
        } else {
            const auto fields = readableFields(message);
            const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
                return field.partId == query.partId && field.name == query.field;
            });
            if (found == fields.end())
                return {MaiErrorCode::NotFound,
                        "Original part or readable field is unavailable in this message."};
            selected = *found;
        }
        if (!isBoundary(selected.text, query.offsetBytes))
            return {MaiErrorCode::InvalidInput,
                    "offset_bytes must be a UTF-8 boundary within the original field."};
        page.excerpts.push_back(excerpt(message, selected, query.offsetBytes, query.maxBytes));
        page.hasMore = page.excerpts.front().nextOffsetBytes != 0;
        return page;
    }
    if (!query.partId.empty() || query.offsetBytes != 0 || query.field != "text" ||
        query.listParts || query.fieldOffset != 0)
        return {MaiErrorCode::InvalidInput, "Part, field, and offset require an exact message_id."};
    // 分页游标也须属于当前会话，不能通过另一会话的 ID 影响或探测其历史。
    if (!query.beforeMessageId.empty()) {
        MaiMessage cursor;
        if (!store.getMessage(sessionId, query.beforeMessageId, cursor))
            return {MaiErrorCode::NotFound, "History cursor is not in the current conversation."};
    }
    std::string before = query.beforeMessageId;
    std::size_t remaining = query.maxBytes;
    std::size_t scanned = 0;
    while (scanned < 1000) {
        const auto messages = store.listMessagesPage(sessionId, before, 64);
        if (messages.empty()) break;
        for (const auto& message : messages) {
            before = message.id;
            ++scanned;
            for (const auto& field : readableFields(message)) {
                if (field.text.empty()) continue;
                const auto match = query.search.empty() ? 0 : field.text.find(query.search);
                if (match == std::string::npos) continue;
                const std::size_t available = std::min(remaining, std::size_t{2048});
                const std::size_t leading =
                    query.search.empty()
                        ? 0
                        : std::min(std::size_t{256},
                                   (available - std::min(available, query.search.size())) / 2);
                std::size_t offset = match > leading ? match - leading : 0;
                while (offset > 0 && !isBoundary(field.text, offset)) --offset;
                auto found =
                    excerpt(message, field, offset, std::min(remaining, std::size_t{2048}));
                remaining -= found.text.size();
                page.excerpts.push_back(std::move(found));
                break;
            }
            if (page.excerpts.size() >= query.maxMessages || remaining < 4 || scanned >= 1000) {
                page.hasMore = !store.listMessagesPage(sessionId, before, 1).empty();
                if (page.hasMore) page.nextBeforeMessageId = before;
                return page;
            }
        }
        if (messages.size() < 64) break;
    }
    return page;
}
