#include "MaiMobileSessionIM.h"

#include "MaiSessionIMRouter.h"
#include "MaiAgent.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiPathGuard.h"
#include "MaiTool.h"
#include "MaiIMHistorySnapshot.h"
#include "MaiIMHistoryFormat.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using Json = nlohmann::json;
using Router = MaiSessionIMRouter;
using Scope = Router::AccountScope;
using Clock = std::chrono::steady_clock;

struct Failure : std::runtime_error {
    std::string code;
    Failure(std::string value, std::string message) : std::runtime_error(std::move(message)), code(std::move(value)) {}
};

void require(bool valid, const char* code, const char* message) {
    if (!valid) throw Failure(code, message);
}

Json error(const std::string& code, const std::string& message) {
    return {{"ok", false}, {"errorCode", code}, {"error", message}};
}

std::string text(const Json& object, const char* key, std::size_t limit = 256) {
    require(object.contains(key) && object[key].is_string(), "invalid_input", "A required string is missing");
    const auto value = object[key].get<std::string>();
    require(!value.empty() && value.size() <= limit && value.find('\0') == std::string::npos, "invalid_input",
            "A string is empty or exceeds its allowed size");
    return value;
}

std::uint64_t number(const Json& value, bool zero = false) {
    require(value.is_string(), "invalid_input", "Numeric identities must be decimal strings");
    const auto raw = value.get<std::string>();
    require(!raw.empty() && raw.size() <= 20 && (raw.size() == 1 || raw.front() != '0'), "invalid_input",
            "Invalid decimal identity");
    std::uint64_t result = 0;
    for (unsigned char ch : raw) {
        require(ch >= '0' && ch <= '9' && result <= (std::numeric_limits<std::uint64_t>::max() - (ch - '0')) / 10,
                "invalid_input", "Decimal identity is out of range");
        result = result * 10 + ch - '0';
    }
    require(zero || result != 0, "invalid_input", "Numeric identities must be nonzero");
    return result;
}

bool flag(const Json& object, const char* key) {
    require(object.contains(key) && object[key].is_boolean(), "invalid_input", "A required boolean is missing");
    return object[key].get<bool>();
}

Json scopeJson(const Scope& scope) {
    return {{"sdk_app_id", std::to_string(scope.sdkAppId)},
            {"owner_id", scope.ownerUserId},
            {"auth_generation", std::to_string(scope.authGeneration)},
            {"controller_generation", std::to_string(scope.controllerGeneration)}};
}

Scope parseScope(const Json& value) {
    require(value.is_object() && value.size() == 4, "stale_scope", "An exact immutable scope is required");
    return {number(value.at("sdk_app_id")), text(value, "owner_id"), number(value.at("auth_generation")),
            number(value.at("controller_generation"))};
}

Json parse(const std::string& raw) {
    require(!raw.empty() && raw.size() <= 1024 * 1024, "invalid_input", "Request size is invalid");
    auto result = Json::parse(raw, nullptr, false);
    require(result.is_object(), "invalid_input", "Request must be a JSON object");
    return result;
}

std::set<std::string> friends(const Json& request, const std::string& owner) {
    require(request.contains("friends") && request["friends"].is_array() && request["friends"].size() <= 4096,
            "invalid_input", "An accepted-friend snapshot is required");
    std::set<std::string> result;
    for (const auto& item : request["friends"]) {
        const auto peer = text(Json{{"peer", item}}, "peer");
        require(peer != owner, "invalid_input", "The owner cannot be an accepted peer");
        result.insert(peer);
    }
    return result;
}

const char* statusName(Router::Status status) {
    switch (status) {
        case Router::Status::Ok: return "ok";
        case Router::Status::NotActive: return "not_active";
        case Router::Status::StaleScope: return "stale_scope";
        case Router::Status::InvalidArgument: return "invalid_input";
        case Router::Status::NotEligible: return "not_eligible";
        case Router::Status::NotBound: return "not_bound";
        case Router::Status::Conflict: return "conflict";
        case Router::Status::Duplicate: return "duplicate";
        case Router::Status::QueueFull: return "queue_full";
        case Router::Status::Busy: return "busy";
        case Router::Status::InvalidToken: return "stale_input";
        case Router::Status::InvalidReceipt: return "invalid_receipt";
        case Router::Status::InvalidPhase: return "invalid_phase";
        case Router::Status::Exhausted: return "capacity_exhausted";
    }
    return "invalid_state";
}

void accepted(Router::Status status) {
    if (status != Router::Status::Ok)
        throw Failure(statusName(status), "The requested routing operation was not accepted");
}

std::string canonical(const std::string& path) {
    const auto candidate = MaiFilePath::fromUtf8(path);
    if (!candidate.isAbsolute() || !MaiFileSystem::isDirectory(candidate)) return {};
    return MaiFileSystem::resolve(candidate).toUtf8();
}

std::string relativeMediaPath(const std::string& root, const std::string& absolute) {
    const auto parent = MaiFilePath::fromUtf8(root);
    const auto child = MaiFilePath::fromUtf8(absolute);
    require(parent.isParentOf(child), "invalid_input", "Prepared media is not below its canonical workspace");
    const auto prefix = parent.components();
    const auto parts = child.components();
    MaiFilePath relative;
    for (std::size_t index = prefix.size(); index < parts.size(); ++index)
        relative = relative.append(MaiFilePath(parts[index]));
    require(!relative.isEmpty() && !relative.isAbsolute(), "invalid_input",
            "Prepared media has no stable relative path");
    return relative.toGenericUtf8();
}

struct Watch {
    std::mutex mutex;
    const std::thread::id submitThread = std::this_thread::get_id();
    bool capturing = true;
    std::vector<std::string> ids;
    std::atomic<bool> valid{true};
    std::atomic<bool> failed{false};
};

struct Signal {
    std::mutex mutex;
    Scope account;
    std::string peer;
    std::uint64_t bindingEpoch = 0;
    std::shared_ptr<Watch> watch;
    std::set<std::string> priorIds;
    std::atomic<std::uint64_t> revision{0};
};

struct Observers {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Signal>> sessions;
};

struct Payload {
    std::string text;
    std::vector<MaiModelImage> images;
    std::vector<MaiVideoPart> videos;
};

struct InputRecord {
    Router::QueuedInput input;
    Clock::time_point admitted = Clock::now();
    std::optional<Payload> payload;
};

struct DeliveryRecord {
    Json value;
    Router::TurnReceipt receipt;
    std::string workspace;
    std::string session;
    std::shared_ptr<Signal> signal;
    std::shared_ptr<Watch> receiptWatch;
    std::uint64_t revision = 0;
    bool claimed = false;
    Clock::time_point expires;
};

struct HistorySource {
    std::string profile;
    std::string path;
    std::string provenance;
    bool operator==(const HistorySource& other) const {
        return profile == other.profile && path == other.path && provenance == other.provenance;
    }
};

struct HistoryJob {
    Json task;
    Router::Binding binding;
    HistorySource source;
    std::shared_ptr<std::atomic<bool>> canceled = std::make_shared<std::atomic<bool>>(false);
    Clock::time_point expires = Clock::now() + std::chrono::seconds(300);
    std::string snapshotId;
    std::string importId;
    std::uint64_t nextSequence = 0;
    std::uint64_t coreSequence = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t overlapCount = 0;
    Json acceptedPage;
    bool issued = false;
    bool eof = false;
    bool committed = false;
    bool committing = false;
};

struct Authority {
    std::mutex mutex;
    Router router;
    std::atomic<bool> alive{true};
    std::atomic<bool> active{false};
    const std::uint64_t generation;
    const std::string bridgeId;
    std::optional<Scope> scope;
    std::string owner;
    std::uint64_t sdk = 0;
    std::uint64_t lastAuth = 0;
    std::set<std::string> acceptedFriends;
    std::map<std::uint64_t, InputRecord> inputs;
    std::map<std::pair<std::uint64_t, std::uint32_t>, DeliveryRecord> deliveries;
    std::set<std::string> changed;
    std::vector<Json> warnings;
    std::optional<HistorySource> historySource;
    std::map<std::string, std::shared_ptr<HistoryJob>> history;
    std::vector<std::shared_ptr<HistoryJob>> historyReap;
    Clock::time_point nextHistoryAbortAttempt{};
    // 只保存需要回收的 Session 元数据，不缓存旧消息或另建后台执行队列。
    std::set<std::string> cleanupSessions;
    std::set<std::string> cleanupWarned;
    std::string cleanupCursor;
    bool cleanupRestorePending = true;
    Clock::time_point nextCleanupAttempt{};
    const std::chrono::milliseconds outputBudget;
    std::shared_ptr<Observers> observers = std::make_shared<Observers>();
    Authority(std::uint64_t id, std::uint64_t budget)
        : generation(id),
          bridgeId("im-session-" + std::to_string(id)),
          outputBudget(std::min<std::uint64_t>(30000, std::max<std::uint64_t>(1, budget))) {}
};

struct Registry {
    std::mutex mutex;
    std::uint64_t next = 1;
    std::uint64_t highWater = 0;
    std::weak_ptr<Authority> active;
    std::map<std::string, std::weak_ptr<Authority>> entries;
};

Registry& registry() {
    static auto* value = new Registry;
    return *value;
}

struct HistoryReader {
    std::timed_mutex io;
    std::mutex metadata;
    const std::string id;
    std::shared_ptr<HistoryJob> job;
    std::weak_ptr<Authority> authority;
    std::unique_ptr<MaiIMHistorySnapshot> reader;
    MaiIMHistorySource source;
    std::shared_ptr<const MaiIMHistoryPage> page;
    Json pageValue;
    bool acknowledged = true;
    std::atomic<bool> opened{false};
    explicit HistoryReader(std::string value) : id(std::move(value)) {}
};

