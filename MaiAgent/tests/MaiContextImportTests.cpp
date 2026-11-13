// 文件作用：用自建存储和确定模型验证外部上下文暂存、原子发布、来源替换及启动互斥。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

#include "MaiAgent.h"
#include "MaiContextCheckpoint.h"
#include "MaiContextImport.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiThread.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

struct OwnedDatabase {
    MaiFilePath directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-context-import-" + MaiIdGenerator::newSessionId()));
    OwnedDatabase() {
        CHECK(!MaiFileSystem::createDirectories(directory));
    }
    ~OwnedDatabase() {
        MaiFileSystem::removeRecursively(directory);
    }
    std::string path() const {
        return directory.append(MaiFilePath::fromUtf8("fixture.sqlite")).toUtf8();
    }
};

std::unique_ptr<MaiSessionStore> sqliteStore(const std::string& path = ":memory:") {
    auto opened = makeMaiSqliteStore(path);
    CHECK(opened);
    return opened ? std::move(opened.value()) : nullptr;
}

std::string createSession(MaiSessionStore& store) {
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.title = "Owned import fixture";
    session.directory = MaiFileSystem::temporaryDirectory().toUtf8();
    session.model = "fixture";
    session.created = 10;
    session.updated = 10;
    store.putSession(session);
    CHECK(!store.lastWriteError());
    return session.id;
}

MaiMessage ordinaryMessage(const std::string& text, MaiRole role = MaiRole::User) {
    MaiMessage message;
    message.id = MaiIdGenerator::newMessageId();
    message.role = role;
    message.created = 11;
    message.completed = 12;
    message.parts.push_back({MaiIdGenerator::newPartId(), MaiTextPart{text}, 11});
    return message;
}

MaiContextImportSpec spec(const std::string& session, const std::string& snapshot,
                          const std::string& source = "owned-source") {
    return {session, source, snapshot, "Quoted external conversation"};
}

std::vector<std::string> texts(const std::vector<MaiMessage>& messages) {
    std::vector<std::string> result;
    for (const auto& message : messages) result.push_back(message.text());
    return result;
}

void checkImportedRow(const MaiMessage& message, const std::string& text, MaiMillis created) {
    CHECK(message.id.rfind("msg_", 0) == 0);
    CHECK(message.role == MaiRole::User);
    CHECK(message.created == created);
    CHECK(message.completed != 0);
    CHECK(message.inputRequestId.empty());
    CHECK(!message.pendingInput);
    CHECK(message.parts.size() == 1);
    if (message.parts.size() == 1) {
        CHECK(std::holds_alternative<MaiTextPart>(message.parts.front().body));
        CHECK(message.parts.front().id.rfind("prt_", 0) == 0);
    }
    CHECK(message.text() == text);
}

void stagingAndChunkRetries(MaiSessionStore& store) {
    const auto session = createSession(store);
    const auto ordinary = ordinaryMessage("Ordinary row survives staging");
    store.putMessage(session, ordinary);
    const auto begun = store.beginContextImport(spec(session, "snapshot-1"));
    CHECK(begun);
    if (!begun) return;
    const auto id = begun.value().importId;
    CHECK(!id.empty());
    CHECK(begun.value().nextSequence == 0 && begun.value().recordCount == 0);
    CHECK(!store.beginContextImport(spec(session, "snapshot-1")));
    const std::vector<MaiContextImportRecord> first{
        {"external-a", "[friend, original time 100] Older incoming", 100},
        {"external-b", "[owner, original time 200] Older outgoing", 200}};
    const auto appended = store.appendContextImport(id, 0, first);
    CHECK(appended);
    if (!appended) return;
    CHECK(appended.value().nextSequence == 1 && appended.value().recordCount == 2);
    CHECK(texts(store.listMessages(session)) == std::vector<std::string>{ordinary.text()});
    CHECK(store.listMessagesPage(session, {}, 200).size() == 1);
    CHECK(store.listQueuedInputs(session).empty());

    const auto retried = store.appendContextImport(id, 0, first);
    CHECK(retried && retried.value().recordCount == 2 && retried.value().nextSequence == 1);
    auto conflicting = first;
    conflicting[0].text += " changed";
    CHECK(!store.appendContextImport(id, 0, conflicting));
    CHECK(!store.appendContextImport(id, 2, {{"external-c", "Skipped sequence", 300}}));
    CHECK(!store.appendContextImport(id, 1, {{"external-a", "Repeated source", 300}}));
    CHECK(!store.appendContextImport(id, 1, {{"older-than-last", "Backwards time", 199}}));
    CHECK(!store.appendContextImport(
        id, 1, {{"external-c", "Duplicate one", 300}, {"external-c", "Duplicate two", 301}}));
    const auto inspected = store.inspectContextImport(id);
    CHECK(inspected && inspected.value().nextSequence == 1 && inspected.value().recordCount == 2);
    CHECK(store.appendContextImport(id, 1, {{"external-c", "[friend] Equal-time tie", 200}}));
    CHECK(store.listMessages(session).size() == 1);
    const auto committed = store.commitContextImport(id);
    CHECK(committed && committed.value().committed && committed.value().recordCount == 3);
    auto visible = store.listMessages(session);
    CHECK(visible.size() == 4);
    if (visible.size() == 4) {
        CHECK(visible[0].id == ordinary.id);
        checkImportedRow(visible[1], first[0].text, 100);
        checkImportedRow(visible[2], first[1].text, 200);
        checkImportedRow(visible[3], "[friend] Equal-time tie", 200);
        CHECK(visible[1].id < visible[2].id && visible[2].id < visible[3].id);
        CHECK(visible[1].id != first[0].sourceId);
        MaiMessage exact;
        CHECK(store.getMessage(session, visible[1].id, exact));
        CHECK(exact.text() == first[0].text);
        CHECK(!store.getMessage(session, first[0].sourceId, exact));
    }
    const auto secondCommit = store.commitContextImport(id);
    CHECK(secondCommit && secondCommit.value().committed);
    CHECK(store.listMessages(session).size() == 4);
    CHECK(!store.appendContextImport(id, 2, {{"external-d", "Too late", 400}}));
}

