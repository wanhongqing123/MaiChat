// 文件作用：阿里 NLS 凭据的独立内存缓存；并发领取合并，提前90秒刷新，登录代际隔离。
package com.kongshang.maichat;

import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.function.LongSupplier;

final class AliyunAsrCredentialCache {
    interface Loader { AliyunAsrCredential load() throws Exception; }
    private final Executor executor;
    private final LongSupplier clock;
    private String session = "";
    private AliyunAsrCredential cached;
    private CompletableFuture<AliyunAsrCredential> pending;
    AliyunAsrCredentialCache(Executor executor, LongSupplier clock) {
        this.executor = executor; this.clock = clock;
    }
    void activate(String identity) {
        CompletableFuture<AliyunAsrCredential> previous;
        synchronized (this) { session = identity; cached = null; previous = pending; pending = null; }
        if (previous != null) previous.completeExceptionally(changed());
    }
    CompletableFuture<AliyunAsrCredential> get(String identity, Loader loader) {
        CompletableFuture<AliyunAsrCredential> request;
        synchronized (this) {
            if (session.isEmpty() || !session.equals(identity)) return failed(changed());
            if (cached != null && cached.expiresAt > clock.getAsLong() + 90)
                return CompletableFuture.completedFuture(cached);
            cached = null;
            if (pending != null) return pending;
            request = new CompletableFuture<>(); pending = request;
        }
        try {
            executor.execute(() -> {
                try {
                    synchronized (this) {
                        if (!session.equals(identity) || pending != request) throw changed();
                    }
                    AliyunAsrCredential issued = loader.load();
                    synchronized (this) {
                        if (!session.equals(identity) || pending != request) throw changed();
                        if (issued == null || issued.expiresAt <= clock.getAsLong() + 90)
                            throw new IOException("阿里云语音识别临时凭据已过期");
                        cached = issued; pending = null;
                    }
                    request.complete(issued);
                } catch (Exception error) {
                    synchronized (this) { if (pending == request) pending = null; }
                    request.completeExceptionally(error);
                }
            });
        } catch (RuntimeException error) {
            synchronized (this) { if (pending == request) pending = null; }
            request.completeExceptionally(error);
        }
        return request;
    }
    private static IOException changed() { return new IOException("登录账号已变化，请重试语音输入"); }
    private static CompletableFuture<AliyunAsrCredential> failed(Exception error) {
        CompletableFuture<AliyunAsrCredential> value = new CompletableFuture<>();
        value.completeExceptionally(error); return value;
    }
}
