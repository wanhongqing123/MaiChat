// 文件作用：用自建历史与持久数据库验证检查点只读分页、版本绑定和旧字节保真。
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <json.hpp>
#include <sqlite3.h>

#include "MaiContextCheckpointRead.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiSqliteStore.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (false)

namespace {
using Json = nlohmann::json;

// 仅允许两个快照读取；任何原史扫描或写操作都留下计数，不能靠原史指纹误判存档。
class MaiCheckpointReadProbe final : public MaiSessionStore {
public:
    MaiContextCheckpoint snapshot;
    mutable std::size_t checkpointReads = 0;
    mutable std::size_t unrelatedCalls = 0;
    bool getSession(const std::string& id, MaiSession& out) const override {
        out.id = id;
        return true;
    }
    bool getContextCheckpoint(const std::string&, MaiContextCheckpoint& out) const override {
        ++checkpointReads;
        out = snapshot;
        return true;
    }
    MaiError lastWriteError() const override {
        return {};
    }
    void putSession(const MaiSession&) override {
        ++unrelatedCalls;
    }
    std::vector<MaiSession> listSessions() const override {
        ++unrelatedCalls;
        return {};
    }
    bool removeSession(const std::string&) override {
        ++unrelatedCalls;
        return false;
    }
    bool clearMessages(const std::string&) override {
        ++unrelatedCalls;
        return false;
    }
    void putMessage(const std::string&, const MaiMessage&) override {
        ++unrelatedCalls;
    }
    std::vector<MaiMessage> listMessages(const std::string&) const override {
        ++unrelatedCalls;
        return {};
    }
    bool getMessage(const std::string&, const std::string&, MaiMessage&) const override {
        ++unrelatedCalls;
        return false;
    }
    std::vector<MaiMessage> listMessagesPage(const std::string&, const std::string&,
                                             std::size_t) const override {
        ++unrelatedCalls;
        return {};
    }
    bool mutateSession(const std::string&, const std::function<void(MaiSession&)>&) override {
        ++unrelatedCalls;
        return false;
    }
    void recoverInterruptedTools(const std::string&, std::int64_t) override {
        ++unrelatedCalls;
    }
    MaiError insertSpecialistTask(const MaiSpecialistTask&) override {
        return unexpected();
    }
    bool getSpecialistTask(const std::string&, const std::string&,
                           MaiSpecialistTask&) const override {
        ++unrelatedCalls;
        return false;
    }
    MaiError deleteCompletedSpecialistTask(const std::string&, const std::string&) override {
        return unexpected();
    }
    MaiError appendSpecialistText(const std::string&, const std::string&, const std::string&,
                                  MaiMillis) override {
        return unexpected();
    }
    MaiError updateSpecialistProgress(const std::string&, const std::string&, MaiMillis) override {
        return unexpected();
    }
    MaiError recoverStatusCheckFailures() override {
        return unexpected();
    }
    MaiError finishSpecialistTask(const std::string&, const std::string&, MaiSpecialistTaskStatus,
                                  const std::string&, const std::string&, MaiMillis) override {
        return unexpected();
    }
    std::vector<MaiSpecialistTask> listActiveSpecialistTasks(std::size_t) const override {
        ++unrelatedCalls;
        return {};
    }
    std::vector<MaiSpecialistTask> listUnnotifiedSpecialistTasks(std::size_t) const override {
        ++unrelatedCalls;
        return {};
    }
    MaiResult<std::string> reserveSpecialistNotification(const std::string&, const std::string&,
                                                         const std::string&, MaiMillis) override {
        return unexpected();
    }
    MaiError markSpecialistNotified(const std::string&, const std::string&, const std::string&,
                                    MaiMillis) override {
        return unexpected();
    }
    MaiError markSpecialistResultAlreadyDelivered(const std::string&, const std::string&,
                                                  MaiMillis) override {
        return unexpected();
    }

private:
    MaiError unexpected() {
        ++unrelatedCalls;
        return {MaiErrorCode::Internal, "Unexpected checkpoint probe operation."};
    }
};

struct MaiOwnedCheckpointDatabase {
    MaiFilePath directory = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8("mai-checkpoint-read-" + MaiIdGenerator::newSessionId()));
    MaiOwnedCheckpointDatabase() {
        CHECK(!MaiFileSystem::createDirectories(directory));
    }
    ~MaiOwnedCheckpointDatabase() {
        MaiFileSystem::removeRecursively(directory);
    }
    std::string path() const {
        return directory.append(MaiFilePath::fromUtf8("fixture.sqlite")).toUtf8();
    }
};

