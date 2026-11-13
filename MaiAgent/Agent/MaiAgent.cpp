// 文件作用：组织 Agent 的公开操作、事件流和会话执行入口。
// 主要入口：submit、pollSpecialistTasks、forwardSpecialistReply；负责提交操作和恢复异步任务结果。
#include "MaiAgent.h"

#include "MaiCloudModelCatalog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <json.hpp>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiThread.h"

#include "MaiContextBuilder.h"
#include "MaiEventEmitter.h"
#include "MaiSessionTitler.h"
#include "MaiTurnRunner.h"

MaiSendPrompt::MaiSendPrompt(std::string sessionIdValue, std::string textValue)
    : sessionId(std::move(sessionIdValue)), text(std::move(textValue)) {}

MaiSendPrompt::MaiSendPrompt(std::string sessionIdValue, std::string textValue,
                             std::vector<MaiModelImage> imageValues)
    : sessionId(std::move(sessionIdValue)),
      text(std::move(textValue)),
      images(std::move(imageValues)) {}

MaiSendPrompt::MaiSendPrompt(std::string sessionIdValue, std::string textValue,
                             std::vector<MaiModelImage> imageValues,
                             std::vector<MaiVideoPart> videoValues)
    : sessionId(std::move(sessionIdValue)),
      text(std::move(textValue)),
      images(std::move(imageValues)),
      videos(std::move(videoValues)) {}

MaiSendPrompt::MaiSendPrompt(std::string sessionIdValue, std::string textValue,
                             std::vector<MaiModelImage> imageValues,
                             std::vector<MaiVideoPart> videoValues,
                             std::string quotedMessageIdValue)
    : sessionId(std::move(sessionIdValue)),
      text(std::move(textValue)),
      images(std::move(imageValues)),
      videos(std::move(videoValues)),
      quotedMessageId(std::move(quotedMessageIdValue)) {}

namespace {

struct SteeringReceipt {
    std::string sessionId;
    std::string turnId;
    std::string requestId;
    std::string fingerprint;
    std::string messageId;
};

// 一轮对话的在跑状态。输入闸门同时串行化补充落库、assistant 分段和收尾，
// 不能用 cancel 替代它，否则“补充当前任务”会打断正在执行的真实工具。
struct ActiveTurn {
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::mutex startMutex;
    std::condition_variable startWake;
    bool ready = false;
    std::mutex steeringMutex;
    std::string id;
    bool acceptingSteering = false;
    std::vector<std::string> pendingSteering;
    std::vector<SteeringReceipt> steeringReceipts;

    void waitReady() {
        std::unique_lock<std::mutex> lock(startMutex);
        startWake.wait(lock, [this] { return ready; });
    }

    void signalReady() {
        {
            std::lock_guard<std::mutex> lock(startMutex);
            ready = true;
        }
        startWake.notify_one();
    }
};

std::string steeringFingerprint(const MaiSteerPrompt& prompt) {
    using Json = nlohmann::json;
    Json images = Json::array();
    Json videos = Json::array();
    for (const auto& image : prompt.images) images.push_back({image.path, image.mimeType});
    for (const auto& video : prompt.videos) videos.push_back({video.path, video.mimeType});
    return Json{{"text", prompt.text},
                {"images", images},
                {"videos", videos},
                {"quote", prompt.quotedMessageId}}
        .dump();
}

std::string inputFingerprint(const MaiMessage& message) {
    MaiSteerPrompt input;
    input.text = message.text();
    for (const auto& part : message.parts) {
        if (const auto* image = std::get_if<MaiImagePart>(&part.body))
            input.images.push_back({image->path, image->mimeType});
        else if (const auto* video = std::get_if<MaiVideoPart>(&part.body))
            input.videos.push_back(*video);
        else if (const auto* quote = std::get_if<MaiQuotePart>(&part.body))
            input.quotedMessageId = quote->messageId;
    }
    return steeringFingerprint(input);
}

constexpr const char* kInterruptedToolError =
    "The app stopped before a final tool result was recorded. The operation may have already "
    "produced effects. Inspect its current state or query the existing task before considering "
    "another attempt; do not blindly repeat the operation.";

const char* specialistStatusName(MaiSpecialistTaskStatus status) {
    switch (status) {
        case MaiSpecialistTaskStatus::Submitted: return "submitted";
        case MaiSpecialistTaskStatus::Running: return "running";
        case MaiSpecialistTaskStatus::NeedsInput: return "needs_input";
        case MaiSpecialistTaskStatus::Succeeded: return "succeeded";
        case MaiSpecialistTaskStatus::Failed: return "failed";
        case MaiSpecialistTaskStatus::Canceled: return "canceled";
    }
    return "unknown";
}

std::string jsonText(const nlohmann::json& value, const char* field) {
    return value.is_object() && value.contains(field) && value[field].is_string()
               ? value[field].get<std::string>()
               : std::string{};
}

std::string quotedPreview(const MaiMessage& message) {
    std::string preview = message.text();
    if (preview.empty()) {
        for (const MaiMessagePart& part : message.parts) {
            if (std::holds_alternative<MaiImagePart>(part.body)) return "Image message";
            if (std::holds_alternative<MaiVideoPart>(part.body)) return "Video message";
            if (const auto* tool = std::get_if<MaiToolPart>(&part.body)) {
                if (tool->tool == "agent_send_media" && tool->state == MaiToolState::Completed)
                    return "Media message";
            }
        }
        return "Message";
    }
    std::replace_if(
        preview.begin(), preview.end(), [](unsigned char byte) { return byte < 0x20; }, ' ');
    if (preview.size() > 160) {
        std::size_t length = 160;
        while (length > 0 && (static_cast<unsigned char>(preview[length]) & 0xc0) == 0x80) --length;
        preview.resize(length);
        preview += "...";
    }
    return preview;
}

std::string specialistFailureNotice(const MaiSpecialistTask& task) {
    std::string reason = task.errorText.empty() ? "The provider gave no reason." : task.errorText;
    const bool statusUnavailable = reason.rfind("Status check failed: ", 0) == 0;
    const bool outputUnavailable = reason.rfind("Output delivery failed: ", 0) == 0;
    if (statusUnavailable) reason.erase(0, std::string("Status check failed: ").size());
    if (outputUnavailable) reason.erase(0, std::string("Output delivery failed: ").size());
    std::replace_if(
        reason.begin(), reason.end(), [](unsigned char byte) { return byte < 0x20; }, ' ');
    if (reason.size() > 500) {
        std::size_t length = 500;
        while (length > 0 && (static_cast<unsigned char>(reason[length]) & 0xc0) == 0x80) --length;
        reason.resize(length);
        reason += "...";
    }
    const bool image = task.specialistName.find("image") != std::string::npos;
    const char* prefix =
        statusUnavailable
            ? (image ? "Image task status unavailable. " : "Video task status unavailable. ")
        : outputUnavailable ? (image ? "Image output unavailable. " : "Video output unavailable. ")
                            : (image ? "Image task failed. " : "Video task failed. ");
    return std::string(prefix) + reason;
}

bool hasDeliveredMediaForProviderTask(const MaiSessionStore& store, const MaiSpecialistTask& task) {
    if (task.providerTaskId.empty() || task.outputPath.empty()) return false;
    std::string beforeId;
    constexpr std::size_t kPageSize = 100;
    for (;;) {
        const auto page = store.listMessagesPage(task.ownerSessionId, beforeId, kPageSize);
        if (page.empty()) return false;
        for (const MaiMessage& message : page) {
            for (const MaiMessagePart& part : message.parts) {
                const auto* tool = std::get_if<MaiToolPart>(&part.body);
                if (tool == nullptr || tool->tool != "agent_send_media" ||
                    tool->state != MaiToolState::Completed)
                    continue;
                if (tool->input.find(task.providerTaskId) != std::string::npos ||
                    tool->output.find(task.providerTaskId) != std::string::npos)
                    return true;
            }
        }
        if (page.size() < kPageSize || page.back().id == beforeId) return false;
        beforeId = page.back().id;
    }
}

}  // namespace

struct MaiAgent::Runtime {
    std::unique_ptr<MaiSessionStore> store;
    std::unique_ptr<MaiModelClient> model;
    std::unique_ptr<MaiToolRegistry> tools;
    Options options;

    MaiEventBus bus;
    MaiEventEmitter emitter{bus};
    MaiContextBuilder context;
    MaiSessionTitler titler;
    // 闸门归门面持有而不是归某一轮：用户的"本会话都允许"要跨轮活着，
    // 而且界面查"还有什么在等授权"时可能一轮都没在跑。
    std::unique_ptr<MaiPermissionGate> permissions;
    std::unique_ptr<MaiQuestionGate> questions;
    // 指回门面。子 Agent 那组工具要通过它起会话、派活、等结果，
    // 而那些动作只有门面做得了（要走 submit，才有 Busy 判断和事件）。
    MaiAgent* owner = nullptr;

