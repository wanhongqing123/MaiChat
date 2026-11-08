// 文件作用：验证桌面端消息去重、时间排序、分页和重启后的持久化。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QSet>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <sqlite3.h>
#include <QTemporaryDir>
#include <QTest>
#include <limits>
#include <cstdio>

#include "model/ChatState.h"
#include "storage/LocalMessageDatabase.h"

namespace {

// Fixtures own only private QTemporaryDir files. Use the same vendored sqlite3
// library as production, without loading a Qt SQL driver or touching account DBs.
class SqlFixture {
public:
    explicit SqlFixture(const QString& path) {
        const QByteArray bytes = path.toUtf8();
        if (sqlite3_open_v2(bytes.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }
    ~SqlFixture() { if (db_) sqlite3_close_v2(db_); }
    SqlFixture(const SqlFixture&) = delete;
    SqlFixture& operator=(const SqlFixture&) = delete;
    bool isOpen() const { return db_ != nullptr; }
    bool exec(const QString& sql) const {
        return db_ && sqlite3_exec(db_, sql.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    bool columnTexts(const QString& sql, int column, QSet<QString>& output) const {
        sqlite3_stmt* statement = nullptr;
        const QByteArray bytes = sql.toUtf8();
        if (!db_ || sqlite3_prepare_v2(db_, bytes.constData(), int(bytes.size()), &statement, nullptr) != SQLITE_OK)
            return false;
        int result = SQLITE_OK;
        while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
            output.insert(QString::fromUtf8(text, sqlite3_column_bytes(statement, column)));
        }
        sqlite3_finalize(statement);
        return result == SQLITE_DONE;
    }
    qint64 integer(const QString& sql) const {
        sqlite3_stmt* statement = nullptr;
        const QByteArray bytes = sql.toUtf8();
        if (!db_ || sqlite3_prepare_v2(db_, bytes.constData(), int(bytes.size()), &statement, nullptr) != SQLITE_OK)
            return std::numeric_limits<qint64>::min();
        const bool row = sqlite3_step(statement) == SQLITE_ROW;
        const qint64 result = row ? sqlite3_column_int64(statement, 0) : std::numeric_limits<qint64>::min();
        sqlite3_finalize(statement);
        return result;
    }
private:
    sqlite3* db_ = nullptr;
};

RemoteIMMessage makeTextMessage(const QString& id, const QString& from, const QString& to,
                                RemoteIMMessageDirection direction, qint64 createdAtMillis,
                                const QString& text) {
    RemoteIMMessage message;
    message.id = id;
    message.fromUserId = from;
    message.toUserId = to;
    message.direction = direction;
    message.status = direction == RemoteIMMessageDirection::Outgoing ? RemoteIMMessageStatus::Pending
                                                                     : RemoteIMMessageStatus::Received;
    message.createdAtMillis = createdAtMillis;
    message.text = text;
    return message;
}

}  // namespace

class LocalMessageDatabaseTest : public QObject {
    Q_OBJECT

private slots:
    void insertsAndDeduplicatesById();
    void persistsAcrossReopen();
    void loadsMessagesSortedByTime();
    void correctsExistingMessageTimeFromSdkDuplicate();
    void updatesMessageStatus();
    void cascadesContactDeletion();
    void adoptsMessageIdAndResolvesConflict();
    void loadsRecentPagePerPeerAndPagesBackward();
    void roundTripsVideoAttachment();
    void roundTripsApprovalRequest();
    void upgradesPreVideoDatabaseWithoutLosingOldMessages();
    void createsGroupsAndRejectsBadNames();
    void movesContactBetweenGroupsAndPersists();
    void deletingGroupKeepsContactsAndUngroupsThem();
    void renamingGroupCarriesItsMembers();
    void contactProfileRefreshKeepsLocalGroup();
    void healsContactPointingAtMissingGroup();
    void upgradesPreGroupDatabaseWithoutLosingContacts();
    void groupHistoryPagesSeparatelyAndSurvivesMembershipRemoval();
    void roundTripsUnicodeNullEmptyAndFullWidthIntegers();
    void rejectsInvalidDatabasePath();
    void cascadeFailureRollsBackBothDeletes();
    void adoptionFailureKeepsTemporaryMessages();
    void groupAdoptionFailureKeepsTemporaryMessages();
    void groupStepFailuresRollBackMembers();
    void commitFailureRollsBackAndKeepsConnectionUsable();
    void failedMigrationPreservesOldSchema();
    void pagesTimestampTiesAndRejectsEmptyBounds();
};

void LocalMessageDatabaseTest::groupHistoryPagesSeparatelyAndSurvivesMembershipRemoval() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("history.sqlite"));
    {
        LocalMessageDatabase db(path);
        QVERIFY(db.isOpen());
        db.upsertGroup({QStringLiteral("@TGS#one"), QStringLiteral("Team"), 3});
        for (int i = 1; i <= 23; ++i) {
            const RemoteIMGroupMessage message{
                QStringLiteral("group-msg-%1").arg(i, 3, 10, QLatin1Char('0')),
                QStringLiteral("@TGS#one"), QStringLiteral("friend"),
                QStringLiteral("message %1").arg(i), i * 1000LL, i, false, false, false};
            QVERIFY(db.insertGroupMessageIfAbsent(message));
            QVERIFY(!db.insertGroupMessageIfAbsent(message));
        }
        QCOMPARE(db.groupMessageCount(QStringLiteral("@TGS#one")), 23);
        const auto latest = db.groupMessagesBefore(QStringLiteral("@TGS#one"),
            std::numeric_limits<qint64>::max(), QStringLiteral("~"), 20);
        QCOMPARE(latest.size(), 20);
        QCOMPARE(latest.first().sequence, qint64(4));
        const auto older = db.groupMessagesBefore(QStringLiteral("@TGS#one"),
            latest.first().createdAtMillis, latest.first().id, 20);
        QCOMPARE(older.size(), 3);
        QCOMPARE(older.first().sequence, qint64(1));
        db.removeGroup(QStringLiteral("@TGS#one"));
        QVERIFY(db.groups().isEmpty());
        QCOMPARE(db.groupMessageCount(QStringLiteral("@TGS#one")), 23);
    }
    LocalMessageDatabase reopened(path);
    reopened.upsertGroup({QStringLiteral("@TGS#one"), QStringLiteral("Team"), 3});
    QCOMPARE(reopened.groups().size(), 1);
    QCOMPARE(reopened.groupMessageCount(QStringLiteral("@TGS#one")), 23);
}

void LocalMessageDatabaseTest::insertsAndDeduplicatesById() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    const RemoteIMMessage message = makeTextMessage(
        "sdk-1#0", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "hello");
    QVERIFY(db.insertMessageIfAbsent(message, "peer"));
    // 同 id 二次插入（如 SDK 漫游重复投递）不生效。
    QVERIFY(!db.insertMessageIfAbsent(message, "peer"));

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("sdk-1#0"));
}