void replacementAbortAndCheckpoint(MaiSessionStore& store) {
    const auto session = createSession(store);
    const auto ordinary = ordinaryMessage("Ordinary context remains exact");
    store.putMessage(session, ordinary);
    const auto first = store.beginContextImport(spec(session, "old"));
    CHECK(first);
    if (!first) return;
    CHECK(store.appendContextImport(first.value().importId, 0, {{"a", "Source A old", 50}}));
    CHECK(store.commitContextImport(first.value().importId));
    const auto other = store.beginContextImport(spec(session, "other", "other-source"));
    CHECK(other);
    if (!other) return;
    CHECK(store.appendContextImport(other.value().importId, 0, {{"a", "Other source", 60}}));
    CHECK(store.commitContextImport(other.value().importId));
    const auto oldVisible = store.listMessages(session);
    CHECK(oldVisible.size() == 3);
    if (oldVisible.size() != 3) return;

    MaiContextCheckpoint checkpoint;
    checkpoint.sessionId = session;
    checkpoint.revision = 1;
    checkpoint.sourceMessageCount = oldVisible.size();
    checkpoint.firstMessageId = oldVisible.front().id;
    checkpoint.throughMessageId = oldVisible.back().id;
    checkpoint.sourceFingerprint =
        maiContextSourceFingerprint(session, oldVisible, oldVisible.size());
    checkpoint.summary = R"({"source_notes":["owned fixture"]})";
    checkpoint.references = "[]";
    checkpoint.model = "fixture";
    CHECK(!store.advanceContextCheckpoint(session, 0, checkpoint));

    const auto replacement = store.beginContextImport(spec(session, "new"));
    CHECK(replacement);
    if (!replacement) return;
    CHECK(
        store.appendContextImport(replacement.value().importId, 0,
                                  {{"a", "Source A replaced", 50}, {"b", "Source A new row", 70}}));
    CHECK(texts(store.listMessages(session)) == texts(oldVisible));
    MaiContextCheckpoint loaded;
    CHECK(store.getContextCheckpoint(session, loaded));
    CHECK(store.commitContextImport(replacement.value().importId));
    CHECK(!store.getContextCheckpoint(session, loaded));
    const auto visible = store.listMessages(session);
    CHECK(texts(visible) == std::vector<std::string>({ordinary.text(), "Other source",
                                                      "Source A replaced", "Source A new row"}));
    MaiMessage exact;
    CHECK(!store.getMessage(session, oldVisible[1].id, exact));
    CHECK(store.getMessage(session, ordinary.id, exact) && exact.text() == ordinary.text());

    const auto abandoned = store.beginContextImport(spec(session, "abandoned"));
    CHECK(abandoned);
    if (!abandoned) return;
    CHECK(store.appendContextImport(abandoned.value().importId, 0,
                                    {{"will-not-publish", "Aborted source", 80}}));
    CHECK(!store.abortContextImport(abandoned.value().importId));
    CHECK(!store.commitContextImport(abandoned.value().importId));
    CHECK(texts(store.listMessages(session)) == texts(visible));

    // 空快照是完整来源的合法替换，不能误清普通消息或另一个来源。
    const auto empty = store.beginContextImport(spec(session, "empty"));
    CHECK(empty && store.commitContextImport(empty.value().importId));
    CHECK(texts(store.listMessages(session)) ==
          std::vector<std::string>({ordinary.text(), "Other source"}));
    const auto cleanup = store.beginContextImport(spec(session, "clear-pending"));
    CHECK(cleanup);
    CHECK(store.clearMessages(session));
    CHECK(store.listMessages(session).empty());
    if (cleanup) CHECK(!store.commitContextImport(cleanup.value().importId));
    const auto deletion = store.beginContextImport(spec(session, "delete-pending"));
    CHECK(deletion);
    CHECK(store.removeSession(session));
    if (deletion) CHECK(!store.commitContextImport(deletion.value().importId));
}

