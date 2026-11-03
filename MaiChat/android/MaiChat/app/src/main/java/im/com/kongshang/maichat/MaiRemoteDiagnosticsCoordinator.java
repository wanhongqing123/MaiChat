// Coordinates a user-confirmed diagnostics request through normal IM messages.
// The transport owns account checks and platform I/O; this state machine is testable without IM.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

final class MaiRemoteDiagnosticsCoordinator {
    enum Delivery { PENDING, SENT, FAILED, UNKNOWN }
    enum Outcome { SENT, PARTIAL_SENT, CANCELED, FAILED, UNCONFIRMED }

    interface Clock {
        long monotonicMillis();
        long wallMillis();
        void sleep(long millis) throws InterruptedException;
    }

    interface Observer { void changed(String status, boolean running, boolean sending); }

    interface Transport {
        default void cancelPendingWrites() { }
        boolean mayContinue(String identity, String peer, String recipient) throws Exception;
        String sendText(String peer, String text, String identity) throws Exception;
        Delivery delivery(String messageId) throws Exception;
        List<Receipt> receipts(String peer) throws Exception;
        JSONObject localEvidence(String peer, long began) throws Exception;
        File writeReport(String id, byte[] data) throws Exception;
        String sendReport(String recipient, File report, String identity) throws Exception;
    }

    static final class Receipt {
        final String id, sender, text, fileName;
        final boolean incoming;
        final File file;
        Receipt(String id, String sender, boolean incoming, String text, String fileName, File file) {
            this.id = id; this.sender = sender; this.incoming = incoming;
            this.text = text; this.fileName = fileName; this.file = file;
        }
    }

    private final Transport transport;
    private final Clock clock;
    private final Observer observer;
    private final long responseTimeout, deliveryTimeout;
    private final AtomicBoolean canceled = new AtomicBoolean();

    MaiRemoteDiagnosticsCoordinator(Transport transport, Observer observer) {
        this(transport, new Clock() {
            public long monotonicMillis() { return System.nanoTime() / 1_000_000; }
            public long wallMillis() { return System.currentTimeMillis(); }
            public void sleep(long millis) throws InterruptedException { Thread.sleep(millis); }
        }, observer, 60_000, 30_000);
    }

    MaiRemoteDiagnosticsCoordinator(Transport transport, Clock clock, Observer observer,
                                    long responseTimeout, long deliveryTimeout) {
        this.transport = transport; this.clock = clock; this.observer = observer;
        this.responseTimeout = responseTimeout; this.deliveryTimeout = deliveryTimeout;
    }

    void cancel() {
        canceled.set(true);
        transport.cancelPendingWrites();
    }

