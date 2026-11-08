// 文件作用：桌面端本地数据持久化；LocalMessageDatabase 负责数据库结构与读写，本文件给出其实现。
// 主要入口：isOpen、migrate、loadInto、loadRecentInto。
// 直接依赖：vendored sqlite3、QtCore DTO、PerformanceLog 和 ContactGroups；不加载 Qt SQL 插件。
#include "diagnostics/PerformanceLog.h"
#include "storage/LocalMessageDatabase.h"

#include "model/ContactGroups.h"

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <sqlite3.h>

#include <limits>

namespace {

// 只适配本库使用的值和单条 SQL。保留原语句/位置绑定顺序，并由析构归还所有 cursor/锁。
// exec() 预读的首行留给 next()，避免 SELECT COUNT/EXISTS 或第一页丢掉第一条记录。
class Statement {
public:
    explicit Statement(sqlite3* database) : database_(database) {}
    ~Statement() { if (statement_) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool prepare(const QString& sql) {
        if (statement_) sqlite3_finalize(statement_);
        statement_ = nullptr;
        columns_.clear();
        binding_ = 0;
        affected_ = 0;
        pendingRow_ = currentRow_ = false;
        const QByteArray bytes = sql.toUtf8();
        valid_ = database_ && bytes.size() <= std::numeric_limits<int>::max() &&
            sqlite3_prepare_v2(database_, bytes.constData(), int(bytes.size()), &statement_, nullptr) == SQLITE_OK &&
            statement_;
        if (valid_) {
            for (int index = 0; index < sqlite3_column_count(statement_); ++index)
                columns_.insert(QString::fromUtf8(sqlite3_column_name(statement_, index)), index);
        }
        return valid_;
    }
    void addBindValue(const QString& value) {
        if (!valid_) return;
        const int index = ++binding_;
        if (value.isNull()) {
            valid_ = sqlite3_bind_null(statement_, index) == SQLITE_OK;
            return;
        }
        const QByteArray bytes = value.toUtf8();
        // QByteArray::constData() 非空，即使字节数为 0 也会写 SQL TEXT 而非 NULL。
        // 显式长度保留 Unicode/内嵌 NUL；TRANSIENT 在局部 QByteArray 销毁前完成复制。
        valid_ = bytes.size() <= std::numeric_limits<int>::max() &&
            sqlite3_bind_text(statement_, index, bytes.constData(), int(bytes.size()), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    void addBindValue(qint64 value) {
        if (valid_) valid_ = sqlite3_bind_int64(statement_, ++binding_, sqlite3_int64(value)) == SQLITE_OK;
    }
    bool exec(const QString& sql) { return prepare(sql) && exec(); }
    bool exec() {
        if (!valid_ || binding_ != sqlite3_bind_parameter_count(statement_)) return false;
        const int result = sqlite3_step(statement_);
        pendingRow_ = result == SQLITE_ROW;
        currentRow_ = false;
        valid_ = result == SQLITE_ROW || result == SQLITE_DONE;
        if (result == SQLITE_DONE) affected_ = sqlite3_changes(database_);
        return valid_;
    }
    bool next() {
        if (!valid_) return false;
        if (pendingRow_) {
            pendingRow_ = false;
            currentRow_ = true;
            return true;
        }
        if (!currentRow_) return false;
        const int result = sqlite3_step(statement_);
        currentRow_ = result == SQLITE_ROW;
        valid_ = result == SQLITE_ROW || result == SQLITE_DONE;
        return currentRow_;
    }
    QVariant value(int column) const {
        if (!currentRow_ || column < 0 || column >= sqlite3_column_count(statement_)) return {};
        switch (sqlite3_column_type(statement_, column)) {
            case SQLITE_INTEGER: return QVariant::fromValue(qint64(sqlite3_column_int64(statement_, column)));
            case SQLITE_FLOAT: return sqlite3_column_double(statement_, column);
            case SQLITE_TEXT: {
                const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
                return QString::fromUtf8(text, sqlite3_column_bytes(statement_, column));
            }
            default: return {};
        }
    }
    QVariant value(const QString& name) const { return value(columns_.value(name, -1)); }
    int numRowsAffected() const { return affected_; }
    bool succeeded() const { return valid_; }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
    QHash<QString, int> columns_;
    int binding_ = 0;
    int affected_ = 0;
    bool valid_ = false;
    bool pendingRow_ = false;
    bool currentRow_ = false;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {
        // 先拿写锁再运行第一条业务语句；BEGIN 失败不能继续变成逐语句 autocommit。
        active_ = database_ && sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    ~Transaction() {
        // COMMIT 忙等/完整性失败后事务仍可能活着；所有失败出口都显式回滚，之后连接还能使用。
        if (active_) sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    explicit operator bool() const { return active_; }
    bool commit() {
        if (!active_ || sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
        active_ = false;
        return true;
    }
private:
    sqlite3* database_;
    bool active_ = false;
};

// 联系人读取统一走这一句：LEFT JOIN 让「指向已不存在的分组」在读取时自动降级成
// 未分组，不必写库修数据，也不会因为一次删组失败就让联系人挂在幽灵分组上。
const auto kSelectContactsSql = QStringLiteral(
    "SELECT c.user_id, c.display_name, c.avatar_url,"
    "       CASE WHEN g.name IS NULL THEN '' ELSE c.group_name END "
    "FROM contacts c LEFT JOIN contact_groups g ON g.name = c.group_name "
    "ORDER BY c.user_id");

QString approvalActionsText(const QList<RemoteIMApprovalAction>& actions) {
    QStringList values;
    for (RemoteIMApprovalAction action : actions) {
        values.append(remoteIMApprovalActionWireName(action));
    }
    return values.join(QLatin1Char(','));
}

QList<RemoteIMApprovalAction> approvalActionsFromText(const QString& text) {
    QList<RemoteIMApprovalAction> actions;
    for (const QString& value : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        RemoteIMApprovalAction action;
        if (!remoteIMApprovalActionFromWireName(value, &action)) return {};
        actions.append(action);
    }
    return actions;
}

RemoteIMMessage messageFromQuery(const Statement& query) {
    RemoteIMMessage message;
    message.id = query.value(QStringLiteral("id")).toString();
    message.fromUserId = query.value(QStringLiteral("from_user")).toString();
    message.toUserId = query.value(QStringLiteral("to_user")).toString();
    message.direction = query.value(QStringLiteral("direction")).toInt() == 1
                            ? RemoteIMMessageDirection::Outgoing
                            : RemoteIMMessageDirection::Incoming;
    message.status = static_cast<RemoteIMMessageStatus>(query.value(QStringLiteral("status")).toInt());
    message.text = query.value(QStringLiteral("text")).toString();
    message.createdAtMillis = query.value(QStringLiteral("created_at")).toLongLong();
    message.hasImage = query.value(QStringLiteral("has_image")).toInt() != 0;
    message.captionAbove = query.value(QStringLiteral("caption_above")).toInt() != 0;
    // 引用块。digest 是判据而不是 msgId：msgId 允许缺失（本地消息没有服务端 ID），
    // 但没有摘要的引用块会渲染成空白，那比没有引用更难看，所以按摘要判定「有没有引用」。
    message.quote.msgId = query.value(QStringLiteral("quote_msg_id")).toString();
    message.quote.senderId = query.value(QStringLiteral("quote_sender")).toString();
    message.quote.digest = query.value(QStringLiteral("quote_digest")).toString();
    message.quote.kind = query.value(QStringLiteral("quote_kind")).toString();
    message.hasQuote = !message.quote.digest.isEmpty();
    message.image = RemoteIMImageAttachment{
        query.value(QStringLiteral("image_path")).toString(),
        query.value(QStringLiteral("image_w")).toInt(),
        query.value(QStringLiteral("image_h")).toInt(),
        query.value(QStringLiteral("image_bytes")).toLongLong()
    };
    message.hasVoice = query.value(QStringLiteral("has_voice")).toInt() != 0;
    message.voice = RemoteIMVoiceAttachment{
        query.value(QStringLiteral("voice_path")).toString(),
        query.value(QStringLiteral("voice_seconds")).toInt()
    };
    message.hasFile = query.value(QStringLiteral("has_file")).toInt() != 0;
    message.file = RemoteIMFileAttachment{
        query.value(QStringLiteral("file_path")).toString(),
        query.value(QStringLiteral("file_name")).toString(),
        query.value(QStringLiteral("file_mime")).toString(),
        query.value(QStringLiteral("file_bytes")).toLongLong()
    };
    message.hasVideo = query.value(QStringLiteral("has_video")).toInt() != 0;
    message.video = RemoteIMVideoAttachment{
        query.value(QStringLiteral("video_path")).toString(),
        query.value(QStringLiteral("video_name")).toString(),
        query.value(QStringLiteral("video_cover")).toString(),
        query.value(QStringLiteral("video_seconds")).toInt(),
        query.value(QStringLiteral("video_bytes")).toLongLong()
    };
    const QString approvalToken = query.value(QStringLiteral("approval_token")).toString();
    const QList<RemoteIMApprovalAction> approvalActions =
        approvalActionsFromText(query.value(QStringLiteral("approval_actions")).toString());
    message.approvalRequest = RemoteIMApprovalRequest{approvalToken, approvalActions};
    message.hasApprovalRequest = message.approvalRequest.isValid();
    if (!message.hasApprovalRequest && approvalActions.size() == 1) {
        message.approvalDecision = RemoteIMApprovalDecision{
            approvalToken,
            approvalActions.first()
        };
        message.hasApprovalDecision = message.approvalDecision.isValid();
    }
    return message;
}

}  // namespace

LocalMessageDatabase::LocalMessageDatabase(const QString& dbFilePath) {
    if (dbFilePath.contains(QChar(0))) return;
    if (!QDir().mkpath(QFileInfo(dbFilePath).absolutePath())) return;
    const QByteArray path = dbFilePath.toUtf8();
    // 与原 QSQLITE 驱动保持私有 cache、可创建和 5000ms 锁等待，不改变 journal/schema。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.constData(), &db_, flags, nullptr) != SQLITE_OK ||
        sqlite3_busy_timeout(db_, 5000) != SQLITE_OK ||
        sqlite3_extended_result_codes(db_, 1) != SQLITE_OK || !migrate()) {
        if (db_) sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

LocalMessageDatabase::~LocalMessageDatabase() {
    if (db_) sqlite3_close_v2(db_);
}

bool LocalMessageDatabase::isOpen() const {
    return db_ != nullptr;
}

// 作用：按数据库版本迁移本地消息表并保留旧记录。
bool LocalMessageDatabase::migrate() {
    Transaction transaction(db_);
    if (!transaction) return false;
    Statement query(db_);
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS contacts ("
        "  user_id      TEXT PRIMARY KEY,"
        "  display_name TEXT NOT NULL,"
        "  avatar_url   TEXT NOT NULL DEFAULT ''"
        ")"))) return false;
    bool hasAvatarUrl = false;
    if (!query.exec(QStringLiteral("PRAGMA table_info(contacts)"))) return false;
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("avatar_url")) {
            hasAvatarUrl = true;
            break;
        }
    }
    if (!query.succeeded()) return false;
    if (!hasAvatarUrl) {
        if (!query.exec(QStringLiteral(
            "ALTER TABLE contacts ADD COLUMN avatar_url TEXT NOT NULL DEFAULT ''"))) return false;
    }
    // 分组同样按「PRAGMA 判存在再 ALTER」加列，不重建表——重建会丢已有联系人。
    bool hasGroupName = false;
    if (!query.exec(QStringLiteral("PRAGMA table_info(contacts)"))) return false;
    while (query.next()) {
        if (query.value(1).toString() == QStringLiteral("group_name")) {
            hasGroupName = true;
            break;
        }
    }
    if (!query.succeeded()) return false;
    if (!hasGroupName) {
        if (!query.exec(QStringLiteral(
            "ALTER TABLE contacts ADD COLUMN group_name TEXT NOT NULL DEFAULT ''"))) return false;
    }
    // 分组单独建表而不是从 contacts.group_name 里隐式推导：推导表示不出「空分组」，
    // 而用户新建一个分组、还没往里放人时，它必须看得见，否则「新建分组」看起来什么都没发生。
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS contact_groups ("
        "  name       TEXT PRIMARY KEY,"
        "  sort_order INTEGER NOT NULL DEFAULT 0"
        ")"))) return false;
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id            TEXT PRIMARY KEY,"
        "  from_user     TEXT NOT NULL,"
        "  to_user       TEXT NOT NULL,"
        "  peer          TEXT NOT NULL,"
        "  direction     INTEGER NOT NULL,"
        "  status        INTEGER NOT NULL,"
        "  text          TEXT NOT NULL DEFAULT '',"
        "  created_at    INTEGER NOT NULL,"
        "  has_image     INTEGER NOT NULL DEFAULT 0,"
        "  image_path    TEXT, image_w INTEGER, image_h INTEGER, image_bytes INTEGER,"
        "  has_voice     INTEGER NOT NULL DEFAULT 0,"
        "  voice_path    TEXT, voice_seconds INTEGER,"
        "  has_file      INTEGER NOT NULL DEFAULT 0,"
        "  file_path     TEXT, file_name TEXT, file_mime TEXT, file_bytes INTEGER,"
        "  has_video     INTEGER NOT NULL DEFAULT 0,"
        "  video_path    TEXT, video_name TEXT, video_cover TEXT,"
        "  video_seconds INTEGER, video_bytes INTEGER,"
        "  approval_token TEXT, approval_actions TEXT,"
        "  caption_above INTEGER NOT NULL DEFAULT 0,"
        "  quote_msg_id TEXT, quote_sender TEXT, quote_digest TEXT, quote_kind TEXT"
        ")"))) return false;
    // 老库（建于视频功能之前）没有这几列，CREATE TABLE IF NOT EXISTS 不会补。
    // 与 contacts.avatar_url 一样按 PRAGMA 判存在再 ALTER，重复启动无副作用。
    QSet<QString> messageColumns;
    if (!query.exec(QStringLiteral("PRAGMA table_info(messages)"))) return false;
    while (query.next()) messageColumns.insert(query.value(1).toString());
    if (!query.succeeded()) return false;
    const QList<QPair<QString, QString>> optionalColumns{
        {QStringLiteral("has_video"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("video_path"), QStringLiteral("TEXT")},
        {QStringLiteral("video_name"), QStringLiteral("TEXT")},
        {QStringLiteral("video_cover"), QStringLiteral("TEXT")},
        {QStringLiteral("video_seconds"), QStringLiteral("INTEGER")},
        {QStringLiteral("video_bytes"), QStringLiteral("INTEGER")},
        {QStringLiteral("caption_above"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("approval_token"), QStringLiteral("TEXT")},
        {QStringLiteral("approval_actions"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_msg_id"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_sender"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_digest"), QStringLiteral("TEXT")},
        {QStringLiteral("quote_kind"), QStringLiteral("TEXT")}
    };
    for (const auto& column : optionalColumns) {
        if (messageColumns.contains(column.first)) continue;
        if (!query.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN %1 %2")
                       .arg(column.first, column.second))) return false;
    }

    // 索引和补列同处一个事务；任何失败都会保留原 schema 和旧时间索引。
    if (!query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_messages_peer_page ON messages(peer, created_at, id)"))) return false;
    if (!query.exec(QStringLiteral("DROP INDEX IF EXISTS idx_messages_peer_time"))) return false;
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS im_groups ("
        "group_id TEXT PRIMARY KEY, name TEXT NOT NULL, member_count INTEGER NOT NULL,"
        "pending_activation INTEGER NOT NULL DEFAULT 0)"))) return false;
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS im_group_messages ("
        "id TEXT PRIMARY KEY, group_id TEXT NOT NULL, sender_id TEXT NOT NULL,"
        "text TEXT NOT NULL, created_at INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0,"
        "from_self INTEGER NOT NULL, pending INTEGER NOT NULL, failed INTEGER NOT NULL)"))) return false;
    if (!query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_im_group_messages_page "
        "ON im_group_messages(group_id, created_at, id)"))) return false;
    return transaction.commit();
}

