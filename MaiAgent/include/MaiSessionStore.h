// 文件作用：定义会话和消息存储接口契约。
// 对外类型：MaiSessionStore；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiContextCheckpoint.h"
#include "MaiMessage.h"
#include "MaiSession.h"
#include "MaiSpecialistTask.h"
#include "MaiToolOutput.h"

// 持久化接口。
//
// 和领域模型分开：MaiSession / MaiMessage 是纯数据，它们不该知道持久化的存在。反过来放在一个头里，
// 任何人 include 领域模型就把存储接口也拖了进来。
//
// 第一天就抽接口的另一个原因：嵌入式上可能根本不落盘，或者换成别的 KV。
class MaiSessionStore : public MaiSpecialistTaskStore {
public:
    virtual ~MaiSessionStore() = default;

    virtual void putSession(const MaiSession& session) = 0;
    virtual bool getSession(const std::string& id, MaiSession& out) const = 0;
    virtual std::vector<MaiSession> listSessions() const = 0;  // 按 updated 倒序
    virtual bool removeSession(const std::string& id) = 0;

    // 清掉一个会话的全部消息，**会话本身留着**。会话不存在返回 false。
    //
    // 和 removeSession 的区别不只是留不留会话：删会话会连带清掉闸门里"本会话都允许"
    // 那份记录（MaiPermissionGate::forgetSession），而清空消息不碰它。
    // 用户点"清空重来"想丢的是聊天记录，不是他刚给过的授权。
    virtual bool clearMessages(const std::string& sessionId) = 0;

    virtual void putMessage(const std::string& sessionId, const MaiMessage& message) = 0;
    virtual std::vector<MaiMessage> listMessages(const std::string& sessionId) const = 0;

    // 保存已结束捕获的工具原始字节快照；与消息共享后端锁/事务。
    // 精确核对已落库的会话、消息、工具片段和 callId；不接受未落库或跨会话归属。
    // 同一 id 永不覆盖，重复写入返回 InvalidInput。单条超过 16 MiB 同样拒绝。
    // 成功写入可按 created/id 淘汰最旧快照，将会话/全局字节数限制在 64/256 MiB。
    // 清空消息或删除会话同步清理其快照；返回值只说明快照保存，不代表执行成功。
    // 自定义后端默认 NotSupported；不得把失败误报为可恢复的输出句柄。
    virtual MaiError putToolOutput(const MaiToolOutputRecord& output);

    // 仅读取 sessionId 内的不可变快照，允许与普通写入并发，返回值归调用方持有。
    // 不存在、已淘汰、跨会话或原始工具归属已失效时返回 NotFound。
    // 分页预算、UTF-8 边界与结束游标按 MaiToolOutputQuery/Page 契约；不载入全文。
    // 参数无效返回 InvalidInput，后端故障返回 Internal，默认返回 NotSupported。
    virtual MaiResult<MaiToolOutputPage> readToolOutput(const std::string& sessionId,
                                                        const MaiToolOutputQuery& query) const;

    // 用户输入的持久接收凭据及未消费队列。返回独立快照；空编号不匹配。
    // 默认实现复用 listMessages，SQLite 实现使用索引，避免重试读取整个会话历史。
    virtual bool getInputMessage(const std::string& sessionId, const std::string& requestId,
                                 MaiMessage& out) const;
    virtual std::vector<MaiMessage> listQueuedInputs(const std::string& sessionId) const;

    // 只读取一个会话的一页消息，按新到旧返回。beforeId 为空时从最新消息开始；
    // 非空时只返回 id 小于它的消息，不包含游标本身。limit 为 0、会话不存在或
    // 查询失败时返回空列表；调用方要区分会话不存在，先用 getSession。
    // 返回的是调用时已落库的快照，可与其他线程的写入并发使用，结果由调用方持有。
    // id 由 MaiIdGenerator 按生成时间排序，因此后续新消息不会改变旧页的游标边界。
    virtual std::vector<MaiMessage> listMessagesPage(const std::string& sessionId,
                                                     const std::string& beforeId,
                                                     std::size_t limit) const = 0;

    // 进程重启时，上次遗留的 Pending/Running 工具调用已没有执行线程。
    // 原子地把这些工具标为 Error，并给所属的未完成消息补 completed 时间。
    // 只修改符合条件的记录，不向调用方加载其他会话的完整消息；无匹配项是空操作。
    // error 是回灌给模型及界面的真实错误文本，调用方负责提供；允许与普通读并发。
    // 原 output 保留；Pending 的执行事实为 NotStarted/None，Running 则为 Unknown/Unknown，
    // 提示 InspectState；已有 taskId 则保留 QueryStatus 入口。
    // 不能据重启错误声称无副作用或建议直接重放原操作。
    virtual void recoverInterruptedTools(const std::string& error, std::int64_t completedAt) = 0;

    // 最近一次写入失败了吗？没失败返回一个空的 MaiError。
    //
    // 为什么写接口返回 void、错误另开一个查询：putMessage 在流式的热路径上被反复调用，
    // 每个调用点都 if 一下会把代码淹掉，而且那些地方也做不了什么补救。
    // 但**失败不能无声无息**——磁盘满了还假装存上了，是用户第二天打开发现对话没了的那种 bug。
    //
    // 约定：一轮结束时查一次（MaiTurnRunner::finish），失败就把会话标成
    // 错误让界面看见。
    virtual MaiError lastWriteError() const = 0;

    // 原子地改会话的一部分字段。
    //
    // 存在的理由是个真 bug：之前跑一轮的流程是"开头读会话、结尾改标题写回"，
    // 中间如果别人改了 model 或 agent，收尾那一写会把人家的改动盖掉（lost update）。
    // 让存储层在锁内做读-改-写，调用方就碰不到这个窗口。返回 false 表示会话不存在。
    virtual bool mutateSession(const std::string& id,
                               const std::function<void(MaiSession&)>& mutator) = 0;

    // 精确读取指定会话中的一条消息及其已落库片段；不使用游标或 ID 前缀推算。
    // 消息/会话不存在、归属不符或查询失败时返回 false，out 保持原值。
    // 与写入共用后端锁，允许活动轮次期间并发读取；返回快照归调用方持有。
    // 标准 SQLite 后端仅查询该消息，不加载同会话的其他消息/工具载荷。
    virtual bool getMessage(const std::string& sessionId, const std::string& messageId,
                            MaiMessage& out) const = 0;

    // 读取会话独立的压缩检查点。不存在/读取失败返回 false，out 不变。
    // 自定义后端默认不提供检查点，原始历史仍可完整读取；需覆盖两个方法才支持压缩。
    virtual bool getContextCheckpoint(const std::string& sessionId,
                                      MaiContextCheckpoint& out) const;
    // 在同一锁/事务内核对 expectedRevision、会话身份及完整源前缀后推进。
    // 首次 expectedRevision=0，候选 revision 必须为 expectedRevision+1。
    // 并发追加在消息/part 覆盖边界之后允许；partial assistant 收尾不会改变已覆盖前缀。
    // 源前缀修改、清空或其他压缩已推进则返回 Busy。
    // 成功只写检查点，不删除/缩短任何原消息；失败不修改原检查点或原历史。
    virtual MaiError advanceContextCheckpoint(const std::string& sessionId,
                                              std::uint64_t expectedRevision,
                                              const MaiContextCheckpoint& checkpoint);
};

// 两个实现的工厂在各自的头里：MaiMemoryStore.h / MaiSqliteStore.h。
//
// 不都堆在这个头上，是因为这里是**接口**——自己实现一个存储的人只需要这个文件，
// 不该被迫看见我们碰巧提供了哪两个实现。