void limitsAndManyPages(MaiSessionStore& store) {
    const auto session = createSession(store);
    CHECK(!store.beginContextImport(spec(session, "invalid-source", "")));
    CHECK(!store.beginContextImport(spec(session, "invalid-source", std::string(129, 's'))));
    CHECK(!store.beginContextImport(spec(session, "invalid-source", "non-ascii-\xc3\xa9")));
    CHECK(!store.beginContextImport(spec("missing-session", "missing")));
    const auto boundary = store.beginContextImport(spec(session, "record-boundary", "boundary"));
    CHECK(boundary);
    if (boundary) {
        CHECK(store.appendContextImport(
            boundary.value().importId, 0,
            {{"exact-limit", std::string(kMaiContextImportMaxRecordTextBytes, 'x'), 0}}));
        CHECK(store.listMessages(session).empty());
        CHECK(!store.abortContextImport(boundary.value().importId));
    }
    const auto begun = store.beginContextImport(spec(session, "bounded-pages"));
    CHECK(begun);
    if (!begun) return;
    const auto id = begun.value().importId;
    CHECK(!store.appendContextImport(
        id, 0, {{"large", std::string(kMaiContextImportMaxRecordTextBytes + 1, 'x'), 0}}));
    CHECK(!store.appendContextImport(id, 0, {{"", "Missing source identity", 0}}));
    CHECK(!store.appendContextImport(id, 0, {{"negative", "Invalid time", -1}}));
    std::vector<MaiContextImportRecord> tooMany(kMaiContextImportMaxBatchRecords + 1);
    for (std::size_t index = 0; index < tooMany.size(); ++index)
        tooMany[index] = {"too-many-" + std::to_string(index), "x", 0};
    CHECK(!store.appendContextImport(id, 0, tooMany));
    CHECK(!store.appendContextImport(
        id, 0,
        {{"large-a", std::string(kMaiContextImportMaxRecordTextBytes, 'a'), 0},
         {"large-b", std::string(kMaiContextImportMaxRecordTextBytes, 'b'), 0}}));
    const auto unchanged = store.inspectContextImport(id);
    CHECK(unchanged && unchanged.value().nextSequence == 0 && unchanged.value().recordCount == 0);

    constexpr std::size_t total = 6001;
    std::uint64_t sequence = 0;
    for (std::size_t first = 0; first < total; first += kMaiContextImportMaxBatchRecords) {
        std::vector<MaiContextImportRecord> page;
        const auto end = std::min(total, first + kMaiContextImportMaxBatchRecords);
        for (std::size_t index = first; index < end; ++index)
            page.push_back({"row-" + std::to_string(index), "Quoted row " + std::to_string(index),
                            static_cast<std::int64_t>(index)});
        const auto appended = store.appendContextImport(id, sequence++, page);
        CHECK(appended && appended.value().recordCount == end);
        CHECK(store.listMessagesPage(session, {}, 1).empty());
    }
    CHECK(store.commitContextImport(id));
    std::set<std::string> ids;
    std::string before;
    std::size_t remaining = total;
    for (;;) {
        const auto page = store.listMessagesPage(session, before, 200);
        CHECK(page.size() <= 200);
        if (page.empty()) break;
        for (const auto& message : page) {
            CHECK(remaining != 0);
            if (remaining == 0) break;
            --remaining;
            CHECK(ids.insert(message.id).second);
            CHECK(message.text() == "Quoted row " + std::to_string(remaining));
        }
        before = page.back().id;
    }
    CHECK(remaining == 0 && ids.size() == total);
}

