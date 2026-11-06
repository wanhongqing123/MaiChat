// 文件作用：为各平台宿主提供同一份提问归属、排序和子任务问答历史投影。
#pragma once

#include <string>
#include <vector>

#include "MaiMessage.h"
#include "MaiQuestion.h"

class MaiAgent;

// UI 投影保留真实消息和片段 ID，但不复制进根会话存储。
// ownerSessionId 是原子会话；界面不得把投影当作根会话可引用的普通消息。
struct MaiQuestionTranscriptMessage {
    std::string ownerSessionId;
    MaiMessage message;
};

struct MaiQuestionTranscript {
    // 先到先答；同一时刻用 ID 稳定排序，跨端不依赖闸门容器的遍历顺序。
    std::vector<MaiQuestionRequest> pending;
    // 只包含后代会话的 question 工具片段，不暴露其它正文、推理或工具内容。
    std::vector<MaiQuestionTranscriptMessage> childMessages;
};

// 判断 owner 是否等于所选会话，或沿 parentId 最终到达该会话。
// 只读、线程安全性与 MaiAgent 的存储查询一致；调用者在宿主工作线程调用。
// 空 ID、丢失会话、循环或超过 64 层的损坏父链均返回 false。
bool maiQuestionBelongsToSession(const MaiAgent& agent, const std::string& selectedSessionId,
                                 const std::string& ownerSessionId);

// 供宿主工作线程把子任务变更路由到所属根会话的同步房间。
// Core 事件回调不得调用此查询；空值表示会话不存在或父链损坏，不广播到其它房间。
std::string maiQuestionRootSessionId(const MaiAgent& agent, const std::string& sessionId);

// 捕获一次只读 UI 快照。优先最近活跃的后代，最多查询 64 个历史 owner，
// 各读取最近八条原消息，再补读全部当前 pending 的原消息（不受 owner 窗口限制），
// 保证正在等待的问题即使落在页外也可显示。最多保留 64 个投影，优先当前待回答项；
// 历史投影按 created/id 排序、去重，传输宿主再应用编码后的字节预算；
// 根会话的 messages/before/hasOlder 原分页语义不受影响。
// includeChildHistory=false 时仅查询待回答列表，不遍历或读取子任务消息；
// 宿主可在正文 delta 或缓存热切换期间复用原历史投影。
// 本结果仅供显示和显式 questionId 路由，不作为新的模型上下文或持久化记录。
MaiQuestionTranscript captureMaiQuestionTranscript(MaiAgent& agent, const std::string& selectedSessionId,
                                                   bool includeChildHistory = true);
