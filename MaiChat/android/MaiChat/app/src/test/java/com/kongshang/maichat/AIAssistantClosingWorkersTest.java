// 文件作用：模拟 A→B→A 快速切号后的两代后台线程，注销屏障不能遗漏第一代 A。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class AIAssistantClosingWorkersTest {
    private Thread waiting(CountDownLatch ready, CountDownLatch release) {
        Thread worker = new Thread(() -> { ready.countDown();
            try { release.await(); } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
        }, "fixture-closing-worker");
        worker.start(); return worker;
    }

    @Test public void secondGenerationExitCannotReleaseTheBarrierWhileFirstGenerationIsAlive() throws Exception {
        String accountA = "fixture-a-" + UUID.randomUUID(), accountB = "fixture-b-" + UUID.randomUUID();
        CountDownLatch ready = new CountDownLatch(3), firstRelease = new CountDownLatch(1),
            secondRelease = new CountDownLatch(1), otherRelease = new CountDownLatch(1);
        Thread first = waiting(ready, firstRelease), other = waiting(ready, otherRelease), second = waiting(ready, secondRelease);
        try {
            assertTrue(ready.await(2, TimeUnit.SECONDS));
            AIAssistantController.registerClosingAccountWorker(accountA, first);
            AIAssistantController.registerClosingAccountWorker(accountB, other);
            AIAssistantController.registerClosingAccountWorker(accountA, second);
            CompletableFuture<Void> barrier = AIAssistantController.stopForAcceptedAccountDeletion(accountA);
            secondRelease.countDown(); second.join(2000); assertFalse(second.isAlive());
            try { barrier.get(150, TimeUnit.MILLISECONDS); fail("First-generation account worker was forgotten"); }
            catch (TimeoutException expected) { }
            firstRelease.countDown(); barrier.get(2, TimeUnit.SECONDS);
            assertTrue("Another account must not be joined or stopped", other.isAlive());
        } finally {
            firstRelease.countDown(); secondRelease.countDown(); otherRelease.countDown();
            first.join(2000); second.join(2000); other.join(2000);
        }
    }
}
