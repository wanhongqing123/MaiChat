// The coordinator uses a synthetic transport and clock; no account or IM write occurs.
package com.kongshang.maichat;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import static org.junit.Assert.*;

public class MaiRemoteDiagnosticsCoordinatorTest {
    private static final String ID = "65de6748-3a4e-4d08-8ffd-bc78e1804ff9";
    @Rule public TemporaryFolder files = new TemporaryFolder();

    private static final class Clock implements MaiRemoteDiagnosticsCoordinator.Clock {
        long now;
        Runnable onSleep;
        public long monotonicMillis() { return now; }
        public long wallMillis() { return 1_800_000_000_000L + now; }
        public void sleep(long millis) { now += millis; if (onSleep != null) onSleep.run(); }
    }

    private final class Transport implements MaiRemoteDiagnosticsCoordinator.Transport {
        boolean current = true;
        final MaiRemoteDiagnosticsOperation permission = new MaiRemoteDiagnosticsOperation();
        int requests, reports;
        String target, command;
        byte[] merged;
        File written;
        Runnable afterWrite;
        boolean throwAfterReportQueue;
        final List<MaiRemoteDiagnosticsCoordinator.Receipt> receipts = new ArrayList<>();
        MaiRemoteDiagnosticsCoordinator.Delivery requestDelivery = MaiRemoteDiagnosticsCoordinator.Delivery.SENT;
        MaiRemoteDiagnosticsCoordinator.Delivery reportDelivery = MaiRemoteDiagnosticsCoordinator.Delivery.SENT;
        public boolean mayContinue(String identity, String peer, String recipient) {
            return permission.isAllowed() && current && identity.equals("account-generation")
                && peer.equals("peer") && recipient.equals("recipient");
        }
        public void cancelPendingWrites() { permission.cancel(); }
        public String sendText(String peer, String text, String identity) { requests++; command = text; return "request"; }
        public MaiRemoteDiagnosticsCoordinator.Delivery delivery(String id) {
            return id.equals("request") ? requestDelivery : reportDelivery;
        }
        public List<MaiRemoteDiagnosticsCoordinator.Receipt> receipts(String peer) { return receipts; }
        public JSONObject localEvidence(String peer, long began) throws Exception {
            return new JSONObject().put("platform", "Android").put("messages", new org.json.JSONArray());
        }
        public File writeReport(String id, byte[] bytes) throws Exception {
            merged = bytes;
            File file = files.newFile("maichat-diagnostics-" + id + ".md");
            Files.write(file.toPath(), bytes);
            written = file;
            if (afterWrite != null) afterWrite.run();
            return file;
        }
        public String sendReport(String recipient, File file, String identity) throws Exception {
            reports++; target = recipient;
            if (throwAfterReportQueue) throw new java.io.IOException("synthetic late bridge failure");
            return "report";
        }
    }

    private MaiRemoteDiagnosticsCoordinator coordinator(Transport transport, Clock clock) {
        return new MaiRemoteDiagnosticsCoordinator(transport, clock, (status, running, sending) -> { }, 1000, 500);
    }

    private File remote(String nonce) throws Exception {
        File file = files.newFile();
        Files.write(file.toPath(), new JSONObject().put("schemaVersion", 1).put("requestId", nonce)
            .put("token", "SECRET_TOKEN").put("files", new org.json.JSONArray()).toString()
            .getBytes(StandardCharsets.UTF_8));
        return file;
    }

    @Test public void matchedPeerAndNonceProduceRedactedReportForLockedRecipient() throws Exception {
        Transport transport = new Transport();
        transport.receipts.add(new MaiRemoteDiagnosticsCoordinator.Receipt("reply", "peer", true, "",
            "remote-diagnostics-" + ID + ".json", remote(ID)));
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.SENT,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertEquals("/diagnostics " + ID, transport.command);
        assertEquals("recipient", transport.target);
        assertEquals(1, transport.requests);
        assertEquals(1, transport.reports);
        assertFalse(new String(transport.merged, StandardCharsets.UTF_8).contains("SECRET_TOKEN"));
    }

    @Test public void unrelatedSenderNonceAndOutgoingAttachmentsDoNotCompleteTheRequest() throws Exception {
        Transport transport = new Transport();
        String name = "remote-diagnostics-" + ID + ".json";
        transport.receipts.add(new MaiRemoteDiagnosticsCoordinator.Receipt("wrong-peer", "other", true, "", name, remote(ID)));
        transport.receipts.add(new MaiRemoteDiagnosticsCoordinator.Receipt("outgoing", "peer", false, "", name, remote(ID)));
        transport.receipts.add(new MaiRemoteDiagnosticsCoordinator.Receipt("wrong-body", "peer", true, "", name,
            remote("75de6748-3a4e-4d08-8ffd-bc78e1804ff9")));
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.PARTIAL_SENT,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertTrue(new String(transport.merged, StandardCharsets.UTF_8).contains("无效或超限"));
    }

