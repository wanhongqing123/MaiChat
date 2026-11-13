#include "MaiSessionIMRouter.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace {

bool hasValue(const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos &&
        std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\v' && ch != '\f';
        });
}

std::uint64_t nextId(std::uint64_t& counter) {
    // 不环绕复用能力编号，旧回调永远不能因计数器回绕而获得新权限。
    return counter == std::numeric_limits<std::uint64_t>::max() ? 0 : counter++;
}

bool validReceipt(const MaiSessionIMRouter::TurnReceipt& receipt) {
    return hasValue(receipt.userMessageId) && hasValue(receipt.assistantMessageId) && hasValue(receipt.turnId) &&
        receipt.userMessageId != receipt.assistantMessageId;
}

}  // namespace

struct MaiSessionIMRouter::Impl {
    struct State {
        ActiveRoute route;
        std::set<std::uint32_t> acknowledged;
    };
    using InputKey = std::tuple<std::uint64_t, std::string, std::string, std::string>;
    mutable std::mutex mutex;
    std::optional<AccountScope> account;
    std::map<std::string, Binding> bindings;
    std::map<std::string, std::string> peerBySession;
    std::map<std::string, std::deque<QueuedInput>> queues;
    std::map<std::string, State> active;
    std::map<std::uint64_t, std::string> tokenSessions;
    std::set<InputKey> seen;
    std::deque<InputKey> seenOrder;
    std::size_t pendingCount = 0;
    std::uint64_t nextEpoch = 1;
    std::uint64_t nextInput = 1;
    std::uint64_t nextToken = 1;

    Status scopeStatus(const AccountScope& scope) const {
        return !account ? Status::NotActive : *account != scope ? Status::StaleScope : Status::Ok;
    }
    void clearRoutesAndBindings() {
        bindings.clear();
        peerBySession.clear();
        queues.clear();
        active.clear();
        tokenSessions.clear();
        pendingCount = 0;
    }
    void eraseRoute(const std::string& session) {
        const auto found = active.find(session);
        if (found == active.end()) return;
        if (found->second.route.phase == Phase::Reserved) --pendingCount;
        tokenSessions.erase(found->second.route.reservation.token);
        active.erase(found);
    }
    void revokePeer(const std::string& peer) {
        const auto found = bindings.find(peer);
        if (found == bindings.end()) return;
        const std::string session = found->second.sessionId;
        const auto queue = queues.find(session);
        if (queue != queues.end()) {
            pendingCount -= queue->second.size();
            queues.erase(queue);
        }
        eraseRoute(session);
        peerBySession.erase(session);
        bindings.erase(found);
    }
    State* state(std::uint64_t token) {
        const auto found = tokenSessions.find(token);
        if (found == tokenSessions.end()) return nullptr;
        const auto route = active.find(found->second);
        return route == active.end() ? nullptr : &route->second;
    }
    const State* state(std::uint64_t token) const {
        const auto found = tokenSessions.find(token);
        if (found == tokenSessions.end()) return nullptr;
        const auto route = active.find(found->second);
        return route == active.end() ? nullptr : &route->second;
    }
    Status authority(std::uint64_t token, const AccountScope& scope, const std::string& workspace) const {
        const auto status = scopeStatus(scope);
        if (status != Status::Ok) return status;
        const auto* found = state(token);
        if (!found) return Status::InvalidToken;
        const auto& captured = found->route.reservation.input.binding;
        const auto binding = bindings.find(captured.peerId);
        if (binding == bindings.end() || binding->second.epoch != captured.epoch ||
            binding->second.sessionId != captured.sessionId || binding->second.account != scope)
            return Status::InvalidToken;
        if (workspace != captured.workspaceKey || workspace != binding->second.workspaceKey)
            return Status::StaleScope;
        return Status::Ok;
    }
    bool remember(const Incoming& input) {
        const InputKey key{input.account.sdkAppId, input.account.ownerUserId, input.peerId, input.messageId};
        if (!seen.insert(key).second) return false;
        seenOrder.push_back(key);
        while (seenOrder.size() > kRememberedInputLimit) {
            seen.erase(seenOrder.front());
            seenOrder.pop_front();
        }
        return true;
    }
};

