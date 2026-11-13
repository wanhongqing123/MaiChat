#include "MaiIMHistorySnapshot.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

#define CHECK(value)                                    \
    do {                                                \
        if (!(value)) throw std::runtime_error(#value); \
    } while (false)

namespace {

using Clock = std::chrono::steady_clock;

void execute(sqlite3* database, const std::string& sql) {
    CHECK(sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}

const char* desktopSchema =
    "CREATE TABLE messages(id TEXT PRIMARY KEY,from_user TEXT,to_user TEXT,peer TEXT,"
    "direction INTEGER,status INTEGER,text TEXT,created_at INTEGER,has_image INTEGER DEFAULT 0,"
    "image_path TEXT,image_w INTEGER,image_h INTEGER,image_bytes INTEGER,has_voice INTEGER DEFAULT "
    "0,"
    "voice_path TEXT,voice_seconds INTEGER,has_file INTEGER DEFAULT 0,file_path TEXT,file_name "
    "TEXT,"
    "file_mime TEXT,file_bytes INTEGER,has_video INTEGER DEFAULT 0,video_path TEXT,video_name TEXT,"
    "video_cover TEXT,video_seconds INTEGER,video_bytes INTEGER,caption_above INTEGER DEFAULT 0,"
    "quote_msg_id TEXT,quote_sender TEXT,quote_digest TEXT,quote_kind TEXT,approval_token TEXT);"
    "CREATE INDEX messages_peer_page ON messages(peer,created_at,id);";

const char* androidSchema =
    "CREATE TABLE messages(owner_id TEXT,id TEXT,remote_id TEXT DEFAULT '',peer_id TEXT,"
    "from_id TEXT,to_id TEXT,body TEXT,direction TEXT,status TEXT,created_at INTEGER,"
    "image_path TEXT DEFAULT '',image_width INTEGER DEFAULT 0,image_height INTEGER DEFAULT 0,"
    "image_size INTEGER DEFAULT 0,voice_path TEXT DEFAULT '',voice_duration INTEGER DEFAULT 0,"
    "file_path TEXT DEFAULT '',file_name TEXT DEFAULT '',file_mime TEXT DEFAULT '',"
    "file_size INTEGER DEFAULT 0,video_path TEXT DEFAULT '',video_cover_path TEXT DEFAULT '',"
    "video_duration INTEGER DEFAULT 0,video_width INTEGER DEFAULT 0,video_height INTEGER DEFAULT 0,"
    "video_size INTEGER DEFAULT 0,caption_above INTEGER DEFAULT 0,quote_msg_id TEXT DEFAULT '',"
    "quote_sender TEXT DEFAULT '',quote_digest TEXT DEFAULT '',quote_kind TEXT DEFAULT '',"
    "approval_request_token TEXT,PRIMARY KEY(owner_id,id));"
    "CREATE INDEX messages_conversation_idx ON messages(owner_id,peer_id,created_at,id);";

const char* iosSchema =
    "CREATE TABLE messages(sdk_app_id TEXT,owner_user_id TEXT,id TEXT,remote_id TEXT,"
    "from_user TEXT,to_user TEXT,text TEXT,direction TEXT,status TEXT,created_at REAL,"
    "voice_attachment TEXT,image_attachment TEXT,file_attachment TEXT,video_attachment TEXT,"
    "caption_above INTEGER DEFAULT 0,quote TEXT,media_caption TEXT,peer_user_id TEXT,group_id TEXT,"
    "approval_request TEXT,PRIMARY KEY(sdk_app_id,owner_user_id,id));"
    "CREATE INDEX messages_account_peer_time ON "
    "messages(sdk_app_id,owner_user_id,peer_user_id,created_at,id);";

// 观察实际 reader 的 SQLite VM 工作量，避免用机器速度或复制出来的 SQL 代替回归。
std::uint64_t iosReadSteps = 0;
int traceIosRead(unsigned, void*, void* statement, void*) {
    auto* query = static_cast<sqlite3_stmt*>(statement);
    const auto* sql = sqlite3_sql(query);
    if (sql && std::strncmp(sql, "SELECT id,remote_id,from_user", 27) == 0)
        iosReadSteps += sqlite3_stmt_status(query, SQLITE_STMTSTATUS_VM_STEP, 0);
    return 0;
}
int observeIosReader(sqlite3* database, char**, const sqlite3_api_routines*) {
    return sqlite3_trace_v2(database, SQLITE_TRACE_PROFILE, traceIosRead, nullptr);
}
struct IosReadObservation {
    IosReadObservation() {
        iosReadSteps = 0;
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(observeIosReader)) == SQLITE_OK);
    }
    ~IosReadObservation() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(observeIosReader));
    }
};