void sqliteRestartAndRollback() {
    OwnedDatabase database;
    auto store = sqliteStore(database.path());
    if (!store) return;
    const auto session = createSession(*store);
    const auto ordinary = ordinaryMessage("Persisted ordinary row");
    store->putMessage(session, ordinary);
    const auto old = store->beginContextImport(spec(session, "committed"));
    CHECK(old);
    if (!old) return;
    CHECK(store->appendContextImport(old.value().importId, 0, {{"old", "Visible old source", 10}}));
    CHECK(store->commitContextImport(old.value().importId));
    auto staging = store->beginContextImport(spec(session, "staged-before-restart"));
    CHECK(staging);
    if (!staging) return;
    CHECK(store->appendContextImport(staging.value().importId, 0,
                                     {{"pending", "Must never publish after restart", 20}}));
    const auto abandonedId = staging.value().importId;
    store.reset();
    store = sqliteStore(database.path());
    if (!store) return;
    CHECK(texts(store->listMessages(session)) ==
          std::vector<std::string>({ordinary.text(), "Visible old source"}));
    CHECK(!store->commitContextImport(abandonedId));
    staging = store->beginContextImport(spec(session, "replacement-after-restart"));
    CHECK(staging);
    if (!staging) return;

    sqlite3* injection = nullptr;
    CHECK(sqlite3_open(database.path().c_str(), &injection) == SQLITE_OK);
    if (!injection) return;
    const auto execute = [&](const char* sql) {
        return sqlite3_exec(injection, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    };
    CHECK(
        execute("CREATE TRIGGER owned_import_failure BEFORE INSERT ON messages "
                "WHEN NEW.created=888888 BEGIN SELECT RAISE(ABORT,'owned import failure'); END"));
    const std::vector<MaiContextImportRecord> batch{{"first", "Would be partial", 777777},
                                                    {"second", "Injected error", 888888}};
    CHECK(!store->appendContextImport(staging.value().importId, 0, batch));
    const auto progress = store->inspectContextImport(staging.value().importId);
    CHECK(progress && progress.value().nextSequence == 0 && progress.value().recordCount == 0);
    CHECK(texts(store->listMessages(session)) ==
          std::vector<std::string>({ordinary.text(), "Visible old source"}));
    CHECK(execute("DROP TRIGGER owned_import_failure"));
    CHECK(store->appendContextImport(staging.value().importId, 0, batch));
    sqlite3_stmt* hiddenStatement = nullptr;
    CHECK(sqlite3_prepare_v2(injection, "SELECT id FROM messages WHERE created=777777", -1,
                             &hiddenStatement, nullptr) == SQLITE_OK);
    if (hiddenStatement) {
        CHECK(sqlite3_step(hiddenStatement) == SQLITE_ROW);
        const auto* bytes = sqlite3_column_text(hiddenStatement, 0);
        const std::string hiddenId = bytes ? reinterpret_cast<const char*>(bytes) : "";
        CHECK(!hiddenId.empty());
        MaiMessage invisible;
        CHECK(!store->getMessage(session, hiddenId, invisible));
        sqlite3_finalize(hiddenStatement);
    }

    // 指针切换失败也必须保留整个旧快照，不能先隐藏旧源再返回错误。
    const auto oldVisible = store->listMessages(session);
    MaiContextCheckpoint checkpoint;
    checkpoint.sessionId = session;
    checkpoint.revision = 1;
    checkpoint.sourceMessageCount = oldVisible.size();
    checkpoint.firstMessageId = oldVisible.front().id;
    checkpoint.throughMessageId = oldVisible.back().id;
    checkpoint.sourceFingerprint =
        maiContextSourceFingerprint(session, oldVisible, oldVisible.size());
    checkpoint.summary = R"({"source_notes":["owned fixture"]})";
    checkpoint.references = "[]";
    checkpoint.model = "fixture";
    CHECK(!store->advanceContextCheckpoint(session, 0, checkpoint));
    CHECK(
        execute("CREATE TRIGGER owned_checkpoint_failure BEFORE DELETE ON context_checkpoints "
                "BEGIN SELECT RAISE(ABORT,'owned checkpoint failure'); END"));
    CHECK(!store->commitContextImport(staging.value().importId));
    CHECK(texts(store->listMessages(session)) == texts(oldVisible));
    MaiContextCheckpoint retained;
    CHECK(store->getContextCheckpoint(session, retained));
    CHECK(execute("DROP TRIGGER owned_checkpoint_failure"));
    sqlite3_close(injection);
    CHECK(store->commitContextImport(staging.value().importId));
    CHECK(texts(store->listMessages(session)) ==
          std::vector<std::string>({ordinary.text(), "Would be partial", "Injected error"}));
    store.reset();
    store = sqliteStore(database.path());
    CHECK(store && store->listMessages(session).size() == 3);
}

class ObservedModel final : public MaiModelClient {
public:
    std::shared_ptr<std::atomic<bool>> destroyed;
    ~ObservedModel() override {
        if (destroyed) destroyed->store(true);
    }
    bool blockFirst = false;
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        std::unique_lock<std::mutex> lock(mMutex);
        mRequests.push_back(request);
        mChanged.notify_all();
        if (blockFirst && mRequests.size() == 1)
            while (!mReleased && !cancel.load())
                mChanged.wait_for(lock, std::chrono::milliseconds(5));
        lock.unlock();
        if (cancel.load()) return {MaiErrorCode::Canceled, "Owned model canceled"};
        if (sink.onText) sink.onText("Owned model answer");
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    bool waitForRequest() {
        std::unique_lock<std::mutex> lock(mMutex);
        return mChanged.wait_for(lock, std::chrono::seconds(3),
                                 [this] { return !mRequests.empty(); });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mMutex);
        mReleased = true;
        mChanged.notify_all();
    }
    std::vector<MaiModelRequest> requests() const {
        std::lock_guard<std::mutex> lock(mMutex);
        return mRequests;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    bool mReleased = false;
    std::vector<MaiModelRequest> mRequests;
};

bool requestContains(const MaiModelRequest& request, const std::string& exact) {
    return std::any_of(request.messages.begin(), request.messages.end(), [&](const auto& message) {
        return message.role == MaiModelRole::User && message.content == exact &&
               message.invocations.empty() && message.reasoning.empty();
    });
}

void agentNoRunAndLiveContext(std::unique_ptr<MaiSessionStore> store) {
    auto model = std::make_unique<ObservedModel>();
    const auto observed = model.get();
    const auto session = createSession(*store);
    MaiAgent agent(std::move(store), std::move(model));
    std::vector<MaiEventType> events;
    const auto token =
        agent.eventBus().subscribe([&](const MaiEvent& event) { events.push_back(event.type); });
    const auto begun = agent.beginContextImport(spec(session, "no-run"));
    CHECK(begun);
    if (!begun) {
        agent.eventBus().unsubscribe(token);
        return;
    }
    const std::string incoming = "Quoted friend: earlier question";
    const std::string outgoing = "Quoted owner: earlier human response";
    CHECK(agent.appendContextImport(begun.value().importId, 0,
                                    {{"incoming", incoming, 1}, {"outgoing", outgoing, 2}}));
    CHECK(observed->requests().empty());
    CHECK(events.empty());
    CHECK(agent.listMessages(session).empty());
    CHECK(agent.commitContextImport(begun.value().importId));
    CHECK(observed->requests().empty());
    CHECK(events == std::vector<MaiEventType>{MaiEventType::ContextImported});
    CHECK(agent.commitContextImport(begun.value().importId));
    CHECK(events.size() == 1);
    MaiSession loaded;
    CHECK(agent.getSession(session, loaded) && loaded.title == "Owned import fixture");
    agent.eventBus().unsubscribe(token);
    CHECK(agent.submit(MaiSendPrompt{session, "Now process this live input"}));
    agent.waitIdle();
    const auto requests = observed->requests();
    CHECK(requests.size() == 1);
    if (requests.size() == 1) {
        CHECK(requestContains(requests.front(), incoming));
        CHECK(requestContains(requests.front(), outgoing));
        CHECK(requestContains(requests.front(), "Now process this live input"));
    }
}

void busyCommitIsRetryable(std::unique_ptr<MaiSessionStore> store) {
    auto model = std::make_unique<ObservedModel>();
    model->blockFirst = true;
    auto* observed = model.get();
    const auto session = createSession(*store);
    MaiAgent agent(std::move(store), std::move(model));
    CHECK(agent.submit(MaiSendPrompt{session, "Existing independent turn"}));
    CHECK(observed->waitForRequest());
    const auto begun = agent.beginContextImport(spec(session, "while-busy"));
    CHECK(begun);
    if (!begun) {
        observed->release();
        agent.waitIdle();
        return;
    }
    CHECK(agent.appendContextImport(begun.value().importId, 0,
                                    {{"older", "Imported after independent turn", 1}}));
    const auto rejected = agent.commitContextImport(begun.value().importId);
    CHECK(!rejected && rejected.error().code() == MaiErrorCode::Busy);
    CHECK(agent.listMessages(session).size() == 2);
    const auto runningRequest = observed->requests();
    CHECK(runningRequest.size() == 1);
    if (!runningRequest.empty())
        CHECK(!requestContains(runningRequest.front(), "Imported after independent turn"));
    observed->release();
    agent.waitIdle();
    CHECK(agent.commitContextImport(begun.value().importId));
    CHECK(agent.submit(MaiSendPrompt{session, "Next live turn"}));
    agent.waitIdle();
    const auto requests = observed->requests();
    CHECK(requests.size() == 2);
    if (requests.size() == 2)
        CHECK(requestContains(requests.back(), "Imported after independent turn"));
}

// 卡在真正的存储提交入口，证明 Agent 只保留会话预占，不持全局 mutex 等待 I/O。
// 无关专业任务接口明确无任务，避免本用例意外创建模型以外的执行链。
class CommitGateStore final : public MaiSessionStore {
public:
    explicit CommitGateStore(std::unique_ptr<MaiSessionStore> store, bool blockBegin = false,
                             bool blockPoll = false)
        : mStore(std::move(store)), mBlockBegin(blockBegin), mBlockPoll(blockPoll) {}
    void putSession(const MaiSession& value) override {
        mStore->putSession(value);
    }
    bool getSession(const std::string& id, MaiSession& value) const override {
        return mStore->getSession(id, value);
    }
    std::vector<MaiSession> listSessions() const override {
        return mStore->listSessions();
    }
    bool removeSession(const std::string& id) override {
        return mStore->removeSession(id);
    }
    bool clearMessages(const std::string& id) override {
        return mStore->clearMessages(id);
    }
    void putMessage(const std::string& id, const MaiMessage& value) override {
        mStore->putMessage(id, value);
    }
    std::vector<MaiMessage> listMessages(const std::string& id) const override {
        return mStore->listMessages(id);
    }
    bool getMessage(const std::string& id, const std::string& message,
                    MaiMessage& value) const override {
        return mStore->getMessage(id, message, value);
    }
    std::vector<MaiMessage> listMessagesPage(const std::string& id, const std::string& before,
                                             std::size_t limit) const override {
        return mStore->listMessagesPage(id, before, limit);
    }
    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutate) override {
        return mStore->mutateSession(id, mutate);
    }
    void recoverInterruptedTools(const std::string& error, std::int64_t completed) override {
        mStore->recoverInterruptedTools(error, completed);
    }
    MaiError lastWriteError() const override {
        return mStore->lastWriteError();
    }
    MaiResult<MaiContextImportProgress> beginContextImport(
        const MaiContextImportSpec& value) override {
        if (mBlockBegin) {
            std::unique_lock<std::mutex> lock(mMutex);
            mEntered = true;
            mChanged.notify_all();
            mChanged.wait(lock, [this] { return mReleased; });
        }
        return mStore->beginContextImport(value);
    }
    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& id, std::uint64_t sequence,
        const std::vector<MaiContextImportRecord>& records) override {
        return mStore->appendContextImport(id, sequence, records);
    }
    MaiResult<MaiContextImportProgress> inspectContextImport(const std::string& id) const override {
        return mStore->inspectContextImport(id);
    }
    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& id) override {
        if (mBlockBegin || mBlockPoll) return mStore->commitContextImport(id);
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mEntered = true;
            mChanged.notify_all();
            mChanged.wait(lock, [this] { return mReleased; });
        }
        return mStore->commitContextImport(id);
    }
    MaiError abortContextImport(const std::string& id) override {
        return mStore->abortContextImport(id);
    }
    bool waitEntered() {
        std::unique_lock<std::mutex> lock(mMutex);
        return mChanged.wait_for(lock, std::chrono::seconds(3), [this] { return mEntered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mMutex);
        mReleased = true;
        mChanged.notify_all();
    }
    MaiError insertSpecialistTask(const MaiSpecialistTask&) override {
        return unsupported();
    }
    bool getSpecialistTask(const std::string&, const std::string&,
                           MaiSpecialistTask&) const override {
        return false;
    }
    MaiError deleteCompletedSpecialistTask(const std::string&, const std::string&) override {
        return unsupported();
    }
    MaiError appendSpecialistText(const std::string&, const std::string&, const std::string&,
                                  MaiMillis) override {
        return unsupported();
    }
    MaiError updateSpecialistProgress(const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }
    MaiError recoverStatusCheckFailures() override {
        return {};
    }
    MaiError finishSpecialistTask(const std::string&, const std::string&, MaiSpecialistTaskStatus,
                                  const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }
    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t) const override {
        if (mBlockPoll) {
            ++pollCalls;
            std::unique_lock<std::mutex> lock(mMutex);
            mEntered = true;
            mChanged.notify_all();
            mChanged.wait(lock, [this] { return mReleased; });
        }
        return {};
    }
    mutable std::atomic<unsigned> pollCalls{0};
    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t) const override {
        return {};
    }
    MaiResult<std::string> reserveSpecialistNotification(const std::string&, const std::string&,
                                                         const std::string&, MaiMillis) override {
        return unsupported();
    }
    MaiError markSpecialistNotified(const std::string&, const std::string&, const std::string&,
                                    MaiMillis) override {
        return unsupported();
    }
    MaiError markSpecialistResultAlreadyDelivered(const std::string&, const std::string&,
                                                  MaiMillis) override {
        return unsupported();
    }

