// 文件作用：收集所选会话及其子任务的待回答问题，并投影可重放的子任务问答。
#include "MaiQuestionTranscript.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "MaiAgent.h"

bool maiQuestionBelongsToSession(const MaiAgent& agent, const std::string& selectedSessionId,
                                 const std::string& ownerSessionId) {
    if (selectedSessionId.empty() || ownerSessionId.empty()) return false;
    std::unordered_set<std::string> visited;
    std::string owner = ownerSessionId;
    for (int depth = 0; depth < 64 && !owner.empty(); ++depth) {
        if (!visited.insert(owner).second) return false;
        MaiSession session;
        if (!agent.getSession(owner, session)) return false;
        if (owner == selectedSessionId) return true;
        owner = session.parentId;
    }
    return false;
}

std::string maiQuestionRootSessionId(const MaiAgent& agent, const std::string& sessionId) {
    std::unordered_set<std::string> visited;
    std::string owner = sessionId;
    for (int depth = 0; depth < 64 && !owner.empty(); ++depth) {
        if (!visited.insert(owner).second) return {};
        MaiSession session;
        if (!agent.getSession(owner, session)) return {};
        if (session.parentId.empty()) return owner;
        owner = session.parentId;
    }
    return {};
}

MaiQuestionTranscript captureMaiQuestionTranscript(MaiAgent& agent, const std::string& selectedSessionId,
                                                   bool includeChildHistory) {
    MaiQuestionTranscript transcript;
    MaiSession selected;
    if (selectedSessionId.empty() || !agent.getSession(selectedSessionId, selected)) return transcript;
    for (const auto& question : agent.listPendingQuestions())
        if (maiQuestionBelongsToSession(agent, selectedSessionId, question.sessionId))
            transcript.pending.push_back(question);
    std::sort(transcript.pending.begin(), transcript.pending.end(), [](const auto& left, const auto& right) {
        return left.asked != right.asked ? left.asked < right.asked : left.id < right.id;
    });
    if (!includeChildHistory) return transcript;

    std::unordered_set<std::string> seenMessages;
    const auto appendQuestionMessage = [&](const std::string& owner, MaiMessage message) {
        message.parts.erase(std::remove_if(message.parts.begin(), message.parts.end(),
                                           [](const auto& part) {
                                               const auto* tool = std::get_if<MaiToolPart>(&part.body);
                                               return tool == nullptr || tool->tool != "question";
                                           }),
                            message.parts.end());
        if (message.role != MaiRole::Assistant || message.parts.empty() || !seenMessages.insert(message.id).second)
            return;
        transcript.childMessages.push_back({owner, std::move(message)});
    };
    // listSessions 有意只返回根会话；后代必须通过子任务接口逐层读取。
    // listSubAgents 保留存储 updated 倒序；逆序压栈后优先最近活跃的子分支。
    // 历史 owner 同时限制深度和总数，不能在首次冷页遍历全部已关闭任务。
    std::vector<std::pair<std::string, int>> owners;
    const auto addChildren = [&](const std::string& parent, int depth) {
        const auto children = agent.listSubAgents(parent);
        for (auto child = children.crbegin(); child != children.crend(); ++child)
            owners.emplace_back(child->sessionId, depth + 1);
    };
    addChildren(selectedSessionId, 0);
    std::unordered_set<std::string> visited{selectedSessionId};
    std::size_t historyOwners = 0;
    while (!owners.empty() && historyOwners < 64) {
        const auto owner = owners.back();
        owners.pop_back();
        if (!visited.insert(owner.first).second || owner.second >= 64) continue;
        ++historyOwners;
        for (auto message : agent.listMessagesPage(owner.first, "", 8))
            appendQuestionMessage(owner.first, std::move(message));
        if (historyOwners < 64 && owner.second < 63) addChildren(owner.first, owner.second);
    }
    for (const auto& question : transcript.pending) {
        if (question.sessionId == selectedSessionId || seenMessages.count(question.messageId) != 0) continue;
        MaiMessage message;
        if (agent.getMessage(question.sessionId, question.messageId, message))
            appendQuestionMessage(question.sessionId, std::move(message));
    }
    const auto earlier = [](const auto& left, const auto& right) {
        return left.message.created != right.message.created ? left.message.created < right.message.created
                                                             : left.message.id < right.message.id;
    };
    if (transcript.childMessages.size() > 64) {
        std::unordered_set<std::string> pendingMessages;
        for (const auto& question : transcript.pending) pendingMessages.insert(question.messageId);
        // 超出窗口时优先保留仍在等待的原消息，再取最近的历史问答。
        std::sort(transcript.childMessages.begin(), transcript.childMessages.end(),
                  [&](const auto& left, const auto& right) { return earlier(right, left); });
        std::stable_partition(transcript.childMessages.begin(), transcript.childMessages.end(),
                              [&](const auto& item) { return pendingMessages.count(item.message.id) != 0; });
        transcript.childMessages.resize(64);
    }
    std::sort(transcript.childMessages.begin(), transcript.childMessages.end(), earlier);
    return transcript;
}
