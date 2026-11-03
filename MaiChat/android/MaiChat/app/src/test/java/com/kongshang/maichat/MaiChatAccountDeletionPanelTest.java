// 文件作用：使用假账号合同验证本地注销表单、危险确认、敏感输入清理和私有回执；禁止真实请求。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.app.Activity;
import android.app.AlertDialog;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public class MaiChatAccountDeletionPanelTest {
    private static final String ID = "fixture_account", UUID = "123e4567-e89b-42d3-a456-426614174000";
    private Activity activity;
    private MaiChatAccountDeletionPanel panel;
    private final AtomicInteger deletionCalls = new AtomicInteger(), challengeCalls = new AtomicInteger(), accepted = new AtomicInteger();
    private final AtomicBoolean current = new AtomicBoolean(true);
    private boolean invalidateAtReceipt;

    private void open(boolean hasPassword) throws Exception {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        MaiChatAccountSettingsService service = new MaiChatAccountSettingsService(ID, current::get, (path, method, body) -> {
            if (path.endsWith("/options")) return new JSONObject().put("has_password", hasPassword)
                .put("phone_masked", "+8613****0001").put("processing_days", 30);
            if (path.endsWith("/challenge")) { challengeCalls.incrementAndGet();
                return new JSONObject().put("challenge_id", UUID).put("expires_in", 300).put("resend_after", 60); }
            assertEquals("auth/account/deletion", path);
            deletionCalls.incrementAndGet();
            if (invalidateAtReceipt) current.set(false);
            return new JSONObject().put("request_id", UUID).put("status", "pending").put("due_at", 1_800_000_000_000L)
                .put("receipt_token", "A".repeat(43)).put("support_email", "support@example.test");
        });
        panel = new MaiChatAccountDeletionPanel(activity, service, accepted::incrementAndGet);
        activity.setContentView(panel);
        await(() -> findInput(panel, "输入上方 MaiChat ID") != null);
    }

    private static EditText findInput(View root, String hint) {
        if (root instanceof EditText && hint.contentEquals(((EditText) root).getHint())) return (EditText) root;
        if (root instanceof ViewGroup) { ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) { EditText result = findInput(group.getChildAt(i), hint); if (result != null) return result; }
        }
        return null;
    }
    private static Button findButton(View root, String title) {
        if (root instanceof Button && title.contentEquals(((Button) root).getText())) return (Button) root;
        if (root instanceof ViewGroup) { ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) { Button result = findButton(group.getChildAt(i), title); if (result != null) return result; }
        }
        return null;
    }
    private static void await(BooleanSupplier ready) throws Exception {
        for (int i = 0; i < 200; i++) { Shadows.shadowOf(Looper.getMainLooper()).idle(); if (ready.getAsBoolean()) return; Thread.sleep(5); }
        fail("Fixture UI did not become ready");
    }
    private AlertDialog confirm() {
        findButton(panel, "提交注销申请").performClick();
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        return ShadowAlertDialog.getLatestAlertDialog();
    }

    @Test public void currentIdAndPasswordAreRequiredAndDangerCancelNeverDeletes() throws Exception {
        open(true);
        Button submit = findButton(panel, "提交注销申请");
        findInput(panel, "输入登录密码").setText("fixture-password");
        assertFalse(panel.safeStatus().toString().contains("fixture-password"));
        assertFalse(panel.safeStatus().has("password")); assertFalse(panel.safeStatus().has("code"));
        findInput(panel, "输入上方 MaiChat ID").setText("another_account"); assertFalse(submit.isEnabled());
        findInput(panel, "输入上方 MaiChat ID").setText(ID); assertTrue(submit.isEnabled());
        AlertDialog danger = confirm(); assertTrue(danger.isShowing());
        danger.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        assertEquals(0, deletionCalls.get()); assertFalse(panel.isSubmitting());
        panel.close(); assertEquals("", findInput(panel, "输入登录密码").getText().toString());
    }

    @Test public void smsModeWaitsForIssuedChallengeAndDoesNotSubmitOnItsOwn() throws Exception {
        open(false);
        findInput(panel, "输入上方 MaiChat ID").setText(ID);
        findInput(panel, "输入 6 位验证码").setText("123456");
        assertFalse(findButton(panel, "提交注销申请").isEnabled());
        findButton(panel, "获取验证码").performClick(); await(() -> challengeCalls.get() == 1 && findButton(panel, "60 秒后重发") != null);
        await(() -> findInput(panel, "输入 6 位验证码").isEnabled());
        findInput(panel, "输入 6 位验证码").setText("123456");
        assertTrue(findButton(panel, "提交注销申请").isEnabled());
        assertEquals(0, deletionCalls.get()); panel.close();
    }

    @Test public void acceptedReceiptIsSavedAndLogoutCallbackWaitsForAcknowledgement() throws Exception {
        open(true);
        findInput(panel, "输入上方 MaiChat ID").setText(ID); findInput(panel, "输入登录密码").setText("fixture-password");
        AlertDialog danger = confirm(); danger.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        await(() -> deletionCalls.get() == 1 && !panel.isSubmitting());
        assertTrue(MaiChatAccountDeletionPanel.hasSavedReceipt(activity)); assertEquals(0, accepted.get());
        AlertDialog receipt = ShadowAlertDialog.getLatestAlertDialog();
        assertNotSame(danger, receipt); receipt.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1, accepted.get()); assertEquals("", findInput(panel, "输入登录密码").getText().toString()); panel.close();
    }

    @Test public void lateAcceptedReceiptIsPreservedWithoutLoggingOutAnotherIdentity() throws Exception {
        open(true); invalidateAtReceipt = true;
        findInput(panel, "输入上方 MaiChat ID").setText(ID); findInput(panel, "输入登录密码").setText("fixture-password");
        AlertDialog danger = confirm(); danger.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        await(() -> deletionCalls.get() == 1 && !panel.isSubmitting());
        assertTrue(MaiChatAccountDeletionPanel.hasSavedReceipt(activity)); assertEquals(0, accepted.get()); panel.close();
    }
}