private:
    static MaiError unsupported() {
        return {MaiErrorCode::NotSupported, "Owned commit gate has no specialist tasks"};
    }
    std::unique_ptr<MaiSessionStore> mStore;
    mutable std::mutex mMutex;
    mutable std::condition_variable mChanged;
    mutable bool mEntered = false;
    mutable bool mReleased = false;
    const bool mBlockBegin;
    const bool mBlockPoll;
};

void commitExcludesConcurrentSend(bool rejectWhenBusy) {
    auto store = std::make_unique<CommitGateStore>(makeMaiMemoryStore());
    auto* gate = store.get();
    const auto session = createSession(*store);
    auto model = std::make_unique<ObservedModel>();
    auto* observed = model.get();
    MaiAgent::Options options;
    options.rejectWhenBusy = rejectWhenBusy;
    MaiAgent agent(std::move(store), std::move(model), nullptr, options);
    const auto begun = agent.beginContextImport(spec(session, "commit-gate"));
    CHECK(begun);
    if (!begun) return;
    CHECK(
        agent.appendContextImport(begun.value().importId, 0, {{"quoted", "Committed context", 1}}));
    auto commit = std::async(std::launch::async,
                             [&] { return agent.commitContextImport(begun.value().importId); });
    const bool entered = gate->waitEntered();
    CHECK(entered);
    if (!entered) {
        gate->release();
        commit.wait();
        return;
    }
    auto send = std::async(std::launch::async, [&] {
        return agent.submit(MaiSendPrompt{session, "Concurrent ordinary input"});
    });
    const bool sendReturnedBeforeStorage =
        send.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    CHECK(sendReturnedBeforeStorage);
    // 无论断言结果如何均先释放测试闸门，避免失败用例自己挂住后续验收。
    gate->release();
    const auto sent = send.get();
    CHECK(!sent && sent.error().code() == MaiErrorCode::Busy);
    CHECK(commit.get());
    agent.waitIdle();
    CHECK(observed->requests().empty());
    CHECK(texts(agent.listMessages(session)) == std::vector<std::string>{"Committed context"});
}

