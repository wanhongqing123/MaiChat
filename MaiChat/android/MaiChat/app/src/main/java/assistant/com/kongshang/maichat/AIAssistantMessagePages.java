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
            .put("before", ticket.before).put("limit", OLDER_LIMIT);
    }
    boolean acceptSnapshot(Ticket ticket, JSONObject snapshot) throws JSONException {
        if (!matches(ticket)) return false;
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
        merge(messages, false);
        merge(snapshot.optJSONArray("messageUpdates"), false);
        return true;
    }
    boolean acceptOlder(Ticket ticket, JSONArray messages) throws JSONException {
        if (!matches(ticket) || pending != ticket) return false;
        merge(messages, true);
        String oldest = oldestPageId(messages);
        if (!oldest.isEmpty()) oldestPageCursor = oldest;
        hasOlder = messages != null && messages.length() >= OLDER_LIMIT;
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
