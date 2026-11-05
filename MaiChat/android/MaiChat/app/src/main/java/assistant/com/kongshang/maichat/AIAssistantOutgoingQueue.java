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

    AIAssistantOutgoingQueue(Storage storage) throws IOException {
        this.storage = storage;
        try {
            String saved = storage.read();
            if (!saved.isEmpty()) items = new JSONArray(saved);
            boolean changed = false;
            JSONArray next = copy(items);
            for (int i = 0; i < next.length(); i++) {
                JSONObject entry = next.getJSONObject(i);
                if (entry.getString("state").equals("dispatching")) {
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
            next.put(new JSONObject().put("id", id).put("owner", owner).put("state", "queued")
                .put("payload", new JSONObject(payload.toString())));
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
                if (owner.equals(entry.getString("owner"))) return new JSONObject(entry.toString());
            }
            return null;
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
                    && !(old.equals("dispatching") && (state.equals("unknown") || state.equals("failed"))))
                    throw new IOException("发送条目状态已改变");
                entry.put("state", state);
                if (payload != null) entry.put("payload", new JSONObject(payload.toString()));
                commit(next); return;
            }
            throw new IOException("发送条目不存在");
        } catch (Exception failure) { throw io(failure); }
    }

    synchronized void accept(String id, String messageId, String turnId) throws IOException {
        try {
            if (messageId.isEmpty() || turnId.isEmpty()) throw new IOException("缺少明确消息与轮次收据");
            // 拷贝后再改尾项；写入失败时，原队首和未绑定草稿均保持原状。
            JSONArray captured = copy(items);
            String acceptedOwner = "";
            for (int i = 0; i < captured.length(); i++) {
                JSONObject entry = captured.getJSONObject(i);
                if (!id.equals(entry.getString("id"))) continue;
                acceptedOwner = entry.getString("owner");
                if (!id.equals(head(acceptedOwner).getString("id")))
                    throw new IOException("只能确认原所有者的队首内容");
            }
            JSONArray next = new JSONArray();
            boolean removed = false;
            for (int i = 0; i < captured.length(); i++) {
                JSONObject entry = captured.getJSONObject(i);
                if (id.equals(entry.getString("id"))) {
                    if (!entry.getString("state").equals("dispatching"))
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
