// 文件作用：保存已读取的 Agent 消息页，在固定大小的原生快照与补页之间合并。
package com.kongshang.maichat;

import java.util.Map;
import java.util.TreeMap;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Worker-confined page state. Tickets isolate late callbacks, including leave-and-return. */
final class AIAssistantMessagePages {
    static final int LATEST_LIMIT = 8;
    static final int OLDER_LIMIT = 20;
    static final int REMOTE_OLDER_LIMIT = 8;
    static final class Ticket {
        final String owner, before;
        final long generation;
        private Ticket(String owner, long generation, String before) {
            this.owner = owner; this.generation = generation; this.before = before;
        }
    }
    private String owner = "";
    // 连续数据库页的边界不能取 loaded.firstKey：窗口外活动消息的增量不是一次历史补页。
    private String oldestPageCursor = "";
    // These fences belong to this exact selected owner/login/session, never to
    // a message's completion flag or a different device's wall clock.
    private String ownerIdleThroughAssistant = "";
    private long acceptedMailboxEnvelope;
    private long generation, revision;
    private boolean initialized, hasOlder, loading, olderVisited;
    private Ticket pending;
    private final TreeMap<String, JSONObject> loaded = new TreeMap<>();
    private final Map<String, String> encoded = new TreeMap<>();
    private final boolean isolateOldUpdates;
    private final java.util.LinkedHashMap<String, JSONObject> displayUpdates = new java.util.LinkedHashMap<>();
    private final Map<String, String> displayEncoded = new TreeMap<>();

    AIAssistantMessagePages() { this(false); }
    AIAssistantMessagePages(boolean isolateOldUpdates) { this.isolateOldUpdates = isolateOldUpdates; }
    int olderLimit() {
        // The remote lane receives complete WS responses, including temporary
        // image receipts and control state. Eight 95 KiB messages fit the shared
        // 1 MiB receive limit; local native history keeps its twenty-row page.
        return isolateOldUpdates ? REMOTE_OLDER_LIMIT : OLDER_LIMIT;
    }
    JSONArray displayUpdates() {
        JSONArray result = new JSONArray();
        for (JSONObject message : displayUpdates.values()) result.put(message);
        return result;
    }