    mutable std::mutex mutex;
    std::condition_variable turnFinished;
    std::condition_variable workersFinished;
    size_t liveWorkers = 0;
    std::unordered_map<std::string, std::shared_ptr<ActiveTurn>> active;
    std::set<std::string> startingSessions;
    // commit 仅短暂预占启动边界；磁盘事务在 Runtime 锁外执行。
    std::set<std::string> contextCommittingSessions;
    std::unordered_map<std::string, std::deque<MaiMessage>> inputQueues;
    std::set<std::string> loadedInputQueues;
    std::set<std::string> pausedInputs;
    bool stopping = false;
    // 可复制的导入能力只弱引用 Runtime。关闭标志在门面析构的第一步撤销，
    // 不等待已开始的数据库操作；事件计数仅覆盖快速同步回调交接。
    std::atomic<bool> contextImportOwnerOpen{true};
    std::mutex contextImportEventMutex;
    std::condition_variable contextImportEventFinished;
    std::size_t contextImportEvents = 0;
    // 结束后的请求仍能幂等重试，但凭据不能随聊天历史无限增长。
    std::deque<SteeringReceipt> recentSteering;
    // 被 close_agent 收掉的子 Agent。只是不再占名额，消息一条不删。
    std::set<std::string> closedSubAgents;
    std::atomic<bool> stopSpecialists{false};
    std::thread specialistWorker;
    std::mutex specialistMutex;
    std::condition_variable specialistWake;
    // 只有专业工具工作线程会读写此表。短暂查询故障有界重试；
    // 达到上限只说明状态无法确认，不代表云端生成失败。
    std::unordered_map<std::string, int> specialistStatusFailures;

    ~Runtime() {
        stopActiveTurns();
    }

    bool canImportContext() const {
        if (!contextImportOwnerOpen.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping && contextImportOwnerOpen.load(std::memory_order_acquire);
    }

    void revokeContextImportOwner() {
        std::unique_lock<std::mutex> lock(contextImportEventMutex);
        contextImportOwnerOpen.store(false, std::memory_order_release);
        contextImportEventFinished.wait(lock, [this] { return contextImportEvents == 0; });
    }

    void emitContextImported(const std::string& sessionId, const std::string& importId) {
        {
            std::lock_guard<std::mutex> lock(contextImportEventMutex);
            if (!contextImportOwnerOpen.load(std::memory_order_acquire)) return;
            ++contextImportEvents;
        }
        struct EventHandoff {
            Runtime& runtime;
            ~EventHandoff() {
                std::lock_guard<std::mutex> lock(runtime.contextImportEventMutex);
                --runtime.contextImportEvents;
                runtime.contextImportEventFinished.notify_all();
            }
        } handoff{*this};
        // 不持有 Runtime、存储或事件交接锁调用订阅者。门面关闭先撤销新交接，
        // 再等待已经进入的短回调完成；不会为等待 SQLite 而阻塞门面关闭。
        emitter.emitSession(MaiEventType::ContextImported, sessionId, importId);
    }

    MaiResult<MaiContextImportProgress> beginContextImport(const MaiContextImportSpec& spec) {
        if (!canImportContext()) return {MaiErrorCode::Canceled, "Context import owner is closed."};
        if (const auto error = maiValidateContextImportSpec(spec)) return error;
        MaiSession session;
        if (!store->getSession(spec.sessionId, session))
            return {MaiErrorCode::NotFound, "Context import session is unavailable."};
        if (!session.isRoot())
            return {MaiErrorCode::InvalidInput, "External context requires a root session."};
        auto result = store->beginContextImport(spec);
        if (result && !contextImportOwnerOpen.load(std::memory_order_acquire)) {
            // 重建者可能已完成启动清理；不能把迟到的 stage0 留给新 owner 永久等待。
            store->abortContextImport(result.value().importId);
            return {MaiErrorCode::Canceled, "Context import owner closed while staging began."};
        }
        return result;
    }

    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& importId, std::uint64_t sequence,
        const std::vector<MaiContextImportRecord>& records) {
        if (!canImportContext()) return {MaiErrorCode::Canceled, "Context import owner is closed."};
        auto result = store->appendContextImport(importId, sequence, records);
        if (result && !contextImportOwnerOpen.load(std::memory_order_acquire)) {
            store->abortContextImport(importId);
            return {MaiErrorCode::Canceled, "Context import owner closed while staging a batch."};
        }
        return result;
    }

    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& importId) {
        if (!canImportContext()) return {MaiErrorCode::Canceled, "Context import owner is closed."};
        const auto before = store->inspectContextImport(importId);
        if (!before || before.value().committed) return before;
        const auto sessionId = before.value().sessionId;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!contextImportOwnerOpen.load(std::memory_order_acquire) || stopping)
                return {MaiErrorCode::Canceled, "Context import owner is closed."};
            if (active.count(sessionId) || startingSessions.count(sessionId))
                return {MaiErrorCode::Busy,
                        "Stop the active turn before publishing external context."};
            startingSessions.insert(sessionId);
            contextCommittingSessions.insert(sessionId);
        }
        auto result = store->commitContextImport(importId);
        {
            std::lock_guard<std::mutex> lock(mutex);
            contextCommittingSessions.erase(sessionId);
            startingSessions.erase(sessionId);
        }
        if (result) emitContextImported(sessionId, importId);
        return result;
    }

    MaiError abortContextImport(const std::string& importId) {
        if (!canImportContext()) return {MaiErrorCode::Canceled, "Context import owner is closed."};
        return store->abortContextImport(importId);
    }

    MaiResult<MaiContextImportPurgeResult> purgeContextImports(const std::string& sessionId,
                                                               std::size_t maxMessages) {
        if (!canImportContext()) return {MaiErrorCode::Canceled, "Context import owner is closed."};
        return store->purgeContextImports(sessionId, maxMessages);
    }

    void stopActiveTurns() {
        // 门面析构先显式停掉原有轮次，让捕获门面的旧 worker 在门面仍有效时结束。
        // 暂存能力只能延长单次存储依赖，不能留下仍调用门面的模型/Host 任务。
        std::vector<std::shared_ptr<ActiveTurn>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            for (auto& [_, turn] : active) {
                turn->cancel.store(true, std::memory_order_relaxed);
                pending.push_back(turn);
            }
        }
        // 卡在等授权的线程看不见 cancel 标志（它睡在闸门的 condition_variable 上），必须显式叫醒，
        // 否则下面的 join 要等满闸门那 250ms 的兜底轮询。靠兜底能过，
        // 但让退出路径依赖一个安全网是不对的。
        if (permissions) {
            for (const auto& request : permissions->listPending())
                permissions->cancelSession(request.sessionId);
        }
        if (questions) {
            for (const auto& request : questions->listPending())
                questions->cancelSession(request.sessionId);
        }
        // retire 会先 detach+erase，然后才发完通知。只等 active 会漏掉这段尾声。
        std::unique_lock<std::mutex> lock(mutex);
        workersFinished.wait(lock, [this] { return liveWorkers == 0; });
    }

    void markWorkerStarted() {
        std::lock_guard<std::mutex> lock(mutex);
        ++liveWorkers;
    }

    void markWorkerFinished() {
        std::lock_guard<std::mutex> lock(mutex);
        --liveWorkers;
        // 析构方看到零后就能释放 Runtime；通知必须在解锁前完成。
        workersFinished.notify_all();
    }

    MaiTurnRunner::Dependencies dependencies(const std::string& sessionId,
                                             const std::shared_ptr<ActiveTurn>& turn) {
        MaiTurnRunner::Dependencies dependencies;
        dependencies.store = store.get();
        dependencies.model = model.get();
        dependencies.emitter = &emitter;
        dependencies.context = &context;
        dependencies.tools = tools.get();
        dependencies.titler = &titler;
        dependencies.permissions = permissions.get();
        dependencies.questions = questions.get();
        dependencies.subAgents = owner;
        dependencies.sessionAgent = owner;
        dependencies.defaultModel = options.defaultModel;
        dependencies.reasoningEffort = options.reasoningEffort;
        dependencies.reasoningEffortProvider = options.reasoningEffortProvider;
        dependencies.baseInstructions = options.baseInstructions;
        dependencies.maxIterations = options.maxToolIterations;
        dependencies.approvalPolicy = options.approvalPolicy;
        dependencies.fileAccessRoot = options.fileAccessRoot;
        dependencies.allowOutsideWorkingDirectory = options.allowOutsideWorkingDirectory;
        dependencies.decodeText = options.decodeText;
        dependencies.pendingSteering = [turn] {
            std::lock_guard<std::mutex> lock(turn->steeringMutex);
            return turn->pendingSteering;
        };
        dependencies.closeSteering = [turn] {
            std::lock_guard<std::mutex> lock(turn->steeringMutex);
            turn->acceptingSteering = false;
        };
        dependencies.advanceSteering =
            [this, sessionId, turn](MaiMessage& assistant, bool closeIfEmpty) -> MaiResult<bool> {
            std::string completedId;
            {
                std::lock_guard<std::mutex> runtimeLock(mutex);
                std::lock_guard<std::mutex> lock(turn->steeringMutex);
                if (turn->cancel.load(std::memory_order_relaxed)) {
                    turn->acceptingSteering = false;
                    return false;
                }
                if (turn->pendingSteering.empty()) {
                    if (closeIfEmpty) turn->acceptingSteering = false;
                    return false;
                }
                MaiMessage completed = assistant;
                completed.completed = MaiTime::getCurrentTime();
                store->putMessage(sessionId, completed);
                if (const MaiError error = store->lastWriteError()) {
                    turn->acceptingSteering = false;
                    return error;
                }
                MaiMessage next;
                next.id = MaiIdGenerator::newMessageId();
                next.role = MaiRole::Assistant;
                next.created = MaiTime::getCurrentTime();
                store->putMessage(sessionId, next);
                if (const MaiError error = store->lastWriteError()) {
                    turn->acceptingSteering = false;
                    return error;
                }
                if (const MaiError error = consumeInputsLocked(sessionId, turn->pendingSteering)) {
                    turn->acceptingSteering = false;
                    return error;
                }
                completedId = completed.id;
                assistant = std::move(next);
                // 新 assistant 的 ID 已在同一把锁内生成并落库。之后到来的 user
                // 必定位于新段之后，下一次安全边界前仍由 pending 过滤。
                turn->pendingSteering.clear();
            }
            emitter.emitMessage(MaiEventType::MessageUpdated, sessionId, completedId);
            emitter.emitMessage(MaiEventType::MessageUpdated, sessionId, assistant.id);
            return true;
        };
        return dependencies;
    }

    // Runtime 锁在外，turn 锁在内；当前工具结束且剩余项补齐未执行回执后，才清掉 pending。
    MaiError consumeInputsLocked(const std::string& sessionId,
                                 const std::vector<std::string>& ids) {
        auto& queue = inputQueues[sessionId];
        for (auto it = queue.begin(); it != queue.end();) {
            if (std::find(ids.begin(), ids.end(), it->id) == ids.end()) {
                ++it;
                continue;
            }
            MaiMessage consumed = *it;
            consumed.pendingInput = false;
            store->putMessage(sessionId, consumed);
            if (const auto error = store->lastWriteError()) return error;
            it = queue.erase(it);
        }
        return {};
    }

    void offerInputsLocked(const std::string& sessionId, const std::shared_ptr<ActiveTurn>& turn) {
        std::lock_guard<std::mutex> lock(turn->steeringMutex);
        if (!turn->acceptingSteering || turn->cancel.load() || pausedInputs.count(sessionId))
            return;
        for (const auto& message : inputQueues[sessionId])
            if (std::find(turn->pendingSteering.begin(), turn->pendingSteering.end(), message.id) ==
                turn->pendingSteering.end())
                turn->pendingSteering.push_back(message.id);
    }

    std::shared_ptr<ActiveTurn> startInputTurnLocked(const std::string& sessionId,
                                                     MaiMessage& assistant) {
        if (stopping || pausedInputs.count(sessionId) || active.count(sessionId) ||
            startingSessions.count(sessionId) || inputQueues[sessionId].empty())
            return {};
        assistant.id = MaiIdGenerator::newMessageId();
        assistant.role = MaiRole::Assistant;
        assistant.created = MaiTime::getCurrentTime();
        store->putMessage(sessionId, assistant);
        if (store->lastWriteError()) return {};
        std::vector<std::string> inputs;
        for (const auto& message : inputQueues[sessionId]) inputs.push_back(message.id);
        if (consumeInputsLocked(sessionId, inputs)) return {};
        auto turn = std::make_shared<ActiveTurn>();
        turn->id = assistant.id;
        turn->acceptingSteering = true;
        active[sessionId] = turn;
        ++liveWorkers;
        auto nextDependencies = dependencies(sessionId, turn);
        turn->worker = std::thread([this, sessionId, turn, nextDependencies, assistant] {
            turn->waitReady();
            MaiThread::setCurrentName("mai-turn");
            MaiTurnRunner runner(nextDependencies, sessionId, assistant);
            runner.run(turn->cancel);
            retire(sessionId, turn);
            markWorkerFinished();
        });
        return turn;
    }

    void retire(const std::string& sessionId, const std::shared_ptr<ActiveTurn>& turn) {
        std::shared_ptr<ActiveTurn> next;
        MaiMessage assistant;
        MaiError startError;
        {
            std::lock_guard<std::mutex> lock(mutex);
            {
                std::lock_guard<std::mutex> inputLock(turn->steeringMutex);
                turn->acceptingSteering = false;
            }
            auto it = active.find(sessionId);
            // 只有还是自己这一轮时才移除：可能已经有新的一轮顶上来了。
            if (it != active.end() && it->second == turn) {
                // 正在执行的就是这个线程，不能 join 自己。
                if (it->second->worker.joinable()) it->second->worker.detach();
                active.erase(it);
                next = startInputTurnLocked(sessionId, assistant);
                startError = store->lastWriteError();
            }
        }
        emitter.emitSession(MaiEventType::SessionIdle, sessionId);
        if (next) {
            emitter.emitMessage(MaiEventType::MessageUpdated, sessionId, assistant.id);
            next->signalReady();
        } else if (startError)
            emitter.emitSession(MaiEventType::SessionError, sessionId, startError.message());
        turnFinished.notify_all();
        specialistWake.notify_one();
    }
};

