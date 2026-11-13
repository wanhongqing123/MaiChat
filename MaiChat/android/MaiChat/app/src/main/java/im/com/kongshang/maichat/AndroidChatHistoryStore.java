// 文件作用：Android IM 身份、消息同步与本地历史；AndroidChatHistoryStore 负责持久化读写与缓存管理，本文件给出其实现。
// 类型关系：AndroidChatHistoryStore extends SQLiteOpenHelper；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：messages、hasEarlier、onCreate、onUpgrade。
package com.kongshang.maichat;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.os.CancellationSignal;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.io.IOException;

public final class AndroidChatHistoryStore extends SQLiteOpenHelper {
    private static final String DATABASE_NAME = "maichat-history.db";
    private static final int DATABASE_VERSION = 9;
    private static final String TAG = "MaiChat.im";
    // 筛选和结果必须采用同一份存储元数据语义，旧路径能否恢复不应改变返回的 kind。
    private static final String HISTORY_KIND_SQL = "CASE WHEN m.image_path <> '' THEN 'image'"
        + " WHEN m.video_path <> '' THEN 'video' WHEN m.voice_path <> '' THEN 'voice'"
        + " WHEN m.file_path <> '' THEN 'file' ELSE 'text' END";

    private final RemoteIMMediaPaths mediaPaths;
    private final java.io.File databaseFile;
    // 游标只属于当前打开的数据库实例；有界表避免把查询文字或账号信息编码到工具游标中。
    private final java.util.LinkedHashMap<String, HistoryCursor> historyCursors = new java.util.LinkedHashMap<>();
    private SQLiteDatabase historyDatabase;
    private Object historyInstance;

    private static final class HistoryCursor {
        final String owner, loginScope, id;
        final long createdAt;
        final RemoteIMMessageHistory.Query filters;
        final Object databaseInstance;
        HistoryCursor(String owner, String loginScope, RemoteIMMessageHistory.Query filters,
                      RemoteIMMessageHistory.Row last, Object databaseInstance) {
            this.owner = owner; this.loginScope = loginScope; this.filters = filters;
            this.id = last.id; this.createdAt = last.createdAtMs;
            this.databaseInstance = databaseInstance;
        }
    }

    public static final class Page {
        private final List<RemoteIMMessage> messages;
        private final boolean hasEarlier;

        Page(List<RemoteIMMessage> messages, boolean hasEarlier) {
            this.messages = Collections.unmodifiableList(messages);
            this.hasEarlier = hasEarlier;
        }

        public List<RemoteIMMessage> messages() {
            return messages;
        }

        public boolean hasEarlier() {
            return hasEarlier;
        }
    }

    public AndroidChatHistoryStore(Context context) {
        this(context, DATABASE_NAME);
    }

    AndroidChatHistoryStore(Context context, String databaseName) {
        super(context.getApplicationContext(), databaseName, null, DATABASE_VERSION);
        // 媒体路径入库存相对形式，读出来再按当前根还原：数据目录换根时历史不会整片失效。
        this.mediaPaths = RemoteIMMediaPaths.forApp(context);
        this.databaseFile = context.getDatabasePath(databaseName);
        // 只读搜索使用独立连接，WAL 防止长扫描的读锁挡住主线程接收消息时的落库。
        setWriteAheadLoggingEnabled(true);
    }

    @Override public void onOpen(SQLiteDatabase database) {
        super.onOpen(database);
        synchronized (historyCursors) {
            historyCursors.clear(); historyDatabase = database; historyInstance = new Object();
        }
    }

    @Override public synchronized void close() {
        synchronized (historyCursors) {
            historyCursors.clear(); historyDatabase = null; historyInstance = null;
        }
        super.close();
    }

    boolean isMessageHistoryCurrent(RemoteIMMessageHistory.Page page) {
        synchronized (historyCursors) {
            return page != null && historyInstance != null && page.databaseInstance == historyInstance;
        }
    }