MaiSessionIMRouter::MaiSessionIMRouter() : impl_(std::make_unique<Impl>()) {}
MaiSessionIMRouter::~MaiSessionIMRouter() = default;
bool MaiSessionIMRouter::AccountScope::operator==(const AccountScope& other) const {
    return sdkAppId == other.sdkAppId && ownerUserId == other.ownerUserId &&
        authGeneration == other.authGeneration && controllerGeneration == other.controllerGeneration;
}
bool MaiSessionIMRouter::TurnReceipt::operator==(const TurnReceipt& other) const {
    return userMessageId == other.userMessageId && assistantMessageId == other.assistantMessageId && turnId == other.turnId;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::activate(const AccountScope& scope) {
    if (!scope.sdkAppId || !hasValue(scope.ownerUserId)) return Status::InvalidArgument;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->account || *impl_->account != scope) {
        impl_->clearRoutesAndBindings();
        impl_->account = scope;
    }
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::logout(const AccountScope& scope) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    impl_->clearRoutesAndBindings();
    impl_->account.reset();
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::bind(const AccountScope& scope, const std::string& peer,
        const std::string& session, const std::string& workspace, bool friendValidated, bool rootSessionValidated) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    if (!hasValue(peer) || peer == scope.ownerUserId || !hasValue(session) || !hasValue(workspace))
        return Status::InvalidArgument;
    if (!friendValidated || !rootSessionValidated) return Status::NotEligible;
    const auto existingSession = impl_->peerBySession.find(session);
    if (existingSession != impl_->peerBySession.end() && existingSession->second != peer) return Status::Conflict;
    const auto existing = impl_->bindings.find(peer);
    if (existing != impl_->bindings.end() && existing->second.sessionId == session &&
        existing->second.workspaceKey == workspace) return Status::Ok;
    const auto epoch = nextId(impl_->nextEpoch);
    if (!epoch) return Status::Exhausted;
    impl_->revokePeer(peer);
    impl_->bindings.emplace(peer, Binding{scope, peer, session, workspace, epoch});
    impl_->peerBySession[session] = peer;
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::unbind(const AccountScope& scope, const std::string& peer) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    if (!hasValue(peer)) return Status::InvalidArgument;
    if (!impl_->bindings.count(peer)) return Status::NotBound;
    impl_->revokePeer(peer);
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::beginHistory(const AccountScope& scope,
        const std::string& peer, std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    const auto binding = impl_->bindings.find(peer);
    if (binding == impl_->bindings.end()) return Status::NotBound;
    if (binding->second.epoch != epoch) return Status::StaleScope;
    if (impl_->active.count(binding->second.sessionId)) return Status::Busy;
    binding->second.historyPending = true;
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::completeHistory(const AccountScope& scope,
        const std::string& peer, std::uint64_t epoch) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    const auto binding = impl_->bindings.find(peer);
    if (binding == impl_->bindings.end()) return Status::NotBound;
    if (binding->second.epoch != epoch) return Status::StaleScope;
    if (!binding->second.historyPending) return Status::Duplicate;
    binding->second.historyPending = false;
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::revokeSession(const AccountScope& scope, const std::string& session) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    if (!hasValue(session)) return Status::InvalidArgument;
    std::vector<std::string> peers;
    for (const auto& binding : impl_->bindings)
        if (binding.second.sessionId == session) peers.push_back(binding.first);
    if (peers.empty()) return Status::NotBound;
    for (const auto& peer : peers) impl_->revokePeer(peer);
    return Status::Ok;
}

MaiSessionIMRouter::Admission MaiSessionIMRouter::accept(const Incoming& input) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(input.account);
    if (status != Status::Ok) return {status, {}};
    if (!hasValue(input.peerId) || input.peerId == input.account.ownerUserId || !hasValue(input.messageId))
        return {Status::InvalidArgument, {}};
    if (!input.live || input.group || input.controlFrame || !input.hasContent || input.origin != Origin::Human ||
        input.senderUserId != input.peerId || input.recipientUserId != input.account.ownerUserId)
        return {Status::NotEligible, {}};
    const auto binding = impl_->bindings.find(input.peerId);
    if (binding == impl_->bindings.end()) return {Status::NotBound, {}};
    if (!input.expectedBindingEpoch || !hasValue(input.expectedSessionId)) return {Status::InvalidArgument, {}};
    if (input.expectedBindingEpoch != binding->second.epoch || input.expectedSessionId != binding->second.sessionId)
        return {Status::StaleScope, {}};
    if (!impl_->remember(input)) return {Status::Duplicate, {}};
    // 满队列也是一次明确拒绝，不能稍后把同一 SDK 回调当新指令自动重放。
    if (impl_->pendingCount >= kPendingLimit) return {Status::QueueFull, {}};
    const auto id = nextId(impl_->nextInput);
    if (!id) return {Status::Exhausted, {}};
    QueuedInput queued{id, input, binding->second, !input.requirePersistence, {}, {}};
    impl_->queues[binding->second.sessionId].push_back(queued);
    ++impl_->pendingCount;
    return {Status::Ok, queued};
}

