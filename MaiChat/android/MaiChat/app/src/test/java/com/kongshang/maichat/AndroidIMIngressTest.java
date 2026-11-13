// 文件作用：拥有的 SQLite/SDK 替身证明 raw 元数据先落库、fence 不等下载、ready 只丰富同一记录。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.database.Cursor;
import android.os.Handler;
import androidx.test.core.app.ApplicationProvider;
import com.tencent.imsdk.v2.*;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AndroidIMIngressTest {
    @Test public void metadataFirstInsertAndReadyEnrichmentKeepOneStableRowAndActualRemoteId() throws Exception {
        try (Fixture fixture = new Fixture()) {
            RemoteIMMessage initial = initial(fixture.context, "local-row", "actual-sdk-id");
            RemoteIMIngress.Metadata metadata = new RemoteIMIngress.Metadata(new RemoteIMIngress.Scope(123, "owner", 1), 1, initial);
            RemoteIMIngress.Stored first = fixture.writer.initial(metadata, () -> true).get(2, TimeUnit.SECONDS);
            assertTrue(first.inserted); assertEquals("local-row", first.sourceRowId); assertEquals("actual-sdk-id", first.remoteId);
            assertFalse(new File(initial.fileAttachment().localPath()).exists());
            RemoteIMMessage ready = new RemoteIMMessage("temporary-ui-id", "actual-sdk-id", "peer", "owner", "ready caption",
                RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, 999, null, null,
                initial.fileAttachment(), null, RemoteIMOrigin.HUMAN);
            RemoteIMMessage enriched = fixture.writer.enrich(metadata, first, ready, () -> true).get(2, TimeUnit.SECONDS);
            assertEquals("local-row", enriched.id()); assertEquals(100, enriched.createdAtMillis());
            RemoteIMIngress.Stored duplicate = fixture.writer.initial(new RemoteIMIngress.Metadata(metadata.scope, 2,
                initial(fixture.context, "new-sdk-callback-row", "actual-sdk-id")), () -> true).get(2, TimeUnit.SECONDS);
            assertFalse(duplicate.inserted); assertEquals("local-row", duplicate.sourceRowId);
            try (Cursor rows = fixture.store.getReadableDatabase().rawQuery("SELECT id,remote_id,body,file_path FROM messages WHERE owner_id='owner' AND peer_id='peer'", null)) {
                assertEquals(1, rows.getCount()); assertTrue(rows.moveToFirst());
                assertEquals("local-row", rows.getString(0)); assertEquals("actual-sdk-id", rows.getString(1));
                assertEquals("ready caption", rows.getString(2)); assertTrue(rows.getString(3).startsWith("Incoming/Files/"));
            }
        }
    }

    @Test public void fenceIncludesRawRegistrationBeforeItsMetadataWorkWasEvenQueued() throws Exception {
        RemoteIMIngress.Tracker tracker = new RemoteIMIngress.Tracker();
        RemoteIMIngress.Scope scope = new RemoteIMIngress.Scope(123, "owner", 1);
        AtomicBoolean current = new AtomicBoolean(true);
        RemoteIMIngress.Registration raw = tracker.register(scope);
        RemoteIMIngress.Fence fence = tracker.fence(scope, current::get);
        assertEquals(1, fence.sequence); assertFalse(fence.await().isDone());
        raw.persisted.complete(new RemoteIMIngress.Stored("real-row", "sdk-id", 100, true));
        fence.await().get(2, TimeUnit.SECONDS);
        current.set(false); assertFalse(fence.isCurrent());
        canceled(fence.await());
    }

    @Test public void metadataFailureAndSdkGenerationChangeAreExplicitFenceFailures() throws Exception {
        RemoteIMIngress.Tracker tracker = new RemoteIMIngress.Tracker();
        RemoteIMIngress.Scope scope = new RemoteIMIngress.Scope(123, "owner", 1);
        RemoteIMIngress.Registration failed = tracker.register(scope);
        RemoteIMIngress.Fence fence = tracker.fence(scope, () -> true);
        failed.persisted.completeExceptionally(new java.io.IOException("owned metadata failure"));
        assertThrows(ExecutionException.class, () -> fence.await().get(2, TimeUnit.SECONDS));
        RemoteIMIngress.Tracker other = new RemoteIMIngress.Tracker(); other.register(scope);
        RemoteIMIngress.Fence stale = other.fence(scope, () -> true); other.invalidate();
        canceled(stale.await());
    }

    @Test public void completedDatabaseFailureBeforeFenceCaptureCannotBeBaselinedAway() throws Exception {
        try (Fixture fixture = new Fixture()) {
            fixture.store.getWritableDatabase().execSQL("DROP TABLE messages");
            RemoteIMIngress.Scope scope = new RemoteIMIngress.Scope(123, "owner", 1);
            RemoteIMIngress.Tracker tracker = new RemoteIMIngress.Tracker();
            RemoteIMIngress.Registration raw = tracker.register(scope);
            CompletableFuture<RemoteIMIngress.Stored> write = fixture.writer.initial(
                new RemoteIMIngress.Metadata(scope, raw.sequence, initial(fixture.context, "failed-local", "failed-sdk")), () -> true);
            ExecutionException failed = assertThrows(ExecutionException.class, () -> write.get(2, TimeUnit.SECONDS));
            raw.persisted.completeExceptionally(failed.getCause());
            assertTrue(raw.persisted.isDone());
            // 捕获发生在失败已经完成之后，不能通过读取“当前失败计数”把这笔债变成成功。
            RemoteIMIngress.Fence later = tracker.fence(scope, () -> true).afterWriter(() -> fixture.writer.fence(() -> true));
            assertThrows(ExecutionException.class, () -> later.await().get(2, TimeUnit.SECONDS));
        }
    }

    @Test public void completedBenignDuplicateBeforeFenceCaptureIsPersistenceSuccess() throws Exception {
        try (Fixture fixture = new Fixture()) {
            RemoteIMIngress.Scope scope = new RemoteIMIngress.Scope(123, "owner", 1);
            RemoteIMIngress.Tracker tracker = new RemoteIMIngress.Tracker();
            RemoteIMIngress.Registration first = tracker.register(scope), duplicate = tracker.register(scope);
            first.persisted.complete(fixture.writer.initial(new RemoteIMIngress.Metadata(scope, first.sequence,
                initial(fixture.context, "first-local", "same-sdk")), () -> true).get(2, TimeUnit.SECONDS));
            RemoteIMIngress.Stored existing = fixture.writer.initial(new RemoteIMIngress.Metadata(scope, duplicate.sequence,
                initial(fixture.context, "duplicate-local", "same-sdk")), () -> true).get(2, TimeUnit.SECONDS);
            assertFalse(existing.inserted); duplicate.persisted.complete(existing);
            tracker.fence(scope, () -> true).afterWriter(() -> fixture.writer.fence(() -> true)).await().get(2, TimeUnit.SECONDS);
            assertEquals(1, fixture.count());
        }
    }

    @Test public void rawBeforeBindMetadataFenceCompletesWhileSdkHandlerAndDownloadAreHeld() throws Exception {
        try (Fixture fixture = new Fixture()) {
            AtomicBoolean published = new AtomicBoolean(false);
            CountDownLatch sdkRelease = new CountDownLatch(1), sdkHeld = new CountDownLatch(1), parsed = new CountDownLatch(1), parseRelease = new CountDownLatch(1);
            RawListener listener = new RawListener(fixture, published);
            TencentIMClient client = client(fixture.context, listener);
            listener.client = client;
            ExecutorService rawCallback = Executors.newSingleThreadExecutor();
            FileElement file = new FileElement();
            try {
                ((Handler) field(client, "sdkHandler")).post(() -> { sdkHeld.countDown(); await(sdkRelease); });
                assertTrue(sdkHeld.await(2, TimeUnit.SECONDS));
                V2TIMAdvancedMsgListener ingress = (V2TIMAdvancedMsgListener) field(client, "messageListener");
                Future<?> arrival = rawCallback.submit(() -> ingress.onRecvNewMessage(new Message(file, parsed, parseRelease)));
                assertTrue(parsed.await(2, TimeUnit.SECONDS));
                client.publishIngressState(() -> published.set(true));
                RemoteIMIngress.Fence fence = client.captureIngressFence(new RemoteIMIngress.Scope(123, "owner", client.sessionIMLoginEpoch()))
                    .afterWriter(() -> fixture.writer.fence(() -> true));
                assertEquals(1, fence.sequence); assertFalse(fence.await().isDone());
                parseRelease.countDown(); arrival.get(2, TimeUnit.SECONDS); fence.await().get(2, TimeUnit.SECONDS);
                assertEquals(0, file.lookups.get()); assertEquals(0, listener.live.get());
                assertEquals(1, fixture.count());
                String initialId = fixture.id();
                assertNotNull(listener.metadata.get()); assertFalse(new File(listener.metadata.get().message().fileAttachment().localPath()).exists());
                sdkRelease.countDown(); assertTrue(file.requested.await(2, TimeUnit.SECONDS));
                file.url.onSuccess("https://fixture.invalid/owned-file");
                assertTrue(listener.enriched.await(2, TimeUnit.SECONDS));
                assertEquals(1, fixture.count()); assertEquals(initialId, fixture.id());
            } finally {
                sdkRelease.countDown(); parseRelease.countDown(); rawCallback.shutdownNow();
                set(client, "initializedSdkAppId", null); client.destroy();
            }
        }
    }

    private static RemoteIMMessage initial(Context context, String id, String remote) {
        String path = new File(RemoteIMMediaPaths.forApp(context).root(), "Incoming/Files/pending.txt").getAbsolutePath();
        return new RemoteIMMessage(id, remote, "peer", "owner", "initial caption", RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED, 100, null, null, new RemoteIMFileAttachment(path, "pending.txt", "text/plain", 4), null, RemoteIMOrigin.HUMAN);
    }
    private static final class Fixture implements AutoCloseable {
        final Context context = ApplicationProvider.getApplicationContext();
        final String name = "owned-ingress-" + UUID.randomUUID() + ".db";
        final AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, name);
        final ExecutorService io = Executors.newSingleThreadExecutor();
        final AndroidIMIngressWriter writer = new AndroidIMIngressWriter(store, io);
        int count() { try (Cursor row = store.getReadableDatabase().rawQuery("SELECT COUNT(*) FROM messages", null)) { row.moveToFirst(); return row.getInt(0); } }
        String id() { try (Cursor row = store.getReadableDatabase().rawQuery("SELECT id FROM messages LIMIT 1", null)) { row.moveToFirst(); return row.getString(0); } }
        @Override public void close() throws Exception { io.shutdown(); assertTrue(io.awaitTermination(3, TimeUnit.SECONDS)); store.close(); context.deleteDatabase(name); }
    }
    private static final class RawListener implements TencentIMClient.Listener {
        final Fixture fixture; final AtomicBoolean published;
        final AtomicInteger live = new AtomicInteger();
        final AtomicReference<RemoteIMIngress.Metadata> metadata = new AtomicReference<>();
        final CountDownLatch enriched = new CountDownLatch(1);
        TencentIMClient client;
        RawListener(Fixture fixture, AtomicBoolean published) { this.fixture = fixture; this.published = published; }
        @Override public TencentIMClient.LiveAuthority captureLiveAuthority() {
            boolean atArrival = published.get();
            return (sender, recipient, id, origin, control, content) -> {
                if (!atArrival) return null; live.incrementAndGet();
                return new TencentIMClient.LiveAdmission() {
                    @Override public void persisted(boolean inserted) { fail("UI cache cannot grant live eligibility"); }
                    @Override public void metadataPersisted(RemoteIMIngress.Stored row) { assertTrue(row.inserted); }
                    @Override public void mediaReady(RemoteIMMessage message) { }
                    @Override public void abandoned() { }
                };
            };
        }
        @Override public CompletableFuture<RemoteIMIngress.Stored> persistRawMetadata(RemoteIMIngress.Metadata value) {
            metadata.set(value); return fixture.writer.initial(value, () -> client.isIngressScopeCurrent(value.scope));
        }
        @Override public void onIncomingMessage(RemoteIMMessage message, TencentIMClient.LiveAdmission admission) {
            TencentIMClient.IncomingTicket ticket = (TencentIMClient.IncomingTicket) admission;
            ticket.stored.thenCompose(row -> fixture.writer.enrich(ticket.metadata, row, message, row::isCurrent))
                .whenComplete((ready, failed) -> { assertNull(failed); enriched.countDown(); });
        }
        @Override public void onIncomingMessage(RemoteIMMessage message) { fail("Tracked ingress required"); }
        @Override public void onConnectionStateChanged(TencentIMClient.ConnectionState state, String detail) { }
        @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
        @Override public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) { }
    }
    private static TencentIMClient client(Context context, RawListener listener) throws Exception {
        TencentIMClient client = new TencentIMClient(context, listener, (url, target, max, scope, completion) -> {
            try { Files.write(target.toPath(), new byte[]{1, 2, 3, 4}); }
            catch (Exception failed) { throw new AssertionError(failed); }
            completion.finished(new MaiChatAttachmentDownload.Result(MaiChatAttachmentDownload.OK, 200, 0, 4));
        }, new TencentIMClient.SelfProfileSdk() {
            @Override public String loginUser() { return "owner"; }
            @Override public int loginStatus() { return V2TIMManager.V2TIM_STATUS_LOGINED; }
            @Override public void get(List<String> users, V2TIMValueCallback<List<V2TIMUserFullInfo>> callback) { fail("No network"); }
            @Override public void set(V2TIMUserFullInfo profile, V2TIMCallback callback) { fail("No network"); }
        });
        set(client, "currentUserId", "owner"); set(client, "loginAcknowledged", true); set(client, "initializedSdkAppId", 123);
        return client;
    }
    private static final class FileElement extends V2TIMFileElem {
        final AtomicInteger lookups = new AtomicInteger(); final CountDownLatch requested = new CountDownLatch(1);
        volatile V2TIMValueCallback<String> url;
        @Override public String getUUID() { return "owned-file-id"; }
        @Override public String getFileName() { return "owned.txt"; }
        @Override public int getFileSize() { return 4; }
        @Override public V2TIMElem getNextElem() { return null; }
        @Override public void getUrl(V2TIMValueCallback<String> callback) { lookups.incrementAndGet(); url = callback; requested.countDown(); }
    }
    private static final class Message extends V2TIMMessage {
        final FileElement file; final CountDownLatch parsed, release;
        Message(FileElement file, CountDownLatch parsed, CountDownLatch release) { this.file = file; this.parsed = parsed; this.release = release; }
        @Override public int getElemType() { return V2TIM_ELEM_TYPE_FILE; }
        @Override public V2TIMFileElem getFileElem() { return file; }
        @Override public String getSender() { parsed.countDown(); await(release); return "peer"; }
        @Override public String getUserID() { return "peer"; }
        @Override public String getGroupID() { return ""; }
        @Override public boolean isSelf() { return false; }
        @Override public String getMsgID() { return "raw-before-bind"; }
        @Override public long getTimestamp() { return 10; }
        @Override public String getCloudCustomData() { return RemoteIMProtocolMetadata.encode(RemoteIMOrigin.HUMAN); }
    }
    private static void await(CountDownLatch latch) { try { if (!latch.await(3, TimeUnit.SECONDS)) throw new AssertionError("owned fixture gate timed out"); } catch (InterruptedException failed) { throw new AssertionError(failed); } }
    private static void canceled(CompletableFuture<?> value) {
        RuntimeException failure = assertThrows(RuntimeException.class, value::join);
        Throwable cause = failure;
        while (cause instanceof CompletionException && cause.getCause() != null) cause = cause.getCause();
        assertTrue(cause instanceof CancellationException);
    }
    private static Object field(Object target, String name) throws Exception { Field value = target.getClass().getDeclaredField(name); value.setAccessible(true); return value.get(target); }
    private static void set(Object target, String name, Object value) throws Exception { Field field = target.getClass().getDeclaredField(name); field.setAccessible(true); field.set(target, value); }
}
