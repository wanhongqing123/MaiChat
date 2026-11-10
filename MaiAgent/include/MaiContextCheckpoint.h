// 文件作用：定义不改写原始历史的持久压缩检查点及其源快照身份。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "MaiMessage.h"

// 一个会话只有一个当前检查点。摘要是历史数据，不是新的系统/用户指令。
// 存储在同一锁/事务内核对 revision 和完整源前缀指纹后才推进；原始消息从不删除。
// 字段由压缩器生成，界面/同步不能把任意摘要直接当作一个有效检查点写入。
struct MaiContextCheckpoint {
    std::string sessionId;
    std::uint64_t revision = 0;
    // 完整源前缀的长度；若有 preservedUserMessageId，该 user 身份参与校验但仍保留原文。
    // sourceFingerprint 核对此前完整消息，保持 v6 指纹格式以复用已有检查点。
    std::size_t sourceMessageCount = 0;
    std::string firstMessageId;
    std::string throughMessageId;
    std::string sourceFingerprint;
    std::string model;
    // 经 schema 校验的模型摘要 JSON；不包含/替换原消息，不得当作新指令执行。
    std::string summary;
    // 独立于模型摘要保存的精确链接/路径及源消息、片段 ID；模型遗漏引用也仍可回查。
    std::string references;
    MaiMillis created = 0;
    // 可选的下一条 assistant 的稳定 parts 前缀。空 ID 表示只有完整消息边界；
    // 非空时它必须等于 history[sourceMessageCount]，partCount/throughPartId 精确限定源。
    // 部分边界忽略 assistant.completed 与未覆盖 tail，所以追加结果及收尾不使摘要失效。
    std::string partialMessageId;
    std::size_t partialPartCount = 0;
    std::string throughPartId;
    std::string partialSourceFingerprint;
    // 部分 assistant 对应的最新 user，位于完整源前缀内；始终作为当前指令原文保留。
    std::string preservedUserMessageId;
};

// SHA-256 覆盖会话身份、消息/片段顺序及所有原字段，字段以长度分隔，避免拼接歧义。
// count 越界或摘要算法失败返回空串；函数不读取外部数据，允许并发调用。
std::string maiContextSourceFingerprint(const std::string& sessionId,
                                        const std::vector<MaiMessage>& history, std::size_t count);

// 对 sourceFingerprint、保留的 user 身份、partial assistant 身份及已覆盖 parts 计算 SHA-256。
// 不含 assistant.completed、parts 总数或未覆盖内容；非法边界返回空串。
std::string maiContextPartialSourceFingerprint(const MaiContextCheckpoint& checkpoint,
                                               const std::vector<MaiMessage>& history);

// 仅在摘要非空、源前缀仍匹配且边界属于这个会话时为真；不会改写检查点或历史。
bool maiContextCheckpointMatches(const MaiContextCheckpoint& checkpoint,
                                 const std::string& sessionId,
                                 const std::vector<MaiMessage>& history);