std::unique_ptr<MaiSessionStore> sqliteStore(const std::string& path) {
    auto opened = makeMaiSqliteStore(path);
    CHECK(opened);
    return opened ? std::move(opened.value()) : nullptr;
}

std::string createSession(MaiSessionStore& store) {
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.title = "Owned checkpoint reader fixture";
    store.putSession(session);
    return session.id;
}

MaiMessage appendMessage(MaiSessionStore& store, const std::string& sessionId, int index) {
    MaiMessage message;
    message.id = MaiIdGenerator::newMessageId();
    message.role = index % 2 == 0 ? MaiRole::User : MaiRole::Assistant;
    message.created = index + 1;
    message.completed = index + 2;
    message.parts.push_back({MaiIdGenerator::newPartId(),
                             MaiTextPart{"Original fixture record " + std::to_string(index)},
                             index + 1});
    store.putMessage(sessionId, message);
    return message;
}

std::string summaryText() {
    return R"({ "goals":["a","b"], "constraints":[], "decisions":["c"], "progress":[1,2,3], "open_questions":["old"], "source_notes":["d"], "unknown":[1,2,3,4] })";
}

MaiContextCheckpoint checkpoint(MaiSessionStore& store, const std::string& sessionId,
                                std::size_t count = 1) {
    auto history = store.listMessages(sessionId);
    while (history.size() < count) {
        appendMessage(store, sessionId, static_cast<int>(history.size()));
        history = store.listMessages(sessionId);
    }
    MaiContextCheckpoint saved;
    saved.sessionId = sessionId;
    saved.revision = 1;
    saved.sourceMessageCount = count;
    saved.firstMessageId = history.front().id;
    saved.throughMessageId = history[count - 1].id;
    saved.sourceFingerprint = maiContextSourceFingerprint(sessionId, history, count);
    saved.summary = summaryText();
    saved.references = "[ ]";
    saved.created = 1795357941507;
    saved.model = "fixture-model";
    return saved;
}

std::string reconstruct(MaiSessionStore& store, const std::string& sessionId,
                        MaiContextCheckpointSection section, std::size_t maximum,
                        bool expectValidUtf8 = true) {
    MaiContextCheckpointReadQuery query;
    query.section = section;
    query.maxBytes = maximum;
    std::string reconstructed;
    bool encounteredInvalid = false;
    for (std::size_t pages = 0; pages < 100000; ++pages) {
        auto result = maiReadContextCheckpoint(store, sessionId, query);
        CHECK(result);
        if (!result) return reconstructed;
        const auto& page = result.value();
        CHECK(page.available);
        CHECK(page.section == section);
        CHECK(page.offsetBytes == reconstructed.size());
        CHECK(page.sourceBytesRead == page.text.size());
        CHECK(page.sourceBytesRead <= maximum);
        CHECK(page.totalBytes >= page.offsetBytes + page.sourceBytesRead);
        CHECK(page.hasMore == (page.offsetBytes + page.sourceBytesRead < page.totalBytes));
        CHECK(page.nextOffsetBytes == (page.hasMore ? page.offsetBytes + page.sourceBytesRead : 0));
        if (expectValidUtf8) CHECK(page.textIsValidUtf8);
        if (!page.textIsValidUtf8) encounteredInvalid = true;
        reconstructed += page.text;
        if (!page.hasMore) {
            if (!expectValidUtf8) CHECK(encounteredInvalid);
            return reconstructed;
        }
        CHECK(page.sourceBytesRead > 0);
        if (page.sourceBytesRead == 0) return reconstructed;
        query.offsetBytes = page.nextOffsetBytes;
        query.expectedRevision = page.info.revision;
    }
    CHECK(false);
    return reconstructed;
}

