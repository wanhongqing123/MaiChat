// 文件作用：会话绑定的 Java 边界适配；所有路由、FIFO、去重、权威和回执状态均归共享 Native 内核。
package com.kongshang.maichat;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.concurrent.CompletableFuture;
import java.util.function.BiConsumer;
import java.util.function.Supplier;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;

public final class MaiSessionIMBindingService implements AutoCloseable {
    public interface Backend {
        interface Observer {
            void projection(JSONObject value);
            void delivery(JSONObject value);
            default void warning(String value) { }
            default boolean historyTaskKnown(JSONObject task) { return false; }
        }
        void request(JSONObject value, BiConsumer<JSONObject, String> completion);
        void incoming(JSONObject admission, RemoteIMMessage ready, BiConsumer<JSONObject, String> completion);
        JSONObject control(JSONObject value);
        void historyTask(JSONObject task, RemoteIMIngress.Fence fence);
        void observe(Observer observer);
        void clearObserver(Observer expected);
        boolean ownsObserver(Observer expected);
        default boolean current() { return true; }
        default boolean publicationCurrent(JSONObject value) { return current(); }
    }
    public interface Host {
        JSONObject activation();
        boolean current(JSONObject scope, String peer);
        void delivery(JSONObject value, Backend backend);
        default void warning(String value) { }
        default void publishIngressState(Runnable publication) { publication.run(); }
        default RemoteIMIngress.Fence historyFence(JSONObject task) { throw new IllegalStateException("No ordinary history writer is attached"); }
    }
    public interface Gateway {
        String peerId();
        void load(BiConsumer<JSONObject, String> completion);
        void bind(String sessionId, BiConsumer<JSONObject, String> completion);
        default void bind(String sessionId, JSONObject displayed, BiConsumer<JSONObject, String> completion) { bind(sessionId, completion); }
        void unbind(BiConsumer<JSONObject, String> completion);
        default AutoCloseable subscribe(Consumer<JSONObject> listener) { return () -> {}; }
    }

    private final Host host;
    private final Context context;
    private final Supplier<Backend> backendFactory;
    private final SharedPreferences preferences;
    private final Runnable changed;
    private volatile JSONObject projection = new JSONObject();
    private volatile Backend backend;
    private volatile String identity = "";
    private String accountIdentity = "";
    private long generation;
    private String friendFingerprint = "";
    private volatile boolean closed;
    private boolean restoring;
    private boolean needsActivation;
    private volatile Backend.Observer observer;
    private String controllerHighwater = "";
    private final java.util.concurrent.CopyOnWriteArrayList<Consumer<JSONObject>> projectionListeners = new java.util.concurrent.CopyOnWriteArrayList<>();
    private final java.util.LinkedHashSet<String> consumedHistoryTasks = new java.util.LinkedHashSet<>();

    public MaiSessionIMBindingService(Context context, Host host, Supplier<Backend> backendFactory, Runnable changed) {
        this.context = context.getApplicationContext(); this.host = host;
        this.backendFactory = backendFactory;
        this.changed = changed;
        preferences = context.getApplicationContext().getSharedPreferences("session-im-bindings", Context.MODE_PRIVATE);
    }