void LocalMessageDatabase::loadInto(ChatState& state) const {
    RemoteDiagnostics::PerformanceSpan performance("history-load");
    if (!isOpen()) return;

    Statement contactQuery(db_);
    contactQuery.exec(kSelectContactsSql);
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString(),
            contactQuery.value(2).toString(),
            contactQuery.value(3).toString()
        });
    }

    Statement messageQuery(db_);
    messageQuery.exec(QStringLiteral("SELECT * FROM messages ORDER BY created_at, id"));
    while (messageQuery.next()) {
        state.appendMessageForRestore(messageFromQuery(messageQuery));
    }
}

QHash<QString, bool> LocalMessageDatabase::loadRecentInto(ChatState& state, int perPeerLimit) const {
    RemoteDiagnostics::PerformanceSpan performance("history-load");
    QHash<QString, bool> hasEarlier;
    if (!isOpen() || perPeerLimit <= 0) return hasEarlier;

    Statement contactQuery(db_);
    contactQuery.exec(kSelectContactsSql);
    while (contactQuery.next()) {
        state.upsertContact(RemoteIMContact{
            contactQuery.value(0).toString(),
            contactQuery.value(1).toString(),
            contactQuery.value(2).toString(),
            contactQuery.value(3).toString()
        });
    }

    // Enumerate only peer keys from the covering index. Reading full rows through a
    // window function would parse every historical payload before applying LIMIT.
    Statement peerQuery(db_);
    peerQuery.exec(QStringLiteral("SELECT DISTINCT peer FROM messages"));
    while (peerQuery.next()) {
        const QString peer = peerQuery.value(0).toString();
        const int readLimit = perPeerLimit < std::numeric_limits<int>::max() ? perPeerLimit + 1 : perPeerLimit;
        QList<RemoteIMMessage> recent = loadRecentMessages(peer, readLimit);
        const bool hasMore = recent.size() > perPeerLimit;
        if (hasMore) recent.removeFirst();
        hasEarlier.insert(peer, hasMore);
        state.appendDatabaseHistoryPage(recent);
    }
    return hasEarlier;
}