struct Fixture {
    MaiFilePath directory;
    MaiFilePath path;
    sqlite3* writer = nullptr;
    MaiIMHistorySnapshotOptions options;

    explicit Fixture(MaiIMHistoryProfile profile = MaiIMHistoryProfile::Desktop,
                     const std::string& filename = "history.db") {
        static unsigned next = 0;
        directory = MaiFileSystem::temporaryDirectory().append(
            MaiFilePath::fromUtf8("mai-history-owned-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                                  std::to_string(++next)));
        CHECK(!MaiFileSystem::createDirectories(directory));
        path = directory.append(MaiFilePath::fromUtf8(filename));
        CHECK(sqlite3_open_v2(path.toUtf8().c_str(), &writer, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
              SQLITE_OK);
        execute(writer, "PRAGMA journal_mode=WAL;");
        execute(writer, profile == MaiIMHistoryProfile::Desktop   ? desktopSchema
                        : profile == MaiIMHistoryProfile::Android ? androidSchema
                                                                  : iosSchema);
        options.profile = profile;
        options.databasePath = path.toUtf8();
        options.ownerUserId = "owner";
        options.peerUserId = "peer";
        options.sdkAppId = 123;
    }
    ~Fixture() {
        if (writer) sqlite3_close(writer);
        // 仅清理本例明确创建的临时子目录，不接触用户库或共享构建目录。
        const auto temporary = MaiFileSystem::resolve(MaiFileSystem::temporaryDirectory());
        if (temporary.isParentOf(MaiFileSystem::resolve(directory))) MaiFileSystem::removeRecursively(directory);
    }
    void desktop(const std::string& id, std::int64_t time, int direction = 0, const std::string& peer = "peer",
                 const std::string& body = "owned body") {
        sqlite3_stmt* statement = nullptr;
        CHECK(sqlite3_prepare_v2(writer,
                                 "INSERT INTO messages(id,from_user,to_user,peer,"
                                 "direction,status,text,created_at) VALUES(?,?,?,?,?,?,?,?)",
                                 -1, &statement, nullptr) == SQLITE_OK);
        const std::vector<std::string> strings{id, direction ? "owner" : "peer", direction ? "peer" : "owner", peer};
        for (int index = 0; index < 4; ++index)
            sqlite3_bind_text(statement, index + 1, strings[index].data(), int(strings[index].size()),
                              SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 5, direction);
        sqlite3_bind_int(statement, 6, direction ? 1 : 2);
        sqlite3_bind_text(statement, 7, body.data(), int(body.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 8, time);
        const auto result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        CHECK(result == SQLITE_DONE);
    }
    std::unique_ptr<MaiIMHistorySnapshot> open() {
        auto result = MaiIMHistorySnapshot::open(options);
        if (!result) throw std::runtime_error(result.error().message());
        return std::move(result.value());
    }
};

void moreThanTwoHundredOrderedRowsAndBothDirections() {
    Fixture fixture;
    for (int index = 449; index >= 0; --index) {
        auto number = std::to_string(index);
        fixture.desktop("id-" + std::string(4 - number.size(), '0') + number, index / 3, index % 2);
    }
    fixture.desktop("other-peer", -100, 0, "other");
    auto reader = fixture.open();
    CHECK(reader->source().provenance == "owner_only_legacy" && !reader->source().sdkAppId);
    CHECK(reader->source().coverage == "local_persisted");
    std::vector<MaiIMHistoryRecord> all;
    std::uint64_t sequence = 0;
    while (true) {
        auto page = reader->read();
        CHECK(page);
        CHECK(page.value().sequence == sequence++ && page.value().records.size() <= 200);
        CHECK(page.value().serializedBytes <= 384 * 1024);
        all.insert(all.end(), page.value().records.begin(), page.value().records.end());
        if (!page.value().hasMore) break;
    }
    CHECK(all.size() == 450 && sequence == 3);
    for (std::size_t index = 0; index < all.size(); ++index) {
        CHECK(all[index].createdAtMillis == std::int64_t(index / 3));
        CHECK(all[index].direction == (index % 2 ? MaiIMHistoryDirection::Outgoing : MaiIMHistoryDirection::Incoming));
        CHECK(!all[index].remoteId);
        if (index)
            CHECK(std::make_pair(all[index - 1].createdAtMillis, all[index - 1].rowId) <
                  std::make_pair(all[index].createdAtMillis, all[index].rowId));
    }
}

void pinnedWalSnapshotDoesNotAdoptWritesOrTimestampChanges() {
    Fixture fixture;
    fixture.desktop("a", 10);
    fixture.desktop("b", 20);
    fixture.options.maxRecords = 1;
    auto reader = fixture.open();
    // open 的首次 SELECT 已固定快照；首个 read 前的正常 WAL 写也必须排除。
    fixture.desktop("late-old-time", 1);
    execute(fixture.writer, "UPDATE messages SET created_at=99,text='changed' WHERE id='a'");
    auto first = reader->read();
    CHECK(first && first.value().records.size() == 1);
    CHECK(first.value().records[0].rowId == "a" && first.value().records[0].text == "owned body");
    fixture.desktop("late-new-time", 30);
    execute(fixture.writer, "PRAGMA wal_checkpoint(PASSIVE)");
    auto second = reader->read();
    CHECK(second && !second.value().hasMore);
    CHECK(second.value().records.size() == 1 && second.value().records[0].rowId == "b");
}

void androidPartitionRemoteIdAndMetadata() {
    Fixture fixture(MaiIMHistoryProfile::Android);
    execute(fixture.writer,
            "INSERT INTO "
            "messages(owner_id,id,remote_id,peer_id,from_id,to_id,body,direction,status,created_at,"
            "file_path,file_name,file_mime,file_size,quote_msg_id,quote_sender,quote_digest,quote_kind,"
            "approval_request_token)"
            " VALUES('owner','a','actual-sdk-id','peer','owner','peer','caption','OUTGOING','SENT',42,"
            "'/not-read/owned.txt','owned.txt','text/plain',8,'quoted','peer','safe "
            "quote','text','SECRET-APPROVAL');"
            "INSERT INTO messages(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at)"
            " VALUES('other-owner','b','peer','peer','other-owner','private','INCOMING','RECEIVED',43)"
            ";");
    auto reader = fixture.open();
    auto page = reader->read();
    CHECK(page);
    CHECK(page.value().records.size() == 1 && !page.value().hasMore);
    const auto& row = page.value().records.front();
    CHECK(row.remoteId && *row.remoteId == "actual-sdk-id" && row.quote && row.quote->digest == "safe quote");
    CHECK(row.media.size() == 1 && row.media[0].name == "owned.txt" && row.media[0].hasLocalReference);
    CHECK(!reader->source().sdkAppId && reader->source().provenance == "owner_only_legacy");
}

void iosActualAccountDirectFilterAndRoundedTime() {
    Fixture fixture(MaiIMHistoryProfile::IOS);
    execute(fixture.writer,
            "INSERT INTO "
            "messages(sdk_app_id,owner_user_id,id,remote_id,from_user,to_user,text,direction,"
            "status,created_at,peer_user_id,image_attachment,quote)"
            " VALUES('123','owner','b','real-remote','peer','owner','image','incoming','received',"
            "1.00049,'peer',"
            "'{\"localFilePath\":\"incoming/images/"
            "owned.png\",\"width\":32,\"height\":24,\"sizeBytes\":9}',"
            "'{\"messageID\":\"quoted\",\"senderID\":\"owner\",\"digest\":\"quote\",\"kind\":"
            "\"text\"}');"
            "INSERT INTO "
            "messages(sdk_app_id,owner_user_id,id,from_user,to_user,text,direction,status,created_"
            "at,peer_user_id) VALUES"
            "('123','owner','a','owner','peer','first','outgoing','sent',1.0004,'peer'),"
            "('456','owner','wrong-sdk','peer','owner','private','incoming','received',1,'peer'),"
            "('123','other','wrong-owner','peer','other','private','incoming','received',1,'peer');"
            "INSERT INTO "
            "messages(sdk_app_id,owner_user_id,id,from_user,to_user,text,direction,status,created_"
            "at,peer_user_id,group_id)"
            " VALUES('123','owner','group','peer','owner','private "
            "group','incoming','received',1,'peer','group-id');");
    fixture.options.maxRecords = 1;
    auto reader = fixture.open();
    CHECK(reader->source().sdkAppId && *reader->source().sdkAppId == 123);
    auto first = reader->read();
    CHECK(first && first.value().hasMore);
    CHECK(first.value().records[0].rowId == "a" && first.value().records[0].createdAtMillis == 1000);
    auto second = reader->read();
    CHECK(second && !second.value().hasMore);
    const auto& row = second.value().records[0];
    CHECK(row.rowId == "b" && row.createdAtMillis == 1000 && row.media.size() == 1 && row.quote);
    CHECK(row.media[0].width == 32 && row.media[0].height == 24 && row.media[0].hasLocalReference);
}

void createIosPeerExpressionIndex(Fixture& fixture) {
    execute(fixture.writer,
            "CREATE INDEX idx_im_query_peer_ms ON messages(sdk_app_id,owner_user_id,"
            "(CASE WHEN peer_user_id IS NULL OR peer_user_id='' THEN CASE WHEN "
            "from_user=owner_user_id THEN to_user ELSE from_user END ELSE peer_user_id END),"
            "CAST(ROUND(created_at*1000.0) AS INTEGER) DESC,id DESC) WHERE group_id IS NULL;");
}

void iosLegacyPeerRows() {
    Fixture fixture(MaiIMHistoryProfile::IOS);
    createIosPeerExpressionIndex(fixture);
    execute(fixture.writer,
            "INSERT INTO messages(sdk_app_id,owner_user_id,id,from_user,to_user,text,direction,"
            "status,created_at,peer_user_id) VALUES"
            "('123','owner','legacy-a','owner','peer','legacy outgoing','outgoing','sent',-2,NULL),"
            "('123','owner','legacy-b','peer','owner','legacy incoming','incoming','received',-1,''),"
            "('123','owner','other','other-peer','owner','private','incoming','received',0,NULL);");
    {
        auto legacy = fixture.open();
        auto page = legacy->read();
        CHECK(page && page.value().records.size() == 2);
        CHECK(page.value().records[0].rowId == "legacy-a" && page.value().records[1].rowId == "legacy-b");
    }
}

void iosIndexedContinuation() {
    Fixture fixture(MaiIMHistoryProfile::IOS);
    createIosPeerExpressionIndex(fixture);
    execute(fixture.writer,
            "WITH RECURSIVE owned(i) AS (VALUES(0) UNION ALL "
            "SELECT i+1 FROM owned WHERE i<19999) INSERT INTO "
            "messages(sdk_app_id,owner_user_id,id,from_user,to_user,text,direction,status,"
            "created_at,peer_user_id) SELECT '123','owner',printf('id-%06d',i),'peer','owner',"
            "'owned','incoming','received',i/3000.0,'peer' FROM owned;");
    IosReadObservation observation;
    auto reader = fixture.open();
    std::size_t rows = 0;
    std::pair<std::int64_t, std::string> previous;
    const auto started = Clock::now();
    while (true) {
        auto page = reader->read();
        CHECK(page);
        for (const auto& row : page.value().records) {
            const auto key = std::make_pair(row.createdAtMillis, row.rowId);
            CHECK(rows == 0 || previous < key);
            previous = key;
            ++rows;
        }
        if (!page.value().hasMore) break;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    std::cout << "OWNED iOS 20000 rows VM steps=" << iosReadSteps << " elapsed_ms=" << elapsed.count() << '\n';
    CHECK(rows == 20000 && iosReadSteps > 0 && iosReadSteps < 10000000);
}

void corruptOptionalMediaIsNotDropped() {
    Fixture fixture(MaiIMHistoryProfile::IOS);
    execute(fixture.writer,
            "INSERT INTO "
            "messages(sdk_app_id,owner_user_id,id,from_user,to_user,text,direction,status,created_"
            "at,peer_user_id,image_attachment,quote)"
            " VALUES('123','owner','corrupt','peer','owner','retained','incoming','received',1,'"
            "peer','{bad','[]');");
    auto reader = fixture.open();
    auto page = reader->read();
    CHECK(page);
    const auto& row = page.value().records.at(0);
    CHECK(row.rowId == "corrupt" && row.text == "retained" && row.media.size() == 1);
    CHECK(row.media[0].metadataUnavailable && !row.media[0].hasLocalReference);
    CHECK(std::find(row.unavailableMetadata.begin(), row.unavailableMetadata.end(), "quote") !=
          row.unavailableMetadata.end());
}

void emptyMissingCanceledAndExpiredAreDistinct() {
    Fixture fixture;
    auto reader = fixture.open();
    auto empty = reader->read();
    CHECK(empty && empty.value().records.empty() && !empty.value().hasMore);
    auto options = fixture.options;
    options.databasePath += ".missing";
    auto missing = MaiIMHistorySnapshot::open(options);
    CHECK(!missing && missing.error().code() == MaiErrorCode::NotFound);
    options = fixture.options;
    options.canceled = std::make_shared<std::atomic<bool>>(true);
    auto canceled = MaiIMHistorySnapshot::open(options);
    CHECK(!canceled && canceled.error().code() == MaiErrorCode::Canceled);
    reader->cancel();
    auto stopped = reader->read();
    CHECK(!stopped && stopped.error().code() == MaiErrorCode::Canceled);
    options = fixture.options;
    options.lifetimeTimeoutMillis = 1;
    auto expires = MaiIMHistorySnapshot::open(options);
    CHECK(expires);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto expired = expires.value()->read();
    CHECK(!expired && expired.error().code() == MaiErrorCode::Busy);
}

void boundedPagesAndOversizedTextFailExplicitly() {
    Fixture fixture;
    fixture.desktop("a", 1, 0, "peer", std::string(1000, 'a'));
    fixture.desktop("b", 2, 0, "peer", std::string(1000, 'b'));
    fixture.options.maxPageBytes = 2000;
    auto reader = fixture.open();
    auto first = reader->read();
    CHECK(first && first.value().records.size() == 1 && first.value().hasMore);
    auto second = reader->read();
    CHECK(second && second.value().records.size() == 1 && !second.value().hasMore);
    fixture.desktop("oversized", 3, 0, "peer", std::string(256 * 1024 + 1, 'x'));
    auto oversized = fixture.open();
    bool failed = false;
    for (int index = 0; index < 4; ++index) {
        auto page = oversized->read();
        if (!page) {
            CHECK(page.error().code() == MaiErrorCode::InvalidInput);
            failed = true;
            break;
        }
        if (!page.value().hasMore) break;
    }
    CHECK(failed);
}

void schemaFailureIsNotAuthoritativeEmpty() {
    Fixture fixture;
    execute(fixture.writer, "DROP TABLE messages");
    auto reader = MaiIMHistorySnapshot::open(fixture.options);
    CHECK(!reader && reader.error().code() == MaiErrorCode::Internal);
}

std::function<void()> traceAction;
int ownedTrace(unsigned, void*, void*, void*) {
    if (traceAction) {
        auto action = std::move(traceAction);
        traceAction = {};
        action();
    }
    return 0;
}
int installOwnedTrace(sqlite3* database, char**, const sqlite3_api_routines*) {
    sqlite3_trace_v2(database, SQLITE_TRACE_STMT, ownedTrace, nullptr);
    return SQLITE_OK;
}
struct OwnedTrace {
    OwnedTrace() {
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(installOwnedTrace)) == SQLITE_OK);
    }
    ~OwnedTrace() {
        traceAction = {};
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(installOwnedTrace));
    }
};

void pageDeadlineAndCancelApplyDuringSql() {
    Fixture fixture;
    fixture.desktop("owned", 1);
    OwnedTrace trace;
    fixture.options.pageTimeoutMillis = 5;
    auto reader = fixture.open();
    // Trace belongs only to this test process/reader connection. It delays the
    // actual read statement after open, without a production clock/test flag.
    traceAction = [] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); };
    auto expired = reader->read();
    CHECK(!expired && expired.error().code() == MaiErrorCode::Busy);
    auto terminal = reader->read();
    CHECK(!terminal && terminal.error().code() == MaiErrorCode::Busy);
    fixture.options.pageTimeoutMillis = 5000;
    auto canceled = fixture.open();
    traceAction = [&] { canceled->cancel(); };
    auto stopped = canceled->read();
    CHECK(!stopped && stopped.error().code() == MaiErrorCode::Canceled);
}

