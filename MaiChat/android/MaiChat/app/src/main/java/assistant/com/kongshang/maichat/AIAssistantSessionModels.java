// Thin projection of the shared session contract; no Java-side session persistence or provider routing.
package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONObject;
import java.util.LinkedHashMap;
import java.util.Map;

final class AIAssistantSessionModels {
    static final String LOCAL = "local:" + AIAssistantModelSettings.LOCAL_MODEL_ID;

    private AIAssistantSessionModels() { }

    interface NativeRequest { JSONObject invoke(JSONObject request) throws Exception; }
    interface LocalPreparation { void prepare(String model) throws Exception; }

    // Ordinary owner input boundary; JNI and immutable local asset resolution remain host seams.
    static JSONObject executeSessionRequest(JSONObject request, NativeRequest nativeRequest,
                                            LocalPreparation localPreparation) throws Exception {
        String operation = request.optString("op");
        if (operation.equals("enqueue") || operation.equals("send") || operation.equals("steer")) {
            String target = request.getString("session");
            JSONObject actual = nativeRequest.invoke(new JSONObject().put("op", "get_session")
                .put("session", target));
            if (!actual.optBoolean("ok") || !target.equals(actual.optString("session")))
                throw new IllegalStateException("原会话主模型不可用，内容未派发");
            String model = actual.getString("model");
            if (model.startsWith("local:")) localPreparation.prepare(model);
        }
        return nativeRequest.invoke(request);
    }

    // The composer handoff marker is the same one used by first-input session creation.
    static String createdDraftSession(String origin, boolean sameVisiblePage, String created,
                                     String previous) {
        return origin.isEmpty() && sameVisiblePage && !created.isEmpty() ? created : previous;
    }

    static JSONObject session(JSONObject data, JSONObject remote, String id) {
        if (remote != null && id.equals(remote.optString("id"))) return remote;
        JSONArray sessions = data.optJSONArray("sessions");
        if (sessions != null) for (int i = 0; i < sessions.length(); i++) {
            JSONObject item = sessions.optJSONObject(i);
            if (item != null && id.equals(item.optString("id"))) return item;
        }
        return new JSONObject();
    }

    static String model(JSONObject data, JSONObject remote, String id, String fallback) {
        String value = session(data, remote, id).optString("model");
        // Remote history/get_session identifies its own session explicitly; never apply another
        // selected page's top-level value to a queued target or catalog row.
        if (value.isEmpty() && !id.isEmpty() && id.equals(data.optString("session")))
            value = data.optString("model");
        return value.isEmpty() ? fallback : value;
    }

    static String effort(JSONObject data, JSONObject remote, String id, String fallback) {
        String value = session(data, remote, id).optString("reasoningEffort");
        if (value.isEmpty() && !id.isEmpty() && id.equals(data.optString("session")))
            value = data.optString("reasoningEffort");
        return value.isEmpty() ? fallback : value;
    }

    static JSONObject selection(String session, String model, String effort) throws Exception {
        if (!AIAssistantModelSettings.supportedCloud(model) && !LOCAL.equals(model))
            throw new IllegalArgumentException("请选择受支持的主模型");
        if (effort != null && !effort.isEmpty() && !effort.equals("low")
                && !effort.equals("high") && !effort.equals("max"))
            throw new IllegalArgumentException("思考强度只支持 low、high、max");
        JSONObject result = new JSONObject().put("op", session.isEmpty() ? "create" : "set_model")
            .put("session", session).put("model", model);
        if (effort != null) result.put("reasoningEffort", effort);
        return result;
    }

    static void validateReceipt(JSONObject receipt, String session, String requestedModel) throws Exception {
        if (!receipt.optBoolean("ok") || !requestedModel.equals(receipt.optString("model"))
            || (!session.isEmpty() && !session.equals(receipt.optString("session"))))
            throw new IllegalStateException(receipt.optString("error", "主模型回执与原会话不匹配"));
    }

    /** Per-account viewer cache. Only a correlated owner ACK can replace a committed value. */
    static final class Remote {
        static final class Ticket {
            final JSONObject owner;
            final String key, model, effort;
            final long serial;
            Ticket(JSONObject owner, String key, String model, String effort, long serial) throws Exception {
                this.owner = new JSONObject(owner.toString());
                this.key = key; this.model = model; this.effort = effort; this.serial = serial;
            }
        }
        private final Map<String, JSONObject> committed = new LinkedHashMap<>();
        private final Map<String, Ticket> active = new LinkedHashMap<>();
        private long serial;