QList<RemoteIMMessage> LocalMessageDatabase::loadRecentMessages(const QString& peer, int limit) const {
    RemoteDiagnostics::PerformanceSpan performance("history-load");
    QList<RemoteIMMessage> result;
    if (!isOpen() || peer.isEmpty() || limit <= 0) return result;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "SELECT * FROM messages WHERE peer = ? ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(peer);
    query.addBindValue(limit);
    query.exec();
    while (query.next()) result.prepend(messageFromQuery(query));
    return result;
}

QList<RemoteIMMessage> LocalMessageDatabase::loadMessagesBefore(const QString& peer,
                                                                qint64 beforeCreatedAt,
                                                                const QString& beforeId,
                                                                int limit) const {
    RemoteDiagnostics::PerformanceSpan performance("history-load");
    QList<RemoteIMMessage> result;
    if (!isOpen() || peer.isEmpty() || limit <= 0) return result;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "SELECT * FROM messages"
        " WHERE peer = ? AND (created_at < ? OR (created_at = ? AND id < ?))"
        " ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(peer);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeId);
    query.addBindValue(limit);
    query.exec();
    while (query.next()) {
        result.prepend(messageFromQuery(query));  // DESC 取出，prepend 还原为升序
    }
    return result;
}

void LocalMessageDatabase::upsertContact(const RemoteIMContact& contact) {
    if (!isOpen() || contact.userId.trimmed().isEmpty()) return;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO contacts(user_id, display_name, avatar_url) VALUES(?, ?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET "
        "display_name = CASE "
        "  WHEN excluded.display_name = excluded.user_id AND contacts.display_name <> contacts.user_id "
        "  THEN contacts.display_name ELSE excluded.display_name END, "
        "avatar_url = CASE WHEN excluded.avatar_url = '' THEN contacts.avatar_url ELSE excluded.avatar_url END"));
    query.addBindValue(contact.userId);
    query.addBindValue(contact.displayName.isEmpty() ? contact.userId : contact.displayName);
    const QString avatarUrl = contact.avatarUrl.trimmed();
    query.addBindValue(avatarUrl.isEmpty() ? QStringLiteral("") : avatarUrl);
    query.exec();
}