struct HistoryReaders {
    std::mutex mutex;
    std::uint64_t next = 1;
    std::map<std::string, std::shared_ptr<HistoryReader>> entries;
};

HistoryReaders& historyReaders() {
    static auto* value = new HistoryReaders;
    return *value;
}

void cancelHistoryLocked(Authority& state, const std::string& session) {
    const auto found = state.history.find(session);
    if (found == state.history.end()) return;
    found->second->canceled->store(true);
    state.cleanupSessions.insert(session);
    state.historyReap.push_back(found->second);
    state.history.erase(found);
}

std::shared_ptr<Signal> signalFor(const std::shared_ptr<Authority>& state, const std::string& session) {
    std::lock_guard<std::mutex> lock(state->observers->mutex);
    const auto found = state->observers->sessions.find(session);
    return found == state->observers->sessions.end() ? nullptr : found->second;
}

void clearLocked(Authority& state) {
    state.active = false;
    if (state.scope) state.router.logout(*state.scope);
    state.scope.reset();
    state.inputs.clear();
    state.deliveries.clear();
    state.warnings.clear();
    for (const auto& item : state.history) {
        item.second->canceled->store(true);
        state.historyReap.push_back(item.second);
        state.cleanupSessions.insert(item.first);
    }
    state.history.clear();
    std::lock_guard<std::mutex> observers(state.observers->mutex);
    for (const auto& item : state.observers->sessions) {
        std::lock_guard<std::mutex> signal(item.second->mutex);
        if (item.second->watch) item.second->watch->valid = false;
    }
    state.observers->sessions.clear();
}

void revokeLocked(Authority& state, const std::string& session) {
    state.cleanupSessions.insert(session);
    cancelHistoryLocked(state, session);
    if (state.scope) state.router.revokeSession(*state.scope, session);
    for (auto it = state.inputs.begin(); it != state.inputs.end();)
        it = it->second.input.binding.sessionId == session ? state.inputs.erase(it) : std::next(it);
    for (auto it = state.deliveries.begin(); it != state.deliveries.end();)
        it = it->second.session == session ? state.deliveries.erase(it) : std::next(it);
    state.changed.insert(session);
}

void failHistoryLocked(Authority& state, const std::string& session, const char* code) {
    revokeLocked(state, session);
    state.warnings.push_back({{"code", code}, {"session_id", session}, {"outcome", "binding_revoked"}});
}

void observe(const std::weak_ptr<Authority>& weak, const MaiEvent& event) {
    const auto state = weak.lock();
    if (!state || !state->alive.load() || !state->active.load()) return;
    const auto signal = signalFor(state, event.sessionId);
    if (!signal) return;
    if (event.type == MaiEventType::SessionUpdated) {
        ++signal->revision;
        return;
    }
    std::shared_ptr<Watch> watch;
    bool prior = false;
    {
        std::lock_guard<std::mutex> lock(signal->mutex);
        watch = signal->watch;
        prior = signal->priorIds.count(event.messageId) != 0;
    }
    if (watch && event.type == MaiEventType::SessionError) {
        watch->failed = true;
        return;
    }
    if (watch) {
        std::lock_guard<std::mutex> lock(watch->mutex);
        if (watch->capturing && watch->submitThread == std::this_thread::get_id()) {
            if (event.type == MaiEventType::MessageUpdated && !event.messageId.empty() &&
                std::find(watch->ids.begin(), watch->ids.end(), event.messageId) == watch->ids.end()) {
                if (watch->ids.size() < 2)
                    watch->ids.push_back(event.messageId);
                else
                    watch->valid = false;
            }
            if (event.type == MaiEventType::SessionDeleted || event.type == MaiEventType::MessageRemoved)
                watch->valid = false;
            return;
        }
        if (event.type == MaiEventType::MessageUpdated &&
            std::find(watch->ids.begin(), watch->ids.end(), event.messageId) != watch->ids.end())
            return;
    } else if (event.type == MaiEventType::MessageUpdated && prior)
        return;
    if (event.type != MaiEventType::MessageUpdated && event.type != MaiEventType::MessageRemoved &&
        event.type != MaiEventType::SessionDeleted)
        return;
    if (watch) watch->valid = false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->alive || !state->active || !state->scope || *state->scope != signal->account) return;
    {
        std::lock_guard<std::mutex> observerLock(state->observers->mutex);
        const auto current = state->observers->sessions.find(event.sessionId);
        if (current == state->observers->sessions.end() || current->second != signal) return;
    }
    const auto binding = state->router.bindingForPeer(*state->scope, signal->peer);
    if (binding && binding->epoch == signal->bindingEpoch && binding->sessionId == event.sessionId)
        revokeLocked(*state, event.sessionId);
}

void checkLocked(const Authority& state, const Json& request) {
    require(state.alive && state.active && state.scope, "stale_bridge", "Session binding runtime is inactive");
    require(text(request, "bridge_id") == state.bridgeId && parseScope(request.at("scope")) == *state.scope,
            "stale_scope", "Session binding scope no longer matches this backend");
}

HistorySource parseHistorySource(const Json& request) {
    require(request.contains("history_source") && request["history_source"].is_object(), "history_unavailable",
            "A trusted local history source is required");
    const auto& value = request["history_source"];
    require(value.size() == 3, "invalid_input", "History source fields must be exact");
    HistorySource source{text(value, "profile"), text(value, "path", 8192), text(value, "provenance")};
    require(source.profile == "desktop" || source.profile == "android" || source.profile == "ios", "invalid_input",
            "Unknown local history schema profile");
    require(source.provenance == (source.profile == "ios" ? "owner_sdk" : "owner_only_legacy") &&
                MaiFilePath::fromUtf8(source.path).isAbsolute(),
            "invalid_input", "History source provenance or path is invalid");
    return source;
}

Json historyProgress(const HistoryJob& job) {
    return {{"task_id", job.task.at("task_id")},
            {"import_id", job.importId},
            {"snapshot_id", job.snapshotId},
            {"next_sequence", std::to_string(job.nextSequence)},
            {"record_count", std::to_string(job.recordCount)},
            {"live_overlap_count", std::to_string(job.overlapCount)},
            {"committed", job.committed}};
}

std::shared_ptr<HistoryJob> historyJobLocked(Authority& state, const Json& task) {
    require(task.is_object(), "stale_history", "An immutable history task is required");
    const auto found = state.history.find(text(task, "session_id"));
    require(found != state.history.end() && found->second->task == task && !found->second->canceled->load(),
            "stale_history", "History task no longer matches its original binding");
    const auto& job = found->second;
    const auto current = state.router.bindingForPeer(job->binding.account, job->binding.peerId);
    require(current && current->epoch == job->binding.epoch && current->sessionId == job->binding.sessionId &&
                current->workspaceKey == job->binding.workspaceKey,
            "stale_history", "History binding was replaced");
    require(Clock::now() < job->expires, "history_timeout", "History bootstrap deadline expired");
    return job;
}

Router::Incoming incoming(const Json& request, const Scope& scope, bool hasContent) {
    Router::Incoming result;
    result.account = scope;
    result.peerId = text(request, "peer_id");
    result.messageId = text(request, "message_id");
    result.senderUserId = text(request, "sender_id");
    result.recipientUserId = text(request, "recipient_id");
    result.expectedBindingEpoch = number(request.at("expected_binding_epoch"));
    result.expectedSessionId = text(request, "expected_session_id");
    const auto origin = text(request, "origin");
    require(origin == "human" || origin == "machine" || origin == "unknown", "invalid_input", "Invalid message origin");
    result.origin = origin == "human"     ? Router::Origin::Human
                    : origin == "machine" ? Router::Origin::Machine
                                          : Router::Origin::Unknown;
    result.live = flag(request, "live");
    result.group = flag(request, "group");
    result.controlFrame = flag(request, "control_frame");
    result.hasContent = hasContent;
    result.requirePersistence = true;
    return result;
}

bool sameInput(const Router::Incoming& a, const Router::Incoming& b) {
    return a.account == b.account && a.peerId == b.peerId && a.messageId == b.messageId &&
           a.senderUserId == b.senderUserId && a.recipientUserId == b.recipientUserId &&
           a.expectedBindingEpoch == b.expectedBindingEpoch && a.expectedSessionId == b.expectedSessionId &&
           a.origin == b.origin && a.live == b.live && a.group == b.group && a.controlFrame == b.controlFrame;
}

const char* historyProfileName(MaiIMHistoryProfile value) {
    return value == MaiIMHistoryProfile::IOS ? "ios" : value == MaiIMHistoryProfile::Android ? "android" : "desktop";
}

Json historySourceJson(const MaiIMHistorySource& source) {
    Json value{{"profile", historyProfileName(source.profile)},
               {"canonical_database_path", source.canonicalDatabasePath},
               {"owner_user_id", source.ownerUserId},
               {"peer_user_id", source.peerUserId},
               {"provenance", source.provenance},
               {"coverage", source.coverage}};
    if (source.sdkAppId) value["sdk_app_id"] = std::to_string(*source.sdkAppId);
    return value;
}