void copiedWorkerUsesOrdinaryStoreOffThread(std::unique_ptr<MaiSessionStore> store) {
    const auto session = createSession(*store);
    auto model = std::make_unique<ObservedModel>();
    auto* observed = model.get();
    MaiAgent agent(std::move(store), std::move(model));
    const auto worker = agent.contextImportWorker();
    const auto copied = worker;
    auto imported = std::async(std::launch::async, [copied, session] {
        MaiThread::setCurrentName("import-test");
        const auto begun = copied.beginContextImport(spec(session, "owned-worker"));
        if (!begun) return begun;
        const auto appended = copied.appendContextImport(
            begun.value().importId, 0,
            {{"a", "Quoted worker first", 1}, {"b", "Quoted worker second", 2}});
        if (!appended) return appended;
        return copied.commitContextImport(begun.value().importId);
    });
    CHECK(imported.get());
    CHECK(texts(agent.listMessages(session)) ==
          std::vector<std::string>({"Quoted worker first", "Quoted worker second"}));
    CHECK(observed->requests().empty());
    const auto purged = copied.purgeContextImports(session, 1);
    CHECK(purged && purged.value().removed == 0 && !purged.value().hasMore);
}

void idleWorkerDoesNotRetainFacadeOrModelAndRejectsClosedCalls() {
    auto store = makeMaiMemoryStore();
    const auto session = createSession(*store);
    const auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto model = std::make_unique<ObservedModel>();
    model->destroyed = destroyed;
    auto agent = std::make_unique<MaiAgent>(std::move(store), std::move(model));
    auto worker = agent->contextImportWorker();
    const auto copied = worker;
    agent.reset();
    CHECK(destroyed->load());
    const auto begin = copied.beginContextImport(spec(session, "closed-owner"));
    CHECK(!begin && begin.error().code() == MaiErrorCode::Canceled);
    const auto append = copied.appendContextImport("old", 0, {{"a", "No late append", 1}});
    CHECK(!append && append.error().code() == MaiErrorCode::Canceled);
    const auto commit = copied.commitContextImport("old");
    CHECK(!commit && commit.error().code() == MaiErrorCode::Canceled);
    CHECK(copied.abortContextImport("old").code() == MaiErrorCode::Canceled);
    const auto purge = copied.purgeContextImports(session, 1);
    CHECK(!purge && purge.error().code() == MaiErrorCode::Canceled);
    MaiContextImportWorker empty;
    CHECK(!empty.beginContextImport(spec(session, "empty-capability")));
}