void LocalMessageDatabaseTest::persistsAcrossReopen() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    {
        LocalMessageDatabase db(path);
        db.upsertContact(RemoteIMContact{"peer", "Peer", "https://example.com/peer.png"});
        db.insertMessageIfAbsent(
            makeTextMessage("m1", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "first"),
            "peer");

        RemoteIMMessage fileMessage = makeTextMessage(
            "m2", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "[文件消息] a.md");
        fileMessage.hasFile = true;
        fileMessage.file = RemoteIMFileAttachment{"C:/tmp/a.md", "a.md", "text/markdown", 42};
        db.insertMessageIfAbsent(fileMessage, "peer");
    }

    LocalMessageDatabase reopened(path);
    ChatState state("me");
    reopened.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().displayName, QStringLiteral("Peer"));
    QCOMPARE(state.contacts().first().avatarUrl, QStringLiteral("https://example.com/peer.png"));
    QCOMPARE(state.messages().size(), 2);
    const RemoteIMMessage restoredFile = state.messages().last();
    QVERIFY(restoredFile.hasFile);
    QCOMPARE(restoredFile.file.fileName, QStringLiteral("a.md"));
    QCOMPARE(restoredFile.file.sizeBytes, static_cast<qint64>(42));
}

void LocalMessageDatabaseTest::loadsMessagesSortedByTime() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("late", "peer", "me", RemoteIMMessageDirection::Incoming, 3000, "late"), "peer");
    db.insertMessageIfAbsent(
        makeTextMessage("early", "peer", "me", RemoteIMMessageDirection::Incoming, 1000, "early"), "peer");

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 2);
    QCOMPARE(state.messages().first().id, QStringLiteral("early"));
    QCOMPARE(state.messages().last().id, QStringLiteral("late"));
}

void LocalMessageDatabaseTest::correctsExistingMessageTimeFromSdkDuplicate() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));

    const RemoteIMMessage legacyAck = makeTextMessage(
        "a-ack", "me", "peer", RemoteIMMessageDirection::Outgoing, 1001, "ack");
    const RemoteIMMessage legacyRequest = makeTextMessage(
        "z-request", "peer", "me", RemoteIMMessageDirection::Incoming, 1482, "request");
    QVERIFY(db.insertMessageIfAbsent(legacyAck, "peer"));
    QVERIFY(db.insertMessageIfAbsent(legacyRequest, "peer"));

    // 漫游命中旧记录时不重复插入，但会用 SDK 会话顺序生成的规范化时间
    // 替换本机乐观发送时间。
    RemoteIMMessage correctedRequest = legacyRequest;
    correctedRequest.createdAtMillis = 1000;
    QVERIFY(!db.insertMessageIfAbsent(correctedRequest, "peer"));

    ChatState state("me");
    db.loadInto(state);
    const QList<RemoteIMMessage> messages = state.messagesWith("peer");
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).id, QStringLiteral("z-request"));
    QCOMPARE(messages.at(1).id, QStringLiteral("a-ack"));
}

void LocalMessageDatabaseTest::updatesMessageStatus() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("m1", "me", "peer", RemoteIMMessageDirection::Outgoing, 1000, "hi"), "peer");
    db.updateMessageStatus("m1", RemoteIMMessageStatus::Sent);

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().first().status, RemoteIMMessageStatus::Sent);
}

void LocalMessageDatabaseTest::cascadesContactDeletion() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.upsertContact(RemoteIMContact{"peer-a", "A"});
    db.upsertContact(RemoteIMContact{"peer-b", "B"});
    db.insertMessageIfAbsent(
        makeTextMessage("a1", "peer-a", "me", RemoteIMMessageDirection::Incoming, 1000, "from a"), "peer-a");
    db.insertMessageIfAbsent(
        makeTextMessage("b1", "peer-b", "me", RemoteIMMessageDirection::Incoming, 2000, "from b"), "peer-b");

    db.removeContactCascade("peer-a");

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().userId, QStringLiteral("peer-b"));
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("b1"));
}

void LocalMessageDatabaseTest::adoptsMessageIdAndResolvesConflict() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.insertMessageIfAbsent(
        makeTextMessage("temp-uuid", "me", "peer", RemoteIMMessageDirection::Outgoing, 1000, "hi"), "peer");

    // 常规采纳：主键换成 SDK 稳定 id。
    db.adoptMessageId("temp-uuid", "sdk-1#0");
    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.messages().size(), 1);
    QCOMPARE(state.messages().first().id, QStringLiteral("sdk-1#0"));

    // 稳定 id 已被漫游占用：旧临时行按重复项清除。
    db.insertMessageIfAbsent(
        makeTextMessage("temp-2", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "hi2"), "peer");
    db.insertMessageIfAbsent(
        makeTextMessage("sdk-2#0", "me", "peer", RemoteIMMessageDirection::Outgoing, 2000, "hi2"), "peer");
    db.adoptMessageId("temp-2", "sdk-2#0");
    ChatState reloaded("me");
    db.loadInto(reloaded);
    QCOMPARE(reloaded.messages().size(), 2);
}

