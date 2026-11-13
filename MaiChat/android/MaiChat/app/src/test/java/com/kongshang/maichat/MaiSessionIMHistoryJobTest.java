// 文件作用：用 owned Native 接口替身验证历史 I/O 协调，不查询任何数据库或实现 Core 导入。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public class MaiSessionIMHistoryJobTest {
    @Test public void busyOpenRetriesAndMetadataWaitRetriesTheExactPageBeforeReadingAnother() throws Exception {
        JSONObject task = task(); String immutable = task.toString();
        AtomicInteger opens = new AtomicInteger(), reads = new AtomicInteger(), closes = new AtomicInteger(), chunks = new AtomicInteger();
        List<String> retried = new ArrayList<>();
        JSONObject result = MaiSessionIMHistoryJob.run(task, fence(), new AIAssistantPreparationJobs.Cancellation(), () -> true,
            request -> {
                String action = request.getString("action");
                if (action.equals("open")) return opens.incrementAndGet() == 1
                    ? new JSONObject().put("ok", false).put("errorCode", "busy") : new JSONObject().put("ok", true).put("snapshot_id", "snapshot");
                if (action.equals("close")) { closes.incrementAndGet(); return new JSONObject().put("ok", true); }
                int index = reads.incrementAndGet();
                return new JSONObject().put("ok", true).put("snapshot_id", "snapshot").put("sequence", Integer.toString(index - 1))
                    .put("records", new JSONArray()).put("has_more", index == 1);
            }, request -> {
                try {
                    assertEquals(immutable, request.getJSONObject("task").toString());
                    if (request.getString("action").equals("history_opened")) return complete(new JSONObject().put("ok", true));
                    retried.add(request.getJSONObject("page").toString());
                    int index = chunks.incrementAndGet();
                    if (index == 1) {
                        assertEquals(1, reads.get());
                        return complete(new JSONObject().put("ok", true).put("page_accepted", false).put("reason", "awaiting_ingress_metadata"));
                    }
                    return complete(new JSONObject().put("ok", true).put("page_accepted", true)
                        .put("history_progress", new JSONObject().put("committed", false)));
                } catch (Exception invalid) { throw new AssertionError(invalid); }
            }, deadline());
        assertEquals(2, opens.get()); assertEquals(2, reads.get()); assertEquals(3, chunks.get());
        assertEquals(retried.get(0), retried.get(1)); assertEquals(1, closes.get());
        assertFalse(result.getJSONObject("history_progress").getBoolean("committed"));
        assertEquals(immutable, task.toString());
    }

    @Test public void sourceErrorAlwaysClosesOwnedSnapshotAndNeverSubmitsAnEmptySuccessPage() throws Exception {
        AtomicInteger close = new AtomicInteger(), chunk = new AtomicInteger();
        assertThrows(IOException.class, () -> MaiSessionIMHistoryJob.run(task(), fence(), new AIAssistantPreparationJobs.Cancellation(), () -> true,
            request -> {
                if (request.optString("action").equals("open")) return new JSONObject().put("ok", true).put("snapshot_id", "snapshot");
                if (request.optString("action").equals("close")) { close.incrementAndGet(); return new JSONObject().put("ok", true); }
                return new JSONObject().put("ok", false).put("errorCode", "source_error");
            }, request -> {
                if (request.optString("action").equals("history_chunk")) chunk.incrementAndGet();
                return complete(ok());
            }, deadline()));
        assertEquals(1, close.get()); assertEquals(0, chunk.get());
    }

    @Test public void scopeChangeAfterOpenClosesSnapshotAndCannotStageAChunk() throws Exception {
        AtomicBoolean current = new AtomicBoolean(true); AtomicInteger close = new AtomicInteger(), chunks = new AtomicInteger();
        assertThrows(java.util.concurrent.CancellationException.class, () -> MaiSessionIMHistoryJob.run(task(), fence(),
            new AIAssistantPreparationJobs.Cancellation(), current::get,
            request -> {
                if (request.optString("action").equals("open")) return new JSONObject().put("ok", true).put("snapshot_id", "snapshot");
                close.incrementAndGet(); return ok();
            }, request -> { current.set(false); if (request.optString("action").equals("history_chunk")) chunks.incrementAndGet(); return complete(ok()); }, deadline()));
        assertEquals(1, close.get()); assertEquals(0, chunks.get());
    }

    @Test public void metadataFenceFailureDoesNotOpenAnyReader() throws Exception {
        CompletableFuture<Void> failed = new CompletableFuture<>(); failed.completeExceptionally(new IOException("owned fixture persistence failed"));
        RemoteIMIngress.Fence fence = new RemoteIMIngress.Fence(new RemoteIMIngress.Scope(123, "owner", 0), 1, failed, () -> true);
        AtomicInteger opens = new AtomicInteger();
        assertThrows(java.util.concurrent.ExecutionException.class, () -> MaiSessionIMHistoryJob.run(task(), fence,
            new AIAssistantPreparationJobs.Cancellation(), () -> true,
            request -> { opens.incrementAndGet(); return ok(); }, request -> complete(ok()), deadline()));
        assertEquals(0, opens.get());
    }

    private static RemoteIMIngress.Fence fence() {
        return new RemoteIMIngress.Fence(new RemoteIMIngress.Scope(123, "owner", 0), 0, CompletableFuture.completedFuture(null), () -> true);
    }
    private static JSONObject task() throws Exception {
        return new JSONObject().put("bridge_id", "bridge").put("scope", new JSONObject().put("sdk_app_id", "123")
            .put("owner_id", "owner").put("auth_generation", "4").put("controller_generation", "5"))
            .put("peer_id", "peer").put("session_id", "session").put("binding_epoch", "1").put("import_id", "").put("task_id", "task-1");
    }
    private static JSONObject ok() { JSONObject result = new JSONObject(); MaiSessionIMBindingService.put(result, "ok", true); return result; }
    private static CompletableFuture<JSONObject> complete(JSONObject value) { return CompletableFuture.completedFuture(value); }
    private static long deadline() { return System.nanoTime() + TimeUnit.SECONDS.toNanos(5); }
}