void checkReadOnlyHistory(MaiSessionStore& store, const std::string& sessionId,
                          const std::vector<MaiMessage>& original) {
    const auto current = store.listMessages(sessionId);
    CHECK(current.size() == original.size());
    if (current.size() != original.size()) return;
    for (std::size_t index = 0; index < original.size(); ++index) {
        CHECK(current[index].id == original[index].id);
        CHECK(current[index].role == original[index].role);
        CHECK(current[index].created == original[index].created);
        CHECK(current[index].completed == original[index].completed);
        CHECK(current[index].parts.size() == original[index].parts.size());
        CHECK(std::get<MaiTextPart>(current[index].parts.front().body).text ==
              std::get<MaiTextPart>(original[index].parts.front().body).text);
    }
}

void testStoredArchiveAndUncoveredTail(MaiSessionStore& store, const std::string& sessionId,
                                       const MaiContextCheckpoint& saved) {
    const auto original = store.listMessages(sessionId);
    MaiContextCheckpointReadQuery query;
    query.section = MaiContextCheckpointSection::Metadata;
    auto result = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(result);
    if (!result) return;
    const auto& page = result.value();
    CHECK(page.available && page.text.empty() && page.sourceBytesRead == 0);
    CHECK(page.totalBytes == 0 && !page.hasMore);
    CHECK(page.info.revision == saved.revision && page.info.created == saved.created);
    CHECK(page.info.model == saved.model);
    CHECK(page.info.sourceMessageCount == 134);
    CHECK(page.info.firstMessageId == saved.firstMessageId);
    CHECK(page.info.throughMessageId == saved.throughMessageId);
    CHECK(page.info.summaryBytes == saved.summary.size());
    CHECK(page.info.referenceBytes == saved.references.size());
    CHECK(page.info.summaryIsJsonObject);
    CHECK(page.info.summaryArrayCounts.size() == 6);
    CHECK(page.info.summaryArrayCounts.at("goals") == 2);
    CHECK(page.info.summaryArrayCounts.at("progress") == 3);
    CHECK(page.info.referenceIndexState == MaiContextReferenceIndexState::Available);
    CHECK(page.info.referenceCountKnown && page.info.referenceCount == 2540);
    CHECK(reconstruct(store, sessionId, MaiContextCheckpointSection::Summary, 7) == saved.summary);
    CHECK(reconstruct(store, sessionId, MaiContextCheckpointSection::References, 131) ==
          saved.references);
    CHECK(original.size() == 138);
    checkReadOnlyHistory(store, sessionId, original);
    MaiContextCheckpoint after;
    CHECK(store.getContextCheckpoint(sessionId, after));
    CHECK(after.revision == saved.revision && after.summary == saved.summary &&
          after.references == saved.references &&
          after.sourceFingerprint == saved.sourceFingerprint);
}

MaiContextCheckpoint seedArchive(MaiSessionStore& store, const std::string& sessionId) {
    auto saved = checkpoint(store, sessionId, 134);
    Json references = Json::array();
    for (int index = 0; index < 2540; ++index)
        references.push_back({{"message_id", saved.firstMessageId},
                              {"part_id", "prt_fixture"},
                              {"value", "https://fixture.invalid/item/" + std::to_string(index)}});
    saved.references = references.dump(1);
    CHECK(!store.advanceContextCheckpoint(sessionId, 0, saved));
    for (int index = 134; index < 138; ++index) appendMessage(store, sessionId, index);
    return saved;
}

void testMemoryAndReopenedSqliteArchive() {
    auto memory = makeMaiMemoryStore();
    const auto memoryId = createSession(*memory);
    const auto memorySaved = seedArchive(*memory, memoryId);
    testStoredArchiveAndUncoveredTail(*memory, memoryId, memorySaved);
    MaiOwnedCheckpointDatabase database;
    auto sqlite = sqliteStore(database.path());
    if (!sqlite) return;
    const auto sqliteId = createSession(*sqlite);
    const auto sqliteSaved = seedArchive(*sqlite, sqliteId);
    sqlite.reset();
    sqlite = sqliteStore(database.path());
    if (sqlite) testStoredArchiveAndUncoveredTail(*sqlite, sqliteId, sqliteSaved);
}