struct MaiContextImportWorker::Impl {
    std::weak_ptr<MaiAgent::Runtime> runtime;
    explicit Impl(const std::shared_ptr<MaiAgent::Runtime>& value) : runtime(value) {}
};

MaiContextImportWorker::MaiContextImportWorker(std::shared_ptr<Impl> implementation)
    : mImplementation(std::move(implementation)) {}

MaiResult<MaiContextImportProgress> MaiContextImportWorker::beginContextImport(
    const MaiContextImportSpec& spec) const {
    const auto runtime = mImplementation ? mImplementation->runtime.lock() : nullptr;
    if (!runtime) return {MaiErrorCode::Canceled, "Context import owner is closed."};
    return runtime->beginContextImport(spec);
}

MaiResult<MaiContextImportProgress> MaiContextImportWorker::appendContextImport(
    const std::string& importId, std::uint64_t sequence,
    const std::vector<MaiContextImportRecord>& records) const {
    const auto runtime = mImplementation ? mImplementation->runtime.lock() : nullptr;
    if (!runtime) return {MaiErrorCode::Canceled, "Context import owner is closed."};
    return runtime->appendContextImport(importId, sequence, records);
}

MaiResult<MaiContextImportProgress> MaiContextImportWorker::commitContextImport(
    const std::string& importId) const {
    const auto runtime = mImplementation ? mImplementation->runtime.lock() : nullptr;
    if (!runtime) return {MaiErrorCode::Canceled, "Context import owner is closed."};
    return runtime->commitContextImport(importId);
}

MaiError MaiContextImportWorker::abortContextImport(const std::string& importId) const {
    const auto runtime = mImplementation ? mImplementation->runtime.lock() : nullptr;
    if (!runtime) return {MaiErrorCode::Canceled, "Context import owner is closed."};
    return runtime->abortContextImport(importId);
}

MaiResult<MaiContextImportPurgeResult> MaiContextImportWorker::purgeContextImports(
    const std::string& sessionId, std::size_t maxMessages) const {
    const auto runtime = mImplementation ? mImplementation->runtime.lock() : nullptr;
    if (!runtime) return {MaiErrorCode::Canceled, "Context import owner is closed."};
    return runtime->purgeContextImports(sessionId, maxMessages);
}

MaiContextImportWorker MaiAgent::contextImportWorker() const {
    return MaiContextImportWorker(std::make_shared<MaiContextImportWorker::Impl>(mRuntime));
}

MaiAgent::MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
                   std::unique_ptr<MaiToolRegistry> tools)
    : MaiAgent(std::move(store), std::move(model), std::move(tools), Options{}) {}

MaiAgent::MaiAgent(std::unique_ptr<MaiSessionStore> store, std::unique_ptr<MaiModelClient> model,
                   std::unique_ptr<MaiToolRegistry> tools, Options options)
    : mRuntime(std::make_shared<Runtime>()) {
    // 这个指针在构造函数里就设好：turn runner 每次都从 Runtime 现取依赖，
    // 设晚了会有一轮拿到空的。
    mRuntime->owner = this;
    mRuntime->store = std::move(store);
    // 直接在存储层恢复孤立工具状态，不把每个会话的完整历史都读入内存。
    mRuntime->store->recoverInterruptedTools(kInterruptedToolError, MaiTime::getCurrentTime());
    // 旧版误把查询错误写成生成失败；启动时恢复原云任务的只读轮询，不重新提交付费任务。
    mRuntime->store->recoverStatusCheckFailures();
    mRuntime->model = std::move(model);
    mRuntime->tools = std::move(tools);
    mRuntime->options = std::move(options);

    MaiPermissionGate::Options gateOptions;
    gateOptions.timeoutMs = mRuntime->options.permissionTimeoutMs;
    mRuntime->permissions = std::make_unique<MaiPermissionGate>(gateOptions);
    // 问答不设超时：超时等于替用户做了决定，而用户可能只是走开了。
    mRuntime->questions = std::make_unique<MaiQuestionGate>();
    mRuntime->specialistWorker = std::thread([this] {
        MaiThread::setCurrentName("mai-specialists");
        while (!mRuntime->stopSpecialists.load(std::memory_order_relaxed)) {
            try {
                pollSpecialistTasks();
            } catch (const std::exception&) {
                // 供应商回执不可信；格式异常时留到下次查询重试，不能让宿主进程退出。
            } catch (...) {
                // 第三方工具也可能抛出非标准异常，仍须隔离在本次查询内。
            }
            std::unique_lock<std::mutex> lock(mRuntime->specialistMutex);
            // 关闭可能发生在 poll 内：在同一等待锁下检查再入睡，不能丢掉关闭通知。
            // 普通任务通知仍能提前结束等待，不能改成只接受 stop 的谓词等待。
            if (!mRuntime->stopSpecialists.load(std::memory_order_relaxed))
                mRuntime->specialistWake.wait_for(lock, std::chrono::seconds(15));
        }
    });
}

MaiAgent::~MaiAgent() {
    mRuntime->revokeContextImportOwner();
    {
        std::lock_guard<std::mutex> lock(mRuntime->specialistMutex);
        mRuntime->stopSpecialists.store(true, std::memory_order_relaxed);
    }
    mRuntime->specialistWake.notify_all();
    if (mRuntime->specialistWorker.joinable()) mRuntime->specialistWorker.join();
    mRuntime->stopActiveTurns();
}

