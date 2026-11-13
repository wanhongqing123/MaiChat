// 文件作用：以普通 Backend/SDK 宿主替身验证绑定适配，不调用 Native、模型、IM 或真实数据库。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import androidx.test.core.app.ApplicationProvider;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MaiSessionIMBindingServiceTest {
    private Context context;
    private Host host;
    private Backend backend;
    @Before public void setup() {
        context = ApplicationProvider.getApplicationContext();
        context.getSharedPreferences("session-im-bindings", Context.MODE_PRIVATE).edit().clear().commit();
        host = new Host(); backend = new Backend(host);
    }
    private MaiSessionIMBindingService service() {
        MaiSessionIMBindingService service = new MaiSessionIMBindingService(context, host, () -> backend, () -> {});
        service.updateAccount(); return service;
    }

    @Test public void ordinaryGatewayAndProjectionPersistOnlyExplicitAccountBindings() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> assertNull(error));
            assertEquals("session", service.binding("peer").getString("session_id"));
            JSONArray saved = new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]"));
            assertEquals(1, saved.length()); assertEquals(2, saved.getJSONObject(0).length());
            service.forPeer("cached-stranger").bind("session", (value, error) -> assertNotNull(error));
            assertEquals(1, backend.count("bind"));
            service.forPeer("peer").unbind((value, error) -> assertNull(error));
            assertNull(service.binding("peer"));
            assertEquals("takeover", backend.controls.get(0).getString("action"));
            assertEquals(0, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
        }
    }

    @Test public void restoresAllSavedBindingsWithoutOverwritingRemainingPreferences() throws Exception {
        context.getSharedPreferences("session-im-bindings", 0).edit().putString("123|owner",
            "[{\"peer_id\":\"peer\",\"session_id\":\"session\"},{\"peer_id\":\"other\",\"session_id\":\"second\"}]").commit();
        try (MaiSessionIMBindingService service = service()) {
            assertNotNull(service.binding("peer")); assertNotNull(service.binding("other"));
            assertEquals(2, backend.count("bind"));
            assertEquals(2, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
        }
    }

    @Test public void admitHappensAtArrivalThenFillsOnlyAfterNewRowAndMediaReady() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            TencentIMClient.LiveAdmission ticket = service.capture("peer", "owner", "sdk-one", RemoteIMOrigin.HUMAN, false, true);
            assertNotNull(ticket); assertEquals(1, backend.controlCount("admit")); assertEquals(0, backend.incoming.size());
            ticket.mediaReady(message("sdk-one", "peer", "owner", RemoteIMOrigin.HUMAN));
            assertEquals(0, backend.incoming.size());
            stored(ticket, "sdk-one", true);
            assertEquals(1, backend.incoming.size());
            JSONObject fill = backend.incoming.get(0);
            assertEquals("1", fill.getString("input_id"));
            assertEquals("1", fill.getString("expected_binding_epoch"));
            assertEquals("session", fill.getString("expected_session_id"));
            assertEquals("sdk-one", fill.getString("message_id"));
            assertFalse(fill.has("has_content"));
        }
    }

    @Test public void duplicateFailureAndWrongReadinessIdentityAbandonReservedInputs() {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            TencentIMClient.LiveAdmission duplicate = service.capture("peer", "owner", "sdk-duplicate", RemoteIMOrigin.HUMAN, false, true);
            stored(duplicate, "sdk-duplicate", false); duplicate.mediaReady(message("sdk-duplicate", "peer", "owner", RemoteIMOrigin.HUMAN));
            TencentIMClient.LiveAdmission failed = service.capture("peer", "owner", "sdk-failed", RemoteIMOrigin.HUMAN, false, true);
            failed.abandoned(); failed.abandoned();
            TencentIMClient.LiveAdmission mismatched = service.capture("peer", "owner", "sdk-original", RemoteIMOrigin.HUMAN, false, true);
            stored(mismatched, "sdk-original", true); mismatched.mediaReady(message("sdk-other", "peer", "owner", RemoteIMOrigin.HUMAN));
            assertEquals(0, backend.incoming.size()); assertEquals(2, backend.controlCount("abandon_input"));
            assertEquals(2, backend.controlCount("persisted_input"));
        }
    }

    @Test public void machineControlHistoricalAndUnacceptedPeersDoNotReserveInputs() {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            assertNull(service.capture("peer", "owner", "machine", RemoteIMOrigin.MACHINE, false, true));
            assertNull(service.capture("peer", "owner", "control", RemoteIMOrigin.HUMAN, true, true));
            assertNull(service.capture("peer", "owner", "empty", RemoteIMOrigin.HUMAN, false, false));
            assertNull(service.capture("cached-stranger", "owner", "unknown", RemoteIMOrigin.HUMAN, false, true));
            assertNull(service.capture("peer", "other-owner", "wrong-owner", RemoteIMOrigin.HUMAN, false, true));
            assertEquals(0, backend.controlCount("admit"));
        }
    }

    @Test public void originalBindingEpochIsCarriedThroughLateMediaAndBackendReplacement() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            Backend original = backend;
            TencentIMClient.LiveAdmission old = service.capture("peer", "owner", "sdk-old", RemoteIMOrigin.HUMAN, false, true);
            original.valid = false; backend = new Backend(host); service.updateAccount();
            stored(old, "sdk-old", true); old.mediaReady(message("sdk-old", "peer", "owner", RemoteIMOrigin.HUMAN));
            assertEquals(0, original.incoming.size()); assertEquals(1, original.controlCount("abandon_input"));
            assertEquals(0, backend.incoming.size());
            assertNotNull(service.binding("peer"));
        }
    }

    @Test public void lateMediaNeverRetargetsTheOriginalNativeAdmission() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            TencentIMClient.LiveAdmission original = service.capture("peer", "owner", "sdk-old-route", RemoteIMOrigin.HUMAN, false, true);
            service.forPeer("peer").bind("second", (value, error) -> {});
            stored(original, "sdk-old-route", true); original.mediaReady(message("sdk-old-route", "peer", "owner", RemoteIMOrigin.HUMAN));
            assertEquals("session", backend.incoming.get(0).getString("expected_session_id"));
            assertEquals("1", backend.incoming.get(0).getString("expected_binding_epoch"));
            assertEquals("1", backend.incoming.get(0).getString("input_id"));
        }
    }

    @Test public void friendProjectionAndLogoutRevokeWithoutReplayingCachedMessages() {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            host.friends = new JSONArray().put("other"); service.updateAccount();
            assertEquals(1, backend.count("set_friends")); assertNull(service.binding("peer"));
            host.active = false; service.updateAccount();
            assertTrue(backend.controlCount("logout") > 0); assertEquals(0, backend.incoming.size());
        }
    }

    @Test public void conflictingRebindPreservesTheOldNativeBindingAndPendingAuthority() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> assertNull(error));
            backend.routes.put("other", "second");
            backend.observer.projection(backend.projection());
            backend.controls.clear();
            service.forPeer("peer").bind("second", (value, error) -> assertNotNull(error));
            assertEquals("session", backend.routes.get("peer"));
            assertEquals("session", service.binding("peer").getString("session_id"));
            assertEquals(0, backend.controlCount("takeover"));
        }
    }

    @Test public void staleControllerGenerationOrPublicationCannotOverwriteUiOrPreferences() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            JSONObject stale = backend.projection(); stale.getJSONObject("scope").put("controller_generation", "4");
            stale.put("bindings", new JSONArray()); backend.observer.projection(stale);
            assertNotNull(service.binding("peer"));
            backend.publications = false;
            JSONObject delayed = backend.projection(); delayed.put("bindings", new JSONArray()); backend.observer.projection(delayed);
            assertNotNull(service.binding("peer"));
            assertEquals(1, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
        }
    }

    @Test public void sameOwnerOnAnotherSdkDoesNotRestoreTheOtherSdkBindings() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            host.sdk = "456"; backend = new Backend(host); service.updateAccount();
            assertNull(service.binding("peer")); assertEquals(0, backend.count("bind"));
            assertEquals(1, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
            assertEquals(0, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("456|owner", "[]")).length());
        }
    }

    @Test public void oldObserverCloseCannotLogoutOrClearTheNewBridgeOwner() {
        MaiSessionIMBindingService first = service();
        MaiSessionIMBindingService second = service();
        MaiSessionIMBindingService.Backend.Observer current = backend.observer;
        first.close();
        assertSame(current, backend.observer); assertEquals(0, backend.controlCount("logout"));
        second.close(); assertEquals(1, backend.controlCount("logout"));
    }

    @Test public void authoritativeUnbindAndImmediateTakeoverPreferencesCannotReviveAfterConfigure() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            service.forPeer("peer").bind("session", (value, error) -> {});
            backend.routes.clear();
            JSONObject authoritative = backend.projection();
            assertTrue(MaiSessionIMBindingService.persistProjection(context, authoritative, () -> true));
            backend.valid = false; backend = new Backend(host); service.updateAccount();
            assertNull(service.binding("peer")); assertEquals(0, backend.count("bind"));
            service.forPeer("peer").bind("session", (value, error) -> {});
            MaiSessionIMBindingService.removeSessionPreference(context, backend.scope(), "session", () -> true);
            backend.valid = false; backend = new Backend(host); service.updateAccount();
            assertNull(service.binding("peer")); assertEquals(0, backend.count("bind"));
        }
    }

    @Test public void bindPinsTheDisplayedWorkspaceAndOldControllerCannotRetarget() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            MaiSessionIMBindingService.Gateway picker = service.forPeer("peer");
            java.util.concurrent.atomic.AtomicReference<JSONObject> displayed = new java.util.concurrent.atomic.AtomicReference<>();
            picker.load((value, error) -> displayed.set(value));
            backend.workspace = "/changed"; backend.observer.projection(backend.projection());
            picker.bind("session", displayed.get(), (value, error) -> assertNotNull(error));
            assertEquals(0, backend.count("bind"));
            picker.load((value, error) -> displayed.set(value));
            picker.bind("session", displayed.get(), (value, error) -> assertNull(error));
            assertEquals("/changed", backend.requests.get(backend.requests.size() - 1).optString("expected_workspace_key"));
            Backend old = backend; old.valid = false; backend = new Backend(host); backend.controller = "6"; service.updateAccount();
            int before = backend.count("bind");
            picker.bind("second", displayed.get(), (value, error) -> assertNotNull(error));
            assertEquals(before, backend.count("bind"));
        }
    }

    @Test public void provisionalHistoryBindingIsVisibleButNeverStoredAsReadyPreference() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            backend.historyState = "pending";
            service.forPeer("peer").bind("session", (value, error) -> assertNull(error));
            assertEquals("pending", service.binding("peer").optString("history_state"));
            assertEquals(0, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
            backend.historyState = "ready"; backend.observer.projection(backend.projection());
            assertEquals(1, new JSONArray(context.getSharedPreferences("session-im-bindings", 0).getString("123|owner", "[]")).length());
        }
    }

    @Test public void everyProjectionConsumesOneShotTasksOnceAfterProvisionalPublication() throws Exception {
        try (MaiSessionIMBindingService service = service()) {
            backend.historyState = "pending";
            host.published = () -> service.binding("peer") != null;
            JSONObject task = new JSONObject().put("bridge_id", "bridge").put("scope", backend.scope()).put("peer_id", "peer")
                .put("session_id", "session").put("binding_epoch", "1").put("import_id", "").put("task_id", "task-1");
            backend.responseTask = task;
            service.forPeer("peer").bind("session", (value, error) -> assertNull(error));
            assertEquals(1, host.fences);
            service.forPeer("peer").load((value, error) -> assertNull(error));
            JSONObject normal = backend.projection().put("history_tasks", new JSONArray().put(task).put(task));
            backend.observer.projection(normal);
            backend.observer.projection(new JSONObject(normal.toString()));
            assertEquals(1, host.fences); assertEquals(1, backend.history.size());
            assertEquals("", backend.history.get(0).getString("import_id"));
            assertEquals(task.toString(), backend.history.get(0).toString());
        }
    }

    private static void stored(TencentIMClient.LiveAdmission ticket, String remote, boolean inserted) {
        ticket.metadataPersisted(new RemoteIMIngress.Stored("row-" + remote, remote, 100, inserted));
    }

    private static RemoteIMMessage message(String id, String from, String to, RemoteIMOrigin origin) {
        return new RemoteIMMessage("local-" + id, id, from, to, "exact body", RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED, 100, null, null, null, null, origin);
    }
    private static final class Host implements MaiSessionIMBindingService.Host {
        boolean active = true;
        String sdk = "123";
        int fences;
        java.util.function.BooleanSupplier published = () -> true;
        JSONArray friends = new JSONArray().put("peer").put("other");
        @Override public JSONObject activation() {
            if (!active) return null;
            JSONObject value = new JSONObject(); put(value, "sdk_app_id", sdk); put(value, "owner_id", "owner");
            put(value, "auth_generation", "4"); put(value, "friends", friends); return value;
        }
        @Override public boolean current(JSONObject scope, String peer) {
            if (!active || scope == null || !"owner".equals(scope.optString("owner_id")) || !sdk.equals(scope.optString("sdk_app_id"))) return false;
            if (peer.isEmpty()) return true;
            for (int i = 0; i < friends.length(); i++) if (peer.equals(friends.optString(i))) return true;
            return false;
        }
        @Override public void delivery(JSONObject value, MaiSessionIMBindingService.Backend backend) { }
        @Override public RemoteIMIngress.Fence historyFence(JSONObject task) {
            assertTrue("Provisional admission projection must be published before fence capture", published.getAsBoolean());
            fences++;
            return new RemoteIMIngress.Fence(new RemoteIMIngress.Scope(123, "owner", 0), 0,
                java.util.concurrent.CompletableFuture.completedFuture(null), () -> true);
        }
    }
    private static final class Backend implements MaiSessionIMBindingService.Backend {
        final Host host;
        final Map<String, String> routes = new LinkedHashMap<>();
        final List<String> actions = new ArrayList<>();
        final List<JSONObject> controls = new ArrayList<>(), incoming = new ArrayList<>();
        final List<JSONObject> requests = new ArrayList<>();
        final List<JSONObject> history = new ArrayList<>();
        boolean valid = true, publications = true; int inputs;
        String workspace = "/workspace", controller = "5", historyState = "ready";
        JSONObject responseTask;
        Observer observer;
        Backend(Host host) { this.host = host; }
        @Override public boolean current() { return valid; }
        @Override public boolean publicationCurrent(JSONObject value) { return valid && publications; }
        @Override public void observe(Observer value) { observer = value; }
        @Override public void clearObserver(Observer expected) { if (observer == expected) observer = null; }
        @Override public boolean ownsObserver(Observer expected) { return expected != null && observer == expected; }
        @Override public void request(JSONObject value, BiConsumer<JSONObject, String> completion) {
            String action = value.optString("action"); actions.add(action);
            requests.add(MaiSessionIMBindingService.copy(value));
            if (action.equals("bind")) {
                if (!workspace.equals(value.optString("expected_workspace_key"))) { completion.accept(null, "Workspace changed"); return; }
                boolean conflict = routes.entrySet().stream().anyMatch(entry -> !entry.getKey().equals(value.optString("peer_id"))
                    && entry.getValue().equals(value.optString("session_id")));
                if (conflict) { completion.accept(null, "Conflict"); return; }
                routes.put(value.optString("peer_id"), value.optString("session_id"));
            }
            if (action.equals("unbind")) routes.remove(value.optString("peer_id"));
            if (action.equals("set_friends")) routes.keySet().removeIf(peer -> !host.current(scope(), peer));
            completion.accept(projection(), null);
        }
        @Override public void incoming(JSONObject value, RemoteIMMessage message, BiConsumer<JSONObject, String> completion) {
            incoming.add(MaiSessionIMBindingService.copy(value));
            JSONObject response = new JSONObject(); put(response, "ok", true); put(response, "accepted", true); completion.accept(response, null);
        }
        @Override public JSONObject control(JSONObject value) {
            controls.add(MaiSessionIMBindingService.copy(value));
            JSONObject result = new JSONObject(); put(result, "ok", true);
            if (value.optString("action").equals("takeover")) {
                routes.entrySet().removeIf(entry -> value.optString("session_id").equals(entry.getValue())); put(result, "revoked", true);
            }
            if (value.optString("action").equals("admit")) {
                put(result, "accepted", true); put(result, "input_id", Integer.toString(++inputs));
            }
            return result;
        }
        @Override public void historyTask(JSONObject task, RemoteIMIngress.Fence fence) {
            assertTrue(fence.isCurrent()); history.add(MaiSessionIMBindingService.copy(task));
        }
        JSONObject scope() {
            JSONObject scope = host.activation();
            if (scope == null) scope = new JSONObject();
            scope.remove("friends"); put(scope, "controller_generation", controller); return scope;
        }
        JSONObject projection() {
            JSONObject value = new JSONObject(); put(value, "ok", true); put(value, "bridge_id", "bridge"); put(value, "scope", scope());
            JSONArray sessions = new JSONArray();
            for (String id : new String[]{"session", "second"}) {
                JSONObject row = new JSONObject(); put(row, "id", id); put(row, "title", id); put(row, "workspace_key", workspace); sessions.put(row);
            }
            JSONArray bindings = new JSONArray();
            routes.forEach((peer, session) -> {
                JSONObject row = new JSONObject(); put(row, "peer_id", peer); put(row, "session_id", session);
                put(row, "session_title", session); put(row, "epoch", "1"); bindings.put(row);
                put(row, "history_state", historyState);
            });
            put(value, "sessions", sessions); put(value, "bindings", bindings); put(value, "has_bindings", !routes.isEmpty()); put(value, "pending_count", 0);
            if (responseTask != null) put(value, "history_tasks", new JSONArray().put(MaiSessionIMBindingService.copy(responseTask)));
            return value;
        }
        int count(String action) { return (int) actions.stream().filter(action::equals).count(); }
        int controlCount(String action) { return (int) controls.stream().filter(value -> action.equals(value.optString("action"))).count(); }
    }
    private static void put(JSONObject object, String key, Object value) { MaiSessionIMBindingService.put(object, key, value); }
}