    @Test public void cancellationBeforeConfirmationSendsNothing() {
        Transport transport = new Transport();
        MaiRemoteDiagnosticsCoordinator coordinator = coordinator(transport, new Clock());
        coordinator.cancel();
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator.run("peer", "recipient", "account-generation", ID));
        assertEquals(0, transport.requests);
        assertEquals(0, transport.reports);
    }

    @Test public void cancellationDuringWaitDoesNotSendTheReport() {
        Transport transport = new Transport(); Clock clock = new Clock();
        MaiRemoteDiagnosticsCoordinator coordinator = coordinator(transport, clock);
        clock.onSleep = coordinator::cancel;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator.run("peer", "recipient", "account-generation", ID));
        assertEquals(1, transport.requests);
        assertEquals(0, transport.reports);
    }

    @Test public void accountChangeInvalidatesEvenTheSameVisibleUser() {
        Transport transport = new Transport(); Clock clock = new Clock();
        clock.onSleep = () -> transport.current = false;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator(transport, clock).run("peer", "recipient", "account-generation", ID));
        assertEquals(0, transport.reports);
    }

    @Test public void staleConfirmationDoesNotSendARequest() {
        Transport transport = new Transport();
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator(transport, new Clock()).run("peer", "recipient", "old-generation", ID));
        assertEquals(0, transport.requests);
    }

    @Test public void failedRequestStillSendsAnExplicitlyPartialLocalReport() {
        Transport transport = new Transport();
        transport.requestDelivery = MaiRemoteDiagnosticsCoordinator.Delivery.FAILED;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.PARTIAL_SENT,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertTrue(new String(transport.merged, StandardCharsets.UTF_8).contains("采集请求发送失败"));
    }

    @Test public void queuedReportWithoutReceiptIsNeverReportedAsSent() {
        Transport transport = new Transport();
        transport.reportDelivery = MaiRemoteDiagnosticsCoordinator.Delivery.PENDING;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.UNCONFIRMED,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertEquals(1, transport.reports);
    }

    @Test public void failedReportDeliveryIsNotSuccess() {
        Transport transport = new Transport();
        transport.reportDelivery = MaiRemoteDiagnosticsCoordinator.Delivery.FAILED;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.FAILED,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertEquals(1, transport.reports);
    }

    @Test public void accountChangeAfterReportPreparationPreventsForwardingAndRemovesUnsentFile() {
        Transport transport = new Transport();
        transport.afterWrite = () -> transport.current = false;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertEquals(0, transport.reports);
        assertNotNull(transport.written);
        assertFalse(transport.written.exists());
    }

    @Test public void cancellationAfterReportPreparationPreventsForwarding() {
        Transport transport = new Transport();
        MaiRemoteDiagnosticsCoordinator coordinator = coordinator(transport, new Clock());
        transport.afterWrite = coordinator::cancel;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.CANCELED,
            coordinator.run("peer", "recipient", "account-generation", ID));
        assertEquals(0, transport.reports);
    }

    @Test public void lateSendBridgeFailureKeepsTheQueuedAttachmentAndReportsUnknownDelivery() {
        Transport transport = new Transport();
        transport.throwAfterReportQueue = true;
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.UNCONFIRMED,
            coordinator(transport, new Clock()).run("peer", "recipient", "account-generation", ID));
        assertEquals(1, transport.reports);
        assertTrue(transport.written.exists());
    }

    @Test public void nonceBoundFailureStopsWaitingWithoutCopyingPeerFreeText() {
        Transport transport = new Transport(); Clock clock = new Clock();
        transport.receipts.add(new MaiRemoteDiagnosticsCoordinator.Receipt("failure", "peer", true,
            "远程排障采集失败（编号 " + ID + "）：PRIVATE_REMOTE_ERROR", "", null));
        assertEquals(MaiRemoteDiagnosticsCoordinator.Outcome.PARTIAL_SENT,
            coordinator(transport, clock).run("peer", "recipient", "account-generation", ID));
        assertEquals(0, clock.now);
        assertFalse(new String(transport.merged, StandardCharsets.UTF_8).contains("PRIVATE_REMOTE_ERROR"));
    }

    @Test public void coordinatorCancellationRevokesPermissionInsideAnAlreadyQueuedMainWrite() throws Exception {
        Transport transport = new Transport();
        MaiRemoteDiagnosticsCoordinator coordinator = coordinator(transport, new Clock());
        java.util.concurrent.FutureTask<String> queued = new java.util.concurrent.FutureTask<>(() ->
            transport.permission.write(() -> true, () -> transport.sendText("peer", "/diagnostics " + ID, "account-generation")));
        coordinator.cancel();
        queued.run();
        try { queued.get(); fail("Canceled queued IM write ran"); }
        catch (java.util.concurrent.ExecutionException expected) {
            assertTrue(expected.getCause() instanceof java.io.IOException);
        }
        assertEquals(0, transport.requests);
    }
}