void LocalMessageDatabase::removeMessagesForPeer(const QString& userId) {
    if (!isOpen()) return;
    Statement deleteMessages(db_);
    deleteMessages.prepare(QStringLiteral("DELETE FROM messages WHERE peer = ?"));
    deleteMessages.addBindValue(userId);
    deleteMessages.exec();
}

QStringList LocalMessageDatabase::contactGroups() const {
    QStringList groups;
    if (!isOpen()) return groups;
    Statement query(db_);
    // 二级按 name 排序：sort_order 撞车或断档时顺序也是稳定的，不会每次启动跳来跳去。
    query.exec(QStringLiteral("SELECT name FROM contact_groups ORDER BY sort_order, name"));
    while (query.next()) groups.append(query.value(0).toString());
    return groups;
}

bool LocalMessageDatabase::createContactGroup(const QString& name) {
    if (!isOpen()) return false;
    const QString clean = ContactGroups::normalize(name);
    if (!ContactGroups::isAcceptableName(clean)) return false;
    Statement query(db_);
    // 新组排在末尾：创建顺序就是用户给出的顺序，无需再让他手工排一遍。
    query.prepare(QStringLiteral(
        "INSERT INTO contact_groups(name, sort_order) "
        "VALUES(?, (SELECT COALESCE(MAX(sort_order), -1) + 1 FROM contact_groups))"));
    query.addBindValue(clean);
    // 主键冲突即重名，exec 返回 false——不必先查一次。
    return query.exec();
}