Json historyRecordJson(const MaiIMHistoryRecord& record) {
    Json media = Json::array();
    for (const auto& item : record.media)
        media.push_back({{"kind", item.kind},
                         {"name", item.name},
                         {"mime_type", item.mimeType},
                         {"local_path", item.localPath},
                         {"cover_path", item.coverPath},
                         {"size_bytes", std::to_string(item.sizeBytes)},
                         {"duration_millis", std::to_string(item.durationMillis)},
                         {"width", item.width},
                         {"height", item.height},
                         {"has_local_reference", item.hasLocalReference},
                         {"metadata_unavailable", item.metadataUnavailable}});
    Json value{{"row_id", record.rowId},
               {"remote_id", record.remoteId ? Json(*record.remoteId) : Json(nullptr)},
               {"from_user_id", record.fromUserId},
               {"to_user_id", record.toUserId},
               {"direction", record.direction == MaiIMHistoryDirection::Incoming   ? "incoming"
                             : record.direction == MaiIMHistoryDirection::Outgoing ? "outgoing"
                                                                                   : "unknown"},
               {"status", record.status},
               {"created_at_ms", std::to_string(record.createdAtMillis)},
               {"text", record.text},
               {"kind", record.kind},
               {"media", media},
               {"caption_above", record.captionAbove},
               {"unavailable_metadata", record.unavailableMetadata},
               {"quote", nullptr}};
    if (record.quote)
        value["quote"] = {{"message_id", record.quote->messageId},
                          {"sender_id", record.quote->senderId},
                          {"digest", record.quote->digest},
                          {"kind", record.quote->kind}};
    return value;
}

std::shared_ptr<HistoryReader> findHistoryReader(const std::string& id) {
    std::lock_guard<std::mutex> lock(historyReaders().mutex);
    const auto found = historyReaders().entries.find(id);
    require(found != historyReaders().entries.end(), "stale_history", "History reader is unavailable");
    return found->second;
}

// 只能由 historySnapshot 的既有 I/O worker 调用。静态控制仅设置 job.canceled，
// 不在 Main/SDK/Agent worker 销毁 SQLite 连接，也不持 registry/authority 锁做 I/O。
void reapHistoryReadersOnIO() {
    std::vector<std::shared_ptr<HistoryReader>> expired;
    {
        std::lock_guard<std::mutex> lock(historyReaders().mutex);
        for (auto it = historyReaders().entries.begin(); it != historyReaders().entries.end();) {
            if (it->second->job->canceled->load() || Clock::now() >= it->second->job->expires) {
                it->second->job->canceled->store(true);
                expired.push_back(it->second);
                it = historyReaders().entries.erase(it);
            } else
                ++it;
        }
    }
    for (const auto& slot : expired) {
        std::unique_lock<std::timed_mutex> io(slot->io, std::defer_lock);
        if (io.try_lock_for(std::chrono::seconds(5))) slot->reader.reset();
        // 另一个 I/O 调用保有 slot 时由该调用最终释放；永远没有 Backend/Agent 指针。
    }
}

}  // namespace

struct MaiMobileSessionIM::Impl {
    std::timed_mutex operation;
    MaiAgent* agent;
    MaiAgent* const identity;
    MaiContextImportWorker importWorker;
    std::string workspace;
    std::function<void(const std::string&)> requireModel;
    std::function<void(const std::string&)> changed;
    std::function<void(const std::string&, bool)> inputValidator;
    std::shared_ptr<Authority> state;
    MaiEventBus::Token subscription = 0;
    std::chrono::milliseconds readiness;
    unsigned historyAbortAttempts = 0;
    bool historyAbortFailedThisRequest = false;

    Impl(MaiAgent& value, std::string root, std::function<void(const std::string&)> model, std::uint64_t budget,
         std::uint64_t outputBudget)
        : agent(&value),
          identity(&value),
          importWorker(value.contextImportWorker()),
          workspace(canonical(root)),
          requireModel(std::move(model)),
          readiness(std::min<std::uint64_t>(120000, std::max<std::uint64_t>(1, budget))) {
        auto& global = registry();
        {
            std::lock_guard<std::mutex> lock(global.mutex);
            require(global.next != std::numeric_limits<std::uint64_t>::max(), "capacity_exhausted",
                    "Native generation exhausted");
            state = std::make_shared<Authority>(global.next++, outputBudget);
            global.entries.emplace(state->bridgeId, state);
        }
        subscription = value.eventBus().subscribe(
            [weak = std::weak_ptr<Authority>(state)](const MaiEvent& event) { observe(weak, event); });
    }