    /** 主线程只发布鉴权与已接受好友的不可变投影，不读取窗口消息或发起模型请求。 */
    public void updateAccount() {
        if (closed) return;
        JSONObject account = host.activation();
        String accountKey = account == null ? "" : account.optString("sdk_app_id") + "|"
            + account.optString("owner_id") + "|" + account.optString("auth_generation");
        if (!accountKey.equals(accountIdentity) || needsActivation || backend != null && !backend.current()) {
            needsActivation = false;
            revoke();
            if (!accountKey.equals(accountIdentity)) controllerHighwater = "";
            accountIdentity = accountKey;
            String next = accountKey.isEmpty() ? "" : accountKey + "#" + (++generation);
            identity = next;
            host.publishIngressState(() -> projection = new JSONObject());
            changed.run();
            publishProjection();
            if (next.isEmpty()) return;
            Backend captured = backendFactory.get();
            backend = captured;
            restoring = true;
            String owner = account.optString("owner_id");
            friendFingerprint = account.optJSONArray("friends").toString();
            observer = new Backend.Observer() {
                @Override public void projection(JSONObject value) {
                    if (!owns(next, captured)) return;
                    if (value.optBoolean("reactivate")) { needsActivation = true; updateAccount(); }
                    else accept(value, true);
                }
                @Override public void delivery(JSONObject value) {
                    if (owns(next, captured)) host.delivery(copy(value), captured);
                    else acknowledge(captured, value);
                }
                @Override public void warning(String value) { if (owns(next, captured)) host.warning(value); }
                @Override public boolean historyTaskKnown(JSONObject task) {
                    return consumedHistoryTasks.contains(historyTaskKey(task));
                }
            };
            captured.observe(observer);
            JSONObject activate = copy(account);
            put(activate, "action", "activate");
            captured.request(activate, (value, error) -> {
                if (!owns(next, captured)) return;
                if (error != null || value == null) {
                    captured.clearObserver(observer); backend = null; identity = ""; needsActivation = true; restoring = false; changed.run(); return;
                }
                if (!accept(value, false)) { needsActivation = true; restoring = false; return; }
                // activate 等待 worker 时好友可能已更新；先发布最新朋友集，再排恢复绑定请求。
                updateAccount();
                JSONArray saved;
                try { saved = new JSONArray(preferences.getString(preferenceKey(account), "[]")); }
                catch (Exception invalid) { saved = new JSONArray(); }
                if (value.optBoolean("has_bindings")) { restoring = false; accept(value, true); }
                else {
                    // 持有旧恢复清单的副本；只让最后一次实际 Native 投影重新成为持久偏好。
                    preferences.edit().remove(preferenceKey(account)).apply();
                    restore(captured, next, saved, 0);
                }
            });
        } else if (account != null && backend != null) {
            String next = identity;
            String friends = account.optJSONArray("friends").toString();
            if (!friends.equals(friendFingerprint) && projection.has("scope")) {
                friendFingerprint = friends;
                JSONObject request = request("set_friends");
                put(request, "friends", account.optJSONArray("friends"));
                Backend captured = backend;
                captured.request(request, (value, error) -> { if (owns(next, captured) && error == null) accept(value, true); });
            }
        }
    }

    private void restore(Backend captured, String ownerIdentity, JSONArray saved, int index) {
        if (!owns(ownerIdentity, captured)) return;
        if (index >= saved.length()) {
            captured.request(request("list"), (value, error) -> {
                if (!owns(ownerIdentity, captured)) return;
                restoring = false;
                if (error == null) accept(value, true);
            });
            return;
        }
        JSONObject item = saved.optJSONObject(index);
        if (item == null || !host.current(projection.optJSONObject("scope"), item.optString("peer_id"))) {
            restore(captured, ownerIdentity, saved, index + 1); return;
        }
        JSONObject command = request("bind");
        put(command, "peer_id", item.optString("peer_id"));
        put(command, "session_id", item.optString("session_id"));
        JSONObject target = session(projection, item.optString("session_id"));
        if (target == null || target.optString("workspace_key").isEmpty()) {
            restore(captured, ownerIdentity, saved, index + 1); return;
        }
        put(command, "expected_workspace_key", target.optString("workspace_key"));
        captured.request(command, (value, error) -> {
            if (!owns(ownerIdentity, captured)) return;
            if (error == null && value != null) accept(value, false);
            restore(captured, ownerIdentity, saved, index + 1);
        });
    }

    private boolean owns(String expected, Backend captured) {
        return !closed && expected.equals(identity) && captured == backend && captured.ownsObserver(observer);
    }

    private boolean accept(JSONObject value, boolean save) {
        if (value == null || !value.optBoolean("ok") || !value.has("scope")) return false;
        Backend captured = backend;
        if (captured == null || !captured.publicationCurrent(value)) return false;
        JSONObject scope = value.optJSONObject("scope");
        if (!host.current(scope, "")) return false;
        String generation = scope.optString("controller_generation");
        if (!generation.matches("[1-9][0-9]*") || compareUnsigned(generation, controllerHighwater) < 0) return false;
        if (projection.has("scope") && !sameAuthority(projection, value)) return false;
        controllerHighwater = generation;
        JSONObject ownedProjection = copy(value);
        host.publishIngressState(() -> projection = ownedProjection);
        if (save && !restoring) {
            persistProjection(context, value, () -> captured == backend && captured.publicationCurrent(value)
                && host.current(scope, "") && sameAuthority(projection, value));
        }
        JSONArray warnings = value.optJSONArray("warnings");
        if (warnings != null) for (int index = 0; index < warnings.length(); index++) {
            JSONObject warning = warnings.optJSONObject(index); if (warning == null) continue;
            String code = warning.optString("code");
            host.warning(code.equals("input_timeout") ? "附件准备超时，本条消息未自动处理"
                : code.equals("delivery_timeout") ? "绑定回复交付未确认，未自动重发"
                : "消息未能加入绑定会话，请在创作中心查看");
        }
        changed.run();
        publishProjection();
        consumeHistoryTasks(value, captured);
        return true;
    }

