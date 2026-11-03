// 文件作用：仅使用本地 JSON fixture 验证注销/举报合同、登录代际与重复提交；不访问真实账号。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import java.io.IOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;

public class MaiChatAccountSettingsServiceTest {
    private static final String ID = "fixture_account";
    private static final String UUID = "123e4567-e89b-42d3-a456-426614174000";
    private JSONObject options() throws Exception {
        return new JSONObject().put("has_password", true).put("phone_masked", "+8613****0001").put("processing_days", 30);
    }
    private JSONObject receipt() throws Exception {
        return new JSONObject().put("request_id", UUID).put("status", "pending").put("due_at", 1_800_000_000_000L)
            .put("receipt_token", "A".repeat(43)).put("support_email", "support@example.test");
    }

    @Test public void passwordSubmissionSendsOnlyTheChosenVerificationAndKeepsReceiptTokenPrivate() throws Exception {
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, () -> true, (path, method, body) -> {
            if (path.endsWith("/options")) return options();
            assertEquals("auth/account/deletion", path); assertEquals("POST", method);
            assertEquals(ID, body.getString("confirm_account_id"));
            assertEquals("fixture-password", body.getString("password"));
            assertFalse(body.has("challenge_id")); assertFalse(body.has("code"));
            return receipt();
        });
        service.options();
        MaiChatAccountSettingsService.Receipt receipt = service.submitDeletion(ID, "fixture-password", null, null);
        assertEquals(UUID, receipt.requestId);
        assertFalse(receipt.safeSummary().has("receipt_token"));
        assertEquals(30, service.options().processingDays);
    }

    @Test public void smsChallengeAndSubmissionUseTheDeletionRoutesAndSixDigitCode() throws Exception {
        AtomicInteger calls = new AtomicInteger();
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, () -> true, (path, method, body) -> {
            calls.incrementAndGet();
            if (path.endsWith("/options")) return options();
            if (path.endsWith("/challenge")) {
                assertEquals(0, body.length());
                return new JSONObject().put("challenge_id", UUID).put("expires_in", 300).put("resend_after", 60);
            }
            assertEquals("123456", body.getString("code")); assertEquals(UUID, body.getString("challenge_id"));
            assertFalse(body.has("password")); return receipt();
        });
        service.options();
        assertEquals(60, service.requestCode().resendAfter);
        assertFalse(service.canSubmit(ID, "", UUID, "12345x"));
        service.submitDeletion(ID, "", UUID, "123456");
        assertEquals(3, calls.get());
    }

    @Test public void wrongConfirmationAndStaleIdentityCannotIssueDeletion() throws Exception {
        AtomicBoolean current = new AtomicBoolean(true); AtomicInteger writes = new AtomicInteger();
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, current::get, (path, method, body) -> {
            if (method.equals("POST")) writes.incrementAndGet();
            return options();
        });
        service.options();
        assertFalse(service.canSubmit("another_account", "fixture-password", null, null));
        try { service.submitDeletion("another_account", "fixture-password", null, null); fail(); }
        catch (IOException expected) { }
        current.set(false);
        try { service.submitDeletion(ID, "fixture-password", null, null); fail(); }
        catch (IOException expected) { }
        assertEquals(0, writes.get());
    }

    @Test public void lateReportReadCannotReturnRecordsToAnotherLogin() throws Exception {
        AtomicBoolean current = new AtomicBoolean(true);
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, current::get, (path, method, body) -> {
            assertEquals("safety/reports", path); assertEquals("GET", method); assertNull(body);
            current.set(false);
            return new JSONObject().put("reports", new JSONArray().put(new JSONObject().put("id", UUID)
                .put("status", "pending").put("reason", "other").put("created_at", 1L)));
        });
        try { service.reports(); fail("Old login read must be rejected"); }
        catch (IOException expected) { }
    }

    @Test public void deletionIsSingleFlightAndCannotRepeatAfterConfirmedReceipt() throws Exception {
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        AtomicInteger writes = new AtomicInteger();
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, () -> true, (path, method, body) -> {
            if (method.equals("GET")) return options();
            writes.incrementAndGet(); entered.countDown();
            assertTrue(release.await(5, TimeUnit.SECONDS)); return receipt();
        });
        service.options();
        java.util.concurrent.ExecutorService worker = Executors.newSingleThreadExecutor();
        try {
            java.util.concurrent.Future<MaiChatAccountSettingsService.Receipt> first = worker.submit(
                () -> service.submitDeletion(ID, "fixture-password", null, null));
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            try { service.submitDeletion(ID, "fixture-password", null, null); fail("Duplicate request accepted"); }
            catch (IOException expected) { }
            release.countDown(); assertEquals(UUID, first.get(5, TimeUnit.SECONDS).requestId);
            try { service.submitDeletion(ID, "fixture-password", null, null); fail("Confirmed request repeated"); }
            catch (IOException expected) { }
            assertEquals(1, writes.get());
        } finally { release.countDown(); worker.shutdownNow(); }
    }

    @Test public void parsesReadOnlyReportRecordsAndRejectsMalformedDeletionReceipt() throws Exception {
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, () -> true, (path, method, body) -> {
            if (path.equals("safety/reports")) return new JSONObject().put("reports", new JSONArray().put(new JSONObject()
                .put("id", UUID).put("status", "resolved").put("reason", "spam").put("created_at", 10L)
                .put("resolution", "本地 fixture 处理结果")));
            return path.endsWith("/options") ? options() : receipt().put("receipt_token", "invalid");
        });
        assertEquals("本地 fixture 处理结果", service.reports().get(0).resolution);
        service.options();
        try { service.submitDeletion(ID, "fixture-password", null, null); fail("Malformed receipt accepted"); }
        catch (IOException expected) { }
    }
}
