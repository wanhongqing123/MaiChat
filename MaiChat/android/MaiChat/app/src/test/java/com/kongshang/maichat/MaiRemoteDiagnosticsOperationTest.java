// Exercises the exact permission used inside queued main-thread IM write closures.
package com.kongshang.maichat;

import java.io.IOException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;
import static org.junit.Assert.*;

public class MaiRemoteDiagnosticsOperationTest {
    @Test public void cancellationBeforeQueuedMainWritesRunBlocksBothRequestAndReport() throws Exception {
        MaiRemoteDiagnosticsOperation permission = new MaiRemoteDiagnosticsOperation();
        AtomicInteger writes = new AtomicInteger();
        FutureTask<String> request = new FutureTask<>(() -> permission.write(() -> true,
            () -> { writes.incrementAndGet(); return "request"; }));
        FutureTask<String> report = new FutureTask<>(() -> permission.write(() -> true,
            () -> { writes.incrementAndGet(); return "report"; }));
        permission.cancel();
        request.run(); report.run();
        assertRejected(request); assertRejected(report);
        assertEquals(0, writes.get());
    }

    @Test public void retryPermissionCannotReviveThePreviousQueuedTask() throws Exception {
        MaiRemoteDiagnosticsOperation previous = new MaiRemoteDiagnosticsOperation();
        AtomicInteger writes = new AtomicInteger();
        FutureTask<Integer> old = new FutureTask<>(() -> previous.write(() -> true, writes::incrementAndGet));
        previous.cancel();
        MaiRemoteDiagnosticsOperation retry = new MaiRemoteDiagnosticsOperation();
        assertEquals(1, (int) retry.write(() -> true, writes::incrementAndGet));
        old.run();
        assertRejected(old);
        assertEquals(1, writes.get());
    }

    @Test public void accountOrPageInvalidationIsCheckedWhenTheQueuedClosureActuallyRuns() throws Exception {
        MaiRemoteDiagnosticsOperation permission = new MaiRemoteDiagnosticsOperation();
        java.util.concurrent.atomic.AtomicBoolean current = new java.util.concurrent.atomic.AtomicBoolean(true);
        AtomicInteger writes = new AtomicInteger();
        FutureTask<Integer> queued = new FutureTask<>(() -> permission.write(current::get, writes::incrementAndGet));
        current.set(false);
        queued.run();
        assertRejected(queued);
        assertEquals(0, writes.get());
    }

    private void assertRejected(FutureTask<?> queued) throws Exception {
        try { queued.get(); fail("Canceled or stale write ran"); }
        catch (ExecutionException expected) { assertTrue(expected.getCause() instanceof IOException); }
    }
}