bool LocalMessageDatabase::renameContactGroup(const QString& from, const QString& to) {
    if (!isOpen()) return false;
    const QString oldName = ContactGroups::normalize(from);
    const QString newName = ContactGroups::normalize(to);
    if (!ContactGroups::isAcceptableName(newName)) return false;
    if (oldName == newName) return true;

    Transaction transaction(db_);
    if (!transaction) return false;
    Statement rename(db_);
    rename.prepare(QStringLiteral("UPDATE contact_groups SET name = ? WHERE name = ?"));
    rename.addBindValue(newName);
    rename.addBindValue(oldName);
    // 目标名已存在会撞主键；此时整笔回滚，绝不把两个组悄悄合并成一个。
    if (!rename.exec() || rename.numRowsAffected() <= 0) {
        return false;
    }
    Statement move(db_);
    move.prepare(QStringLiteral("UPDATE contacts SET group_name = ? WHERE group_name = ?"));
    move.addBindValue(newName);
    move.addBindValue(oldName);
    if (!move.exec()) {
        return false;
    }
    return transaction.commit();
}

void LocalMessageDatabase::deleteContactGroup(const QString& name) {
    if (!isOpen()) return;
    const QString clean = ContactGroups::normalize(name);
    if (clean.isEmpty()) return;

    // 先清成员再删组，且放在同一个事务里。顺序反过来的话，中途失败会留下一批
    // 指向已删分组的联系人；而删组顺手删人是那种一旦发生就无法挽回的事，
    // 所以这里只动 group_name，永远不碰 contacts 的行本身。
    Transaction transaction(db_);
    if (!transaction) return;
    Statement clear(db_);
    clear.prepare(QStringLiteral("UPDATE contacts SET group_name = '' WHERE group_name = ?"));
    clear.addBindValue(clean);
    if (!clear.exec()) {
        return;
    }
    Statement drop(db_);
    drop.prepare(QStringLiteral("DELETE FROM contact_groups WHERE name = ?"));
    drop.addBindValue(clean);
    if (!drop.exec()) {
        return;
    }
    transaction.commit();
}