    // Called once on a worker. Every IM write is preceded by the transport's
    // main-thread identity/lifecycle gate as well as this cancellation gate.
    Outcome run(String peer, String recipient, String identity, String requestId) {
        File report = null;
        boolean reportQueued = false;
        try {
            check(identity, peer, recipient);
            long began = clock.wallMillis();
            long deadline = clock.monotonicMillis() + responseTimeout;
            JSONObject local = transport.localEvidence(peer, began);
            observer.changed("正在请求远端现场，最长等待 60 秒…", true, false);
            check(identity, peer, recipient);
            String requestMessage = transport.sendText(peer, MaiRemoteDiagnosticsProtocol.command(requestId), identity);
            Delivery requested = awaitDelivery(requestMessage, identity, peer, recipient,
                Math.min(deadline, clock.monotonicMillis() + deliveryTimeout));
            String missing = requested == Delivery.FAILED ? "采集请求发送失败，未取得远端报告。" :
                "等待结束仍未收到有效报告；可能是对方未响应、版本不支持、采集失败或附件下载失败。";
            if (requested == Delivery.PENDING || requested == Delivery.UNKNOWN)
                missing += "采集请求发送结果尚未确认，不能据此断言未送达。";
            JSONObject remote = null;
            Set<String> inspected = new HashSet<>();
            boolean refused = false;
            while (requested != Delivery.FAILED && clock.monotonicMillis() < deadline) {
                check(identity, peer, recipient);
                for (Receipt receipt : transport.receipts(peer)) {
                    if (!receipt.incoming || !peer.equals(receipt.sender)) continue;
                    String refusal = MaiRemoteDiagnosticsProtocol.failureReceipt(receipt.text, requestId);
                    if (refusal != null) { missing = refusal; refused = true; break; }
                    if (!MaiRemoteDiagnosticsProtocol.reportName(requestId).equals(receipt.fileName)
                            || receipt.file == null || !receipt.file.isFile()
                            || !inspected.add(receipt.id)) continue;
                    try {
                        remote = MaiRemoteDiagnosticsProtocol.readReport(receipt.file, requestId);
                        break;
                    } catch (IOException invalid) {
                        // Never copy a peer-controlled parse error or file path into the report.
                        missing = "收到的远端附件无效或超限，已拒绝合并；报告只包含本地现场。";
                    }
                }
                if (remote != null || refused) break;
                clock.sleep(Math.min(250, Math.max(1, deadline - clock.monotonicMillis())));
            }
            check(identity, peer, recipient);
            byte[] merged = MaiRemoteDiagnosticsProtocol.mergedReport(requestId, local, remote,
                remote == null ? missing : null);
            report = transport.writeReport(requestId, merged);
            check(identity, peer, recipient);
            observer.changed(remote == null ? "正在发送部分报告，远端缺失原因已写入报告…" : "正在发送合并报告…",
                true, true);
            // An SDK bridge timeout can race a write already accepted on the
            // main thread. From this point retain the file and report unknown
            // delivery rather than claiming that no attachment was sent.
            reportQueued = true;
            String outgoing = transport.sendReport(recipient, report, identity);
            Delivery delivered = awaitDelivery(outgoing, identity, peer, recipient,
                clock.monotonicMillis() + deliveryTimeout);
            check(identity, peer, recipient);
            if (delivered == Delivery.SENT) return finish(remote == null ?
                "已发送给所选好友（远端现场缺失，原因已写入报告）。" : "合并报告已发送给所选好友。",
                remote == null ? Outcome.PARTIAL_SENT : Outcome.SENT);
            if (delivered == Delivery.FAILED)
                return finish("报告发送失败，没有确认送达。请查看接收好友聊天中的消息状态。", Outcome.FAILED);
            return finish("发送结果尚未确认，请查看接收好友聊天中的消息状态，避免连续重发。", Outcome.UNCONFIRMED);
        } catch (Stopped | InterruptedException stopped) {
            if (stopped instanceof InterruptedException) Thread.currentThread().interrupt();
            return finish("已取消回传；已经发出的消息无法撤回。", Outcome.CANCELED);
        } catch (Exception failure) {
            return finish(reportQueued ? "发送结果尚未确认，请查看接收好友聊天中的消息状态。" :
                "无法完成排障采集，未发送报告附件。", reportQueued ? Outcome.UNCONFIRMED : Outcome.FAILED);
        } finally {
            transport.cancelPendingWrites();
            // Queued attachments remain available for the normal history/retry
            // path. Only an app-owned report never queued to IM may be removed.
            if (report != null && !reportQueued) report.delete();
        }
    }

    private Delivery awaitDelivery(String message, String identity, String peer, String recipient,
                                   long deadline) throws Exception {
        while (true) {
            check(identity, peer, recipient);
            Delivery state = transport.delivery(message);
            if (state == Delivery.SENT || state == Delivery.FAILED || clock.monotonicMillis() >= deadline)
                return state;
            clock.sleep(Math.min(250, Math.max(1, deadline - clock.monotonicMillis())));
        }
    }

    private void check(String identity, String peer, String recipient) throws Exception {
        if (canceled.get() || !transport.mayContinue(identity, peer, recipient)) throw new Stopped();
    }

    private Outcome finish(String status, Outcome outcome) {
        observer.changed(status, false, false);
        return outcome;
    }

    private static final class Stopped extends Exception { }
}