void MaiAgent::pollSpecialistTasks() {
    using Json = nlohmann::json;
    const MaiMillis now = MaiTime::getCurrentTime();
    const auto retryStatusCheck = [this](const MaiSpecialistTask& task, MaiMillis checkedAt) {
        // 查询异常无法证明云任务失败。保留任务为运行中，三次后降到每分钟重试一次。
        int& failures = mRuntime->specialistStatusFailures[task.id];
        if (failures < 3) ++failures;
        mRuntime->store->updateSpecialistProgress(task.id, task.ownerSessionId, checkedAt);
    };
    for (const MaiSpecialistTask& task : mRuntime->store->listActiveSpecialistTasks(16)) {
        if (mRuntime->stopSpecialists.load(std::memory_order_relaxed)) return;
        const auto priorFailures = mRuntime->specialistStatusFailures.find(task.id);
        const MaiMillis minimumInterval =
            priorFailures != mRuntime->specialistStatusFailures.end() && priorFailures->second >= 3
                ? 60'000
                : 10'000;
        if (task.lastCheckedAt != 0 && now - task.lastCheckedAt < minimumInterval) continue;
        MaiTool* tool = mRuntime->tools ? mRuntime->tools->find(task.specialistName) : nullptr;
        if (!tool) {
            retryStatusCheck(task, now);
            continue;
        }
        MaiSession session;
        if (!mRuntime->store->getSession(task.ownerSessionId, session)) continue;
        MaiToolContext context;
        context.sessionId = task.ownerSessionId;
        context.root = session.directory;
        context.fileAccessRoot = mRuntime->options.fileAccessRoot;
        context.allowOutsideWorkingDirectory = mRuntime->options.allowOutsideWorkingDirectory;
        context.specialistTasks = mRuntime->store.get();
        context.cancel = &mRuntime->stopSpecialists;
        const std::string arguments =
            Json{{"action", "continue"}, {"conversation_id", task.id}, {"poll_once", true}}.dump();
        const MaiToolResult result = [&] {
            try {
                return tool->execute(arguments, context);
            } catch (const std::exception&) {
                return MaiToolResult::failure(
                    MaiErrorCode::Internal,
                    Json{{"code", "internal"},
                         {"message", "The specialist status check threw an exception."}}
                        .dump());
            } catch (...) {
                return MaiToolResult::failure(
                    MaiErrorCode::Internal,
                    Json{{"code", "internal"},
                         {"message", "The specialist status check failed unexpectedly."}}
                        .dump());
            }
        }();
        if (mRuntime->stopSpecialists.load(std::memory_order_relaxed)) return;
        const MaiMillis checkedAt = MaiTime::getCurrentTime();
        if (result.hasError()) {
            const Json error = Json::parse(result.error().message(), nullptr, false);
            const std::string code = jsonText(error, "code");
            if (code == "task_failed") {
                mRuntime->specialistStatusFailures.erase(task.id);
                const std::string message = jsonText(error, "message");
                mRuntime->store->finishSpecialistTask(
                    task.id, task.ownerSessionId, MaiSpecialistTaskStatus::Failed,
                    message.empty() ? "The provider did not explain the task failure." : message,
                    {}, checkedAt);
            } else {
                retryStatusCheck(task, checkedAt);
            }
            continue;
        }
        const Json reply = Json::parse(result.output(), nullptr, false);
        if (!reply.is_object()) {
            retryStatusCheck(task, checkedAt);
            continue;
        }
        const std::string status = jsonText(reply, "status");
        if (status == "succeeded" || status == "SUCCEEDED") {
            const std::string path = jsonText(reply, "path");
            std::uint64_t bytes = 0;
            if (path.empty() || !MaiFileSystem::fileSize(MaiFilePath::fromUtf8(path), bytes) ||
                bytes == 0) {
                retryStatusCheck(task, checkedAt);
                continue;
            }
            mRuntime->specialistStatusFailures.erase(task.id);
            mRuntime->store->finishSpecialistTask(task.id, task.ownerSessionId,
                                                  MaiSpecialistTaskStatus::Succeeded,
                                                  jsonText(reply, "reply"), path, checkedAt);
        } else if (status == "cancelled" || status == "CANCELED") {
            mRuntime->specialistStatusFailures.erase(task.id);
            mRuntime->store->finishSpecialistTask(
                task.id, task.ownerSessionId, MaiSpecialistTaskStatus::Canceled,
                "The cloud video task was canceled.", {}, checkedAt);
        } else if (status == "failed" || status == "FAILED") {
            mRuntime->specialistStatusFailures.erase(task.id);
            const std::string message = jsonText(reply, "reply");
            mRuntime->store->finishSpecialistTask(
                task.id, task.ownerSessionId, MaiSpecialistTaskStatus::Failed,
                message.empty() ? "The provider did not explain the task failure." : message, {},
                checkedAt);
        } else if (status == "submitted" || status == "queued" || status == "running" ||
                   status == "PENDING" || status == "RUNNING" || status == "PROCESSING") {
            mRuntime->specialistStatusFailures.erase(task.id);
            mRuntime->store->updateSpecialistProgress(task.id, task.ownerSessionId, checkedAt);
        } else {
            retryStatusCheck(task, checkedAt);
        }
    }
    for (const MaiSpecialistTask& task : mRuntime->store->listUnnotifiedSpecialistTasks(16)) {
        if (mRuntime->stopSpecialists.load(std::memory_order_relaxed)) return;
        if (task.notificationAttemptAt != 0 && now - task.notificationAttemptAt < 30'000) continue;
        forwardSpecialistReply(task);
    }
}

void MaiAgent::forwardSpecialistReply(const MaiSpecialistTask& task) {
    // 用户已经手动投递过同一云 task_id 时，恢复旧任务不能再次生成媒体卡片。
    if (task.status == MaiSpecialistTaskStatus::Succeeded &&
        hasDeliveredMediaForProviderTask(*mRuntime->store, task)) {
        mRuntime->store->markSpecialistResultAlreadyDelivered(task.id, task.ownerSessionId,
                                                              MaiTime::getCurrentTime());
        return;
    }
    if (!mRuntime->model && task.status != MaiSpecialistTaskStatus::Failed &&
        task.status != MaiSpecialistTaskStatus::Canceled)
        return;
    auto turn = std::make_shared<ActiveTurn>();
    {
        std::lock_guard<std::mutex> lock(mRuntime->mutex);
        // 后台通知同样必须尊重普通发送/上下文发布的启动预占，不能绕过 Session 空闲边界。
        if (mRuntime->stopping || mRuntime->active.count(task.ownerSessionId) ||
            mRuntime->startingSessions.count(task.ownerSessionId))
            return;
        mRuntime->active[task.ownerSessionId] = turn;
    }
    const auto reservation = mRuntime->store->reserveSpecialistNotification(
        task.id, task.ownerSessionId, MaiIdGenerator::newMessageId(), MaiTime::getCurrentTime());
    if (!reservation) {
        mRuntime->retire(task.ownerSessionId, turn);
        return;
    }
    const std::string messageId = reservation.value();
    const auto existing =
        mRuntime->store->listMessagesPage(task.ownerSessionId, messageId + "~", 1);
    MaiMessage assistant = task.status == MaiSpecialistTaskStatus::Failed && !existing.empty() &&
                                   existing.front().id == messageId
                               ? existing.front()
                               : MaiMessage{};
    assistant.id = messageId;
    assistant.role = MaiRole::Assistant;
    if (assistant.created == 0) assistant.created = MaiTime::getCurrentTime();
    assistant.completed = 0;
    if (task.status == MaiSpecialistTaskStatus::Failed && !assistant.parts.empty()) {
        const auto* text = std::get_if<MaiTextPart>(&assistant.parts.front().body);
        const bool statusNotice =
            text != nullptr && (text->text.rfind("Image task failed. ", 0) == 0 ||
                                text->text.rfind("Video task failed. ", 0) == 0 ||
                                text->text.rfind("Image task status unavailable. ", 0) == 0 ||
                                text->text.rfind("Video task status unavailable. ", 0) == 0 ||
                                text->text.rfind("Image output unavailable. ", 0) == 0 ||
                                text->text.rfind("Video output unavailable. ", 0) == 0);
        if (statusNotice)
            assistant.parts.resize(1);
        else
            assistant.parts.clear();
    }
    if ((task.status == MaiSpecialistTaskStatus::Failed ||
         task.status == MaiSpecialistTaskStatus::Canceled) &&
        assistant.parts.empty() && !mRuntime->model) {
        MaiMessagePart notice;
        notice.id = MaiIdGenerator::newPartId();
        notice.created = MaiTime::getCurrentTime();
        notice.body = MaiTextPart{task.status == MaiSpecialistTaskStatus::Canceled
                                      ? "The video task was canceled."
                                      : specialistFailureNotice(task)};
        assistant.parts.push_back(std::move(notice));
    }
    mRuntime->store->putMessage(task.ownerSessionId, assistant);
    if (mRuntime->store->lastWriteError()) {
        mRuntime->retire(task.ownerSessionId, turn);
        return;
    }
    mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, task.ownerSessionId, assistant.id);
    if (!mRuntime->model) {
        mRuntime->store->markSpecialistNotified(task.id, task.ownerSessionId, messageId,
                                                MaiTime::getCurrentTime());
        mRuntime->retire(task.ownerSessionId, turn);
        return;
    }
    using Json = nlohmann::json;
    std::uint64_t outputBytes = 0;
    const bool outputAvailable =
        !task.outputPath.empty() &&
        MaiFileSystem::fileSize(MaiFilePath::fromUtf8(task.outputPath), outputBytes) &&
        outputBytes > 0;
    const std::string reply = Json{
        {"specialist", task.specialistName},
        {"task_id", task.id},
        {"status", specialistStatusName(task.status)},
        {"original_intent", task.intent},
        {"input_reference", task.inputReference},
        {"reply", task.finalText},
        {"error", task.errorText},
        {"output_path", task.outputPath},
        {"output_available",
         outputAvailable}}.dump();
    {
        std::lock_guard<std::mutex> lock(turn->steeringMutex);
        turn->id = assistant.id;
        turn->acceptingSteering = true;
    }
    auto dependencies = mRuntime->dependencies(task.ownerSessionId, turn);
    dependencies.specialistReply = reply;
    try {
        turn->worker = std::thread([this, task, turn, dependencies, assistant, messageId] {
            turn->waitReady();
            MaiThread::setCurrentName("mai-child-reply");
            MaiTurnRunner runner(dependencies, task.ownerSessionId, assistant);
            runner.run(turn->cancel);
            if (runner.succeeded() && !turn->cancel.load(std::memory_order_relaxed)) {
                const auto messages =
                    mRuntime->store->listMessagesPage(task.ownerSessionId, messageId + "~", 1);
                if (!messages.empty() && messages.front().id == messageId &&
                    runner.hasVisibleText())
                    mRuntime->store->markSpecialistNotified(task.id, task.ownerSessionId, messageId,
                                                            MaiTime::getCurrentTime());
            } else if (!turn->cancel.load(std::memory_order_relaxed) &&
                       task.status == MaiSpecialistTaskStatus::Failed) {
                const auto messages =
                    mRuntime->store->listMessagesPage(task.ownerSessionId, messageId + "~", 1);
                if (!messages.empty() && messages.front().id == messageId) {
                    MaiMessage fallback = messages.front();
                    MaiMessagePart notice;
                    notice.id = MaiIdGenerator::newPartId();
                    notice.created = MaiTime::getCurrentTime();
                    notice.body = MaiTextPart{specialistFailureNotice(task)};
                    fallback.parts.push_back(std::move(notice));
                    fallback.completed = MaiTime::getCurrentTime();
                    mRuntime->store->putMessage(task.ownerSessionId, fallback);
                    if (!mRuntime->store->lastWriteError()) {
                        mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated,
                                                      task.ownerSessionId, fallback.id);
                        mRuntime->store->markSpecialistNotified(
                            task.id, task.ownerSessionId, messageId, MaiTime::getCurrentTime());
                    }
                }
            }
            mRuntime->retire(task.ownerSessionId, turn);
            mRuntime->markWorkerFinished();
        });
        mRuntime->markWorkerStarted();
        turn->signalReady();
    } catch (...) {
        mRuntime->retire(task.ownerSessionId, turn);
    }
}

