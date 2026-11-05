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

    Ticket select(String nextOwner) {
        if (!owner.equals(nextOwner)) { owner = nextOwner; reset(); }
        return new Ticket(owner, generation, "");
    }
    void reset() {
        generation++; initialized = false; hasOlder = false; loading = false; olderVisited = false; pending = null;
        loaded.clear(); encoded.clear();
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
    private void merge(JSONArray messages, boolean historical) throws JSONException {
        if (messages == null) return;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject item = messages.optJSONObject(i);
            if (item == null) continue;
            String id = item.optString("id");
            if (id.isEmpty() || (historical && loaded.containsKey(id))) continue;
            JSONObject copy = new JSONObject(item.toString());
            copy.remove("uiRevision");
            String value = copy.toString();
            if (value.equals(encoded.get(id))) continue;
            // 工具正文只在消息实际改变时产生新版本，回收行的重复 bind 不再解析原始输出。
            copy.put("uiRevision", ++revision);
            encoded.put(id, value); loaded.put(id, copy);
        }
    }
}
