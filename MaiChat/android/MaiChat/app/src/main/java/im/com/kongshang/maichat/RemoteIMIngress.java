// 文件作用：普通 SDK 入站的持久化身份与栅栏；只等待元数据写入，不排列路由任务或等待下载。
package com.kongshang.maichat;

import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

public final class RemoteIMIngress {
    private RemoteIMIngress() { }

    public static final class Scope {
        public final int sdkAppId;
        public final String ownerId;
        public final long loginEpoch;
        public Scope(int sdkAppId, String ownerId, long loginEpoch) {
            this.sdkAppId = sdkAppId; this.ownerId = ownerId; this.loginEpoch = loginEpoch;
        }
        @Override public boolean equals(Object value) {
            if (!(value instanceof Scope)) return false;
            Scope other = (Scope) value;
            return sdkAppId == other.sdkAppId && loginEpoch == other.loginEpoch && ownerId.equals(other.ownerId);
        }
        @Override public int hashCode() { return Objects.hash(sdkAppId, ownerId, loginEpoch); }
    }

    public static final class Metadata {
        public final Scope scope;
        public final long sequence;
        private final RemoteIMMessage initial;
        public Metadata(Scope scope, long sequence, RemoteIMMessage initial) {
            this.scope = scope; this.sequence = sequence; this.initial = initial.snapshot();
        }
        public RemoteIMMessage message() { return initial.snapshot(); }
    }

    public static final class Stored {
        public final String sourceRowId, remoteId;
        public final long createdAtMs;
        public final boolean inserted;
        private final BooleanSupplier current;
        public Stored(String sourceRowId, String remoteId, long createdAtMs, boolean inserted) {
            this(sourceRowId, remoteId, createdAtMs, inserted, () -> true);
        }
        private Stored(String sourceRowId, String remoteId, long createdAtMs, boolean inserted, BooleanSupplier current) {
            this.sourceRowId = sourceRowId; this.remoteId = remoteId; this.createdAtMs = createdAtMs; this.inserted = inserted; this.current = current;
        }
        Stored bound(BooleanSupplier current) { return new Stored(sourceRowId, remoteId, createdAtMs, inserted, current); }
        public boolean isCurrent() { return current.getAsBoolean(); }
    }

    public static final class Fence {
        public final Scope scope;
        public final long sequence;
        private final CompletableFuture<Void> metadata;
        private final BooleanSupplier current;
        Fence(Scope scope, long sequence, CompletableFuture<Void> metadata, BooleanSupplier current) {
            this.scope = scope; this.sequence = sequence; this.metadata = metadata; this.current = current;
        }
        public boolean isCurrent() { return current.getAsBoolean(); }
        public CompletableFuture<Void> await() {
            return metadata.thenRun(() -> { if (!isCurrent()) throw new CancellationException("入站持久化身份已变化"); });
        }
        public Fence afterWriter(Supplier<CompletableFuture<Void>> writer) {
            return new Fence(scope, sequence, await().thenCompose(ignored -> writer.get()), current);
        }
        public Fence requireCurrent(BooleanSupplier authority) {
            return new Fence(scope, sequence, metadata, () -> current.getAsBoolean() && authority.getAsBoolean());
        }
    }

    /** 前缀 future 覆盖已经到达但还没进入 SDK handler 的消息；没有消息/导入队列或全量记录缓存。 */
    static final class Tracker {
        private Scope scope;
        private long sequence;
        private CompletableFuture<Void> prefix = CompletableFuture.completedFuture(null);
        synchronized Registration register(Scope requested) {
            select(requested);
            Registration registration = new Registration(requested, ++sequence);
            prefix = CompletableFuture.allOf(prefix, registration.persisted);
            return registration;
        }
        synchronized Fence fence(Scope requested, BooleanSupplier current) {
            select(requested);
            return new Fence(requested, sequence, prefix, current);
        }
        private void select(Scope requested) {
            if (requested.equals(scope)) return;
            prefix.completeExceptionally(new CancellationException("SDK 入站代际已变化"));
            scope = requested; sequence = 0; prefix = CompletableFuture.completedFuture(null);
        }
        synchronized void invalidate() {
            prefix.completeExceptionally(new CancellationException("SDK 入站已关闭"));
            scope = null; sequence = 0; prefix = CompletableFuture.completedFuture(null);
        }
        synchronized void publish(Runnable publication) { publication.run(); }
    }

    static final class Registration {
        final Scope scope;
        final long sequence;
        final CompletableFuture<Stored> persisted = new CompletableFuture<>();
        Registration(Scope scope, long sequence) { this.scope = scope; this.sequence = sequence; }
    }
}