void LocalMessageDatabaseTest::loadsRecentPagePerPeerAndPagesBackward() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    // peer-a 5 条、peer-b 2 条；每会话页大小 3。
    for (int i = 1; i <= 5; ++i) {
        db.insertMessageIfAbsent(makeTextMessage(QStringLiteral("a%1").arg(i), "peer-a", "me",
                                                 RemoteIMMessageDirection::Incoming, i * 100,
                                                 QStringLiteral("a-msg-%1").arg(i)),
                                 "peer-a");
    }
    for (int i = 1; i <= 2; ++i) {
        db.insertMessageIfAbsent(makeTextMessage(QStringLiteral("b%1").arg(i), "peer-b", "me",
                                                 RemoteIMMessageDirection::Incoming, i * 100,
                                                 QStringLiteral("b-msg-%1").arg(i)),
                                 "peer-b");
    }

    ChatState state("me");
    const QHash<QString, bool> hasEarlier = db.loadRecentInto(state, 3);
    // peer-a 只载最近 3 条（a3..a5），peer-b 全量 2 条。
    QCOMPARE(state.messagesWith("peer-a").size(), 3);
    QCOMPARE(state.messagesWith("peer-a").first().id, QStringLiteral("a3"));
    QCOMPARE(state.messagesWith("peer-b").size(), 2);
    QCOMPARE(hasEarlier.value("peer-a"), true);
    QCOMPARE(hasEarlier.value("peer-b"), false);

    // 键集向上翻页：严格早于 (300, "a3") 的是 a1、a2，升序返回。
    const QList<RemoteIMMessage> earlier = db.loadMessagesBefore("peer-a", 300, "a3", 3);
    QCOMPARE(earlier.size(), 2);
    QCOMPARE(earlier.first().id, QStringLiteral("a1"));
    QCOMPARE(earlier.last().id, QStringLiteral("a2"));

    // 已到最早：不再返回。
    QCOMPARE(db.loadMessagesBefore("peer-a", 100, "a1", 3).size(), 0);
}

void LocalMessageDatabaseTest::roundTripsVideoAttachment() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    RemoteIMMessage message;
    message.id = QStringLiteral("video-1");
    message.fromUserId = QStringLiteral("me");
    message.toUserId = QStringLiteral("peer");
    message.direction = RemoteIMMessageDirection::Outgoing;
    message.status = RemoteIMMessageStatus::Sent;
    message.text = QStringLiteral("看下这段录屏");
    message.createdAtMillis = 1700000000000LL;
    message.hasVideo = true;
    message.video = RemoteIMVideoAttachment{
        QStringLiteral("E:/clips/screen-record.mp4"),
        QStringLiteral("screen-record.mp4"),
        QStringLiteral("E:/covers/cover-1.jpg"),
        42,
        8388608LL
    };
    QVERIFY(db.insertMessageIfAbsent(message, QStringLiteral("peer")));

    ChatState restored(QStringLiteral("me"));
    db.loadInto(restored);
    const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
    QCOMPARE(messages.size(), 1);
    const RemoteIMMessage& loaded = messages.first();
    // 插入是位置绑定（34 个 ?），列名单/占位符/绑定顺序错位不会报错，只会把值写进
    // 相邻的列。逐字段比对是唯一能发现错位的手段。
    QVERIFY(loaded.hasVideo);
    QVERIFY(!loaded.hasFile);
    QVERIFY(!loaded.hasImage);
    QCOMPARE(loaded.video.localPath, QStringLiteral("E:/clips/screen-record.mp4"));
    QCOMPARE(loaded.video.fileName, QStringLiteral("screen-record.mp4"));
    QCOMPARE(loaded.video.coverPath, QStringLiteral("E:/covers/cover-1.jpg"));
    QCOMPARE(loaded.video.durationSeconds, 42);
    QCOMPARE(loaded.video.sizeBytes, 8388608LL);
    QCOMPARE(loaded.text, QStringLiteral("看下这段录屏"));
}

// 审批请求持久化后重开数据库，字段和动作标识必须原样恢复才能安全继续处理。
void LocalMessageDatabaseTest::roundTripsApprovalRequest() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    QVERIFY(db.isOpen());

    RemoteIMMessage message = makeTextMessage(
        QStringLiteral("approval-1"),
        QStringLiteral("multi-ai-code"),
        QStringLiteral("me"),
        RemoteIMMessageDirection::Incoming,
        1700000000100LL,
        QStringLiteral("Codex 请求执行一条高风险命令"));
    message.hasApprovalRequest = true;
    message.approvalRequest = RemoteIMApprovalRequest{
        QStringLiteral("approval-desktop-persisted"),
        {RemoteIMApprovalAction::ApproveOnce,
         RemoteIMApprovalAction::ApprovePrefix,
         RemoteIMApprovalAction::Reject}
    };
    QVERIFY(db.insertMessageIfAbsent(message, QStringLiteral("multi-ai-code")));

    ChatState restored(QStringLiteral("me"));
    db.loadInto(restored);
    const RemoteIMMessage loaded = restored.messagesWith(QStringLiteral("multi-ai-code")).first();
    QVERIFY(loaded.hasApprovalRequest);
    QCOMPARE(loaded.approvalRequest.token, QStringLiteral("approval-desktop-persisted"));
    QVERIFY(loaded.approvalRequest.actions == message.approvalRequest.actions);

    RemoteIMMessage decision = makeTextMessage(
        QStringLiteral("approval-decision-1"),
        QStringLiteral("me"),
        QStringLiteral("multi-ai-code"),
        RemoteIMMessageDirection::Outgoing,
        1700000000200LL,
        QStringLiteral("审批操作：同意并记住"));
    decision.status = RemoteIMMessageStatus::Sent;
    decision.hasApprovalDecision = true;
    decision.approvalDecision = RemoteIMApprovalDecision{
        QStringLiteral("approval-desktop-persisted"),
        RemoteIMApprovalAction::ApprovePrefix
    };
    QVERIFY(db.insertMessageIfAbsent(decision, QStringLiteral("multi-ai-code")));

    RemoteIMMessage resolution = makeTextMessage(
        QStringLiteral("approval-resolution-1"),
        QStringLiteral("multi-ai-code"),
        QStringLiteral("me"),
        RemoteIMMessageDirection::Incoming,
        1700000000300LL,
        QStringLiteral("该审批已自动拒绝"));
    resolution.status = RemoteIMMessageStatus::Received;
    resolution.hasApprovalDecision = true;
    resolution.approvalDecision = RemoteIMApprovalDecision{
        QStringLiteral("approval-desktop-persisted"),
        RemoteIMApprovalAction::AutoDeclined
    };
    QVERIFY(db.insertMessageIfAbsent(resolution, QStringLiteral("multi-ai-code")));

    ChatState restarted(QStringLiteral("me"));
    db.loadInto(restarted);
    const QList<RemoteIMMessage> restartedMessages =
        restarted.messagesWith(QStringLiteral("multi-ai-code"));
    QCOMPARE(restartedMessages.size(), 3);
    const RemoteIMMessage& loadedDecision = restartedMessages.at(1);
    QVERIFY(loadedDecision.hasApprovalDecision);
    QCOMPARE(loadedDecision.approvalDecision.token,
             QStringLiteral("approval-desktop-persisted"));
    QCOMPARE(loadedDecision.approvalDecision.action,
             RemoteIMApprovalAction::ApprovePrefix);
    const RemoteIMMessage& loadedResolution = restartedMessages.last();
    QVERIFY(loadedResolution.hasApprovalDecision);
    QCOMPARE(loadedResolution.approvalDecision.action,
             RemoteIMApprovalAction::AutoDeclined);
}

