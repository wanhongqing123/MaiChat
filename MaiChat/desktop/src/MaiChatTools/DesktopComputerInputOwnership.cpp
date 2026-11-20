#include "MaiChatTools/DesktopComputerInputOwnership.h"

#include "MaiThread.h"

#include <algorithm>
#include <utility>

namespace {
// Identity is the native owner object, not just a model-supplied session ID.
// Other controllers may observe, but cannot inject into a held input session.
std::mutex ownerMutex;
std::weak_ptr<DesktopNativeInputSession> desktopOwner;
}  // namespace

DesktopComputerInputOwnership::DesktopComputerInputOwnership(std::shared_ptr<DesktopComputerBackend> backend,
                                                             std::function<void()> invalidate,
                                                             std::chrono::milliseconds timeout)
    : backend_(std::move(backend)),
      invalidate_(std::move(invalidate)),
      timeout_((std::max)(std::chrono::milliseconds(1), timeout)) {}

DesktopComputerInputOwnership::~DesktopComputerInputOwnership() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
    for (auto& item : entries_) {
        if (!item.second.native->state().empty()) {
            item.second.native->releaseAll();
            invalidate_();
        }
    }
}

void DesktopComputerInputOwnership::expire(Entry& entry) {
    if (entry.native->state().empty()) return;
    entry.native->releaseAll();
    entry.expired = true;
    entry.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    invalidate_();
}

MaiResult<std::shared_ptr<DesktopNativeInputSession>> DesktopComputerInputOwnership::prepare(
    const std::string& session, const DesktopComputerAction& action, bool create, bool releasing) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(session);
    if (!releasing && found != entries_.end() && std::chrono::steady_clock::now() >= found->second.deadline) {
        if (found->second.native->state().empty()) {
            // A native API session can retain configuration and wheel remainder
            // without any held input or virtual device. Its idle cleanup must
            // retire that context too, not manufacture a device in state().
            const auto cleaned = found->second.native->releaseAll();
            if (cleaned) return cleaned;
            entries_.erase(found);
            found = entries_.end();
        } else
            expire(found->second);
    }
    if (!releasing && found != entries_.end() && found->second.expired)
        return {MaiErrorCode::InvalidInput, "held input expired; observe the current state again"};
    const auto local = found == entries_.end() ? nullptr : found->second.native;
    if (releasing && (found == entries_.end() || found->second.turn != action.turnId))
        return std::shared_ptr<DesktopNativeInputSession>{};
    std::lock_guard<std::mutex> global(ownerMutex);
    if (const auto owner = desktopOwner.lock(); owner && owner != local && !owner->state().empty())
        return {MaiErrorCode::Busy, "another input session owns held desktop buttons or keys"};
    const auto target = action.desktopScope ? action.displayId : action.windowId;
    const bool changedReceiver = !releasing && found != entries_.end() &&
        (found->second.executionWindowId != action.executionWindowId ||
         found->second.executionWindowProcessId != action.executionWindowProcessId);
    if (found != entries_.end() && !local->state().empty() &&
        ((target != 0 && (found->second.display != action.desktopScope || found->second.target != target)) ||
         found->second.turn != action.turnId || changedReceiver))
        return {MaiErrorCode::Busy, "release owned inputs before changing input target or task"};
    const bool changedContext = found != entries_.end() &&
        (found->second.turn != action.turnId || changedReceiver ||
         (target != 0 && (found->second.display != action.desktopScope || found->second.target != target)));
    if (!create && (found == entries_.end() || changedContext))
        return std::shared_ptr<DesktopNativeInputSession>{};
    if (found == entries_.end() || changedContext) {
        if (found != entries_.end()) {
            const auto cleaned = local->releaseAll();
            if (cleaned) return cleaned;
            if (!local->state().empty())
                return {MaiErrorCode::Busy, "previous native input cleanup is still pending"};
        }
        auto native = backend_->createInputSession();
        if (!native) return {MaiErrorCode::NotSupported, "independent input is unavailable in this backend"};
        found = entries_
                    .insert_or_assign(session, Entry{native, action.turnId, action.desktopScope, target,
                                                     action.executionWindowId, action.executionWindowProcessId,
                                                     std::chrono::steady_clock::now() + timeout_, false})
                    .first;
        if (!watchdog_.joinable()) watchdog_ = std::thread([this] { watch(); });
    }
    desktopOwner = found->second.native;
    found->second.deadline = std::chrono::steady_clock::now() + timeout_;
    changed_.notify_all();
    return found->second.native;
}

