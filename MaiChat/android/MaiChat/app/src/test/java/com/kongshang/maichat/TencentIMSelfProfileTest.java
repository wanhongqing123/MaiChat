// 文件作用：在真实宿主排队和代际检查上替换 SDK 边界，验证头像确认与切号/停止竞争。
package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import androidx.test.core.app.ApplicationProvider;
import com.tencent.imsdk.v2.V2TIMCallback;
import com.tencent.imsdk.v2.V2TIMManager;
import com.tencent.imsdk.v2.V2TIMUserFullInfo;
import com.tencent.imsdk.v2.V2TIMValueCallback;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class TencentIMSelfProfileTest {
    private static final String OWNER = "im-owner";
    private static final String URL = "https://example.test/avatar.jpg";

    private static final class Sdk implements TencentIMClient.SelfProfileSdk {
        String user = OWNER;
        final AtomicInteger writes = new AtomicInteger();
        final CountDownLatch sent = new CountDownLatch(1), queried = new CountDownLatch(1);
        volatile V2TIMCallback write;
        volatile V2TIMValueCallback<List<V2TIMUserFullInfo>> query;
        volatile String faceUrl;
        @Override public String loginUser() { return user; }
        @Override public int loginStatus() { return V2TIMManager.V2TIM_STATUS_LOGINED; }
        @Override public void get(List<String> users,
                V2TIMValueCallback<List<V2TIMUserFullInfo>> callback) {
            query = callback; queried.countDown();
        }
        @Override public void set(V2TIMUserFullInfo profile, V2TIMCallback callback) {
            writes.incrementAndGet(); faceUrl = profile.getFaceUrl();
            write = callback; sent.countDown();
        }
    }

    private static class Listener implements TencentIMClient.Listener {
        List<RemoteIMContact> contacts;
        TencentIMClient.SelfProfile own;
        final CountDownLatch refreshed = new CountDownLatch(1);
        @Override public void onConnectionStateChanged(TencentIMClient.ConnectionState state,
                                                       String detail) { }
        @Override public void onIncomingMessage(RemoteIMMessage message) { }
        @Override public void onProfilesUpdated(List<RemoteIMContact> contacts) {
            this.contacts = contacts; refreshed.countDown();
        }
        @Override public void onSelfProfileUpdated(TencentIMClient.SelfProfile profile) { own = profile; }
        @Override public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) { }
    }

    private static final class Completion implements TencentIMClient.OperationCompletion {
        final CountDownLatch done = new CountDownLatch(1);
        boolean success;
        @Override public void onSuccess() { success = true; done.countDown(); }
        @Override public void onError(int code, String message) { done.countDown(); }
    }

    private static Object field(TencentIMClient client, String name) throws Exception {
        Field field = TencentIMClient.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(client);
    }

    private static TencentIMClient client(Sdk sdk, Listener listener) throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        TencentIMClient client = new TencentIMClient(context, listener,
            (url, target, maximum, scope, complete) -> { throw new AssertionError("No media download"); },
            sdk);
        Field owner = TencentIMClient.class.getDeclaredField("currentUserId");
        owner.setAccessible(true); owner.set(client, OWNER);
        Field logged = TencentIMClient.class.getDeclaredField("loginAcknowledged");
        logged.setAccessible(true); logged.setBoolean(client, true);
        return client;
    }

    private static V2TIMUserFullInfo profile(String user) {
        return new V2TIMUserFullInfo() {
            @Override public String getUserID() { return user; }
            @Override public String getNickName() { return "nickname"; }
            @Override public String getFaceUrl() { return URL; }
        };
    }

    @Test public void rechecksCancellationAfterWaitingInSdkQueue() throws Exception {
        Sdk sdk = new Sdk();
        TencentIMClient client = client(sdk, new Listener());
        CountDownLatch blocked = new CountDownLatch(1), release = new CountDownLatch(1);
        try {
            ((Handler) field(client, "sdkHandler")).post(() -> {
                blocked.countDown();
                try { release.await(3, TimeUnit.SECONDS); }
                catch (InterruptedException failure) { Thread.currentThread().interrupt(); }
            });
            assertTrue(blocked.await(3, TimeUnit.SECONDS));
            AtomicBoolean current = new AtomicBoolean(true);
            Completion completion = new Completion();
            client.setSelfAvatar(OWNER, URL, current::get, completion);
            current.set(false); release.countDown();
            assertTrue(completion.done.await(3, TimeUnit.SECONDS));
            assertFalse(completion.success);
            assertEquals(0, sdk.writes.get());
        } finally { release.countDown(); client.destroy(); }
    }

    @Test public void rejectsDifferentActualSdkIdentityBeforeSending() throws Exception {
        Sdk sdk = new Sdk(); sdk.user = "different-im-owner";
        TencentIMClient client = client(sdk, new Listener());
        try {
            Completion completion = new Completion();
            client.setSelfAvatar(OWNER, URL, () -> true, completion);
            assertTrue(completion.done.await(3, TimeUnit.SECONDS));
            assertFalse(completion.success);
            assertEquals(0, sdk.writes.get());
        } finally { client.destroy(); }
    }

    @Test public void waitsForSdkAcknowledgmentAndPreservesConfirmedWriteAfterStop() throws Exception {
        Sdk sdk = new Sdk();
        TencentIMClient client = client(sdk, new Listener());
        try {
            AtomicBoolean current = new AtomicBoolean(true);
            Completion completion = new Completion();
            client.setSelfAvatar(OWNER, URL, current::get, () -> true, () -> { }, completion);
            assertTrue(sdk.sent.await(3, TimeUnit.SECONDS));
            assertEquals(1, completion.done.getCount());
            assertEquals(URL, sdk.faceUrl);
            current.set(false);
            sdk.write.onSuccess();
            assertTrue(completion.done.await(3, TimeUnit.SECONDS));
            assertTrue(completion.success);
        } finally { client.destroy(); }
    }

    @Test public void sameAccountNewLoginEpochRejectsOldSuccess() throws Exception {
        Sdk sdk = new Sdk();
        TencentIMClient client = client(sdk, new Listener());
        try {
            Completion completion = new Completion();
            client.setSelfAvatar(OWNER, URL, () -> true, completion);
            assertTrue(sdk.sent.await(3, TimeUnit.SECONDS));
            ((AtomicLong) field(client, "selfProfileEpoch")).incrementAndGet();
            sdk.write.onSuccess();
            assertTrue(completion.done.await(3, TimeUnit.SECONDS));
            assertFalse(completion.success);
        } finally { client.destroy(); }
    }

    @Test public void selfQueryNeverPublishesOwnProfileAsFriend() throws Exception {
        Sdk sdk = new Sdk(); Listener listener = new Listener();
        TencentIMClient client = client(sdk, listener);
        try {
            CountDownLatch done = new CountDownLatch(1);
            AtomicReference<TencentIMClient.SelfProfile> result = new AtomicReference<>();
            client.getSelfProfile(OWNER, () -> true, new TencentIMClient.SelfProfileCompletion() {
                @Override public void onSuccess(TencentIMClient.SelfProfile profile) {
                    result.set(profile); done.countDown();
                }
                @Override public void onError(int code, String message) { done.countDown(); }
            });
            assertTrue(sdk.queried.await(3, TimeUnit.SECONDS));
            sdk.query.onSuccess(Arrays.asList(profile("other-user"), profile(OWNER)));
            assertTrue(done.await(3, TimeUnit.SECONDS));
            assertNotNull(result.get());
            assertEquals(OWNER, result.get().imUserId);
            assertEquals(URL, result.get().avatarUrl);
            assertNull(listener.contacts);
        } finally { client.destroy(); }
    }

    @Test public void mixedProfileRefreshRoutesSelfSeparatelyFromFriendList() throws Exception {
        Sdk sdk = new Sdk(); Listener listener = new Listener();
        TencentIMClient client = client(sdk, listener);
        try {
            client.refreshProfiles(Arrays.asList(OWNER, "other-user"));
            assertTrue(sdk.queried.await(3, TimeUnit.SECONDS));
            sdk.query.onSuccess(Arrays.asList(profile(OWNER), profile("other-user")));
            assertTrue(listener.refreshed.await(3, TimeUnit.SECONDS));
            assertEquals(1, listener.contacts.size());
            assertEquals("other-user", listener.contacts.get(0).userId());
            assertEquals(OWNER, listener.own.imUserId);
        } finally { client.destroy(); }
    }
}