    public JSONObject binding(String peer) {
        return binding(projection, peer);
    }
    private static JSONObject binding(JSONObject projection, String peer) {
        JSONArray bindings = projection.optJSONArray("bindings");
        if (bindings != null) for (int index = 0; index < bindings.length(); index++) {
            JSONObject item = bindings.optJSONObject(index);
            if (item != null && peer.equals(item.optString("peer_id"))) return copy(item);
        }
        return null;
    }

    public Gateway forPeer(String peer) {
        return new Gateway() {
            private JSONObject displayed = copy(projection);
            @Override public String peerId() { return peer; }
            @Override public void load(BiConsumer<JSONObject, String> completion) { mutate("list", "", null, completion); }
            @Override public void bind(String session, BiConsumer<JSONObject, String> completion) { mutate("bind", session, displayed, completion); }
            @Override public void bind(String session, JSONObject shown, BiConsumer<JSONObject, String> completion) {
                mutate("bind", session, copy(shown), completion);
            }
            @Override public void unbind(BiConsumer<JSONObject, String> completion) { mutate("unbind", "", displayed, completion); }
            @Override public AutoCloseable subscribe(Consumer<JSONObject> listener) {
                Consumer<JSONObject> owned = value -> listener.accept(value == null ? null : copy(value));
                projectionListeners.add(owned);
                owned.accept(projection.has("scope") ? projection : null);
                return () -> projectionListeners.remove(owned);
            }
            private void mutate(String action, String selectedSession, JSONObject expected, BiConsumer<JSONObject, String> completion) {
                updateAccount();
                Backend captured = backend;
                String ownerIdentity = identity;
                if (captured == null || !host.current(projection.optJSONObject("scope"), peer)) {
                    completion.accept(null, "登录或好友资料尚未准备好，请稍后重试"); return;
                }
                if (!action.equals("list") && !sameAuthority(expected, projection)) {
                    completion.accept(null, "会话状态已变化，请重新选择"); return;
                }
                JSONObject selected = null;
                if (action.equals("bind")) {
                    selected = session(expected, selectedSession);
                    JSONObject actual = session(projection, selectedSession);
                    if (selected == null || actual == null || selected.optString("workspace_key").isEmpty()
                            || !selected.optString("workspace_key").equals(actual.optString("workspace_key"))) {
                        completion.accept(null, "所选会话或目录已变化，请重新选择"); return;
                    }
                }
                if (action.equals("unbind")) takeover(binding(peer));
                JSONObject command = request(action.equals("list") ? projection : expected, action);
                put(command, "peer_id", peer);
                if (!selectedSession.isEmpty()) put(command, "session_id", selectedSession);
                if (selected != null) put(command, "expected_workspace_key", selected.optString("workspace_key"));
                captured.request(command, (value, error) -> {
                    if (!owns(ownerIdentity, captured) || !host.current(projection.optJSONObject("scope"), peer)) {
                        completion.accept(null, "登录账号或好友已变化"); return;
                    }
                    String failure = error;
                    if (failure == null && (value == null || !value.optBoolean("ok") || !captured.publicationCurrent(value)
                            || !host.current(value.optJSONObject("scope"), peer) || !sameAuthority(projection, value)))
                        failure = "会话绑定回执未确认，请重试";
                    JSONObject actual = value == null ? null : binding(value, peer);
                    if (failure == null && action.equals("bind") && (actual == null || !selectedSession.equals(actual.optString("session_id"))))
                        failure = "未获得所选会话的绑定回执";
                    if (failure == null && action.equals("unbind") && actual != null) failure = "解除绑定尚未确认";
                    if (failure == null) { accept(value, true); displayed = copy(value); }
                    completion.accept(failure == null ? copy(value) : null, failure);
                });
            }
        };
    }