std::vector<MaiSession> MaiAgent::listSessions() const {
    // **只给根会话。** 子 Agent 也是会话，但用户没开过它们，
    // 列出来只会让人以为自己漏了什么。要看子 Agent 走 listSubAgents()。
    std::vector<MaiSession> all = mRuntime->store->listSessions();
    std::vector<MaiSession> roots;
    roots.reserve(all.size());
    for (MaiSession& session : all) {
        if (session.isRoot()) roots.push_back(std::move(session));
    }
    return roots;
}

// ── 子 Agent ────────────────────────────────────────────────────

namespace {

// 这个会话直接起过的孩子。
std::vector<MaiSession> childrenOf(const MaiSessionStore& store, const std::string& parentId) {
    std::vector<MaiSession> children;
    for (MaiSession& session : store.listSessions()) {
        if (session.parentId == parentId) children.push_back(std::move(session));
    }
    return children;
}

}  // namespace

MaiResult<std::string> MaiAgent::spawnSubAgent(const std::string& parentSessionId,
                                               const std::string& taskName,
                                               const std::string& prompt) {
    if (prompt.empty()) return {MaiErrorCode::InvalidInput, "the sub-agent needs something to do"};

    MaiSession parent;
    if (!mRuntime->store->getSession(parentSessionId, parent))
        return {MaiErrorCode::NotFound, "session not found"};

    // 深度和数量两条上限，**报错要分得开**：一个是「别再往下分了」，
    // 一个是「先收掉几个」，模型的应对完全不一样。
    if (parent.depth + 1 > mRuntime->options.maxSubAgentDepth) {
        return {MaiErrorCode::InvalidInput,
                "sub-agents are already nested " + std::to_string(parent.depth) +
                    " deep, which is the limit. Do this part of the work yourself."};
    }
    int open = 0;
    for (const MaiSession& child : childrenOf(*mRuntime->store, parentSessionId)) {
        if (mRuntime->closedSubAgents.count(child.id) == 0) ++open;
    }
    if (open >= mRuntime->options.maxOpenSubAgents) {
        return {MaiErrorCode::InvalidInput,
                "there are already " + std::to_string(open) +
                    " sub-agents open, which is the limit. Close the ones you are done with."};
    }

    MaiSession child;
    child.id = MaiIdGenerator::newSessionId();
    child.title = taskName.empty() ? std::string(kMaiDefaultSessionTitle) : taskName;
    // **工作目录继承父的，没有参数能改。** 子能看见的文件因此是父的子集——
    // 这是「子只能比父弱」落到实处的第一条。
    child.directory = parent.directory;
    child.model = parent.model;
    child.reasoningEffort = parent.reasoningEffort;
    child.agent = parent.agent;
    child.parentId = parentSessionId;
    child.depth = parent.depth + 1;
    child.created = MaiTime::getCurrentTime();
    child.updated = child.created;
    mRuntime->store->putSession(child);

    // 注意这里**没有**把父的会话级授权带过去。会话豁免是按 sessionId 记的，
    // 子是新 id，天然不继承——父点过「以后都允许跑 rm」，子照样要重新问。
    // 这条不是刻意写的代码，是数据结构自带的，所以写在这儿免得以后被"顺手"改掉。

    MaiResult<std::string> started = submit(MaiSendPrompt{child.id, prompt});
    if (!started) return started.error();
    return child.id;
}

MaiError MaiAgent::sendToSubAgent(const std::string& parentSessionId,
                                  const std::string& childSessionId, const std::string& prompt) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child))
        return MaiError(MaiErrorCode::NotFound, "no such sub-agent");
    // **只能戳自己的孩子。** 不查这一条的话，一个子 Agent 就能拿别人的会话 id
    // 去给别人派活，那是横向越权。
    if (child.parentId != parentSessionId)
        return MaiError(MaiErrorCode::InvalidInput, "that sub-agent belongs to someone else");

    MaiResult<std::string> sent = submit(MaiSendPrompt{childSessionId, prompt});
    return sent ? MaiError() : sent.error();
}

bool MaiAgent::waitForSubAgent(const std::string& parentSessionId,
                               const std::string& childSessionId, MaiMillis timeoutMs,
                               const std::atomic<bool>& cancel) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child)) return false;
    if (child.parentId != parentSessionId) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(mRuntime->mutex);
    while (mRuntime->active.count(childSessionId) > 0) {
        // 中断时没人会 notify 这个 condvar，所以不能一睡到底，
        // 得醒过来自己查一眼 cancel。和两个闸门一个路子。
        if (cancel.load(std::memory_order_relaxed)) return false;
        if (timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) return false;
        mRuntime->turnFinished.wait_for(lock, std::chrono::milliseconds(100));
    }
    return true;
}

std::vector<MaiSubAgentInfo> MaiAgent::listSubAgents(const std::string& parentSessionId) {
    std::vector<MaiSubAgentInfo> out;
    for (const MaiSession& child : childrenOf(*mRuntime->store, parentSessionId)) {
        MaiSubAgentInfo info;
        info.sessionId = child.id;
        info.taskName = child.title;
        info.depth = child.depth;
        if (mRuntime->closedSubAgents.count(child.id) > 0) {
            info.status = "closed";
        } else {
            info.status = isBusy(child.id) ? "running" : "idle";
        }
        out.push_back(std::move(info));
    }
    return out;
}

MaiError MaiAgent::closeSubAgent(const std::string& parentSessionId,
                                 const std::string& childSessionId) {
    MaiSession child;
    if (!mRuntime->store->getSession(childSessionId, child))
        return MaiError(MaiErrorCode::NotFound, "no such sub-agent");
    if (child.parentId != parentSessionId)
        return MaiError(MaiErrorCode::InvalidInput, "that sub-agent belongs to someone else");

    submit(MaiInterrupt{childSessionId});
    {
        std::lock_guard<std::mutex> lock(mRuntime->mutex);
        mRuntime->closedSubAgents.insert(childSessionId);
    }
    // **消息不删。** 父之后还要能翻它说过什么，而且用户排障时那段历史是唯一的线索。
    // 收掉只是把名额腾出来。
    return MaiError();
}

std::string MaiAgent::subAgentReport(const std::string& childSessionId) {
    const std::vector<MaiMessage> messages = mRuntime->store->listMessages(childSessionId);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != MaiRole::Assistant) continue;
        const std::string text = it->text();
        if (!text.empty()) return text;
    }
    return std::string();
}

bool MaiAgent::getSession(const std::string& id, MaiSession& out) const {
    return mRuntime->store->getSession(id, out);
}

std::vector<MaiMessage> MaiAgent::listMessages(const std::string& sessionId) const {
    return mRuntime->store->listMessages(sessionId);
}

MaiResult<MaiContextImportProgress> MaiAgent::beginContextImport(const MaiContextImportSpec& spec) {
    return contextImportWorker().beginContextImport(spec);
}

MaiResult<MaiContextImportProgress> MaiAgent::appendContextImport(
    const std::string& importId, std::uint64_t sequence,
    const std::vector<MaiContextImportRecord>& records) {
    return contextImportWorker().appendContextImport(importId, sequence, records);
}

MaiResult<MaiContextImportProgress> MaiAgent::commitContextImport(const std::string& importId) {
    return contextImportWorker().commitContextImport(importId);
}