void LocalMessageDatabase::setContactGroup(const QString& userId, const QString& groupName) {
    if (!isOpen() || userId.trimmed().isEmpty()) return;
    const QString clean = ContactGroups::normalize(groupName);
    // 必须是「空但非 null」的 QString：默认构造的 QString 是 null，绑定进去写的是
    // SQL NULL，而 group_name 是 NOT NULL 列，整条 UPDATE 会被约束静默拒绝——
    // 表现就是「移出分组」点了没反应。upsertContact 里的 avatar_url 同理。
    QString target = QStringLiteral("");
    if (!clean.isEmpty()) {
        // 只认已存在的分组。写入一个不存在的组名等于凭空造组，
        // 而分组的存在与否只由 contact_groups 说了算。
        Statement exists(db_);
        exists.prepare(QStringLiteral("SELECT 1 FROM contact_groups WHERE name = ?"));
        exists.addBindValue(clean);
        if (exists.exec() && exists.next()) target = clean;
    }
    Statement query(db_);
    query.prepare(QStringLiteral("UPDATE contacts SET group_name = ? WHERE user_id = ?"));
    query.addBindValue(target);
    query.addBindValue(userId);
    query.exec();
}

void LocalMessageDatabase::removeContactCascade(const QString& userId) {
    if (!isOpen()) return;
    // 两次 DELETE 是同一用户操作。第二步失败必须留下联系人及其原有消息。
    Transaction transaction(db_);
    if (!transaction) return;
    Statement deleteMessages(db_);
    deleteMessages.prepare(QStringLiteral("DELETE FROM messages WHERE peer = ?"));
    deleteMessages.addBindValue(userId);
    if (!deleteMessages.exec()) return;

    Statement deleteContact(db_);
    deleteContact.prepare(QStringLiteral("DELETE FROM contacts WHERE user_id = ?"));
    deleteContact.addBindValue(userId);
    if (!deleteContact.exec()) return;
    transaction.commit();
}