void destroyedOwnerDoesNotWaitForInflightSqlOrEmitLateImport() {
    auto store = std::make_unique<CommitGateStore>(makeMaiMemoryStore());
    auto* gate = store.get();
    const auto session = createSession(*store);
    auto agent = std::make_unique<MaiAgent>(std::move(store), std::make_unique<ObservedModel>());
    std::atomic<unsigned> importedEvents{0};
    agent->eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type == MaiEventType::ContextImported) ++importedEvents;
    });
    const auto worker = agent->contextImportWorker();
    const auto begun = worker.beginContextImport(spec(session, "destruction-gate"));
    CHECK(begun);
    if (!begun) return;
    CHECK(worker.appendContextImport(begun.value().importId, 0,
                                     {{"a", "Inflight hidden source", 1}}));
    auto commit = std::async(std::launch::async, [worker, id = begun.value().importId] {
        MaiThread::setCurrentName("import-commit");
        return worker.commitContextImport(id);
    });
    const bool entered = gate->waitEntered();
    CHECK(entered);
    if (!entered) {
        gate->release();
        commit.wait();
        return;
    }
    auto closed = std::async(std::launch::async, [&] {
        MaiThread::setCurrentName("import-close");
        agent.reset();
    });
    const bool destroyedBeforeStorage =
        closed.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    CHECK(destroyedBeforeStorage);
    if (destroyedBeforeStorage) {
        closed.get();
        const auto late = worker.beginContextImport(spec(session, "cannot-revive"));
        CHECK(!late && late.error().code() == MaiErrorCode::Canceled);
        CHECK(importedEvents.load() == 0);
    }
    // 先释放真实存储闸门，再读取所有结果，失败路径也不把测试变成永久等待。
    gate->release();
    CHECK(commit.get());
    if (!destroyedBeforeStorage) closed.get();
    CHECK(importedEvents.load() == 0);
}

void ownerCloseWaitsOnlyForAnEnteredImportEventHandoff() {
    auto store = makeMaiMemoryStore();
    const auto session = createSession(*store);
    auto agent = std::make_unique<MaiAgent>(std::move(store), std::make_unique<ObservedModel>());
    const auto worker = agent->contextImportWorker();
    const auto begun = worker.beginContextImport(spec(session, "event-handoff"));
    CHECK(begun);
    if (!begun) return;
    CHECK(worker.appendContextImport(begun.value().importId, 0,
                                     {{"a", "Visible before event handoff", 1}}));
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    agent->eventBus().subscribe([&](const MaiEvent& event) {
        if (event.type != MaiEventType::ContextImported) return;
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; });
    });
    auto commit = std::async(std::launch::async, [worker, id = begun.value().importId] {
        MaiThread::setCurrentName("import-event");
        return worker.commitContextImport(id);
    });
    bool reached = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        reached = changed.wait_for(lock, std::chrono::seconds(1), [&] { return entered; });
    }
    CHECK(reached);
    auto closed = std::async(std::launch::async, [&] {
        MaiThread::setCurrentName("event-close");
        agent.reset();
    });
    const bool waitingForEnteredCallback =
        closed.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
    CHECK(waitingForEnteredCallback);
    CHECK(commit.get());
    closed.get();
    const auto late = worker.commitContextImport(begun.value().importId);
    CHECK(!late && late.error().code() == MaiErrorCode::Canceled);
}

