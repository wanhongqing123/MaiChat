// 文件作用：为自定义存储提供用户输入凭据及队列的普通查询兜底。
#include "MaiSessionStore.h"

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
