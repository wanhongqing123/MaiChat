// 文件作用：真实 controller/worker/service 的 owned Native transport 测试；不初始化 JNI、模型或 SDK。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import androidx.test.core.app.ApplicationProvider;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
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
import org.robolectric.shadows.ShadowLog;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, shadows = AIAssistantControllerHistoryHandoffTest.OwnedNative.class)
public class AIAssistantControllerHistoryHandoffTest {
    private static volatile Driver active;
    private AIAssistantController controller;
    private MaiSessionIMBindingService service;
    private Driver driver;

    @Before public void setup() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        context.getSharedPreferences("session-im-bindings", 0).edit().clear().commit();
        driver = new Driver(); active = driver;
        controller = new AIAssistantController(context, "http://owned.invalid");
        driver.controller = controller;
        field("handle").setLong(controller, 41L); field("nativeEpoch").setLong(controller, 7L);
        service = new MaiSessionIMBindingService(context, new Host(driver), controller::sessionIMBackend, () -> {});
        service.updateAccount();
        waitFor(() -> driver.lists.get() >= 2);
        drain();
    }

    @After public void close() throws Exception {
        if (service != null) service.close();
        if (controller != null) {
            Method close = AIAssistantController.class.getDeclaredMethod("close"); close.setAccessible(true); close.invoke(controller);
        }
        active = null;
    }

    @Test public void bindAndFreshHelperTasksSurviveFourOtherSessionTakeovers() throws Exception {
        driver.arm(4, false);
        service.forPeer("friend-A").bind("A", (value, error) -> {});
        assertTerminated("task-A"); assertTerminated("task-C");
        assertEquals(4, driver.takeovers.get());
        assertEquals(1, driver.opened("task-A") + driver.failed("task-A"));
        assertEquals(1, driver.opened("task-C") + driver.failed("task-C"));
    }

    @Test public void pumpAndFreshHelperTasksSurviveFourOtherSessionTakeovers() throws Exception {
        driver.arm(4, true);
        ((Handler) field("worker").get(controller)).post(() -> {
            try { Method pump = AIAssistantController.class.getDeclaredMethod("pumpSessionIM"); pump.setAccessible(true); pump.invoke(controller); }
            catch (Exception failure) { throw new AssertionError(failure); }
        });
        assertTerminated("task-A"); assertTerminated("task-C");
        assertEquals(4, driver.takeovers.get());
        assertEquals(1, driver.opened("task-A") + driver.failed("task-A"));
        assertEquals(1, driver.opened("task-C") + driver.failed("task-C"));
    }

    @Test public void repeatedFreshFailuresExplicitlyTerminateEachOriginalOneShotOnce() throws Exception {
        driver.arm(1000, false);
        service.forPeer("friend-A").bind("A", (value, error) -> {});
        for (String id : new String[]{"task-A", "task-C", "task-D"}) {
            assertTerminated(id); assertEquals(0, driver.opened(id)); assertEquals(1, driver.failed(id));
        }
    }

    @Test public void tasksFirstReceivedDuringFailedPublicationRetryAlsoSurvive() throws Exception {
        driver.arm(8, false);
        service.forPeer("friend-A").bind("A", (value, error) -> {});
        for (String id : new String[]{"task-A", "task-C", "task-D"}) {
            assertTerminated(id); assertEquals(1, driver.opened(id) + driver.failed(id));
        }
    }

    @Test public void realPumpWarningsRemainReadableChinese() throws Exception {
        driver.warnings = true;
        ((Handler) field("worker").get(controller)).post(() -> {
            try { Method pump = AIAssistantController.class.getDeclaredMethod("pumpSessionIM"); pump.setAccessible(true); pump.invoke(controller); }
            catch (Exception failure) { throw new AssertionError(failure); }
        });
        waitFor(() -> driver.localWarnings.size() == 3);
        assertEquals(Arrays.asList("附件准备超时，本条消息未自动处理", "绑定回复交付未确认，未自动重发", "消息未能加入绑定会话，请在创作中心查看"), driver.localWarnings);
    }

    @Test public void backgroundCleanupLogsLocallyWithoutMaskingLiveWarningOrLingeringAfterRecovery() throws Exception {
        ShadowLog.clear();
        driver.cleanupPending = true; driver.maintenanceWarning = true;
        int before = driver.lists.get();
        postPump(); waitFor(() -> driver.lists.get() > before);
        assertEquals(Arrays.asList("消息未能加入绑定会话，请在创作中心查看"), driver.localWarnings);
        assertTrue(field("sessionIMCleanupPending").getBoolean(controller));
        assertEquals(1, ShadowLog.getLogsForTag("MaiChatAgent").stream()
            .filter(item -> item.msg.equals("Session IM local history cleanup will retry")).count());

        driver.cleanupPending = false; driver.maintenanceWarning = false;
        int recovery = driver.lists.get();
        postPump(); waitFor(() -> driver.lists.get() > recovery);
        assertFalse(field("sessionIMCleanupPending").getBoolean(controller));
        assertEquals(Arrays.asList("消息未能加入绑定会话，请在创作中心查看"), driver.localWarnings);
        assertEquals(1, ShadowLog.getLogsForTag("MaiChatAgent").stream()
            .filter(item -> item.msg.equals("Session IM local history cleanup will retry")).count());
    }

    private void postPump() throws Exception {
        ((Handler) field("worker").get(controller)).post(() -> {
            try { Method pump = AIAssistantController.class.getDeclaredMethod("pumpSessionIM"); pump.setAccessible(true); pump.invoke(controller); }
            catch (Exception failure) { throw new AssertionError(failure); }
        });
    }

    private void assertTerminated(String task) throws Exception {
        waitFor(() -> driver.opened(task) + driver.failed(task) > 0);
    }
    private static Field field(String name) throws Exception {
        Field value = AIAssistantController.class.getDeclaredField(name); value.setAccessible(true); return value;
    }
    private static void drain() { Shadows.shadowOf(Looper.getMainLooper()).idle(); }
    private static void waitFor(BooleanSupplier condition) throws Exception {
        long until = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(3);
        while (!condition.getAsBoolean() && System.nanoTime() < until) {
            Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(25)); Thread.sleep(5);
        }
        drain(); assertTrue("owned controller task was neither handed off nor explicitly failed", condition.getAsBoolean());
    }

    @Implements(value = AIAssistantController.class, isInAndroidSdk = false)
    public static class OwnedNative {
        @Implementation protected void initialize() { /* Owned transport: no native library, credentials or model initialization. */ }
        @Implementation protected boolean ownsModelConfiguration() { return active != null && active.ownerGate.get(); }
        @Implementation protected static byte[] nativeRequest(long handle, byte[] request) {
            assertEquals(41L, handle); assertNotEquals(Looper.getMainLooper(), Looper.myLooper());
            try { return active.invoke(new JSONObject(new String(request, StandardCharsets.UTF_8))).toString().getBytes(StandardCharsets.UTF_8); }
            catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Implementation protected static byte[] nativeSessionIMControl(byte[] request) {
            try {
                JSONObject value = new JSONObject(new String(request, StandardCharsets.UTF_8));
                if (value.optString("action").equals("takeover")) active.takeovers.incrementAndGet();
                return new JSONObject().put("ok", true).put("revoked", true).toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Implementation protected static byte[] nativeIMHistorySnapshot(byte[] request) {
            assertNotEquals(Looper.getMainLooper(), Looper.myLooper());
            try {
                JSONObject value = new JSONObject(new String(request, StandardCharsets.UTF_8));
                String action = value.optString("action"); JSONObject result = new JSONObject().put("ok", true);
                if (action.equals("open")) {
                    String id = value.getJSONObject("task").getString("task_id"); result.put("snapshot_id", "snapshot-" + id);
                } else if (action.equals("read")) result.put("snapshot_id", value.getString("snapshot_id")).put("sequence", "0")
                    .put("records", new JSONArray()).put("has_more", false);
                return result.toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) { throw new AssertionError(failure); }
        }
        @Implementation protected static void nativeDestroy(long handle) { }
    }

    private static final class Host implements MaiSessionIMBindingService.Host {
        final Driver driver; Host(Driver driver) { this.driver = driver; }
        @Override public JSONObject activation() {
            JSONObject value = Driver.scope(); value.remove("controller_generation");
            put(value, "friends", new JSONArray().put("friend-A").put("friend-B").put("friend-C")); return value;
        }
        @Override public boolean current(JSONObject scope, String peer) { return scope != null && "owner".equals(scope.optString("owner_id")); }
        @Override public void delivery(JSONObject value, MaiSessionIMBindingService.Backend backend) { fail("No SDK send"); }
        @Override public RemoteIMIngress.Fence historyFence(JSONObject task) {
            return new RemoteIMIngress.Fence(new RemoteIMIngress.Scope(123, "owner", 0), 0, CompletableFuture.completedFuture(null), () -> true);
        }
        @Override public void warning(String value) { driver.localWarnings.add(value); }
    }

    private static final class Driver {
        final AtomicBoolean ownerGate = new AtomicBoolean(true);
        final AtomicInteger lists = new AtomicInteger(), takeovers = new AtomicInteger();
        final Map<String, AtomicInteger> opened = new ConcurrentHashMap<>(), failed = new ConcurrentHashMap<>();
        final List<String> localWarnings = new CopyOnWriteArrayList<>();
        volatile AIAssistantController controller;
        volatile boolean cleanupPending, maintenanceWarning;
        int disrupt, freshAttempts; boolean armed, fromPump, initialIssued, helperIssued, a, c, d, warnings;
        void arm(int count, boolean pump) { disrupt = count; fromPump = pump; armed = true; }
        int opened(String id) { return opened.getOrDefault(id, new AtomicInteger()).get(); }
        int failed(String id) { return failed.getOrDefault(id, new AtomicInteger()).get(); }
        synchronized JSONObject invoke(JSONObject request) throws Exception {
            assertTrue(ownerGate.get()); assertEquals("im_session_binding", request.optString("op"));
            String action = request.getString("action"); JSONObject task = null;
            if (action.equals("bind") && armed && !fromPump && !initialIssued) { a = true; initialIssued = true; task = task("A"); }
            if (action.equals("pump") && armed && fromPump && !initialIssued) { a = true; initialIssued = true; task = task("A"); }
            if (action.equals("list")) {
                lists.incrementAndGet();
                if (initialIssued && disrupt > 0) {
                    disrupt--; freshAttempts++;
                    long before = field("nativeEpoch").getLong(controller);
                    JSONObject authority = MaiSessionIMBindingService.copy((JSONObject) field("sessionIMAuthority").get(controller));
                    Method takeover = AIAssistantController.class.getDeclaredMethod("takeOverSessionIM", String.class); takeover.setAccessible(true); takeover.invoke(controller, "B");
                    assertEquals(before, field("nativeEpoch").getLong(controller));
                    assertTrue(MaiSessionIMBindingService.sameAuthority(authority, (JSONObject) field("sessionIMAuthority").get(controller)));
                    if (!helperIssued) { c = true; helperIssued = true; task = task("C"); }
                    else if (freshAttempts == 5) { d = true; task = task("D"); }
                }
            }
            if (action.equals("history_opened")) opened.computeIfAbsent(request.getJSONObject("task").getString("task_id"), ignored -> new AtomicInteger()).incrementAndGet();
            if (action.equals("history_failed")) failed.computeIfAbsent(request.getJSONObject("task").getString("task_id"), ignored -> new AtomicInteger()).incrementAndGet();
            JSONObject value = projection();
            if (task != null) value.put("history_tasks", new JSONArray().put(task));
            if (action.equals("history_chunk")) value.put("page_accepted", true).put("history_progress", new JSONObject().put("committed", true));
            if (action.equals("pump") && warnings) value.put("warnings", new JSONArray().put(new JSONObject().put("code", "input_timeout"))
                .put(new JSONObject().put("code", "delivery_timeout")).put(new JSONObject().put("code", "input_rejected")));
            if (action.equals("pump") && maintenanceWarning) value.put("warnings", new JSONArray()
                .put(new JSONObject().put("code", "history_cleanup_failed").put("outcome", "retry_local_cleanup"))
                .put(new JSONObject().put("code", "input_rejected").put("outcome", "input_not_submitted")));
            return value;
        }
        JSONObject projection() throws Exception {
            JSONArray sessions = new JSONArray(), bindings = new JSONArray();
            for (String id : new String[]{"A", "B", "C", "D"}) {
                sessions.put(new JSONObject().put("id", id).put("title", id).put("workspace_key", "/owned-workspace").put("busy", false));
                if (id.equals("A") && a || id.equals("C") && c || id.equals("D") && d) bindings.put(new JSONObject().put("peer_id", "friend-" + id)
                    .put("session_id", id).put("session_title", id).put("epoch", "1").put("history_state", "pending"));
            }
            return new JSONObject().put("ok", true).put("bridge_id", "bridge").put("scope", scope()).put("sessions", sessions)
                .put("bindings", bindings).put("has_bindings", a || c).put("pending_count", 0)
                .put("history_cleanup_pending", cleanupPending);
        }
        static JSONObject scope() {
            JSONObject value = new JSONObject(); put(value, "sdk_app_id", "123"); put(value, "owner_id", "owner");
            put(value, "auth_generation", "4"); put(value, "controller_generation", "5"); return value;
        }
        static JSONObject task(String session) throws Exception {
            return new JSONObject().put("bridge_id", "bridge").put("scope", scope()).put("peer_id", "friend-" + session)
                .put("session_id", session).put("binding_epoch", "1").put("import_id", "").put("task_id", "task-" + session);
        }
    }
    private static void put(JSONObject value, String key, Object data) { MaiSessionIMBindingService.put(value, key, data); }
}