void lateBeginCannotLeaveOrphanStagingAfterReplacementStartup() {
    OwnedDatabase database;
    auto store = std::make_unique<CommitGateStore>(sqliteStore(database.path()), true);
    auto* gate = store.get();
    const auto session = createSession(*store);
    auto agent = std::make_unique<MaiAgent>(std::move(store), std::make_unique<ObservedModel>());
    const auto worker = agent->contextImportWorker();
    auto begin = std::async(std::launch::async, [worker, session] {
        MaiThread::setCurrentName("late-import");
        return worker.beginContextImport(spec(session, "late-owner"));
    });
    const bool entered = gate->waitEntered();
    CHECK(entered);
    if (!entered) {
        gate->release();
        begin.wait();
        return;
    }
    auto closed = std::async(std::launch::async, [&] {
        MaiThread::setCurrentName("late-close");
        agent.reset();
    });
    const bool destroyedBeforeStorage =
        closed.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    CHECK(destroyedBeforeStorage);
    if (destroyedBeforeStorage) closed.get();
    // 新连接先完成启动清理，旧 worker 才开始真正的 BEGIN/INSERT。
    auto replacement = sqliteStore(database.path());
    gate->release();
    const auto late = begin.get();
    if (!destroyedBeforeStorage) closed.get();
    CHECK(!late && late.error().code() == MaiErrorCode::Canceled);
    CHECK(replacement);
    if (replacement) {
        const auto current = replacement->beginContextImport(spec(session, "replacement-owner"));
        CHECK(current);
        if (current) CHECK(!replacement->abortContextImport(current.value().importId));
    }
}

void shutdownNotificationDuringPollCannotBeLost() {
    auto store = std::make_unique<CommitGateStore>(makeMaiMemoryStore(), false, true);
    auto* gate = store.get();
    auto agent = std::make_unique<MaiAgent>(std::move(store), nullptr);
    const auto worker = agent->contextImportWorker();
    CHECK(gate->waitEntered());
    std::promise<void> started;
    auto closed = std::async(std::launch::async, [&] {
        MaiThread::setCurrentName("poll-close");
        started.set_value();
        agent.reset();
    });
    started.get_future().wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool revoked = false;
    do {
        const auto attempt = worker.beginContextImport({});
        revoked = !attempt && attempt.error().code() == MaiErrorCode::Canceled;
        if (!revoked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!revoked && std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gate->release();
    const bool prompt = closed.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    closed.get();
    CHECK(revoked && prompt);
}

void ordinaryTaskNotificationStillWakesTheSpecialistPoll() {
    auto store = std::make_unique<CommitGateStore>(makeMaiMemoryStore(), false, true);
    auto* gate = store.get();
    MaiAgent agent(std::move(store), std::make_unique<ObservedModel>());
    CHECK(gate->waitEntered());
    gate->release();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto created = agent.submit(MaiCreateSession{"", "Ordinary poll wake", ""});
    CHECK(created);
    if (!created) return;
    CHECK(agent.submit(MaiSendPrompt{created.value(), "Owned ordinary task"}));
    agent.waitIdle();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (gate->pollCalls.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(gate->pollCalls.load() >= 2);
}

}  // namespace

int main() {
    std::puts("Context import: memory and SQLite staged-source contract");
    for (int sqlite = 0; sqlite != 2; ++sqlite) {
        std::printf("Backend: %s\n", sqlite ? "SQLite" : "memory");
        auto store = sqlite ? sqliteStore() : makeMaiMemoryStore();
        if (!store) continue;
        stagingAndChunkRetries(*store);
        replacementAbortAndCheckpoint(*store);
        limitsAndManyPages(*store);
        agentNoRunAndLiveContext(sqlite ? sqliteStore() : makeMaiMemoryStore());
        busyCommitIsRetryable(sqlite ? sqliteStore() : makeMaiMemoryStore());
        copiedWorkerUsesOrdinaryStoreOffThread(sqlite ? sqliteStore() : makeMaiMemoryStore());
    }
    sqliteRestartAndRollback();
    commitExcludesConcurrentSend(true);
    commitExcludesConcurrentSend(false);
    idleWorkerDoesNotRetainFacadeOrModelAndRejectsClosedCalls();
    destroyedOwnerDoesNotWaitForInflightSqlOrEmitLateImport();
    ownerCloseWaitsOnlyForAnEnteredImportEventHandoff();
    lateBeginCannotLeaveOrphanStagingAfterReplacementStartup();
    shutdownNotificationDuringPollCannotBeLost();
    ordinaryTaskNotificationStillWakesTheSpecialistPoll();
    std::printf("Context import failures: %d\n", failures);
    return failures ? 1 : 0;
}