// 用旧版表结构执行迁移；新增视频列不能清掉既有聊天记录或改变消息方向。
void LocalMessageDatabaseTest::upgradesPreVideoDatabaseWithoutLosingOldMessages() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");

    // 1) 先造一个「视频功能之前」的库：手工建不含 video_* 列的表并塞一条文件消息。
    {
        SqlFixture legacy(path);
        QVERIFY(legacy.isOpen());
        QVERIFY(legacy.exec(QStringLiteral(
            "CREATE TABLE messages ("
            "  id TEXT PRIMARY KEY, from_user TEXT NOT NULL, to_user TEXT NOT NULL,"
            "  peer TEXT NOT NULL, direction INTEGER NOT NULL, status INTEGER NOT NULL,"
            "  text TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL,"
            "  has_image INTEGER NOT NULL DEFAULT 0,"
            "  image_path TEXT, image_w INTEGER, image_h INTEGER, image_bytes INTEGER,"
            "  has_voice INTEGER NOT NULL DEFAULT 0, voice_path TEXT, voice_seconds INTEGER,"
            "  has_file INTEGER NOT NULL DEFAULT 0,"
            "  file_path TEXT, file_name TEXT, file_mime TEXT, file_bytes INTEGER)")));
        QVERIFY(legacy.exec(QStringLiteral(
            "INSERT INTO messages(id, from_user, to_user, peer, direction, status, text,"
            " created_at, has_file, file_path, file_name, file_mime, file_bytes)"
            " VALUES('old-1','peer','me','peer',0,2,'旧的文件消息',1600000000000,1,"
            "'E:/old/report.md','report.md','text/markdown',2048)")));
    }

    // 2) 用新版打开：迁移必须补列，且旧消息一条不丢。
    {
        LocalMessageDatabase db(path);
        QVERIFY(db.isOpen());
        ChatState restored(QStringLiteral("me"));
        db.loadInto(restored);
        const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
        QCOMPARE(messages.size(), 1);
        QVERIFY(messages.first().hasFile);
        QCOMPARE(messages.first().file.fileName, QStringLiteral("report.md"));
        QVERIFY(!messages.first().hasVideo);

        // 缺列和默认值都可能表现为 hasVideo==false；直接查 schema 才能证明补列确实发生。
        QSet<QString> columns;
        {
            SqlFixture probe(path);
            QVERIFY(probe.isOpen());
            QVERIFY(probe.columnTexts(QStringLiteral("PRAGMA table_info(messages)"), 1, columns));
        }
        for (const QString& expected : {QStringLiteral("has_video"), QStringLiteral("video_path"),
                                        QStringLiteral("video_name"), QStringLiteral("video_cover"),
                                        QStringLiteral("video_seconds"), QStringLiteral("video_bytes"),
                                        QStringLiteral("approval_token"), QStringLiteral("approval_actions")}) {
            QVERIFY2(columns.contains(expected), qPrintable(QStringLiteral("缺列 %1").arg(expected)));
        }

        RemoteIMMessage video;
        video.id = QStringLiteral("video-new");
        video.fromUserId = QStringLiteral("me");
        video.toUserId = QStringLiteral("peer");
        video.direction = RemoteIMMessageDirection::Outgoing;
        video.status = RemoteIMMessageStatus::Sent;
        video.createdAtMillis = 1700000000000LL;
        // text 必须显式给：默认构造的 QString 是 null，会绑成 SQL NULL 撞上
        // text NOT NULL，而 INSERT OR IGNORE 会把约束冲突静默吞掉。
        video.text = QStringLiteral("[视频消息] a.mp4");
        video.hasVideo = true;
        video.video = RemoteIMVideoAttachment{QStringLiteral("E:/clips/a.mp4"),
                                              QStringLiteral("a.mp4"),
                                              QStringLiteral("E:/covers/a.jpg"), 7, 1024LL};
        QVERIFY(db.insertMessageIfAbsent(video, QStringLiteral("peer")));
    }

    // 3) 再开一次：迁移必须幂等（重复 ALTER 会报 duplicate column），两条都在。
    {
        LocalMessageDatabase db(path);
        QVERIFY(db.isOpen());
        ChatState restored(QStringLiteral("me"));
        db.loadInto(restored);
        const QList<RemoteIMMessage> messages = restored.messagesWith(QStringLiteral("peer"));
        QCOMPARE(messages.size(), 2);
        const RemoteIMMessage& video = messages.last();
        QVERIFY(video.hasVideo);
        QCOMPARE(video.video.coverPath, QStringLiteral("E:/covers/a.jpg"));
        QCOMPARE(video.video.durationSeconds, 7);
    }
}