MaiSessionIMRouter::ReservationResult MaiSessionIMRouter::reserveNext(const AccountScope& scope,
        const std::string& session, const std::string& workspace, bool sessionIdle) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return {status, {}};
    const auto peer = impl_->peerBySession.find(session);
    if (peer == impl_->peerBySession.end()) return {Status::NotBound, {}};
    if (impl_->bindings.at(peer->second).workspaceKey != workspace) return {Status::StaleScope, {}};
    if (impl_->bindings.at(peer->second).historyPending) return {Status::Busy, {}};
    if (!sessionIdle || impl_->active.count(session)) return {Status::Busy, {}};
    const auto queue = impl_->queues.find(session);
    if (queue == impl_->queues.end() || queue->second.empty()) return {Status::Ok, {}};
    if (!queue->second.front().persistenceKnown) return {Status::Busy, {}};
    const auto token = nextId(impl_->nextToken);
    if (!token) return {Status::Exhausted, {}};
    Reservation reservation{token, queue->second.front()};
    queue->second.pop_front();
    if (queue->second.empty()) impl_->queues.erase(queue);
    ActiveRoute route;
    route.reservation = reservation;
    impl_->active.emplace(session, Impl::State{route, {}});
    impl_->tokenSessions.emplace(token, session);
    return {Status::Ok, reservation};
}

MaiSessionIMRouter::Status MaiSessionIMRouter::confirmPersisted(const AccountScope& scope, std::uint64_t inputId,
        bool inserted, const std::string& rowId, const std::string& remoteId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    if (!inputId || !hasValue(rowId)) return Status::InvalidArgument;
    const auto confirm = [&](QueuedInput& input) {
        if (input.persistenceKnown && !input.persistedRowId.empty())
            return input.persistedRowId == rowId && input.persistedRemoteId == remoteId
                ? Status::Duplicate : Status::InvalidArgument;
        input.persistenceKnown = true;
        input.persistedRowId = rowId;
        input.persistedRemoteId = remoteId;
        return Status::Ok;
    };
    for (auto queue = impl_->queues.begin(); queue != impl_->queues.end(); ++queue) {
        const auto found = std::find_if(queue->second.begin(), queue->second.end(), [inputId](const QueuedInput& item) {
            return item.inputId == inputId;
        });
        if (found == queue->second.end()) continue;
        if (inserted) return confirm(*found);
        if (found->persistenceKnown && !found->persistedRowId.empty()) return Status::InvalidArgument;
        queue->second.erase(found);
        --impl_->pendingCount;
        if (queue->second.empty()) impl_->queues.erase(queue);
        return Status::Ok;
    }
    for (auto& active : impl_->active) {
        if (active.second.route.reservation.input.inputId != inputId) continue;
        if (active.second.route.phase != Phase::Reserved) return Status::InvalidPhase;
        if (inserted) return confirm(active.second.route.reservation.input);
        const std::string session = active.first;
        impl_->eraseRoute(session);
        return Status::Ok;
    }
    return Status::InvalidToken;
}