void testMissingValidationAndRevisionConflicts(MaiSessionStore& store) {
    const auto sessionId = createSession(store);
    auto absent = maiReadContextCheckpoint(store, sessionId, {});
    CHECK(absent && !absent.value().available);
    if (absent) {
        CHECK(absent.value().info.revision == 0 && absent.value().info.model.empty());
        CHECK(absent.value().info.summaryArrayCounts.empty());
        CHECK(!absent.value().info.referenceCountKnown);
    }
    auto missing = maiReadContextCheckpoint(store, "ses_not_owned", {});
    CHECK(!missing && missing.error().code() == MaiErrorCode::NotFound);
    for (const auto& badId :
         {std::string{}, std::string(129, 's'), std::string("ses_\0hidden", 11)}) {
        const auto bad = maiReadContextCheckpoint(store, badId, {});
        CHECK(!bad && bad.error().code() == MaiErrorCode::InvalidInput);
    }
    for (const auto budget : {0u, 1u, 3u, 65537u}) {
        MaiContextCheckpointReadQuery query;
        query.maxBytes = budget;
        const auto bad = maiReadContextCheckpoint(store, sessionId, query);
        CHECK(!bad && bad.error().code() == MaiErrorCode::InvalidInput);
    }
    for (const auto section :
         {MaiContextCheckpointSection::Summary, MaiContextCheckpointSection::Metadata,
          static_cast<MaiContextCheckpointSection>(77)}) {
        MaiContextCheckpointReadQuery query;
        query.section = section;
        query.offsetBytes = 1;
        const auto bad = maiReadContextCheckpoint(store, sessionId, query);
        CHECK(!bad && bad.error().code() == MaiErrorCode::InvalidInput);
    }
    auto saved = checkpoint(store, sessionId);
    CHECK(!store.advanceContextCheckpoint(sessionId, 0, saved));
    const auto otherSessionId = createSession(store);
    const auto other = maiReadContextCheckpoint(store, otherSessionId, {});
    CHECK(other && !other.value().available && other.value().text.empty());
    MaiContextCheckpointReadQuery query;
    query.expectedRevision = saved.revision;
    query.offsetBytes = 4;
    auto continuation = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(continuation && continuation.value().text == saved.summary.substr(4));
    query.offsetBytes = saved.summary.size();
    auto eof = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(eof && eof.value().text.empty() && !eof.value().hasMore);
    query.offsetBytes = std::numeric_limits<std::size_t>::max();
    const auto beyond = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(!beyond && beyond.error().code() == MaiErrorCode::InvalidInput);
    query.offsetBytes = 4;
    auto newer = saved;
    newer.revision = 2;
    newer.summary = "newer stored summary";
    CHECK(!store.advanceContextCheckpoint(sessionId, 1, newer));
    const auto conflict = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(!conflict && conflict.error().code() == MaiErrorCode::Busy);
    CHECK(store.clearMessages(sessionId));
    const auto removed = maiReadContextCheckpoint(store, sessionId, query);
    CHECK(!removed && removed.error().code() == MaiErrorCode::Busy);
}

