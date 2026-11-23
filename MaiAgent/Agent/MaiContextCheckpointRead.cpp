// 文件作用：只读当前保存的检查点副本，按版本绑定分页并保留原始摘要和引用字节。
#include "MaiContextCheckpointRead.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include <json.hpp>

#include "MaiSessionStore.h"

namespace {
using Json = nlohmann::json;
constexpr std::size_t kSummaryMetadataMaxBytes = 1024 * 1024;
constexpr std::size_t kReferenceMetadataMaxBytes = 32 * 1024 * 1024;

// 与现有原史/工具输出分页一样，参考 Chromium ReadUnicodeCharacter 先解码再核对标量。
// 非法源字节返回 0，由分页逐字节保留；不能替换后再把显示长度当作源游标。
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

bool isValidQuery(const std::string& sessionId, const MaiContextCheckpointReadQuery& query) {
    if (sessionId.empty() || sessionId.size() > 128 || sessionId.find('\0') != std::string::npos ||
        query.maxBytes < 4 || query.maxBytes > 65536)
        return false;
    switch (query.section) {
        case MaiContextCheckpointSection::Metadata: return query.offsetBytes == 0;
        case MaiContextCheckpointSection::Summary:
        case MaiContextCheckpointSection::References:
            return query.offsetBytes == 0 || query.expectedRevision != 0;
    }
    return false;
}

MaiContextCheckpointInfo checkpointInfo(const MaiContextCheckpoint& checkpoint) {
    MaiContextCheckpointInfo info;
    info.revision = checkpoint.revision;
    info.created = checkpoint.created;
    info.model = checkpoint.model;
    info.sourceMessageCount = checkpoint.sourceMessageCount;
    info.firstMessageId = checkpoint.firstMessageId;
    info.throughMessageId = checkpoint.throughMessageId;
    info.partialMessageId = checkpoint.partialMessageId;
    info.partialPartCount = checkpoint.partialPartCount;
    info.throughPartId = checkpoint.throughPartId;
    info.preservedUserMessageId = checkpoint.preservedUserMessageId;
    info.summaryBytes = checkpoint.summary.size();
    info.referenceBytes = checkpoint.references.size();

    // 这里只描述存档的可解析结构，不重新验证源历史，也不把六数组当作摘要正确性的证明。
    // 超限时保留未知状态；原字段仍可分页，不能以解析预算把有效源数据隐藏掉。
    if (checkpoint.summary.size() <= kSummaryMetadataMaxBytes) {
        const auto summary = Json::parse(checkpoint.summary, nullptr, false);
        info.summaryIsJsonObject = summary.is_object();
        if (info.summaryIsJsonObject) {
            for (const char* key : {"goals", "constraints", "decisions", "progress",
                                    "open_questions", "source_notes"}) {
                const auto array = summary.find(key);
                if (array != summary.end() && array->is_array())
                    info.summaryArrayCounts.emplace(key, array->size());
            }
        }
    }
    if (checkpoint.references.size() > kReferenceMetadataMaxBytes) {
        info.referenceIndexState = MaiContextReferenceIndexState::MetadataBudgetExceeded;
    } else {
        const auto references = Json::parse(checkpoint.references, nullptr, false);
        if (references.is_array()) {
            info.referenceIndexState = MaiContextReferenceIndexState::Available;
            info.referenceCountKnown = true;
            info.referenceCount = references.size();
        } else {
            info.referenceIndexState = MaiContextReferenceIndexState::Invalid;
        }
    }
    return info;
}
}  // namespace

MaiResult<MaiContextCheckpointPage> maiReadContextCheckpoint(
    const MaiSessionStore& store, const std::string& sessionId,
    const MaiContextCheckpointReadQuery& query) {
    if (!isValidQuery(sessionId, query))
        return {MaiErrorCode::InvalidInput, "Context checkpoint query is invalid."};

    MaiSession session;
    if (!store.getSession(sessionId, session)) {
        // 旧 bool 存储接口把缺失与读取失败合并。保守保留后端错误，避免伪装成空存档；
        // lastWriteError 也可能是先前写失败，现有接口无法进一步区分，不清除宿主错误状态。
        if (const auto error = store.lastWriteError()) return error;
        return {MaiErrorCode::NotFound, "Session does not exist."};
    }
    if (session.id != sessionId)
        return {MaiErrorCode::NotFound, "Session identity does not match the requested session."};

    MaiContextCheckpoint checkpoint;
    if (!store.getContextCheckpoint(sessionId, checkpoint)) {
        if (const auto error = store.lastWriteError()) return error;
        if (query.expectedRevision != 0)
            return {MaiErrorCode::Busy,
                    "Stored context checkpoint was removed; read its first page again."};
        MaiContextCheckpointPage absent;
        absent.section = query.section;
        return absent;
    }
    if (checkpoint.sessionId != sessionId)
        return {MaiErrorCode::NotFound, "Stored context checkpoint belongs to another session."};
    // 比较和分页来自这一次不可变副本。压缩并发推进不影响已返回页；后续请求核对版本。
    if (query.expectedRevision != 0 && query.expectedRevision != checkpoint.revision)
        return {MaiErrorCode::Busy,
                "Stored context checkpoint changed; read its first page again."};

    MaiContextCheckpointPage page;
    page.available = true;
    page.section = query.section;
    if (query.section == MaiContextCheckpointSection::Metadata) {
        page.info = checkpointInfo(checkpoint);
        return page;
    }

    const std::string& source = query.section == MaiContextCheckpointSection::Summary
                                    ? checkpoint.summary
                                    : checkpoint.references;
    if (query.offsetBytes > source.size())
        return {MaiErrorCode::InvalidInput, "Context checkpoint offset exceeds the stored field."};
    const auto before = std::min<std::size_t>(3, query.offsetBytes);
    for (std::size_t offset = query.offsetBytes - before; offset < query.offsetBytes; ++offset) {
        if (scalarBytes(source, offset) > query.offsetBytes - offset)
            return {MaiErrorCode::InvalidInput, "Context checkpoint offset splits a UTF-8 scalar."};
    }
    page.offsetBytes = query.offsetBytes;
    page.totalBytes = source.size();
    // maxBytes 只约束源字节；工具层的 JSON/替换预览/base64 另有编码预算，不在这里混算。
    while (page.sourceBytesRead < source.size() - query.offsetBytes) {
        const auto length = scalarBytes(source, query.offsetBytes + page.sourceBytesRead);
        const auto consumed = length == 0 ? std::size_t{1} : length;
        if (consumed > query.maxBytes - page.sourceBytesRead) break;
        page.sourceBytesRead += consumed;
        if (length == 0) page.textIsValidUtf8 = false;
        if (page.sourceBytesRead == query.maxBytes) break;
    }
    page.text = source.substr(query.offsetBytes, page.sourceBytesRead);
    page.hasMore = page.sourceBytesRead < source.size() - query.offsetBytes;
    if (page.hasMore) page.nextOffsetBytes = query.offsetBytes + page.sourceBytesRead;
    page.info = checkpointInfo(checkpoint);
    return page;
}
