// 文件作用：无损保存会话值快照；这是内部持久化接口，不是模型协议或权限导入入口。
#pragma once

#include <string>

#include "MaiContextCheckpoint.h"
#include "MaiError.h"
#include "MaiMessage.h"
#include "MaiSession.h"
#include "MaiToolOutput.h"

// 无共享状态、无 IO，可并发调用；输入和返回值由调用方持有。
// 编码产生带 codec_version=1 的 UTF-8 对象；语义文本的合法 UTF-8 保持字符串，
// 仅非法 UTF-8 文本使用带类型标签的 base64 对象，NUL 原样往返。
// ToolOutputRecord.text 是原始二进制快照，始终用独立 $mai_binary/base64 包装，
// 防止合法的 16 MiB NUL 输出被 JSON 转义膨胀到 96 MiB；不改变其他文本或响应项。
// 结构、枚举、版本或身份关系不合法返回 InvalidInput，不输出半条记录。
// 此接口只验证值结构，不能验证消息归属或授予执行、文件访问、恢复权限。
// 恢复 contextImportOrigin 前，调用者必须核对权威存储来源及会话所有权；
// 解码结果不能作为普通用户写入路径建立只读来源事实的依据。
class MaiSessionRecordCodec {
public:
    // 真实计量的内部值编码；无原供应商 payload，不接受估计值或授予调用权限。
    // 无 IO、可并发；缺失/零值和来源等级无损保留，损坏输入返回 InvalidInput。
    static MaiResult<std::string> encodeContextUsage(const MaiModelContextUsage& value);
    static MaiResult<MaiModelContextUsage> decodeContextUsage(const std::string& source);
    static MaiResult<std::string> encodeSession(const MaiSession& value);
    static MaiResult<MaiSession> decodeSession(const std::string& source);
    static MaiResult<std::string> encodeMessage(const MaiMessage& value);
    static MaiResult<MaiMessage> decodeMessage(const std::string& source);
    static MaiResult<std::string> encodePart(const MaiMessagePart& value);
    static MaiResult<MaiMessagePart> decodePart(const std::string& source);
    static MaiResult<std::string> encodeCheckpoint(const MaiContextCheckpoint& value);
    static MaiResult<MaiContextCheckpoint> decodeCheckpoint(const std::string& source);
    // 仅原始输出的 text 接受二进制包装；兼容先前版本的 UTF-8 字符串/非法字节包装。
    static MaiResult<std::string> encodeToolOutput(const MaiToolOutputRecord& value);
    static MaiResult<MaiToolOutputRecord> decodeToolOutput(const std::string& source);

    // 只保存/恢复 turnId 和 turnOutcome，采用现有存储的稳定字段及枚举语义。
    // 所有上下文容量事实、错误恢复建议均完整保留；失败时 message 保持原值。
    // 空 source 仅为旧存储的“没有轮次状态”，成功时清空这两个字段。
    // 解码 outcome 前须设置初始 message.id 和 Assistant 角色，以便核对轮次归属。
    // 仅此入口兼容没有版本/kind 的旧存储元数据；明确的未知版本始终拒绝。
    static MaiResult<std::string> encodeTurnState(const MaiMessage& message);
    static MaiError decodeTurnState(const std::string& source, MaiMessage& message);
};