void writeLegacyPayloads(const std::string& databasePath, const MaiContextCheckpoint& saved) {
    if (databasePath.empty()) return;
    // 正常新写入会修复坏 UTF-8；直接绑定自建库才能复现历史库原本已有的坏字节。
    sqlite3* connection = nullptr;
    CHECK(sqlite3_open(databasePath.c_str(), &connection) == SQLITE_OK);
    if (!connection) return;
    sqlite3_stmt* statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(
        connection,
        "UPDATE context_checkpoints SET summary=?1,reference_text=?2 WHERE session_id=?3", -1,
        &statement, nullptr);
    CHECK(prepared == SQLITE_OK);
    if (prepared == SQLITE_OK) {
        CHECK(sqlite3_bind_text(statement, 1, saved.summary.data(),
                                static_cast<int>(saved.summary.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(statement, 2, saved.references.data(),
                                static_cast<int>(saved.references.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_bind_text(statement, 3, saved.sessionId.c_str(), -1, SQLITE_TRANSIENT) ==
              SQLITE_OK);
        CHECK(sqlite3_step(statement) == SQLITE_DONE);
        CHECK(sqlite3_changes(connection) == 1);
    }
    sqlite3_finalize(statement);
    sqlite3_close(connection);
}

void testUtf8BoundariesAndLegacyBytes(MaiSessionStore& store,
                                      const std::string& databasePath = {}) {
    const auto sessionId = createSession(store);
    auto saved = checkpoint(store, sessionId);
    saved.summary = "A\u4e2d\U0001f642Z";
    saved.references = std::string("[\"A\xff\0", 6) + "B\"]";
    CHECK(!store.advanceContextCheckpoint(sessionId, 0, saved));
    writeLegacyPayloads(databasePath, saved);
    CHECK(reconstruct(store, sessionId, MaiContextCheckpointSection::Summary, 4) == saved.summary);
    CHECK(reconstruct(store, sessionId, MaiContextCheckpointSection::References, 4, false) ==
          saved.references);
    for (const std::size_t offset : {2u, 3u, 5u, 6u, 7u}) {
        MaiContextCheckpointReadQuery query;
        query.offsetBytes = offset;
        query.expectedRevision = 1;
        const auto split = maiReadContextCheckpoint(store, sessionId, query);
        CHECK(!split && split.error().code() == MaiErrorCode::InvalidInput);
    }
    const auto invalid = maiReadContextCheckpoint(store, sessionId, {});
    CHECK(invalid && !invalid.value().info.summaryIsJsonObject);
    CHECK(invalid &&
          invalid.value().info.referenceIndexState == MaiContextReferenceIndexState::Invalid);
    CHECK(invalid && !invalid.value().info.referenceCountKnown);
    auto legacy = saved;
    legacy.revision = 2;
    legacy.summary = std::string("\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80\xf0\x9f", 11) + "Z";
    CHECK(!store.advanceContextCheckpoint(sessionId, 1, legacy));
    writeLegacyPayloads(databasePath, legacy);
    CHECK(reconstruct(store, sessionId, MaiContextCheckpointSection::Summary, 4, false) ==
          legacy.summary);
    for (std::size_t offset = 0; offset < legacy.summary.size(); ++offset) {
        MaiContextCheckpointReadQuery query;
        query.expectedRevision = 2;
        query.offsetBytes = offset;
        query.maxBytes = 4;
        const auto raw = maiReadContextCheckpoint(store, sessionId, query);
        CHECK(raw && !raw.value().text.empty() &&
              raw.value().text.front() == legacy.summary[offset]);
    }
}

void testMetadataBudgetsAndCorruptIndex() {
    auto store = makeMaiMemoryStore();
    const auto sessionId = createSession(*store);
    auto saved = checkpoint(*store, sessionId);
    saved.summary = "{\"goals\":false,\"constraints\":[1],\"extra\":[2]}";
    saved.references = "{}";
    CHECK(!store->advanceContextCheckpoint(sessionId, 0, saved));
    auto result = maiReadContextCheckpoint(*store, sessionId, {});
    CHECK(result && result.value().info.summaryIsJsonObject);
    CHECK(result && result.value().info.summaryArrayCounts.size() == 1);
    CHECK(result && result.value().info.summaryArrayCounts.at("constraints") == 1);
    CHECK(result && !result.value().info.referenceCountKnown);
    CHECK(result &&
          result.value().info.referenceIndexState == MaiContextReferenceIndexState::Invalid);
    saved.revision = 2;
    saved.summary = "{\"goals\":[]}" + std::string(1024 * 1024, ' ');
    saved.references = "[]" + std::string(32 * 1024 * 1024, ' ');
    CHECK(!store->advanceContextCheckpoint(sessionId, 1, saved));
    result = maiReadContextCheckpoint(*store, sessionId, {});
    CHECK(result && !result.value().info.summaryIsJsonObject);
    CHECK(result && result.value().info.summaryArrayCounts.empty());
    CHECK(result && !result.value().info.referenceCountKnown);
    CHECK(result && result.value().info.referenceIndexState ==
                        MaiContextReferenceIndexState::MetadataBudgetExceeded);
    MaiContextCheckpointReadQuery tail;
    tail.section = MaiContextCheckpointSection::References;
    tail.expectedRevision = 2;
    tail.offsetBytes = saved.references.size() - 4;
    tail.maxBytes = 4;
    const auto rawTail = maiReadContextCheckpoint(*store, sessionId, tail);
    CHECK(rawTail && rawTail.value().text == "    " && !rawTail.value().hasMore);
    saved.revision = 3;
    saved.references.clear();
    CHECK(!store->advanceContextCheckpoint(sessionId, 2, saved));
    tail.expectedRevision = 3;
    tail.offsetBytes = 0;
    const auto empty = maiReadContextCheckpoint(*store, sessionId, tail);
    CHECK(empty && empty.value().available && empty.value().text.empty());
    CHECK(empty && empty.value().totalBytes == 0 && !empty.value().hasMore);
    CHECK(empty && !empty.value().info.referenceCountKnown);
    CHECK(empty &&
          empty.value().info.referenceIndexState == MaiContextReferenceIndexState::Invalid);
}

void testReadFailureIsNotReportedAsAbsence() {
    MaiOwnedCheckpointDatabase database;
    auto store = sqliteStore(database.path());
    if (!store) return;
    const auto sessionId = createSession(*store);
    // 只破坏自己临时数据库的表，验证现有 bool 接口失败时不会伪装成没有检查点。
    sqlite3* connection = nullptr;
    CHECK(sqlite3_open(database.path().c_str(), &connection) == SQLITE_OK);
    if (!connection) return;
    CHECK(sqlite3_exec(connection, "DROP TABLE context_checkpoints", nullptr, nullptr, nullptr) ==
          SQLITE_OK);
    sqlite3_close(connection);
    const auto result = maiReadContextCheckpoint(*store, sessionId, {});
    CHECK(!result && result.error().code() == MaiErrorCode::Internal);
}

void testSnapshotIdentityAndEmptyStoredFieldsWithoutHistoryScan() {
    MaiCheckpointReadProbe store;
    store.snapshot.sessionId = "ses_different";
    store.snapshot.revision = 9;
    store.snapshot.summary = "Private archive of another session";
    const auto foreign = maiReadContextCheckpoint(store, "ses_current", {});
    CHECK(!foreign && foreign.error().code() == MaiErrorCode::NotFound);
    CHECK(store.checkpointReads == 1 && store.unrelatedCalls == 0);
    store.snapshot.sessionId = "ses_current";
    store.snapshot.partialMessageId = "msg_partial";
    store.snapshot.partialPartCount = 7;
    store.snapshot.throughPartId = "prt_partial";
    store.snapshot.preservedUserMessageId = "msg_preserved";
    store.snapshot.summary.clear();
    store.snapshot.references = "[]";
    const auto empty = maiReadContextCheckpoint(store, "ses_current", {});
    CHECK(empty && empty.value().available && empty.value().text.empty());
    CHECK(empty && empty.value().sourceBytesRead == 0 && !empty.value().hasMore);
    CHECK(empty && empty.value().info.revision == 9);
    CHECK(empty && empty.value().info.partialMessageId == "msg_partial");
    CHECK(empty && empty.value().info.partialPartCount == 7);
    CHECK(empty && empty.value().info.throughPartId == "prt_partial");
    CHECK(empty && empty.value().info.preservedUserMessageId == "msg_preserved");
    CHECK(empty && empty.value().info.referenceCountKnown &&
          empty.value().info.referenceCount == 0);
    CHECK(store.checkpointReads == 2 && store.unrelatedCalls == 0);
}
}  // namespace

int main() {
    testMemoryAndReopenedSqliteArchive();
    auto memory = makeMaiMemoryStore();
    testMissingValidationAndRevisionConflicts(*memory);
    testUtf8BoundariesAndLegacyBytes(*memory);
    auto sqlite = sqliteStore(":memory:");
    if (sqlite) {
        testMissingValidationAndRevisionConflicts(*sqlite);
    }
    MaiOwnedCheckpointDatabase legacyDatabase;
    auto legacySqlite = sqliteStore(legacyDatabase.path());
    if (legacySqlite) testUtf8BoundariesAndLegacyBytes(*legacySqlite, legacyDatabase.path());
    testMetadataBudgetsAndCorruptIndex();
    testReadFailureIsNotReportedAsAbsence();
    testSnapshotIdentityAndEmptyStoredFieldsWithoutHistoryScan();
    std::printf("MaiContextCheckpointReadTests: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