MaiError MaiAgent::abortContextImport(const std::string& importId) {
    return contextImportWorker().abortContextImport(importId);
}

MaiResult<MaiContextImportPurgeResult> MaiAgent::purgeContextImports(const std::string& sessionId,
                                                                     std::size_t maxMessages) {
    return contextImportWorker().purgeContextImports(sessionId, maxMessages);
}

std::vector<MaiMessage> MaiAgent::listMessagesPage(const std::string& sessionId,
                                                   const std::string& beforeId,
                                                   std::size_t limit) const {
    return mRuntime->store->listMessagesPage(sessionId, beforeId, limit);
}

std::vector<MaiPermissionRequest> MaiAgent::listPendingPermissions() const {
    return mRuntime->permissions->listPending();
}

bool MaiAgent::getMessage(const std::string& sessionId, const std::string& messageId,
                          MaiMessage& out) const {
    return mRuntime->store->getMessage(sessionId, messageId, out);
}

MaiResult<MaiConversationHistoryPage> MaiAgent::readHistory(
    const std::string& sessionId, const MaiConversationHistoryQuery& query) const {
    return maiReadConversationHistory(*mRuntime->store, sessionId, query);
}

MaiResult<MaiToolDetailPage> MaiAgent::readToolDetail(const std::string& sessionId,
                                                      const MaiToolDetailQuery& query) const {
    return maiReadToolDetail(*mRuntime->store, sessionId, query);
}

MaiResult<MaiToolOutputPage> MaiAgent::readToolOutput(const std::string& sessionId,
                                                      const MaiToolOutputQuery& query) const {
    return mRuntime->store->readToolOutput(sessionId, query);
}

std::vector<MaiQuestionRequest> MaiAgent::listPendingQuestions() const {
    return mRuntime->questions->listPending();
}

bool MaiAgent::isBusy(const std::string& sessionId) const {
    std::lock_guard<std::mutex> lock(mRuntime->mutex);
    return mRuntime->active.count(sessionId) > 0;
}

MaiResult<std::string> MaiAgent::interruptTurn(const std::string& sessionId,
                                               const std::string& expectedTurnId) {
    std::lock_guard<std::mutex> runtimeLock(mRuntime->mutex);
    const auto found = mRuntime->active.find(sessionId);
    if (found == mRuntime->active.end())
        return {MaiErrorCode::NotFound, "no turn is running for this session"};
    if (!expectedTurnId.empty()) {
        // 专用模型回复先在 Runtime 预占空轮次，再持 steeringMutex 初始化 id；
        // 因而 Runtime 锁单独不能保护此字段。沿 activeTurnId 的 Runtime → steering 锁序读取，
        // 初始化完成后 id 才保持不变。旧无目标 Stop 不读取 id，仍能取消预占中的轮次。
        std::lock_guard<std::mutex> steeringLock(found->second->steeringMutex);
        if (found->second->id != expectedTurnId)
            return {MaiErrorCode::NotFound, "the original turn is no longer active"};
    }
    // Stop also pauses queued inputs; a stale turn-targeted Stop must not pause a later task.
    mRuntime->pausedInputs.insert(sessionId);
    found->second->cancel.store(true, std::memory_order_relaxed);
    // Keep gate wakeups inside the same Runtime critical section. Their own
    // cancelSession methods only settle entries and notify; they invoke no user
    // callback. A replacement turn cannot enter until both old-turn gates wake.
    mRuntime->permissions->cancelSession(sessionId);
    mRuntime->questions->cancelSession(sessionId);
    return sessionId;
}

std::string MaiAgent::activeTurnId(const std::string& sessionId) const {
    std::lock_guard<std::mutex> runtimeLock(mRuntime->mutex);
    const auto found = mRuntime->active.find(sessionId);
    if (found == mRuntime->active.end()) return {};
    std::lock_guard<std::mutex> steeringLock(found->second->steeringMutex);
    return found->second->id;
}

bool MaiAgent::setApprovalPolicy(MaiApprovalPolicy policy) {
    std::lock_guard<std::mutex> lock(mRuntime->mutex);
    if (!mRuntime->active.empty()) return false;
    mRuntime->options.approvalPolicy = policy;
    return true;
}

MaiApprovalPolicy MaiAgent::approvalPolicy() const {
    std::lock_guard<std::mutex> lock(mRuntime->mutex);
    return mRuntime->options.approvalPolicy;
}

void MaiAgent::waitIdle() {
    std::unique_lock<std::mutex> lock(mRuntime->mutex);
    mRuntime->turnFinished.wait(lock, [this] { return mRuntime->active.empty(); });
}

MaiEventBus& MaiAgent::eventBus() {
    return mRuntime->bus;
}
const MaiEventBus& MaiAgent::eventBus() const {
    return mRuntime->bus;
}

