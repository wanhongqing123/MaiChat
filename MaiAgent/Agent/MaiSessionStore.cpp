// 文件作用：为自定义存储提供用户输入凭据及队列的普通查询兜底。
#include "MaiSessionStore.h"

#include <array>
#include <limits>
#include <memory>
#include <set>
#include <string_view>

#include <openssl/evp.h>

namespace {

bool isImportUtf8(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        const std::size_t count = lead >= 0xc2 && lead <= 0xdf   ? 2
                                  : lead >= 0xe0 && lead <= 0xef ? 3
                                  : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                 : 0;
        if (count == 0 || count > value.size() - index) return false;
        std::uint32_t scalar = lead & (count == 2 ? 31 : count == 3 ? 15 : 7);
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 63);
        }
        if ((count == 3 && scalar < 0x800) || (count == 4 && scalar < 0x10000) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
        index += count;
    }
    return true;
}

bool isImportIdentifier(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum && isImportUtf8(value) &&
           value.find('\0') == std::string_view::npos;
}

}  // namespace

MaiError maiValidateContextImportSpec(const MaiContextImportSpec& spec) {
    if (!isImportIdentifier(spec.sessionId, 128) || !isImportIdentifier(spec.sourceKey, 128) ||
        !isImportIdentifier(spec.snapshotId, 256) || spec.label.size() > 1024 ||
        !isImportUtf8(spec.label))
        return {MaiErrorCode::InvalidInput, "Invalid context import identity or label."};
    for (unsigned char byte : spec.sourceKey)
        if (byte < 0x21 || byte > 0x7e)
            return {MaiErrorCode::InvalidInput, "Context source key must be printable ASCII."};
    return {};
}

MaiError maiValidateContextImportBatch(const std::vector<MaiContextImportRecord>& records) {
    if (records.empty() || records.size() > kMaiContextImportMaxBatchRecords)
        return {MaiErrorCode::InvalidInput, "Invalid context import batch record count."};
    std::size_t bytes = 0;
    std::int64_t previousTime = 0;
    std::set<std::string> sourceIds;
    for (const auto& record : records) {
        if (!isImportIdentifier(record.sourceId, 256) || record.text.empty() ||
            record.text.size() > kMaiContextImportMaxRecordTextBytes ||
            !isImportUtf8(record.text) || record.originalCreatedAtMillis < previousTime ||
            !sourceIds.insert(record.sourceId).second)
            return {MaiErrorCode::InvalidInput, "Invalid, duplicate or unordered context record."};
        const auto cost = record.sourceId.size() + record.text.size();
        if (cost > kMaiContextImportMaxBatchBytes - bytes)
            return {MaiErrorCode::InvalidInput, "Context import batch exceeds its byte budget."};
        bytes += cost;
        previousTime = record.originalCreatedAtMillis;
    }
    return {};
}

std::string maiContextImportBatchDigest(const std::vector<MaiContextImportRecord>& records) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) return {};
    const auto append = [&](std::string_view value) {
        const auto length = std::to_string(value.size()) + ":";
        return EVP_DigestUpdate(context.get(), length.data(), length.size()) == 1 &&
               EVP_DigestUpdate(context.get(), value.data(), value.size()) == 1;
    };
    if (!append("MaiContextImportBatch/v1")) return {};
    for (const auto& record : records)
        if (!append(record.sourceId) || !append(record.text) ||
            !append(std::to_string(record.originalCreatedAtMillis)))
            return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int index = 0; index < size; ++index) {
        result += hex[bytes[index] >> 4];
        result += hex[bytes[index] & 15];
    }
    return result;
}

MaiResult<MaiContextImportProgress> MaiSessionStore::beginContextImport(
    const MaiContextImportSpec&) {
    return {MaiErrorCode::NotSupported, "This store does not support staged context imports."};
}

MaiResult<MaiContextImportProgress> MaiSessionStore::appendContextImport(
    const std::string&, std::uint64_t, const std::vector<MaiContextImportRecord>&) {
    return {MaiErrorCode::NotSupported, "This store does not support staged context imports."};
}

MaiResult<MaiContextImportProgress> MaiSessionStore::inspectContextImport(
    const std::string&) const {
    return {MaiErrorCode::NotSupported, "This store does not support staged context imports."};
}

MaiResult<MaiContextImportProgress> MaiSessionStore::commitContextImport(const std::string&) {
    return {MaiErrorCode::NotSupported, "This store does not support staged context imports."};
}

MaiError MaiSessionStore::abortContextImport(const std::string&) {
    return {MaiErrorCode::NotSupported, "This store does not support staged context imports."};
}

MaiResult<MaiContextImportPurgeResult> MaiSessionStore::purgeContextImports(const std::string&,
                                                                            std::size_t) {
    return {MaiErrorCode::NotSupported, "This store does not support context import maintenance."};
}

MaiError MaiSessionStore::putToolOutput(const MaiToolOutputRecord&) {
    return {MaiErrorCode::NotSupported, "This session store does not retain tool output."};
}

MaiResult<MaiToolOutputPage> MaiSessionStore::readToolOutput(const std::string&,
                                                             const MaiToolOutputQuery&) const {
    return {MaiErrorCode::NotSupported, "This session store does not retain tool output."};
}

bool MaiSessionStore::getContextCheckpoint(const std::string&, MaiContextCheckpoint&) const {
    return false;
}

MaiError MaiSessionStore::advanceContextCheckpoint(const std::string&, std::uint64_t,
                                                   const MaiContextCheckpoint&) {
    return {MaiErrorCode::NotSupported, "This session store does not persist context checkpoints."};
}

bool MaiSessionStore::getInputMessage(const std::string& sessionId, const std::string& requestId,
                                      MaiMessage& out) const {
    if (requestId.empty()) return false;
    for (const auto& message : listMessages(sessionId)) {
        if (message.role == MaiRole::User && message.inputRequestId == requestId) {
            out = message;
            return true;
        }
    }
    return false;
}

std::vector<MaiMessage> MaiSessionStore::listQueuedInputs(const std::string& sessionId) const {
    std::vector<MaiMessage> result;
    for (const auto& message : listMessages(sessionId))
        if (message.role == MaiRole::User && message.pendingInput) result.push_back(message);
    return result;
}
