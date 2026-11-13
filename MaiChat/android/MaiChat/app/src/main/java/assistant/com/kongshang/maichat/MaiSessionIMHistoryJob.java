// 文件作用：现有 preparation worker 上的 Native 快照 I/O 交接；不实现 SQL、Core 导入或路由队列。
package com.kongshang.maichat;

import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;

final class MaiSessionIMHistoryJob {
    interface SnapshotIO { JSONObject call(JSONObject request) throws Exception; }
    interface Requests { CompletableFuture<JSONObject> call(JSONObject request); }

    static JSONObject run(JSONObject task, RemoteIMIngress.Fence fence, AIAssistantPreparationJobs.Cancellation cancellation,
            BooleanSupplier current, SnapshotIO snapshots, Requests requests, long deadline) throws Exception {
        JSONObject owned = MaiSessionIMBindingService.copy(task);
        String snapshot = "";
        try {
            fence.await().get(remaining(deadline), TimeUnit.MILLISECONDS);
            check(cancellation, current, fence, deadline);
            while (snapshot.isEmpty()) {
                JSONObject opened = snapshots.call(new JSONObject().put("action", "open").put("task", owned));
                if (opened.optBoolean("ok")) {
                    snapshot = opened.optString("snapshot_id");
                    if (snapshot.isEmpty()) throw new IOException("Native 快照未返回身份");
                } else if (opened.optString("errorCode").equals("busy")) {
                    pause(cancellation, current, fence, deadline);
                } else throw new IOException("本机历史快照打开失败");
            }
            JSONObject start = request("history_opened", owned).put("snapshot_id", snapshot);
            requireOk(requests.call(start).get(remaining(deadline), TimeUnit.MILLISECONDS));
            while (true) {
                check(cancellation, current, fence, deadline);
                JSONObject page = snapshots.call(new JSONObject().put("action", "read").put("snapshot_id", snapshot));
                requireOk(page);
                if (!snapshot.equals(page.optString("snapshot_id")) || !page.has("has_more"))
                    throw new IOException("Native 历史页身份不一致");
                JSONObject chunk = request("history_chunk", owned).put("page", page);
                JSONObject accepted;
                while (true) {
                    check(cancellation, current, fence, deadline);
                    accepted = requests.call(MaiSessionIMBindingService.copy(chunk)).get(remaining(deadline), TimeUnit.MILLISECONDS);
                    requireOk(accepted);
                    if (accepted.optBoolean("page_accepted")) break;
                    if (!accepted.optString("reason").equals("awaiting_ingress_metadata")) throw new IOException("历史页未被接受");
                    // 保留整个原页重试；不能在元数据未知时读下一页或修改 sequence。
                    pause(cancellation, current, fence, deadline);
                }
                if (!page.getBoolean("has_more")) return accepted;
            }
        } finally {
            if (!snapshot.isEmpty()) {
                try { snapshots.call(new JSONObject().put("action", "close").put("snapshot_id", snapshot)); }
                catch (Exception closed) { /* 已被 scope 失效关闭的 owned reader 不回退为另一快照。 */ }
            }
        }
    }

    static JSONObject request(String action, JSONObject task) throws Exception {
        return new JSONObject().put("action", action).put("bridge_id", task.get("bridge_id"))
            .put("scope", task.get("scope")).put("task", MaiSessionIMBindingService.copy(task));
    }
    private static void requireOk(JSONObject value) throws IOException {
        if (value != null && !value.optBoolean("ok") && value.optString("errorCode").equals("canceled"))
            throw new java.util.concurrent.CancellationException("历史任务已取消");
        if (value == null || !value.optBoolean("ok")) throw new IOException("本机历史操作失败");
    }
    private static long remaining(long deadline) throws TimeoutException {
        long left = deadline - System.nanoTime();
        if (left <= 0) throw new TimeoutException("本机历史导入超时");
        return Math.max(1, TimeUnit.NANOSECONDS.toMillis(left));
    }
    private static void check(AIAssistantPreparationJobs.Cancellation cancellation, BooleanSupplier current,
            RemoteIMIngress.Fence fence, long deadline) throws TimeoutException {
        cancellation.check(); remaining(deadline);
        if (!current.getAsBoolean() || !fence.isCurrent()) throw new java.util.concurrent.CancellationException("历史导入身份已变化");
    }
    private static void pause(AIAssistantPreparationJobs.Cancellation cancellation, BooleanSupplier current,
            RemoteIMIngress.Fence fence, long deadline) throws Exception {
        check(cancellation, current, fence, deadline); Thread.sleep(Math.min(100, remaining(deadline)));
    }
}
