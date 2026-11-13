// 文件作用：确定性门控附件 I/O，验证停止/新输入不等待上传，以及旧 epoch 不能继续派发。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantPreparationJobsTest {
    @Test public void guardedLocalCopyKeepsControlResponsiveAndPrefixCancelClosesTheSource() throws Exception {
        ExecutorService control = Executors.newSingleThreadExecutor(), io = Executors.newSingleThreadExecutor();
        AIAssistantPreparationJobs jobs = new AIAssistantPreparationJobs(control, io);
        CountDownLatch reading = new CountDownLatch(1), closed = new CountDownLatch(1), tick = new CountDownLatch(1), completed = new CountDownLatch(1);
        AtomicInteger submitted = new AtomicInteger();
        java.io.InputStream blocked = new java.io.InputStream() {
            @Override public int read() throws java.io.IOException {
                reading.countDown();
                try { if (!closed.await(3, TimeUnit.SECONDS)) throw new java.io.IOException("fixture read timed out"); }
                catch (InterruptedException canceled) { throw new java.io.IOException(canceled); }
                throw new java.io.IOException("fixture stream closed");
            }
            @Override public void close() { closed.countDown(); }
        };
        try {
            jobs.start("session-im:session:1", 1, () -> 1, cancel -> {
                try (java.io.InputStream input = AIAssistantPreparationJobs.guard(blocked, cancel, () -> true,
                        System.nanoTime() + TimeUnit.SECONDS.toNanos(30))) { input.read(); }
                return new JSONObject();
            }, (value, failure) -> { if (failure == null) submitted.incrementAndGet(); completed.countDown(); });
            assertTrue(reading.await(2, TimeUnit.SECONDS));
            control.execute(tick::countDown); assertTrue(tick.await(500, TimeUnit.MILLISECONDS));
            jobs.cancelPrefix("session-im:session:");
            assertTrue(closed.await(500, TimeUnit.MILLISECONDS)); assertTrue(completed.await(2, TimeUnit.SECONDS));
            assertEquals(0, submitted.get());
        } finally { blocked.close(); jobs.close().get(2, TimeUnit.SECONDS); control.shutdownNow(); }
    }

    @Test public void expiredPreparationDeadlineCannotReadOrMarkMediaReady() throws Exception {
        AtomicInteger reads = new AtomicInteger();
        java.io.InputStream source = new java.io.InputStream() { @Override public int read() { reads.incrementAndGet(); return -1; } };
        try (java.io.InputStream guarded = AIAssistantPreparationJobs.guard(source,
                new AIAssistantPreparationJobs.Cancellation(), () -> true, System.nanoTime() - 1)) {
            assertThrows(java.io.IOException.class, guarded::read); assertEquals(0, reads.get());
        }
    }

    @Test public void changedPreparationOwnerCannotReadTheSource() throws Exception {
        AtomicInteger reads = new AtomicInteger();
        java.io.InputStream source = new java.io.InputStream() { @Override public int read() { reads.incrementAndGet(); return -1; } };
        try (java.io.InputStream guarded = AIAssistantPreparationJobs.guard(source,
                new AIAssistantPreparationJobs.Cancellation(), () -> false, Long.MAX_VALUE)) {
            assertThrows(CancellationException.class, guarded::read); assertEquals(0, reads.get());
        }
    }
    @Test public void blockedUploadDoesNotBlockStopOrAnotherInputOnTheControlQueue() throws Exception {
        ExecutorService control = Executors.newSingleThreadExecutor(), io = Executors.newFixedThreadPool(2);
        AIAssistantPreparationJobs jobs = new AIAssistantPreparationJobs(control, io);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        CountDownLatch stopped = new CountDownLatch(1), nextInput = new CountDownLatch(1);
        AtomicInteger nativeDispatches = new AtomicInteger();
        try {
            assertTrue(jobs.start("owner-one", 1, () -> 1, cancel -> {
                entered.countDown(); release.await(); return new JSONObject();
            }, (value, failure) -> { if (failure == null) nativeDispatches.incrementAndGet(); }));
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            control.execute(stopped::countDown); control.execute(nextInput::countDown);
            assertTrue("Stop must run while upload is still gated", stopped.await(500, TimeUnit.MILLISECONDS));
            assertTrue("New intake must not wait for the old upload", nextInput.await(500, TimeUnit.MILLISECONDS));
            assertFalse("One owner must not upload the same checkpoint concurrently",
                jobs.start("owner-one", 1, () -> 1, cancel -> new JSONObject(), (value, failure) -> { }));
            jobs.cancel("owner-one"); release.countDown();
            jobs.close().get(2, TimeUnit.SECONDS);
            assertEquals(0, nativeDispatches.get());
        } finally { release.countDown(); jobs.close().get(2, TimeUnit.SECONDS); control.shutdownNow(); }
    }

    @Test public void completionFromAnOldEpochCannotInvokeTheNativeContinuation() throws Exception {
        ExecutorService control = Executors.newSingleThreadExecutor(), io = Executors.newSingleThreadExecutor();
        AIAssistantPreparationJobs jobs = new AIAssistantPreparationJobs(control, io);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1), completed = new CountDownLatch(1);
        AtomicLong epoch = new AtomicLong(1);
        AtomicInteger dispatches = new AtomicInteger();
        try {
            jobs.start("owner-one", 1, epoch::get, cancel -> {
                entered.countDown(); release.await(); return new JSONObject();
            }, (value, failure) -> { if (failure == null) dispatches.incrementAndGet(); completed.countDown(); });
            assertTrue(entered.await(2, TimeUnit.SECONDS)); epoch.set(2); release.countDown();
            assertTrue(completed.await(2, TimeUnit.SECONDS)); assertEquals(0, dispatches.get());
        } finally { release.countDown(); jobs.close().get(2, TimeUnit.SECONDS); control.shutdownNow(); }
    }

    @Test public void accountCloseWaitsForActualIoExitAndCancelsItsTransportScope() throws Exception {
        ExecutorService control = Executors.newSingleThreadExecutor(), io = Executors.newSingleThreadExecutor();
        AIAssistantPreparationJobs jobs = new AIAssistantPreparationJobs(control, io);
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicInteger cancelCalls = new AtomicInteger(), dispatches = new AtomicInteger();
        try {
            jobs.start("owner-one", 1, () -> 1, cancel -> {
                cancel.onCancel(cancelCalls::incrementAndGet); entered.countDown(); release.await();
                return new JSONObject();
            }, (value, failure) -> dispatches.incrementAndGet());
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            CompletableFuture<Void> barrier = jobs.close();
            assertEquals(1, cancelCalls.get());
            assertFalse("Deleting an account must wait for its checkpoint writer", barrier.isDone());
            release.countDown(); barrier.get(2, TimeUnit.SECONDS);
            assertEquals(0, dispatches.get());
        } finally { release.countDown(); jobs.close().get(2, TimeUnit.SECONDS); control.shutdownNow(); }
    }

    @Test public void accountDeletionBarrierAlsoWaitsForAttachmentJobsAfterTheControlThreadExited() throws Exception {
        String owner = "fixture-preparation-" + java.util.UUID.randomUUID();
        CompletableFuture<Void> writerExited = new CompletableFuture<>();
        AIAssistantController.registerClosingAttachmentJobs(owner, writerExited);
        CompletableFuture<Void> barrier = AIAssistantController.stopForAcceptedAccountDeletion(owner);
        try {
            try { barrier.get(100, TimeUnit.MILLISECONDS); fail("Account deletion passed an active checkpoint writer"); }
            catch (TimeoutException expected) { }
            writerExited.complete(null); barrier.get(2, TimeUnit.SECONDS);
        } finally { writerExited.complete(null); }
    }
}