    void notify() {
        std::set<std::string> sessions;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            sessions.swap(state->changed);
        }
        if (changed)
            for (const auto& session : sessions) changed(session);
    }

    void activate(const Json& request) {
        Scope scope{number(request.at("sdk_app_id")), text(request, "owner_id"), number(request.at("auth_generation")),
                    state->generation};
        const auto acceptedFriends = friends(request, scope.ownerUserId);
        const auto source = parseHistorySource(request);
        auto& global = registry();
        std::lock_guard<std::mutex> registryLock(global.mutex);
        require(state->generation >= global.highWater, "stale_backend", "A newer native backend has already activated");
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->alive, "stale_bridge", "Native backend was invalidated");
        require(state->owner.empty() || (state->owner == scope.ownerUserId && state->sdk == scope.sdkAppId),
                "stale_scope", "Changing account identity requires backend replacement");
        require(scope.authGeneration > state->lastAuth || (state->active && state->scope && *state->scope == scope),
                "stale_scope", "Authentication generation cannot move backward or revive a logged-out scope");
        if (const auto previous = global.active.lock(); previous && previous != state) {
            std::lock_guard<std::mutex> previousLock(previous->mutex);
            clearLocked(*previous);
        }
        const bool scopeChanged = !state->active || !state->scope || *state->scope != scope;
        if (scopeChanged) {
            clearLocked(*state);
            accepted(state->router.activate(scope));
            state->scope = scope;
            state->owner = scope.ownerUserId;
            state->sdk = scope.sdkAppId;
            state->lastAuth = scope.authGeneration;
        }
        const bool sourceChanged = !state->historySource || !(*state->historySource == source);
        if (!scopeChanged && sourceChanged) {
            const auto bindings = state->router.snapshot(scope).bindings;
            for (const auto& binding : bindings) revokeLocked(*state, binding.sessionId);
        }
        state->historySource = source;
        if (scopeChanged || sourceChanged) {
            state->cleanupRestorePending = true;
            state->nextCleanupAttempt = {};
        }
        state->active = true;
        global.highWater = state->generation;
        global.active = state;
        if (setFriendsLocked(acceptedFriends) || scopeChanged || sourceChanged) state->changed.insert("");
    }

    bool setFriendsLocked(const std::set<std::string>& values) {
        const bool changed = state->acceptedFriends != values;
        state->acceptedFriends = values;
        const auto snapshot = state->router.snapshot(*state->scope);
        for (const auto& binding : snapshot.bindings)
            if (!values.count(binding.peerId)) revokeLocked(*state, binding.sessionId);
        return changed;
    }

    Json projection(const Json* expected = nullptr, bool issueTasks = true) {
        require(agent != nullptr, "stale_bridge", "Native Agent is unavailable");
        Router::Snapshot snapshot;
        Scope scope;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            require(state->active && state->scope, "not_active", "Activate the current account first");
            if (expected) checkLocked(*state, *expected);
            scope = *state->scope;
        }
        Json sessions = Json::array();
        std::map<std::string, std::string> titles;
        for (const auto& session : agent->listSessions())
            if (session.isRoot()) {
                titles[session.id] = session.title;
                sessions.push_back({{"id", session.id},
                                    {"title", session.title},
                                    {"busy", agent->isBusy(session.id)},
                                    {"workspace_key", workspace}});
            }
        // Session 读取可能让出时间给纯控制撤销；最终返回的路由、进度和任务
        // 必须在同一个最新 authority 快照内形成，不能返回数据库读取前的旧 ready。
        std::lock_guard<std::mutex> lock(state->mutex);
        require(state->active && state->scope && *state->scope == scope, "stale_scope",
                "Account changed during projection");
        snapshot = state->router.snapshot(scope);
        if (state->cleanupRestorePending) {
            for (const auto& title : titles) state->cleanupSessions.insert(title.first);
            state->cleanupRestorePending = false;
        }
        Json bindings = Json::array();
        for (const auto& binding : snapshot.bindings) {
            if (!titles.count(binding.sessionId)) continue;
            bindings.push_back({{"peer_id", binding.peerId},
                                {"session_id", binding.sessionId},
                                {"session_title", titles[binding.sessionId]},
                                {"epoch", std::to_string(binding.epoch)},
                                {"history_state", binding.historyPending ? "pending" : "ready"}});
        }
        Json tasks = Json::array();
        Json progress = Json::array();
        for (auto& entry : state->history) {
            auto& job = *entry.second;
            progress.push_back(historyProgress(job));
            if (issueTasks && !job.issued && !job.canceled->load() && !job.committed) {
                tasks.push_back(job.task);
                job.issued = true;
            }
        }
        for (auto& binding : bindings) {
            const auto found = state->history.find(binding.at("session_id").get<std::string>());
            binding["import_id"] = found == state->history.end() ? "" : found->second->importId;
            binding["imported_count"] =
                found == state->history.end() ? "0" : std::to_string(found->second->recordCount);
        }
        return {{"ok", true},
                {"bridge_id", state->bridgeId},
                {"scope", scopeJson(scope)},
                {"sessions", sessions},
                {"bindings", bindings},
                {"has_bindings", !bindings.empty()},
                {"pending_count", snapshot.pendingCount},
                {"history_tasks", tasks},
                {"history_progress_all", progress},
                {"history_cleanup_pending", state->cleanupRestorePending || !state->cleanupSessions.empty() ||
                    !state->historyReap.empty()}};
    }

    void bind(const Json& request, const std::function<void()>& beforeMutation = {}) {
        const auto peer = text(request, "peer_id");
        const auto sessionId = text(request, "session_id");
        require(text(request, "expected_workspace_key", 8192) == workspace, "stale_workspace",
                "Selected Session belongs to a different workspace generation");
        Scope scope;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            scope = *state->scope;
            require(state->acceptedFriends.count(peer) != 0, "not_eligible", "Peer is not an accepted friend");
            require(state->historySource.has_value(), "history_unavailable",
                    "Trusted local history source is unavailable");
            for (const auto& existing : state->router.snapshot(scope).bindings)
                require(existing.peerId == peer || existing.sessionId != sessionId, "conflict",
                        "Session is already bound to another friend");
            const auto old = state->router.bindingForPeer(scope, peer);
            if (old && old->sessionId == sessionId && (!old->historyPending || state->history.count(sessionId))) return;
        }
        MaiSession session;
        require(agent && agent->getSession(sessionId, session) && session.isRoot(), "not_found",
                "Local root Session was not found");
        require(!workspace.empty(), "workspace_unavailable", "Trusted current workspace is unavailable");
        if (session.directory != workspace) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                if (beforeMutation) beforeMutation();
                const auto updated = agent->submit(MaiUpdateSession{sessionId, "", "", "", workspace});
                require(bool(updated), "workspace_unavailable",
                        "Could not migrate Session into the trusted current workspace");
            }
            require(agent->getSession(sessionId, session), "not_found", "Session disappeared while binding");
        }
        const auto key = canonical(session.directory);
        require(!key.empty() && key == workspace, "workspace_unavailable",
                "Session workspace does not match the trusted current workspace");
        auto signal = std::make_shared<Signal>();
        for (const auto& message : agent->listMessagesPage(sessionId, "", 512)) signal->priorIds.insert(message.id);
        std::lock_guard<std::mutex> lock(state->mutex);
        checkLocked(*state, request);
        const auto old = state->router.bindingForPeer(scope, peer);
        if (beforeMutation) beforeMutation();
        accepted(state->router.bind(scope, peer, sessionId, key, state->acceptedFriends.count(peer), true));
        const auto current = state->router.bindingForPeer(scope, peer);
        if (!old || old->epoch != current->epoch) {
            if (old) {
                cancelHistoryLocked(*state, old->sessionId);
                revokePayloads(old->sessionId);
            }
            signal->account = scope;
            signal->peer = peer;
            signal->bindingEpoch = current->epoch;
            std::lock_guard<std::mutex> observerLock(state->observers->mutex);
            state->observers->sessions[sessionId] = signal;
        }
        accepted(state->router.beginHistory(scope, peer, current->epoch));
        auto job = std::make_shared<HistoryJob>();
        job->binding = *state->router.bindingForPeer(scope, peer);
        job->source = *state->historySource;
        job->task = {
            {"task_id", "history-task-" + std::to_string(state->generation) + "-" + std::to_string(current->epoch)},
            {"bridge_id", state->bridgeId},
            {"scope", scopeJson(scope)},
            {"peer_id", peer},
            {"session_id", sessionId},
            {"binding_epoch", std::to_string(current->epoch)},
            {"import_id", ""}};
        state->history[sessionId] = std::move(job);
        state->changed.insert(sessionId);
    }

    void revokePayloads(const std::string& session) {
        for (auto it = state->inputs.begin(); it != state->inputs.end();)
            it = it->second.input.binding.sessionId == session ? state->inputs.erase(it) : std::next(it);
        for (auto it = state->deliveries.begin(); it != state->deliveries.end();)
            it = it->second.session == session ? state->deliveries.erase(it) : std::next(it);
    }

    void reapHistory() {
        std::shared_ptr<HistoryJob> job;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto& jobs = state->historyReap;
            jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const auto& item) {
                return item->importId.empty() || item->committed;
            }), jobs.end());
            if (!agent || jobs.empty() || historyAbortAttempts == 0 || Clock::now() < state->nextHistoryAbortAttempt)
                return;
            job = jobs.front();
            --historyAbortAttempts;
        }
        // 一次普通 handle 最多尝试一次撤销；失败任务仍由队列持有，不能把写锁错误当完成。
        const auto aborted = importWorker.abortContextImport(job->importId);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->cleanupSessions.insert(job->binding.sessionId);
        auto& jobs = state->historyReap;
        jobs.erase(std::remove(jobs.begin(), jobs.end(), job), jobs.end());
        if (!aborted || aborted.code() == MaiErrorCode::NotFound || aborted.code() == MaiErrorCode::Canceled) {
            // NotFound 也可能表示其它既有 worker 已把它标为 state2；仍必须清理该 Session。
            state->nextHistoryAbortAttempt = {};
            return;
        }
        jobs.push_back(job);
        state->nextHistoryAbortAttempt = Clock::now() + std::chrono::seconds(1);
        historyAbortFailedThisRequest = true;
        if (state->cleanupWarned.insert(job->binding.sessionId).second)
            state->warnings.push_back({{"code", "history_cleanup_failed"}, {"session_id", job->binding.sessionId},
                {"outcome", "retry_local_cleanup"}});
    }

    void purgeRetiredHistory() {
        if (historyAbortFailedThisRequest) return;
        // 两次有界事务且仅在调用之间检查时间预算。不会持有 authority 等待 SQL，
        // 也不会因大量旧快照把一个普通 pump 变成无限清理循环。
        const auto deadline = Clock::now() + std::chrono::milliseconds(15);
        for (unsigned batch = 0; batch < 2 && Clock::now() < deadline; ++batch) {
            std::string session;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->alive || !state->active || Clock::now() < state->nextCleanupAttempt ||
                    (!state->historyReap.empty() && Clock::now() < state->nextHistoryAbortAttempt) ||
                    state->cleanupSessions.empty())
                    return;
                auto next = state->cleanupSessions.upper_bound(state->cleanupCursor);
                if (next == state->cleanupSessions.end()) next = state->cleanupSessions.begin();
                session = *next;
                state->cleanupCursor = session;
            }
            const auto purged = importWorker.purgeContextImports(session, kMaiContextImportMaxPurgeMessages);
            std::lock_guard<std::mutex> lock(state->mutex);
            if (purged) {
                state->cleanupWarned.erase(session);
                if (!purged.value().hasMore) state->cleanupSessions.erase(session);
            } else if (purged.error().code() == MaiErrorCode::NotFound) {
                state->cleanupWarned.erase(session);
                state->cleanupSessions.erase(session);
            } else {
                state->nextCleanupAttempt = Clock::now() + std::chrono::seconds(1);
                if (state->cleanupWarned.insert(session).second)
                    state->warnings.push_back({{"code", "history_cleanup_failed"},
                                               {"session_id", session},
                                               {"outcome", "retry_local_cleanup"}});
                return;
            }
        }
    }

    void historyOpened(const Json& request, Json& response) {
        const auto& task = request.at("task");
        std::shared_ptr<HistoryJob> job;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            job = historyJobLocked(*state, task);
        }
        const auto slot = findHistoryReader(text(request, "snapshot_id"));
        require(slot->opened.load() && slot->job == job && slot->job->task == task, "stale_history",
                "Snapshot is not yet open or belongs to another history task");
        if (!job->importId.empty()) {
            require(job->snapshotId == slot->id, "stale_history", "History task was opened with another snapshot");
            response["history_progress"] = historyProgress(*job);
            return;
        }
        const auto sourceIdentity = Json::array({job->source.profile, std::to_string(job->binding.account.sdkAppId),
                                                 job->binding.account.ownerUserId, job->binding.peerId})
                                        .dump();
        const auto sourceKey = "im:" + maiContextImportBatchDigest({{"logical-source", sourceIdentity, 0}});
        require(sourceKey.size() > 3, "operation_failed", "Could not identify context source");
        const auto begun =
            agent->beginContextImport({job->binding.sessionId, sourceKey, slot->id, "Quoted local IM history"});
        if (!begun) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                failHistoryLocked(*state, job->binding.sessionId, "history_stage_failed");
            }
            throw Failure(maiErrorCodeToString(begun.error().code()), begun.error().message());
        }
        job->importId = begun.value().importId;
        job->snapshotId = slot->id;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (job->canceled->load())
                state->historyReap.push_back(job);
            else {
                historyJobLocked(*state, task);
                state->changed.insert(job->binding.sessionId);
            }
        }
        require(!job->canceled->load(), "canceled", "History binding was revoked during staging");
        response["history_progress"] = historyProgress(*job);
    }

    void sweepExpiryLocked(const Scope& scope) {
        if (!state->active || !state->scope || *state->scope != scope) return;
        const auto now = Clock::now();
        std::vector<std::string> expiredHistory;
        for (const auto& entry : state->history)
            if (!entry.second->committed && now >= entry.second->expires) expiredHistory.push_back(entry.first);
        for (const auto& session : expiredHistory) failHistoryLocked(*state, session, "history_timeout");
        std::vector<std::uint64_t> expiredInputs;
        for (const auto& item : state->inputs)
            if ((!item.second.payload || !item.second.input.persistenceKnown) && now - item.second.admitted >= readiness)
                expiredInputs.push_back(item.first);
        for (const auto id : expiredInputs) {
            const auto input = state->inputs.find(id);
            if (input == state->inputs.end()) continue;
            const auto binding = state->router.bindingForPeer(scope, input->second.input.binding.peerId);
            if (binding && binding->historyPending) {
                failHistoryLocked(*state, binding->sessionId, "history_input_timeout");
                continue;
            }
            state->router.discardInput(scope, id);
            state->warnings.push_back({{"code", "input_timeout"}, {"input_id", std::to_string(id)},
                {"session_id", input->second.input.binding.sessionId}, {"outcome", "not_submitted"}});
            state->changed.insert(input->second.input.binding.sessionId);
            state->inputs.erase(input);
        }
    }

    void commitHistory(const std::shared_ptr<HistoryJob>& job) {
        if (!job->eof || job->committed || job->canceled->load()) return;
        const auto signal = signalFor(state, job->binding.sessionId);
        std::uint64_t revision = 0;
        if (!validateWorkspace(job->binding, signal, revision)) return;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            sweepExpiryLocked(job->binding.account);
            if (job->canceled->load()) return;
            historyJobLocked(*state, job->task);
            if (job->canceled->load()) return;
            job->committing = true;
        }
        // 一次性发布在独立 Core 事务内完成；不持有 authority 锁等待 SQLite。
        const auto committed = agent->commitContextImport(job->importId);
        job->committing = false;
        if (!committed) {
            if (committed.error().code() == MaiErrorCode::Busy) return;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                failHistoryLocked(*state, job->binding.sessionId, "history_commit_failed");
            }
            return;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        // SQL 期间仍可能跨过 live 准备期限；先撤销过期授权，再记录已提交的真实事实。
        sweepExpiryLocked(job->binding.account);
        job->committed = true;
        state->cleanupSessions.insert(job->binding.sessionId);
        if (job->canceled->load() || !state->active || !state->scope || *state->scope != job->binding.account) return;
        const auto current = state->router.bindingForPeer(job->binding.account, job->binding.peerId);
        if (!current || current->epoch != job->binding.epoch) return;
        accepted(state->router.completeHistory(job->binding.account, job->binding.peerId, job->binding.epoch));
        state->changed.insert(job->binding.sessionId);
    }

    void historyChunk(const Json& request, Json& response) {
        const auto& task = request.at("task");
        const auto& value = request.at("page");
        std::shared_ptr<HistoryJob> job;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            job = historyJobLocked(*state, task);
        }
        require(!job->importId.empty() && text(value, "snapshot_id") == job->snapshotId, "stale_history",
                "History snapshot was not opened for this import");
        if (!job->acceptedPage.is_null() && job->acceptedPage == value) {
            response["page_accepted"] = true;
            response["history_progress"] = historyProgress(*job);
            return;
        }
        const auto slot = findHistoryReader(job->snapshotId);
        std::shared_ptr<const MaiIMHistoryPage> page;
        {
            std::lock_guard<std::mutex> meta(slot->metadata);
            require(slot->job == job && slot->page && slot->pageValue == value, "stale_history",
                    "History page is not the exact native reader result");
            page = slot->page;
        }
        require(!job->eof && page->sequence == job->nextSequence, "invalid_input",
                "History page sequence is not the next immutable page");
        std::vector<MaiContextImportRecord> records;
        std::uint64_t overlap = 0;
        for (const auto& record : page->records) {
            const auto match =
                state->router.historyMatch(job->binding.account, job->binding.peerId, job->binding.epoch, record.rowId,
                                           record.remoteId.value_or(""), record.fromUserId, record.toUserId);
            require(match != Router::HistoryMatch::Stale, "stale_history",
                    "History binding was revoked during matching");
            if (match == Router::HistoryMatch::AwaitingPersistence) {
                response["page_accepted"] = false;
                response["reason"] = "awaiting_ingress_metadata";
                response["history_progress"] = historyProgress(*job);
                return;
            }
            if (match == Router::HistoryMatch::LiveInput) {
                ++overlap;
                continue;
            }
            const auto formatted = maiFormatIMHistoryContextRecord(record, slot->source);
            if (!formatted) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    failHistoryLocked(*state, job->binding.sessionId, "history_format_failed");
                }
                throw Failure(maiErrorCodeToString(formatted.error().code()), formatted.error().message());
            }
            records.push_back(formatted.value());
        }
        // Reader 的 DTO 字节预算不包含每条重复的出处声明。格式化后重新按 Core
        // 批次预算分块，整页全部暂存成功才确认 Reader；任一失败仍只留下不可见暂存。
        for (std::size_t offset = 0; offset < records.size();) {
            std::vector<MaiContextImportRecord> batch;
            std::size_t bytes = 0;
            while (offset < records.size() && batch.size() < kMaiContextImportMaxBatchRecords) {
                const auto cost = records[offset].sourceId.size() + records[offset].text.size();
                if (!batch.empty() && cost > kMaiContextImportMaxBatchBytes - bytes) break;
                require(cost <= kMaiContextImportMaxBatchBytes, "invalid_input",
                        "Formatted history record exceeds Core batch budget");
                bytes += cost;
                batch.push_back(records[offset++]);
            }
            require(!job->canceled->load(), "canceled", "History import was canceled");
            const auto appended = agent->appendContextImport(job->importId, job->coreSequence, batch);
            if (!appended) {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    failHistoryLocked(*state, job->binding.sessionId, "history_stage_failed");
                }
                throw Failure(maiErrorCodeToString(appended.error().code()), appended.error().message());
            }
            ++job->coreSequence;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            require(!job->canceled->load(), "canceled", "History binding was revoked while appending");
            historyJobLocked(*state, task);
            ++job->nextSequence;
            job->recordCount += records.size();
            job->overlapCount += overlap;
            job->eof = !page->hasMore;
            job->acceptedPage = value;
            state->changed.insert(job->binding.sessionId);
        }
        {
            std::lock_guard<std::mutex> meta(slot->metadata);
            slot->acknowledged = true;
        }
        if (job->eof) commitHistory(job);
        response["page_accepted"] = true;
        response["history_progress"] = historyProgress(*job);
    }

    Payload payload(const Json& request, const std::string& root) {
        require(request.contains("text") && request["text"].is_string(), "invalid_input", "Prepared text is required");
        Payload result;
        result.text = request["text"].get<std::string>();
        require(result.text.size() <= 64 * 1024, "invalid_input", "Text exceeds the ordinary input limit");
        for (const char* kind : {"images", "videos"}) {
            if (!request.contains(kind)) continue;
            require(request[kind].is_array() && request[kind].size() <= 10, "invalid_input",
                    "Invalid media input count");
            for (const auto& item : request[kind]) {
                const auto path = text(item, "path", 8192);
                const auto mime = text(item, "mimeType");
                const auto resolved = maiResolvePathWithinRoot(root, path);
                require(!resolved.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(resolved)) &&
                            !MaiFileSystem::isDirectory(MaiFilePath::fromUtf8(resolved)),
                        "invalid_input", "Prepared media leaves the bound workspace or is missing");
                const auto relative = relativeMediaPath(root, resolved);
                if (std::string(kind) == "images")
                    result.images.push_back({relative, mime});
                else
                    result.videos.push_back({relative, mime});
            }
        }
        require(!result.text.empty() || !result.images.empty() || !result.videos.empty(), "invalid_input",
                "Prepared input is empty");
        return result;
    }

    void receive(const Json& request, Json& response) {
        const auto id = number(request.at("input_id"));
        InputRecord input;
        Scope scope;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            scope = *state->scope;
            const auto found = state->inputs.find(id);
            require(found != state->inputs.end(), "stale_input", "Input was not admitted or was revoked");
            require(sameInput(found->second.input.source, incoming(request, scope, true)), "stale_input",
                    "Prepared input does not match its original live admission");
            input = found->second;
            if (input.payload) {
                response["accepted"] = false;
                response["reason"] = "duplicate_ready";
                return;
            }
        }
        Payload prepared;
        try {
            prepared = payload(request, input.input.binding.workspaceKey);
        } catch (...) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->active && state->scope && *state->scope == scope) {
                const auto binding = state->router.bindingForPeer(scope, input.input.binding.peerId);
                if (binding && binding->epoch == input.input.binding.epoch && binding->historyPending)
                    failHistoryLocked(*state, binding->sessionId, "history_input_failed");
            }
            throw;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        checkLocked(*state, request);
        const auto current = state->inputs.find(id);
        const auto binding = state->router.bindingForPeer(scope, input.input.binding.peerId);
        require(current != state->inputs.end() && binding && binding->epoch == input.input.binding.epoch &&
                    binding->sessionId == input.input.binding.sessionId,
                "stale_input", "Binding changed while preparing input");
        require(Clock::now() - input.admitted < readiness, "input_timeout", "Input readiness budget expired");
        current->second.payload = std::move(prepared);
        response["accepted"] = true;
        response["input_id"] = std::to_string(id);
    }

    bool validateWorkspace(const Router::Binding& binding, const std::shared_ptr<Signal>& signal,
                           std::uint64_t& revision) {
        const auto before = signal ? signal->revision.load() : 0;
        MaiSession session;
        const bool current = agent && agent->getSession(binding.sessionId, session) && session.isRoot() &&
                             canonical(session.directory) == binding.workspaceKey;
        const auto after = signal ? signal->revision.load() : 0;
        if (!current) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->active && state->scope && *state->scope == binding.account)
                revokeLocked(*state, binding.sessionId);
            return false;
        }
        revision = after;
        return before == after;
    }

    void pump(const Json& request) {
        reapHistory();
        std::vector<std::shared_ptr<HistoryJob>> jobs;
        Scope scope;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            scope = *state->scope;
            sweepExpiryLocked(scope);
            for (const auto& entry : state->history) jobs.push_back(entry.second);
        }
        for (const auto& job : jobs)
            if (!job->canceled->load() && job->eof && !job->committed) commitHistory(job);
        Router::Snapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            scope = *state->scope;
            for (auto it = state->deliveries.begin(); it != state->deliveries.end();) {
                if (Clock::now() >= it->second.expires) {
                    state->router.acknowledgeDispatch(it->first.first, it->second.receipt, scope, it->second.workspace,
                                                      it->first.second);
                    state->warnings.push_back({{"code", "delivery_timeout"},
                                               {"token", std::to_string(it->first.first)},
                                               {"session_id", it->second.session},
                                               {"outcome", "unknown"}});
                    it = state->deliveries.erase(it);
                } else
                    ++it;
            }
            snapshot = state->router.snapshot(scope);
        }
        for (const auto& binding : snapshot.bindings) {
            const auto signal = signalFor(state, binding.sessionId);
            std::uint64_t revision = 0;
            if (!validateWorkspace(binding, signal, revision)) continue;
            std::optional<Router::ActiveRoute> route;
            for (const auto& active : state->router.snapshot(scope).active)
                if (active.reservation.input.binding.sessionId == binding.sessionId) route = active;
            if (!route) {
                const auto reserved = state->router.reserveNext(scope, binding.sessionId, binding.workspaceKey,
                                                                !agent->isBusy(binding.sessionId));
                if (reserved.reservation) {
                    Router::ActiveRoute active;
                    active.reservation = *reserved.reservation;
                    route = active;
                }
            }
            if (!route) continue;
            if (route->phase == Router::Phase::Reserved) {
                std::optional<Payload> prepared;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    const auto found = state->inputs.find(route->reservation.input.inputId);
                    if (found != state->inputs.end()) prepared = found->second.payload;
                }
                if (!prepared || agent->isBusy(binding.sessionId)) continue;
                MaiSession session;
                if (!agent->getSession(binding.sessionId, session)) continue;
                try {
                    if (requireModel) requireModel(session.model);
                    if (inputValidator)
                        inputValidator(session.model, !prepared->images.empty() || !prepared->videos.empty());
                } catch (const std::exception&) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->router.releaseReservation(route->reservation.token, scope, binding.workspaceKey, false);
                    state->inputs.erase(route->reservation.input.inputId);
                    if (state->active && state->scope && *state->scope == scope)
                        state->warnings.push_back({{"code", "input_rejected"},
                                                   {"input_id", std::to_string(route->reservation.input.inputId)},
                                                   {"session_id", binding.sessionId},
                                                   {"outcome", "not_submitted"}});
                    continue;
                }
                // 凭据和文件检查不持有控制锁；最终普通 submit 仅跨快速 Core 受理边界。
                if (!validateWorkspace(binding, signal, revision)) continue;
                auto watch = std::make_shared<Watch>();
                {
                    std::lock_guard<std::mutex> lock(signal->mutex);
                    signal->watch = watch;
                }
                std::optional<MaiResult<std::string>> submitted;
                std::vector<std::string> ids;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->active || !state->scope || *state->scope != scope ||
                        !state->router.isCurrent(route->reservation.token, scope, binding.workspaceKey))
                        continue;
                    submitted = agent->submit(
                        MaiSendPrompt{binding.sessionId, prepared->text, prepared->images, prepared->videos});
                    {
                        std::lock_guard<std::mutex> capture(watch->mutex);
                        watch->capturing = false;
                        ids = watch->ids;
                    }
                    if (!*submitted) {
                        const bool busy = submitted->error().code() == MaiErrorCode::Busy;
                        state->router.releaseReservation(route->reservation.token, scope, binding.workspaceKey, busy);
                        if (!busy) {
                            state->inputs.erase(route->reservation.input.inputId);
                            state->warnings.push_back({{"code", "submit_rejected"},
                                                       {"session_id", binding.sessionId},
                                                       {"outcome", "not_submitted"}});
                        }
                    }
                }
                if (!*submitted) continue;
                const auto& result = *submitted;
                // Core 的普通 submit 同步广播恰好 user/assistant；不在事件回调查询存储。
                std::string user;
                for (const auto& id : ids) {
                    MaiMessage message;
                    if (agent->getMessage(binding.sessionId, id, message) && message.role == MaiRole::User) user = id;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->active || !state->scope || *state->scope != scope) continue;
                if (!watch->valid || user.empty() || std::find(ids.begin(), ids.end(), result.value()) == ids.end()) {
                    revokeLocked(*state, binding.sessionId);
                    continue;
                }
                accepted(state->router.bindReceipt(route->reservation.token, {user, result.value(), result.value()},
                                                   scope, binding.workspaceKey));
                state->inputs.erase(route->reservation.input.inputId);
                continue;
            }
            if (route->phase != Router::Phase::Running || !route->receipt || agent->isBusy(binding.sessionId)) continue;
            MaiMessage reply;
            if (!agent->getMessage(binding.sessionId, route->receipt->assistantMessageId, reply) ||
                reply.role != MaiRole::Assistant || !reply.completed)
                continue;
            std::shared_ptr<Watch> watch;
            if (signal) {
                std::lock_guard<std::mutex> lock(signal->mutex);
                watch = signal->watch;
            }
            if (!watch || !watch->valid || watch->failed) {
                state->router.finishWithoutOutput(route->reservation.token, *route->receipt, scope,
                                                  binding.workspaceKey);
                continue;
            }
            std::vector<Json> output;
            if (!reply.text().empty()) output.push_back({{"kind", "text"}, {"text", reply.text()}});
            MaiToolContext context;
            context.root = binding.workspaceKey;
            context.sessionId = binding.sessionId;
            std::set<std::string> mediaPaths;
            for (const auto& part : reply.parts) {
                const auto* tool = std::get_if<MaiToolPart>(&part.body);
                if (!tool || tool->tool != "agent_send_media" || tool->state != MaiToolState::Completed) continue;
                const auto value = Json::parse(tool->output, nullptr, false);
                if (!value.is_object() || value.value("delivery", "") != "current_ai_session") continue;
                const auto prepared = maiPrepareAgentMedia(value.value("path", ""), value.value("type", ""), context);
                if (!prepared || value.value("mime_type", "") != prepared.value().mimeType ||
                    !value.contains("bytes") || !value["bytes"].is_number_unsigned() ||
                    value["bytes"].get<std::uint64_t>() != prepared.value().bytes)
                    continue;
                const auto absolute = maiResolvePathWithinRoot(binding.workspaceKey, prepared.value().relativePath);
                if (!mediaPaths.insert(absolute).second) continue;
                output.push_back({{"kind", prepared.value().type == "audio" ? "file" : prepared.value().type},
                                  {"path", absolute},
                                  {"mime_type", prepared.value().mimeType},
                                  {"caption", value.value("caption", "")}});
            }
            if (!validateWorkspace(binding, signal, revision)) continue;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->active || !state->scope || *state->scope != scope || !watch->valid) continue;
            if (watch->failed) {
                state->router.finishWithoutOutput(route->reservation.token, *route->receipt, scope,
                                                  binding.workspaceKey);
                continue;
            }
            if (output.empty()) {
                state->router.finishWithoutOutput(route->reservation.token, *route->receipt, scope,
                                                  binding.workspaceKey);
                continue;
            }
            if (state->router.beginOutput(route->reservation.token, *route->receipt, scope, binding.workspaceKey,
                                          static_cast<std::uint32_t>(output.size())) != Router::Status::Ok)
                continue;
            for (std::uint32_t index = 0; index < output.size(); ++index) {
                Json delivery = {{"bridge_id", state->bridgeId},
                                 {"scope", scopeJson(scope)},
                                 {"token", std::to_string(route->reservation.token)},
                                 {"dispatch_index", std::to_string(index)},
                                 {"input_id", std::to_string(route->reservation.input.inputId)},
                                 {"source_message_id", route->reservation.input.source.messageId},
                                 {"peer_id", binding.peerId},
                                 {"session_id", binding.sessionId},
                                 {"workspace_key", binding.workspaceKey},
                                 {"user_message_id", route->receipt->userMessageId},
                                 {"assistant_message_id", route->receipt->assistantMessageId},
                                 {"turn_id", route->receipt->turnId}};
                delivery.update(output[index]);
                state->deliveries[{route->reservation.token, index}] = {
                    delivery,          *route->receipt, binding.workspaceKey,
                    binding.sessionId, signal,          watch,
                    revision,          false,           Clock::now() + state->outputBudget};
            }
        }
        purgeRetiredHistory();
    }

    Json handle(const Json& request, const std::function<void()>& beforeMutation = {}, bool issueTasks = true) {
        historyAbortAttempts = 1;
        historyAbortFailedThisRequest = false;
        require(agent != nullptr, "stale_bridge", "Native Agent was invalidated");
        const auto action = text(request, "action");
        Json extra = Json::object();
        if (action == "activate")
            activate(request);
        else {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
            }
            if (action == "bind")
                bind(request, beforeMutation);
            else if (action == "unbind") {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                const auto peer = text(request, "peer_id");
                const auto binding = state->router.bindingForPeer(*state->scope, peer);
                if (binding) {
                    if (beforeMutation) beforeMutation();
                    revokeLocked(*state, binding->sessionId);
                }
            } else if (action == "set_friends") {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                if (setFriendsLocked(friends(request, state->scope->ownerUserId))) state->changed.insert("");
            } else if (action == "takeover") {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                revokeLocked(*state, text(request, "session_id"));
            } else if (action == "logout") {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                clearLocked(*state);
                return {{"ok", true}, {"bridge_id", state->bridgeId}, {"revoked", true}};
            } else if (action == "pump")
                pump(request);
            else if (action == "history_opened")
                historyOpened(request, extra);
            else if (action == "history_chunk")
                historyChunk(request, extra);
            else if (action == "history_failed") {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, request);
                const auto job = historyJobLocked(*state, request.at("task"));
                if (request.contains("reason")) text(request, "reason", 512);
                failHistoryLocked(*state, job->binding.sessionId, "history_source_failed");
            } else if (action != "list" && action != "receive")
                throw Failure("invalid_input", "Unknown binding action");
        }
        reapHistory();
        Json response = projection(action == "activate" ? nullptr : &request, issueTasks);
        response.update(extra);
        if (action == "bind") {
            std::lock_guard<std::mutex> lock(state->mutex);
            checkLocked(*state, request);
            const auto binding = state->router.bindingForPeer(*state->scope, text(request, "peer_id"));
            response["accepted"] = bool(binding);
            response["history_state"] = binding && binding->historyPending ? "pending" : "ready";
        }
        if (action == "receive") receive(request, response);
        if (action == "pump") {
            response["deliveries"] = Json::array();
            std::lock_guard<std::mutex> lock(state->mutex);
            for (const auto& item : state->deliveries)
                if (!item.second.claimed) response["deliveries"].push_back(item.second.value);
            response["warnings"] = state->warnings;
            state->warnings.clear();
        }
        return response;
    }
};