MaiSessionIMRouter::HistoryMatch MaiSessionIMRouter::historyMatch(const AccountScope& scope,
        const std::string& peer, std::uint64_t epoch, const std::string& rowId, const std::string& remoteId,
        const std::string& senderId, const std::string& recipientId) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->scopeStatus(scope) != Status::Ok) return HistoryMatch::Stale;
    const auto binding = impl_->bindings.find(peer);
    if (binding == impl_->bindings.end() || binding->second.epoch != epoch) return HistoryMatch::Stale;
    const auto classify = [&](const QueuedInput& input) {
        if (input.binding.peerId != peer || input.binding.epoch != epoch ||
            input.source.senderUserId != senderId || input.source.recipientUserId != recipientId)
            return HistoryMatch::None;
        const bool same = input.source.messageId == rowId ||
            (!remoteId.empty() && input.source.messageId == remoteId) ||
            (!input.persistedRowId.empty() && input.persistedRowId == rowId) ||
            (!remoteId.empty() && !input.persistedRemoteId.empty() && input.persistedRemoteId == remoteId);
        if (!same) return HistoryMatch::None;
        return input.persistenceKnown ? HistoryMatch::LiveInput : HistoryMatch::AwaitingPersistence;
    };
    const auto queue = impl_->queues.find(binding->second.sessionId);
    if (queue != impl_->queues.end())
        for (const auto& input : queue->second) {
            const auto result = classify(input);
            if (result != HistoryMatch::None) return result;
        }
    const auto active = impl_->active.find(binding->second.sessionId);
    return active == impl_->active.end() ? HistoryMatch::None : classify(active->second.route.reservation.input);
}

MaiSessionIMRouter::Status MaiSessionIMRouter::discardInput(const AccountScope& scope, std::uint64_t inputId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->scopeStatus(scope);
    if (status != Status::Ok) return status;
    if (!inputId) return Status::InvalidArgument;
    for (auto queue = impl_->queues.begin(); queue != impl_->queues.end(); ++queue) {
        const auto found = std::find_if(queue->second.begin(), queue->second.end(), [inputId](const QueuedInput& item) {
            return item.inputId == inputId;
        });
        if (found == queue->second.end()) continue;
        queue->second.erase(found);
        --impl_->pendingCount;
        if (queue->second.empty()) impl_->queues.erase(queue);
        return Status::Ok;
    }
    for (const auto& route : impl_->active) {
        if (route.second.route.reservation.input.inputId != inputId) continue;
        if (route.second.route.phase != Phase::Reserved) return Status::InvalidPhase;
        const std::string session = route.first;
        impl_->eraseRoute(session);
        return Status::Ok;
    }
    return Status::InvalidToken;
}

bool MaiSessionIMRouter::isCurrent(std::uint64_t token, const AccountScope& scope, const std::string& workspace) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->authority(token, scope, workspace) == Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::releaseReservation(std::uint64_t token, const AccountScope& scope,
        const std::string& workspace, bool retryAtFront) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->authority(token, scope, workspace);
    if (status != Status::Ok) return status;
    const auto* found = impl_->state(token);
    if (found->route.phase != Phase::Reserved) return Status::InvalidPhase;
    const auto reservation = found->route.reservation;
    if (retryAtFront) impl_->queues[reservation.input.binding.sessionId].push_front(reservation.input);
    impl_->eraseRoute(reservation.input.binding.sessionId);
    if (retryAtFront) ++impl_->pendingCount;
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::bindReceipt(std::uint64_t token, const TurnReceipt& receipt,
        const AccountScope& scope, const std::string& workspace) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->authority(token, scope, workspace);
    if (status != Status::Ok) return status;
    if (!validReceipt(receipt)) return Status::InvalidReceipt;
    auto* found = impl_->state(token);
    if (found->route.receipt) return *found->route.receipt == receipt ? Status::Duplicate : Status::InvalidReceipt;
    if (found->route.phase != Phase::Reserved) return Status::InvalidPhase;
    found->route.receipt = receipt;
    found->route.phase = Phase::Running;
    --impl_->pendingCount;
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::beginOutput(std::uint64_t token, const TurnReceipt& receipt,
        const AccountScope& scope, const std::string& workspace, std::uint32_t dispatchCount) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->authority(token, scope, workspace);
    if (status != Status::Ok) return status;
    auto* found = impl_->state(token);
    if (!found->route.receipt || *found->route.receipt != receipt) return Status::InvalidReceipt;
    if (!dispatchCount) return Status::InvalidArgument;
    if (found->route.phase != Phase::Running) return Status::Busy;
    found->route.phase = Phase::Finishing;
    found->route.dispatchCount = dispatchCount;
    return Status::Ok;
}

