// 文件作用：在 Robolectric 临时数据库验证已受理账号清理，只删除对应 owner 与不共享的受管媒体。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import java.io.File;
import java.nio.file.Files;
import java.util.UUID;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class AndroidAcceptedAccountPurgeTest {
    private File media(Context context, String name) throws Exception {
        File file = new File(RemoteIMMediaPaths.forApp(context).root(), "Incoming/Images/" + name);
        assertTrue(file.getParentFile().isDirectory() || file.getParentFile().mkdirs()); Files.write(file.toPath(), new byte[]{1}); return file;
    }
    private void message(SQLiteDatabase db, String owner, String id, String path) {
        ContentValues row = new ContentValues(); row.put("owner_id", owner); row.put("id", id); row.put("peer_id", "peer");
        row.put("from_id", owner); row.put("to_id", "peer"); row.put("body", "fixture"); row.put("direction", "OUTGOING");
        row.put("status", "SENT"); row.put("created_at", 1L); row.put("image_path", path);
        db.insertOrThrow("messages", null, row);
    }
    private int count(SQLiteDatabase db, String table, String owner) {
        try (Cursor cursor = db.rawQuery("SELECT COUNT(*) FROM " + table + " WHERE owner_id=?", new String[]{owner})) { cursor.moveToFirst(); return cursor.getInt(0); }
    }

    @Test public void purgesOnlyOwnerRowsAndMediaNotReferencedByAnotherAccount() throws Exception {
        Context context = RuntimeEnvironment.getApplication();
        String name = "accepted-purge-fixture-" + UUID.randomUUID() + ".db";
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name);
        File own = media(context, "a.jpg"), shared = media(context, "shared.jpg"), other = media(context, "b.jpg");
        try {
            SQLiteDatabase db = store.getWritableDatabase();
            store.upsertContact("owner-a", new RemoteIMContact("peer-a", "Alice")); store.upsertContact("owner-b", new RemoteIMContact("peer-b", "Bob"));
            store.createContactGroup("owner-a", "A"); store.createContactGroup("owner-b", "B");
            message(db, "owner-a", "a", "Incoming/Images/a.jpg"); message(db, "owner-a", "as", "Incoming/Images/shared.jpg");
            message(db, "owner-b", "b", "Incoming/Images/b.jpg"); message(db, "owner-b", "bs", "Incoming/Images/shared.jpg");
            store.purgeAcceptedAccount("owner-a");
            for (String table : new String[]{"messages", "contacts", "contact_groups", "im_groups", "group_messages"}) assertEquals(0, count(db, table, "owner-a"));
            assertEquals(2, count(db, "messages", "owner-b")); assertEquals(1, count(db, "contacts", "owner-b"));
            assertFalse(own.exists()); assertTrue(shared.exists()); assertTrue(other.exists());
        } finally { store.close(); context.deleteDatabase(name); own.delete(); shared.delete(); other.delete(); }
    }

    @Test public void absoluteAndParentPathsCannotDeleteUnmanagedFiles() throws Exception {
        Context context = RuntimeEnvironment.getApplication(); String name = "accepted-purge-fixture-" + UUID.randomUUID() + ".db";
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name);
        File outside = File.createTempFile("unmanaged-project-fixture-", ".txt");
        try {
            message(store.getWritableDatabase(), "owner-a", "absolute", outside.getAbsolutePath());
            message(store.getWritableDatabase(), "owner-a", "parent", "../../" + outside.getName());
            store.purgeAcceptedAccount("owner-a");
            assertTrue(outside.exists()); assertEquals(0, count(store.getReadableDatabase(), "messages", "owner-a"));
        } finally { store.close(); context.deleteDatabase(name); outside.delete(); }
    }
}
