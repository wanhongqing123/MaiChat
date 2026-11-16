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
    if (!releasing && found != entries_.end() && std::chrono::steady_clock::now() >= found->second.deadline)
        expire(found->second);
    if (!releasing && found != entries_.end() && found->second.expired)
        return {MaiErrorCode::InvalidInput, "held input expired; observe the current state again"};
    const auto local = found == entries_.end() ? nullptr : found->second.native;
    if (releasing && (found == entries_.end() || found->second.turn != action.turnId))
        return std::shared_ptr<DesktopNativeInputSession>{};
    std::lock_guard<std::mutex> global(ownerMutex);
    if (const auto owner = desktopOwner.lock(); owner && owner != local && !owner->state().empty())
        return {MaiErrorCode::Busy, "another input session owns held desktop buttons or keys"};
    const auto target = action.desktopScope ? action.displayId : action.windowId;
    if (found != entries_.end() && !local->state().empty() &&
        ((target != 0 && (found->second.display != action.desktopScope || found->second.target != target)) ||
         found->second.turn != action.turnId))
        return {MaiErrorCode::Busy, "release owned inputs before changing input target or task"};
    if (!create && (found == entries_.end() || local->state().empty()))
        return std::shared_ptr<DesktopNativeInputSession>{};
    if (found == entries_.end() || local->state().empty()) {
        auto native = backend_->createInputSession();
        if (!native) return {MaiErrorCode::NotSupported, "independent input is unavailable in this backend"};
        found = entries_
                    .insert_or_assign(session, Entry{native, action.turnId, action.desktopScope, target,
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
    if (std::chrono::steady_clock::now() >= found->second.deadline) expire(found->second);
    if (found->second.native->state().empty())
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

void DesktopComputerInputOwnership::watch() {
    MaiThread::setCurrentName("mai-input-hold");
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        for (auto& item : entries_) {
            if (item.second.native->state().empty()) continue;
            if (std::chrono::steady_clock::now() >= item.second.deadline || item.second.native->validate())
                expire(item.second);
            next = (std::min)(next, item.second.deadline);
        }
        changed_.wait_until(lock, next);
    }
}