MaiMobileSessionIM::MaiMobileSessionIM(MaiAgent& agent, std::string workspace,
                                       std::function<void(const std::string&)> configured, std::uint64_t readiness,
                                       std::uint64_t outputBudget)
    : impl_(std::make_unique<Impl>(agent, std::move(workspace), std::move(configured), readiness, outputBudget)) {}

MaiMobileSessionIM::~MaiMobileSessionIM() {
    invalidate();
}

void MaiMobileSessionIM::invalidate() {
    if (!impl_) return;
    std::lock_guard<std::timed_mutex> operation(impl_->operation);
    MaiAgent* agent = impl_->agent;
    impl_->agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(registry().mutex);
        registry().entries.erase(impl_->state->bridgeId);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        impl_->state->alive = false;
        clearLocked(*impl_->state);
    }
    // 退订不持有控制锁，已在执行的观察者只保有自有 weak/shared 控制值。
    if (agent && impl_->subscription) agent->eventBus().unsubscribe(impl_->subscription);
    impl_->subscription = 0;
}

void MaiMobileSessionIM::revokeSession(const std::string& session) {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    if (impl_->state->alive && impl_->state->active) revokeLocked(*impl_->state, session);
}

void MaiMobileSessionIM::setChangeHandler(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::timed_mutex> lock(impl_->operation);
    impl_->changed = std::move(callback);
}