        static String key(JSONObject owner) {
            return new JSONArray().put(owner.optString("device_slot"))
                .put(owner.optString("login_session_id")).put(owner.optString("id")).toString();
        }

        static boolean matchesOwner(JSONObject owner, JSONObject envelope) {
            return !owner.optString("id").isEmpty() && !owner.optString("device_slot").isEmpty()
                && !owner.optString("login_session_id").isEmpty()
                && owner.optString("id").equals(envelope.optString("session_id"))
                && owner.optString("device_slot").equals(envelope.optString("from_slot"))
                && owner.optString("login_session_id").equals(envelope.optString("from_login_session_id"));
        }

        static JSONObject request(String model, String effort) throws Exception {
            JSONObject result = new JSONObject().put("action", model == null ? "get_session" : "set_model");
            if (model != null) {
                if (!AIAssistantModelSettings.supportedCloud(model))
                    throw new IllegalArgumentException("请选择受支持的云端主模型");
                selection("remote", model, effort);
                result.put("model", model);
                if (effort != null) result.put("reasoningEffort", effort);
            }
            return result;
        }

        Ticket begin(JSONObject owner, String model) throws Exception {
            return begin(owner, model, null);
        }
        Ticket begin(JSONObject owner, String model, String effort) throws Exception {
            if (owner.optString("id").isEmpty() || owner.optString("device_slot").isEmpty()
                || owner.optString("login_session_id").isEmpty())
                throw new IllegalArgumentException("所属设备会话身份不完整");
            if (model != null && !AIAssistantModelSettings.supportedCloud(model))
                throw new IllegalArgumentException("请选择受支持的云端主模型");
            String key = key(owner);
            if (!active.containsKey(key) && active.size() >= 256)
                throw new IllegalStateException("等待所属设备的操作过多，请稍后重试");
            Ticket request = new Ticket(owner, key, model, effort, ++serial);
            active.put(key, request);
            return request;
        }

        boolean changing(JSONObject owner) {
            Ticket ticket = active.get(key(owner));
            return ticket != null && ticket.model != null;
        }
        boolean pending(JSONObject owner) { return active.containsKey(key(owner)); }

        boolean current(Ticket ticket) { return active.get(ticket.key) == ticket; }
        void finish(Ticket ticket) { if (current(ticket)) active.remove(ticket.key); }

        boolean accept(Ticket ticket, JSONObject result) throws Exception {
            if (!current(ticket)) return false;
            try {
                if (ticket.model != null) validateReceipt(result, ticket.owner.optString("id"), ticket.model);
                if (!result.optBoolean("ok") || !ticket.owner.optString("id").equals(result.optString("session")))
                    throw new IllegalStateException(result.optString("error", "所属设备模型回执与原会话不匹配"));
                String model = result.optString("model"), wire = result.optString("wire");
                Object effort = result.opt("reasoningEffort");
                if (ticket.effort != null && !ticket.effort.isEmpty() && !ticket.effort.equals(effort))
                    throw new IllegalStateException("所属设备思考强度回执与原选择不匹配");
                if ((!AIAssistantModelSettings.supportedCloud(model) && !LOCAL.equals(model))
                    || !(effort instanceof String) || (!effort.equals("") && !effort.equals("low")
                        && !effort.equals("high") && !effort.equals("max"))
                    || !(wire.equals("chat_completions") || wire.equals("responses") || wire.equals("local")))
                    throw new IllegalStateException("所属设备未返回完整主模型配置");
                committed.put(ticket.key, new JSONObject().put("model", model)
                    .put("reasoningEffort", effort).put("wire", wire));
                while (committed.size() > 256) committed.remove(committed.keySet().iterator().next());
                return true;
            } finally { finish(ticket); }
        }

        JSONObject project(JSONObject owner) throws Exception {
            JSONObject result = new JSONObject(owner.toString());
            JSONObject saved = committed.get(key(owner));
            if (saved != null) for (String field : new String[]{"model", "reasoningEffort", "wire"})
                result.put(field, saved.get(field));
            return result;
        }
    }
}