// 作用：以稳定消息标识写入新记录并避免重复消息。
bool LocalMessageDatabase::insertMessageIfAbsent(const RemoteIMMessage& message, const QString& peer) {
    RemoteDiagnostics::PerformanceSpan performance("history-write");
    if (!isOpen() || message.id.isEmpty()) return false;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO messages("
        "  id, from_user, to_user, peer, direction, status, text, created_at,"
        "  has_image, image_path, image_w, image_h, image_bytes,"
        "  has_voice, voice_path, voice_seconds,"
        "  has_file, file_path, file_name, file_mime, file_bytes,"
        "  has_video, video_path, video_name, video_cover, video_seconds, video_bytes,"
        "  approval_token, approval_actions, caption_above,"
        "  quote_msg_id, quote_sender, quote_digest, quote_kind"
        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(message.id);
    query.addBindValue(message.fromUserId);
    query.addBindValue(message.toUserId);
    query.addBindValue(peer);
    query.addBindValue(message.direction == RemoteIMMessageDirection::Outgoing ? 1 : 0);
    query.addBindValue(static_cast<int>(message.status));
    query.addBindValue(message.text);
    query.addBindValue(message.createdAtMillis);
    query.addBindValue(message.hasImage ? 1 : 0);
    query.addBindValue(message.image.localPath);
    query.addBindValue(message.image.width);
    query.addBindValue(message.image.height);
    query.addBindValue(message.image.sizeBytes);
    query.addBindValue(message.hasVoice ? 1 : 0);
    query.addBindValue(message.voice.localPath);
    query.addBindValue(message.voice.durationSeconds);
    query.addBindValue(message.hasFile ? 1 : 0);
    query.addBindValue(message.file.localPath);
    query.addBindValue(message.file.fileName);
    query.addBindValue(message.file.mimeType);
    query.addBindValue(message.file.sizeBytes);
    query.addBindValue(message.hasVideo ? 1 : 0);
    query.addBindValue(message.video.localPath);
    query.addBindValue(message.video.fileName);
    query.addBindValue(message.video.coverPath);
    query.addBindValue(message.video.durationSeconds);
    query.addBindValue(message.video.sizeBytes);
    const QString approvalToken = message.hasApprovalRequest
        ? message.approvalRequest.token
        : message.hasApprovalDecision ? message.approvalDecision.token : QString();
    const QList<RemoteIMApprovalAction> approvalActions = message.hasApprovalRequest
        ? message.approvalRequest.actions
        : message.hasApprovalDecision
            ? QList<RemoteIMApprovalAction>{message.approvalDecision.action}
            : QList<RemoteIMApprovalAction>{};
    query.addBindValue(approvalToken);
    query.addBindValue(approvalActionsText(approvalActions));
    query.addBindValue(message.captionAbove ? 1 : 0);
    // 没有引用时写空串而不是默认构造的 QString：后者是 null，绑进去成 SQL NULL。
    // 这几列虽然可空，但读取侧统一按空串判定，混进 NULL 会让判据出现两种形状。
    query.addBindValue(message.hasQuote ? message.quote.msgId : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.senderId : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.digest : QStringLiteral(""));
    query.addBindValue(message.hasQuote ? message.quote.kind : QStringLiteral(""));
    if (!query.exec()) return false;
    const bool inserted = query.numRowsAffected() > 0;
    if (!inserted && message.createdAtMillis > 0) {
        Statement enrich(db_);
        enrich.prepare(QStringLiteral("UPDATE messages SET created_at = ? WHERE id = ?"));
        enrich.addBindValue(message.createdAtMillis);
        enrich.addBindValue(message.id);
        enrich.exec();
    }
    return inserted;
}

void LocalMessageDatabase::adoptMessageId(const QString& oldId, const QString& newId) {
    if (!isOpen() || oldId.isEmpty() || newId.isEmpty() || oldId == newId) return;
    Transaction transaction(db_);
    if (!transaction) return;
    Statement rename(db_);
    rename.prepare(QStringLiteral("UPDATE OR IGNORE messages SET id = ? WHERE id = ?"));
    rename.addBindValue(newId);
    rename.addBindValue(oldId);
    if (!rename.exec()) return;
    if (rename.numRowsAffected() == 0) {
        Statement exists(db_);
        exists.prepare(QStringLiteral("SELECT 1 FROM messages WHERE id = ?"));
        exists.addBindValue(newId);
        // OR IGNORE 也可能由其它约束/trigger 触发；只有稳定 id 确实存在才允许清理旧行。
        if (!exists.exec() || !exists.next()) return;
    }
    // 稳定 id 已存在（漫游先落库）时上面被 IGNORE，旧临时行是重复项，清掉。
    Statement cleanup(db_);
    cleanup.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    cleanup.addBindValue(oldId);
    if (!cleanup.exec()) return;
    transaction.commit();
}

void LocalMessageDatabase::updateMessageStatus(const QString& messageId, RemoteIMMessageStatus status) {
    RemoteDiagnostics::PerformanceSpan performance("history-write");
    if (!isOpen()) return;
    Statement query(db_);
    query.prepare(QStringLiteral("UPDATE messages SET status = ? WHERE id = ?"));
    query.addBindValue(static_cast<int>(status));
    query.addBindValue(messageId);
    query.exec();
}

void LocalMessageDatabase::updateMessageTime(const QString& messageId, qint64 createdAtMillis) {
    if (!isOpen() || messageId.isEmpty()) return;
    Statement query(db_);
    query.prepare(QStringLiteral("UPDATE messages SET created_at = ? WHERE id = ? AND ? > 0"));
    query.addBindValue(createdAtMillis);
    query.addBindValue(messageId);
    query.addBindValue(createdAtMillis);
    query.exec();
}

QList<RemoteIMGroup> LocalMessageDatabase::groups() const {
    QList<RemoteIMGroup> result;
    if (!isOpen()) return result;
    Statement query(db_);
    query.exec(QStringLiteral("SELECT group_id, name, member_count, pending_activation FROM im_groups"));
    while (query.next()) result.append({query.value(0).toString(), query.value(1).toString(),
                                        query.value(2).toInt(), query.value(3).toBool()});
    return result;
}

void LocalMessageDatabase::upsertGroup(const RemoteIMGroup& group) {
    if (!isOpen() || group.groupId.isEmpty()) return;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO im_groups(group_id, name, member_count, pending_activation) VALUES(?,?,?,?) "
        "ON CONFLICT(group_id) DO UPDATE SET name=excluded.name, "
        "member_count=excluded.member_count, pending_activation=excluded.pending_activation"));
    query.addBindValue(group.groupId);
    query.addBindValue(group.name);
    query.addBindValue(group.memberCount);
    query.addBindValue(group.pendingActivation ? 1 : 0);
    query.exec();
}