void MaiMobileSessionIM::setInputValidator(std::function<void(const std::string&, bool)> callback) {
    std::lock_guard<std::timed_mutex> lock(impl_->operation);
    impl_->inputValidator = std::move(callback);
}

std::string MaiMobileSessionIM::request(const std::string& raw) {
    try {
        const auto request = parse(raw);
        std::unique_lock<std::timed_mutex> operation(impl_->operation);
        auto result = impl_->handle(request);
        impl_->notify();
        return result.dump();
    } catch (const Failure& failure) {
        return error(failure.code, failure.what()).dump();
    } catch (const std::exception&) {
        return error("operation_failed", "Session binding operation failed").dump();
    }
}

std::string MaiMobileSessionIM::historySnapshot(const std::string& raw) {
    try {
        const auto request = parse(raw);
        const auto action = text(request, "action");
        reapHistoryReadersOnIO();
        if (action == "open") {
            const auto& task = request.at("task");
            std::shared_ptr<Authority> state;
            {
                std::lock_guard<std::mutex> lock(registry().mutex);
                const auto found = registry().entries.find(text(task, "bridge_id"));
                if (found != registry().entries.end()) state = found->second.lock();
            }
            require(bool(state), "stale_bridge", "History task backend is unavailable");
            std::shared_ptr<HistoryJob> job;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                checkLocked(*state, task);
                job = historyJobLocked(*state, task);
            }
            std::shared_ptr<HistoryReader> slot;
            {
                std::lock_guard<std::mutex> lock(historyReaders().mutex);
                for (const auto& item : historyReaders().entries)
                    require(item.second->job != job, "busy", "This task already has a history reader");
                require(historyReaders().entries.size() < 2 && historyReaders().next != UINT64_MAX, "busy",
                        "Local history reader capacity is exhausted");
                slot = std::make_shared<HistoryReader>("im-history-" + std::to_string(historyReaders().next++));
                slot->job = job;
                slot->authority = state;
                historyReaders().entries.emplace(slot->id, slot);
            }
            std::unique_lock<std::timed_mutex> io(slot->io);
            MaiIMHistorySnapshotOptions options;
            options.profile = job->source.profile == "ios"       ? MaiIMHistoryProfile::IOS
                              : job->source.profile == "android" ? MaiIMHistoryProfile::Android
                                                                 : MaiIMHistoryProfile::Desktop;
            options.databasePath = job->source.path;
            options.ownerUserId = job->binding.account.ownerUserId;
            options.peerUserId = job->binding.peerId;
            options.sdkAppId = job->binding.account.sdkAppId;
            options.canceled = job->canceled;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(job->expires - Clock::now()).count();
            options.lifetimeTimeoutMillis = static_cast<std::uint64_t>(std::max<std::int64_t>(1, remaining));
            auto opened = MaiIMHistorySnapshot::open(options);
            if (!opened || job->canceled->load() || Clock::now() >= job->expires) {
                if (opened) opened.value().reset();
                {
                    std::lock_guard<std::mutex> lock(historyReaders().mutex);
                    historyReaders().entries.erase(slot->id);
                }
                if (!opened) return error(maiErrorCodeToString(opened.error().code()), opened.error().message()).dump();
                return error("canceled", "History task was revoked while opening").dump();
            }
            slot->reader = std::move(opened.value());
            slot->source = slot->reader->source();
            if (slot->source.ownerUserId != options.ownerUserId || slot->source.peerUserId != options.peerUserId ||
                slot->source.profile != options.profile || slot->source.provenance != job->source.provenance ||
                (options.profile == MaiIMHistoryProfile::IOS && slot->source.sdkAppId != options.sdkAppId)) {
                slot->reader.reset();
                {
                    std::lock_guard<std::mutex> lock(historyReaders().mutex);
                    historyReaders().entries.erase(slot->id);
                }
                return error("stale_scope", "Opened history source does not match its trusted scope").dump();
            }
            slot->opened.store(true);
            return Json{{"ok", true}, {"snapshot_id", slot->id}, {"source", historySourceJson(slot->source)}}.dump();
        }
        require(action == "read" || action == "close", "invalid_input", "Unknown history snapshot action");
        const auto slot = findHistoryReader(text(request, "snapshot_id"));
        std::unique_lock<std::timed_mutex> io(slot->io, std::defer_lock);
        require(io.try_lock_for(std::chrono::seconds(5)), "busy", "History reader is already in use");
        if (action == "close") {
            slot->reader.reset();
            {
                std::lock_guard<std::mutex> lock(historyReaders().mutex);
                historyReaders().entries.erase(slot->id);
            }
            return Json{{"ok", true}, {"closed", true}, {"snapshot_id", slot->id}}.dump();
        }
        require(slot->reader && !slot->job->canceled->load() && Clock::now() < slot->job->expires, "canceled",
                "History snapshot was canceled or expired");
        {
            std::lock_guard<std::mutex> meta(slot->metadata);
            if (!slot->acknowledged) return slot->pageValue.dump();
            if (slot->page && !slot->page->hasMore) return slot->pageValue.dump();
        }
        auto page = slot->reader->read();
        if (!page || slot->job->canceled->load()) {
            slot->reader.reset();
            {
                std::lock_guard<std::mutex> lock(historyReaders().mutex);
                historyReaders().entries.erase(slot->id);
            }
            if (!page) return error(maiErrorCodeToString(page.error().code()), page.error().message()).dump();
            return error("canceled", "History snapshot was revoked during reading").dump();
        }
        Json records = Json::array();
        for (const auto& record : page.value().records) records.push_back(historyRecordJson(record));
        Json value{{"ok", true},
                   {"snapshot_id", slot->id},
                   {"sequence", std::to_string(page.value().sequence)},
                   {"records", records},
                   {"has_more", page.value().hasMore},
                   {"coverage", slot->source.coverage},
                   {"provenance", slot->source.provenance},
                   {"source", historySourceJson(slot->source)}};
        {
            std::lock_guard<std::mutex> meta(slot->metadata);
            slot->page = std::make_shared<const MaiIMHistoryPage>(std::move(page.value()));
            slot->pageValue = value;
            slot->acknowledged = false;
        }
        return value.dump();
    } catch (const Failure& failure) {
        return error(failure.code, failure.what()).dump();
    } catch (const std::exception&) {
        return error("operation_failed", "Local history snapshot operation failed").dump();
    }
}

