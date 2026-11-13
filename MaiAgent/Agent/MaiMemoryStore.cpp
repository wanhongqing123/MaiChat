// 文件作用：实现内存中的 Agent 会话和消息存储。
// 主要内容：以进程内结构保存会话、消息和片段；不进行磁盘持久化，适合测试与短期运行。
#include "MaiMemoryStore.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>

#include "MaiIdGenerator.h"

namespace {

class MemoryStore final : public MaiSessionStore {
public:
    MaiResult<MaiContextImportProgress> beginContextImport(
        const MaiContextImportSpec& spec) override {
        if (const auto error = maiValidateContextImportSpec(spec)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        if (mSessions.count(spec.sessionId) == 0)
            return {MaiErrorCode::NotFound, "Context import session is unavailable."};
        for (const auto& entry : mImports)
            if (!entry.second.progress.committed && !entry.second.aborted &&
                entry.second.spec.sessionId == spec.sessionId &&
                entry.second.spec.sourceKey == spec.sourceKey)
                return {MaiErrorCode::Busy, "A context snapshot is already being staged."};
        ImportSnapshot snapshot;
        snapshot.spec = spec;
        snapshot.progress = {MaiIdGenerator::generate("imp"),
                             spec.sessionId,
                             spec.sourceKey,
                             spec.snapshotId,
                             0,
                             0,
                             false};
        const auto progress = snapshot.progress;
        mImports.emplace(progress.importId, std::move(snapshot));
        return progress;
    }

    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& importId, std::uint64_t sequence,
        const std::vector<MaiContextImportRecord>& records) override {
        if (const auto error = maiValidateContextImportBatch(records)) return error;
        const auto digest = maiContextImportBatchDigest(records);
        if (digest.empty()) return {MaiErrorCode::Internal, "Cannot fingerprint context batch."};
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mImports.find(importId);
        if (found == mImports.end())
            return {MaiErrorCode::NotFound, "Context import is unavailable."};
        auto& snapshot = found->second;
        if (snapshot.aborted) return {MaiErrorCode::NotFound, "Context import was aborted."};
        if (snapshot.progress.committed)
            return {MaiErrorCode::InvalidInput, "Context import is already committed."};
        const auto prior = snapshot.chunks.find(sequence);
        if (prior != snapshot.chunks.end()) {
            if (prior->second == digest) return snapshot.progress;
            return {MaiErrorCode::InvalidInput, "Context batch retry has different content."};
        }
        if (sequence != snapshot.progress.nextSequence ||
            sequence >= static_cast<std::uint64_t>(INT64_MAX) ||
            (snapshot.progress.recordCount &&
             records.front().originalCreatedAtMillis < snapshot.lastCreated))
            return {MaiErrorCode::InvalidInput, "Context batch sequence or chronology is invalid."};
        for (const auto& record : records)
            if (snapshot.sourceIds.count(record.sourceId))
                return {MaiErrorCode::InvalidInput, "Context source record is duplicated."};
        // 全批验证后才改变暂存，普通读仍只跟随可见来源指针。
        for (const auto& record : records) {
            MaiMessage message;
            message.id = MaiIdGenerator::newMessageId();
            message.created = record.originalCreatedAtMillis;
            message.completed = std::max<std::int64_t>(1, message.created);
            message.parts.push_back(
                {MaiIdGenerator::newPartId(), MaiTextPart{record.text}, message.created});
            snapshot.messages.emplace(message.id, std::move(message));
            snapshot.sourceIds.insert(record.sourceId);
        }
        snapshot.lastCreated = records.back().originalCreatedAtMillis;
        snapshot.chunks.emplace(sequence, digest);
        ++snapshot.progress.nextSequence;
        snapshot.progress.recordCount += records.size();
        return snapshot.progress;
    }