    private void publishProjection() {
        JSONObject value = projection.has("scope") ? projection : null;
        for (Consumer<JSONObject> listener : projectionListeners) listener.accept(value == null ? null : copy(value));
    }

    private void consumeHistoryTasks(JSONObject value, Backend captured) {
        JSONArray tasks = historyTasks(value);
        for (int index = 0; index < tasks.length(); index++) {
            JSONObject task = tasks.optJSONObject(index);
            if (task == null || !sameAuthority(value, task) || !host.current(task.optJSONObject("scope"), task.optString("peer_id"))) continue;
            JSONObject route = binding(value, task.optString("peer_id"));
            if (route == null || !task.optString("session_id").equals(route.optString("session_id"))
                    || !task.optString("binding_epoch").equals(route.optString("epoch"))) continue;
            String id = historyTaskKey(task);
            if (task.optString("task_id").isEmpty() || !consumedHistoryTasks.add(id)) continue;
            while (consumedHistoryTasks.size() > 512) consumedHistoryTasks.remove(consumedHistoryTasks.iterator().next());
            JSONObject owned = copy(task);
            try {
                // accept 已经原子发布 provisional 投影，才能捕获其后/其前原始 SDK 入站的持久化栅栏。
                RemoteIMIngress.Fence fence = host.historyFence(owned);
                captured.historyTask(owned, fence);
            } catch (Exception failed) {
                try {
                    JSONObject failure = MaiSessionIMHistoryJob.request("history_failed", owned);
                    put(failure, "reason", "ingress_fence_failed");
                    captured.request(failure, (response, error) -> { if (response != null && error == null) accept(response, true); });
                } catch (Exception invalid) { /* 失效任务不能改用当前账号或新 task 重试。 */ }
                host.warning("本机历史准备失败，本次绑定未启用");
            }
        }
    }

    static JSONArray historyTasks(JSONObject value) {
        JSONArray result = new JSONArray(); java.util.HashSet<String> seen = new java.util.HashSet<>();
        JSONArray many = value.optJSONArray("history_tasks");
        if (many != null) for (int index = 0; index < many.length(); index++) {
            JSONObject task = many.optJSONObject(index);
            if (task != null && seen.add(historyTaskKey(task))) result.put(copy(task));
        }
        return result;
    }
    static String historyTaskKey(JSONObject task) {
        JSONArray key = new JSONArray().put(task.optString("bridge_id"));
        JSONObject scope = task.optJSONObject("scope");
        for (String field : new String[]{"sdk_app_id", "owner_id", "auth_generation", "controller_generation"})
            key.put(scope == null ? "" : scope.optString(field));
        return key.put(task.optString("task_id")).put(task.optString("binding_epoch")).toString();
    }

    private static JSONObject session(JSONObject projection, String id) {
        JSONArray sessions = projection == null ? null : projection.optJSONArray("sessions");
        if (sessions != null) for (int index = 0; index < sessions.length(); index++) {
            JSONObject item = sessions.optJSONObject(index);
            if (item != null && id.equals(item.optString("id"))) return item;
        }
        return null;
    }

    /** SDK 下载开始前调用；只复制投影。两个 future 仅汇合数据库新消息判断和媒体就绪，不排列路由任务。 */
    public TencentIMClient.LiveAdmission capture(String sender, String recipient, String messageId,
            RemoteIMOrigin origin, boolean control, boolean hasContent) {
        return captureAuthority().admit(sender, recipient, messageId, origin, control, hasContent);
    }

    public TencentIMClient.LiveAuthority captureAuthority() {
        Backend captured = backend; JSONObject current = projection; String ownerIdentity = identity;
        return (sender, recipient, messageId, origin, control, hasContent) ->
            capture(current, captured, ownerIdentity, sender, recipient, messageId, origin, control, hasContent);
    }

