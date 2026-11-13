// 文件作用：隔离附件 I/O 和 Agent 控制队列；取消与关闭屏障覆盖真正的文件写入生命周期。
package com.kongshang.maichat;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.LongSupplier;
import java.util.function.BooleanSupplier;
import java.io.InputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import org.json.JSONObject;

final class AIAssistantPreparationJobs {
    interface Work { JSONObject run(Cancellation cancellation) throws Exception; }
    interface Completion { void complete(JSONObject value, Exception failure); }
    static final class Cancellation {
        private final AtomicBoolean canceled = new AtomicBoolean();
        private final List<Runnable> actions = new ArrayList<>();
        boolean isCanceled() { return canceled.get(); }
        synchronized void onCancel(Runnable action) {
            if (canceled.get()) action.run(); else actions.add(action);
        }
        synchronized void cancel() {
            if (!canceled.compareAndSet(false, true)) return;
            for (Runnable action : actions) action.run();
            actions.clear();
        }
        void check() throws CancellationException {
            if (canceled.get()) throw new CancellationException("附件准备已取消，内容仍已保留");
        }
    }
    private static final class Job {
        final Cancellation cancellation = new Cancellation();
        final CompletableFuture<Void> finished = new CompletableFuture<>();
    }
    private final Executor control;
    private final ExecutorService io;
    private final Map<String, Job> active = new HashMap<>();
    private boolean closed;
    AIAssistantPreparationJobs(Executor control, ExecutorService io) { this.control = control; this.io = io; }

    /** 普通附件流的身份、协作式期限与关闭取消；只用于现有 I/O worker，绝不在控制队列读文件。 */
    static InputStream guard(InputStream input, Cancellation cancellation, BooleanSupplier current, long deadlineNanos) {
        cancellation.onCancel(() -> { try { input.close(); } catch (IOException ignored) { } });
        return new FilterInputStream(input) {
            private void check() throws IOException {
                cancellation.check();
                if (!current.getAsBoolean()) throw new CancellationException("附件所属会话已变化");
                if (System.nanoTime() >= deadlineNanos) throw new IOException("附件准备超时");
            }
            @Override public int read() throws IOException { check(); int value = in.read(); check(); return value; }
            @Override public int read(byte[] bytes, int offset, int count) throws IOException {
                check(); int value = in.read(bytes, offset, count); check(); return value;
            }
        };
    }

    synchronized boolean start(String owner, long epoch, LongSupplier currentEpoch, Work work, Completion completion) {
        if (closed || active.containsKey(owner) || active.size() >= 128) return false;
        Job job = new Job(); active.put(owner, job);
        io.execute(() -> {
            try {
                JSONObject result = null;
                Exception error = null;
                try { job.cancellation.check(); result = work.run(job.cancellation); job.cancellation.check(); }
                catch (Exception failure) { error = failure; }
                final JSONObject prepared = result;
                final Exception failed = error;
                synchronized (AIAssistantPreparationJobs.this) { if (closed) return; }
                try { control.execute(() -> {
                    Exception failure = failed;
                    synchronized (AIAssistantPreparationJobs.this) {
                        if (closed || active.get(owner) != job) return;
                        active.remove(owner);
                        boolean canceled = job.cancellation.isCanceled();
                        if (canceled || currentEpoch.getAsLong() != epoch) {
                            job.cancellation.cancel();
                            failure = new CancellationException(canceled
                                ? "附件准备已取消，内容仍已保留" : "原会话已改变，附件未派发");
                        }
                    }
                    completion.complete(prepared, failure);
                }); } catch (RejectedExecutionException stopped) {
                    job.cancellation.cancel();
                    synchronized (AIAssistantPreparationJobs.this) { active.remove(owner, job); }
                }
            } finally { job.finished.complete(null); }
        });
        return true;
    }
    synchronized void cancel(String owner) {
        Job job = active.get(owner);
        if (job != null) job.cancellation.cancel();
    }
    /** 只取消同一普通附件所有者组，不保存或排列任何 IM 路由任务。 */
    synchronized void cancelPrefix(String prefix) {
        for (Map.Entry<String, Job> item : active.entrySet())
            if (item.getKey().startsWith(prefix)) item.getValue().cancellation.cancel();
    }
    synchronized CompletableFuture<Void> close() {
        closed = true;
        List<CompletableFuture<Void>> futures = new ArrayList<>();
        for (Job job : active.values()) { job.cancellation.cancel(); futures.add(job.finished); }
        // 让尚未开始的任务观察 canceled 后完成屏障；shutdownNow 会遗失排队任务的完成信号。
        io.shutdown();
        return CompletableFuture.allOf(futures.toArray(new CompletableFuture<?>[0]));
    }
}