namespace {

// 找出某个联系人当前落在哪个分组（空串 = 未分组）。
QString groupOf(const LocalMessageDatabase& db, const QString& userId) {
    ChatState state("me");
    db.loadInto(state);
    for (const RemoteIMContact& contact : state.contacts()) {
        if (contact.userId == userId) return contact.groupName;
    }
    return QStringLiteral("<absent>");
}

}  // namespace

void LocalMessageDatabaseTest::createsGroupsAndRejectsBadNames() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));

    QVERIFY(db.createContactGroup("同事"));
    QVERIFY(db.createContactGroup("家人"));
    // 重名：分组名是主键，第二次必须失败而不是悄悄建出两个同名组。
    QVERIFY(!db.createContactGroup("同事"));
    // 首尾空白归一化之后同样算重名。
    QVERIFY(!db.createContactGroup("  同事  "));
    QVERIFY(!db.createContactGroup(""));
    QVERIFY(!db.createContactGroup("   "));
    // 界面上没有「未分组」这一节，所以它也不是保留名——建一个叫这名字的组
    // 只是个普通分组，没有任何东西会跟它撞。
    QVERIFY(db.createContactGroup("未分组"));

    // 创建顺序即显示顺序。
    QCOMPARE(db.contactGroups(), QStringList({"同事", "家人", "未分组"}));
}

void LocalMessageDatabaseTest::movesContactBetweenGroupsAndPersists() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    {
        LocalMessageDatabase db(path);
        db.upsertContact(RemoteIMContact{"alice", "Alice", ""});
        QVERIFY(db.createContactGroup("同事"));
        db.setContactGroup("alice", "同事");
        QCOMPARE(groupOf(db, "alice"), QStringLiteral("同事"));

        // 不存在的分组不会被凭空造出来，联系人落回未分组。
        db.setContactGroup("alice", "查无此组");
        QCOMPARE(groupOf(db, "alice"), QString());
        QCOMPARE(db.contactGroups(), QStringList({"同事"}));

        db.setContactGroup("alice", "同事");
        // 空串就是「移出到未分组」。
        db.setContactGroup("alice", "");
        QCOMPARE(groupOf(db, "alice"), QString());

        db.setContactGroup("alice", "同事");
    }
    // 重开库仍在原分组：分组是落库的，不是只活在内存里。
    LocalMessageDatabase reopened(path);
    QCOMPARE(reopened.contactGroups(), QStringList({"同事"}));
    QCOMPARE(groupOf(reopened, "alice"), QStringLiteral("同事"));
}

void LocalMessageDatabaseTest::deletingGroupKeepsContactsAndUngroupsThem() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.upsertContact(RemoteIMContact{"alice", "Alice", ""});
    db.upsertContact(RemoteIMContact{"bob", "Bob", ""});
    QVERIFY(db.createContactGroup("同事"));
    db.setContactGroup("alice", "同事");
    db.setContactGroup("bob", "同事");

    db.deleteContactGroup("同事");

    // 删组绝不删人——这是那种一旦发生就无法挽回的事。
    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.contacts().size(), 2);
    QCOMPARE(groupOf(db, "alice"), QString());
    QCOMPARE(groupOf(db, "bob"), QString());
    QVERIFY(db.contactGroups().isEmpty());
}

void LocalMessageDatabaseTest::renamingGroupCarriesItsMembers() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.upsertContact(RemoteIMContact{"alice", "Alice", ""});
    QVERIFY(db.createContactGroup("同时"));
    QVERIFY(db.createContactGroup("家人"));
    db.setContactGroup("alice", "同时");

    QVERIFY(db.renameContactGroup("同时", "同事"));
    QCOMPARE(db.contactGroups(), QStringList({"同事", "家人"}));
    // 成员跟着改名走，不会被落在一个已经不存在的组里。
    QCOMPARE(groupOf(db, "alice"), QStringLiteral("同事"));

    // 改成一个已存在的名字必须整笔失败，不能把两个组悄悄合并。
    QVERIFY(!db.renameContactGroup("同事", "家人"));
    QCOMPARE(db.contactGroups(), QStringList({"同事", "家人"}));
    QCOMPARE(groupOf(db, "alice"), QStringLiteral("同事"));

    QVERIFY(!db.renameContactGroup("同事", "   "));
    QVERIFY(!db.renameContactGroup("查无此组", "新名"));
}

void LocalMessageDatabaseTest::contactProfileRefreshKeepsLocalGroup() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    db.upsertContact(RemoteIMContact{"alice", "Alice", ""});
    QVERIFY(db.createContactGroup("同事"));
    db.setContactGroup("alice", "同事");

    // SDK 拉到的资料对象里从来没有分组信息。若 upsert 跟着覆盖，
    // 用户分好的组会在下一次资料刷新时被静默清空。
    db.upsertContact(RemoteIMContact{"alice", "Alice Chen", "https://cdn/a.png"});

    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().displayName, QStringLiteral("Alice Chen"));
    QCOMPARE(state.contacts().first().avatarUrl, QStringLiteral("https://cdn/a.png"));
    QCOMPARE(state.contacts().first().groupName, QStringLiteral("同事"));
}

void LocalMessageDatabaseTest::healsContactPointingAtMissingGroup() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    {
        LocalMessageDatabase db(path);
        db.upsertContact(RemoteIMContact{"alice", "Alice", ""});
        QVERIFY(db.createContactGroup("同事"));
        db.setContactGroup("alice", "同事");
    }
    // 绕过接口直接把分组删掉，模拟历史数据或半途失败留下的悬空引用。
    {
        SqlFixture raw(path);
        QVERIFY(raw.isOpen());
        QVERIFY(raw.exec("DELETE FROM contact_groups WHERE name = '同事'"));
    }

    // 读取时自愈成未分组：界面上不会冒出一个点不开、删不掉的幽灵分组。
    LocalMessageDatabase db(path);
    QCOMPARE(groupOf(db, "alice"), QString());
}