    MaiResult<MaiContextImportProgress> inspectContextImport(
        const std::string& importId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mImports.find(importId);
        if (found == mImports.end() || found->second.aborted)
            return {MaiErrorCode::NotFound, "Context import is unavailable."};
        return found->second.progress;
    }

    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& importId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mImports.find(importId);
        if (found == mImports.end())
            return {MaiErrorCode::NotFound, "Context import is unavailable."};
        auto& snapshot = found->second;
        if (snapshot.aborted) return {MaiErrorCode::NotFound, "Context import was aborted."};
        if (snapshot.progress.committed) return snapshot.progress;
        mCurrentImports[{snapshot.spec.sessionId, snapshot.spec.sourceKey}] = importId;
        mContextCheckpoints.erase(snapshot.spec.sessionId);
        snapshot.progress.committed = true;
        return snapshot.progress;
    }

    MaiError abortContextImport(const std::string& importId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mImports.find(importId);
        if (found == mImports.end() || found->second.aborted)
            return {MaiErrorCode::NotFound, "Context import is unavailable."};
        if (found->second.progress.committed)
            return {MaiErrorCode::InvalidInput, "Published context cannot be aborted."};
        found->second.aborted = true;
        return {};
    }

    MaiResult<MaiContextImportPurgeResult> purgeContextImports(const std::string& sessionId,
                                                               std::size_t maxMessages) override {
        if (maxMessages == 0 || maxMessages > kMaiContextImportMaxPurgeMessages)
            return {MaiErrorCode::InvalidInput, "Context maintenance budget is invalid."};
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mSessions.count(sessionId))
            return {MaiErrorCode::NotFound, "Context session is unavailable."};
        const auto eligible = [&](const ImportSnapshot& snapshot) {
            const auto current =
                mCurrentImports.find({snapshot.spec.sessionId, snapshot.spec.sourceKey});
            return snapshot.spec.sessionId == sessionId &&
                   (snapshot.aborted || snapshot.progress.committed) &&
                   (current == mCurrentImports.end() ||
                    current->second != snapshot.progress.importId);
        };
        MaiContextImportPurgeResult result;
        std::size_t metadataBudget = maxMessages;
        for (auto it = mImports.begin(); it != mImports.end();) {
            if (!eligible(it->second)) {
                ++it;
                continue;
            }
            while (!it->second.messages.empty() && result.removed < maxMessages) {
                it->second.messages.erase(it->second.messages.begin());
                ++result.removed;
            }
            // 空来源也有独立配额，避免很多空快照令单次维护无限扫描/释放。
            if (it->second.messages.empty()) {
                while (!it->second.sourceIds.empty() && metadataBudget != 0) {
                    it->second.sourceIds.erase(it->second.sourceIds.begin());
                    --metadataBudget;
                }
                while (!it->second.chunks.empty() && metadataBudget != 0) {
                    it->second.chunks.erase(it->second.chunks.begin());
                    --metadataBudget;
                }
                if (it->second.sourceIds.empty() && it->second.chunks.empty() &&
                    metadataBudget != 0) {
                    --metadataBudget;
                    it = mImports.erase(it);
                } else
                    ++it;
            } else
                ++it;
            if (result.removed == maxMessages && metadataBudget == 0) break;
        }
        for (const auto& entry : mImports) {
            if (eligible(entry.second)) {
                result.hasMore = true;
                break;
            }
        }
        return result;
    }

    void putSession(const MaiSession& session) override {
        std::lock_guard<std::mutex> lock(mMutex);
        mSessions[session.id] = session;
    }

    // 纯内存，写不会失败——除非内存耗尽，那时候进程已经没了。
    MaiError lastWriteError() const override {
        return {};
    }

    bool getSession(const std::string& id, MaiSession& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSessions.find(id);
        if (it == mSessions.end()) return false;
        out = it->second;
        return true;
    }

    std::vector<MaiSession> listSessions() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSession> out;
        out.reserve(mSessions.size());
        for (const auto& [_, session] : mSessions) out.push_back(session);
        // 界面左侧列表按最近活跃排序，这里就排好，省得每个调用方各排一遍。
        std::sort(out.begin(), out.end(), [](const MaiSession& left, const MaiSession& right) {
            return left.updated > right.updated;
        });
        return out;
    }

    bool removeSession(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mMutex);
        mMessages.erase(id);
        clearImportsLocked(id);
        mContextCheckpoints.erase(id);
        clearToolOutputsLocked(id);
        for (auto it = mSpecialistTasks.begin(); it != mSpecialistTasks.end();) {
            if (it->second.ownerSessionId == id) {
                it = mSpecialistTasks.erase(it);
            } else
                ++it;
        }
        return mSessions.erase(id) > 0;
    }

    bool clearMessages(const std::string& sessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mSessions.find(sessionId) == mSessions.end()) return false;
        mMessages.erase(sessionId);
        clearImportsLocked(sessionId);
        mContextCheckpoints.erase(sessionId);
        clearToolOutputsLocked(sessionId);
        for (auto it = mSpecialistTasks.begin(); it != mSpecialistTasks.end();) {
            if (it->second.ownerSessionId == sessionId) {
                it = mSpecialistTasks.erase(it);
            } else
                ++it;
        }
        return true;
    }

    MaiError putToolOutput(const MaiToolOutputRecord& output) override {
        if (const auto error = maiValidateToolOutputRecord(output)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        if (!ownsToolOutputLocked(output) || mToolOutputIds.count(output.id) != 0)
            return {MaiErrorCode::InvalidInput, "Tool output owner or immutable ID is invalid."};

        // 先算淘汰集合再修改，避免新快照自身过旧时返回成功却已无法读取。
        std::vector<const MaiToolOutputRecord*> retained;
        std::size_t sessionBytes = output.text.size();
        std::size_t totalBytes = output.text.size();
        for (const auto& entry : mToolOutputs) {
            retained.push_back(&entry.second);
            totalBytes += entry.second.text.size();
            if (entry.second.sessionId == output.sessionId)
                sessionBytes += entry.second.text.size();
        }
        retained.push_back(&output);
        std::sort(retained.begin(), retained.end(), [](const auto* left, const auto* right) {
            if (left->created != right->created) return left->created < right->created;
            return left->id < right->id;
        });
        std::vector<std::string> evicted;
        for (const auto* candidate : retained) {
            if (sessionBytes <= kMaiToolOutputSessionMaxBytes) break;
            if (candidate->sessionId != output.sessionId) continue;
            evicted.push_back(candidate->id);
            sessionBytes -= candidate->text.size();
            totalBytes -= candidate->text.size();
        }
        for (const auto* candidate : retained) {
            if (totalBytes <= kMaiToolOutputTotalMaxBytes) break;
            if (std::find(evicted.begin(), evicted.end(), candidate->id) != evicted.end()) continue;
            evicted.push_back(candidate->id);
            totalBytes -= candidate->text.size();
        }
        if (std::find(evicted.begin(), evicted.end(), output.id) != evicted.end())
            return {MaiErrorCode::InvalidInput, "Tool output is older than the retention window."};
        for (const auto& id : evicted) mToolOutputs.erase(id);
        mToolOutputIds.emplace(output.id, output.sessionId);
        mToolOutputs.emplace(output.id, output);
        return {};
    }

    MaiResult<MaiToolOutputPage> readToolOutput(const std::string& sessionId,
                                                const MaiToolOutputQuery& query) const override {
        if (const auto error = maiValidateToolOutputQuery(sessionId, query)) return error;
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mToolOutputs.find(query.outputId);
        if (found == mToolOutputs.end() || found->second.sessionId != sessionId ||
            !ownsToolOutputLocked(found->second))
            return {MaiErrorCode::NotFound, "Retained tool output is unavailable."};
        return maiPageToolOutput(found->second, query, found->second.text, 0,
                                 found->second.text.size());
    }

    bool getContextCheckpoint(const std::string& sessionId,
                              MaiContextCheckpoint& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mContextCheckpoints.find(sessionId);
        if (found == mContextCheckpoints.end() || mSessions.count(sessionId) == 0) return false;
        out = found->second;
        return true;
    }

    MaiError advanceContextCheckpoint(const std::string& sessionId, std::uint64_t expectedRevision,
                                      const MaiContextCheckpoint& checkpoint) override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mContextCheckpoints.find(sessionId);
        const std::uint64_t revision =
            found == mContextCheckpoints.end() ? 0 : found->second.revision;
        const auto history = visibleMessagesLocked(sessionId);
        if (mSessions.count(sessionId) == 0 || history.empty())
            return {MaiErrorCode::NotFound, "Session history no longer exists."};
        if (revision != expectedRevision || checkpoint.revision == 0 ||
            checkpoint.revision - 1 != expectedRevision ||
            !maiContextCheckpointMatches(checkpoint, sessionId, history))
            return {MaiErrorCode::Busy, "Context source changed while preparing its summary."};
        mContextCheckpoints[sessionId] = checkpoint;
        return {};
    }

    MaiError insertSpecialistTask(const MaiSpecialistTask& task) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (task.id.empty() || task.ownerSessionId.empty() || task.specialistName.empty() ||
            mSessions.find(task.ownerSessionId) == mSessions.end() ||
            mSpecialistTasks.find(task.id) != mSpecialistTasks.end())
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist task identity");
        if (!task.parentTaskId.empty()) {
            const auto parent = mSpecialistTasks.find(task.parentTaskId);
            if (parent == mSpecialistTasks.end() ||
                parent->second.ownerSessionId != task.ownerSessionId ||
                parent->second.specialistName != task.specialistName)
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "specialist parent belongs to another task chain");
        }
        MaiSpecialistTask stored = task;
        if (!stored.outputPath.empty()) {
            stored.status = MaiSpecialistTaskStatus::Succeeded;
            stored.notifiedAt = stored.created;
        }
        mSpecialistTasks.emplace(task.id, std::move(stored));
        return {};
    }

    bool getSpecialistTask(const std::string& id, const std::string& ownerSessionId,
                           MaiSpecialistTask& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto it = mSpecialistTasks.find(id);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return false;
        out = it->second;
        return true;
    }

    MaiError deleteCompletedSpecialistTask(const std::string& taskId,
                                           const std::string& ownerSessionId) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status == MaiSpecialistTaskStatus::Submitted ||
            it->second.status == MaiSpecialistTaskStatus::Running || it->second.notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist task is active or delivery is pending");
        for (const auto& [_, child] : mSpecialistTasks) {
            if (child.parentTaskId == taskId)
                return MaiError::make(MaiErrorCode::InvalidInput,
                                      "specialist task has revision children");
        }
        mSpecialistTasks.erase(it);
        return {};
    }

    MaiError appendSpecialistText(const std::string& taskId, const std::string& ownerSessionId,
                                  const std::string& chunk, MaiMillis checkedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist task is not running");
        if (chunk.size() > 16 * 1024 || it->second.partialText.size() + chunk.size() > 64 * 1024)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist text is too large");
        it->second.partialText += chunk;
        it->second.lastCheckedAt = checkedAt;
        it->second.status = MaiSpecialistTaskStatus::Running;
        return {};
    }

    MaiError updateSpecialistProgress(const std::string& taskId, const std::string& ownerSessionId,
                                      MaiMillis checkedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput, "specialist task is not running");
        it->second.status = MaiSpecialistTaskStatus::Running;
        it->second.lastCheckedAt = checkedAt;
        return {};
    }

    MaiError recoverStatusCheckFailures() override {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& [_, task] : mSpecialistTasks) {
            if (task.status != MaiSpecialistTaskStatus::Failed || task.providerTaskId.empty() ||
                task.errorText.rfind("Status check failed:", 0) != 0)
                continue;
            task.status = MaiSpecialistTaskStatus::Running;
            task.finalText.clear();
            task.errorText.clear();
            task.outputPath.clear();
            task.lastCheckedAt = 0;
            task.notificationMessageId.clear();
            task.notificationAttemptAt = 0;
            task.notifiedAt = 0;
        }
        return {};
    }

    MaiError finishSpecialistTask(const std::string& taskId, const std::string& ownerSessionId,
                                  MaiSpecialistTaskStatus status, const std::string& finalText,
                                  const std::string& outputPath, MaiMillis finishedAt) override {
        if (status != MaiSpecialistTaskStatus::Succeeded &&
            status != MaiSpecialistTaskStatus::NeedsInput &&
            status != MaiSpecialistTaskStatus::Failed &&
            status != MaiSpecialistTaskStatus::Canceled)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid final specialist status");
        if (finalText.size() > 64 * 1024 || outputPath.size() > 4096 || finishedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist reply");
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId)
            return MaiError::make(MaiErrorCode::NotFound, "specialist task was not found");
        if (it->second.status != MaiSpecialistTaskStatus::Submitted &&
            it->second.status != MaiSpecialistTaskStatus::Running)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist task is already finished");
        if (status == MaiSpecialistTaskStatus::Succeeded && finalText.empty() &&
            it->second.partialText.empty() && outputPath.empty())
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "a successful specialist needs text or an output file");
        it->second.status = status;
        it->second.finalText = finalText.empty() ? it->second.partialText : finalText;
        it->second.outputPath = outputPath;
        if (status == MaiSpecialistTaskStatus::Failed ||
            status == MaiSpecialistTaskStatus::Canceled)
            it->second.errorText = it->second.finalText;
        it->second.lastCheckedAt = finishedAt;
        return {};
    }

    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        for (const auto& [_, task] : mSpecialistTasks) {
            if (!task.providerTaskId.empty() &&
                (task.status == MaiSpecialistTaskStatus::Submitted ||
                 task.status == MaiSpecialistTaskStatus::Running))
                tasks.push_back(task);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            if (left.lastCheckedAt != right.lastCheckedAt)
                return left.lastCheckedAt < right.lastCheckedAt;
            return left.created < right.created;
        });
        if (tasks.size() > limit) tasks.resize(limit);
        return tasks;
    }

    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSpecialistTask> tasks;
        for (const auto& [_, task] : mSpecialistTasks) {
            if (task.notifiedAt == 0 && task.status != MaiSpecialistTaskStatus::Submitted &&
                task.status != MaiSpecialistTaskStatus::Running)
                tasks.push_back(task);
        }
        std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
            return left.lastCheckedAt < right.lastCheckedAt;
        });
        if (tasks.size() > limit) tasks.resize(limit);
        return tasks;
    }

    MaiResult<std::string> reserveSpecialistNotification(const std::string& taskId,
                                                         const std::string& ownerSessionId,
                                                         const std::string& proposedMessageId,
                                                         MaiMillis attemptedAt) override {
        if (proposedMessageId.empty() || attemptedAt == 0)
            return {MaiErrorCode::InvalidInput, "invalid specialist notification"};
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId ||
            it->second.notifiedAt != 0 || it->second.status == MaiSpecialistTaskStatus::Submitted ||
            it->second.status == MaiSpecialistTaskStatus::Running)
            return {MaiErrorCode::NotFound, "specialist reply is unavailable"};
        if (it->second.notificationMessageId.empty())
            it->second.notificationMessageId = proposedMessageId;
        it->second.notificationAttemptAt = attemptedAt;
        return it->second.notificationMessageId;
    }

    MaiError markSpecialistNotified(const std::string& taskId, const std::string& ownerSessionId,
                                    const std::string& messageId, MaiMillis notifiedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId ||
            it->second.notificationMessageId != messageId || messageId.empty() || notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist notification identity does not match");
        it->second.notifiedAt = notifiedAt;
        return {};
    }

    MaiError markSpecialistResultAlreadyDelivered(const std::string& taskId,
                                                  const std::string& ownerSessionId,
                                                  MaiMillis notifiedAt) override {
        if (notifiedAt == 0)
            return MaiError::make(MaiErrorCode::InvalidInput, "invalid specialist delivery time");
        std::lock_guard<std::mutex> lock(mMutex);
        const auto it = mSpecialistTasks.find(taskId);
        if (it == mSpecialistTasks.end() || it->second.ownerSessionId != ownerSessionId ||
            it->second.status != MaiSpecialistTaskStatus::Succeeded ||
            it->second.providerTaskId.empty() || it->second.notifiedAt != 0)
            return MaiError::make(MaiErrorCode::InvalidInput,
                                  "specialist result cannot be deduplicated");
        it->second.notifiedAt = notifiedAt;
        return {};
    }

    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutator) override {
        // 读-改-写整个在锁内完成。调用方自己做这三步的话，
        // 中间那段窗口里别人的改动会被盖掉（lost update）——之前跑一轮的收尾就有这个洞。
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mSessions.find(id);
        if (it == mSessions.end()) return false;
        mutator(it->second);
        return true;
    }

    void putMessage(const std::string& sessionId, const MaiMessage& message) override {
        std::lock_guard<std::mutex> lock(mMutex);
        auto& list = mMessages[sessionId];
        // 同 id 视为更新：流式期间同一条消息会被反复写回。
        auto it = std::find_if(list.begin(), list.end(), [&](const MaiMessage& existing) {
            return existing.id == message.id;
        });
        if (it != list.end()) {
            *it = message;
        } else {
            list.push_back(message);
        }
    }

    std::vector<MaiMessage> listMessages(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return visibleMessagesLocked(sessionId);
    }

    bool getMessage(const std::string& sessionId, const std::string& messageId,
                    MaiMessage& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto session = mMessages.find(sessionId);
        if (session == mMessages.end()) return importedMessageLocked(sessionId, messageId, out);
        const auto message =
            std::find_if(session->second.begin(), session->second.end(),
                         [&](const MaiMessage& value) { return value.id == messageId; });
        if (message == session->second.end())
            return importedMessageLocked(sessionId, messageId, out);
        out = *message;
        return true;
    }

    std::vector<MaiMessage> listMessagesPage(const std::string& sessionId,
                                             const std::string& beforeId,
                                             std::size_t limit) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiMessage> page;
        if (limit == 0) return page;
        auto it = mMessages.find(sessionId);
        if (it != mMessages.end()) {
            page.reserve(std::min(limit, it->second.size()));
            for (auto message = it->second.rbegin(); message != it->second.rend(); ++message) {
                if (!beforeId.empty() && message->id >= beforeId) continue;
                page.push_back(*message);
                if (page.size() == limit) break;
            }
        }
        for (const auto& source : mCurrentImports) {
            if (source.first.first != sessionId) continue;
            const auto& messages = mImports.at(source.second).messages;
            auto cursor = beforeId.empty() ? messages.end() : messages.lower_bound(beforeId);
            for (std::size_t count = 0; cursor != messages.begin() && count < limit; ++count) {
                --cursor;
                page.push_back(cursor->second);
            }
        }
        std::sort(page.begin(), page.end(),
                  [](const auto& left, const auto& right) { return left.id > right.id; });
        if (page.size() > limit) page.resize(limit);
        return page;
    }

    void recoverInterruptedTools(const std::string& error, std::int64_t completedAt) override {
        std::lock_guard<std::mutex> lock(mMutex);
        for (auto& entry : mMessages) {
            for (MaiMessage& message : entry.second) {
                bool changed = false;
                for (MaiMessagePart& part : message.parts) {
                    auto* tool = std::get_if<MaiToolPart>(&part.body);
                    if (!tool || (tool->state != MaiToolState::Pending &&
                                  tool->state != MaiToolState::Running))
                        continue;
                    const bool pending = tool->state == MaiToolState::Pending;
                    auto execution = tool->execution.value_or(MaiToolExecution{});
                    execution.status = pending ? MaiToolExecutionStatus::NotStarted
                                               : MaiToolExecutionStatus::Unknown;
                    execution.effectStatus =
                        pending ? MaiToolEffectStatus::None : MaiToolEffectStatus::Unknown;
                    execution.nextAction = pending ? MaiToolNextAction::None
                                           : !execution.taskId.empty()
                                               ? MaiToolNextAction::QueryStatus
                                               : MaiToolNextAction::InspectState;
                    execution.exitCode.reset();
                    if (pending) {
                        execution.started = false;
                        execution.timedOut = false;
                        execution.canceled = false;
                    } else if (!execution.started.value_or(false))
                        execution.started.reset();
                    tool->execution = std::move(execution);
                    tool->state = MaiToolState::Error;
                    tool->error = error;
                    changed = true;
                }
                if (changed && message.completed == 0) message.completed = completedAt;
            }
        }
    }