    /** 所有筛选和稳定分页均在 SQL 内完成，不能用 LIMIT 后的页面缓存筛选代替。 */
    public RemoteIMMessageHistory.Page queryMessages(String ownerId, String loginScope,
            RemoteIMMessageHistory.Query query, CancellationSignal cancellation) {
        String owner = clean(ownerId);
        if (owner.isEmpty() || clean(loginScope).isEmpty())
            throw new IllegalArgumentException("Active history owner is required");
        cancellation.throwIfCanceled();
        // 查询不能通过 SQLiteOpenHelper 自动建空库，把数据库丢失误报为没有消息。
        if (!databaseFile.isFile()) throw new IllegalStateException("Local history database is unavailable");
        SQLiteDatabase database = getReadableDatabase();
        Object databaseInstance;
        synchronized (historyCursors) {
            // helper 在拿到 handle 后可被另一调用关闭/重开，必须同时绑定 handle 和 epoch。
            if (database != historyDatabase || historyInstance == null)
                throw new IllegalStateException("Local history database changed");
            databaseInstance = historyInstance;
        }
        HistoryCursor before = null;
        if (!query.cursor.isEmpty()) {
            synchronized (historyCursors) { before = historyCursors.get(query.cursor); }
            if (before == null || !before.owner.equals(owner) || !before.loginScope.equals(loginScope)
                    || before.databaseInstance != databaseInstance || !before.filters.sameFilters(query))
                throw new IllegalArgumentException("History cursor is invalid or expired");
        }
        List<String> args = new ArrayList<>();
        args.add(owner);
        StringBuilder where = new StringBuilder("m.owner_id = ? AND m.peer_id <> ''");
        if (!query.peerId.isEmpty()) { where.append(" AND m.peer_id = ?"); args.add(query.peerId); }
        if (query.query != null) {
            // 保留 instr 的文字匹配：%/_ 不是通配符，lower 只折叠 ASCII 大小写。
            where.append(" AND m.body <> '' AND instr(lower(m.body), lower(?)) > 0");
            args.add(query.query);
        }
        if (!query.kind.isEmpty()) {
            where.append(" AND (").append(HISTORY_KIND_SQL).append(") = ?");
            args.add(query.kind);
        }
        if (!query.direction.isEmpty()) {
            where.append(" AND m.direction = ?");
            args.add(query.direction.equals("incoming") ? "INCOMING" : "OUTGOING");
        }
        if (query.sinceMs != null) { where.append(" AND m.created_at >= ?"); args.add(query.sinceMs.toString()); }
        if (query.untilMs != null) { where.append(" AND m.created_at <= ?"); args.add(query.untilMs.toString()); }
        if (before != null) {
            where.append(" AND (m.created_at < ? OR (m.created_at = ? AND m.id < ?))");
            args.add(Long.toString(before.createdAt)); args.add(Long.toString(before.createdAt)); args.add(before.id);
        }
        args.add(Integer.toString(query.limit + 1));
        List<RemoteIMMessageHistory.Row> matches = new ArrayList<>();
        // 仅投影七项查询字段；完整 UI 消息的路径解析和无关损坏元数据都不能影响本机检索。
        try (Cursor rows = database.rawQuery("SELECT m.id,m.peer_id,m.from_id,m.body," + HISTORY_KIND_SQL
                + ",lower(m.direction),m.created_at FROM messages m WHERE "
                + where + " ORDER BY m.created_at DESC, m.id DESC LIMIT ?", args.toArray(new String[0]), cancellation)) {
            while (rows.moveToNext()) {
                cancellation.throwIfCanceled();
                matches.add(new RemoteIMMessageHistory.Row(rows.getString(0), rows.getString(1), rows.getString(2),
                    rows.getString(3), rows.getString(4), rows.getString(5), rows.getLong(6)));
            }
        }
        cancellation.throwIfCanceled();
        boolean more = matches.size() > query.limit;
        if (more) matches.remove(matches.size() - 1);
        String next = null;
        synchronized (historyCursors) {
            // 不能让旧连接完成后往新连接的注册表发布游标；只锁发布，不锁整个 SQL 扫描。
            if (database != historyDatabase || databaseInstance != historyInstance)
                throw new IllegalStateException("Local history database changed");
            if (more) {
                next = java.util.UUID.randomUUID().toString();
                historyCursors.put(next, new HistoryCursor(owner, loginScope, query,
                    matches.get(matches.size() - 1), databaseInstance));
                while (historyCursors.size() > 512) historyCursors.remove(historyCursors.keySet().iterator().next());
            }
        }
        return new RemoteIMMessageHistory.Page(matches, more, next, owner, loginScope, databaseInstance);
    }