bool MaiSessionIMRouter::isOutputCurrent(std::uint64_t token, const TurnReceipt& receipt,
        const AccountScope& scope, const std::string& workspace) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->authority(token, scope, workspace) != Status::Ok) return false;
    const auto* found = impl_->state(token);
    return found->route.phase == Phase::Finishing && found->route.receipt && *found->route.receipt == receipt;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::acknowledgeDispatch(std::uint64_t token, const TurnReceipt& receipt,
        const AccountScope& scope, const std::string& workspace, std::uint32_t index) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->authority(token, scope, workspace);
    if (status != Status::Ok) return status;
    auto* found = impl_->state(token);
    if (!found->route.receipt || *found->route.receipt != receipt) return Status::InvalidReceipt;
    if (found->route.phase != Phase::Finishing) return Status::InvalidPhase;
    if (index >= found->route.dispatchCount) return Status::InvalidArgument;
    if (!found->acknowledged.insert(index).second) return Status::Duplicate;
    found->route.acknowledgedDispatches = static_cast<std::uint32_t>(found->acknowledged.size());
    if (found->route.acknowledgedDispatches == found->route.dispatchCount)
        impl_->eraseRoute(found->route.reservation.input.binding.sessionId);
    return Status::Ok;
}

MaiSessionIMRouter::Status MaiSessionIMRouter::finishWithoutOutput(std::uint64_t token, const TurnReceipt& receipt,
        const AccountScope& scope, const std::string& workspace) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto status = impl_->authority(token, scope, workspace);
    if (status != Status::Ok) return status;
    const auto* found = impl_->state(token);
    if (found->route.phase != Phase::Running) return Status::InvalidPhase;
    if (!found->route.receipt || *found->route.receipt != receipt) return Status::InvalidReceipt;
    impl_->eraseRoute(found->route.reservation.input.binding.sessionId);
    return Status::Ok;
}

MaiSessionIMRouter::Snapshot MaiSessionIMRouter::snapshot(const AccountScope& scope) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Snapshot result;
    result.status = impl_->scopeStatus(scope);
    if (result.status != Status::Ok) return result;
    for (const auto& binding : impl_->bindings) result.bindings.push_back(binding.second);
    for (const auto& queue : impl_->queues)
        result.pending.insert(result.pending.end(), queue.second.begin(), queue.second.end());
    std::sort(result.pending.begin(), result.pending.end(), [](const QueuedInput& left, const QueuedInput& right) {
        return left.inputId < right.inputId;
    });
    for (const auto& route : impl_->active) result.active.push_back(route.second.route);
    result.pendingCount = impl_->pendingCount;
    result.rememberedInputs = impl_->seen.size();
    return result;
}

std::optional<MaiSessionIMRouter::Binding> MaiSessionIMRouter::bindingForPeer(const AccountScope& scope,
        const std::string& peer) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->scopeStatus(scope) != Status::Ok) return {};
    const auto found = impl_->bindings.find(peer);
    return found == impl_->bindings.end() ? std::optional<Binding>{} : found->second;
}
