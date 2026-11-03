// 文件作用：验证离线资料读取不触发鉴权/IM 请求，以及不同鉴权响应之间的遮罩缓存归并。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class AccountProfileCacheTest {
    private SharedPreferences preferences;
    private MaiChatAuthClient auth;

    @Before public void prepareOfflineAccount() {
        Context context = ApplicationProvider.getApplicationContext();
        preferences = context.getSharedPreferences("maichat-phone-auth", Context.MODE_PRIVATE);
        preferences.edit().clear().commit();
        auth = new MaiChatAuthClient(context);
    }

    @After public void clearFixture() { preferences.edit().clear().commit(); }

    private static JSONObject user(String account, String imUser, boolean includeMask) throws Exception {
        JSONObject user = new JSONObject().put("id", account).put("im_user_id", imUser)
            .put("nickname", "Cached nickname");
        if (includeMask) user.put("phone_masked", "138****1234");
        return user;
    }

    private void cache(JSONObject user, boolean newLogin) throws Exception {
        auth.cacheAccountProfile(new MaiChatAuthClient.AccountProfile(user), newLogin);
    }

    private MaiChatAuthClient.AccountProfile snapshot(String account) throws Exception {
        return auth.currentAccountProfile(account, auth.accountSessionGeneration());
    }

    @Test public void offlineSnapshotDoesNotRefreshOrDecryptCredentials() throws Exception {
        cache(user("account_a", "im_a", true), false);
        // 没有可用 access token，刷新密文也故意无效；任何鉴权/网络路径都会使本测试失败。
        preferences.edit().putString("refresh", "unreadable-test-fixture").commit();
        MaiChatAuthClient.AccountProfile profile = snapshot("account_a");
        assertEquals("account_a", profile.accountId);
        assertEquals("im_a", profile.imUserId);
        assertEquals("Cached nickname", profile.nickname);
        assertEquals("138****1234", profile.phoneMasked);
    }

    @Test public void sameAccountRefreshWithoutPhoneFieldKeepsLastAuthMeMask() throws Exception {
        cache(user("account_a", "im_a", true), false);
        // 真实 refresh 的 publicUser 只返回 ID、IM ID 和昵称，缺字段不表示取消绑定手机号。
        cache(user("account_a", "im_a", false).put("nickname", "Refreshed nickname"), false);
        MaiChatAuthClient.AccountProfile profile = snapshot("account_a");
        assertEquals("138****1234", profile.phoneMasked);
        assertEquals("Refreshed nickname", profile.nickname);
    }

    @Test public void explicitNullPhoneClearsThePreviousMask() throws Exception {
        cache(user("account_a", "im_a", true), false);
        cache(user("account_a", "im_a", false).put("phone_masked", JSONObject.NULL), false);
        assertNull(snapshot("account_a").phoneMasked);
    }

    @Test public void anotherAccountCannotInheritPhoneFromPartialResponse() throws Exception {
        cache(user("account_a", "im_a", true), false);
        cache(user("account_b", "im_b", false), false);
        MaiChatAuthClient.AccountProfile profile = snapshot("account_b");
        assertEquals("im_b", profile.imUserId);
        assertNull(profile.phoneMasked);
        assertThrows(java.io.IOException.class, () -> snapshot("account_a"));
    }

    @Test public void newLoginWithoutPhoneDoesNotKeepAnOlderSessionMask() throws Exception {
        cache(user("account_a", "im_a", true), false);
        cache(user("account_a", "im_a", false), true);
        assertNull(snapshot("account_a").phoneMasked);
    }

    private static void setField(Object owner, String name, Object value) throws Exception {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(owner, value);
    }

    @Test public void sessionQueryReadsCacheWithoutAnImClientOrNetworkWorker() throws Exception {
        cache(user("account_a", "im_a", true), false);
        preferences.edit().remove("im_user_id")
            .putString("refresh", "unreadable-test-fixture").commit();
        Path root = Files.createTempDirectory("maichat-cached-account-session");
        RemoteIMSessionController session = new RemoteIMSessionController(
            new LocalSettingsStore(root.resolve("settings.properties").toFile()),
            new LocalChatHistoryStore(root.resolve("history").toFile()));
        session.login("im_a");
        // 保留生产资料入口与代际检查，只注入已恢复的账号状态；client 留空，不能查询 SDK。
        setField(session, "productionMode", true);
        setField(session, "authClient", auth);
        setField(session, "mainHandler", new Handler(Looper.getMainLooper()));
        setField(session, "selfAvatarUrl", "https://example.test/cached-avatar.jpg");
        Field executor = RemoteIMSessionController.class.getDeclaredField("ioExecutor");
        executor.setAccessible(true);
        ((ExecutorService) executor.get(session)).shutdown();
        AtomicReference<RemoteIMSessionController.CurrentAccount> result = new AtomicReference<>();
        AtomicReference<String> error = new AtomicReference<>();
        CountDownLatch completed = new CountDownLatch(1);
        Thread query = new Thread(() -> session.getCurrentAccount((profile, failure) -> {
            result.set(profile); error.set(failure); completed.countDown();
        }), "MaiChat-account-cache-test");
        query.start(); query.join(3000);
        assertFalse(query.isAlive());
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(completed.await(1, TimeUnit.SECONDS));
        assertNull(error.get());
        assertNotNull(result.get());
        assertTrue(result.get().loggedIn);
        assertEquals("account_a", result.get().accountId);
        assertEquals("im_a", result.get().imUserId);
        assertEquals("138****1234", result.get().phoneMasked);
        assertEquals("https://example.test/cached-avatar.jpg", result.get().avatarUrl);
    }
}