    /** 已获注销回执后的本地清理；共享数据库保留其他 owner，受管媒体也先排除其他账号引用。 */
    public void purgeAcceptedAccount(String ownerId) throws IOException {
        String owner = clean(ownerId);
        if (owner.isEmpty()) throw new IOException("Accepted account owner is required");
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            Map<String, String> owned = new HashMap<>(); Set<String> shared = new HashSet<>();
            try (Cursor cursor = database.rawQuery("SELECT owner_id,image_path,voice_path,file_path,video_path,video_cover_path FROM messages", null)) {
                while (cursor.moveToNext()) {
                    boolean ours = owner.equals(cursor.getString(0));
                    for (int column = 1; column <= 5; column++) {
                        String relative = cursor.getString(column);
                        // 旧绝对路径/越界字符串不属于 App 管理，不尝试删除其指向的用户文件。
                        if (!eligibleMedia(relative)) continue;
                        try {
                            String identity = MaiChatAcceptedAccountCleanup.mediaIdentity(mediaPaths.root(), relative);
                            if (ours) owned.put(identity, relative); else shared.add(identity);
                        } catch (IOException unsafe) { if (ours) throw unsafe; }
                    }
                }
            }
            for (Map.Entry<String, String> item : owned.entrySet()) if (!shared.contains(item.getKey()))
                MaiChatAcceptedAccountCleanup.deleteMedia(mediaPaths.root(), item.getValue());
            for (String table : new String[]{"messages", "contacts", "contact_groups", "im_groups", "group_messages"})
                database.delete(table, "owner_id=?", new String[]{owner});
            database.setTransactionSuccessful();
        } finally { database.endTransaction(); }
    }

    private static boolean eligibleMedia(String value) {
        if (value == null || !value.matches("(?:Incoming|Outgoing)/(?:Images|Videos|VideoCovers|Voices|Files)/.+")) return false;
        for (String part : value.replace('\\', '/').split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals("..")) return false;
        return true;
    }

    @Override
    // 作用：创建本地消息表、索引和版本所需的结构。
    public void onCreate(SQLiteDatabase database) {
        database.execSQL(
            "CREATE TABLE contacts ("
                + "owner_id TEXT NOT NULL,"
                + "user_id TEXT NOT NULL,"
                + "display_name TEXT NOT NULL,"
                + "avatar_url TEXT NOT NULL DEFAULT '',"
                + "group_name TEXT NOT NULL DEFAULT '',"
                + "PRIMARY KEY(owner_id, user_id))"
        );
        createContactGroupsTable(database);
        database.execSQL(
            "CREATE TABLE messages ("
                + "owner_id TEXT NOT NULL,"
                + "id TEXT NOT NULL,"
                + "remote_id TEXT NOT NULL DEFAULT '',"
                + "peer_id TEXT NOT NULL,"
                + "from_id TEXT NOT NULL,"
                + "to_id TEXT NOT NULL,"
                + "body TEXT NOT NULL,"
                + "direction TEXT NOT NULL,"
                + "status TEXT NOT NULL,"
                + "created_at INTEGER NOT NULL,"
                + "image_path TEXT NOT NULL DEFAULT '',"
                + "image_width INTEGER NOT NULL DEFAULT 0,"
                + "image_height INTEGER NOT NULL DEFAULT 0,"
                + "image_size INTEGER NOT NULL DEFAULT 0,"
                + "voice_path TEXT NOT NULL DEFAULT '',"
                + "voice_duration INTEGER NOT NULL DEFAULT 0,"
                + "file_path TEXT NOT NULL DEFAULT '',"
                + "file_name TEXT NOT NULL DEFAULT '',"
                + "file_mime TEXT NOT NULL DEFAULT '',"
                + "file_size INTEGER NOT NULL DEFAULT 0,"
                + "origin TEXT NOT NULL DEFAULT 'human',"
                + "video_path TEXT NOT NULL DEFAULT '',"
                + "video_cover_path TEXT NOT NULL DEFAULT '',"
                + "video_duration INTEGER NOT NULL DEFAULT 0,"
                + "video_width INTEGER NOT NULL DEFAULT 0,"
                + "video_height INTEGER NOT NULL DEFAULT 0,"
                + "video_size INTEGER NOT NULL DEFAULT 0,"
                + "approval_request_token TEXT NOT NULL DEFAULT '',"
                + "approval_request_actions TEXT NOT NULL DEFAULT '',"
                + "approval_decision_token TEXT NOT NULL DEFAULT '',"
                + "approval_decision_action TEXT NOT NULL DEFAULT '',"
                + "caption_above INTEGER NOT NULL DEFAULT 0,"
                + "quote_msg_id TEXT NOT NULL DEFAULT '',"
                + "quote_sender TEXT NOT NULL DEFAULT '',"
                + "quote_digest TEXT NOT NULL DEFAULT '',"
                + "quote_kind TEXT NOT NULL DEFAULT '',"
                + "PRIMARY KEY(owner_id, id))"
        );
        database.execSQL(
            "CREATE INDEX messages_conversation_idx "
                + "ON messages(owner_id, peer_id, created_at DESC, id DESC)"
        );
        database.execSQL(
            "CREATE INDEX messages_remote_idx ON messages(owner_id, remote_id)"
        );
        createOwnerHistoryIndex(database);
        createGroupTables(database);
        createGroupRemoteUniqueIndex(database);
    }

    @Override
    public void onUpgrade(SQLiteDatabase database, int oldVersion, int newVersion) {
        if (oldVersion == newVersion) return;
        int version = oldVersion;
        if (version < 2) {
            addVideoColumns(database);
            version = 2;
        }
        if (version < 3) {
            addApprovalColumns(database);
            version = 3;
        }
        if (version < 4) {
            database.execSQL("ALTER TABLE contacts ADD COLUMN group_name TEXT NOT NULL DEFAULT ''");
            createContactGroupsTable(database);
            version = 4;
        }
        if (version < 5) {
            database.execSQL(
                "ALTER TABLE messages ADD COLUMN caption_above INTEGER NOT NULL DEFAULT 0"
            );
            version = 5;
        }
        if (version < 6) {
            addQuoteColumns(database);
            version = 6;
        }
        if (version < 7) {
            createGroupTables(database);
            version = 7;
        }
        if (version < 8) {
            database.execSQL("DELETE FROM group_messages WHERE remote_id<>'' AND rowid NOT IN "
                + "(SELECT MAX(rowid) FROM group_messages WHERE remote_id<>'' "
                + "GROUP BY owner_id,group_id,remote_id)");
            createGroupRemoteUniqueIndex(database);
            version = 8;
        }
        if (version < 9) {
            createOwnerHistoryIndex(database);
            version = 9;
        }
        if (version != newVersion) {
            // 只有在没有可用迁移路径时才重建。重建会清空用户本地聊天记录，
            // 为了加几列而删历史，这个代价不该由用户承担，所以放在最后一步。
            database.execSQL("DROP TABLE IF EXISTS messages");
            database.execSQL("DROP TABLE IF EXISTS contacts");
            onCreate(database);
        }
    }

    private static void createOwnerHistoryIndex(SQLiteDatabase database) {
        // 跨会话查询按账号和时间直接扫描，避免取少量命中也先排序当前账号全部历史。
        database.execSQL("CREATE INDEX IF NOT EXISTS messages_owner_order_idx "
            + "ON messages(owner_id,created_at DESC,id DESC)");
    }

    private static void addVideoColumns(SQLiteDatabase database) {
        database.execSQL("ALTER TABLE messages ADD COLUMN video_path TEXT NOT NULL DEFAULT ''");
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN video_cover_path TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL("ALTER TABLE messages ADD COLUMN video_duration INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_width INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_height INTEGER NOT NULL DEFAULT 0");
        database.execSQL("ALTER TABLE messages ADD COLUMN video_size INTEGER NOT NULL DEFAULT 0");
    }

    private static void addApprovalColumns(SQLiteDatabase database) {
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_request_token TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_request_actions TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_decision_token TEXT NOT NULL DEFAULT ''"
        );
        database.execSQL(
            "ALTER TABLE messages ADD COLUMN approval_decision_action TEXT NOT NULL DEFAULT ''"
        );
    }

    private static void addQuoteColumns(SQLiteDatabase database) {
        database.execSQL("ALTER TABLE messages ADD COLUMN quote_msg_id TEXT NOT NULL DEFAULT ''");
        database.execSQL("ALTER TABLE messages ADD COLUMN quote_sender TEXT NOT NULL DEFAULT ''");
        database.execSQL("ALTER TABLE messages ADD COLUMN quote_digest TEXT NOT NULL DEFAULT ''");
        database.execSQL("ALTER TABLE messages ADD COLUMN quote_kind TEXT NOT NULL DEFAULT ''");
    }

    private static void createContactGroupsTable(SQLiteDatabase database) {
        database.execSQL(
            "CREATE TABLE IF NOT EXISTS contact_groups ("
                + "owner_id TEXT NOT NULL,"
                + "name TEXT NOT NULL,"
                + "sort_order INTEGER NOT NULL,"
                + "PRIMARY KEY(owner_id, name))"
        );
    }

    private static void createGroupTables(SQLiteDatabase database) {
        database.execSQL("CREATE TABLE IF NOT EXISTS im_groups ("
            + "owner_id TEXT NOT NULL,group_id TEXT NOT NULL,name TEXT NOT NULL,"
            + "member_count INTEGER NOT NULL,pending_activation INTEGER NOT NULL DEFAULT 0,"
            + "PRIMARY KEY(owner_id,group_id))");
        database.execSQL("CREATE TABLE IF NOT EXISTS group_messages ("
            + "owner_id TEXT NOT NULL,id TEXT NOT NULL,remote_id TEXT NOT NULL DEFAULT '',"
            + "group_id TEXT NOT NULL,sender_id TEXT NOT NULL,body TEXT NOT NULL,"
            + "status TEXT NOT NULL,created_at INTEGER NOT NULL,PRIMARY KEY(owner_id,id))");
        database.execSQL("CREATE INDEX IF NOT EXISTS group_messages_order_idx ON "
            + "group_messages(owner_id,group_id,created_at DESC,id DESC)");
        database.execSQL("CREATE INDEX IF NOT EXISTS group_messages_remote_idx ON "
            + "group_messages(owner_id,group_id,remote_id)");
    }

    private static void createGroupRemoteUniqueIndex(SQLiteDatabase database) {
        database.execSQL("CREATE UNIQUE INDEX IF NOT EXISTS group_messages_remote_unique_idx "
            + "ON group_messages(owner_id,group_id,remote_id) WHERE remote_id<>''");
    }

    public List<RemoteIMGroup> loadGroups(String ownerId) {
        List<RemoteIMGroup> groups = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().rawQuery(
            "SELECT group_id,name,member_count,pending_activation FROM im_groups WHERE owner_id=? "
                + "ORDER BY name COLLATE NOCASE",
            new String[]{clean(ownerId)})) {
            while (cursor.moveToNext())
                groups.add(new RemoteIMGroup(cursor.getString(0), cursor.getString(1),
                    cursor.getInt(2),cursor.getInt(3) != 0));
        }
        return groups;
    }

    public void replaceGroups(String ownerId, List<RemoteIMGroup> groups) {
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            database.delete("im_groups", "owner_id=?", new String[]{clean(ownerId)});
            for (RemoteIMGroup group : groups) {
                ContentValues values = new ContentValues();
                values.put("owner_id", clean(ownerId));
                values.put("group_id", group.groupId());
                values.put("name", group.name());
                values.put("member_count", group.memberCount());
                values.put("pending_activation", group.pendingActivation() ? 1 : 0);
                database.insertOrThrow("im_groups", null, values);
            }
            database.setTransactionSuccessful();
        } finally { database.endTransaction(); }
    }

    public void upsertGroup(String ownerId, RemoteIMGroup group) {
        ContentValues values = new ContentValues();
        values.put("owner_id", clean(ownerId));
        values.put("group_id", group.groupId());
        values.put("name", group.name());
        values.put("member_count", group.memberCount());
        values.put("pending_activation", group.pendingActivation() ? 1 : 0);
        getWritableDatabase().insertWithOnConflict("im_groups", null, values,
            SQLiteDatabase.CONFLICT_REPLACE);
    }

    public void upsertGroupMessage(String ownerId, RemoteIMGroupMessage message) {
        ContentValues values = new ContentValues();
        values.put("owner_id", clean(ownerId));
        values.put("id", message.id());
        values.put("remote_id", message.remoteId());
        values.put("group_id", message.groupId());
        values.put("sender_id", message.senderId());
        values.put("body", message.text());
        values.put("status", message.status().name());
        values.put("created_at", message.createdAtMillis());
        getWritableDatabase().insertWithOnConflict("group_messages", null, values,
            SQLiteDatabase.CONFLICT_REPLACE);
    }

    public void deleteGroupMessage(String ownerId, String messageId) {
        getWritableDatabase().delete("group_messages", "owner_id=? AND id=?",
            new String[]{clean(ownerId),clean(messageId)});
    }

    public List<RemoteIMGroupMessage> loadGroupSummaries(String ownerId) {
        List<RemoteIMGroupMessage> result = new ArrayList<>();
        String sql = "SELECT id,remote_id,group_id,sender_id,body,status,created_at "
            + "FROM group_messages WHERE owner_id=? AND id IN "
            + "(SELECT id FROM group_messages AS recent WHERE recent.owner_id=? "
            + "AND recent.group_id=group_messages.group_id ORDER BY created_at DESC,id DESC LIMIT 1)";
        try (Cursor cursor = getReadableDatabase().rawQuery(sql,
            new String[]{clean(ownerId), clean(ownerId)})) {
            while (cursor.moveToNext()) result.add(groupMessage(cursor));
        }
        return result;
    }

    public List<RemoteIMGroupMessage> loadGroupPage(String ownerId, String groupId,
            Long beforeTime, String beforeId, int limit) {
        List<RemoteIMGroupMessage> result = new ArrayList<>();
        String where = "owner_id=? AND group_id=?";
        List<String> args = new ArrayList<>();
        args.add(clean(ownerId)); args.add(clean(groupId));
        if (beforeTime != null && beforeId != null) {
            where += " AND (created_at<? OR (created_at=? AND id<?))";
            args.add(String.valueOf(beforeTime)); args.add(String.valueOf(beforeTime)); args.add(beforeId);
        }
        try (Cursor cursor = getReadableDatabase().query("group_messages",
            new String[]{"id","remote_id","group_id","sender_id","body","status","created_at"},
            where,args.toArray(new String[0]),null,null,"created_at DESC,id DESC",
            String.valueOf(Math.max(1, Math.min(limit, 100))))) {
            while (cursor.moveToNext()) result.add(groupMessage(cursor));
        }
        Collections.reverse(result);
        return result;
    }

    private static RemoteIMGroupMessage groupMessage(Cursor cursor) {
        RemoteIMGroupMessage.Status status;
        try { status = RemoteIMGroupMessage.Status.valueOf(cursor.getString(5)); }
        catch (RuntimeException ignored) { status = RemoteIMGroupMessage.Status.RECEIVED; }
        return new RemoteIMGroupMessage(cursor.getString(0), cursor.getString(1),
            cursor.getString(2), cursor.getString(3), cursor.getString(4),cursor.getLong(6),status);
    }

    public List<String> loadContactGroups(String ownerId) {
        List<String> groups = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query(
            "contact_groups",
            new String[]{"name"},
            "owner_id = ?",
            new String[]{clean(ownerId)},
            null,
            null,
            "sort_order ASC, name COLLATE NOCASE ASC"
        )) {
            while (cursor.moveToNext()) groups.add(cursor.getString(0));
        }
        return groups;
    }

    public List<RemoteIMContact> loadContacts(String ownerId) {
        List<RemoteIMContact> contacts = new ArrayList<>();
        String sql = "SELECT c.user_id,c.display_name,c.avatar_url,"
            + "CASE WHEN g.name IS NULL THEN '' ELSE c.group_name END "
            + "FROM contacts c LEFT JOIN contact_groups g "
            + "ON g.owner_id=c.owner_id AND g.name=c.group_name "
            + "WHERE c.owner_id=? ORDER BY c.display_name COLLATE NOCASE,c.user_id";
        try (Cursor cursor = getReadableDatabase().rawQuery(sql, new String[]{clean(ownerId)})) {
            while (cursor.moveToNext()) {
                contacts.add(new RemoteIMContact(
                    cursor.getString(0),
                    cursor.getString(1),
                    cursor.getString(2),
                    cursor.getString(3)
                ));
            }
        }
        return contacts;
    }

    public void upsertContact(String ownerId, RemoteIMContact contact) {
        String cleanOwnerId = clean(ownerId);
        ContentValues values = new ContentValues();
        values.put("display_name", contact.displayName());
        values.put("avatar_url", contact.avatarUrl());
        SQLiteDatabase database = getWritableDatabase();
        int updated = database.update(
            "contacts",
            values,
            "owner_id = ? AND user_id = ?",
            new String[]{cleanOwnerId, contact.userId()}
        );
        // 资料刷新只更新显示名和头像，不能用 REPLACE：REPLACE 会把本地 group_name
        // 一起重置。只有新联系人插入时才采纳其分组，且未知分组降级为空串。
        if (updated > 0) return;
        values.put("owner_id", cleanOwnerId);
        values.put("user_id", contact.userId());
        values.put("group_name", existingGroupName(database, cleanOwnerId, contact.groupName()));
        database.insertWithOnConflict("contacts", null, values, SQLiteDatabase.CONFLICT_IGNORE);
    }

    public boolean createContactGroup(String ownerId, String name) {
        String cleanOwnerId = clean(ownerId);
        String cleanName = ContactGroups.normalize(name);
        if (cleanOwnerId.isEmpty() || !ContactGroups.isAcceptableName(cleanName)) return false;
        SQLiteDatabase database = getWritableDatabase();
        long nextOrder = 0;
        try (Cursor cursor = database.rawQuery(
            "SELECT COALESCE(MAX(sort_order),-1)+1 FROM contact_groups WHERE owner_id=?",
            new String[]{cleanOwnerId}
        )) {
            if (cursor.moveToFirst()) nextOrder = cursor.getLong(0);
        }
        ContentValues values = new ContentValues();
        values.put("owner_id", cleanOwnerId);
        values.put("name", cleanName);
        values.put("sort_order", nextOrder);
        return database.insertWithOnConflict(
            "contact_groups", null, values, SQLiteDatabase.CONFLICT_IGNORE
        ) != -1;
    }

    public boolean renameContactGroup(String ownerId, String from, String to) {
        String cleanOwnerId = clean(ownerId);
        String oldName = ContactGroups.normalize(from);
        String newName = ContactGroups.normalize(to);
        if (cleanOwnerId.isEmpty() || !ContactGroups.isAcceptableName(newName)) return false;
        if (oldName.equals(newName)) return true;
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            ContentValues groupValues = new ContentValues();
            groupValues.put("name", newName);
            int changed = database.update(
                "contact_groups",
                groupValues,
                "owner_id = ? AND name = ?",
                new String[]{cleanOwnerId, oldName}
            );
            if (changed <= 0) return false;
            ContentValues contactValues = new ContentValues();
            contactValues.put("group_name", newName);
            database.update(
                "contacts",
                contactValues,
                "owner_id = ? AND group_name = ?",
                new String[]{cleanOwnerId, oldName}
            );
            database.setTransactionSuccessful();
            return true;
        } catch (RuntimeException error) {
            return false;
        } finally {
            database.endTransaction();
        }
    }

    public boolean deleteContactGroup(String ownerId, String name) {
        String cleanOwnerId = clean(ownerId);
        String cleanName = ContactGroups.normalize(name);
        if (cleanOwnerId.isEmpty() || cleanName.isEmpty()) return false;
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            ContentValues values = new ContentValues();
            values.put("group_name", "");
            database.update(
                "contacts", values, "owner_id = ? AND group_name = ?",
                new String[]{cleanOwnerId, cleanName}
            );
            int removed = database.delete(
                "contact_groups", "owner_id = ? AND name = ?",
                new String[]{cleanOwnerId, cleanName}
            );
            if (removed <= 0) return false;
            database.setTransactionSuccessful();
            return true;
        } finally {
            database.endTransaction();
        }
    }

    public boolean setContactGroup(String ownerId, String userId, String groupName) {
        String cleanOwnerId = clean(ownerId);
        String cleanUserId = clean(userId);
        if (cleanOwnerId.isEmpty() || cleanUserId.isEmpty()) return false;
        SQLiteDatabase database = getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("group_name", existingGroupName(
            database, cleanOwnerId, ContactGroups.normalize(groupName)
        ));
        return database.update(
            "contacts", values, "owner_id = ? AND user_id = ?",
            new String[]{cleanOwnerId, cleanUserId}
        ) > 0;
    }

    private static String existingGroupName(
        SQLiteDatabase database,
        String ownerId,
        String groupName
    ) {
        if (groupName.isEmpty()) return "";
        try (Cursor cursor = database.query(
            "contact_groups", new String[]{"name"},
            "owner_id = ? AND name = ?", new String[]{ownerId, groupName},
            null, null, null, "1"
        )) {
            return cursor.moveToFirst() ? groupName : "";
        }
    }

    public void deleteContact(String ownerId, String userId) {
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try {
            database.delete(
                "contacts",
                "owner_id = ? AND user_id = ?",
                new String[]{clean(ownerId), clean(userId)}
            );
            database.delete(
                "messages",
                "owner_id = ? AND peer_id = ?",
                new String[]{clean(ownerId), clean(userId)}
            );
            database.setTransactionSuccessful();
        } finally {
            database.endTransaction();
        }
    }

    public List<RemoteIMMessage> loadConversationSummaries(String ownerId) {
        List<RemoteIMMessage> messages = new ArrayList<>();
        String sql = "SELECT " + columns("m")
            + " FROM messages m"
            + " WHERE m.owner_id = ?"
            + " AND m.id = (SELECT i.id FROM messages i"
            + " WHERE i.owner_id = m.owner_id AND i.peer_id = m.peer_id"
            + " ORDER BY i.created_at DESC, i.id DESC LIMIT 1)"
            + " ORDER BY m.created_at ASC, m.id ASC";
        try (Cursor cursor = getReadableDatabase().rawQuery(sql, new String[]{clean(ownerId)})) {
            while (cursor.moveToNext()) messages.add(readMessage(cursor));
        }
        return messages;
    }

    public Page loadConversationPage(
        String ownerId,
        String peerId,
        Long beforeCreatedAt,
        String beforeMessageId,
        int limit
    ) {
        int safeLimit = Math.max(1, Math.min(limit, 200));
        ArrayList<String> arguments = new ArrayList<>();
        arguments.add(clean(ownerId));
        arguments.add(clean(peerId));
        StringBuilder where = new StringBuilder("owner_id = ? AND peer_id = ?");
        if (beforeCreatedAt != null && beforeMessageId != null) {
            where.append(" AND (created_at < ? OR (created_at = ? AND id < ?))");
            arguments.add(String.valueOf(beforeCreatedAt));
            arguments.add(String.valueOf(beforeCreatedAt));
            arguments.add(beforeMessageId);
        }
        List<RemoteIMMessage> descending = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().query(
            "messages",
            columnNames(),
            where.toString(),
            arguments.toArray(new String[0]),
            null,
            null,
            "created_at DESC, id DESC",
            String.valueOf(safeLimit + 1)
        )) {
            while (cursor.moveToNext()) descending.add(readMessage(cursor));
        }
        boolean hasEarlier = descending.size() > safeLimit;
        if (hasEarlier) descending.remove(descending.size() - 1);
        Collections.reverse(descending);
        return new Page(descending, hasEarlier);
    }

    /**
     * 搜索当前账号完整的持久化历史。`instr` 有意把 `%` 和 `_` 当作普通文字，
     * 而非 LIKE 通配符。SQLite lower() 只折叠 ASCII 大小写；中文仍按原文匹配。
     */
    public List<RemoteIMMessageSearchHit> searchMessages(
        String ownerId,
        String query,
        int limit
    ) {
        String cleanOwnerId = clean(ownerId);
        String cleanQuery = clean(query);
        int safeLimit = Math.max(1, Math.min(limit, 200));
        if (cleanOwnerId.isEmpty() || cleanQuery.isEmpty()) return Collections.emptyList();

        String sql = "SELECT " + columns("m") + ",m.peer_id FROM messages m"
            + " WHERE m.owner_id = ? AND m.body <> ''"
            + " AND instr(lower(m.body), lower(?)) > 0"
            + " ORDER BY m.created_at DESC, m.id DESC LIMIT ?";
        List<RemoteIMMessageSearchHit> hits = new ArrayList<>();
        try (Cursor cursor = getReadableDatabase().rawQuery(
            sql,
            new String[]{cleanOwnerId, cleanQuery, String.valueOf(safeLimit)}
        )) {
            while (cursor.moveToNext()) {
                String peerUserId = clean(cursor.getString(columnNames().length));
                if (!peerUserId.isEmpty()) {
                    hits.add(new RemoteIMMessageSearchHit(peerUserId, readMessage(cursor)));
                }
            }
        }
        return Collections.unmodifiableList(hits);
    }

    public boolean containsRemoteId(String ownerId, String remoteId) {
        String cleanRemoteId = clean(remoteId);
        if (cleanRemoteId.isEmpty()) return false;
        try (Cursor cursor = getReadableDatabase().rawQuery(
            "SELECT 1 FROM messages WHERE owner_id = ? AND remote_id = ? LIMIT 1",
            new String[]{clean(ownerId), cleanRemoteId}
        )) {
            return cursor.moveToFirst();
        }
    }

    public RemoteIMMessage messageWithRemoteId(String ownerId, String peerId, String remoteId) {
        String cleanRemoteId = clean(remoteId);
        if (cleanRemoteId.isEmpty()) return null;
        try (Cursor cursor = getReadableDatabase().query(
            "messages",
            columnNames(),
            "owner_id = ? AND peer_id = ? AND remote_id = ?",
            new String[]{clean(ownerId), clean(peerId), cleanRemoteId},
            null,
            null,
            "created_at DESC, id DESC",
            "1"
        )) {
            return cursor.moveToFirst() ? readMessage(cursor) : null;
        }
    }

    /** 普通入站第一阶段：事务中判断真实 SDK ID 是否已存在，只插入安全元数据，不把重复当新消息。 */
    public RemoteIMIngress.Stored insertInitialMetadata(String ownerId, RemoteIMMessage initial) {
        String owner = clean(ownerId), peer = initial.fromUserId();
        if (owner.isEmpty() || peer.isEmpty() || !owner.equals(initial.toUserId())
                || initial.direction() != RemoteIMMessage.Direction.INCOMING)
            throw new IllegalArgumentException("Invalid incoming metadata owner");
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransactionNonExclusive();
        try {
            if (!initial.remoteId().isEmpty()) try (Cursor rows = database.query("messages",
                    new String[]{"id", "remote_id", "created_at"}, "owner_id=? AND peer_id=? AND remote_id=?",
                    new String[]{owner, peer, initial.remoteId()}, null, null, "created_at DESC,id DESC", "1")) {
                if (rows.moveToFirst()) {
                    RemoteIMIngress.Stored existing = new RemoteIMIngress.Stored(rows.getString(0), rows.getString(1), rows.getLong(2), false);
                    database.setTransactionSuccessful(); return existing;
                }
            }
            // 第一阶段只保留安全历史元数据；即使普通调用者传入完整对象，也不提前落审批控制令牌。
            RemoteIMMessage safe = new RemoteIMMessage(initial.id(), initial.remoteId(), initial.fromUserId(), initial.toUserId(),
                initial.text(), initial.direction(), initial.status(), initial.createdAtMillis(), initial.imageAttachment(),
                initial.voiceAttachment(), initial.fileAttachment(), initial.videoAttachment(), initial.origin());
            safe.setQuote(initial.quote()); safe.setCaptionAbove(initial.captionAbove());
            upsertMessage(owner, safe);
            try (Cursor check = database.rawQuery("SELECT 1 FROM messages WHERE owner_id=? AND id=?",
                    new String[]{owner, initial.id()})) {
                if (!check.moveToFirst()) throw new IllegalStateException("Incoming metadata was not persisted");
            }
            database.setTransactionSuccessful();
            return new RemoteIMIngress.Stored(initial.id(), initial.remoteId(), initial.createdAtMillis(), true);
        } finally { database.endTransaction(); }
    }

    // 作用：按账号和消息稳定标识插入或更新本地记录。
    public void upsertMessage(String ownerId, RemoteIMMessage message) {
        String owner = clean(ownerId);
        String peerId = message.fromUserId().equals(owner)
            ? message.toUserId()
            : message.fromUserId();
        ContentValues values = new ContentValues();
        values.put("owner_id", owner);
        values.put("id", message.id());
        values.put("remote_id", message.remoteId());
        values.put("peer_id", peerId);
        values.put("from_id", message.fromUserId());
        values.put("to_id", message.toUserId());
        values.put("body", message.text());
        values.put("direction", message.direction().name());
        values.put("status", message.status().name());
        values.put("created_at", message.createdAtMillis());
        RemoteIMImageAttachment image = message.imageAttachment();
        values.put("image_path", image == null ? "" : storedMediaPath(image.localPath()));
        values.put("image_width", image == null ? 0 : image.width());
        values.put("image_height", image == null ? 0 : image.height());
        values.put("image_size", image == null ? 0 : image.sizeBytes());
        RemoteIMVoiceAttachment voice = message.voiceAttachment();
        values.put("voice_path", voice == null ? "" : storedMediaPath(voice.localPath()));
        values.put("voice_duration", voice == null ? 0 : voice.durationSeconds());
        RemoteIMFileAttachment file = message.fileAttachment();
        values.put("file_path", file == null ? "" : storedMediaPath(file.localPath()));
        values.put("file_name", file == null ? "" : file.fileName());
        values.put("file_mime", file == null ? "" : file.mimeType());
        values.put("file_size", file == null ? 0 : file.sizeBytes());
        RemoteIMVideoAttachment video = message.videoAttachment();
        values.put("video_path", video == null ? "" : storedMediaPath(video.localPath()));
        values.put("video_cover_path", video == null ? "" : storedMediaPath(video.coverPath()));
        values.put("video_duration", video == null ? 0 : video.durationSeconds());
        values.put("video_width", video == null ? 0 : video.width());
        values.put("video_height", video == null ? 0 : video.height());
        values.put("video_size", video == null ? 0 : video.sizeBytes());
        RemoteIMApprovalRequest approvalRequest = message.approvalRequest();
        values.put(
            "approval_request_token",
            approvalRequest == null ? "" : approvalRequest.token()
        );
        values.put(
            "approval_request_actions",
            approvalRequest == null ? "" : approvalActions(approvalRequest.actions())
        );
        RemoteIMApprovalDecision approvalDecision = message.approvalDecision();
        values.put(
            "approval_decision_token",
            approvalDecision == null ? "" : approvalDecision.token()
        );
        values.put(
            "approval_decision_action",
            approvalDecision == null ? "" : approvalDecision.action().wireValue()
        );
        values.put("caption_above", message.captionAbove() ? 1 : 0);
        RemoteIMQuote quote = message.quote();
        values.put("quote_msg_id", quote == null ? "" : quote.messageId());
        values.put("quote_sender", quote == null ? "" : quote.senderId());
        values.put("quote_digest", quote == null ? "" : quote.digest());
        values.put("quote_kind", quote == null ? "" : quote.kind());
        values.put("origin", message.origin().wireValue());
        getWritableDatabase().insertWithOnConflict(
            "messages",
            null,
            values,
            SQLiteDatabase.CONFLICT_REPLACE
        );
    }

    public void deleteConversation(String ownerId, String peerId) {
        getWritableDatabase().delete(
            "messages",
            "owner_id = ? AND peer_id = ?",
            new String[]{clean(ownerId), clean(peerId)}
        );
    }

    private static String[] columnNames() {
        return new String[]{
            "id", "remote_id", "from_id", "to_id", "body", "direction", "status",
            "created_at", "image_path", "image_width", "image_height", "image_size",
            "voice_path", "voice_duration", "file_path", "file_name", "file_mime",
            "file_size", "origin",
            "video_path", "video_cover_path", "video_duration", "video_width",
            "video_height", "video_size", "approval_request_token",
            "approval_request_actions", "approval_decision_token",
            "approval_decision_action", "caption_above", "quote_msg_id", "quote_sender",
            "quote_digest", "quote_kind"
        };
    }

    private static String columns(String alias) {
        StringBuilder result = new StringBuilder();
        String prefix = alias == null || alias.isEmpty() ? "" : alias + ".";
        for (String column : columnNames()) {
            if (result.length() > 0) result.append(',');
            result.append(prefix).append(column);
        }
        return result.toString();
    }

    /**
     * 媒体路径入库前压成相对形式。落在媒体根之外的不入库——但静默丢附件极难排查，
     * 所以留一行日志。正常收发的媒体都由 RemoteIMMediaPaths 分配位置，不会走到这里。
     */
    private String storedMediaPath(String runtimePath) {
        String stored = mediaPaths.toStoredPath(runtimePath);
        if (stored.isEmpty() && runtimePath != null && !runtimePath.trim().isEmpty()) {
            Log.w(TAG, "history: media path outside the media root was dropped."
                + " path=" + runtimePath
                + " <- attachment will not be persisted");
        }
        return stored;
    }

    // 作用：从数据库游标重建完整消息及附件字段。
    private RemoteIMMessage readMessage(Cursor cursor) {
        RemoteIMImageAttachment image = null;
        String imagePath = mediaPaths.toAbsolutePath(cursor.getString(8));
        if (!imagePath.isEmpty()) {
            image = new RemoteIMImageAttachment(
                imagePath,
                cursor.getInt(9),
                cursor.getInt(10),
                cursor.getLong(11)
            );
        }
        RemoteIMVoiceAttachment voice = null;
        String voicePath = mediaPaths.toAbsolutePath(cursor.getString(12));
        if (!voicePath.isEmpty()) {
            voice = new RemoteIMVoiceAttachment(voicePath, cursor.getInt(13));
        }
        RemoteIMFileAttachment file = null;
        String filePath = mediaPaths.toAbsolutePath(cursor.getString(14));
        if (!filePath.isEmpty()) {
            file = new RemoteIMFileAttachment(
                filePath,
                cursor.getString(15),
                cursor.getString(16),
                cursor.getLong(17)
            );
        }
        RemoteIMVideoAttachment video = null;
        String videoPath = mediaPaths.toAbsolutePath(cursor.getString(19));
        if (!videoPath.isEmpty()) {
            video = new RemoteIMVideoAttachment(
                videoPath,
                mediaPaths.toAbsolutePath(cursor.getString(20)),
                cursor.getInt(21),
                cursor.getInt(22),
                cursor.getInt(23),
                cursor.getLong(24)
            );
        }
        RemoteIMApprovalRequest approvalRequest = readApprovalRequest(
            cursor.getString(25),
            cursor.getString(26)
        );
        RemoteIMApprovalDecision approvalDecision = readApprovalDecision(
            cursor.getString(27),
            cursor.getString(28)
        );
        RemoteIMMessage message = new RemoteIMMessage(
            cursor.getString(0),
            cursor.getString(1),
            cursor.getString(2),
            cursor.getString(3),
            cursor.getString(4),
            RemoteIMMessage.Direction.valueOf(cursor.getString(5)),
            RemoteIMMessage.Status.valueOf(cursor.getString(6)),
            cursor.getLong(7),
            image,
            voice,
            file,
            video,
            RemoteIMOrigin.fromWireValue(cursor.getString(18)),
            approvalRequest,
            approvalDecision
        );
        message.setCaptionAbove(cursor.getInt(29) != 0);
        String quoteDigest = clean(cursor.getString(32));
        if (!quoteDigest.isEmpty()) {
            try {
                message.setQuote(new RemoteIMQuote(
                    cursor.getString(30),
                    cursor.getString(31),
                    quoteDigest,
                    cursor.getString(33)
                ));
            } catch (IllegalArgumentException ignored) {
                // 落库引用即使格式损坏，也不能让整条消息无法读取。
            }
        }
        return message;
    }

    private static String approvalActions(List<RemoteIMApprovalAction> actions) {
        StringBuilder value = new StringBuilder();
        for (RemoteIMApprovalAction action : actions) {
            if (value.length() > 0) value.append(',');
            value.append(action.wireValue());
        }
        return value.toString();
    }

    private static RemoteIMApprovalRequest readApprovalRequest(String token, String rawActions) {
        String cleanToken = clean(token);
        String cleanActions = clean(rawActions);
        if (cleanToken.isEmpty() || cleanActions.isEmpty()) return null;
        List<RemoteIMApprovalAction> actions = new ArrayList<>();
        for (String rawAction : cleanActions.split(",", -1)) {
            RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
            if (action == null) return null;
            actions.add(action);
        }
        try {
            return new RemoteIMApprovalRequest(cleanToken, actions);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static RemoteIMApprovalDecision readApprovalDecision(String token, String rawAction) {
        String cleanToken = clean(token);
        RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
        if (cleanToken.isEmpty() || action == null) return null;
        try {
            return new RemoteIMApprovalDecision(cleanToken, action);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