MaiResult<std::string> MaiAgent::submit(const MaiOperation& operation) {
    return std::visit(
        [this](const auto& operation) -> MaiResult<std::string> {
            using T = std::decay_t<decltype(operation)>;

            if constexpr (std::is_same_v<T, MaiCreateSession>) {
                if (!maiIsValidReasoningEffort(operation.reasoningEffort))
                    return {MaiErrorCode::InvalidInput, "Unsupported reasoning effort."};
                if (!operation.model.empty() && mRuntime->model &&
                    !mRuntime->model->wireApiForModel(operation.model))
                    return {MaiErrorCode::InvalidInput, "The selected model is not configured."};
                MaiSession session;
                session.id = MaiIdGenerator::newSessionId();
                session.directory = operation.directory;
                session.title = operation.title.empty() ? kMaiDefaultSessionTitle : operation.title;
                session.model = operation.model;
                session.reasoningEffort = operation.reasoningEffort;
                session.created = MaiTime::getCurrentTime();
                session.updated = session.created;
                mRuntime->store->putSession(session);
                mRuntime->emitter.emitSession(MaiEventType::SessionCreated, session.id,
                                              session.title);
                return session.id;

            } else if constexpr (std::is_same_v<T, MaiUpdateSession>) {
                // 空标题仍表示“不改”；显式改名使用与工具相同的跨设备标题契约。
                if (!operation.title.empty()) {
                    const auto valid = maiValidateSessionTitle(operation.title);
                    if (valid) return valid;
                }
                // 所有策略先校验再进入同一次 mutation，非法 effort 不能顺带改掉标题或模型。
                if (operation.reasoningEffort &&
                    !maiIsValidReasoningEffort(*operation.reasoningEffort))
                    return {MaiErrorCode::InvalidInput, "Unsupported reasoning effort."};
                if (!operation.model.empty() && mRuntime->model &&
                    !mRuntime->model->wireApiForModel(operation.model))
                    return {MaiErrorCode::InvalidInput, "The selected model is not configured."};
                std::string title;
                bool changed = false;
                std::unique_lock<std::mutex> runtimeLock(mRuntime->mutex);
                if ((!operation.model.empty() || operation.reasoningEffort.has_value()) &&
                    (mRuntime->active.count(operation.sessionId) ||
                     mRuntime->startingSessions.count(operation.sessionId)))
                    return {MaiErrorCode::Busy,
                            "Stop the target session before changing its model or effort."};
                const bool found =
                    mRuntime->store->mutateSession(operation.sessionId, [&](MaiSession& session) {
                        // 标题与显式命名标记在同一次存储 mutation 中写入；Titler 的锁内复查
                        // 因此不会在当前轮收尾时把有效的占位同名标题覆盖掉。
                        if (!operation.title.empty() &&
                            (session.title != operation.title || !session.titleEdited)) {
                            session.title = operation.title;
                            session.titleEdited = true;
                            changed = true;
                        }
                        if (!operation.model.empty() && session.model != operation.model) {
                            session.model = operation.model;
                            changed = true;
                        }
                        if (operation.reasoningEffort &&
                            session.reasoningEffort != *operation.reasoningEffort) {
                            session.reasoningEffort = *operation.reasoningEffort;
                            changed = true;
                        }
                        if (!operation.agent.empty() && session.agent != operation.agent) {
                            session.agent = operation.agent;
                            changed = true;
                        }
                        if (!operation.directory.empty() &&
                            session.directory != operation.directory) {
                            session.directory = operation.directory;
                            changed = true;
                        }
                        if (changed) session.updated = MaiTime::getCurrentTime();
                        title = session.title;
                    });
                if (!found) return {MaiErrorCode::NotFound, "session not found"};
                const auto persisted = mRuntime->store->lastWriteError();
                if (persisted) return persisted;
                if (!changed) return operation.sessionId;
                runtimeLock.unlock();
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, operation.sessionId,
                                              title);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiDeleteSession>) {
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->active.count(operation.sessionId) ||
                        mRuntime->startingSessions.count(operation.sessionId))
                        return {MaiErrorCode::Busy,
                                "a turn is still running; interrupt it before deleting"};
                    if (!mRuntime->store->removeSession(operation.sessionId))
                        return {MaiErrorCode::NotFound, "session not found"};
                    mRuntime->inputQueues.erase(operation.sessionId);
                    mRuntime->loadedInputQueues.erase(operation.sessionId);
                    mRuntime->pausedInputs.erase(operation.sessionId);
                }
                // 会话没了，它还在等的授权就没意义了；"本会话都允许"也要一起清掉，
                // 否则以后建一个同 id 的会话会白捡上一个的授权。
                mRuntime->permissions->cancelSession(operation.sessionId);
                mRuntime->permissions->forgetSession(operation.sessionId);
                mRuntime->emitter.emitSession(MaiEventType::SessionDeleted, operation.sessionId);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiClearMessages>) {
                std::string title;
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->active.count(operation.sessionId) ||
                        mRuntime->startingSessions.count(operation.sessionId))
                        return {MaiErrorCode::Busy,
                                "a turn is still running; interrupt it before clearing"};
                    if (!mRuntime->store->clearMessages(operation.sessionId))
                        return {MaiErrorCode::NotFound, "session not found"};
                    // 清历史也撤销旧输入队列，不能在下一次发送时把已清掉的消息写回。
                    mRuntime->inputQueues.erase(operation.sessionId);
                    mRuntime->loadedInputQueues.erase(operation.sessionId);
                    mRuntime->pausedInputs.erase(operation.sessionId);

                    // 标题是拿第一句话起的。记录清了标题还留着的话，头部会一直挂着一句
                    // 已经不存在的对话——所以一起清掉，让下一轮重新起名。
                    mRuntime->store->mutateSession(operation.sessionId, [](MaiSession& session) {
                        session.title.clear();
                        session.titleEdited = false;
                        session.updated = MaiTime::getCurrentTime();
                    });
                }

                // **不逐条发 MessageRemoved。** 历史可能有几百条，那是几百条事件，
                // 而界面要做的只有一件事：把列表清空。发一条会话级的更新，
                // 界面照常去 listMessages 拉全量（这次拉到的是空的）。
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, operation.sessionId,
                                              title);
                return operation.sessionId;

            } else if constexpr (std::is_same_v<T, MaiSteerPrompt>) {
                if (operation.requestId.empty() || operation.requestId.size() > 128 ||
                    !std::all_of(operation.requestId.begin(), operation.requestId.end(),
                                 [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; }))
                    return {MaiErrorCode::InvalidInput, "requestId must be 1..128 ASCII bytes"};
                if (operation.turnId.empty() || operation.turnId.size() > 128)
                    return {MaiErrorCode::InvalidInput, "turnId is required"};
                const bool hasText = std::any_of(
                    operation.text.begin(), operation.text.end(), [](unsigned char byte) {
                        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' &&
                               byte != '\f' && byte != '\v';
                    });
                if (!hasText && operation.images.empty() && operation.videos.empty())
                    return {MaiErrorCode::InvalidInput, "text or media is required"};
                if (operation.text.size() > 64 * 1024 || operation.images.size() > 10 ||
                    operation.videos.size() > 10)
                    return {MaiErrorCode::InvalidInput, "steering input exceeds its size limit"};
                const auto validMedia = [](const auto& media) {
                    return !media.path.empty() && media.path.size() <= 8192 &&
                           !media.mimeType.empty() && media.mimeType.size() <= 256;
                };
                if (!std::all_of(operation.images.begin(), operation.images.end(), validMedia) ||
                    !std::all_of(operation.videos.begin(), operation.videos.end(), validMedia))
                    return {MaiErrorCode::InvalidInput, "invalid steering media reference"};
                MaiSession session;
                if (!mRuntime->store->getSession(operation.sessionId, session))
                    return {MaiErrorCode::NotFound, "session not found"};
                MaiMessage quoted;
                if (!operation.quotedMessageId.empty()) {
                    if (operation.quotedMessageId.rfind("msg_", 0) != 0 ||
                        operation.quotedMessageId.size() > 128)
                        return {MaiErrorCode::InvalidInput, "invalid quoted message ID"};
                    const auto page = mRuntime->store->listMessagesPage(
                        operation.sessionId, operation.quotedMessageId + "~", 1);
                    if (page.empty() || page.front().id != operation.quotedMessageId)
                        return {MaiErrorCode::NotFound,
                                "quoted message was not found in this conversation"};
                    quoted = page.front();
                }
                const std::string fingerprint = steeringFingerprint(operation);
                if (fingerprint.size() > 256 * 1024)
                    return {MaiErrorCode::InvalidInput, "steering input exceeds its size limit"};
                MaiMessage user;
                {
                    // 固定锁序 Runtime → turn。工作线程只取 turn 锁，广播永远在锁外，
                    // 订阅者因此仍能重入查询和 submit，不会被同步事件总线锁死。
                    std::lock_guard<std::mutex> runtimeLock(mRuntime->mutex);
                    const auto matches = [&](const SteeringReceipt& receipt) {
                        return receipt.sessionId == operation.sessionId &&
                               receipt.turnId == operation.turnId &&
                               receipt.requestId == operation.requestId;
                    };
                    const auto recent = std::find_if(mRuntime->recentSteering.begin(),
                                                     mRuntime->recentSteering.end(), matches);
                    if (recent != mRuntime->recentSteering.end()) {
                        if (recent->fingerprint != fingerprint)
                            return {MaiErrorCode::InvalidInput,
                                    "requestId was already used for different steering input"};
                        return recent->messageId;
                    }
                    const auto found = mRuntime->active.find(operation.sessionId);
                    if (found == mRuntime->active.end())
                        return {MaiErrorCode::NotFound, "no turn is running for this session"};
                    ActiveTurn& turn = *found->second;
                    std::lock_guard<std::mutex> steeringLock(turn.steeringMutex);
                    const auto accepted = std::find_if(turn.steeringReceipts.begin(),
                                                       turn.steeringReceipts.end(), matches);
                    if (accepted != turn.steeringReceipts.end()) {
                        if (accepted->fingerprint != fingerprint)
                            return {MaiErrorCode::InvalidInput,
                                    "requestId was already used for different steering input"};
                        return accepted->messageId;
                    }
                    if (turn.id != operation.turnId)
                        return {MaiErrorCode::InvalidInput, "the target turn is no longer active"};
                    if (turn.cancel.load(std::memory_order_relaxed))
                        return {MaiErrorCode::Canceled, "the target turn is stopping"};
                    if (!turn.acceptingSteering)
                        return {MaiErrorCode::Busy, "the target turn is finishing"};
                    if (turn.steeringReceipts.size() >= 64)
                        return {MaiErrorCode::Busy, "the target turn reached its steering limit"};
                    user.id = MaiIdGenerator::newMessageId();
                    user.role = MaiRole::User;
                    user.created = MaiTime::getCurrentTime();
                    user.completed = user.created;
                    const auto addPart = [&](MaiMessagePartBody body) {
                        MaiMessagePart part;
                        part.id = MaiIdGenerator::newPartId();
                        part.created = user.created;
                        part.body = std::move(body);
                        user.parts.push_back(std::move(part));
                    };
                    if (!operation.quotedMessageId.empty())
                        addPart(MaiQuotePart{operation.quotedMessageId, quotedPreview(quoted)});
                    if (!operation.text.empty()) addPart(MaiTextPart{operation.text});
                    for (const auto& image : operation.images)
                        addPart(MaiImagePart{image.path, image.mimeType});
                    for (const auto& video : operation.videos) addPart(video);
                    turn.pendingSteering.push_back(user.id);
                    mRuntime->store->putMessage(operation.sessionId, user);
                    mRuntime->store->mutateSession(operation.sessionId, [](MaiSession& value) {
                        value.updated = MaiTime::getCurrentTime();
                    });
                    if (const MaiError error = mRuntime->store->lastWriteError()) {
                        turn.acceptingSteering = false;
                        return error;
                    }
                    SteeringReceipt receipt{operation.sessionId, operation.turnId,
                                            operation.requestId, fingerprint, user.id};
                    turn.steeringReceipts.push_back(receipt);
                    mRuntime->recentSteering.push_back(std::move(receipt));
                    if (mRuntime->recentSteering.size() > 128) mRuntime->recentSteering.pop_front();
                }
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, operation.sessionId,
                                              user.id);
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, operation.sessionId);
                return user.id;

            } else if constexpr (std::is_same_v<T, MaiEnqueuePrompt>) {
                const auto& prompt = operation.prompt;
                if (operation.requestId.empty() || operation.requestId.size() > 128 ||
                    !std::all_of(operation.requestId.begin(), operation.requestId.end(),
                                 [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }))
                    return {MaiErrorCode::InvalidInput, "requestId must be 1..128 ASCII bytes"};
                const bool hasText = std::any_of(prompt.text.begin(), prompt.text.end(),
                                                 [](unsigned char c) { return !std::isspace(c); });
                if ((!hasText && prompt.images.empty() && prompt.videos.empty()) ||
                    prompt.text.size() > 64 * 1024 || prompt.images.size() > 10 ||
                    prompt.videos.size() > 10)
                    return {MaiErrorCode::InvalidInput,
                            "text or media is required within input limits"};
                const auto validMedia = [](const auto& item) {
                    return !item.path.empty() && item.path.size() <= 8192 &&
                           !item.mimeType.empty() && item.mimeType.size() <= 256;
                };
                if (!std::all_of(prompt.images.begin(), prompt.images.end(), validMedia) ||
                    !std::all_of(prompt.videos.begin(), prompt.videos.end(), validMedia))
                    return {MaiErrorCode::InvalidInput, "invalid input media reference"};
                MaiSession session;
                if (!mRuntime->store->getSession(prompt.sessionId, session))
                    return {MaiErrorCode::NotFound, "session not found"};
                MaiMessage quoted;
                if (!prompt.quotedMessageId.empty()) {
                    if (prompt.quotedMessageId.rfind("msg_", 0) != 0 ||
                        prompt.quotedMessageId.size() > 128)
                        return {MaiErrorCode::InvalidInput, "invalid quoted message ID"};
                    const auto page = mRuntime->store->listMessagesPage(
                        prompt.sessionId, prompt.quotedMessageId + "~", 1);
                    if (page.empty() || page.front().id != prompt.quotedMessageId)
                        return {MaiErrorCode::NotFound,
                                "quoted message was not found in this conversation"};
                    quoted = page.front();
                }
                MaiMessage user;
                user.role = MaiRole::User;
                user.inputRequestId = operation.requestId;
                user.pendingInput = true;
                user.created = user.completed = MaiTime::getCurrentTime();
                const auto add = [&](MaiMessagePartBody body) {
                    user.parts.push_back(
                        {MaiIdGenerator::newPartId(), std::move(body), user.created});
                };
                if (!prompt.quotedMessageId.empty())
                    add(MaiQuotePart{prompt.quotedMessageId, quotedPreview(quoted)});
                if (!prompt.text.empty()) add(MaiTextPart{prompt.text});
                for (const auto& image : prompt.images)
                    add(MaiImagePart{image.path, image.mimeType});
                for (const auto& video : prompt.videos) add(video);
                std::shared_ptr<ActiveTurn> started;
                MaiMessage assistant;
                MaiError startError;
                bool duplicate = false;
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->stopping)
                        return {MaiErrorCode::Canceled, "the Agent is stopping"};
                    if (mRuntime->contextCommittingSessions.count(prompt.sessionId))
                        return {MaiErrorCode::Busy, "External context is being published."};
                    if (!mRuntime->store->getSession(prompt.sessionId, session))
                        return {MaiErrorCode::NotFound, "session not found"};
                    MaiMessage previous;
                    if (mRuntime->store->getInputMessage(prompt.sessionId, operation.requestId,
                                                         previous)) {
                        if (inputFingerprint(previous) != inputFingerprint(user))
                            return {MaiErrorCode::InvalidInput,
                                    "requestId was already used for different input"};
                        if (!previous.pendingInput) return previous.id;
                        user = std::move(previous);
                        duplicate = true;
                    }
                    if (const auto error = mRuntime->store->lastWriteError()) return error;
                    auto& queue = mRuntime->inputQueues[prompt.sessionId];
                    if (!mRuntime->loadedInputQueues.count(prompt.sessionId)) {
                        const auto pending = mRuntime->store->listQueuedInputs(prompt.sessionId);
                        if (const auto error = mRuntime->store->lastWriteError()) return error;
                        queue.assign(pending.begin(), pending.end());
                        mRuntime->loadedInputQueues.insert(prompt.sessionId);
                    }
                    if (!duplicate) {
                        if (queue.size() >= 256)
                            return {MaiErrorCode::Busy, "the session input inbox is full"};
                        user.id = MaiIdGenerator::newMessageId();
                        mRuntime->store->putMessage(prompt.sessionId, user);
                        if (const auto error = mRuntime->store->lastWriteError()) return error;
                        queue.push_back(user);
                    }
                    mRuntime->pausedInputs.erase(prompt.sessionId);
                    const auto active = mRuntime->active.find(prompt.sessionId);
                    if (active != mRuntime->active.end())
                        mRuntime->offerInputsLocked(prompt.sessionId, active->second);
                    else
                        started = mRuntime->startInputTurnLocked(prompt.sessionId, assistant);
                    startError = mRuntime->store->lastWriteError();
                }
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, prompt.sessionId,
                                              user.id);
                mRuntime->emitter.emitSession(MaiEventType::SessionUpdated, prompt.sessionId);
                if (started) {
                    mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, prompt.sessionId,
                                                  assistant.id);
                    started->signalReady();
                } else if (startError)
                    mRuntime->emitter.emitSession(MaiEventType::SessionError, prompt.sessionId,
                                                  startError.message());
                return user.id;

            } else if constexpr (std::is_same_v<T, MaiSendPrompt>) {
                if (operation.text.empty() && operation.images.empty() && operation.videos.empty())
                    return {MaiErrorCode::InvalidInput, "text or media is required"};

                MaiSession session;
                if (!mRuntime->store->getSession(operation.sessionId, session))
                    return {MaiErrorCode::NotFound, "session not found"};

                MaiMessage quoted;
                if (!operation.quotedMessageId.empty()) {
                    if (operation.quotedMessageId.rfind("msg_", 0) != 0)
                        return {MaiErrorCode::InvalidInput, "invalid quoted message ID"};
                    const auto page = mRuntime->store->listMessagesPage(
                        operation.sessionId, operation.quotedMessageId + "~", 1);
                    if (page.empty() || page.front().id != operation.quotedMessageId)
                        return {MaiErrorCode::NotFound,
                                "quoted message was not found in this conversation"};
                    quoted = page.front();
                }

                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    if (mRuntime->contextCommittingSessions.count(operation.sessionId) ||
                        (mRuntime->options.rejectWhenBusy &&
                         (mRuntime->active.count(operation.sessionId) ||
                          mRuntime->startingSessions.count(operation.sessionId))))
                        return {MaiErrorCode::Busy, "a turn is already running for this session"};
                    mRuntime->startingSessions.insert(operation.sessionId);
                }

                // 用户消息先落库并广播，界面立刻看到自己发的话。
                MaiMessage user;
                user.id = MaiIdGenerator::newMessageId();
                user.role = MaiRole::User;
                user.created = MaiTime::getCurrentTime();
                user.completed = user.created;
                if (!operation.quotedMessageId.empty()) {
                    MaiMessagePart quote;
                    quote.id = MaiIdGenerator::newPartId();
                    quote.body = MaiQuotePart{operation.quotedMessageId, quotedPreview(quoted)};
                    quote.created = user.created;
                    user.parts.push_back(std::move(quote));
                }
                if (!operation.text.empty()) {
                    MaiMessagePart up;
                    up.id = MaiIdGenerator::newPartId();
                    up.body = MaiTextPart{operation.text};
                    up.created = user.created;
                    user.parts.push_back(std::move(up));
                }
                for (const auto& image : operation.images) {
                    MaiMessagePart imagePart;
                    imagePart.id = MaiIdGenerator::newPartId();
                    imagePart.body = MaiImagePart{image.path, image.mimeType};
                    imagePart.created = user.created;
                    user.parts.push_back(std::move(imagePart));
                }
                for (const auto& video : operation.videos) {
                    MaiMessagePart videoPart;
                    videoPart.id = MaiIdGenerator::newPartId();
                    videoPart.body = video;
                    videoPart.created = user.created;
                    user.parts.push_back(std::move(videoPart));
                }
                mRuntime->store->putMessage(operation.sessionId, user);
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, operation.sessionId,
                                              user.id);

                // assistant 消息此刻就建好，后续 delta 都挂在它下面。
                MaiMessage assistant;
                assistant.id = MaiIdGenerator::newMessageId();
                assistant.role = MaiRole::Assistant;
                assistant.created = MaiTime::getCurrentTime();
                mRuntime->store->putMessage(operation.sessionId, assistant);
                mRuntime->emitter.emitMessage(MaiEventType::MessageUpdated, operation.sessionId,
                                              assistant.id);

                mRuntime->store->mutateSession(operation.sessionId, [](MaiSession& sess) {
                    sess.updated = MaiTime::getCurrentTime();
                });

                auto turn = std::make_shared<ActiveTurn>();
                turn->id = assistant.id;
                turn->acceptingSteering = true;
                {
                    std::lock_guard<std::mutex> lock(mRuntime->mutex);
                    mRuntime->active[operation.sessionId] = turn;
                    mRuntime->startingSessions.erase(operation.sessionId);
                    mRuntime->offerInputsLocked(operation.sessionId, turn);
                }

                // 单独线程跑，submit 立刻返回——同步等会让 HTTP 请求挂几十秒。
                const std::string sessionId = operation.sessionId;
                auto dependencies = mRuntime->dependencies(sessionId, turn);
                turn->worker = std::thread([this, sessionId, turn, dependencies, assistant] {
                    turn->waitReady();
                    // 给线程起名字。抓 dump 或者挂调试器时，
                    // 一堆并发的轮次才分得清谁是谁——否则只有一串线程 ID。Linux 上限 15 字节，
                    // 所以名字要短。
                    MaiThread::setCurrentName("mai-turn");
                    MaiTurnRunner runner(dependencies, sessionId, assistant);
                    runner.run(turn->cancel);
                    mRuntime->retire(sessionId, turn);
                    mRuntime->markWorkerFinished();
                });
                mRuntime->markWorkerStarted();
                turn->signalReady();
                return assistant.id;

            } else if constexpr (std::is_same_v<T, MaiInterrupt>) {
                return interruptTurn(operation.sessionId, {});

            } else if constexpr (std::is_same_v<T, MaiReplyQuestion>) {
                if (operation.questionId.empty())
                    return {MaiErrorCode::InvalidInput, "questionId is required"};
                // 找不到就是找不到：界面重复回答、或者对着已经被中断的提问回答，
                // 都会走到这里。不是故障，但也不能假装成功。
                if (!mRuntime->questions->reply(operation.questionId, operation.answer))
                    return {MaiErrorCode::NotFound, "question is no longer pending"};
                return operation.questionId;

            } else if constexpr (std::is_same_v<T, MaiReplyPermission>) {
                if (operation.permissionId.empty())
                    return {MaiErrorCode::InvalidInput, "permissionId is required"};
                // 找不到就是找不到：界面重复点、或者对着已经被中断的请求点，都会走到这里。
                // 不是故障，但也不能假装成功——界面要据此把那个已经过期的对话框收掉。
                if (!mRuntime->permissions->reply(operation.permissionId, operation.decision))
                    return {MaiErrorCode::NotFound, "permission request is no longer pending"};
                // permission.replied 由等在闸门上的那一轮发出（只有它知道请求的全貌）。
                // 这里只负责放行，不重复广播。
                return operation.permissionId;

            } else {
                return {MaiErrorCode::Internal, "unhandled operation"};
            }
        },
        operation);
}