void LocalMessageDatabaseTest::upgradesPreGroupDatabaseWithoutLosingContacts() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    // 手工造一个分组功能之前的库：contacts 只有三列，没有 contact_groups 表。
    {
        SqlFixture legacy(path);
        QVERIFY(legacy.isOpen());
        QVERIFY(legacy.exec("CREATE TABLE contacts (user_id TEXT PRIMARY KEY,"
                           " display_name TEXT NOT NULL, avatar_url TEXT NOT NULL DEFAULT '')"));
        QVERIFY(legacy.exec("INSERT INTO contacts VALUES ('alice', 'Alice', '')"));
    }

    // 升级靠 ALTER 加列，不重建表——重建会把已有联系人一起丢掉。
    LocalMessageDatabase db(path);
    QVERIFY(db.isOpen());
    ChatState state("me");
    db.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().displayName, QStringLiteral("Alice"));
    QCOMPARE(state.contacts().first().groupName, QString());
    // 新表也补上了，可以正常建组。
    QVERIFY(db.createContactGroup("同事"));
    db.setContactGroup("alice", "同事");
    QCOMPARE(groupOf(db, "alice"), QStringLiteral("同事"));
}

// UTF-8 byte lengths and SQL NULL are independent of QString character counts.
// Check nullable fields through raw SQL as well as DTOs so a lossy conversion
// cannot pass by normalizing both sides of the assertion to an empty string.
void LocalMessageDatabaseTest::roundTripsUnicodeNullEmptyAndFullWidthIntegers() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("消息📂/聊天.sqlite"));
    LocalMessageDatabase db(path);
    QVERIFY(db.isOpen());
    const QString peer = QStringLiteral("对端'🙂");
    const qint64 large = std::numeric_limits<qint64>::max() - 2;
    const QString text = QStringLiteral("引号'🙂 é ") + QChar(0) + QStringLiteral(" 后段");
    RemoteIMMessage message = makeTextMessage(QStringLiteral("unicode'🧪"), peer, "me",
        RemoteIMMessageDirection::Incoming, large, text);
    message.hasImage = message.hasVoice = message.hasFile = message.hasVideo = true;
    message.captionAbove = true;
    message.image = {QString(), 640, 480, large - 1};
    message.voice = {QStringLiteral("声音/你好🙂.m4a"), 17};
    message.file = {QStringLiteral("文件/你好.md"), QStringLiteral(""), "text/markdown", large - 2};
    message.video = {QStringLiteral("视频/片段.mp4"), QStringLiteral("片段🙂.mp4"),
        QStringLiteral("封面/一.jpg"), 42, large - 3};
    message.hasQuote = true;
    message.quote = {QStringLiteral(""), peer, QStringLiteral("引用\n🙂"), QStringLiteral("image")};
    QVERIFY(db.insertMessageIfAbsent(message, peer));
    const auto rows = db.loadRecentMessages(peer, 2);
    QCOMPARE(rows.size(), 1);
    const auto& loaded = rows.first();
    QCOMPARE(loaded.id, message.id);
    QCOMPARE(loaded.text, text);
    QCOMPARE(loaded.createdAtMillis, large);
    QVERIFY(loaded.image.localPath.isNull());
    QCOMPARE(loaded.image.width, 640);
    QCOMPARE(loaded.image.height, 480);
    QCOMPARE(loaded.image.sizeBytes, large - 1);
    QCOMPARE(loaded.voice.localPath, message.voice.localPath);
    QCOMPARE(loaded.voice.durationSeconds, 17);
    QVERIFY(!loaded.file.fileName.isNull());
    QVERIFY(loaded.file.fileName.isEmpty());
    QCOMPARE(loaded.file.localPath, message.file.localPath);
    QCOMPARE(loaded.file.mimeType, message.file.mimeType);
    QCOMPARE(loaded.file.sizeBytes, large - 2);
    QCOMPARE(loaded.video.localPath, message.video.localPath);
    QCOMPARE(loaded.video.fileName, message.video.fileName);
    QCOMPARE(loaded.video.coverPath, message.video.coverPath);
    QCOMPARE(loaded.video.durationSeconds, 42);
    QCOMPARE(loaded.video.sizeBytes, large - 3);
    QVERIFY(loaded.captionAbove && loaded.hasImage && loaded.hasVoice && loaded.hasFile && loaded.hasVideo);
    QVERIFY(loaded.hasQuote);
    QCOMPARE(loaded.quote.msgId, QStringLiteral(""));
    QCOMPARE(loaded.quote.senderId, peer);
    QCOMPARE(loaded.quote.digest, message.quote.digest);
    QCOMPARE(loaded.quote.kind, message.quote.kind);

    SqlFixture raw(path);
    QVERIFY(raw.isOpen());
    QCOMPARE(raw.integer("SELECT image_path IS NULL FROM messages"), qint64(1));
    QCOMPARE(raw.integer("SELECT typeof(file_name)='text' AND length(file_name)=0 FROM messages"), qint64(1));
    QCOMPARE(raw.integer("SELECT typeof(created_at)='integer' FROM messages"), qint64(1));

    auto empty = makeTextMessage("empty-text", peer, "me", RemoteIMMessageDirection::Incoming,
        20, QStringLiteral(""));
    QVERIFY(db.insertMessageIfAbsent(empty, peer));
    auto null = empty;
    null.id = QStringLiteral("null-text");
    null.text = QString();
    QVERIFY(!db.insertMessageIfAbsent(null, peer));  // INSERT OR IGNORE must preserve NOT NULL semantics.
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM messages"), qint64(2));
    const auto emptyRow = db.loadMessagesBefore(peer, 21, "~", 1);
    QCOMPARE(emptyRow.size(), 1);
    QVERIFY(emptyRow.first().text.isEmpty() && !emptyRow.first().text.isNull());

    const RemoteIMGroupMessage groupMessage{QStringLiteral("群'🧪"), QStringLiteral("群🙂"), peer,
        text, large, std::numeric_limits<qint64>::min() + 3, true, true, false};
    QVERIFY(db.insertGroupMessageIfAbsent(groupMessage));
    auto groupRows = db.groupMessagesBefore(groupMessage.groupId,
        std::numeric_limits<qint64>::max(), "~", 1);
    QCOMPARE(groupRows.size(), 1);
    QCOMPARE(groupRows.first().text, text);
    QCOMPARE(groupRows.first().createdAtMillis, large);
    QCOMPARE(groupRows.first().sequence, groupMessage.sequence);
    QVERIFY(groupRows.first().fromSelf && groupRows.first().pending && !groupRows.first().failed);
    db.updateGroupMessageStatus(groupMessage.id, false, true);
    groupRows = db.groupMessagesBefore(groupMessage.groupId,
        std::numeric_limits<qint64>::max(), "~", 1);
    QVERIFY(!groupRows.first().pending && groupRows.first().failed);
}