std::string MaiMobileSessionIM::control(const std::string& raw) {
    try {
        const auto request = parse(raw);
        const auto action = text(request, "action");
        const bool deliveryAction = action == "claim" || action == "ack";
        const auto& owner = deliveryAction ? request.at("delivery") : request;
        const auto bridge = text(owner, "bridge_id");
        std::shared_ptr<Authority> state;
        {
            std::lock_guard<std::mutex> lock(registry().mutex);
            const auto found = registry().entries.find(bridge);
            if (found != registry().entries.end()) state = found->second.lock();
        }
        require(bool(state), "stale_bridge", "Native binding lifetime is unavailable");
        std::lock_guard<std::mutex> lock(state->mutex);
        checkLocked(*state, owner);
        if (action == "admit") {
            const auto input = incoming(request, *state->scope, flag(request, "has_content"));
            if (!state->acceptedFriends.count(input.peerId))
                return Json{{"ok", true}, {"accepted", false}, {"reason", "not_eligible"}}.dump();
            const auto accepted = state->router.accept(input);
            Json result{{"ok", true},
                        {"accepted", accepted.status == Router::Status::Ok},
                        {"reason", statusName(accepted.status)}};
            if (accepted.input) {
                state->inputs.emplace(accepted.input->inputId, InputRecord{*accepted.input, Clock::now(), {}});
                result["input_id"] = std::to_string(accepted.input->inputId);
            }
            if (accepted.status == Router::Status::QueueFull) {
                const auto binding = state->router.bindingForPeer(*state->scope, input.peerId);
                if (binding && binding->historyPending) {
                    failHistoryLocked(*state, binding->sessionId, "history_live_overflow");
                    result["ok"] = false;
                    result["history_aborted"] = true;
                    result["errorCode"] = "history_live_overflow";
                }
            }
            return result.dump();
        }
        if (action == "persisted_input") {
            const auto id = number(request.at("input_id"));
            const auto found = state->inputs.find(id);
            require(found != state->inputs.end(), "stale_input", "Metadata input was revoked or already consumed");
            const auto rowId = text(request, "source_row_id");
            const auto remoteId = request.contains("remote_id") && !request["remote_id"].is_null()
                                      ? std::optional<std::string>(text(request, "remote_id"))
                                      : std::nullopt;
            const bool inserted = flag(request, "inserted");
            accepted(state->router.confirmPersisted(*state->scope, id, inserted, rowId, remoteId.value_or("")));
            if (!inserted) {
                state->inputs.erase(found);
                return Json{{"ok", true}, {"accepted", false}, {"reason", "persisted_duplicate"}}.dump();
            }
            for (const auto& queued : state->router.snapshot(*state->scope).pending)
                if (queued.inputId == id) found->second.input = queued;
            return Json{{"ok", true}, {"accepted", true}}.dump();
        }
        if (action == "abandon_input") {
            const auto id = number(request.at("input_id"));
            const auto input = state->inputs.find(id);
            if (input != state->inputs.end()) {
                const auto binding = state->router.bindingForPeer(*state->scope, input->second.input.binding.peerId);
                if (binding && binding->historyPending) {
                    failHistoryLocked(*state, binding->sessionId, "history_input_failed");
                    return Json{{"ok", false},
                                {"abandoned", true},
                                {"history_aborted", true},
                                {"errorCode", "history_input_failed"}}
                        .dump();
                }
            }
            const auto status = state->router.discardInput(*state->scope, id);
            if (status == Router::Status::Ok) state->inputs.erase(id);
            return Json{{"ok", status == Router::Status::Ok},
                        {"abandoned", status == Router::Status::Ok},
                        {"reason", statusName(status)}}
                .dump();
        }
        if (action == "takeover") {
            revokeLocked(*state, text(request, "session_id"));
            return Json{{"ok", true}, {"revoked", true}}.dump();
        }
        if (action == "logout") {
            clearLocked(*state);
            return Json{{"ok", true}, {"revoked", true}}.dump();
        }
        require(deliveryAction, "invalid_input", "Unknown binding control action");
        const auto token = number(owner.at("token"));
        const auto index64 = number(owner.at("dispatch_index"), true);
        require(index64 <= std::numeric_limits<std::uint32_t>::max(), "invalid_input", "Invalid dispatch index");
        const auto index = static_cast<std::uint32_t>(index64);
        const auto found = state->deliveries.find({token, index});
        if (found == state->deliveries.end())
            return Json{{"ok", true}, {"allowed", false}, {"acknowledged", false}, {"reason", "stale_delivery"}}.dump();
        require(found->second.value == owner, "stale_delivery", "Delivery is not the exact immutable native value");
        auto& delivery = found->second;
        const bool current =
            state->router.isOutputCurrent(token, delivery.receipt, *state->scope, delivery.workspace) &&
            delivery.signal && delivery.signal->revision.load() == delivery.revision && delivery.receiptWatch &&
            delivery.receiptWatch->valid && !delivery.receiptWatch->failed && Clock::now() < delivery.expires;
        if (action == "claim") {
            const bool allowed = current && !delivery.claimed;
            if (allowed) delivery.claimed = true;
            return Json{{"ok", true}, {"allowed", allowed}}.dump();
        }
        const auto status =
            state->router.acknowledgeDispatch(token, delivery.receipt, *state->scope, delivery.workspace, index);
        state->deliveries.erase(found);
        return Json{{"ok", true}, {"acknowledged", status == Router::Status::Ok}}.dump();
    } catch (const Failure& failure) {
        return error(failure.code, failure.what()).dump();
    } catch (const std::exception&) {
        return error("invalid_input", "Invalid Session binding control request").dump();
    }
}