    Ticket select(String nextOwner) {
        if (!owner.equals(nextOwner)) { owner = nextOwner; reset(); }
        return new Ticket(owner, generation, "");
    }
    void reset() {
        generation++; initialized = false; hasOlder = false; loading = false; olderVisited = false; pending = null;
        loaded.clear(); encoded.clear(); displayUpdates.clear(); displayEncoded.clear();
        oldestPageCursor = "";
        ownerIdleThroughAssistant = ""; acceptedMailboxEnvelope = 0;
    }
    boolean admitMailboxEnvelope(long envelope) {
        if (envelope <= 0) return true; // Legacy local fixture/transport without an inbox receipt.
        if (envelope <= acceptedMailboxEnvelope) return false;
        acceptedMailboxEnvelope = envelope;
        return true;
    }
    boolean isConfirmedEndedTurn(String initialTurn) {
        return !initialTurn.isEmpty() && !ownerIdleThroughAssistant.isEmpty()
            && initialTurn.compareTo(ownerIdleThroughAssistant) <= 0;
    }
    void observeOwnerIdle(JSONArray acceptedMessages) {
        // Called only after accepting explicit owner busy=false/turnId="".
        // Core creates each new initial assistant at turn start with monotonic IDs;
        // steering segments may finish earlier and never call this method themselves.
        if (acceptedMessages == null) return;
        JSONObject latest = null;
        for (int i = 0; i < acceptedMessages.length(); i++) {
            JSONObject message = acceptedMessages.optJSONObject(i);
            if (message != null && message.optString("role").equals("assistant") &&
                (latest == null || message.optString("id").compareTo(latest.optString("id")) > 0)) latest = message;
        }
        // Even with an explicit owner idle, an earlier completed segment is not
        // the frontier if this very snapshot still contains a later active one.
        if (latest == null || latest.optBoolean("active")) return;
        for (JSONObject part : partMap(latest).values())
            if (part.optString("kind").equals("tool") && toolPhase(part.optString("state")) < 2) return;
        if (latest.optString("id").compareTo(ownerIdleThroughAssistant) > 0)
            ownerIdleThroughAssistant = latest.optString("id");
    }
    boolean matches(Ticket ticket) {
        return ticket != null && owner.equals(ticket.owner) && generation == ticket.generation;
    }
    JSONObject snapshotRequest(String session, boolean force) throws JSONException {
        return new JSONObject().put("op", "snapshot").put("session", session).put("force", force)
            .put("messageLimit", LATEST_LIMIT).put("includeMessageUpdates", true);
    }
    Ticket beginOlder() {
        if (loading || !hasOlder || oldestPageCursor.isEmpty()) return null;
        loading = true;
        olderVisited = true;
        pending = new Ticket(owner, generation, oldestPageCursor);
        return pending;
    }
    JSONObject pageRequest(String session, Ticket ticket) throws JSONException {
        if (!matches(ticket) || ticket.before.isEmpty())
            throw new IllegalStateException("The historical page belongs to a different view");
        return new JSONObject().put("op", "messages_page").put("session", session)
            .put("before", ticket.before).put("limit", olderLimit());
    }
    boolean acceptSnapshot(Ticket ticket, JSONObject snapshot) throws JSONException {
        if (!matches(ticket)) return false;
        // 初始 history 请求可能晚于直播更新返回。它仍负责补齐连续历史窗口，
        // 但同 ID 的较新正文/工具终态必须保留，不能被请求开始时的旧 running 覆盖。
        boolean delayedInitialRead = initialized && snapshot.optBoolean("initialHistoryPage");
        JSONArray messages = snapshot.optJSONArray("messages");
        if (!initialized || (snapshot.optBoolean("initialHistoryPage") && !olderVisited)) {
            int count = messages == null ? 0 : messages.length();
            hasOlder = snapshot.optBoolean("hasOlderMessages", count >= LATEST_LIMIT);
            oldestPageCursor = oldestPageId(messages);
            if (isolateOldUpdates && snapshot.optBoolean("initialHistoryPage") && !oldestPageCursor.isEmpty()) {
                // 首次 history 到达前可能先收到旧 assistant 广播；它不能永久占据连续根窗口。
                for (String id : new java.util.ArrayList<>(loaded.keySet())) if (id.compareTo(oldestPageCursor) < 0) {
                    displayUpdates.put(id, loaded.remove(id));
                    displayEncoded.put(id, encoded.remove(id));
                }
                trimDisplayUpdates();
            }
            initialized = true;
        }
        merge(delayedInitialRead ? retainedInitialVersions(messages) : messages, false);
        JSONArray updates = snapshot.optJSONArray("messageUpdates");
        merge(delayedInitialRead ? retainedInitialVersions(updates) : updates, false);
        return true;
    }
    // 到达先后不是数据版本。迟到 history 可带来真正更新的完成态，也可带来旧进度。
    // 只在最新 assistant 窗口存在因果进展时采纳其运行态；从不凭单段 completed 推整轮 idle。
    boolean initialHistoryAdvancesCurrent(JSONObject snapshot, String activeTurn) {
        String latestAssistant = "", incomingAssistant = "";
        for (JSONObject row : loaded.values())
            if (row.optString("role").equals("assistant") && row.optString("id").compareTo(latestAssistant) > 0)
                latestAssistant = row.optString("id");
        if (latestAssistant.isEmpty()) return false;
        boolean advances = false;
        for (String field : new String[] {"messages", "messageUpdates"}) {
            JSONArray source = snapshot.optJSONArray(field);
            if (source == null) continue;
            for (int i = 0; i < source.length(); i++) {
                JSONObject row = source.optJSONObject(i);
                if (row == null) continue;
                String id = row.optString("id");
                JSONObject known = known(id);
                int progression = known == null ? 1 : messageProgression(row, known);
                if (progression < 0) return false;
                if (!row.optString("role").equals("assistant")) continue;
                if (id.compareTo(incomingAssistant) > 0) incomingAssistant = id;
                if (id.compareTo(latestAssistant) > 0 || id.equals(latestAssistant) && progression > 0)
                    advances = true;
                // 明确的 owner idle + 相同最新终结段才是确认；仍保护目录先公布、正文未到的新 turn。
                if (snapshot.has("busy") && !snapshot.optBoolean("busy") && id.equals(latestAssistant)
                    && row.optLong("completed") > 0 && !row.optBoolean("active")) advances = true;
            }
        }
        return advances && (activeTurn.isEmpty() || incomingAssistant.compareTo(activeTurn) >= 0);
    }
    private JSONObject known(String id) {
        JSONObject row = loaded.get(id);
        return row != null ? row : displayUpdates.get(id);
    }
    private JSONArray retainedInitialVersions(JSONArray source) {
        if (source == null) return null;
        JSONArray retained = new JSONArray();
        for (int i = 0; i < source.length(); i++) {
            JSONObject row = source.optJSONObject(i);
            if (row == null) continue;
            JSONObject known = known(row.optString("id"));
            retained.put(known != null && messageProgression(row, known) <= 0 ? known : row);
        }
        return retained;
    }
    // 同一消息的完成标记、追加 part、正文前缀及工具阶段是现有单调信息。
    // 不比较 hash 的字典序，也不把不同终态或非前缀改写猜成更新版本。
    private static int messageProgression(JSONObject next, JSONObject previous) {
        boolean newer = next.optLong("completed") > 0 && previous.optLong("completed") == 0;
        boolean older = next.optLong("completed") == 0 && previous.optLong("completed") > 0;
        Map<String, JSONObject> before = partMap(previous), after = partMap(next);
        for (String id : before.keySet()) if (!after.containsKey(id)) older = true;
        for (Map.Entry<String, JSONObject> item : after.entrySet()) {
            JSONObject was = before.get(item.getKey()), now = item.getValue();
            if (was == null) { newer = true; continue; }
            if (!now.optString("kind").equals(was.optString("kind"))) return -2;
            String kind = now.optString("kind");
            if (kind.equals("text")) {
                String a = now.optString("text"), b = was.optString("text");
                if (a.equals(b)) continue;
                if (a.startsWith(b)) newer = true;
                else if (b.startsWith(a)) older = true;
                else return -2;
            } else if (kind.equals("tool")) {
                int a = toolPhase(now.optString("state")), b = toolPhase(was.optString("state"));
                if (a < 0 || b < 0 || a == b && !now.optString("state").equals(was.optString("state"))) return -2;
                newer |= a > b; older |= a < b;
            } else if (kind.equals("image")) {
                JSONObject a = now.optJSONObject("remoteImage"), b = was.optJSONObject("remoteImage");
                if (a != null && b != null) {
                    newer |= a.optLong("generation") > b.optLong("generation");
                    older |= a.optLong("generation") < b.optLong("generation");
                    if (a.optLong("generation") == b.optLong("generation") &&
                        b.optString("state").equals("pending") && !a.optString("state").equals("pending")) newer = true;
                }
            }
        }
        if (newer && older) return -2;
        return newer ? 1 : older ? -1 : 0;
    }
    private static Map<String, JSONObject> partMap(JSONObject message) {
        Map<String, JSONObject> parts = new TreeMap<>();
        JSONArray source = message.optJSONArray("parts");
        if (source != null) for (int i = 0; i < source.length(); i++) {
            JSONObject part = source.optJSONObject(i);
            if (part != null && !part.optString("id").isEmpty()) parts.put(part.optString("id"), part);
        }
        return parts;
    }
    private static int toolPhase(String state) {
        return state.equals("pending") ? 0 : state.equals("running") ? 1
            : state.equals("completed") || state.equals("error") ? 2 : -1;
    }
    boolean acceptOlder(Ticket ticket, JSONArray messages) throws JSONException {
        if (!matches(ticket) || pending != ticket) return false;
        merge(messages, true);
        String oldest = oldestPageId(messages);
        if (!oldest.isEmpty()) oldestPageCursor = oldest;
        hasOlder = messages != null && messages.length() >= olderLimit();
        loading = false; pending = null;
        return true;
    }
    void failOlder(Ticket ticket) {
        if (!matches(ticket) || pending != ticket) return;
        loading = false; pending = null;
    }
    boolean hasOlder() { return hasOlder; }
    boolean loading() { return loading; }
    JSONArray messages() {
        JSONArray result = new JSONArray();
        for (JSONObject message : loaded.values()) result.put(message);
        return result;
    }
    private static String oldestPageId(JSONArray messages) {
        String oldest = "";
        if (messages != null) for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            String id = message == null ? "" : message.optString("id");
            if (!id.isEmpty() && (oldest.isEmpty() || id.compareTo(oldest) < 0)) oldest = id;
        }
        return oldest;
    }
    private void trimDisplayUpdates() {
        while (displayUpdates.size() > 20) {
            String oldest = displayUpdates.keySet().iterator().next();
            displayUpdates.remove(oldest); displayEncoded.remove(oldest);
        }
    }
    private void merge(JSONArray messages, boolean historical) throws JSONException {
        if (messages == null) return;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject item = messages.optJSONObject(i);
            if (item == null) continue;
            String id = item.optString("id");
            if (id.isEmpty() || (historical && loaded.containsKey(id))) continue;
            JSONObject current = known(id);
            if (!historical && current != null && messageProgression(item, current) == -1)
                item = current; // Proven older live data cannot erase an already accepted terminal/body/receipt.
            if (historical && displayUpdates.containsKey(id)) {
                // 真正的连续旧页到达才接管旁路；不能用较旧 history 覆盖已收到的新工具结果。
                item = displayUpdates.remove(id);
                displayEncoded.remove(id);
            }
            JSONObject copy = new JSONObject(item.toString());
            copy.remove("uiRevision");
            String value = copy.toString();
            if (isolateOldUpdates && !historical && !loaded.containsKey(id) && !oldestPageCursor.isEmpty()
                && id.compareTo(oldestPageCursor) < 0) {
                if (value.equals(displayEncoded.get(id))) continue;
                copy.put("uiRevision", ++revision);
                displayUpdates.remove(id);
                displayUpdates.put(id, copy); displayEncoded.put(id, value);
                trimDisplayUpdates();
                continue;
            }
            displayUpdates.remove(id); displayEncoded.remove(id);
            if (value.equals(encoded.get(id))) continue;
            // 工具正文只在消息实际改变时产生新版本，回收行的重复 bind 不再解析原始输出。
            copy.put("uiRevision", ++revision);
            encoded.put(id, value); loaded.put(id, copy);
        }
    }
}
