// A single confirmation's permission, rechecked inside the actual main-thread write.
package com.kongshang.maichat;

import java.io.IOException;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

final class MaiRemoteDiagnosticsOperation {
    private final AtomicBoolean revoked = new AtomicBoolean();

    boolean isAllowed() { return !revoked.get(); }
    void cancel() { revoked.set(true); }

    // Android calls this inside the UI-thread closure, so UI cancellation and
    // the gate/write pair are serialized. A queued task keeps its original
    // permission; a later confirmation always receives a different instance.
    <T> T write(BooleanSupplier current, Callable<T> write) throws Exception {
        if (!isAllowed() || !current.getAsBoolean()) throw new IOException("诊断操作已取消或失效");
        return write.call();
    }
}
