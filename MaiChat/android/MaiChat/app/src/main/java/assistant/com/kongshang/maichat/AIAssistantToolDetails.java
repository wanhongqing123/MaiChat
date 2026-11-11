// 文件作用：将按需工具详情的请求及缓存绑定到原账号、所有者、调用和修订。
package com.kongshang.maichat;

import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

/** Worker-confined read state. It never enters the durable prompt/command outbox. */
final class AIAssistantToolDetails {
    static final String[] FIELDS = {"input", "output", "error"};
    private static final int PAGE_BYTES = 4096, MAX_PARTS = 24, MAX_FIELD_CHARS = 64 * 1024;
    private String scope = "", session = "";
    private JSONObject owner;
    private long selection;
    private final LinkedHashMap<String, Part> parts = new LinkedHashMap<>(16, .75f, true);
    private final Map<String, Ticket> pending = new LinkedHashMap<>();

    static final class Ticket {
        final JSONObject request, owner;
        final String key, call, tool, scope;
        final long selection;
        String command = "";
        Ticket(JSONObject request, JSONObject owner, String key, Part part, String scope, long selection) {
            this.request = request; this.owner = copy(owner); this.key = key;
            call = part.observedCall; tool = part.tool; this.scope = scope; this.selection = selection;
        }
    }
    private static final class Part {
        final String call, tool, revision, metadataState;
        String readRevision, observedCall, readState;
        boolean mayAdoptState;
        final JSONObject fields = new JSONObject();
        Part(JSONObject part) {
            call = part.optString("callId"); tool = part.optString("tool"); revision = part.optString("detailRevision");
            readRevision = revision;
            observedCall = call;
            metadataState = part.optString("state"); readState = metadataState;
            for (String field : FIELDS) put(fields, field, new JSONObject());
        }
    }
    static String key(String message, String part) { return message + "\u001f" + part; }
    static JSONObject copy(JSONObject source) {
        try { return source == null ? null : new JSONObject(source.toString()); }
        catch (Exception invalid) { throw new IllegalArgumentException("Invalid detail JSON", invalid); }
    }
    private static void put(JSONObject object, String key, Object value) {
        try { object.put(key, value); }
        catch (Exception invalid) { throw new IllegalArgumentException("Invalid detail value", invalid); }
    }
    private static String identity(String account, long generation, String session, JSONObject owner) {
        return new JSONArray().put(account).put(generation).put(session)
            .put(owner == null ? "local" : owner.optString("device_slot"))
            .put(owner == null ? "" : owner.optString("login_session_id")).toString();
    }
    void select(String account, long generation, String session, JSONObject owner) {
        String next = identity(account, generation, session, owner);
        if (next.equals(scope)) return;
        scope = next; this.session = session; this.owner = copy(owner);
        selection++; parts.clear(); pending.clear();
    }
    void clearPending() {
        selection++; pending.clear();
        for (Part part : parts.values()) for (String field : FIELDS) {
            JSONObject value = part.fields.optJSONObject(field);
            if (value.optBoolean("loading")) {
                put(value, "loading", false); put(value, "error", "详情读取已取消，可重试");
            }
        }
    }
    // Snapshot updates invalidate only an already-open call. A changed revision cannot retain
    // an old page even when the tool's running/completed state has not changed.
    void observe(String message, JSONObject part) {
        String key = key(message, part.optString("id"));
        Part old = parts.get(key);
        if (old != null && (!old.call.equals(part.optString("callId")) || !old.tool.equals(part.optString("tool"))
                || !old.revision.equals(part.optString("detailRevision")) || !old.metadataState.equals(part.optString("state")))) {
            parts.put(key, new Part(part));
            pending.values().removeIf(ticket -> ticket.key.equals(key));
        }
    }
    void observeMessages(JSONArray messages) {
        if (messages == null) return;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            JSONArray values = message == null ? null : message.optJSONArray("parts");
            if (values != null) for (int j = 0; j < values.length(); j++) {
                JSONObject part = values.optJSONObject(j);
                if (part != null && part.optString("kind").equals("tool")) observe(message.optString("id"), part);
            }
        }
    }
    Ticket begin(String message, JSONObject source, String field, boolean more, boolean retry) {
        if (message.isEmpty() || !source.optBoolean("detailsAvailable") || !validField(field)) return null;
        observe(message, source);
        String key = key(message, source.optString("id"));
        Part part = parts.computeIfAbsent(key, ignored -> new Part(source));
        while (parts.size() > MAX_PARTS) {
            String oldest = parts.keySet().iterator().next(); parts.remove(oldest);
            pending.values().removeIf(ticket -> ticket.key.equals(oldest));
        }
        JSONObject value = part.fields.optJSONObject(field);
        if (value.optBoolean("loading") || (!retry && (value.has("text") || value.has("error")))) return null;
        long offset = more && value.optBoolean("hasMore") ? value.optLong("nextOffset") : 0;
        JSONObject request = new JSONObject();
        put(request, "action", "get_tool_detail"); put(request, "requestId", UUID.randomUUID().toString());
        put(request, "messageId", message); put(request, "partId", source.optString("id"));
        put(request, "field", field); put(request, "offset", offset); put(request, "limit", PAGE_BYTES);
        put(request, "detailRevision", part.readRevision);
        nativeRequest(session, request);
        Ticket ticket = new Ticket(request, owner, key, part, scope, selection);
        pending.put(request.optString("requestId"), ticket);
        put(value, "loading", true); value.remove("error");
        return ticket;
    }
    void bind(Ticket ticket, String command) { if (current(ticket)) ticket.command = command; }
    boolean handles(String command) {
        for (Ticket ticket : pending.values()) if (ticket.command.equals(command)) return true;
        return false;
    }
    private boolean current(Ticket ticket) {
        return ticket != null && ticket.selection == selection && ticket.scope.equals(scope)
            && pending.get(ticket.request.optString("requestId")) == ticket && parts.containsKey(ticket.key);
    }
    private static boolean sameOwner(JSONObject expected, JSONObject envelope, String session) {
        return expected == null ? envelope == null : envelope != null
            && session.equals(envelope.optString("session_id"))
            && expected.optString("device_slot").equals(envelope.optString("from_slot"))
            && expected.optString("login_session_id").equals(envelope.optString("from_login_session_id"));
    }
    boolean accept(String command, JSONObject envelope, JSONObject response) {
        if (response == null) return false;
        Ticket ticket = pending.get(response.optString("requestId"));
        if (!current(ticket) || !ticket.command.equals(command) || !sameOwner(ticket.owner, envelope, session)
            || !(response.opt("ok") instanceof Boolean)) return false;
        JSONObject request = ticket.request;
        for (String field : new String[] {"requestId", "messageId", "partId", "field"})
            if (!(response.opt(field) instanceof String) || !request.optString(field).equals(response.optString(field))) return false;
        if (!session.equals(response.optString("sessionId")) || !integer(response.opt("offset"))
            || request.optLong("offset") != response.optLong("offset")) return false;
        Part part = parts.get(ticket.key);
        JSONObject value = part.fields.optJSONObject(request.optString("field"));
        if (!response.optBoolean("ok")) {
            if (response.optString("errorCode").isEmpty()) return false;
            pending.remove(request.optString("requestId")); put(value, "loading", false);
            put(value, "error", response.optString("errorCode").equals("busy")
                ? "工具详情已更新，请重新读取" : response.optString("error", "所属设备未返回详情，可重试"));
            if (response.optString("errorCode").equals("busy")) {
                // A revision covers every field. Do not mix a cached old input with a fresh
                // output, and do not repeat a stale revision forever when metadata arrives late.
                pending.values().removeIf(other -> other.key.equals(ticket.key));
                for (String name : FIELDS) {
                    JSONObject reset = new JSONObject();
                    put(reset, "error", "工具详情已更新，请重新读取");
                    put(part.fields, name, reset);
                }
                value = part.fields.optJSONObject(request.optString("field"));
                put(value, "error", "工具详情已更新，请重新读取");
                part.readRevision = "";
                part.readState = ""; part.mayAdoptState = true;
            }
            return true;
        }
        // Large optional call IDs are omitted from compact snapshots. In that case the
        // immutable message/part/revision tuple still identifies the call; learn its actual
        // ID from the first verified page and require it for every subsequent response.
        if (!(response.opt("callId") instanceof String)
            || (!ticket.call.isEmpty() && !ticket.call.equals(response.optString("callId")))
            || (!part.observedCall.isEmpty() && !part.observedCall.equals(response.optString("callId")))
            || !ticket.tool.equals(response.optString("tool"))
            || !validRevision(response.opt("detailRevision"))
            || (!part.readRevision.isEmpty() && !part.readRevision.equals(response.optString("detailRevision")))
            || !validState(response.opt("state"))) return false;
        if (!part.readState.equals(response.optString("state"))
            && !(part.mayAdoptState && part.readState.isEmpty() && request.optLong("offset") == 0)) return false;
        for (String field : new String[] {"sourceBytesRead", "totalBytes", "nextOffset"})
            if (!integer(response.opt(field))) return false;
        if (!(response.opt("hasMore") instanceof Boolean) || !(response.opt("textIsValidUtf8") instanceof Boolean)
            || !(response.opt("text") instanceof String)) return false;
        long offset = request.optLong("offset"), read = response.optLong("sourceBytesRead"), total = response.optLong("totalBytes");
        long next = response.optLong("nextOffset");
        boolean more = response.optBoolean("hasMore");
        if (read < 0 || read > PAGE_BYTES || total < 0 || offset > total || read > total - offset
            || (more ? read == 0 || next != offset + read || next >= total : next != 0 || offset + read != total)) return false;
        String text = response.optString("text");
        if (response.optBoolean("textIsValidUtf8")) {
            if (text.getBytes(StandardCharsets.UTF_8).length != read) return false;
        } else {
            try {
                byte[] raw = java.util.Base64.getDecoder().decode(response.optString("rawBase64"));
                if (raw.length != read) return false;
                text = "[非 UTF-8 原始字节，Base64]\n" + response.optString("rawBase64") + "\n";
            } catch (IllegalArgumentException invalid) { return false; }
        }
        String combined = (offset == 0 ? "" : value.optString("text")) + text;
        boolean prefixOmitted = offset != 0 && value.optBoolean("prefixOmitted");
        if (combined.length() > MAX_FIELD_CHARS) {
            int start = combined.length() - MAX_FIELD_CHARS;
            if (Character.isLowSurrogate(combined.charAt(start))) start++;
            combined = "[较早已加载内容已收起，可重新读取]\n" + combined.substring(start);
            prefixOmitted = true;
        }
        pending.remove(request.optString("requestId"));
        part.readRevision = response.optString("detailRevision");
        part.observedCall = response.optString("callId");
        part.readState = response.optString("state"); part.mayAdoptState = false;
        put(value, "text", combined); put(value, "loading", false); value.remove("error");
        put(value, "hasMore", more); put(value, "nextOffset", next); put(value, "totalBytes", total);
        put(value, "detailRevision", response.optString("detailRevision"));
        put(value, "prefixOmitted", prefixOmitted);
        return true;
    }
    boolean acceptCommandResult(JSONObject envelope, JSONObject payload) {
        if (payload == null || !payload.optString("type").equals("command_result")
            || !(payload.opt("command_id") instanceof String)) return false;
        JSONObject response = payload.optJSONObject("result");
        // command_id is authoritative. The optional old alias can agree with the flat
        // request binding, but may never redirect or consume a different pending read.
        if (payload.has("request_id") && (!(payload.opt("request_id") instanceof String)
            || response == null || !payload.optString("request_id").equals(response.optString("requestId")))) return false;
        return accept(payload.optString("command_id"), envelope, response);
    }
    void fail(Ticket ticket, String failure) {
        if (!current(ticket)) return;
        pending.remove(ticket.request.optString("requestId"));
        JSONObject value = parts.get(ticket.key).fields.optJSONObject(ticket.request.optString("field"));
        put(value, "loading", false); put(value, "error", failure);
    }
    JSONObject snapshot() {
        JSONObject result = new JSONObject();
        for (Map.Entry<String, Part> item : parts.entrySet()) {
            Part part = item.getValue();
            JSONObject value = copy(part.fields);
            put(value, "state", part.readState.isEmpty() ? part.metadataState : part.readState);
            put(value, "detailRevision", part.readRevision.isEmpty() ? part.revision : part.readRevision);
            put(result, item.getKey(), value);
        }
        return result;
    }
    static JSONObject visiblePart(JSONObject part, JSONObject cached) {
        if (part == null || cached == null || cached.optString("state").isEmpty()
            || !part.optString("kind").equals("tool")) return part;
        JSONObject projected = copy(part); put(projected, "state", cached.optString("state")); return projected;
    }
    static boolean validField(String field) { return field.equals("input") || field.equals("output") || field.equals("error"); }
    private static boolean validState(Object value) {
        return value instanceof String && java.util.Arrays.asList("pending", "running", "completed", "error", "canceled").contains(value);
    }
    private static boolean validRevision(Object value) {
        if (!(value instanceof String) || ((String) value).length() != 64) return false;
        // A Busy response opens adoption of another real snapshot, never an arbitrary
        // marker that could consume the pending read before its legitimate SHA arrives.
        String revision = (String) value;
        for (int i = 0; i < revision.length(); i++) {
            char digit = revision.charAt(i);
            if (!(digit >= '0' && digit <= '9') && !(digit >= 'a' && digit <= 'f')) return false;
        }
        return true;
    }
    static boolean hasInlineDetails(JSONObject part) {
        // A capability-bearing owner may retain only a safe static error summary. It must
        // not disable fetching the original input, output and error through the new API.
        return !part.optBoolean("detailsAvailable") && (part.has("input") || part.has("output") || part.has("error"));
    }
    static String displayText(JSONObject part, JSONObject cached) {
        // Older owners already inline these fields; displaying them must never rerun a tool.
        if (hasInlineDetails(part))
            return part.optString("input") + "\n" + part.optString("output") + "\n" + part.optString("error");
        if (!part.optBoolean("detailsAvailable")) return "所属设备未提供工具详情";
        StringBuilder text = new StringBuilder();
        if (!part.optString("error").isEmpty()) text.append("提示：").append(part.optString("error"));
        for (String field : FIELDS) {
            if (text.length() > 0) text.append("\n\n");
            text.append(fieldTitle(field)).append("\n");
            JSONObject value = cached == null ? null : cached.optJSONObject(field);
            if (value != null && value.optBoolean("loading") && !value.has("text")) text.append("正在读取所属设备…");
            else if (value == null || (!value.has("text") && !value.has("error"))) text.append("尚未加载，点击读取");
            else {
                if (value.has("text")) text.append(value.optString("text").isEmpty() ? "（空）" : value.optString("text"));
                if (value.has("error")) text.append(value.has("text") ? "\n" : "").append(value.optString("error"));
                else if (value.optBoolean("loading")) text.append("\n正在读取下一页…");
            }
        }
        return text.toString();
    }
    static String fieldTitle(String field) { return field.equals("input") ? "参数" : field.equals("output") ? "输出" : "错误"; }
    private static boolean integer(Object value) { return value instanceof Integer || value instanceof Long; }
    static JSONObject nativeRequest(String session, JSONObject action) {
        if (action == null || !action.optString("action").equals("get_tool_detail"))
            throw new IllegalArgumentException("Invalid tool detail action");
        for (Iterator<String> fields = action.keys(); fields.hasNext();) {
            String field = fields.next();
            if (!java.util.Arrays.asList("action", "requestId", "messageId", "partId", "field", "offset", "limit", "detailRevision").contains(field))
                throw new IllegalArgumentException("Unexpected tool detail field");
        }
        for (String field : new String[] {"requestId", "messageId", "partId"}) {
            if (!(action.opt(field) instanceof String) || action.optString(field).isEmpty() || action.optString(field).length() > 128)
                throw new IllegalArgumentException("Invalid tool detail identity");
        }
        if (session.isEmpty() || !validField(action.optString("field")) || !integer(action.opt("offset"))
            || action.optLong("offset") < 0 || !integer(action.opt("limit")) || action.optLong("limit") < 1
            || action.optLong("limit") > PAGE_BYTES || (action.has("detailRevision") && !(action.opt("detailRevision") instanceof String))
            || action.optString("detailRevision").length() > 128
            || (action.optLong("offset") > 0 && action.optString("detailRevision").isEmpty()))
            throw new IllegalArgumentException("Invalid tool detail page");
        JSONObject request = copy(action); request.remove("action");
        put(request, "op", "tool_detail"); put(request, "session", session);
        return request;
    }
    static JSONObject failure(String session, JSONObject action, String code, String error) {
        JSONObject result = new JSONObject();
        put(result, "ok", false); put(result, "sessionId", safeId(session));
        put(result, "errorCode", code != null && code.length() <= 64 && code.matches("[a-z_]+") ? code : "internal");
        // Even pre-CABI rejections are control messages: never reflect malformed peer JSON
        // or an arbitrary-size offset/ID through the error path.
        String message = error == null ? "详情读取失败" : error;
        int end = Math.min(2048, message.length());
        if (end > 0 && Character.isHighSurrogate(message.charAt(end - 1))) end--;
        put(result, "error", message.substring(0, end));
        for (String field : new String[] {"requestId", "messageId", "partId"})
            put(result, field, safeId(action == null ? null : action.opt(field)));
        String field = action == null || !(action.opt("field") instanceof String) ? "" : action.optString("field");
        put(result, "field", validField(field) ? field : "");
        Object offset = action == null ? null : action.opt("offset");
        long number = integer(offset) ? ((Number) offset).longValue() : -1;
        put(result, "offset", number >= 0 && number <= 9_007_199_254_740_991L ? number : 0);
        return result;
    }
    private static String safeId(Object value) {
        if (!(value instanceof String)) return "";
        String text = (String) value;
        return !text.isEmpty() && text.getBytes(StandardCharsets.UTF_8).length <= 128 ? text : "";
    }
}
