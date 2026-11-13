#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 仅保存路由控制值；不调用 SDK、模型、存储或文件系统，也不创建线程/轮询器。
// Host 保留消息正文/附件，以 Admission.inputId 查找不可变的自有 payload。
class MaiSessionIMRouter final {
public:
    static constexpr std::size_t kPendingLimit = 32;
    static constexpr std::size_t kRememberedInputLimit = 512;

    enum class Status {
        Ok,
        NotActive,
        StaleScope,
        InvalidArgument,
        NotEligible,
        NotBound,
        Conflict,
        Duplicate,
        QueueFull,
        Busy,
        InvalidToken,
        InvalidReceipt,
        InvalidPhase,
        Exhausted
    };
    enum class Origin { Unknown, Human, Machine };
    enum class Phase { Reserved, Running, Finishing };
    enum class HistoryMatch { None, AwaitingPersistence, LiveInput, Stale };

    struct AccountScope {
        std::uint64_t sdkAppId = 0;
        std::string ownerUserId;
        std::uint64_t authGeneration = 0;
        std::uint64_t controllerGeneration = 0;
        bool operator==(const AccountScope& other) const;
        bool operator!=(const AccountScope& other) const { return !(*this == other); }
    };

    struct Binding {
        AccountScope account;
        std::string peerId;
        std::string sessionId;
        std::string workspaceKey;
        std::uint64_t epoch = 0;
        bool historyPending = false;
    };

    struct Incoming {
        AccountScope account;
        std::string peerId;
        std::string messageId;
        std::string senderUserId;
        std::string recipientUserId;
        std::uint64_t expectedBindingEpoch = 0;
        std::string expectedSessionId;
        Origin origin = Origin::Unknown;
        bool live = false;
        bool group = false;
        bool controlFrame = false;
        bool hasContent = false;
        bool requirePersistence = false;
    };

    struct QueuedInput {
        std::uint64_t inputId = 0;
        Incoming source;
        Binding binding;
        bool persistenceKnown = true;
        std::string persistedRowId;
        std::string persistedRemoteId;
    };
    struct Admission {
        Status status = Status::NotActive;
        // 只有 Ok 才返回新 inputId；正文/附件仍由 Host 保存，不经过工具 JSON。
        std::optional<QueuedInput> input;
    };
    struct Reservation {
        std::uint64_t token = 0;
        QueuedInput input;
    };
    struct ReservationResult {
        Status status = Status::NotActive;
        std::optional<Reservation> reservation;
    };
    struct TurnReceipt {
        std::string userMessageId;
        std::string assistantMessageId;
        std::string turnId;
        bool operator==(const TurnReceipt& other) const;
        bool operator!=(const TurnReceipt& other) const { return !(*this == other); }
    };
    struct ActiveRoute {
        Reservation reservation;
        Phase phase = Phase::Reserved;
        std::optional<TurnReceipt> receipt;
        std::uint32_t dispatchCount = 0;
        std::uint32_t acknowledgedDispatches = 0;
    };
    struct Snapshot {
        Status status = Status::NotActive;
        std::vector<Binding> bindings;
        std::vector<QueuedInput> pending;
        std::vector<ActiveRoute> active;
        // 包括尚在准备的 Reserved 输入，不包括已交给普通 MaiSendPrompt 的 Running。
        std::size_t pendingCount = 0;
        std::size_t rememberedInputs = 0;
    };

    MaiSessionIMRouter();
    ~MaiSessionIMRouter();
    MaiSessionIMRouter(const MaiSessionIMRouter&) = delete;
    MaiSessionIMRouter& operator=(const MaiSessionIMRouter&) = delete;

    // 新 scope 不继承旧绑定/队列/凭据；Host 可从已有偏好显式恢复绑定。
    // authGeneration 须在账号/连接授权边界改变；两个 generation 均是 Host 的不透明值。
    Status activate(const AccountScope& scope);
    Status logout(const AccountScope& expectedScope);
    Status bind(const AccountScope& scope, const std::string& peerId,
                const std::string& sessionId, const std::string& workspaceKey,
                bool friendValidated, bool rootSessionValidated);
    Status unbind(const AccountScope& scope, const std::string& peerId);
    // 历史是独立持久上下文，不占 live 槽。准备期间仍接收原 live 元数据，但不预约模型输入。
    Status beginHistory(const AccountScope& scope, const std::string& peerId, std::uint64_t epoch);
    Status completeHistory(const AccountScope& scope, const std::string& peerId, std::uint64_t epoch);
    // 资格来自普通 DB 元数据事务，不来自 UI 窗口。重复旧行撤掉 live 槽，仍作为历史上下文。
    Status confirmPersisted(const AccountScope& scope, std::uint64_t inputId, bool inserted,
                            const std::string& rowId, const std::string& remoteId = {});
    HistoryMatch historyMatch(const AccountScope& scope, const std::string& peerId, std::uint64_t epoch,
                              const std::string& rowId, const std::string& remoteId,
                              const std::string& senderId, const std::string& recipientId) const;
    // 本地接管、Session 删除或工作目录改变共用此撤销；不停止模型/工具。
    Status revokeSession(const AccountScope& scope, const std::string& sessionId);

    Admission accept(const Incoming& input);
    // 下载/准备失败只丢弃准确输入；Running/Finishing 已提交输入不能经此接口取消。
    Status discardInput(const AccountScope& scope, std::uint64_t inputId);
    ReservationResult reserveNext(const AccountScope& scope, const std::string& sessionId,
                                  const std::string& currentWorkspaceKey, bool sessionIdle);
    bool isCurrent(std::uint64_t token, const AccountScope& scope,
                   const std::string& currentWorkspaceKey) const;
    // 准备失败可丢弃；普通 submit 返回 Busy 可原样放回队首，仍保留去重身份。
    Status releaseReservation(std::uint64_t token, const AccountScope& scope,
                              const std::string& currentWorkspaceKey, bool retryAtFront);
    // 仅绑定普通 submit 的准确 user/assistant/turn 回执，绝不选择最新消息。
    Status bindReceipt(std::uint64_t token, const TurnReceipt& receipt,
                       const AccountScope& scope, const std::string& currentWorkspaceKey);
    // Host 先核对 canonical assistant 已结束、正文/媒体可交付，再声明 SDK 派发数。
    // 重复调用不会重置已确认项；只有首次 Ok 才应开始派发。
    Status beginOutput(std::uint64_t token, const TurnReceipt& receipt,
                       const AccountScope& scope, const std::string& currentWorkspaceKey,
                       std::uint32_t dispatchCount);
    bool isOutputCurrent(std::uint64_t token, const TurnReceipt& receipt,
                         const AccountScope& scope, const std::string& currentWorkspaceKey) const;
    // index 范围为 [0, dispatchCount)，失败的准备也须确认已终结；重复回调不推进 FIFO。
    // 最后一项确认后才释放该 Session。此处确认 SDK 派发/放弃，不等待网络送达回执。
    Status acknowledgeDispatch(std::uint64_t token, const TurnReceipt& receipt,
                               const AccountScope& scope, const std::string& currentWorkspaceKey,
                               std::uint32_t index);
    Status finishWithoutOutput(std::uint64_t token, const TurnReceipt& receipt,
                               const AccountScope& scope, const std::string& currentWorkspaceKey);

    Snapshot snapshot(const AccountScope& scope) const;
    std::optional<Binding> bindingForPeer(const AccountScope& scope, const std::string& peerId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
