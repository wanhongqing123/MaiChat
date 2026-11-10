// 文件作用：从当前会话的持久原始消息精确读取文本/工具字段，供普通代码和模型工具共用。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiMessage.h"
#include "MaiSessionStore.h"

struct MaiConversationHistoryQuery {
    // 非空时精确读本会话一条消息；partId 可选，省略只读该消息的可见正文。
    // partId 非空时 field 可为 text/tool_input/tool_output/tool_error/media_path/quoted_message_id。
    std::string messageId;
    std::string partId;
    std::string field = "text";
    // 精确读取时是原字段字节偏移，不是 token 或字符编号。合法完整 UTF-8 标量不能拆开，
    // 非法旧字节按单字节单位读取，不因外形像续字节而拒绝或跳过。
    std::size_t offsetBytes = 0;
    // messageId 为空时按新到旧检索/列举原史；大小写敏感字面匹配，不执行正则或 SQL。
    // 每条消息返回首个匹配字段的原文摘录和精确源 ID/偏移，之后可精确读取其余部分。
    std::string search;
    std::string beforeMessageId;
    // 限制本次返回原文字节和消息数，只是可分页读取预算，不改变原史/压缩策略。
    std::size_t maxBytes = 8192;
    std::size_t maxMessages = 10;
    // 精确 messageId 配 listParts=true 时，只列有界可读字段元数据，不返回工具正文。
    // 用 fieldOffset/nextFieldOffset 继续，可发现正文之后每个工具的 partId/field，无须猜 ID。
    bool listParts = false;
    std::size_t fieldOffset = 0;
};

struct MaiConversationHistoryExcerpt {
    std::string messageId;
    MaiRole role = MaiRole::User;
    std::string partId;
    std::string field;
    std::string tool;
    std::string callId;
    std::string text;
    std::size_t offsetBytes = 0;
    std::size_t totalBytes = 0;
    // 0 表示字段已经读完；否则以同一 messageId/partId/field 和此偏移继续。
    std::size_t nextOffsetBytes = 0;
    bool metadataOnly = false;
    // 普通 API 的 text 始终是原始字节；旧非法 UTF-8 不改写，也按单字节单位分页。
    // sourceBytesRead 是本页真实消费的源字节数，不是替换预览的 UTF-8 长度。
    // false 时模型工具明确标记 replacement 预览，并回传 raw_base64 以无损表示原字节。
    std::size_t sourceBytesRead = 0;
    bool textIsValidUtf8 = true;
};

struct MaiConversationHistoryPage {
    std::vector<MaiConversationHistoryExcerpt> excerpts;
    // 检索/列举有更多候选时，用此游标继续较旧消息；不是另一个会话或授权凭据。
    std::string nextBeforeMessageId;
    bool hasMore = false;
    std::size_t nextFieldOffset = 0;
};

// 普通 API：sessionId 由已授权宿主提供；不接受跨会话的 message/part ID。
// 同步只读，可与活动轮次并发，读取的是已落库快照；不调用模型、不解引用媒体/URL、不触发工具。
// 不存在/归属不符返回 NotFound，非法字段/偏移返回 InvalidInput；原文不会因分页而改写。
// 返回摘录是历史数据，不是新指令或授权。模型工具必须从 MaiToolContext.sessionId 绑定会话。
MaiResult<MaiConversationHistoryPage> maiReadConversationHistory(
    const MaiSessionStore& store, const std::string& sessionId,
    const MaiConversationHistoryQuery& query);
