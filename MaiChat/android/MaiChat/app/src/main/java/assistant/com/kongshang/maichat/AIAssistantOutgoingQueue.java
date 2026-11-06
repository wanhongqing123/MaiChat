// 文件作用：仅持久保存 Android 尚未确认的点击快照；推理与工具队列仍走共享 C++ 核心。
package com.kongshang.maichat;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

final class AIAssistantOutgoingQueue {
    interface Storage {
        String read();
        boolean write(String value);
    }
    private final Storage storage;
    private JSONArray items = new JSONArray();
    private final java.util.LinkedHashSet<String> answeredQuestions = new java.util.LinkedHashSet<>();

    AIAssistantOutgoingQueue(Storage storage) throws IOException {
        this.storage = storage;
        try {
            String saved = storage.read();
            if (!saved.isEmpty()) items = new JSONArray(saved);
            boolean changed = false;
            JSONArray next = copy(items);
            for (int i = 0; i < next.length(); i++) {
                JSONObject entry = next.getJSONObject(i);
                if ((entry.getString("state").equals("dispatching") || entry.getString("state").equals("forwarded"))) {
                    // 进程退出不是失败收据；恢复时不能重放可能已经执行的 send/steer。
                    entry.put("state", "unknown"); changed = true;
                } else if (entry.getString("state").equals("preparing")) {
                    // preparing 尚未发送 command；上传中断不代表模型已受理，可保留原绑定继续准备。
                    entry.put("state", "queued"); changed = true;
                }
            }
            if (changed) commit(next);
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized String enqueue(String owner, JSONObject payload) throws IOException {
        try {
            if (items.length() >= 128 || owner.isEmpty()
                || payload.toString().getBytes(StandardCharsets.UTF_8).length > 128 * 1024)
                throw new IOException("发送队列已满或内容过长，草稿已保留");
            String id = UUID.randomUUID().toString();
            JSONArray next = copy(items);
            JSONObject capture = new JSONObject(payload.toString());
            if (capture.optString("operation").equals("answer") && !capture.optString("questionId").isEmpty()) {
                JSONArray candidates = capture.optJSONArray("questionCandidates");
                if (candidates == null) candidates = new JSONArray().put(capture.optJSONObject("question") != null
                    ? capture.getJSONObject("question") : new JSONObject().put("id", capture.getString("questionId")));
                JSONObject selected = null;
                for (int i = 0; i < candidates.length(); i++) {
                    JSONObject candidate = candidates.optJSONObject(i);
                    if (candidate == null || candidate.optString("id").isEmpty()) continue;
                    String question = candidate.getString("id");
                    boolean claimed = answeredQuestions.contains(owner + "\u001f" + question);
                    for (int j = 0; !claimed && j < next.length(); j++) {
                        JSONObject prior = next.getJSONObject(j), priorPayload = prior.getJSONObject("payload");
                        claimed = owner.equals(prior.optString("owner")) && question.equals(priorPayload.optString("questionId"));
                    }
                    if (!claimed) { selected = candidate; break; }
                }
                if (selected != null) capture.put("questionId", selected.getString("id"))
                    .put("question", new JSONObject(selected.toString()));
                else {
                    // 包括 ACK 已处理、UI 仍是旧快照的间隙；后续输入只能补充原轮次。
                    if (capture.optString("turnId").isEmpty())
                        throw new IOException("问题的回答尚未确认，后续内容已保留草稿");
                    capture.put("operation", "steer");
                    capture.remove("questionId");
                    capture.remove("question");
                }
            }
            capture.remove("questionCandidates");
            next.put(new JSONObject().put("id", id).put("owner", owner).put("state", "queued")
                .put("payload", capture));
            commit(next);
            return id;
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized JSONArray entries() throws IOException {
        try { return copy(items); } catch (Exception failure) { throw io(failure); }
    }

    synchronized JSONObject head(String owner) throws IOException {
        try {
            for (int i = 0; i < items.length(); i++) {
                JSONObject entry = items.getJSONObject(i);
                if (!owner.equals(entry.getString("owner"))) continue;
                String state = entry.getString("state");
                // These remote inputs already have independent delivery state;
                // their model/business receipts must not block later input.
                if (entry.getJSONObject("payload").optString("operation").equals("enqueue") &&
                    (state.equals("forwarded") || state.equals("unknown") || state.equals("failed"))) continue;
                return new JSONObject(entry.toString());
            }
            return null;
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized JSONObject entry(String id) throws IOException {
        try {
            for (int i = 0; i < items.length(); i++) {
                JSONObject entry = items.getJSONObject(i);
                if (id.equals(entry.getString("id"))) return new JSONObject(entry.toString());
            }
            return null;
        } catch (Exception failure) { throw io(failure); }
    }

    // Only owner-queued remote prompts can be resent across turn changes. The
    // same ID and prepared attachment references reach the owner's durable dedup.
    synchronized void retryInput(String id) throws IOException {
        try {
            JSONArray next = copy(items);
            for (int i = 0; i < next.length(); i++) {
                JSONObject entry = next.getJSONObject(i);
                if (!id.equals(entry.getString("id"))) continue;
                String state = entry.getString("state");
                if (!entry.getJSONObject("payload").optString("operation").equals("enqueue") ||
                    (!state.equals("unknown") && !state.equals("failed")))
                    throw new IOException("This input cannot be retried");
                entry.put("state", "queued"); commit(next); return;
            }
            throw new IOException("Input was already acknowledged");
        } catch (Exception failure) { throw io(failure); }
    }

    // 第一条空会话点击创建 Session 后，其他尚未派发的点击必须一起归到同一实际所有者。
    synchronized void retarget(String oldOwner, String newOwner, String session) throws IOException {
        try {
            JSONArray next = copy(items);
            for (int i = 0; i < next.length(); i++) {
                JSONObject entry = next.getJSONObject(i);
                if (!oldOwner.equals(entry.getString("owner"))) continue;
                if (!entry.getString("state").equals("queued")) throw new IOException("不能改绑已派发的内容");
                entry.put("owner", newOwner).getJSONObject("payload").put("session", session);
            }
            commit(next);
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized void transition(String id, String state, JSONObject payload) throws IOException {
        try {
            JSONArray next = copy(items);
            for (int i = 0; i < next.length(); i++) {
                JSONObject entry = next.getJSONObject(i);
                if (!id.equals(entry.getString("id"))) continue;
                String old = entry.getString("state");
                if (!(old.equals("queued") && (state.equals("preparing") || state.equals("dispatching") || state.equals("failed")))
                    && !(old.equals("preparing") && (state.equals("dispatching") || state.equals("failed")))
                    && !(old.equals("dispatching") && (state.equals("forwarded") || state.equals("unknown") || state.equals("failed")))
                    && !(old.equals("forwarded") && (state.equals("unknown") || state.equals("failed"))))
                    throw new IOException("发送条目状态已改变");
                entry.put("state", state);
                if (payload != null) entry.put("payload", new JSONObject(payload.toString()));
                commit(next); return;
            }
            throw new IOException("发送条目不存在");
        } catch (Exception failure) { throw io(failure); }
    }

    // answer 没有新 Core user message，必须用明确的 question_id 收据，不能伪造消息编号。
    synchronized void acceptAnswer(String id, String questionId) throws IOException {
        try {
            JSONArray next = new JSONArray();
            boolean removed = false;
            String answeredOwner = "";
            for (int i = 0; i < items.length(); i++) {
                JSONObject entry = items.getJSONObject(i);
                if (!id.equals(entry.optString("id"))) { next.put(new JSONObject(entry.toString())); continue; }
                JSONObject payload = entry.getJSONObject("payload");
                if (!entry.optString("state").equals("dispatching")
                    || !payload.optString("operation").equals("answer") || questionId.isEmpty()
                    || !questionId.equals(payload.optString("questionId"))
                    || !id.equals(head(entry.getString("owner")).optString("id")))
                    throw new IOException("回答收据与原问题不一致，内容已保留");
                removed = true;
                answeredOwner = entry.getString("owner");
            }
            if (!removed) throw new IOException("发送条目不存在");
            commit(next);
            answeredQuestions.add(answeredOwner + "\u001f" + questionId);
            while (answeredQuestions.size() > 128) answeredQuestions.remove(answeredQuestions.iterator().next());
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized void accept(String id, String messageId, String turnId) throws IOException {
        try {
            JSONObject original = entry(id);
            if (original == null) throw new IOException("发送条目不存在");
            boolean queuedInput = original.getJSONObject("payload").optString("operation").equals("enqueue");
            if (messageId.isEmpty() || (!queuedInput && turnId.isEmpty()))
                throw new IOException("缺少明确消息与轮次收据");
            // 拷贝后再改尾项；写入失败时，原队首和未绑定草稿均保持原状。
            JSONArray captured = copy(items);
            String acceptedOwner = "";
            for (int i = 0; i < captured.length(); i++) {
                JSONObject entry = captured.getJSONObject(i);
                if (!id.equals(entry.getString("id"))) continue;
                acceptedOwner = entry.getString("owner");
                if (!queuedInput && !id.equals(head(acceptedOwner).getString("id")))
                    throw new IOException("只能确认原所有者的队首内容");
            }
            JSONArray next = new JSONArray();
            boolean removed = false;
            for (int i = 0; i < captured.length(); i++) {
                JSONObject entry = captured.getJSONObject(i);
                if (id.equals(entry.getString("id"))) {
                    if (!queuedInput && !entry.getString("state").equals("dispatching"))
                        throw new IOException("不能移除未确认派发的内容");
                    removed = true;
                } else {
                    JSONObject payload = entry.getJSONObject("payload");
                    if (acceptedOwner.equals(entry.getString("owner")) && entry.getString("state").equals("queued")
                        && payload.optString("operation").isEmpty() && payload.optString("turnId").isEmpty())
                        payload.put("operation", "steer").put("turnId", turnId);
                    next.put(entry);
                }
            }
            if (!removed) throw new IOException("发送条目不存在");
            commit(next);
        } catch (Exception failure) { throw io(failure); }
    }

    // 仅明确未接受的 failed 条目可恢复；unknown 不能复制成新发送导致重复执行。
    synchronized JSONObject restore(String id) throws IOException {
        try {
            JSONArray next = new JSONArray();
            JSONObject payload = null;
            for (int i = 0; i < items.length(); i++) {
                JSONObject entry = items.getJSONObject(i);
                if (id.equals(entry.getString("id"))) {
                    if (!entry.getString("state").equals("failed"))
                        throw new IOException("送达未知的内容请先核对历史，不能重复发送");
                    payload = new JSONObject(entry.getJSONObject("payload").toString());
                } else next.put(entry);
            }
            if (payload == null) throw new IOException("发送条目不存在");
            commit(next); return payload;
        } catch (Exception failure) { throw io(failure); }
    }

    private void commit(JSONArray next) throws IOException {
        String serialized = next.toString();
        // 限制整份私有 prefs 的同步写放大；超限拒绝接收，输入框继续保有原稿。
        if (serialized.getBytes(StandardCharsets.UTF_8).length > 4 * 1024 * 1024
            || !storage.write(serialized)) throw new IOException("发送队列保存失败，草稿已保留");
        items = next;
    }
    private static JSONArray copy(JSONArray source) throws Exception { return new JSONArray(source.toString()); }
    private static IOException io(Exception error) {
        return error instanceof IOException ? (IOException) error : new IOException("发送队列无法读取", error);
    }
}