void malformedNumericMetadataAndEmptyBodyRemainRows() {
    Fixture fixture(MaiIMHistoryProfile::Android);
    execute(fixture.writer,
            "INSERT INTO messages(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at,"
            "image_path,image_width,image_height,voice_duration) VALUES"
            "('owner','bad-width','peer','peer','owner','','INCOMING','RECEIVED',1,'/not-read/"
            "image.png',-3,24,0),"
            "('owner','missing-voice','peer','owner','peer','','OUTGOING','SENT',2,'',0,0,2);");
    auto reader = fixture.open();
    auto page = reader->read();
    CHECK(page);
    CHECK(page.value().records.size() == 2 && !page.value().hasMore);
    const auto& image = page.value().records[0];
    CHECK(image.text.empty() && image.media.size() == 1 && image.media[0].metadataUnavailable);
    const auto& voice = page.value().records[1];
    CHECK(voice.text.empty() && voice.media.size() == 1 && !voice.media[0].hasLocalReference);
    CHECK(voice.media[0].durationMillis == 2000 && voice.media[0].metadataUnavailable);
}

void invalidOptionalUtf8AndFlagRemainUnavailableMetadata() {
    Fixture fixture;
    fixture.desktop("bad-utf8", 1);
    fixture.desktop("bad-flag", 2);
    execute(fixture.writer,
            "UPDATE messages SET has_image=1,image_path=CAST(X'80' AS TEXT) WHERE id='bad-utf8';"
            "UPDATE messages SET has_image=2,image_path='/not-read/image.png' WHERE id='bad-flag';");
    auto reader = fixture.open();
    auto page = reader->read();
    CHECK(page && page.value().records.size() == 2);
    for (const auto& row : page.value().records) {
        CHECK(row.text == "owned body" && row.media.size() == 1 && row.media[0].metadataUnavailable);
    }
    CHECK(!page.value().records[0].media[0].hasLocalReference);
}

