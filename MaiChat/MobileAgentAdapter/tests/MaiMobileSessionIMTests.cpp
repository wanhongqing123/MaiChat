#include "MaiMobileSessionIM.h"
#include "MaiAgent.h"
#include "MaiAgentSendMediaTool.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"
#include "MaiPathGuard.h"
#include "MaiTool.h"
#include <json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sqlite3.h>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
#define CHECK(value)                                    \
    do {                                                \
        if (!(value)) throw std::runtime_error(#value); \
    } while (false)

namespace {

struct ModelState {
    std::atomic<int> calls{0};
    bool media = false;
    bool fail = false;
    std::atomic<bool> hold{false};
    std::mutex mutex;
    std::vector<MaiModelRequest> requests;
};

class ScriptedModel final : public MaiModelClient {
public:
    explicit ScriptedModel(std::shared_ptr<ModelState> state) : state_(std::move(state)) {}
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        const int call = ++state_->calls;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->requests.push_back(request);
        }
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (state_->hold && !cancel && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (state_->media && call == 1) {
            if (sink.onToolCall)
                sink.onToolCall({"owned-media", "agent_send_media",
                                 R"({"file_path":"owned.png","type":"image","caption":"owned caption"})"});
            return {};
        }
        if (sink.onReasoning) sink.onReasoning("PRIVATE-REASONING");
        if (sink.onText) sink.onText(state_->media ? "owned final body" : "reply:" + request.messages.back().content);
        return state_->fail ? MaiError{MaiErrorCode::Network, "PRIVATE-PROVIDER-ERROR"} : MaiError{};
    }

private:
    std::shared_ptr<ModelState> state_;
};

// 在真实普通存储读取处阻塞，让纯控制撤销确定地发生在投影读取期间。
class ProjectionGateStore final : public MaiSessionStore {
public:
    explicit ProjectionGateStore(std::unique_ptr<MaiSessionStore> store) : mStore(std::move(store)) {}
    void putSession(const MaiSession& value) override {
        mStore->putSession(value);
    }
    bool getSession(const std::string& id, MaiSession& value) const override {
        return mStore->getSession(id, value);
    }
    std::vector<MaiSession> listSessions() const override {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mArmed) {
                mEntered = true;
                mChanged.notify_all();
                mChanged.wait_for(lock, std::chrono::seconds(3), [this] { return mReleased; });
            }
        }
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
    bool getMessage(const std::string& id, const std::string& message, MaiMessage& value) const override {
        return mStore->getMessage(id, message, value);
    }
    std::vector<MaiMessage> listMessagesPage(const std::string& id, const std::string& before,
                                             std::size_t limit) const override {
        return mStore->listMessagesPage(id, before, limit);
    }
    bool mutateSession(const std::string& id, const std::function<void(MaiSession&)>& mutate) override {
        return mStore->mutateSession(id, mutate);
    }
    void recoverInterruptedTools(const std::string& error, std::int64_t completed) override {
        mStore->recoverInterruptedTools(error, completed);
    }
    MaiError lastWriteError() const override {
        return mStore->lastWriteError();
    }
    MaiResult<MaiContextImportProgress> beginContextImport(const MaiContextImportSpec& value) override {
        return mStore->beginContextImport(value);
    }
    MaiResult<MaiContextImportProgress> appendContextImport(
        const std::string& id, std::uint64_t sequence, const std::vector<MaiContextImportRecord>& records) override {
        return mStore->appendContextImport(id, sequence, records);
    }
    MaiResult<MaiContextImportProgress> inspectContextImport(const std::string& id) const override {
        return mStore->inspectContextImport(id);
    }
    MaiResult<MaiContextImportProgress> commitContextImport(const std::string& id) override {
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mCommitArmed) {
                mCommitEntered = true;
                mChanged.notify_all();
                mChanged.wait_for(lock, std::chrono::seconds(3), [this] { return mCommitReleased; });
            }
        }
        return mStore->commitContextImport(id);
    }
    void armCommit() {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommitArmed = true;
        mCommitEntered = false;
        mCommitReleased = false;
    }
    bool waitCommitEntered() {
        std::unique_lock<std::mutex> lock(mMutex);
        return mChanged.wait_for(lock, std::chrono::seconds(2), [this] { return mCommitEntered; });
    }
    void releaseCommit() {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommitReleased = true;
        mChanged.notify_all();
    }
    MaiError abortContextImport(const std::string& id) override {
        return mStore->abortContextImport(id);
    }
    void arm() {
        std::lock_guard<std::mutex> lock(mMutex);
        mArmed = true;
        mEntered = false;
        mReleased = false;
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
    bool getSpecialistTask(const std::string&, const std::string&, MaiSpecialistTask&) const override {
        return false;
    }
    MaiError deleteCompletedSpecialistTask(const std::string&, const std::string&) override {
        return unsupported();
    }
    MaiError appendSpecialistText(const std::string&, const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }
    MaiError updateSpecialistProgress(const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }
    MaiError recoverStatusCheckFailures() override {
        return {};
    }
    MaiError finishSpecialistTask(const std::string&, const std::string&, MaiSpecialistTaskStatus, const std::string&,
                                  const std::string&, MaiMillis) override {
        return unsupported();
    }
    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t) const override {
        return {};
    }
    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t) const override {
        return {};
    }
    MaiResult<std::string> reserveSpecialistNotification(const std::string&, const std::string&, const std::string&,
                                                         MaiMillis) override {
        return unsupported();
    }
    MaiError markSpecialistNotified(const std::string&, const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }
    MaiError markSpecialistResultAlreadyDelivered(const std::string&, const std::string&, MaiMillis) override {
        return unsupported();
    }

private:
    static MaiError unsupported() {
        return {MaiErrorCode::NotSupported, "Owned projection gate has no specialist tasks"};
    }
    std::unique_ptr<MaiSessionStore> mStore;
    mutable std::mutex mMutex;
    mutable std::condition_variable mChanged;
    mutable bool mEntered = false;
    mutable bool mArmed = false;
    mutable bool mReleased = false;
    bool mCommitArmed = false;
    bool mCommitEntered = false;
    bool mCommitReleased = false;
};