void LocalMessageDatabase::removeGroup(const QString& groupId) {
    if (!isOpen() || groupId.isEmpty()) return;
    Statement query(db_);
    query.prepare(QStringLiteral("DELETE FROM im_groups WHERE group_id = ?"));
    query.addBindValue(groupId);
    query.exec();
}

QList<RemoteIMGroupMessage> LocalMessageDatabase::groupMessagesBefore(
    const QString& groupId, qint64 beforeCreatedAt, const QString& beforeId, int limit) const {
    QList<RemoteIMGroupMessage> result;
    if (!isOpen() || groupId.isEmpty() || limit <= 0) return result;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "SELECT id, group_id, sender_id, text, created_at, seq, from_self, pending, failed "
        "FROM im_group_messages WHERE group_id = ? "
        "AND (created_at < ? OR (created_at = ? AND id < ?)) "
        "ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(groupId);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeCreatedAt);
    query.addBindValue(beforeId);
    query.addBindValue(limit);
    if (!query.exec()) return result;
    while (query.next()) result.prepend({query.value(0).toString(), query.value(1).toString(),
                                        query.value(2).toString(), query.value(3).toString(),
                                        query.value(4).toLongLong(), query.value(5).toLongLong(),
                                        query.value(6).toBool(), query.value(7).toBool(),
                                        query.value(8).toBool()});
    return result;
}

int LocalMessageDatabase::groupMessageCount(const QString& groupId) const {
    if (!isOpen() || groupId.isEmpty()) return 0;
    Statement query(db_);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM im_group_messages WHERE group_id = ?"));
    query.addBindValue(groupId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

bool LocalMessageDatabase::insertGroupMessageIfAbsent(const RemoteIMGroupMessage& message) {
    if (!isOpen() || message.id.isEmpty() || message.groupId.isEmpty()) return false;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO im_group_messages "
        "(id,group_id,sender_id,text,created_at,seq,from_self,pending,failed) "
        "VALUES(?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(message.id);
    query.addBindValue(message.groupId);
    query.addBindValue(message.senderId);
    query.addBindValue(message.text);
    query.addBindValue(message.createdAtMillis);
    query.addBindValue(message.sequence);
    query.addBindValue(message.fromSelf ? 1 : 0);
    query.addBindValue(message.pending ? 1 : 0);
    query.addBindValue(message.failed ? 1 : 0);
    return query.exec() && query.numRowsAffected() > 0;
}

void LocalMessageDatabase::adoptGroupMessageId(const QString& oldId, const QString& remoteId,
                                                qint64 createdAtMillis, qint64 sequence) {
    if (!isOpen() || oldId.isEmpty()) return;
    Transaction transaction(db_);
    if (!transaction) return;
    Statement query(db_);
    query.prepare(QStringLiteral(
        "UPDATE OR IGNORE im_group_messages SET id=?, created_at=?, seq=?, pending=0 "
        "WHERE id=?"));
    query.addBindValue(remoteId.isEmpty() ? oldId : remoteId);
    query.addBindValue(createdAtMillis);
    query.addBindValue(sequence);
    query.addBindValue(oldId);
    if (!query.exec()) return;
    if (!remoteId.isEmpty() && remoteId != oldId) {
        if (query.numRowsAffected() == 0) {
            Statement exists(db_);
            exists.prepare(QStringLiteral("SELECT 1 FROM im_group_messages WHERE id = ?"));
            exists.addBindValue(remoteId);
            if (!exists.exec() || !exists.next()) return;
        }
        query.prepare(QStringLiteral("DELETE FROM im_group_messages WHERE id=?"));
        query.addBindValue(oldId);
        if (!query.exec()) return;
    }
    transaction.commit();
}

void LocalMessageDatabase::updateGroupMessageStatus(const QString& id, bool pending, bool failed) {
    if (!isOpen() || id.isEmpty()) return;
    Statement query(db_);
    query.prepare(QStringLiteral("UPDATE im_group_messages SET pending=?, failed=? WHERE id=?"));
    query.addBindValue(pending ? 1 : 0);
    query.addBindValue(failed ? 1 : 0);
    query.addBindValue(id);
    query.exec();
}