void DesktopComputerInputOwnership::touch(const std::string& session, const std::string& expectedTurn) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(session);
    if (found == entries_.end()) return;
    if (!expectedTurn.empty() && found->second.turn != expectedTurn) return;
    if (std::chrono::steady_clock::now() >= found->second.deadline) {
        if (found->second.native->state().empty()) {
            const auto cleaned = found->second.native->releaseAll();
            if (!cleaned)
                entries_.erase(found);
            else {
                found->second.expired = true;
                found->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            changed_.notify_all();
            return;
        }
        expire(found->second);
    }
    if (found->second.expired && found->second.native->state().empty())
        entries_.erase(found);
    else if (!found->second.expired)
        found->second.deadline = std::chrono::steady_clock::now() + timeout_;
    changed_.notify_all();
}

DesktopNativeInputState DesktopComputerInputOwnership::state(const std::string& session) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(session);
    if (found == entries_.end()) return {};
    auto result = found->second.native->state();
    if (!result.empty())
        result.idleRemainingMs =
            (std::max)(std::int64_t(0), std::chrono::duration_cast<std::chrono::milliseconds>(
                                            found->second.deadline - std::chrono::steady_clock::now())
                                            .count());
    return result;
}

MaiResult<DesktopInputFeedbackBatch> DesktopComputerInputOwnership::feedback(const std::string& session,
                                                                             std::uint64_t afterSequence,
                                                                             std::uint32_t limit) const {
    if (!limit || limit > 256) return {MaiErrorCode::InvalidInput, "feedback limit must be 1..256"};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(session);
    if (found == entries_.end()) {
        DesktopInputFeedbackBatch empty;
        empty.nextSequence = afterSequence;
        return empty;
    }
    return found->second.native->feedback(afterSequence, limit);
}

MaiError DesktopComputerInputOwnership::release(const std::string& session, const std::string& expectedTurn,
                                                bool* released) {
    if (released) *released = false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(session);
    if (found == entries_.end() || (!expectedTurn.empty() && found->second.turn != expectedTurn)) return {};
    const bool held = !found->second.native->state().empty();
    if (released) *released = held || found->second.expired;
    const auto result = found->second.native->releaseAll();
    if (held) invalidate_();
    if (found->second.native->state().empty()) entries_.erase(found);
    changed_.notify_all();
    return result;
}

MaiError DesktopComputerInputOwnership::reset(const std::string& session, const std::string& expectedTurn,
                                              bool* reset) {
    if (reset) *reset = false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(session);
    if (found == entries_.end() || (!expectedTurn.empty() && found->second.turn != expectedTurn)) return {};
    auto& entry = found->second;
    const bool held = !entry.native->state().empty();
    if (reset) *reset = held || entry.expired;
    // A reset cannot turn a timed-out/canceled lease into a retained device.
    const bool expired = entry.expired || std::chrono::steady_clock::now() >= entry.deadline;
    const auto result = expired ? entry.native->releaseAll() : entry.native->resetControls();
    if (held) invalidate_();
    if (expired && entry.native->state().empty())
        entries_.erase(found);
    else if (expired) {
        entry.expired = true;
        entry.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    } else if (!result)
        entry.deadline = std::chrono::steady_clock::now() + timeout_;
    changed_.notify_all();
    return result;
}

void DesktopComputerInputOwnership::watch() {
    MaiThread::setCurrentName("mai-input-hold");
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        for (auto item = entries_.begin(); item != entries_.end();) {
            if (item->second.native->state().empty() &&
                std::chrono::steady_clock::now() >= item->second.deadline) {
                const auto cleaned = item->second.native->releaseAll();
                if (!cleaned) {
                    item = entries_.erase(item);
                    continue;
                }
                item->second.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            }
            ++item;
        }
        std::vector<std::pair<std::string, std::shared_ptr<DesktopNativeInputSession>>> pending;
        for (const auto& item : entries_) {
            if (item.second.native->state().empty()) continue;
            pending.emplace_back(item.first, item.second.native);
        }
        for (const auto& item : pending) {
            // Validation may synchronously ask the target GUI for UIA/AX data.
            // Let that GUI read/cancel owner state while servicing the request.
            // Recheck the native identity after reacquiring the lock: a result
            // for an old owner must never expire a replacement task's resources.
            lock.unlock();
            const auto invalid = item.second->validate();
            lock.lock();
            if (stopping_) break;
            const auto current = entries_.find(item.first);
            if (current == entries_.end() || current->second.native != item.second ||
                current->second.native->state().empty())
                continue;
            if (std::chrono::steady_clock::now() >= current->second.deadline || invalid) expire(current->second);
        }
        for (const auto& item : entries_) {
            next = (std::min)(next, item.second.deadline);
        }
        if (stopping_) break;
        changed_.wait_until(lock, next);
    }
}