MaiToolResult MaiMobileSessionIM::executeBindingTool(const std::string& raw, const MaiToolContext& context) {
    bool started = false;
    bool mutation = false;
    auto fail = [&](MaiErrorCode code, const std::string& message) {
        MaiToolExecution execution;
        execution.started = started;
        execution.status = started ? MaiToolExecutionStatus::Unknown : MaiToolExecutionStatus::NotStarted;
        execution.effectStatus = started && mutation ? MaiToolEffectStatus::Unknown : MaiToolEffectStatus::None;
        execution.nextAction = started && mutation ? MaiToolNextAction::InspectState : MaiToolNextAction::None;
        execution.recoveryTool = started && mutation ? "maichat_session_im_binding" : "";
        return MaiToolResult::failure(code, message).withExecution(execution);
    };
    try {
        const auto arguments = parse(raw);
        const auto action = text(arguments, "action");
        require(action == "list" || action == "bind" || action == "unbind", "invalid_input",
                "Tool action must be list, bind or unbind");
        mutation = action != "list";
        require(arguments.size() == (action == "list"   ? 1
                                     : action == "bind" ? 3
                                                        : 2),
                "invalid_input", "Tool arguments must use the exact flat action fields");
        if (mutation) text(arguments, "peer_id");
        if (action == "bind") text(arguments, "session_id");
        if (context.sessionAgent != impl_->identity)
            return fail(MaiErrorCode::NotConfigured, "Tool Agent identity does not match this backend");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        std::unique_lock<std::timed_mutex> operation(impl_->operation, std::defer_lock);
        while (!operation.try_lock_for(std::chrono::milliseconds(5))) {
            if (context.isCanceled()) return fail(MaiErrorCode::Canceled, "Binding tool was canceled before execution");
            if (Clock::now() >= deadline)
                return fail(MaiErrorCode::Busy, "Binding tool is waiting for another native operation");
        }
        if (context.isCanceled()) return fail(MaiErrorCode::Canceled, "Binding tool was canceled before execution");
        if (Clock::now() >= deadline) return fail(MaiErrorCode::Busy, "Binding tool execution budget expired");
        require(impl_->agent != nullptr, "stale_bridge", "Native Agent was invalidated");
        MaiSession caller;
        require(impl_->agent->getSession(context.sessionId, caller) && caller.isRoot(), "not_eligible",
                "Binding tool requires its owning local root Session");
        Json request = arguments;
        {
            std::lock_guard<std::mutex> lock(impl_->state->mutex);
            require(impl_->state->active && impl_->state->scope, "not_active",
                    "Activate the trusted account before managing bindings");
            request["bridge_id"] = impl_->state->bridgeId;
            request["scope"] = scopeJson(*impl_->state->scope);
            if (action == "bind") request["expected_workspace_key"] = impl_->workspace;
        }
        // 仅在本次同步调用栈读取原生取消标志，绝不交给事件观察者或静态控制注册表。
        const auto beforeMutation = [&] {
            if (context.isCanceled()) throw Failure("canceled", "Binding tool was canceled before mutation");
            if (Clock::now() >= deadline) throw Failure("timeout", "Binding tool budget expired before mutation");
            started = true;
        };
        if (!mutation) started = true;
        auto result = impl_->handle(request, beforeMutation, false);
        if (context.isCanceled())
            return fail(MaiErrorCode::Canceled, "Binding operation started; inspect current bindings before retrying");
        impl_->notify();
        if (context.isCanceled())
            return fail(MaiErrorCode::Canceled, "Binding operation started; inspect current bindings before retrying");
        MaiToolExecution execution;
        execution.started = true;
        execution.status = MaiToolExecutionStatus::Completed;
        execution.effectStatus = mutation ? MaiToolEffectStatus::Confirmed : MaiToolEffectStatus::None;
        return MaiToolResult::success(result.dump()).withExecution(execution);
    } catch (const Failure& failure) {
        return fail(failure.code == "invalid_input" ? MaiErrorCode::InvalidInput
                    : failure.code == "canceled"    ? MaiErrorCode::Canceled
                    : failure.code == "timeout"     ? MaiErrorCode::Busy
                                                    : MaiErrorCode::NotConfigured,
                    failure.what());
    } catch (const std::exception&) {
        return fail(MaiErrorCode::Internal, "Binding operation failed; inspect state before retrying");
    }
}