    private TencentIMClient.LiveAdmission capture(JSONObject current, Backend captured, String ownerIdentity,
            String sender, String recipient, String messageId, RemoteIMOrigin origin, boolean control, boolean hasContent) {
        JSONObject route = binding(current, sender);
        JSONObject scope = current.optJSONObject("scope");
        if (closed || captured == null || !owns(ownerIdentity, captured) || route == null || messageId.isEmpty() || control || !hasContent
                || origin != RemoteIMOrigin.HUMAN || scope == null || !recipient.equals(scope.optString("owner_id"))
                || !host.current(scope, sender)) return null;
        JSONObject admission = request(current, "receive");
        put(admission, "peer_id", sender); put(admission, "sender_id", sender); put(admission, "recipient_id", recipient);
        put(admission, "message_id", messageId); put(admission, "origin", origin.wireValue());
        put(admission, "live", true); put(admission, "group", false); put(admission, "control_frame", control);
        put(admission, "expected_binding_epoch", route.optString("epoch"));
        put(admission, "expected_session_id", route.optString("session_id"));
        put(admission, "has_content", hasContent);
        JSONObject reserve = copy(admission); put(reserve, "action", "admit");
        JSONObject admitted = captured.control(reserve);
        if (!admitted.optBoolean("ok") || !admitted.optBoolean("accepted") || admitted.optString("input_id").isEmpty()) return null;
        admission.remove("has_content");
        put(admission, "input_id", admitted.optString("input_id"));
        java.util.concurrent.atomic.AtomicBoolean abandoned = new java.util.concurrent.atomic.AtomicBoolean();
        java.util.concurrent.atomic.AtomicBoolean duplicate = new java.util.concurrent.atomic.AtomicBoolean();
        java.util.concurrent.atomic.AtomicBoolean recorded = new java.util.concurrent.atomic.AtomicBoolean();
        java.util.concurrent.atomic.AtomicReference<String> failureReason = new java.util.concurrent.atomic.AtomicReference<>("preparation_failed");
        Runnable abandon = () -> {
            if (duplicate.get()) return;
            if (!abandoned.compareAndSet(false, true)) return;
            JSONObject cancel = request(current, "abandon_input");
            put(cancel, "input_id", admitted.optString("input_id")); put(cancel, "reason", failureReason.get()); captured.control(cancel);
        };
        CompletableFuture<Boolean> inserted = new CompletableFuture<>();
        CompletableFuture<RemoteIMMessage> ready = new CompletableFuture<>();
        inserted.thenCombine(ready, (eligible, message) -> {
            if (eligible && owns(ownerIdentity, captured) && host.current(scope, sender)
                    && messageId.equals(message.remoteId()) && sender.equals(message.fromUserId())
                    && recipient.equals(message.toUserId()) && message.origin() == RemoteIMOrigin.HUMAN)
                captured.incoming(copy(admission), message.snapshot(), (value, error) -> {
                    if (error != null || value == null || !value.optBoolean("accepted")) abandon.run();
                });
            else abandon.run();
            return null;
        }).whenComplete((value, error) -> { if (error != null) abandon.run(); });
        return new TencentIMClient.LiveAdmission() {
            @Override public void persisted(boolean ignored) { /* UI 就绪/缓存合并不是 DB 首写回执，不能授予 live 资格。 */ }
            @Override public void metadataPersisted(RemoteIMIngress.Stored receipt) {
                if (!recorded.compareAndSet(false, true)) return;
                if (receipt == null || receipt.sourceRowId.isEmpty()
                        || !receipt.remoteId.isEmpty() && !messageId.equals(receipt.remoteId)) { persistenceFailed(); return; }
                JSONObject record = request(current, "persisted_input");
                put(record, "input_id", admitted.optString("input_id")); put(record, "inserted", receipt.inserted);
                put(record, "source_row_id", receipt.sourceRowId);
                if (!receipt.remoteId.isEmpty()) put(record, "remote_id", receipt.remoteId);
                JSONObject result = captured.control(record);
                if (!result.optBoolean("ok")) { abandoned("canceled"); return; }
                duplicate.set(!receipt.inserted);
                inserted.complete(receipt.inserted);
                if (!receipt.inserted) ready.cancel(false);
            }
            @Override public void mediaReady(RemoteIMMessage message) { ready.complete(message.snapshot()); }
            @Override public void abandoned() { inserted.complete(false); ready.cancel(false); abandon.run(); }
            @Override public void abandoned(String reason) { failureReason.set(reason); abandoned(); }
            @Override public void persistenceFailed() { abandoned("persistence_failed"); }
        };
    }

