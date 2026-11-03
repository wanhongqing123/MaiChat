// 文件作用：通过普通 Session 头像入口和可控上传/SDK 边界验证并行拒绝、取消及迟到回执。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import com.tencent.imsdk.v2.V2TIMCallback;
import com.tencent.imsdk.v2.V2TIMManager;
import com.tencent.imsdk.v2.V2TIMUserFullInfo;
import com.tencent.imsdk.v2.V2TIMValueCallback;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class AccountAvatarConcurrencyTest {
    private static final String OWNER = "im_avatar_owner";

    private static final class Result {
        final CountDownLatch done = new CountDownLatch(1);
        String url, error;
        void complete(String url, String error) {
            this.url = url; this.error = error; done.countDown();
        }
    }

    private static final class Write {
        final String url;
        final V2TIMCallback completion;
        Write(String url, V2TIMCallback completion) { this.url = url; this.completion = completion; }
    }

    private static final class Sdk implements TencentIMClient.SelfProfileSdk {
        final BlockingQueue<Write> writes = new LinkedBlockingQueue<>();
        @Override public String loginUser() { return OWNER; }
        @Override public int loginStatus() { return V2TIMManager.V2TIM_STATUS_LOGINED; }
        @Override public void get(List<String> users,
                V2TIMValueCallback<List<V2TIMUserFullInfo>> callback) {
            throw new AssertionError("Avatar mutation must not query profiles");
        }
        @Override public void set(V2TIMUserFullInfo profile, V2TIMCallback callback) {
            writes.add(new Write(profile.getFaceUrl(), callback));
        }
    }

    private static void setField(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true); field.set(owner, value);
    }

    private static final class Fixture implements AutoCloseable {
        final RemoteIMSessionController session;
        final TencentIMClient client;
        final SharedPreferences preferences;
        final Handler main = new Handler(Looper.getMainLooper());
        final Sdk sdk = new Sdk();
        final AtomicInteger uploads = new AtomicInteger();
        final List<RemoteIMSessionController.AccountTask> tasks = new ArrayList<>();
        final File source;

        Fixture() throws Exception {
            Context context = ApplicationProvider.getApplicationContext();
            preferences = context.getSharedPreferences("maichat-phone-auth", Context.MODE_PRIVATE);
            preferences.edit().clear().commit();
            MaiChatAuthClient auth = new MaiChatAuthClient(context);
            auth.cacheAccountProfile(new MaiChatAuthClient.AccountProfile(new org.json.JSONObject()
                .put("id", "avatar_account").put("im_user_id", OWNER)), false);
            Path root = Files.createTempDirectory("maichat-avatar-concurrency");
            source = Files.write(root.resolve("opaque-upload-fixture.png"), new byte[]{1}).toFile();
            session = new RemoteIMSessionController(
                new LocalSettingsStore(root.resolve("settings.properties").toFile()),
                new LocalChatHistoryStore(root.resolve("history").toFile()));
            session.login(OWNER);
            client = new TencentIMClient(context, new TencentIMClient.Listener() {
                @Override public void onConnectionStateChanged(TencentIMClient.ConnectionState state,
                                                                String detail) { }
                @Override public void onIncomingMessage(RemoteIMMessage message) { }
                @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) { }
                @Override public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) { }
            }, (url, target, maximum, scope, complete) -> { throw new AssertionError("No media download"); }, sdk);
            setField(client, "currentUserId", OWNER);
            setField(client, "loginAcknowledged", true);
            setField(session, "productionMode", true);
            setField(session, "authClient", auth);
            setField(session, "client", client);
            setField(session, "mainHandler", main);
            upload((file, account, generation, current, scope) -> {
                assertTrue(current.getAsBoolean());
                return "https://example.test/avatar-" + uploads.incrementAndGet() + ".jpg";
            });
        }

        void upload(RemoteIMSessionController.AvatarUploadOperation operation) throws Exception {
            setField(session, "avatarUploadOperation", operation);
        }

        RemoteIMSessionController.AccountTask start(Result result) {
            RemoteIMSessionController.AccountTask task = session.setSelfAvatar(source, result::complete);
            tasks.add(task);
            shadowOf(Looper.getMainLooper()).idle();
            return task;
        }

        Write write() throws Exception {
            Write write = sdk.writes.poll(3, TimeUnit.SECONDS);
            assertNotNull("SDK write was never dispatched", write);
            shadowOf(Looper.getMainLooper()).idle();
            return write;
        }

        void completed(Result result) throws Exception {
            shadowOf(Looper.getMainLooper()).idle();
            assertTrue("Avatar request did not complete", result.done.await(1, TimeUnit.SECONDS));
        }

        void busy(Result result) throws Exception {
            completed(result);
            assertNull(result.url);
            assertTrue(result.error, result.error != null && result.error.contains("已有头像修改"));
        }

        @Override public void close() throws Exception {
            for (RemoteIMSessionController.AccountTask task : tasks) task.cancel();
            Field field = RemoteIMSessionController.class.getDeclaredField("ioExecutor");
            field.setAccessible(true);
            ExecutorService executor = (ExecutorService) field.get(session);
            executor.shutdownNow(); executor.awaitTermination(3, TimeUnit.SECONDS);
            shadowOf(Looper.getMainLooper()).idle();
            main.removeCallbacksAndMessages(null);
            client.destroy();
            preferences.edit().clear().commit();
        }
    }

    @Test public void uploadingRequestRejectsParallelCallAndPreSdkCancelReleasesIt() throws Exception {
        try (Fixture fixture = new Fixture()) {
            CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
            fixture.upload((file, account, generation, current, scope) -> {
                int number = fixture.uploads.incrementAndGet();
                if (number == 1) {
                    entered.countDown();
                    assertTrue(release.await(3, TimeUnit.SECONDS));
                }
                return "https://example.test/avatar-" + number + ".jpg";
            });
            Result first = new Result();
            RemoteIMSessionController.AccountTask task = fixture.start(first);
            try {
                assertTrue(entered.await(3, TimeUnit.SECONDS));
                Result busy = new Result(); fixture.start(busy); fixture.busy(busy);
                task.cancel(); fixture.completed(first);
                assertNotNull(first.error);
                Result next = new Result(); fixture.start(next);
                release.countDown();
                Write write = fixture.write();
                assertEquals("https://example.test/avatar-2.jpg", write.url);
                assertEquals(2, fixture.uploads.get());
                write.completion.onSuccess(); fixture.completed(next);
                assertNull(next.error);
            } finally { release.countDown(); }
        }
    }

    @Test public void cancelAfterSdkSendKeepsBusyUntilItsConfirmedResult() throws Exception {
        try (Fixture fixture = new Fixture()) {
            Result first = new Result();
            RemoteIMSessionController.AccountTask task = fixture.start(first);
            Write write = fixture.write();
            task.cancel();
            assertEquals(1L, first.done.getCount());
            Result busy = new Result(); fixture.start(busy); fixture.busy(busy);
            assertEquals(1, fixture.uploads.get());
            write.completion.onSuccess(); fixture.completed(first);
            assertEquals(write.url, first.url);
            assertNull(first.error);
            Result next = new Result(); fixture.start(next);
            Write nextWrite = fixture.write();
            nextWrite.completion.onSuccess(); fixture.completed(next);
            assertEquals(nextWrite.url, next.url);
        }
    }

    @Test public void sdkRejectionReleasesBusyForTheNextRequest() throws Exception {
        try (Fixture fixture = new Fixture()) {
            Result first = new Result(); fixture.start(first);
            fixture.write().completion.onError(500, "synthetic failure"); fixture.completed(first);
            assertNotNull(first.error);
            Result next = new Result(); fixture.start(next);
            fixture.write().completion.onSuccess(); fixture.completed(next);
            assertNull(next.error);
        }
    }

    @Test public void timeoutReleasesBusyAndLateOldReplyCannotReleaseTheNewRequest() throws Exception {
        try (Fixture fixture = new Fixture()) {
            Result first = new Result(); fixture.start(first);
            Write oldWrite = fixture.write();
            shadowOf(Looper.getMainLooper()).idleFor(
                Duration.ofMillis(RemoteIMSessionController.AVATAR_SDK_TIMEOUT_MS + 1));
            fixture.completed(first);
            assertNotNull(first.error);
            assertTrue(first.error.contains("超时"));
            Result next = new Result(); fixture.start(next);
            Write currentWrite = fixture.write();
            oldWrite.completion.onSuccess();
            shadowOf(Looper.getMainLooper()).idle();
            Result busy = new Result(); fixture.start(busy); fixture.busy(busy);
            assertEquals(2, fixture.uploads.get());
            assertEquals(1L, next.done.getCount());
            currentWrite.completion.onSuccess(); fixture.completed(next);
            assertNull(next.error);
        }
    }
}