void unicodeDatabasePathAndEmptyTextArePreserved() {
    Fixture fixture(MaiIMHistoryProfile::Desktop, "history-\u4e2d\u6587.db");
    fixture.desktop("unicode-path", 1, 0, "peer", "");
    auto reader = fixture.open();
    auto page = reader->read();
    CHECK(page && page.value().records.size() == 1 && !page.value().hasMore);
    CHECK(page.value().records[0].text.empty());
}

void pathReplacementIsPreventedOrRejected() {
    Fixture fixture;
    fixture.desktop("a", 1);
    auto reader = fixture.open();
    const auto renamed = fixture.directory.append(MaiFilePath::fromUtf8("renamed.db"));
    // Win32 的 Core 句柄不共享删除；POSIX 则允许 rename，再由 HAS_MOVED 拒绝。
    const auto moved = MaiFileSystem::publishNewFile(fixture.path, renamed);
#if defined(_WIN32)
    CHECK(moved);
    auto page = reader->read();
    CHECK(page && page.value().records.size() == 1);
#else
    CHECK(!moved);
    auto page = reader->read();
    CHECK(!page && page.error().code() == MaiErrorCode::InvalidInput);
#endif
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"more than200 and both directions", moreThanTwoHundredOrderedRowsAndBothDirections},
        {"real pinned WAL snapshot", pinnedWalSnapshotDoesNotAdoptWritesOrTimestampChanges},
        {"Android owner partition and remote id", androidPartitionRemoteIdAndMetadata},
        {"iOS account direct filter and rounded keyset", iosActualAccountDirectFilterAndRoundedTime},
        {"iOS legacy peer rows", iosLegacyPeerRows},
        {"iOS indexed continuation", iosIndexedContinuation},
        {"corrupt optional metadata retained", corruptOptionalMediaIsNotDropped},
        {"empty missing cancel lifetime", emptyMissingCanceledAndExpiredAreDistinct},
        {"page bytes and text limits", boundedPagesAndOversizedTextFailExplicitly},
        {"schema failure", schemaFailureIsNotAuthoritativeEmpty},
        {"SQL deadline and owned cancellation", pageDeadlineAndCancelApplyDuringSql},
        {"corrupt numeric media and empty body", malformedNumericMetadataAndEmptyBodyRemainRows},
        {"corrupt optional UTF8 and flags", invalidOptionalUtf8AndFlagRemainUnavailableMetadata},
        {"Unicode database path and empty text", unicodeDatabasePathAndEmptyTextArePreserved},
        {"path replacement", pathReplacementIsPreventedOrRejected}};
    unsigned failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() << " owned history snapshot groups, " << failures << " failures\n";
    return failures ? 1 : 0;
}