private:
    struct ImportSnapshot {
        MaiContextImportSpec spec;
        MaiContextImportProgress progress;
        std::map<std::uint64_t, std::string> chunks;
        std::set<std::string> sourceIds;
        std::map<std::string, MaiMessage> messages;
        std::int64_t lastCreated = 0;
        bool aborted = false;
    };

    void clearImportsLocked(const std::string& sessionId) {
        for (auto it = mCurrentImports.begin(); it != mCurrentImports.end();)
            if (it->first.first == sessionId)
                it = mCurrentImports.erase(it);
            else
                ++it;
        for (auto it = mImports.begin(); it != mImports.end();)
            if (it->second.spec.sessionId == sessionId)
                it = mImports.erase(it);
            else
                ++it;
    }

    std::vector<MaiMessage> visibleMessagesLocked(const std::string& sessionId) const {
        std::vector<MaiMessage> result;
        const auto ordinary = mMessages.find(sessionId);
        if (ordinary != mMessages.end()) result = ordinary->second;
        bool imported = false;
        for (const auto& source : mCurrentImports)
            if (source.first.first == sessionId)
                for (const auto& message : mImports.at(source.second).messages) {
                    result.push_back(message.second);
                    imported = true;
                }
        if (imported)
            std::sort(result.begin(), result.end(),
                      [](const auto& left, const auto& right) { return left.id < right.id; });
        return result;
    }

    bool importedMessageLocked(const std::string& sessionId, const std::string& messageId,
                               MaiMessage& out) const {
        for (const auto& source : mCurrentImports) {
            if (source.first.first != sessionId) continue;
            const auto& messages = mImports.at(source.second).messages;
            const auto found = messages.find(messageId);
            if (found != messages.end()) {
                out = found->second;
                return true;
            }
        }
        return false;
    }

    std::map<std::string, ImportSnapshot> mImports;
    std::map<std::pair<std::string, std::string>, std::string> mCurrentImports;
    // 锁内核验当前源片段；保留快照不能绕过消息或 callId 后续变更。
    bool ownsToolOutputLocked(const MaiToolOutputRecord& output) const {
        if (mSessions.count(output.sessionId) == 0) return false;
        const auto session = mMessages.find(output.sessionId);
        if (session == mMessages.end()) return false;
        for (const auto& message : session->second) {
            if (message.id != output.messageId) continue;
            for (const auto& part : message.parts) {
                if (part.id != output.partId) continue;
                const auto* tool = std::get_if<MaiToolPart>(&part.body);
                return tool && tool->callId == output.callId;
            }
        }
        return false;
    }

    // 同锁清理字节和编号墓碑；淘汰仅丢字节，直至消息被清空才释放其不可变编号。
    void clearToolOutputsLocked(const std::string& sessionId) {
        for (auto it = mToolOutputs.begin(); it != mToolOutputs.end();) {
            if (it->second.sessionId == sessionId)
                it = mToolOutputs.erase(it);
            else
                ++it;
        }
        for (auto it = mToolOutputIds.begin(); it != mToolOutputIds.end();) {
            if (it->second == sessionId)
                it = mToolOutputIds.erase(it);
            else
                ++it;
        }
    }

    mutable std::mutex mMutex;
    std::unordered_map<std::string, MaiSession> mSessions;
    std::unordered_map<std::string, std::vector<MaiMessage>> mMessages;
    std::unordered_map<std::string, MaiContextCheckpoint> mContextCheckpoints;
    std::unordered_map<std::string, MaiSpecialistTask> mSpecialistTasks;
    std::unordered_map<std::string, MaiToolOutputRecord> mToolOutputs;
    std::unordered_map<std::string, std::string> mToolOutputIds;
};

}  // namespace

std::unique_ptr<MaiSessionStore> makeMaiMemoryStore() {
    return std::make_unique<MemoryStore>();
}
