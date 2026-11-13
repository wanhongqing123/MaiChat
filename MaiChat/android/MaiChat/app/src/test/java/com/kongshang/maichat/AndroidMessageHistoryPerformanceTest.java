// 文件作用：使用合成历史测量 SQL 查询计划、分页和取消，不接触用户数据库或线上服务。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteStatement;
import android.os.CancellationSignal;
import androidx.test.core.app.ApplicationProvider;
import java.util.concurrent.TimeUnit;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AndroidMessageHistoryPerformanceTest {
    @Test public void concurrentReopenRejectsAnOldQueryBeforeCursorPublication() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        String name = "history-reopen-synthetic.db";
        context.deleteDatabase(name);
        java.util.concurrent.ExecutorService worker = java.util.concurrent.Executors.newSingleThreadExecutor();
        try (AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name)) {
            SQLiteDatabase database = store.getWritableDatabase();
            database.execSQL("INSERT INTO messages(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at)"
                + " VALUES('synthetic-owner','a','peer','peer','synthetic-owner','common','INCOMING','RECEIVED',1),"
                + "('synthetic-owner','b','peer','peer','synthetic-owner','common','INCOMING','RECEIVED',2)");
            java.lang.reflect.Field registryField = AndroidChatHistoryStore.class.getDeclaredField("historyCursors");
            registryField.setAccessible(true);
            Object registry = registryField.get(store);
            java.util.concurrent.atomic.AtomicReference<Thread> querying = new java.util.concurrent.atomic.AtomicReference<>();
            java.util.concurrent.Future<RemoteIMMessageHistory.Page> result;
            synchronized (registry) {
                result = worker.submit(() -> {
                    querying.set(Thread.currentThread());
                    return store.queryMessages("synthetic-owner", "login-1",
                        new RemoteIMMessageHistory.Query("peer", "common", 1, "", "", null, null, ""), new CancellationSignal());
                });
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
                while (System.nanoTime() < deadline && (querying.get() == null || querying.get().getState() != Thread.State.BLOCKED))
                    Thread.sleep(1);
                assertEquals(Thread.State.BLOCKED, querying.get().getState());
                // 旧读已捕获 handle；在发布游标的锁释放前替换 helper 的实际数据库实例。
                store.close();
                store.getWritableDatabase();
            }
            try { result.get(2, TimeUnit.SECONDS); fail("An old database query must not publish a new-instance cursor"); }
            catch (java.util.concurrent.ExecutionException changedDatabase) {
                assertTrue(changedDatabase.getCause() instanceof IllegalStateException);
            }
        } finally { worker.shutdownNow(); context.deleteDatabase(name); }
    }

    @Test public void schemaEightUpgradePreservesRowsAndAddsGlobalOrderIndex() {
        Context context = ApplicationProvider.getApplicationContext();
        String name = "history-migration-synthetic.db";
        context.deleteDatabase(name);
        try {
            try (AndroidChatHistoryStore original = new AndroidChatHistoryStore(context, name)) {
                SQLiteDatabase database = original.getWritableDatabase();
                database.execSQL("INSERT INTO messages(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at)"
                    + " VALUES('synthetic-owner','retained','peer','peer','synthetic-owner','common','INCOMING','RECEIVED',1)");
                database.execSQL("DROP INDEX messages_owner_order_idx");
                database.setVersion(8);
            }
            try (AndroidChatHistoryStore upgraded = new AndroidChatHistoryStore(context, name)) {
                SQLiteDatabase database = upgraded.getReadableDatabase();
                assertEquals(9, database.getVersion());
                assertEquals("retained", upgraded.queryMessages("synthetic-owner", "login-1", query("peer", "common", ""),
                    new CancellationSignal()).rows.get(0).id);
                assertTrue(explain(database, false).contains("messages_owner_order_idx"));
            }
        } finally { context.deleteDatabase(name); }
    }

    @Test public void historyEnablesWalForConcurrentIncomingWrites() {
        Context context = ApplicationProvider.getApplicationContext();
        String name = "history-wal-synthetic.db";
        context.deleteDatabase(name);
        try (AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name)) {
            assertTrue("History readers must not block incoming writes", store.getWritableDatabase().isWriteAheadLoggingEnabled());
        } finally { context.deleteDatabase(name); }
    }

    @Test public void syntheticTenThousandAndHundredThousandQueriesStayBounded() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        String name = "history-performance-synthetic.db";
        context.deleteDatabase(name);
        try (AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name)) {
            SQLiteDatabase database = store.getWritableDatabase();
            String globalPlan = "";
            String olderCursor = null;
            for (int size : new int[]{10_000, 100_000}) {
                database.delete("messages", null, null);
                database.beginTransaction();
                try (SQLiteStatement insert = database.compileStatement("INSERT INTO messages"
                        + "(owner_id,id,peer_id,from_id,to_id,body,direction,status,created_at)"
                        + " VALUES ('synthetic-owner',?,?,?,'synthetic-owner',?,'INCOMING','RECEIVED',?)")) {
                    for (int index = 0; index < size; index++) {
                        insert.bindString(1, String.format(java.util.Locale.ROOT, "m-%08d", index));
                        insert.bindString(2, "peer-" + (index % 1000));
                        insert.bindString(3, "peer-" + (index % 1000));
                        insert.bindString(4, index % 5000 == 7 ? "common rare-marker" : "common body");
                        insert.bindLong(5, index / 3);
                        insert.executeInsert();
                    }
                    database.setTransactionSuccessful();
                } finally { database.endTransaction(); }
                measure(store, size, "specific", query("peer-7", "common", ""), size / 1000, size > 50_000);
                RemoteIMMessageHistory.Page first = measure(store, size, "cross-peer", query("", "common", ""), 50, true);
                measure(store, size, "rare", query("", "rare-marker", ""), size / 5000, false);
                measure(store, size, "no-hit", query("", "absent-marker", ""), 0, false);
                RemoteIMMessageHistory.Page next = measure(store, size, "paging", query("", "common", first.nextCursor), 50, true);
                assertTrue(first.rows.get(49).createdAtMs >= next.rows.get(0).createdAtMs);
                // 保留旧边界后写入更多更新消息，测量翻到 90% 历史位置时是否扫描全部新行。
                if (size == 10_000) olderCursor = first.nextCursor;
                else measure(store, size, "late-page", query("", "common", olderCursor), 50, true);
                globalPlan = explain(database, false);
                System.out.println("HISTORY_PLAN rows=" + size + " global=" + globalPlan + " peer=" + explain(database, true));
            }
            assertFalse("Global history order must use an owner/date index: " + globalPlan, globalPlan.contains("TEMP B-TREE"));
            java.util.concurrent.ExecutorService reader = java.util.concurrent.Executors.newSingleThreadExecutor();
            try {
                java.util.concurrent.CountDownLatch reading = new java.util.concurrent.CountDownLatch(1);
                java.util.concurrent.Future<?> scanning = reader.submit(() -> {
                    reading.countDown();
                    for (int iteration = 0; iteration < 10; iteration++)
                        store.queryMessages("synthetic-owner", "login-1", query("", "absent-marker", ""), new CancellationSignal());
                });
                assertTrue(reading.await(1, TimeUnit.SECONDS));
                long writeStarted = System.nanoTime();
                database.execSQL("UPDATE messages SET status='RECEIVED' WHERE owner_id='synthetic-owner' AND id='m-00000001'");
                long writeMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - writeStarted);
                System.out.println("HISTORY_CONCURRENCY incoming_write_ms=" + writeMs);
                assertTrue("A history reader blocked incoming writes", writeMs < 250);
                scanning.get(5, TimeUnit.SECONDS);
                CancellationSignal active = new CancellationSignal();
                java.util.concurrent.CountDownLatch starting = new java.util.concurrent.CountDownLatch(1);
                java.util.concurrent.Future<?> running = reader.submit(() -> {
                    starting.countDown();
                    store.queryMessages("synthetic-owner", "login-1", query("", "absent-marker", ""), active);
                });
                assertTrue(starting.await(1, TimeUnit.SECONDS));
                Thread.sleep(2);
                long cancelStarted = System.nanoTime();
                active.cancel();
                try { running.get(1, TimeUnit.SECONDS); }
                catch (java.util.concurrent.ExecutionException canceledDuringSql) {
                    assertTrue(canceledDuringSql.getCause() instanceof android.os.OperationCanceledException);
                }
                System.out.println("HISTORY_CANCEL running_ms=" + TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - cancelStarted));
            } finally { reader.shutdownNow(); }
            CancellationSignal canceled = new CancellationSignal();
            canceled.cancel();
            long started = System.nanoTime();
            assertThrows(android.os.OperationCanceledException.class,
                () -> store.queryMessages("synthetic-owner", "login-1", query("", "absent-marker", ""), canceled));
            System.out.println("HISTORY_CANCEL pre_canceled_ms=" + TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started));
        } finally { context.deleteDatabase(name); }
    }

    private static RemoteIMMessageHistory.Page measure(AndroidChatHistoryStore store, int size,
            String label, RemoteIMMessageHistory.Query query, int expectedCount, boolean more) {
        long started = System.nanoTime();
        RemoteIMMessageHistory.Page page = store.queryMessages("synthetic-owner", "login-1", query, new CancellationSignal());
        long millis = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started);
        System.out.println("HISTORY_QUERY rows=" + size + " case=" + label + " elapsed_ms=" + millis
            + " count=" + page.rows.size() + " has_more=" + page.hasMore);
        assertEquals(Math.min(expectedCount, 50), page.rows.size());
        assertEquals(more, page.hasMore);
        assertTrue("Synthetic query exceeded subsecond budget: " + label, millis < 1000);
        return page;
    }

    private static RemoteIMMessageHistory.Query query(String peer, String text, String cursor) {
        return new RemoteIMMessageHistory.Query(peer, text, 50, "", "", null, null, cursor);
    }

    private static String explain(SQLiteDatabase database, boolean peer) {
        StringBuilder plan = new StringBuilder();
        try (Cursor cursor = database.rawQuery("EXPLAIN QUERY PLAN SELECT id,created_at FROM messages m"
                + " WHERE m.owner_id=? AND m.peer_id<>''" + (peer ? " AND m.peer_id='peer-7'" : "")
                + " AND instr(lower(m.body),lower(?))>0 ORDER BY m.created_at DESC,m.id DESC LIMIT 51",
                new String[]{"synthetic-owner", "common"})) {
            while (cursor.moveToNext()) { if (plan.length() > 0) plan.append("; "); plan.append(cursor.getString(3)); }
        }
        return plan.toString();
    }
}
