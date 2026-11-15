#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "MaiContextImport.h"
#include "MaiIMHistorySnapshot.h"
#include "MaiMessage.h"

struct MaiSession;

// 只把普通持久消息的安全元数据转成带出处的引用上下文；不访问文件或执行模型。
// 不构造 assistant、工具、像素或媒体交付片段，也不赋予引用路径的读取/发送权限。
MaiResult<MaiContextImportRecord> maiFormatIMHistoryContextRecord(const MaiIMHistoryRecord& record,
                                                                  const MaiIMHistorySource& source);

// 这是显示状态，不是 Core 消息角色、模型输入、待消费输入或媒体交付回执。
enum class MaiIMHistoryDisplayState { NotImported, Available, Unavailable };

// 引用元数据只作占位展示；不返回本地/封面路径，也不证明文件可读取或允许发送。
struct MaiIMHistoryDisplayMedia {
    std::string kind;
    std::string name;
    std::string mimeType;
    std::uint64_t sizeBytes = 0;
    std::uint64_t durationMillis = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool referenceMetadataPresent = false;
    bool metadataUnavailable = false;
};

// 原 IM 引用不是 Core 的消息 ID；只给摘要，不提供跳转或文件访问能力。
struct MaiIMHistoryDisplayQuote {
    std::string digest;
    std::string kind;
};

struct MaiIMHistoryDisplayRecord {
    std::string text;
    MaiIMHistoryDirection direction = MaiIMHistoryDirection::Unknown;
    std::string kind;
    std::int64_t createdAtMillis = 0;
    bool captionAbove = false;
    std::optional<MaiIMHistoryDisplayQuote> quote;
    std::vector<MaiIMHistoryDisplayMedia> media;
};

struct MaiIMHistoryDisplay {
    MaiIMHistoryDisplayState state = MaiIMHistoryDisplayState::NotImported;
    MaiIMHistoryDisplayRecord record;
};

// 仅接受标准存储回传的 IM 导入来源事实；普通用户粘贴相同前缀/JSON 不会被解包。
// 可信 IM 来源的完成 User/单 Text 及完整旧格式校验通过才返回 Available；原文最多
// 按 JSON 解码一次，不递归处理正文，不改 message/model text，也不执行任何媒体 I/O。
// 可信 IM 导入损坏时返回 Unavailable 且 record 为空，界面不得回退展示内部封装。
// 不是 IM 的普通消息/通用导入返回 NotImported。调用不保存指针，可在后台线程使用。
MaiIMHistoryDisplay maiProjectIMHistoryForDisplay(const MaiMessage& message);

// 只读显示修复：先用候选检查避免普通标题触发历史读取，再传标准存储顺序的原史。
// 仅在未人工编辑、首条 User 是可信 IM 导入且旧标题精确来自其封装时，
// 改为首条真实用户输入的短标题；没有真实输入返回空，沿用界面“新对话”占位。
// 不写会话/消息，不按前缀批量改名；普通同名文本与人工标题保持原样。
bool maiNeedsIMHistoryTitleProjection(const MaiSession& session);
std::string maiProjectIMHistoryTitleForDisplay(const MaiSession& session,
                                              const std::vector<MaiMessage>& history);