struct Fixture {
    MaiFilePath directory;
    std::shared_ptr<ModelState> model = std::make_shared<ModelState>();
    std::unique_ptr<MaiAgent> agent;
    std::unique_ptr<MaiMobileSessionIM> bridge;
    Json activation;
    std::string session;
    sqlite3* historyWriter = nullptr;
    std::string historyPath;
    Json initialTask;
    std::vector<std::string> readerIds;
    std::string ownerId = "owner";
    std::string peerId = "peer";
    std::string corePath;
    explicit Fixture(std::uint64_t readiness = 120000, std::uint64_t handoff = 30000,
                     std::function<void(const std::string&)> configured = {}, bool bootstrap = true,
                     std::size_t identityLength = 0, std::unique_ptr<MaiSessionStore> store = {},
                     bool sqliteCore = false) {
        if (identityLength) {
            ownerId.assign(identityLength, 'o');
            peerId.assign(identityLength, 'p');
        }
        static std::atomic<unsigned> next{0};
        directory = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-mobile-im-owned-" + std::to_string(Clock::now().time_since_epoch().count()) +
                                  "-" + std::to_string(++next)));
        CHECK(!MaiFileSystem::createDirectories(directory));
        if (sqliteCore && !store) {
            corePath = directory.append(MaiFilePath::fromUtf8("owned-core.sqlite")).toUtf8();
            auto opened = makeMaiSqliteStore(corePath);
            CHECK(opened);
            store = std::move(opened.value());
        }
        historyPath = directory.append(MaiFilePath::fromUtf8("owned-history.sqlite")).toUtf8();
        CHECK(sqlite3_open(historyPath.c_str(), &historyWriter) == SQLITE_OK);
        execute(
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE messages(id TEXT PRIMARY KEY,from_user TEXT,to_user TEXT,peer TEXT,"
            "direction INTEGER,status INTEGER,text TEXT,created_at INTEGER,has_image INTEGER DEFAULT 0,"
            "image_path TEXT,image_w INTEGER,image_h INTEGER,image_bytes INTEGER,has_voice INTEGER DEFAULT 0,"
            "voice_path TEXT,voice_seconds INTEGER,has_file INTEGER DEFAULT 0,file_path TEXT,file_name TEXT,"
            "file_mime TEXT,file_bytes INTEGER,has_video INTEGER DEFAULT 0,video_path TEXT,video_name TEXT,"
            "video_cover TEXT,video_seconds INTEGER,video_bytes INTEGER,caption_above INTEGER DEFAULT 0,"
            "quote_msg_id TEXT,quote_sender TEXT,quote_digest TEXT,quote_kind TEXT,approval_token TEXT);"
            "CREATE INDEX messages_peer_page ON messages(peer,created_at,id);");
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(makeMaiAgentSendMediaTool());
        agent = std::make_unique<MaiAgent>(store ? std::move(store) : makeMaiMemoryStore(),
                                           std::make_unique<ScriptedModel>(model), std::move(tools));
        const auto created = agent->submit(MaiCreateSession{directory.toUtf8(), "Owned root", "owned-model"});
        CHECK(created);
        session = created.value();
        bridge =
            std::make_unique<MaiMobileSessionIM>(*agent, directory.toUtf8(), std::move(configured), readiness, handoff);
        activation = call({{"action", "activate"},
                           {"sdk_app_id", "123"},
                           {"owner_id", ownerId},
                           {"auth_generation", "1"},
                           {"friends", Json::array({peerId, "other"})}});
        CHECK(activation.value("ok", false));
        const auto bound = call(scoped({{"action", "bind"}, {"peer_id", peerId}, {"session_id", session}}));
        CHECK(bound.value("ok", false) && bound.at("history_tasks").size() == 1);
        initialTask = bound.at("history_tasks").at(0);
        if (bootstrap) complete(initialTask);
    }
    ~Fixture() {
        if (bridge) bridge->invalidate();
        if (agent) agent->waitIdle();
        bridge.reset();
        agent.reset();
        for (const auto& id : readerIds)
            MaiMobileSessionIM::historySnapshot(Json{{"action", "close"}, {"snapshot_id", id}}.dump());
        if (historyWriter) sqlite3_close(historyWriter);
        // 只删除本次明确创建的系统临时子目录，不触碰用户文件或共享构建目录。
        if (!maiResolvePathWithinRoot(MaiFileSystem::temporaryDirectory().toUtf8(), directory.toUtf8()).empty())
            MaiFileSystem::removeRecursively(directory);
    }
    void execute(const std::string& sql) {
        CHECK(sqlite3_exec(historyWriter, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    Json source() const {
        return {{"profile", "desktop"}, {"path", historyPath}, {"provenance", "owner_only_legacy"}};
    }
    Json call(Json request) {
        if (request.value("action", "") == "bind" && !request.contains("expected_workspace_key"))
            request["expected_workspace_key"] = MaiFileSystem::resolve(directory).toUtf8();
        if (request.value("action", "") == "activate" && !request.contains("history_source"))
            request["history_source"] = source();
        return Json::parse(bridge->request(request.dump()));
    }
    void insertHistory(const std::string& id, const std::string& body, std::int64_t time, bool outgoing = false) {
        sqlite3_stmt* statement = nullptr;
        CHECK(sqlite3_prepare_v2(historyWriter,
                                 "INSERT INTO messages(id,from_user,to_user,peer,direction,status,text,created_at) "
                                 "VALUES(?1,?2,?3,?4,?5,2,?6,?7)",
                                 -1, &statement, nullptr) == SQLITE_OK);
        sqlite3_bind_text(statement, 1, id.c_str(), int(id.size()), SQLITE_TRANSIENT);
        const auto& sender = outgoing ? ownerId : peerId;
        const auto& recipient = outgoing ? peerId : ownerId;
        sqlite3_bind_text(statement, 2, sender.c_str(), int(sender.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, recipient.c_str(), int(recipient.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, peerId.c_str(), int(peerId.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, outgoing ? 1 : 0);
        sqlite3_bind_text(statement, 6, body.c_str(), int(body.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 7, time);
        const auto result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        CHECK(result == SQLITE_DONE);
    }
    Json open(const Json& task) {
        auto result = Json::parse(MaiMobileSessionIM::historySnapshot(Json{{"action", "open"}, {"task", task}}.dump()));
        CHECK(result.value("ok", false));
        readerIds.push_back(result.at("snapshot_id"));
        const auto started =
            call(scoped({{"action", "history_opened"}, {"task", task}, {"snapshot_id", result.at("snapshot_id")}}));
        CHECK(started.value("ok", false));
        return result;
    }
    Json read(const Json& opened) {
        return Json::parse(MaiMobileSessionIM::historySnapshot(
            Json{{"action", "read"}, {"snapshot_id", opened.at("snapshot_id")}}.dump()));
    }
    Json chunk(const Json& task, const Json& page) {
        return call(scoped({{"action", "history_chunk"}, {"task", task}, {"page", page}}));
    }
    void complete(const Json& task) {
        const auto opened = open(task);
        while (true) {
            const auto page = read(opened);
            CHECK(page.value("ok", false));
            const auto response = chunk(task, page);
            CHECK(response.value("ok", false) && response.value("page_accepted", false));
            if (!page.at("has_more").get<bool>()) break;
        }
        CHECK(Json::parse(MaiMobileSessionIM::historySnapshot(
                              Json{{"action", "close"}, {"snapshot_id", opened.at("snapshot_id")}}.dump()))
                  .value("ok", false));
    }
    void cleanup() {
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            const auto result = pump();
            CHECK(result.value("ok", false));
            if (!result.value("history_cleanup_pending", false)) return;
        }
        throw std::runtime_error("Owned cleanup did not finish within bounded pump cycles");
    }
    std::int64_t coreCount(const std::string& sql) const {
        sqlite3* reader = nullptr;
        CHECK(sqlite3_open_v2(corePath.c_str(), &reader, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
        sqlite3_stmt* statement = nullptr;
        const int prepared = sqlite3_prepare_v2(reader, sql.c_str(), -1, &statement, nullptr);
        if (prepared != SQLITE_OK) {
            sqlite3_close(reader);
            throw std::runtime_error("Owned Core count failed");
        }
        const int result = sqlite3_step(statement);
        const auto count = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
        sqlite3_finalize(statement);
        sqlite3_close(reader);
        CHECK(result == SQLITE_ROW);
        return count;
    }
    void restartBackend() {
        CHECK(!corePath.empty());
        bridge->invalidate();
        agent->waitIdle();
        bridge.reset();
        agent.reset();
        for (const auto& id : readerIds)
            MaiMobileSessionIM::historySnapshot(Json{{"action", "close"}, {"snapshot_id", id}}.dump());
        readerIds.clear();
        auto reopened = makeMaiSqliteStore(corePath);
        CHECK(reopened);
        auto tools = std::make_unique<MaiToolRegistry>();
        tools->add(makeMaiAgentSendMediaTool());
        agent = std::make_unique<MaiAgent>(std::move(reopened.value()), std::make_unique<ScriptedModel>(model),
                                           std::move(tools));
        bridge = std::make_unique<MaiMobileSessionIM>(*agent, directory.toUtf8());
        activation = call({{"action", "activate"},
                           {"sdk_app_id", "123"},
                           {"owner_id", ownerId},
                           {"auth_generation", "1"},
                           {"friends", Json::array({peerId, "other"})}});
        CHECK(activation.value("ok", false));
    }
    Json scoped(Json request) const {
        request["bridge_id"] = activation.at("bridge_id");
        request["scope"] = activation.at("scope");
        return request;
    }
    Json list() {
        return call(scoped({{"action", "list"}}));
    }
    Json metadata(const std::string& id) {
        const auto current = list();
        const auto& binding = current.at("bindings").at(0);
        return scoped({{"action", "admit"},
                       {"peer_id", "peer"},
                       {"message_id", id},
                       {"sender_id", "peer"},
                       {"recipient_id", "owner"},
                       {"expected_binding_epoch", binding.at("epoch")},
                       {"expected_session_id", binding.at("session_id")},
                       {"origin", "human"},
                       {"live", true},
                       {"group", false},
                       {"control_frame", false},
                       {"has_content", true}});
    }
    Json rawAdmit(Json metadata) {
        return Json::parse(MaiMobileSessionIM::control(metadata.dump()));
    }
    Json persisted(const Json& admitted, const std::string& row, bool inserted) {
        return Json::parse(MaiMobileSessionIM::control(scoped({{"action", "persisted_input"},
                                                               {"input_id", admitted.at("input_id")},
                                                               {"source_row_id", row},
                                                               {"inserted", inserted}})
                                                           .dump()));
    }
    Json admit(Json metadata) {
        const auto admitted = rawAdmit(metadata);
        if (admitted.value("accepted", false)) {
            const auto id = metadata.at("message_id").get<std::string>();
            insertHistory(id, "owned live metadata", 1000000 + std::int64_t(sqlite3_total_changes(historyWriter)));
            CHECK(persisted(admitted, id, true).value("accepted", false));
        }
        return admitted;
    }
    Json ready(Json metadata, const Json& admitted, const std::string& body) {
        metadata["action"] = "receive";
        metadata["input_id"] = admitted.at("input_id");
        metadata["text"] = body;
        return call(metadata);
    }
    Json pump() {
        return call(scoped({{"action", "pump"}}));
    }
    Json delivery() {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        do {
            const auto result = pump();
            CHECK(result.value("ok", false));
            if (!result.at("deliveries").empty()) return result.at("deliveries").at(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < deadline);
        throw std::runtime_error("delivery timed out");
    }
};

Json control(const char* action, const Json& delivery) {
    return Json::parse(MaiMobileSessionIM::control(Json{{"action", action}, {"delivery", delivery}}.dump()));
}

void exactReceiptBodyAndOneShotClaimAck() {
    Fixture f;
    const auto metadata = f.metadata("sdk-source-one");
    const auto admitted = f.admit(metadata);
    CHECK(admitted.value("accepted", false));
    CHECK(f.ready(metadata, admitted, "first").value("accepted", false));
    const auto delivery = f.delivery();
    CHECK(delivery.at("text") == "reply:first");
    CHECK(delivery.at("source_message_id") == "sdk-source-one");
    CHECK(delivery.at("input_id") == admitted.at("input_id"));
    CHECK(delivery.at("turn_id") == delivery.at("assistant_message_id"));
    CHECK(delivery.dump().find("PRIVATE-REASONING") == std::string::npos);
    MaiMessage user, assistant;
    CHECK(f.agent->getMessage(f.session, delivery.at("user_message_id"), user) && user.role == MaiRole::User);
    CHECK(f.agent->getMessage(f.session, delivery.at("assistant_message_id"), assistant) &&
          assistant.role == MaiRole::Assistant && assistant.completed);
    CHECK(control("claim", delivery).value("allowed", false));
    CHECK(!control("claim", delivery).value("allowed", true));
    CHECK(control("ack", delivery).value("acknowledged", false));
    CHECK(!control("ack", delivery).value("acknowledged", true));
    CHECK(f.model->calls == 1);
}

void originalAdmissionOrderWaitsForMissingMediaAndAllDispatchAcks() {
    Fixture f;
    const auto a = f.metadata("media-arrived-first");
    const auto first = f.admit(a);
    const auto b = f.metadata("text-arrived-second");
    const auto second = f.admit(b);
    CHECK(f.ready(b, second, "second").value("accepted", false));
    CHECK(f.pump().value("ok", false));
    CHECK(f.model->calls == 0 && f.agent->listMessages(f.session).empty());
    CHECK(f.ready(a, first, "first ready").value("accepted", false));
    const auto one = f.delivery();
    CHECK(one.at("source_message_id") == "media-arrived-first");
    CHECK(f.pump().at("deliveries").size() == 1 && f.model->calls == 1);
    CHECK(control("claim", one).value("allowed", false));
    CHECK(control("ack", one).value("acknowledged", false));
    const auto two = f.delivery();
    CHECK(two.at("source_message_id") == "text-arrived-second");
    CHECK(two.at("text") == "reply:second");
}

void abandonAndReadinessDeadlineAdvanceOnlyLaterInput() {
    Fixture f(50);
    const auto a = f.metadata("missing-callback");
    const auto first = f.admit(a);
    const auto b = f.metadata("later-ready");
    const auto second = f.admit(b);
    CHECK(f.ready(b, second, "later").value("accepted", false));
    CHECK(f.pump().value("ok", false));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto next = f.delivery();
    CHECK(next.at("source_message_id") == "later-ready");
    CHECK(!f.ready(a, first, "too late").value("ok", true));
    CHECK(f.model->calls == 1);
}

void rebindRejectsOriginalImportedPayload() {
    Fixture f;
    const auto original = f.metadata("importing");
    const auto admitted = f.admit(original);
    const auto created = f.agent->submit(MaiCreateSession{f.directory.toUtf8(), "Replacement", "owned-model"});
    CHECK(created);
    CHECK(f.call(f.scoped({{"action", "bind"}, {"peer_id", "peer"}, {"session_id", created.value()}}))
              .value("ok", false));
    CHECK(!f.ready(original, admitted, "old import").value("ok", true));
    CHECK(!f.admit(original).value("accepted", true));
    CHECK(f.pump().at("deliveries").empty() && f.model->calls == 0);
}

void replacementHighWaterNeverRevivesAnOldLiveAdapter() {
    Fixture old;
    const auto m = old.metadata("old-delivery");
    const auto a = old.admit(m);
    CHECK(old.ready(m, a, "old").value("accepted", false));
    const auto ticket = old.delivery();
    Fixture current;
    CHECK(!control("claim", ticket).value("allowed", false));
    auto late = Json{{"action", "activate"},
                     {"sdk_app_id", "123"},
                     {"owner_id", "owner"},
                     {"auth_generation", "2"},
                     {"friends", Json::array({"peer"})}};
    CHECK(!old.call(late).value("ok", true));
    CHECK(current.list().value("ok", false));
    auto scope = current.activation.at("scope");
    auto refresh = late;
    refresh["auth_generation"] = "2";
    CHECK(current.call(refresh).value("ok", false));
    CHECK(!current.list().value("ok", true));
    current.activation = current.call(refresh);
    CHECK(current.activation.value("ok", false));
    CHECK(current.activation.at("scope").at("controller_generation") == scope.at("controller_generation"));
    CHECK(!current
               .call(Json{{"action", "activate"},
                          {"sdk_app_id", "123"},
                          {"owner_id", "different"},
                          {"auth_generation", "3"},
                          {"friends", Json::array()}})
               .value("ok", true));
}

void exactRefreshDoesNotFeedbackLoopAndChangeCallbackCanUsePureControl() {
    Fixture f;
    int notifications = 0;
    f.bridge->setChangeHandler([&](const std::string&) { ++notifications; });
    const auto before = f.list().at("bindings");
    const auto refreshed = f.call(Json{{"action", "activate"},
                                       {"sdk_app_id", "123"},
                                       {"owner_id", "owner"},
                                       {"auth_generation", "1"},
                                       {"friends", Json::array({"peer", "other"})}});
    CHECK(refreshed.at("bindings") == before && notifications == 0);
    f.bridge->setChangeHandler([&](const std::string& session) {
        ++notifications;
        if (!session.empty()) {
            const auto result = Json::parse(
                MaiMobileSessionIM::control(f.scoped({{"action", "takeover"}, {"session_id", session}}).dump()));
            CHECK(result.value("ok", false));
        }
    });
    CHECK(f.call(f.scoped({{"action", "bind"}, {"peer_id", "peer"}, {"session_id", f.session}})).value("ok", false));
    CHECK(notifications == 0);
    const auto replacement = f.agent->submit(MaiCreateSession{f.directory.toUtf8(), "Changed binding", "owned-model"});
    CHECK(replacement);
    CHECK(f.call(f.scoped({{"action", "bind"}, {"peer_id", "peer"}, {"session_id", replacement.value()}}))
              .value("ok", false));
    CHECK(notifications == 1 && f.list().at("bindings").empty());
}

void missingCredentialAndVisionPolicyDropTheInputWithoutRetry() {
    int credentialCalls = 0;
    Fixture f(120000, 30000, [&](const std::string&) {
        ++credentialCalls;
        throw std::runtime_error("PRIVATE-CREDENTIAL");
    });
    const auto m = f.metadata("bad-model");
    const auto admitted = f.admit(m);
    CHECK(f.ready(m, admitted, "do not retry").value("accepted", false));
    const auto result = f.pump();
    CHECK(result.value("ok", false));
    CHECK(result.at("pending_count") == 0 && !result.at("warnings").empty());
    CHECK(f.pump().value("ok", false) && credentialCalls == 1 && f.model->calls == 0);
    Fixture vision;
    int policyCalls = 0;
    vision.bridge->setInputValidator([&](const std::string&, bool hasVision) {
        ++policyCalls;
        if (hasVision) throw std::runtime_error("owned text-only route");
    });
    const auto image = vision.directory.append(MaiFilePath::fromUtf8("input.png"));
    CHECK(!MaiFileSystem::writeFile(image, "owned"));
    auto source = vision.metadata("unsupported-vision");
    const auto input = vision.admit(source);
    source["action"] = "receive";
    source["input_id"] = input.at("input_id");
    source["text"] = "image";
    source["images"] = Json::array({{{"path", image.toUtf8()}, {"mimeType", "image/png"}}});
    CHECK(vision.call(source).value("accepted", false));
    CHECK(vision.pump().at("pending_count") == 0 && policyCalls == 1 && vision.model->calls == 0);
}

void outputHandoffExpiresAndStaleDestroyedDeliveryCannotSend() {
    Json late;
    {
        Fixture f(120000, 50);
        const auto m = f.metadata("expire-output");
        const auto a = f.admit(m);
        CHECK(f.ready(m, a, "one").value("accepted", false));
        late = f.delivery();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(!control("claim", late).value("allowed", true));
        const auto timedOut = f.pump();
        CHECK(!timedOut.at("warnings").empty());
        CHECK(timedOut.at("deliveries").empty());
    }
    CHECK(!control("claim", late).value("ok", true));
}

void unexpectedOrdinaryCoreInputAndWorkspaceChangeRevokeDelivery() {
    Fixture f;
    const auto m = f.metadata("before-local");
    const auto a = f.admit(m);
    CHECK(f.ready(m, a, "remote").value("accepted", false));
    const auto delivery = f.delivery();
    CHECK(f.agent->submit(MaiSendPrompt{f.session, "local bypassing native frontend"}));
    CHECK(!control("claim", delivery).value("allowed", true));
    CHECK(f.list().at("bindings").empty());
}

void modelToolAgentIdentityAndPostCancelEffectsStayHonest() {
    Fixture f;
    MaiToolContext context;
    context.sessionId = f.session;
    CHECK(f.bridge->executeBindingTool(R"({"action":"list"})", context).hasError());
    context.sessionAgent = f.agent.get();
    CHECK(!f.bridge->executeBindingTool(R"({"action":"list"})", context).hasError());
    std::atomic_bool canceled{true};
    context.cancel = &canceled;
    const auto before = f.bridge->executeBindingTool(R"({"action":"unbind","peer_id":"peer"})", context);
    CHECK(before.hasError() && before.execution() && before.execution()->started == false);
    canceled = false;
    f.bridge->setChangeHandler([&](const std::string&) { canceled = true; });
    const auto after = f.bridge->executeBindingTool(R"({"action":"unbind","peer_id":"peer"})", context);
    CHECK(after.hasError() && after.execution() && after.execution()->started == true);
    CHECK(after.execution()->effectStatus == MaiToolEffectStatus::Unknown &&
          after.execution()->nextAction == MaiToolNextAction::InspectState);
}

void lateSessionErrorDeniesAnAlreadyPreparedDelivery() {
    Fixture f;
    const auto metadata = f.metadata("late-error");
    const auto admitted = f.admit(metadata);
    CHECK(f.ready(metadata, admitted, "completed body").value("accepted", false));
    const auto delivery = f.delivery();
    MaiEvent failed;
    failed.type = MaiEventType::SessionError;
    failed.sessionId = f.session;
    failed.detail = "PRIVATE-LATE-ERROR";
    f.agent->eventBus().publish(failed);
    CHECK(!control("claim", delivery).value("allowed", true));
}

void persistedImageAndVideoPathsSurviveWorkspaceMigration() {
    Fixture f;
    const auto image = f.directory.append(MaiFilePath::fromUtf8("input.png"));
    const auto video = f.directory.append(MaiFilePath::fromUtf8("input.mp4"));
    CHECK(!MaiFileSystem::writeFile(image, "owned image bytes"));
    CHECK(!MaiFileSystem::writeFile(video, "owned video bytes"));
    auto metadata = f.metadata("relative-persistence");
    const auto admitted = f.admit(metadata);
    metadata["action"] = "receive";
    metadata["input_id"] = admitted.at("input_id");
    metadata["text"] = "inspect";
    metadata["images"] = Json::array({{{"path", image.toUtf8()}, {"mimeType", "image/png"}}});
    metadata["videos"] = Json::array({{{"path", video.toUtf8()}, {"mimeType", "video/mp4"}}});
    CHECK(f.call(metadata).value("accepted", false));
    CHECK(f.pump().value("ok", false));
    f.agent->waitIdle();
    std::vector<std::string> paths;
    for (const auto& message : f.agent->listMessages(f.session))
        if (message.role == MaiRole::User) {
            for (const auto& part : message.parts) {
                if (const auto* value = std::get_if<MaiImagePart>(&part.body)) paths.push_back(value->path);
                if (const auto* value = std::get_if<MaiVideoPart>(&part.body)) paths.push_back(value->path);
            }
        }
    CHECK(paths.size() == 2 && paths[0] == "input.png" && paths[1] == "input.mp4");
    const auto migrated = f.directory.append(MaiFilePath::fromUtf8("new-container-workspace"));
    CHECK(!MaiFileSystem::createDirectories(migrated));
    CHECK(!MaiFileSystem::writeFile(migrated.append(MaiFilePath::fromUtf8("input.png")), "owned image bytes"));
    CHECK(!MaiFileSystem::writeFile(migrated.append(MaiFilePath::fromUtf8("input.mp4")), "owned video bytes"));
    CHECK(f.agent->submit(MaiUpdateSession{f.session, "", "", "", migrated.toUtf8()}));
    MaiSession reopened;
    CHECK(f.agent->getSession(f.session, reopened));
    for (const auto& path : paths) {
        CHECK(!MaiFilePath::fromUtf8(path).isAbsolute());
        const auto resolved = maiResolvePathWithinRoot(reopened.directory, path);
        CHECK(!resolved.empty() && MaiFileSystem::exists(MaiFilePath::fromUtf8(resolved)));
        CHECK(resolved == MaiFileSystem::resolve(migrated.append(MaiFilePath::fromUtf8(path))).toUtf8());
    }
}

void historyStreamsAllRowsAtomicallyBeforeOneLiveTurn() {
    Fixture f(120000, 30000, {}, false);
    for (int index = 0; index < 450; ++index) {
        const auto suffix = std::to_string(index);
        f.insertHistory("history-" + std::string(4 - suffix.size(), '0') + suffix, "historical-body-" + suffix,
                        index / 3, index % 2 != 0);
    }
    CHECK(f.initialTask.at("import_id") == "");
    const auto opened = f.open(f.initialTask);
    f.insertHistory("outside-pinned-snapshot", "not in original snapshot", 0);
    auto first = f.read(opened);
    CHECK(first.at("records").size() == 200);
    auto response = f.chunk(f.initialTask, first);
    CHECK(response.value("ok", false) && response.at("history_progress").at("record_count") == "200");
    CHECK(f.agent->listMessages(f.session).empty() && f.model->calls == 0);
    CHECK(f.chunk(f.initialTask, first).at("history_progress").at("record_count") == "200");
    auto tampered = first;
    tampered["records"][0]["text"] = "forged historical instruction";
    CHECK(!f.chunk(f.initialTask, tampered).value("ok", true));
    while (first.at("has_more").get<bool>()) {
        first = f.read(opened);
        response = f.chunk(f.initialTask, first);
        CHECK(response.value("ok", false) && response.value("page_accepted", false));
    }
    CHECK(response.at("history_progress").at("committed") == true);
    CHECK(response.at("history_progress").at("record_count") == "450");
    CHECK(f.model->calls == 0 && f.list().at("bindings")[0].at("history_state") == "ready");
    const auto stored = f.agent->listMessages(f.session);
    CHECK(stored.size() == 450);
    for (std::size_t index = 0; index < stored.size(); ++index) {
        CHECK(stored[index].role == MaiRole::User && stored[index].completed && !stored[index].pendingInput);
        CHECK(stored[index].created == std::int64_t(index / 3));
        CHECK(stored[index].text().find("historical-body-" + std::to_string(index)) != std::string::npos);
        CHECK(stored[index].text().find("approval_token") == std::string::npos);
    }
    const auto metadata = f.metadata("one-live-after-history");
    const auto admitted = f.admit(metadata);
    CHECK(f.ready(metadata, admitted, "current live input").value("accepted", false));
    CHECK(f.delivery().at("text") == "reply:current live input");
    CHECK(f.model->calls == 1);
    {
        std::lock_guard<std::mutex> lock(f.model->mutex);
        CHECK(std::any_of(
            f.model->requests.front().messages.begin(), f.model->requests.front().messages.end(),
            [](const auto& message) { return message.content.find("historical-body-449") != std::string::npos; }));
    }
}

void historyWaitsForMetadataThenKeepsReplayOrSkipsExactlyOneNewLive() {
    {
        Fixture f(120000, 30000, {}, false);
        f.insertHistory("persisted-replay", "old record must remain context", 1);
        const auto opened = f.open(f.initialTask);
        const auto page = f.read(opened);
        const auto metadata = f.metadata("persisted-replay");
        const auto admitted = f.rawAdmit(metadata);
        CHECK(admitted.value("accepted", false));
        const auto deferred = f.chunk(f.initialTask, page);
        CHECK(deferred.value("ok", false) && !deferred.value("page_accepted", true));
        CHECK(deferred.at("reason") == "awaiting_ingress_metadata" && f.read(opened) == page);
        CHECK(f.persisted(admitted, "persisted-replay", false).at("reason") == "persisted_duplicate");
        const auto imported = f.chunk(f.initialTask, page);
        CHECK(imported.value("ok", false) && imported.at("history_progress").at("record_count") == "1");
        CHECK(f.pump().at("deliveries").empty() && f.model->calls == 0);
        CHECK(f.agent->listMessages(f.session).size() == 1 && f.list().at("bindings").size() == 1);
    }
    {
        Fixture f(120000, 30000, {}, false);
        const auto metadata = f.metadata("new-live-in-snapshot");
        const auto admitted = f.rawAdmit(metadata);
        CHECK(admitted.value("accepted", false));
        f.insertHistory("new-live-in-snapshot", "raw metadata", 1);
        const auto opened = f.open(f.initialTask);
        const auto page = f.read(opened);
        CHECK(!f.chunk(f.initialTask, page).value("page_accepted", true));
        CHECK(f.persisted(admitted, "new-live-in-snapshot", true).value("accepted", false));
        CHECK(f.ready(metadata, admitted, "prepared live body").value("accepted", false));
        CHECK(f.pump().at("deliveries").empty() && f.model->calls == 0);
        const auto imported = f.chunk(f.initialTask, page);
        CHECK(imported.value("ok", false) && imported.at("history_progress").at("record_count") == "0");
        CHECK(imported.at("history_progress").at("live_overlap_count") == "1");
        CHECK(f.agent->listMessages(f.session).empty());
        CHECK(f.delivery().at("text") == "reply:prepared live body" && f.model->calls == 1);
    }
}

void historyRevocationOverflowAndPreparationFailureNeverBecomeReady() {
    {
        Fixture f(120000, 30000, {}, false);
        const auto opened = f.open(f.initialTask);
        auto mutated = f.initialTask;
        mutated["import_id"] = "mutated-task";
        CHECK(!f.call(f.scoped(
                          {{"action", "history_opened"}, {"task", mutated}, {"snapshot_id", opened.at("snapshot_id")}}))
                   .value("ok", true));
        CHECK(Json::parse(
                  MaiMobileSessionIM::control(f.scoped({{"action", "takeover"}, {"session_id", f.session}}).dump()))
                  .value("ok", false));
        CHECK(!f.read(opened).value("ok", true));
        CHECK(f.pump().at("bindings").empty() && f.agent->listMessages(f.session).empty());
    }
    {
        Fixture f(120000, 30000, {}, false);
        for (int index = 0; index < 32; ++index)
            CHECK(f.admit(f.metadata("burst-" + std::to_string(index))).value("accepted", false));
        const auto rejected = f.rawAdmit(f.metadata("overflow"));
        CHECK(!rejected.value("ok", true) && rejected.value("history_aborted", false));
        CHECK(f.pump().at("bindings").empty() && f.model->calls == 0);
    }
    {
        Fixture f(1, 30000, {}, false);
        CHECK(f.admit(f.metadata("vanished-preparation")).value("accepted", false));
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        CHECK(f.pump().at("bindings").empty() && f.model->calls == 0);
    }
}

void historySourceFailureAndModelToolBindRemainPendingWithoutHostReentry() {
    Fixture f;
    const auto original = f.list().at("bindings");
    auto missingPin = f.scoped({{"action", "bind"}, {"peer_id", "peer"}, {"session_id", f.session}});
    CHECK(!Json::parse(f.bridge->request(missingPin.dump())).value("ok", true));
    auto wrongPin = missingPin;
    wrongPin["expected_workspace_key"] = "/wrong-owned-workspace";
    CHECK(!f.call(wrongPin).value("ok", true) && f.list().at("bindings") == original);
    CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", "peer"}})).value("ok", false));
    MaiToolContext context;
    context.sessionId = f.session;
    context.sessionAgent = f.agent.get();
    int changes = 0;
    f.bridge->setChangeHandler([&](const std::string&) { ++changes; });
    const auto tool = f.bridge->executeBindingTool(
        Json{{"action", "bind"}, {"peer_id", "peer"}, {"session_id", f.session}}.dump(), context);
    CHECK(!tool.hasError() && changes == 1 && f.model->calls == 0);
    const auto projected = f.pump();
    CHECK(projected.at("history_tasks").size() == 1);
    const auto task = projected.at("history_tasks")[0];
    CHECK(task.at("import_id") == "");
    CHECK(f.list().at("history_tasks").empty());
    CHECK(f.call(f.scoped({{"action", "history_failed"}, {"task", task}, {"reason", "owned source failure"}}))
              .value("ok", false));
    CHECK(f.list().at("bindings").empty() && f.agent->listMessages(f.session).empty());
}

void historySelfBindStagesDuringCallerTurnAndCommitsOnlyAfterIdle() {
    Fixture f;
    CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", "peer"}})).value("ok", false));
    f.insertHistory("historical-before-caller", "context staged by running caller", 1);
    f.model->hold = true;
    struct Release {
        std::shared_ptr<ModelState> model;
        ~Release() {
            model->hold = false;
        }
    } release{f.model};
    CHECK(f.agent->submit(MaiSendPrompt{f.session, "independent current caller"}));
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (!f.model->calls && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(f.model->calls == 1 && f.agent->isBusy(f.session));
    MaiToolContext context;
    context.sessionId = f.session;
    context.sessionAgent = f.agent.get();
    CHECK(!f.bridge
               ->executeBindingTool(Json{{"action", "bind"}, {"peer_id", "peer"}, {"session_id", f.session}}.dump(),
                                    context)
               .hasError());
    const auto task = f.pump().at("history_tasks").at(0);
    const auto opened = f.open(task);
    const auto page = f.read(opened);
    const auto staged = f.chunk(task, page);
    CHECK(staged.value("ok", false) && staged.value("page_accepted", false));
    CHECK(!staged.at("history_progress").at("committed").get<bool>());
    CHECK(f.agent->listMessages(f.session).size() == 2 && f.model->calls == 1);
    f.model->hold = false;
    f.agent->waitIdle();
    CHECK(f.pump().at("bindings")[0].at("history_state") == "ready");
    CHECK(f.agent->listMessages(f.session).size() == 3 && f.model->calls == 1);
}

void historyFormattedPageSplitsCoreBudgetWithoutTruncation() {
    Fixture f(120000, 30000, {}, false, 256);
    for (int index = 0; index < 200; ++index) {
        const auto suffix = std::to_string(index);
        f.insertHistory("long-" + std::string(3 - suffix.size(), '0') + suffix, std::string(800, 'x') + suffix, index,
                        index % 2 != 0);
    }
    const auto opened = f.open(f.initialTask);
    const auto page = f.read(opened);
    CHECK(page.at("records").size() == 200 && !page.at("has_more").get<bool>());
    const auto imported = f.chunk(f.initialTask, page);
    CHECK(imported.value("ok", false) && imported.value("page_accepted", false));
    CHECK(imported.at("history_progress").at("record_count") == "200");
    CHECK(imported.at("history_progress").at("next_sequence") == "1");
    const auto records = f.agent->listMessages(f.session);
    CHECK(records.size() == 200);
    std::size_t totalText = 0;
    for (std::size_t index = 0; index < records.size(); ++index) {
        totalText += records[index].text().size();
        CHECK(records[index].text().find(std::string(800, 'x') + std::to_string(index)) != std::string::npos);
    }
    // 一个合法 Reader 页确实超过 Core 单批上限，因此必须分块而不能裁剪正文。
    CHECK(totalText > kMaiContextImportMaxBatchBytes && f.model->calls == 0);
}

void projectionRechecksTakeoverAfterOrdinarySessionRead() {
    auto store = std::make_unique<ProjectionGateStore>(makeMaiMemoryStore());
    auto* gate = store.get();
    Fixture f(120000, 30000, {}, true, 0, std::move(store));
    gate->arm();
    auto projected = std::async(std::launch::async, [&] { return f.list(); });
    const bool entered = gate->waitEntered();
    Json revoked;
    if (entered)
        revoked = Json::parse(
            MaiMobileSessionIM::control(f.scoped({{"action", "takeover"}, {"session_id", f.session}}).dump()));
    gate->release();
    const auto result = projected.get();
    CHECK(entered && revoked.value("ok", false) && result.value("ok", false));
    CHECK(result.at("bindings").empty() && !result.at("has_bindings").get<bool>());
    CHECK(result.at("history_tasks").empty() && result.at("history_progress_all").empty());
    CHECK(f.model->calls == 0);
}

void historyCleanupReclaimsRebindAbortAndRestartPayloads() {
    Fixture f(120000, 30000, {}, true, 0, {}, true);
    CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", f.peerId}})).value("ok", false));
    CHECK(f.agent->submit(MaiSendPrompt{f.session, "Ordinary local messages survive cleanup"}));
    f.agent->waitIdle();
    CHECK(f.model->calls == 1);
    for (int index = 0; index < 450; ++index) {
        const auto suffix = std::to_string(index);
        f.insertHistory("gc-" + std::string(3 - suffix.size(), '0') + suffix, "owned persisted historical row", index,
                        index % 2 != 0);
    }
    const auto bind = [&] {
        const auto result = f.call(f.scoped({{"action", "bind"}, {"peer_id", f.peerId}, {"session_id", f.session}}));
        CHECK(result.value("ok", false) && result.at("history_tasks").size() == 1);
        return result.at("history_tasks")[0];
    };
    const auto checkPreserved = [&] {
        CHECK(f.coreCount("SELECT count(*) FROM messages") == 452);
        CHECK(f.coreCount("SELECT count(*) FROM messages WHERE context_import_id IS NULL") == 2);
        CHECK(f.agent->listMessages(f.session).size() == 452);
        CHECK(f.model->calls == 1);
        sqlite3_stmt* count = nullptr;
        CHECK(sqlite3_prepare_v2(f.historyWriter, "SELECT count(*) FROM messages", -1, &count, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int64(count, 0) == 450);
        sqlite3_finalize(count);
    };
    for (int round = 0; round < 4; ++round) {
        f.execute("UPDATE messages SET text='owned source refresh " + std::to_string(round) + "'");
        f.complete(bind());
        CHECK(f.list().value("history_cleanup_pending", false));
        f.cleanup();
        checkPreserved();
        CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", f.peerId}})).value("ok", false));
    }
    // 未发布的半页被撤销后，最后一个绑定已消失；cleanup 标志仍驱动普通 pump。
    auto task = bind();
    auto opened = f.open(task);
    auto page = f.read(opened);
    CHECK(page.at("has_more") == true && f.chunk(task, page).value("page_accepted", false));
    CHECK(f.coreCount("SELECT count(*) FROM messages") == 652);
    const auto unbound = f.call(f.scoped({{"action", "unbind"}, {"peer_id", f.peerId}}));
    CHECK(unbound.at("bindings").empty() && unbound.value("history_cleanup_pending", false));
    f.cleanup();
    checkPreserved();
    for (int restart = 0; restart < 2; ++restart) {
        task = bind();
        opened = f.open(task);
        page = f.read(opened);
        CHECK(f.chunk(task, page).value("page_accepted", false));
        CHECK(f.coreCount("SELECT count(*) FROM messages") == 652);
        // 模拟真实后端/SQLite 重开：启动作废的 stage0 由恢复扫描发现并有界回收。
        f.restartBackend();
        CHECK(f.activation.at("bindings").empty() && f.activation.value("history_cleanup_pending", false));
        f.cleanup();
        checkPreserved();
        f.complete(bind());
        f.cleanup();
        checkPreserved();
        CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", f.peerId}})).value("ok", false));
    }
}

void terminalHistoryPageCannotPublishAfterLiveReadinessExpired() {
    Fixture f(1, 30000, {}, false);
    const auto opened = f.open(f.initialTask);
    const auto terminal = f.read(opened);
    CHECK(terminal.at("records").empty() && !terminal.at("has_more").get<bool>());
    CHECK(f.admit(f.metadata("expired-before-eof")).value("accepted", false));
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto result = f.chunk(f.initialTask, terminal);
    CHECK(result.value("ok", false) && result.at("bindings").empty());
    CHECK(!result.at("history_progress").at("committed").get<bool>());
    const auto pumped = f.pump();
    CHECK(pumped.at("bindings").empty() && !pumped.at("warnings").empty());
    CHECK(f.model->calls == 0 && f.agent->listMessages(f.session).empty());
}

void deferredHistoryCommitChecksReadinessBeforePublishingAtIdle() {
    Fixture f(100);
    CHECK(f.call(f.scoped({{"action", "unbind"}, {"peer_id", "peer"}})).value("ok", false));
    f.model->hold = true;
    struct Release {
        std::shared_ptr<ModelState> model;
        ~Release() { model->hold = false; }
    } release{f.model};
    CHECK(f.agent->submit(MaiSendPrompt{f.session, "owned busy caller"}));
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (!f.model->calls && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(f.agent->isBusy(f.session));
    const auto bound = f.call(f.scoped({{"action", "bind"}, {"peer_id", "peer"}, {"session_id", f.session}}));
    CHECK(bound.value("ok", false) && bound.at("history_tasks").size() == 1);
    const auto task = bound.at("history_tasks")[0];
    const auto opened = f.open(task); const auto terminal = f.read(opened);
    CHECK(f.admit(f.metadata("expires-before-idle-commit")).value("accepted", false));
    const auto staged = f.chunk(task, terminal);
    CHECK(staged.value("ok", false) && staged.at("bindings")[0].at("history_state") == "pending");
    CHECK(!staged.at("history_progress").at("committed").get<bool>());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    f.model->hold = false; f.agent->waitIdle();
    const auto pumped = f.pump();
    CHECK(pumped.at("bindings").empty() && !pumped.at("warnings").empty());
    CHECK(f.model->calls == 1 && f.agent->listMessages(f.session).size() == 2);
}

void historyCommitCannotReleaseBarrierWhenLiveExpiresDuringStoreCommit() {
    auto store = std::make_unique<ProjectionGateStore>(makeMaiMemoryStore());
    auto* gate = store.get();
    Fixture f(500, 30000, {}, false, 0, std::move(store));
    f.insertHistory("prior-context", "complete context may publish under the started lease", 1);
    const auto opened = f.open(f.initialTask); const auto terminal = f.read(opened);
    CHECK(f.admit(f.metadata("expires-during-core-commit")).value("accepted", false));
    gate->armCommit();
    auto committed = std::async(std::launch::async, [&] { return f.chunk(f.initialTask, terminal); });
    const bool entered = gate->waitCommitEntered();
    if (entered) std::this_thread::sleep_for(std::chrono::milliseconds(550));
    gate->releaseCommit();
    const auto result = committed.get();
    CHECK(entered && result.value("ok", false));
    CHECK(result.at("history_progress").at("committed").get<bool>());
    CHECK(result.at("bindings").empty());
    const auto pumped = f.pump();
    CHECK(pumped.at("bindings").empty() && !pumped.at("warnings").empty());
    CHECK(f.model->calls == 0 && f.agent->listMessages(f.session).size() == 1);
}

void failedHistoryAbortRetainsPendingUntilWriteLockIsReleased() {
    Fixture f(120000, 30000, {}, false, 0, {}, true);
    for (int index = 0; index < 450; ++index) {
        const auto suffix = std::to_string(index);
        f.insertHistory("locked-" + std::string(3 - suffix.size(), '0') + suffix,
                        "owned history waiting for an abort retry", index);
    }
    const auto opened = f.open(f.initialTask);
    const auto page = f.read(opened);
    CHECK(page.at("has_more") == true && f.chunk(f.initialTask, page).value("page_accepted", false));
    CHECK(f.coreCount("SELECT count(*) FROM messages") == 200);
    std::promise<int> acquired;
    auto locked = acquired.get_future();
    auto writer = std::async(std::launch::async, [&] {
        sqlite3* connection = nullptr;
        int result = sqlite3_open_v2(f.corePath.c_str(), &connection, SQLITE_OPEN_READWRITE, nullptr);
        if (result == SQLITE_OK) result = sqlite3_exec(connection, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        acquired.set_value(result);
        if (result == SQLITE_OK) {
            // 使用普通独立连接真正占写锁，跨过 Core 的既有五秒 busy_timeout。
            std::this_thread::sleep_for(std::chrono::milliseconds(6500));
            result = sqlite3_exec(connection, "COMMIT", nullptr, nullptr, nullptr);
        }
        if (connection) sqlite3_close(connection);
        return result;
    });
    CHECK(locked.get() == SQLITE_OK);
    const auto began = Clock::now();
    const auto unbound = f.call(f.scoped({{"action", "unbind"}, {"peer_id", f.peerId}}));
    const auto unbindMillis = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
    CHECK(unbound.value("ok", false) && unbound.at("bindings").empty());
    CHECK(unbound.value("history_cleanup_pending", false));
    auto response = f.pump();
    CHECK(writer.get() == SQLITE_OK);
    const auto firstRows = f.coreCount("SELECT count(*) FROM messages");
    const bool pendingWasHonest = response.value("history_cleanup_pending", false) || firstRows == 0;
    for (unsigned cycle = 0; cycle < 30 && response.value("history_cleanup_pending", false); ++cycle) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        response = f.pump();
    }
    const auto finalRows = f.coreCount("SELECT count(*) FROM messages");
    std::cout << "ABORT_RETRY unbind_ms=" << unbindMillis << " first_rows=" << firstRows
              << " pending_honest=" << pendingWasHonest << " final_rows=" << finalRows << '\n';
    CHECK(pendingWasHonest && finalRows == 0 && !response.value("history_cleanup_pending", true));
    CHECK(f.agent->listMessages(f.session).empty() && f.model->calls == 0);
    sqlite3_stmt* count = nullptr;
    CHECK(sqlite3_prepare_v2(f.historyWriter, "SELECT count(*) FROM messages", -1, &count, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int64(count, 0) == 450);
    sqlite3_finalize(count);
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"exact receipts and claim ack", exactReceiptBodyAndOneShotClaimAck},
        {"original readiness FIFO", originalAdmissionOrderWaitsForMissingMediaAndAllDispatchAcks},
        {"readiness expiry", abandonAndReadinessDeadlineAdvanceOnlyLaterInput},
        {"rebind rejects imported payload", rebindRejectsOriginalImportedPayload},
        {"backend replacement high water", replacementHighWaterNeverRevivesAnOldLiveAdapter},
        {"activation idempotence and callback", exactRefreshDoesNotFeedbackLoopAndChangeCallbackCanUsePureControl},
        {"credential and vision parity", missingCredentialAndVisionPolicyDropTheInputWithoutRetry},
        {"handoff expiry and destroyed weak delivery", outputHandoffExpiresAndStaleDestroyedDeliveryCannotSend},
        {"Core bypass takeover", unexpectedOrdinaryCoreInputAndWorkspaceChangeRevokeDelivery},
        {"tool context and effect receipt", modelToolAgentIdentityAndPostCancelEffectsStayHonest}};
    auto allTests = tests;
    allTests.push_back({"late SessionError claim denial", lateSessionErrorDeniesAnAlreadyPreparedDelivery});
    allTests.push_back({"relative media workspace migration", persistedImageAndVideoPathsSurviveWorkspaceMigration});
    allTests.push_back({"history streamed atomic context", historyStreamsAllRowsAtomicallyBeforeOneLiveTurn});
    allTests.push_back(
        {"history replay and new-live metadata", historyWaitsForMetadataThenKeepsReplayOrSkipsExactlyOneNewLive});
    allTests.push_back(
        {"history revoke overflow expiry", historyRevocationOverflowAndPreparationFailureNeverBecomeReady});
    allTests.push_back(
        {"history tool pending and source error", historySourceFailureAndModelToolBindRemainPendingWithoutHostReentry});
    allTests.push_back({"history self bind waits idle", historySelfBindStagesDuringCallerTurnAndCommitsOnlyAfterIdle});
    allTests.push_back({"history formatted page Core budget", historyFormattedPageSplitsCoreBudgetWithoutTruncation});
    allTests.push_back({"projection takeover during Session read", projectionRechecksTakeoverAfterOrdinarySessionRead});
    allTests.push_back({"history GC rebind abort restart", historyCleanupReclaimsRebindAbortAndRestartPayloads});
    allTests.push_back({"history EOF rejects expired live", terminalHistoryPageCannotPublishAfterLiveReadinessExpired});
    allTests.push_back({"history deferred EOF checks expired live", deferredHistoryCommitChecksReadinessBeforePublishingAtIdle});
    allTests.push_back({"history expiry during Core commit", historyCommitCannotReleaseBarrierWhenLiveExpiresDuringStoreCommit});
    allTests.push_back({"history abort retries real SQLite lock", failedHistoryAbortRetainsPendingUntilWriteLockIsReleased});
    int failed = 0;
    for (const auto& test : allTests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& exception) {
            ++failed;
            std::cout << "FAIL " << test.first << ": " << exception.what() << '\n';
        }
    }
    std::cout << "TOTAL " << allTests.size() - failed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