void LocalMessageDatabaseTest::rejectsInvalidDatabasePath() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.path()); // sqlite3 cannot open a directory as a DB file.
    QVERIFY(!db.isOpen());
    QVERIFY(!db.createContactGroup("safe"));
    QVERIFY(db.contactGroups().isEmpty());
    QVERIFY(db.loadRecentMessages("peer", 1).isEmpty());
}

void LocalMessageDatabaseTest::cascadeFailureRollsBackBothDeletes() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    LocalMessageDatabase db(path);
    db.upsertContact({"peer", "Peer"});
    QVERIFY(db.insertMessageIfAbsent(makeTextMessage("keep", "peer", "me",
        RemoteIMMessageDirection::Incoming, 1, "keep me"), "peer"));
    SqlFixture raw(path);
    QVERIFY(raw.exec("CREATE TRIGGER reject_delete BEFORE DELETE ON contacts "
                     "BEGIN SELECT RAISE(ABORT, 'fixture rejects second delete'); END"));
    db.removeContactCascade("peer");
    QCOMPARE(db.loadRecentMessages("peer", 10).size(), 1);
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM contacts"), qint64(1));
    QVERIFY(raw.exec("DROP TRIGGER reject_delete"));
    db.removeContactCascade("peer");
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM contacts"), qint64(0));
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM messages"), qint64(0));
}

void LocalMessageDatabaseTest::adoptionFailureKeepsTemporaryMessages() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    LocalMessageDatabase db(path);
    QVERIFY(db.insertMessageIfAbsent(makeTextMessage("temporary", "me", "peer",
        RemoteIMMessageDirection::Outgoing, 1, "keep until acknowledged"), "peer"));
    SqlFixture raw(path);
    QVERIFY(raw.exec("CREATE TRIGGER reject_adoption BEFORE UPDATE OF id ON messages "
                     "BEGIN SELECT RAISE(ABORT, 'fixture rejects id change'); END"));
    db.adoptMessageId("temporary", "stable");
    auto rows = db.loadRecentMessages("peer", 10);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().id, QStringLiteral("temporary"));
    QVERIFY(raw.exec("DROP TRIGGER reject_adoption"));
    db.adoptMessageId("temporary", "stable");
    rows = db.loadRecentMessages("peer", 10);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().id, QStringLiteral("stable"));
}

void LocalMessageDatabaseTest::groupAdoptionFailureKeepsTemporaryMessages() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    LocalMessageDatabase db(path);
    const RemoteIMGroupMessage temporary{"temp", "group", "me", "keep", 1, 0, true, true, false};
    QVERIFY(db.insertGroupMessageIfAbsent(temporary));
    SqlFixture raw(path);
    QVERIFY(raw.exec("CREATE TRIGGER reject_adoption BEFORE UPDATE OF id ON im_group_messages "
                     "BEGIN SELECT RAISE(ABORT, 'fixture rejects group id change'); END"));
    db.adoptGroupMessageId("temp", "stable", 20, 20000000000000001LL);
    auto rows = db.groupMessagesBefore("group", 30, "~", 10);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().id, QStringLiteral("temp"));
    QCOMPARE(rows.first().createdAtMillis, qint64(1));
    QVERIFY(rows.first().pending);
    QVERIFY(raw.exec("DROP TRIGGER reject_adoption"));
    db.adoptGroupMessageId("temp", "stable", 20, 20000000000000001LL);
    rows = db.groupMessagesBefore("group", 30, "~", 10);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().id, QStringLiteral("stable"));
    QCOMPARE(rows.first().createdAtMillis, qint64(20));
    QCOMPARE(rows.first().sequence, qint64(20000000000000001LL));
    QVERIFY(!rows.first().pending);
    QVERIFY(db.insertGroupMessageIfAbsent(temporary));
    db.adoptGroupMessageId("temp", "stable", 25, 99);
    QCOMPARE(db.groupMessageCount("group"), 1);
    rows = db.groupMessagesBefore("group", 30, "~", 10);
    QCOMPARE(rows.first().sequence, qint64(20000000000000001LL));
}

void LocalMessageDatabaseTest::groupStepFailuresRollBackMembers() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    LocalMessageDatabase db(path);
    db.upsertContact({"peer", "Peer"});
    QVERIFY(db.createContactGroup("old"));
    db.setContactGroup("peer", "old");
    SqlFixture raw(path);
    QVERIFY(raw.exec("CREATE TRIGGER reject_move BEFORE UPDATE OF group_name ON contacts "
                     "BEGIN SELECT RAISE(ABORT, 'fixture rejects move'); END"));
    QVERIFY(!db.renameContactGroup("old", "new"));
    QCOMPARE(db.contactGroups(), QStringList({"old"}));
    QCOMPARE(groupOf(db, "peer"), QStringLiteral("old"));
    QVERIFY(raw.exec("DROP TRIGGER reject_move"));
    QVERIFY(raw.exec("CREATE TRIGGER reject_drop BEFORE DELETE ON contact_groups "
                     "BEGIN SELECT RAISE(ABORT, 'fixture rejects group delete'); END"));
    db.deleteContactGroup("old");
    QCOMPARE(db.contactGroups(), QStringList({"old"}));
    QCOMPARE(groupOf(db, "peer"), QStringLiteral("old"));
    QVERIFY(raw.exec("DROP TRIGGER reject_drop"));
    QVERIFY(db.renameContactGroup("old", "new"));
    QCOMPARE(groupOf(db, "peer"), QStringLiteral("new"));
}

