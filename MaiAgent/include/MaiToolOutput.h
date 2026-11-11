// 文件作用：定义独立于消息预览的工具原始输出及按字节分页契约。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "MaiError.h"
#include "MaiTime.h"

// 一次已结束捕获的原始字节快照；text 可以包含 NUL 和非法 UTF-8。
// id 为不可变句柄，必须精确归属于已落库的 session/message/tool part/call。
// 所有编号必须为非空、至多 128 字节、无 NUL 的合法 UTF-8。
// 存储成功只证明此快照已保存；truncated=true 表示达到容量上限，或执行/采集被中断，
// 无法声称它是正常结束操作的完整输出。
// 不含执行状态，不能据此推断命令是否成功或是否产生副作用。
struct MaiToolOutputRecord {
    std::string id;
    std::string sessionId;
    std::string messageId;
    std::string partId;
    std::string callId;
    std::string text;
    bool truncated = false;
    MaiMillis created = 0;
};

// 按原始源字节定位；maxBytes 范围为 1..4096。
// offsetBytes 必须位于合法 UTF-8 标量或单个非法字节的起点，可等于总长度。
// 页预算容不下首个完整标量时返回 InvalidInput，调用方须扩大预算。
struct MaiToolOutputQuery {
    std::string outputId;
    std::size_t offsetBytes = 0;
    std::size_t maxBytes = 4096;
};

// 返回独立快照。text 保留原始字节，textIsValidUtf8 只描述本页是否合法。
// sourceBytesRead 是本页消耗的源字节数，totalBytes 是保留快照的总字节数。
// nextOffsetBytes=0 表示读到末尾；其他值可直接作为下一页 offsetBytes。
// truncated 描述整个捕获是否不完整（容量上限、中断或采集故障），与是否还有下一页无关。
struct MaiToolOutputPage {
    std::string outputId;
    std::string messageId;
    std::string partId;
    std::string callId;
    std::string text;
    std::size_t offsetBytes = 0;
    std::size_t sourceBytesRead = 0;
    std::size_t totalBytes = 0;
    std::size_t nextOffsetBytes = 0;
    bool truncated = false;
    bool textIsValidUtf8 = true;
};

// 保留策略对两个标准后端一致；超出单条上限拒绝写入，绝不静默截断。
// 会话及全局上限通过 created、id 升序淘汰最旧快照满足。
constexpr std::size_t kMaiToolOutputMaxBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaiToolOutputSessionMaxBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaiToolOutputTotalMaxBytes = 256 * 1024 * 1024;

// 标准后端共用的无状态校验；失败返回 InvalidInput，不修改输入、不访问存储。
// 校验编号及单条字节上限；归属和不可变性仍须在后端锁/事务内检查。
MaiError maiValidateToolOutputRecord(const MaiToolOutputRecord& output);

// 校验非空会话/输出编号、页预算及绝不可能有效的超上限偏移。
// 无状态且允许并发；实际快照长度及 UTF-8 标量边界在分页时继续校验。
MaiError maiValidateToolOutputQuery(const std::string& sessionId, const MaiToolOutputQuery& query);

// 后端内部共用的纯分页函数，不做 IO；返回独立字节快照，允许并发调用。
// output 提供身份及截断标记，bytes 是源数据的局部窗口，windowOffset 为其绝对起点。
// 窗口必须覆盖请求偏移前至多 3 字节及页预算后至多 3 字节，totalBytes 为全文长度。
// 内存后端可以直接传入全文视图；SQLite 用有限 BLOB substr 避免加载全文。
// 调用方先核验归属；无效查询返回 InvalidInput，不完整窗口返回 Internal。
MaiResult<MaiToolOutputPage> maiPageToolOutput(const MaiToolOutputRecord& output,
                                               const MaiToolOutputQuery& query,
                                               std::string_view bytes, std::size_t windowOffset,
                                               std::size_t totalBytes);
