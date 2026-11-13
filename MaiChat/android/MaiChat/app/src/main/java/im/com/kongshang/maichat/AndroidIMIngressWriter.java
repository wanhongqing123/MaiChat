// 文件作用：复用普通历史 writer 的入站元数据、同记录丰富与 awaitable 栅栏；不读取快照、不导入会话。
package com.kongshang.maichat;

import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.function.BooleanSupplier;

final class AndroidIMIngressWriter {
    private final AndroidChatHistoryStore store;
    private final Executor writer;
    AndroidIMIngressWriter(AndroidChatHistoryStore store, Executor writer) { this.store = store; this.writer = writer; }

    CompletableFuture<RemoteIMIngress.Stored> initial(RemoteIMIngress.Metadata metadata, BooleanSupplier current) {
        CompletableFuture<RemoteIMIngress.Stored> result = new CompletableFuture<>();
        execute(result, current, () -> store.insertInitialMetadata(metadata.scope.ownerId, metadata.message()).bound(current));
        return result;
    }

    CompletableFuture<RemoteIMMessage> enrich(RemoteIMIngress.Metadata metadata, RemoteIMIngress.Stored stored,
            RemoteIMMessage ready, BooleanSupplier current) {
        CompletableFuture<RemoteIMMessage> result = new CompletableFuture<>();
        execute(result, current, () -> {
            RemoteIMMessage initial = metadata.message();
            if (!initial.fromUserId().equals(ready.fromUserId()) || !initial.toUserId().equals(ready.toUserId())
                    || !stored.remoteId.equals(initial.remoteId())) throw new IllegalArgumentException("Incoming metadata identity changed");
            RemoteIMMessage canonical = new RemoteIMMessage(stored.sourceRowId, stored.remoteId, initial.fromUserId(), initial.toUserId(),
                ready.text(), RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, stored.createdAtMs,
                ready.imageAttachment(), ready.voiceAttachment(), ready.fileAttachment(), ready.videoAttachment(),
                ready.origin(), ready.approvalRequest(), ready.approvalDecision());
            canonical.setCaptionAbove(ready.captionAbove()); canonical.setQuote(ready.quote());
            store.upsertMessage(metadata.scope.ownerId, canonical);
            return canonical;
        });
        return result;
    }

    CompletableFuture<Void> fence(BooleanSupplier current) {
        CompletableFuture<Void> result = new CompletableFuture<>(); execute(result, current, () -> null); return result;
    }

    private interface Operation<T> { T run() throws Exception; }
    private <T> void execute(CompletableFuture<T> result, BooleanSupplier current, Operation<T> operation) {
        try {
            writer.execute(() -> {
                try {
                    if (!current.getAsBoolean()) throw new CancellationException("历史持久化身份已变化");
                    T value = operation.run();
                    if (!current.getAsBoolean()) throw new CancellationException("历史持久化身份已变化");
                    result.complete(value);
                } catch (Exception failure) { result.completeExceptionally(failure); }
            });
        } catch (RuntimeException closed) { result.completeExceptionally(closed); }
    }
}