void LocalMessageDatabaseTest::commitFailureRollsBackAndKeepsConnectionUsable() {
    QTemporaryDir dir;
    const QString path = dir.filePath("messages.db");
    LocalMessageDatabase db(path);
    db.upsertContact({"peer", "Peer"});
    QVERIFY(db.createContactGroup("old"));
    db.setContactGroup("peer", "old");
    // A child process holds the SHARED lock: the Qt baseline driver and our
    // vendored sqlite library must contend through the OS, not two SQLite copies
    // whose per-process POSIX-lock bookkeeping cannot coordinate with each other.
    QProcess reader;
    reader.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-storage-read-lock"), path});
    const auto releaseReader = qScopeGuard([&] {
        if (reader.state() != QProcess::NotRunning) {
            reader.write("\n");
            reader.closeWriteChannel();
            reader.waitForFinished(5000);
        }
    });
    QVERIFY(reader.waitForStarted(5000));
    QVERIFY(reader.waitForReadyRead(5000));
    QCOMPARE(reader.readLine().trimmed(), QByteArray("locked"));
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!db.renameContactGroup("old", "new"));
    // Match the old QSQLITE driver's 5000 ms busy timeout, rather than silently
    // switching the native implementation to immediate SQLITE_BUSY failures.
    QVERIFY(elapsed.elapsed() >= 4500);
    QVERIFY(elapsed.elapsed() < 15000);
    QCOMPARE(db.contactGroups(), QStringList({"old"}));
    QCOMPARE(groupOf(db, "peer"), QStringLiteral("old"));
    reader.write("\n");
    reader.closeWriteChannel();
    QVERIFY(reader.waitForFinished(5000));
    QCOMPARE(reader.exitCode(), 0);
    QVERIFY(db.createContactGroup("after-failed-commit"));
    QVERIFY(db.renameContactGroup("old", "new"));
    QCOMPARE(groupOf(db, "peer"), QStringLiteral("new"));
}

void LocalMessageDatabaseTest::failedMigrationPreservesOldSchema() {
    QTemporaryDir dir;
    const QString path = dir.filePath("legacy.db");
    SqlFixture raw(path);
    QVERIFY(raw.exec("CREATE TABLE contacts(user_id TEXT PRIMARY KEY, display_name TEXT NOT NULL)"));
    QVERIFY(raw.exec("INSERT INTO contacts VALUES('legacy', '旧联系人🙂')"));
    // The last CREATE INDEX must fail on this view, after all earlier DDL has
    // already run. The old contact row and original two-column schema must survive.
    QVERIFY(raw.exec("CREATE VIEW im_group_messages AS SELECT 'blocked' AS id"));
    {
        LocalMessageDatabase rejected(path);
        QVERIFY(!rejected.isOpen());
        QVERIFY(!rejected.createContactGroup("must-not-write"));
    }
    QSet<QString> columns;
    QVERIFY(raw.columnTexts("PRAGMA table_info(contacts)", 1, columns));
    QCOMPARE(columns, QSet<QString>({"user_id", "display_name"}));
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM contacts WHERE display_name='旧联系人🙂'"), qint64(1));
    QCOMPARE(raw.integer("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='messages'"), qint64(0));
    QVERIFY(raw.exec("DROP VIEW im_group_messages"));
    LocalMessageDatabase recovered(path);
    QVERIFY(recovered.isOpen());
    ChatState state("me");
    recovered.loadInto(state);
    QCOMPARE(state.contacts().size(), 1);
    QCOMPARE(state.contacts().first().displayName, QStringLiteral("旧联系人🙂"));
}

void LocalMessageDatabaseTest::pagesTimestampTiesAndRejectsEmptyBounds() {
    QTemporaryDir dir;
    LocalMessageDatabase db(dir.filePath("messages.db"));
    for (const QString& id : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                             QStringLiteral("d"), QStringLiteral("e")}) {
        QVERIFY(db.insertMessageIfAbsent(makeTextMessage(id, "peer", "me",
            RemoteIMMessageDirection::Incoming, 100, id), "peer"));
    }
    const auto recent = db.loadRecentMessages("peer", 3);
    QCOMPARE(recent.size(), 3);
    QCOMPARE(recent.first().id, QStringLiteral("c"));
    const auto earlier = db.loadMessagesBefore("peer", 100, "c", 3);
    QCOMPARE(earlier.size(), 2);
    QCOMPARE(earlier.first().id, QStringLiteral("a"));
    QCOMPARE(earlier.last().id, QStringLiteral("b"));
    QVERIFY(db.loadRecentMessages("peer", 0).isEmpty());
    QVERIFY(db.loadRecentMessages(QString(), 5).isEmpty());
    QVERIFY(db.loadMessagesBefore("peer", 100, "a", 5).isEmpty());
    QVERIFY(db.loadMessagesBefore("peer", 100, "c", -1).isEmpty());
    db.updateMessageTime("a", 0);
    db.updateMessageTime("b", -1);
    QCOMPARE(db.loadRecentMessages("peer", 5).first().createdAtMillis, qint64(100));
    db.updateMessageTime("a", 20000000000000001LL);
    QCOMPARE(db.loadRecentMessages("peer", 5).last().createdAtMillis, qint64(20000000000000001LL));
    db.removeMessagesForPeer("peer");
    QVERIFY(db.loadRecentMessages("peer", 5).isEmpty());
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--hold-storage-read-lock") {
        SqlFixture reader(application.arguments().at(2));
        if (!reader.exec("BEGIN") || !reader.exec("SELECT * FROM contact_groups")) return 2;
        std::puts("locked");
        std::fflush(stdout);
        std::getc(stdin);
        return reader.exec("ROLLBACK") ? 0 : 3;
    }
    LocalMessageDatabaseTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "LocalMessageDatabaseTest.moc"
