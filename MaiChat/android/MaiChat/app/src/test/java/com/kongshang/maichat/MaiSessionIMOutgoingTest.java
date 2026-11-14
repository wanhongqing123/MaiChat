// 文件作用：缺失、损坏附件必须结束交接并报告本地错误，不能静默消失或尝试 SDK 发送。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.content.Context;
import androidx.test.core.app.ApplicationProvider;
import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MaiSessionIMOutgoingTest {
    @Test public void textApprovalKeepsTypedInteractionAndOriginalEnvelope() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        JSONObject value = new JSONObject().put("scope", new JSONObject().put("owner_id", "owner"))
            .put("kind", "text").put("peer_id", "peer").put("text", "  Review this operation  ")
            .put("opaque_future_field", new JSONObject().put("proof", 7))
            .put("interaction", new JSONObject().put("kind", "approval-request").put("token", "approval-owned")
                .put("actions", new org.json.JSONArray().put("approve-once").put("reject")));
        String original = value.toString();
        MaiSessionIMOutgoing prepared = MaiSessionIMOutgoing.prepare(context, value, () -> true);
        assertNotNull(prepared.message.approvalRequest());
        assertEquals("approval-owned", prepared.message.approvalRequest().token());
        assertEquals("  Review this operation  ", prepared.exactBody);
        assertEquals(original, value.toString());
        value.put("interaction", new JSONObject().put("kind", "approval-resolved").put("token", "approval-owned")
            .put("outcome", "auto-declined"));
        prepared = MaiSessionIMOutgoing.prepare(context, value, () -> true);
        assertEquals(RemoteIMApprovalAction.AUTO_DECLINED, prepared.message.approvalDecision().action());
    }
    @Test public void reusedProcessLocalDeliveryTokensCannotCollideInPersistedRowIds() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        JSONObject scope = new JSONObject().put("sdk_app_id", "123").put("owner_id", "owner")
            .put("auth_generation", "4").put("controller_generation", "1");
        JSONObject delivery = new JSONObject().put("scope", scope).put("bridge_id", "im-session-1").put("token", "1")
            .put("dispatch_index", "0").put("kind", "text").put("peer_id", "peer").put("text", "exact body");
        MaiSessionIMOutgoing firstProcess = MaiSessionIMOutgoing.prepare(context, delivery, () -> true);
        MaiSessionIMOutgoing restartedProcess = MaiSessionIMOutgoing.prepare(context, new JSONObject(delivery.toString()), () -> true);
        assertNotEquals(firstProcess.message.id(), restartedProcess.message.id());
        String stable = firstProcess.message.id();
        firstProcess.message.setRemoteId("real-sdk-id"); firstProcess.message.setStatus(RemoteIMMessage.Status.SENT);
        assertEquals(stable, firstProcess.message.id()); assertEquals(stable, firstProcess.message.snapshot().id());
    }
    @Test public void malformedInteractionNeverFallsBackToPlainMachineText() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        for (String interaction : new String[]{"{}", "{\"kind\":\"approval-decision\",\"token\":\"approval-one\",\"action\":\"approve-once\"}",
                "{\"kind\":\"approval-resolved\",\"token\":\"approval-one\",\"outcome\":\"approved\"}"}) {
            JSONObject value = new JSONObject().put("scope", new JSONObject().put("owner_id", "owner"))
                .put("kind", "text").put("peer_id", "peer").put("text", "Review").put("interaction", new JSONObject(interaction));
            try { MaiSessionIMOutgoing.prepare(context, value, () -> true); fail("Must reject invalid control"); }
            catch (java.io.IOException expected) { assertTrue(expected.getMessage().contains("审批")); }
        }
        JSONObject question = new JSONObject().put("scope", new JSONObject().put("owner_id", "owner"))
            .put("kind", "text").put("peer_id", "peer").put("text", "Choose A or B").put("question_id", "qst_owned");
        MaiSessionIMOutgoing prepared = MaiSessionIMOutgoing.prepare(context, question, () -> true);
        assertEquals("Choose A or B", prepared.exactBody); assertNull(prepared.message.approvalRequest());
        question.put("interaction", "{\"kind\":\"approval-request\",\"token\":\"approval-one\",\"actions\":[\"approve-once\",\"reject\"]}");
        try { MaiSessionIMOutgoing.prepare(context, question, () -> true); fail("String cannot replace interaction object"); }
        catch (java.io.IOException expected) { assertTrue(expected.getMessage().contains("审批")); }
    }
    @Test public void missingMediaAcknowledgesWithoutSdkSendAndReportsOneLocalError() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        check(context, new File(context.getFilesDir(), "missing-bound-image.png"));
    }
    @Test public void corruptImageAcknowledgesWithoutSdkSendAndReportsOneLocalError() throws Exception {
        Context context = ApplicationProvider.getApplicationContext();
        File file = new File(context.getFilesDir(), "corrupt-bound-image.png");
        Files.write(file.toPath(), new byte[]{1, 2, 3});
        org.robolectric.shadows.ShadowBitmapFactory.setAllowInvalidImageData(false);
        try { check(context, file); }
        finally { org.robolectric.shadows.ShadowBitmapFactory.setAllowInvalidImageData(true); file.delete(); }
    }
    private static void check(Context context, File file) throws Exception {
        JSONObject scope = new JSONObject().put("sdk_app_id", "123").put("owner_id", "owner")
            .put("auth_generation", "4").put("controller_generation", "5");
        JSONObject delivery = new JSONObject().put("scope", scope).put("bridge_id", "bridge").put("token", "1")
            .put("dispatch_index", "0").put("kind", "image").put("peer_id", "peer")
            .put("path", file.getAbsolutePath()).put("caption", "caption");
        AtomicInteger sdkSend = new AtomicInteger(), ack = new AtomicInteger(); List<String> errors = new ArrayList<>();
        MaiSessionIMOutgoing.prepareAndDispatch(context, delivery, () -> true, ack::incrementAndGet, errors::add,
            prepared -> sdkSend.incrementAndGet());
        assertEquals(0, sdkSend.get()); assertEquals(1, ack.get()); assertEquals(1, errors.size());
        assertTrue(errors.get(0).contains("未自动重发")); assertTrue(errors.get(0).length() < 100);
    }
}
