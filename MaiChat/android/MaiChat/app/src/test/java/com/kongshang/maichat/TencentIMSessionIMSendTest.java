// 文件作用：用 SDK 传输替身验证 Machine dispatch 边界，不登录 IM、不调用 Native 或模型。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.os.Handler;
import androidx.test.core.app.ApplicationProvider;
import com.tencent.imsdk.v2.*;
import java.lang.reflect.Field;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class TencentIMSessionIMSendTest {
    private static final String OWNER = "owner", PEER = "peer";
    private static final class Profile implements TencentIMClient.SelfProfileSdk {
        @Override public String loginUser() { return OWNER; }
        @Override public int loginStatus() { return V2TIMManager.V2TIM_STATUS_LOGINED; }
        @Override public void get(List<String> users, V2TIMValueCallback<List<V2TIMUserFullInfo>> callback) { fail("No profile read"); }
        @Override public void set(V2TIMUserFullInfo profile, V2TIMCallback callback) { fail("No profile write"); }
    }
    private static final class Receipt extends V2TIMMessage {
        final String id; final long time;
        Receipt(String id, long time) { this.id = id; this.time = time; }
        @Override public String getMsgID() { return id; }
        @Override public long getTimestamp() { return time; }
    }
    private static final class Transport implements TencentIMClient.MachineSdk {
        final AtomicInteger creates = new AtomicInteger(), sends = new AtomicInteger();
        final CountDownLatch sent = new CountDownLatch(1);
        volatile V2TIMSendCallback<V2TIMMessage> callback;
        volatile String body, metadata;
        Runnable duringCreate = () -> {};
        @Override public V2TIMMessage create(RemoteIMMessage source, String exactBody) {
            creates.incrementAndGet(); body = exactBody; duringCreate.run(); return new Receipt("", 0);
        }
        @Override public void send(V2TIMMessage message, String peer, String metadata, V2TIMSendCallback<V2TIMMessage> callback) {
            assertEquals(PEER, peer); this.metadata = metadata; this.callback = callback; sends.incrementAndGet(); sent.countDown();
        }
    }
    private static class Listener implements TencentIMClient.Listener {
        int incoming, updates;
        @Override public void onConnectionStateChanged(TencentIMClient.ConnectionState state, String detail) { }
        @Override public void onIncomingMessage(RemoteIMMessage message) { incoming++; }
        @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
        @Override public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) { }
        @Override public void onVideoMediaUpdated(String sender, String recipient, String id, RemoteIMVideoAttachment video) { updates++; }
    }
    private static final class Completion implements TencentIMClient.SendCompletion {
        final CountDownLatch done = new CountDownLatch(1);
        final AtomicInteger callbacks = new AtomicInteger();
        String id; long millis; int error;
        @Override public void onSuccess(String id, long millis) { this.id = id; this.millis = millis; callbacks.incrementAndGet(); done.countDown(); }
        @Override public void onError(int code, String description) { error = code; callbacks.incrementAndGet(); done.countDown(); }
    }
    private static TencentIMClient client(Transport sdk, Listener listener) throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        TencentIMClient client = new TencentIMClient(context, listener,
            (url, target, maximum, scope, complete) -> { throw new AssertionError("No download"); }, new Profile(), sdk);
        set(client, "currentUserId", OWNER); set(client, "loginAcknowledged", true);
        return client;
    }
    private static void set(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); field.set(owner, value);
    }
    private static Object field(Object owner, String name) throws Exception {
        Field field = owner.getClass().getDeclaredField(name); field.setAccessible(true); return field.get(owner);
    }
    private static RemoteIMMessage source(RemoteIMOrigin origin) {
        return new RemoteIMMessage("machine-local", "", OWNER, PEER, "body", RemoteIMMessage.Direction.OUTGOING,
            RemoteIMMessage.Status.PENDING, 100, null, null, null, null, origin);
    }

    @Test public void acknowledgesActualDispatchBeforeNetworkReceiptAndKeepsExactBodyAndMachineProtocol() throws Exception {
        Transport sdk = new Transport(); TencentIMClient client = client(sdk, new Listener());
        Completion completion = new Completion(); CountDownLatch ack = new CountDownLatch(1);
        AtomicInteger pending = new AtomicInteger(), acks = new AtomicInteger();
        try {
            String body = "  exact assistant\nbody  ";
            client.sendSessionIM(OWNER, client.sessionIMLoginEpoch(), () -> true, () -> true,
                source(RemoteIMOrigin.MACHINE), body, pending::incrementAndGet,
                () -> { acks.incrementAndGet(); ack.countDown(); }, completion);
            assertTrue(sdk.sent.await(3, TimeUnit.SECONDS)); assertTrue(ack.await(3, TimeUnit.SECONDS));
            assertEquals(body, sdk.body); assertEquals(RemoteIMOrigin.MACHINE, RemoteIMProtocolMetadata.decode(sdk.metadata));
            assertTrue(sdk.metadata.contains("\"version\":2"));
            assertEquals(1, pending.get()); assertEquals(0, completion.callbacks.get());
            sdk.callback.onSuccess(new Receipt("real-sdk-id", 321));
            assertTrue(completion.done.await(3, TimeUnit.SECONDS)); assertEquals("real-sdk-id", completion.id);
            assertEquals(321_000, completion.millis); assertEquals(1, acks.get());
            sdk.callback.onError(99, "late duplicate"); sdk.callback.onSuccess(new Receipt("duplicate", 999));
            assertEquals(1, completion.callbacks.get()); assertEquals(1, sdk.sends.get());
        } finally { client.destroy(); }
    }

    @Test public void queuedOwnerChangeRejectsBeforeClaimCreateOrSend() throws Exception {
        queuedInvalidation(false);
    }
    @Test public void queuedSameOwnerLoginEpochChangeRejectsBeforeClaimCreateOrSend() throws Exception {
        queuedInvalidation(true);
    }
    private static void queuedInvalidation(boolean epoch) throws Exception {
        Transport sdk = new Transport(); TencentIMClient client = client(sdk, new Listener());
        CountDownLatch release = new CountDownLatch(1), occupied = new CountDownLatch(1), ack = new CountDownLatch(1);
        Completion completion = new Completion(); AtomicBoolean current = new AtomicBoolean(true); AtomicInteger claims = new AtomicInteger();
        try {
            ((Handler) field(client, "sdkHandler")).post(() -> { occupied.countDown(); try { release.await(); } catch (InterruptedException ignored) { } });
            assertTrue(occupied.await(3, TimeUnit.SECONDS));
            client.sendSessionIM(OWNER, client.sessionIMLoginEpoch(), current::get,
                () -> { claims.incrementAndGet(); return true; }, source(RemoteIMOrigin.MACHINE), "body", () -> fail("No pending row"), ack::countDown, completion);
            if (epoch) ((AtomicLong) field(client, "selfProfileEpoch")).incrementAndGet(); else current.set(false);
            release.countDown(); assertTrue(completion.done.await(3, TimeUnit.SECONDS)); assertTrue(ack.await(3, TimeUnit.SECONDS));
            assertEquals(0, sdk.creates.get()); assertEquals(0, sdk.sends.get()); assertEquals(0, claims.get()); assertNull(completion.id);
        } finally { release.countDown(); client.destroy(); }
    }

    @Test public void deniedNativeClaimNeverCreatesSdkMessageAndStillAcknowledgesAbandonment() throws Exception {
        Transport sdk = new Transport(); TencentIMClient client = client(sdk, new Listener());
        Completion completion = new Completion(); CountDownLatch ack = new CountDownLatch(1);
        try {
            client.sendSessionIM(OWNER, client.sessionIMLoginEpoch(), () -> true, () -> false,
                source(RemoteIMOrigin.MACHINE), "body", () -> fail("No pending row"), ack::countDown, completion);
            assertTrue(completion.done.await(3, TimeUnit.SECONDS)); assertTrue(ack.await(3, TimeUnit.SECONDS));
            assertEquals(0, sdk.creates.get()); assertEquals(0, sdk.sends.get());
        } finally { client.destroy(); }
    }

    @Test public void scopeRevokedDuringSdkCreationCannotSend() throws Exception {
        Transport sdk = new Transport(); TencentIMClient client = client(sdk, new Listener());
        AtomicBoolean current = new AtomicBoolean(true); sdk.duringCreate = () -> current.set(false);
        Completion completion = new Completion(); CountDownLatch ack = new CountDownLatch(1);
        try {
            client.sendSessionIM(OWNER, client.sessionIMLoginEpoch(), current::get, () -> true,
                source(RemoteIMOrigin.MACHINE), "body", () -> fail("No pending row"), ack::countDown, completion);
            assertTrue(completion.done.await(3, TimeUnit.SECONDS)); assertTrue(ack.await(3, TimeUnit.SECONDS));
            assertEquals(1, sdk.creates.get()); assertEquals(0, sdk.sends.get());
        } finally { client.destroy(); }
    }

    @Test public void unknownReceiptDoesNotInventTimestampIdOrRetry() throws Exception {
        for (Receipt bad : new Receipt[]{new Receipt("", 100), new Receipt("sdk-id", 0)}) {
            Transport sdk = new Transport(); TencentIMClient client = client(sdk, new Listener()); Completion completion = new Completion();
            try {
                client.sendSessionIM(OWNER, client.sessionIMLoginEpoch(), () -> true, () -> true,
                    source(RemoteIMOrigin.MACHINE), "body", () -> {}, () -> {}, completion);
                assertTrue(sdk.sent.await(3, TimeUnit.SECONDS)); sdk.callback.onSuccess(bad);
                assertTrue(completion.done.await(3, TimeUnit.SECONDS)); assertEquals(-2, completion.error);
                assertNull(completion.id); assertEquals(0, completion.millis); assertEquals(1, sdk.sends.get());
            } finally { client.destroy(); }
        }
    }

    @Test public void videoCoverDoesNotBecomeModelReadyBeforeVideoBytesFinish() throws Exception {
        Transport sdk = new Transport(); Listener listener = new Listener(); TencentIMClient client = client(sdk, listener);
        AtomicInteger ready = new AtomicInteger();
        TencentIMClient.LiveAdmission ticket = new TencentIMClient.LiveAdmission() {
            @Override public void persisted(boolean inserted) { }
            @Override public void mediaReady(RemoteIMMessage message) { assertEquals("sdk-video", message.remoteId()); ready.incrementAndGet(); }
        };
        try {
            java.lang.reflect.Method emit = TencentIMClient.class.getDeclaredMethod("emitVideoMessage", String.class,
                String.class, String.class, long.class, RemoteIMOrigin.class, String.class, String.class,
                int.class, int.class, int.class, long.class, String.class, boolean.class, RemoteIMQuote.class,
                boolean.class, TencentIMClient.LiveAdmission.class, boolean.class);
            emit.setAccessible(true);
            emit.invoke(client, "sdk-video", PEER, OWNER, 100L, RemoteIMOrigin.HUMAN, "/fixture/video.mp4", "/fixture/cover.jpg",
                2, 160, 90, 100L, "caption", false, null, true, ticket, false);
            assertEquals(1, listener.incoming); assertEquals(0, ready.get());
            emit.invoke(client, "sdk-video", PEER, OWNER, 100L, RemoteIMOrigin.HUMAN, "/fixture/video.mp4", "/fixture/cover.jpg",
                2, 160, 90, 100L, "caption", false, null, false, ticket, true);
            assertEquals(1, listener.incoming); assertEquals(1, listener.updates); assertEquals(1, ready.get());
        } finally { client.destroy(); }
    }
}
