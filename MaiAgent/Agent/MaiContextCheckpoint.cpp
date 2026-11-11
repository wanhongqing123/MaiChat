// 文件作用：核对压缩摘要覆盖的原始历史，防止重启、并发修改或跨会话误用摘要。
#include "MaiContextCheckpoint.h"

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>

#include <openssl/evp.h>

namespace {

class SourceDigest {
public:
    SourceDigest() : mContext(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
        mValid = mContext && EVP_DigestInit_ex(mContext.get(), EVP_sha256(), nullptr) == 1;
    }
    void append(std::string_view value) {
        const std::string length = std::to_string(value.size()) + ":";
        if (mValid)
            mValid = EVP_DigestUpdate(mContext.get(), length.data(), length.size()) == 1 &&
                     EVP_DigestUpdate(mContext.get(), value.data(), value.size()) == 1;
    }
    std::string finish() {
        std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
        unsigned int length = 0;
        if (!mValid || EVP_DigestFinal_ex(mContext.get(), bytes.data(), &length) != 1) return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(length * 2);
        for (unsigned int index = 0; index < length; ++index) {
            result += hex[bytes[index] >> 4];
            result += hex[bytes[index] & 15];
        }
        return result;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mContext;
    bool mValid = false;
};

void appendPart(SourceDigest& digest, const MaiMessagePart& part) {
    digest.append(part.id);
    digest.append(std::to_string(part.created));
    digest.append(std::to_string(part.body.index()));
    std::visit(
        [&](const auto& body) {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, MaiTextPart> ||
                          std::is_same_v<Body, MaiReasoningPart>) {
                digest.append(body.text);
            } else if constexpr (std::is_same_v<Body, MaiImagePart> ||
                                 std::is_same_v<Body, MaiVideoPart>) {
                digest.append(body.path);
                digest.append(body.mimeType);
            } else if constexpr (std::is_same_v<Body, MaiQuotePart>) {
                digest.append(body.messageId);
                digest.append(body.preview);
            } else if constexpr (std::is_same_v<Body, MaiToolPart>) {
                digest.append(body.tool);
                digest.append(body.callId);
                digest.append(body.input);
                digest.append(body.output);
                digest.append(body.error);
                digest.append(maiToolStateToString(body.state));
                // 旧记录没有此字段，维持既有指纹；新增事实必须使已覆盖源摘要失效。
                if (body.execution) {
                    digest.append("MaiToolExecution/v1");
                    digest.append(maiSerializeToolExecution(*body.execution));
                }
            }
        },
        part.body);
}

}  // namespace

std::string maiContextSourceFingerprint(const std::string& sessionId,
                                        const std::vector<MaiMessage>& history, std::size_t count) {
    if (sessionId.empty() || count > history.size()) return {};
    SourceDigest digest;
    digest.append("MaiContextSource/v1");
    digest.append(sessionId);
    digest.append(std::to_string(count));
    for (std::size_t index = 0; index < count; ++index) {
        const auto& message = history[index];
        digest.append(message.id);
        digest.append(maiRoleToString(message.role));
        digest.append(std::to_string(message.created));
        digest.append(std::to_string(message.completed));
        digest.append(message.inputRequestId);
        digest.append(message.pendingInput ? "pending" : "consumed");
        digest.append(std::to_string(message.parts.size()));
        for (const auto& part : message.parts) {
            appendPart(digest, part);
        }
    }
    return digest.finish();
}

std::string maiContextPartialSourceFingerprint(const MaiContextCheckpoint& checkpoint,
                                               const std::vector<MaiMessage>& history) {
    if (checkpoint.partialMessageId.empty() || checkpoint.sourceMessageCount >= history.size() ||
        checkpoint.partialPartCount == 0)
        return {};
    const auto& message = history[checkpoint.sourceMessageCount];
    if (message.id != checkpoint.partialMessageId || message.role != MaiRole::Assistant ||
        checkpoint.partialPartCount > message.parts.size())
        return {};
    SourceDigest digest;
    digest.append("MaiContextPartialSource/v1");
    digest.append(checkpoint.sessionId);
    digest.append(checkpoint.sourceFingerprint);
    digest.append(checkpoint.preservedUserMessageId);
    digest.append(message.id);
    digest.append(maiRoleToString(message.role));
    digest.append(std::to_string(message.created));
    digest.append(std::to_string(checkpoint.partialPartCount));
    for (std::size_t index = 0; index < checkpoint.partialPartCount; ++index)
        appendPart(digest, message.parts[index]);
    return digest.finish();
}

bool maiContextCheckpointMatches(const MaiContextCheckpoint& checkpoint,
                                 const std::string& sessionId,
                                 const std::vector<MaiMessage>& history) {
    if (checkpoint.sessionId != sessionId || checkpoint.revision == 0 ||
        checkpoint.sourceMessageCount == 0 || checkpoint.sourceMessageCount > history.size() ||
        checkpoint.summary.empty() || checkpoint.sourceFingerprint.empty() ||
        checkpoint.firstMessageId != history.front().id ||
        checkpoint.throughMessageId != history[checkpoint.sourceMessageCount - 1].id ||
        checkpoint.sourceFingerprint !=
            maiContextSourceFingerprint(sessionId, history, checkpoint.sourceMessageCount))
        return false;
    std::string latestUser;
    for (std::size_t index = 0; index < checkpoint.sourceMessageCount; ++index) {
        const auto& message = history[index];
        if (message.pendingInput || (message.role == MaiRole::Assistant && message.completed == 0))
            return false;
        if (message.role == MaiRole::User) latestUser = message.id;
    }
    if (checkpoint.partialMessageId.empty())
        return checkpoint.partialPartCount == 0 && checkpoint.throughPartId.empty() &&
               checkpoint.partialSourceFingerprint.empty() &&
               checkpoint.preservedUserMessageId.empty();
    if (checkpoint.sourceMessageCount >= history.size() ||
        checkpoint.preservedUserMessageId.empty() ||
        latestUser != checkpoint.preservedUserMessageId)
        return false;
    const auto& partial = history[checkpoint.sourceMessageCount];
    if (partial.id != checkpoint.partialMessageId || partial.role != MaiRole::Assistant ||
        partial.pendingInput || checkpoint.partialPartCount == 0 ||
        checkpoint.partialPartCount > partial.parts.size() ||
        checkpoint.throughPartId != partial.parts[checkpoint.partialPartCount - 1].id)
        return false;
    // 只校验已覆盖源。媒体组是否可切分由创建边界时决定，不能读取后来追加的
    // 未覆盖 tail 再推翻稳定前缀；否则保留工具之后追加观察会让摘要无故失效。
    for (std::size_t index = 0; index < checkpoint.partialPartCount; ++index) {
        const auto* tool = std::get_if<MaiToolPart>(&partial.parts[index].body);
        if (tool && (tool->state == MaiToolState::Running || tool->state == MaiToolState::Pending))
            return false;
    }
    return !checkpoint.partialSourceFingerprint.empty() &&
           checkpoint.partialSourceFingerprint ==
               maiContextPartialSourceFingerprint(checkpoint, history);
}