    private JSONObject request(String action) {
        return request(projection, action);
    }
    private static JSONObject request(JSONObject projection, String action) {
        JSONObject value = new JSONObject();
        put(value, "action", action); put(value, "bridge_id", projection.optString("bridge_id"));
        put(value, "scope", projection.optJSONObject("scope"));
        return value;
    }
    private void takeover(JSONObject route) {
        if (route == null || backend == null) return;
        JSONObject control = request("takeover"); put(control, "session_id", route.optString("session_id"));
        backend.control(control);
    }
    private void revoke() {
        Backend captured = backend;
        if (captured != null && captured.ownsObserver(observer)) {
            if (projection.has("scope")) captured.control(request("logout"));
            captured.clearObserver(observer);
        }
        backend = null;
    }
    private static void acknowledge(Backend captured, JSONObject delivery) {
        JSONObject request = new JSONObject(); put(request, "action", "ack"); put(request, "delivery", copy(delivery));
        captured.control(request);
    }
    @Override public void close() {
        if (closed) return; revoke(); closed = true; identity = ""; accountIdentity = "";
        host.publishIngressState(() -> projection = new JSONObject());
        publishProjection(); projectionListeners.clear();
    }
    static JSONObject copy(JSONObject value) {
        try { return value == null ? new JSONObject() : new JSONObject(value.toString()); }
        catch (Exception invalid) { throw new IllegalArgumentException("Invalid binding metadata", invalid); }
    }
    static String preferenceKey(JSONObject scope) { return scope.optString("sdk_app_id") + "|" + scope.optString("owner_id"); }
    static synchronized boolean persistProjection(Context context, JSONObject value, BooleanSupplier current) {
        if (!current.getAsBoolean() || !value.optBoolean("ok") || value.optJSONObject("scope") == null) return false;
        JSONArray saved = new JSONArray(), bindings = value.optJSONArray("bindings");
        if (bindings != null) for (int index = 0; index < bindings.length(); index++) {
            JSONObject row = bindings.optJSONObject(index); if (row == null) continue;
            if (!"ready".equals(row.optString("history_state"))) continue;
            JSONObject entry = new JSONObject(); put(entry, "peer_id", row.optString("peer_id"));
            put(entry, "session_id", row.optString("session_id")); saved.put(entry);
        }
        context.getSharedPreferences("session-im-bindings", Context.MODE_PRIVATE).edit()
            .putString(preferenceKey(value.optJSONObject("scope")), saved.toString()).apply();
        return true;
    }
    static synchronized void removeSessionPreference(Context context, JSONObject scope, String session, BooleanSupplier current) {
        if (scope == null || !current.getAsBoolean()) return;
        SharedPreferences preferences = context.getSharedPreferences("session-im-bindings", Context.MODE_PRIVATE);
        String key = preferenceKey(scope); JSONArray kept = new JSONArray();
        try {
            JSONArray existing = new JSONArray(preferences.getString(key, "[]"));
            for (int index = 0; index < existing.length(); index++) {
                JSONObject entry = existing.optJSONObject(index);
                if (entry != null && !session.equals(entry.optString("session_id"))) kept.put(entry);
            }
        } catch (Exception invalid) { /* 旧损坏偏好不能恢复为已授权绑定。 */ }
        preferences.edit().putString(key, kept.toString()).apply();
    }
    static boolean sameAuthority(JSONObject first, JSONObject second) {
        if (first == null || second == null || !first.optString("bridge_id").equals(second.optString("bridge_id"))) return false;
        JSONObject a = first.optJSONObject("scope"), b = second.optJSONObject("scope");
        if (a == null || b == null) return false;
        for (String key : new String[]{"sdk_app_id", "owner_id", "auth_generation", "controller_generation"})
            if (a.optString(key).isEmpty() || !a.optString(key).equals(b.optString(key))) return false;
        return true;
    }
    private static int compareUnsigned(String first, String second) {
        if (second.isEmpty()) return 1;
        return first.length() == second.length() ? first.compareTo(second) : Integer.compare(first.length(), second.length());
    }
    static void put(JSONObject target, String key, Object value) {
        try { target.put(key, value); }
        catch (Exception invalid) { throw new IllegalArgumentException("Invalid binding metadata", invalid); }
    }
}
