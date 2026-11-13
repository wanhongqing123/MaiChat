// 文件作用：用独立 SQLite 历史验证 IM 工具，避免页面缓存窗口掩盖持久化查询缺陷。
// 运行边界：只操作 Robolectric 测试数据库，不连接账号、IM SDK 或模型服务。
package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import com.kongshang.maichat.AndroidChatHistoryStore;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.LocalChatHistoryStore;
import com.kongshang.maichat.LocalSettingsStore;
import com.kongshang.maichat.RemoteIMMessage;
import com.kongshang.maichat.RemoteIMMessageHistory;
import com.kongshang.maichat.RemoteIMOrigin;
import com.kongshang.maichat.RemoteIMSessionController;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MaiChatMessageHistoryToolsTest {
    private Context context;
    private AndroidChatHistoryStore store;
    private RemoteIMSessionController session;
    private MaiChatIMTools tools;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    @Before public void setUp() throws Exception {
        context = ApplicationProvider.getApplicationContext();
        context.deleteDatabase("maichat-history.db");
        store = new AndroidChatHistoryStore(context);
        java.io.File root = Files.createTempDirectory("maichat-message-tool").toFile();
        session = new RemoteIMSessionController(new LocalSettingsStore(new java.io.File(root, "settings")),
            new LocalChatHistoryStore(new java.io.File(root, "legacy")));
        session.login("history-owner");
        // 使用现有构造器隔离 SDK；注入真实数据库，页面缓存保持未打开状态。
        setField("historyStore", store);
        setField("mainHandler", new Handler(Looper.getMainLooper()));
        setField("productionMode", true);
        tools = new MaiChatIMTools(null, session);
    }

    @After public void tearDown() throws Exception {
        worker.shutdownNow();
        ((ExecutorService) field("ioExecutor")).shutdownNow();
        ((ExecutorService) field("messageHistoryExecutor")).shutdownNow();
        store.close();
        context.deleteDatabase("maichat-history.db");
    }

    @Test public void peerFilterRunsBeforeLimitAcrossMoreThanTwoHundredOtherPeerRows() throws Exception {
        store.upsertMessage("history-owner", message("old-target", "target", "literal%_needle", 1));
        for (int index = 0; index < 240; index++)
            store.upsertMessage("history-owner", message("noise-" + index, "other", "literal%_needle", 100 + index));
        JSONObject result = execute("maichat_search_messages", new JSONObject()
            .put("query", "literal%_needle").put("peer_id", "target").put("limit", 1));
        assertEquals(1, result.getInt("count"));
        assertEquals("old-target", result.getJSONArray("matches").getJSONObject(0).getString("id"));
        assertEquals("local_persisted", result.getString("coverage"));
        assertFalse(result.getBoolean("cloud_history_complete"));
        assertTrue(session.chatState().messagesWith("target").isEmpty());
    }

    @Test public void getReadsUnopenedRetainedConversationAndPaginatesEqualTimestamps() throws Exception {
        for (String id : new String[]{"a", "b", "c"})
            store.upsertMessage("history-owner", message(id, "retained", "history", 100));
        store.upsertMessage("another-owner", message("other-account", "retained", "private", 500));
        JSONObject first = execute("maichat_get_messages", new JSONObject().put("peer_id", "retained").put("limit", 2));
        assertEquals(2, first.getInt("count"));
        assertEquals("c", first.getJSONArray("messages").getJSONObject(0).getString("id"));
        assertEquals("b", first.getJSONArray("messages").getJSONObject(1).getString("id"));
        assertTrue(first.getBoolean("has_more"));
        JSONObject second = execute("maichat_get_messages", new JSONObject().put("peer_id", "retained")
            .put("limit", 2).put("cursor", first.getString("next_cursor")));
        assertEquals(1, second.getInt("count"));
        assertEquals("a", second.getJSONArray("messages").getJSONObject(0).getString("id"));
        assertFalse(second.getBoolean("has_more"));
        assertFalse(second.getBoolean("snapshot"));
        assertEquals("created_at_ms_desc_id_desc", second.getString("order"));
        assertTrue(session.chatState().messagesWith("retained").isEmpty());
    }

    @Test public void inclusiveTimeDirectionAndKindFiltersAreAppliedInDatabase() throws Exception {
        store.upsertMessage("history-owner", message("lower", "target", "needle", 10));
        store.upsertMessage("history-owner", message("upper", "target", "needle", 20));
        store.upsertMessage("history-owner", message("outside", "target", "needle", 21));
        JSONObject result = execute("maichat_search_messages", new JSONObject().put("query", "needle")
            .put("peer_id", "target").put("since_ms", "10").put("until_ms", 20L)
            .put("kind", "text").put("direction", "incoming"));
        assertEquals(2, result.getInt("count"));
        assertEquals("upper", result.getJSONArray("matches").getJSONObject(0).getString("id"));
        JSONObject outgoing = execute("maichat_get_messages", new JSONObject().put("peer_id", "target")
            .put("direction", "outgoing"));
        assertEquals(0, outgoing.getInt("count"));
        assertEquals("no_matches", outgoing.getString("status"));
    }

    @Test public void invalidFiltersAndDatabaseFailureAreErrorsNotEmptySuccess() throws Exception {
        store.upsertMessage("history-owner", message("one", "target", "needle", 10));
        assertToolError("maichat_get_messages", new JSONObject().put("peer_id", "target").put("query", "needle"));
        assertToolError("maichat_search_messages", new JSONObject().put("query", "needle").put("since_ms", 1.5));
        assertToolError("maichat_search_messages", new JSONObject().put("query", "needle").put("kind", "unknown"));
        store.getWritableDatabase().execSQL("DROP TABLE messages");
        assertToolError("maichat_get_messages", new JSONObject().put("peer_id", "target"));
    }

    @Test public void cursorRejectsChangedFiltersOrLoginGeneration() throws Exception {
        store.upsertMessage("history-owner", message("a", "target", "needle", 10));
        store.upsertMessage("history-owner", message("b", "target", "needle", 20));
        JSONObject first = execute("maichat_search_messages", new JSONObject().put("query", "needle")
            .put("peer_id", "target").put("limit", 1));
        String cursor = first.getString("next_cursor");
        assertToolError("maichat_search_messages", new JSONObject().put("query", "other")
            .put("peer_id", "target").put("limit", 1).put("cursor", cursor));
        setField("accountGeneration", ((Integer) field("accountGeneration")) + 1);
        assertToolError("maichat_search_messages", new JSONObject().put("query", "needle")
            .put("peer_id", "target").put("limit", 1).put("cursor", cursor));
    }

    @Test public void accountChangeWhileDatabaseWorkIsQueuedReturnsError() throws Exception {
        store.upsertMessage("history-owner", message("private", "target", "needle", 10));
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        ((ExecutorService) field("messageHistoryExecutor")).submit(() -> { release.await(); return null; });
        java.util.concurrent.atomic.AtomicReference<Thread> querying = new java.util.concurrent.atomic.AtomicReference<>();
        Future<Object> result = worker.submit(() -> {
            querying.set(Thread.currentThread());
            return tools.execute("maichat_get_messages", new JSONObject().put("peer_id", "target"));
        });
        long readyDeadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (!result.isDone() && System.nanoTime() < readyDeadline
                && (querying.get() == null || querying.get().getState() != Thread.State.TIMED_WAITING))
            Thread.sleep(2);
        // 捕获请求身份后才切代际，防止旧查询将前账号内容交付到新登录会话。
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        setField("accountGeneration", ((Integer) field("accountGeneration")) + 1);
        release.countDown();
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (!result.isDone() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(2);
        }
        try { result.get(1, java.util.concurrent.TimeUnit.SECONDS); fail("Expected changed-owner error"); }
        catch (java.util.concurrent.ExecutionException expected) {
            assertTrue(expected.getCause() instanceof IllegalStateException);
        }
    }

    @Test public void queryBypassesNetworkQueueAndLeavesMainResponsive() throws Exception {
        store.upsertMessage("history-owner", message("one", "target", "needle", 10));
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        ((ExecutorService) field("ioExecutor")).submit(() -> { release.await(); return null; });
        try {
            java.util.concurrent.atomic.AtomicBoolean heartbeat = new java.util.concurrent.atomic.AtomicBoolean();
            new Handler(Looper.getMainLooper()).post(() -> heartbeat.set(true));
            JSONObject output = execute("maichat_get_messages", new JSONObject().put("peer_id", "target"));
            assertEquals(1, output.getInt("count"));
            assertTrue(heartbeat.get());
        } finally { release.countDown(); }
    }

    @Test public void canceledQueuedQueryCompletesOnceWithoutReadingOrMutatingPage() throws Exception {
        store.upsertMessage("history-owner", message("one", "target", "needle", 10));
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.ThreadPoolExecutor executor = (java.util.concurrent.ThreadPoolExecutor) field("messageHistoryExecutor");
        executor.submit(() -> { release.await(); return null; });
        java.util.concurrent.atomic.AtomicInteger callbacks = new java.util.concurrent.atomic.AtomicInteger();
        java.util.concurrent.atomic.AtomicReference<String> failure = new java.util.concurrent.atomic.AtomicReference<>();
        RemoteIMSessionController.AccountTask request = session.queryPersistedMessages(
            new RemoteIMMessageHistory.Query("target", null, 50, "", "", null, null, ""), (page, error) -> {
                callbacks.incrementAndGet(); failure.set(error); assertEquals(null, page);
            });
        try {
            request.cancel();
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertEquals(1, callbacks.get());
            assertTrue(failure.get() != null);
            assertEquals(0, executor.getQueue().size());
            assertTrue(session.chatState().messagesWith("target").isEmpty());
        } finally { release.countDown(); }
    }

    @Test public void queryDeadlineCancelsQueuedWorkWhileMainLooperIsHeld() throws Exception {
        store.upsertMessage("history-owner", message("deadline", "target", "synthetic needle", 10));
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.ThreadPoolExecutor executor = (java.util.concurrent.ThreadPoolExecutor) field("messageHistoryExecutor");
        executor.submit(() -> { release.await(); return null; });
        java.util.concurrent.atomic.AtomicInteger callbacks = new java.util.concurrent.atomic.AtomicInteger();
        java.util.concurrent.atomic.AtomicReference<String> error = new java.util.concurrent.atomic.AtomicReference<>();
        RemoteIMSessionController.AccountTask request = session.queryPersistedMessages(
            new RemoteIMMessageHistory.Query("target", "needle", 50, "", "", null, null, ""), (page, failure) -> {
                callbacks.incrementAndGet(); error.set(failure); assertEquals(null, page);
            });
        try {
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertEquals(1, executor.getQueue().size());
            // 模拟主线程长任务：不推进 Looper，数据库队列仍必须按真实的五秒预算独立取消。
            long started = System.nanoTime();
            Thread.sleep(5_300);
            assertEquals("A busy main looper must not extend the DB queue budget", 0, executor.getQueue().size());
            assertEquals(0, callbacks.get());
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertEquals(1, callbacks.get());
            assertTrue(error.get() != null);
            System.out.println("HISTORY_MAIN_HELD deadline_cleanup_ms="
                + java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started));
        } finally { request.cancel(); release.countDown(); }
    }

    @Test public void missingDatabaseAndEmptyCursorPageDoNotClaimNoMatches() throws Exception {
        store.upsertMessage("history-owner", message("a", "target", "needle", 10));
        store.upsertMessage("history-owner", message("b", "target", "needle", 20));
        JSONObject first = execute("maichat_get_messages", new JSONObject().put("peer_id", "target").put("limit", 1));
        store.getWritableDatabase().delete("messages", "id=?", new String[]{"a"});
        JSONObject next = execute("maichat_get_messages", new JSONObject().put("peer_id", "target")
            .put("limit", 1).put("cursor", first.getString("next_cursor")));
        assertEquals(0, next.getInt("count"));
        assertEquals("ok", next.getString("status"));
        store.close();
        context.deleteDatabase("maichat-history.db");
        assertToolError("maichat_get_messages", new JSONObject().put("peer_id", "target"));
        assertFalse(context.getDatabasePath("maichat-history.db").exists());
    }

    @Test public void historyRowsHaveOnlyStoredMetadataAndDecimalStringTimestamps() throws Exception {
        long timestamp = Long.MAX_VALUE - 1;
        store.upsertMessage("history-owner", message("one", "target", "needle", timestamp));
        for (String tool : new String[]{"maichat_get_messages", "maichat_search_messages"}) {
            JSONObject arguments = new JSONObject().put("peer_id", "target");
            if (tool.equals("maichat_search_messages")) arguments.put("query", "needle");
            JSONObject output = execute(tool, arguments);
            JSONObject row = output.getJSONArray(tool.equals("maichat_get_messages") ? "messages" : "matches").getJSONObject(0);
            assertEquals(7, row.length());
            assertEquals("one", row.getString("id"));
            assertEquals("target", row.getString("peer_id"));
            assertEquals("target", row.getString("sender_id"));
            assertEquals("needle", row.getString("text"));
            assertEquals("text", row.getString("kind"));
            assertEquals("incoming", row.getString("direction"));
            assertTrue("Timestamp wire type must preserve 64-bit milliseconds", row.get("created_at_ms") instanceof String);
            assertEquals(Long.toString(timestamp), row.getString("created_at_ms"));
        }
    }

    @Test @Config(shadows = NoHistoryMediaSideEffects.class)
    public void twoHundredImageRowsNeverEnterAgentOrMediaMaterialization() throws Exception {
        for (int index = 0; index < 200; index++)
            store.upsertMessage("history-owner", message("image-" + index, "target", "image needle", index));
        // 仅插入持久化元数据，文件故意不存在；查询不可创建工作区、下载或尝试 native 解码。
        store.getWritableDatabase().execSQL("UPDATE messages SET image_path='Incoming/Images/unavailable.png',"
            + "image_width=640,image_height=480,image_size=4096 WHERE owner_id='history-owner'");
        java.io.File media = new java.io.File(context.getFilesDir(), "RemoteIMMedia/Incoming/Images/unavailable.png");
        assertFalse(media.exists());
        for (String tool : new String[]{"maichat_get_messages", "maichat_search_messages"}) {
            JSONObject arguments = new JSONObject().put("peer_id", "target").put("limit", 200).put("kind", "image");
            if (tool.equals("maichat_search_messages")) arguments.put("query", "needle");
            JSONObject output = execute(tool, arguments);
            org.json.JSONArray rows = output.getJSONArray(tool.equals("maichat_get_messages") ? "messages" : "matches");
            assertEquals(200, rows.length());
            for (int index = 0; index < rows.length(); index++) {
                JSONObject row = rows.getJSONObject(index);
                assertEquals(7, row.length());
                assertEquals("image", row.getString("kind"));
                assertFalse(row.has("workspace_path"));
                assertFalse(row.has("mime_type"));
            }
        }
        assertFalse(media.exists());
        assertTrue(session.chatState().messagesWith("target").isEmpty());
    }

    @Test @Config(shadows = NoHistoryMediaSideEffects.class)
    public void storedImageKindSurvivesLegacyAbsoluteAndEscapingPaths() throws Exception {
        store.upsertMessage("history-owner", message("absolute", "target", "image needle", 10));
        store.upsertMessage("history-owner", message("outside", "target", "image needle", 20));
        store.getWritableDatabase().execSQL("UPDATE messages SET image_path=? WHERE id='absolute'",
            new Object[]{context.getFilesDir().getAbsolutePath() + "/old-outside-media.png"});
        store.getWritableDatabase().execSQL("UPDATE messages SET image_path='../../outside-media.png' WHERE id='outside'");
        assertImageMetadataRows(2);
    }

    @Test @Config(shadows = NoHistoryMediaSideEffects.class)
    public void invalidUnselectedAttachmentAndStatusMetadataCannotBreakQueryRows() throws Exception {
        store.upsertMessage("history-owner", message("bad-metadata", "target", "image needle", 10));
        android.content.ContentValues damaged = new android.content.ContentValues();
        // 这些旧损坏字段不是查询记录的一部分，不应经过路径解析或完整 UI 消息恢复。
        damaged.put("image_path", "invalid\u0000image.png");
        damaged.put("image_width", "invalid-width");
        damaged.put("image_height", -100);
        damaged.put("status", "invalid-status");
        damaged.put("approval_request_token", "invalid-request");
        damaged.put("approval_request_actions", "invalid-actions");
        damaged.put("quote_digest", "unrelated stored quote");
        store.getWritableDatabase().update("messages", damaged, "id=?", new String[]{"bad-metadata"});
        assertImageMetadataRows(1);
    }

    private void assertImageMetadataRows(int count) throws Exception {
        for (String tool : new String[]{"maichat_get_messages", "maichat_search_messages"}) {
            JSONObject arguments = new JSONObject().put("peer_id", "target").put("kind", "image");
            if (tool.equals("maichat_search_messages")) arguments.put("query", "needle");
            JSONObject result = execute(tool, arguments);
            org.json.JSONArray rows = result.getJSONArray(tool.equals("maichat_get_messages") ? "messages" : "matches");
            assertEquals(count, rows.length());
            for (int index = 0; index < rows.length(); index++) {
                JSONObject row = rows.getJSONObject(index);
                assertEquals(7, row.length());
                assertEquals("image", row.getString("kind"));
                assertEquals("target", row.getString("peer_id"));
                assertEquals("target", row.getString("sender_id"));
                assertEquals("image needle", row.getString("text"));
                assertEquals("incoming", row.getString("direction"));
                assertTrue(row.get("created_at_ms") instanceof String);
            }
        }
    }

    /** 查询不能为了图片元数据进入 Agent 初始化、native/网络准备或工作区物化路径。 */
    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class NoHistoryMediaSideEffects {
        @Implementation public static AIAssistantController initializedAccountToolOwner() {
            throw new AssertionError("A DB row formatter must not resolve an Agent owner");
        }
        @Implementation public static AIAssistantController shared(Context context) {
            throw new AssertionError("A DB query must not initialize native or network capabilities");
        }
        @Implementation public java.io.File workspaceFile(String path) {
            throw new AssertionError("A DB query must not materialize media in a workspace");
        }
    }

    private void assertToolError(String tool, JSONObject arguments) throws Exception {
        try { execute(tool, arguments); fail("Expected a history query error"); }
        catch (java.util.concurrent.ExecutionException expected) {
            assertTrue(expected.getCause() instanceof IllegalArgumentException
                || expected.getCause() instanceof IllegalStateException);
        }
    }

    private JSONObject execute(String tool, JSONObject arguments) throws Exception {
        Future<Object> result = worker.submit(() -> tools.execute(tool, arguments));
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(5);
        while (!result.isDone() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(2);
        }
        return (JSONObject) result.get(1, java.util.concurrent.TimeUnit.SECONDS);
    }

    private Object field(String name) throws Exception {
        Field field = RemoteIMSessionController.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(session);
    }

    private void setField(String name, Object value) throws Exception {
        Field field = RemoteIMSessionController.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(session, value);
    }

    private static RemoteIMMessage message(String id, String peer, String body, long createdAt) throws Exception {
        java.lang.reflect.Constructor<RemoteIMMessage> constructor = RemoteIMMessage.class.getDeclaredConstructor(
            String.class, String.class, String.class, String.class, String.class,
            RemoteIMMessage.Direction.class, RemoteIMMessage.Status.class, long.class,
            com.kongshang.maichat.RemoteIMImageAttachment.class,
            com.kongshang.maichat.RemoteIMVoiceAttachment.class,
            com.kongshang.maichat.RemoteIMFileAttachment.class,
            com.kongshang.maichat.RemoteIMVideoAttachment.class, RemoteIMOrigin.class);
        constructor.setAccessible(true);
        return constructor.newInstance(id, "remote-" + id, peer, "history-owner", body,
            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, createdAt,
            null, null, null, null, RemoteIMOrigin.HUMAN);
    }
}
