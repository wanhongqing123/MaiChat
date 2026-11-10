// 文件作用：把历史消息、工具结果和附件整理为模型上下文。
// 对外类型：MaiContextBuilder；线程、失败和生命周期以类型附近注释为准。
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "MaiModelClient.h"
#include "MaiMessage.h"

// 把历史消息组装成给模型看的上下文。
//
// 单独抽出来，是因为这里将来会长出一堆策略，而它们都不该塞进 MaiAgent：
//   - 增量缓存：现在每轮 O(n) 重建整个历史，聊长了整体就是 O(n²)。
//     要改成保留已序列化的前缀、每轮只追加——这是计划里的一号性能风险。
//   - 显式上下文策略：默认完整保留历史文本和工具结果。窗口触发的语义压缩由
//     MaiContextCompactor 负责，先持久化有效摘要，再以完整原文轮次为边界替换请求前缀。
//   - 项目规则文件等随历史演进的上下文注入。稳定的基础指令属于 MaiModelRequest。
// 混在 MaiAgent 里就没地方放这些，也没法单独测。
class MaiContextBuilder {
public:
    struct Options {
        // 默认不回灌历史思考草稿，避免 Chat 与 Responses 的上下文出现差异。
        // 显式启用时才把草稿交给模型。
        bool includeReasoning = false;
        // 只保留当前用户轮次里最新的工具图片。旧像素仍在磁盘和会话存储中，
        // 但每轮都重新嵌入全部图片会让模型请求不断膨胀，最终耗尽流式期限。
        std::size_t maxRecentToolImages = 8;
        // 模型文本中只保留最近的用户图片引用；当前用户新发送的附件始终保留，
        // 不能被旧图集挤掉。
        std::size_t maxRecentUserImageReferences = 12;
        // 这四个文本预算的零值均表示不设限；默认不省略旧消息、不替换原文中段。
        // 非零值是宿主明确选择的有损策略，只影响请求副本，不改会话存储；
        // maxHistoryTextBytes 同时限制当前用户输入和总历史，另三项分别限制对应内容。
        // 本类不估算模型窗口；MaiContextCompactor 使用实际模型容量和 token 计数/估计。
        // 非零字节策略不是语义压缩，不能用于默认模型请求。图片/思考策略与文本预算独立。
        std::size_t maxHistoryTextBytes = 0;
        std::size_t maxToolResultBytes = 0;
        std::size_t maxOlderMessageBytes = 0;
        std::size_t maxReasoningBytes = 0;
    };

    MaiContextBuilder();
    explicit MaiContextBuilder(Options options);

    // firstMessage 为已成功摘要覆盖的源前缀长度，默认 0；firstPart 只跳过其 assistant
    // 稳定片段前缀。preservedUserMessageId 允许把部分 assistant 的当前 user 原文保留在前。
    // 越界/试图裁 user parts 返回空列表。引用仍在完整 history 中解析；图片预算只计剩余 tail。
    std::vector<MaiModelMessage> build(const std::vector<MaiMessage>& history,
                                       std::size_t firstMessage = 0, std::size_t firstPart = 0,
                                       const std::string& preservedUserMessageId = {}) const;

private:
    Options mOptions;
};
