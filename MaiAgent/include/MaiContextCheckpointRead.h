// 文件作用：只读检查当前会话保存的压缩摘要和完整引用表；不让模型复述代替存档。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "MaiError.h"
#include "MaiTime.h"

class MaiSessionStore;

enum class MaiContextCheckpointSection { Metadata, Summary, References };
enum class MaiContextReferenceIndexState { Unavailable, Available, Invalid, MetadataBudgetExceeded };

struct MaiContextCheckpointReadQuery {
    MaiContextCheckpointSection section = MaiContextCheckpointSection::Summary;
    // 0 选择当前保存版本；继续分页必须带首屏返回的 revision，变化时返回 Busy，不混页。
    std::uint64_t expectedRevision = 0;
    // Summary/References 均是原存档字段的字节分页，不假装片段是完整 JSON。
    std::size_t offsetBytes = 0;
    std::size_t maxBytes = 8192;
};

struct MaiContextCheckpointInfo {
    std::uint64_t revision = 0;
    MaiMillis created = 0;
    std::string model;
    std::size_t sourceMessageCount = 0;
    std::string firstMessageId;
    std::string throughMessageId;
    std::string partialMessageId;
    std::size_t partialPartCount = 0;
    std::string throughPartId;
    std::string preservedUserMessageId;
    std::size_t summaryBytes = 0;
    std::size_t referenceBytes = 0;
    bool summaryIsJsonObject = false;
    // 仅统计已知六个数组，不声称自然语言摘要无损或正确。读取不改写/修复旧存档。
    std::map<std::string, std::size_t> summaryArrayCounts;
    MaiContextReferenceIndexState referenceIndexState = MaiContextReferenceIndexState::Unavailable;
    bool referenceCountKnown = false;
    std::size_t referenceCount = 0;
};

struct MaiContextCheckpointPage {
    bool available = false;
    MaiContextCheckpointInfo info;
    MaiContextCheckpointSection section = MaiContextCheckpointSection::Summary;
    // 普通 API 始终保留原字节；合法 UTF-8 标量不拆开，旧非法字节按单字节前进。
    std::string text;
    std::size_t offsetBytes = 0;
    std::size_t totalBytes = 0;
    std::size_t sourceBytesRead = 0;
    // 已读到末尾时为0；否则是offsetBytes+sourceBytesRead，只在hasMore=true时继续。
    std::size_t nextOffsetBytes = 0;
    bool hasMore = false;
    bool textIsValidUtf8 = true;
};

// 同步普通入口；sessionId 由已授权宿主提供。只读该会话当前检查点，不调用模型、工具或媒体。
// 首次没有检查点时 available=false；失效的指定 revision 返回 Busy，调用方重新读首屏。
// maxBytes 为源字段预算（4..65536），Metadata 不接收偏移；offset>0 必须指定 revision。
// 返回的是保存的检查点，不是实际最后一次模型请求，也不重新验证原史指纹。完整引用表和
// 摘要均可逐字节读回；元数据解析超过32MiB引用/1MiB摘要时不解析，计数明确为未知。
// 可与压缩提交并发；版本比较与本页内容来自同一次 store 检查点副本。UI 须在工作线程调用。
MaiResult<MaiContextCheckpointPage> maiReadContextCheckpoint(
    const MaiSessionStore& store, const std::string& sessionId,
    const MaiContextCheckpointReadQuery& query);
