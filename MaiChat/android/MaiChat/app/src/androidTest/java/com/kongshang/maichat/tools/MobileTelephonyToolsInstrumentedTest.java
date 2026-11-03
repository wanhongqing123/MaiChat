// 文件作用：Android 设备集成测试；MobileTelephonyToolsInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：grantedSmsPermissionReadsABoundedInboxResult、readsOnlyAfterPhonePermissionAndNeverReadsSmsWithoutItsGrant。
// 直接依赖：com.kongshang.maichat.MainActivity；实现与这些接口协作。
package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.MainActivity;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.Assume;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MobileTelephonyToolsInstrumentedTest {
    // Enable explicitly on a test device and accept the real OS permission dialog.
    @Test public void requestsSmsPermissionAndReusesTheGrant() throws Exception {
        Assume.assumeTrue("true".equals(InstrumentationRegistry.getArguments()
            .getString("interactiveSmsPermission")));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_SMS)
            != PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            Field field = MainActivity.class.getDeclaredField("maiChatHostTools");
            field.setAccessible(true);
            MaiChatHostTools tools = (MaiChatHostTools) field.get(activity);
            JSONObject permission = requestPermission(tools, "sms");
            assertEquals("granted", permission.getString("status"));
            assertEquals(true, permission.getBoolean("prompted"));
            assertEquals(false, requestPermission(tools, "sms").getBoolean("prompted"));
            JSONObject result = runAsync(() -> (JSONObject) tools.execute("mobile_read_sms",
                new JSONObject().put("limit", 1)));
            assertEquals("android_sms_inbox", result.getString("source"));
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }

    // Run with -e interactiveCallPermission true and approve only the OS prompts.
    @Test public void contactsGrantDoesNotReplacePhoneCallGrant() throws Exception {
        Assume.assumeTrue("true".equals(InstrumentationRegistry.getArguments()
            .getString("interactiveCallPermission")));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_CONTACTS)
            != PackageManager.PERMISSION_GRANTED);
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.CALL_PHONE)
            != PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            Field field = MainActivity.class.getDeclaredField("maiChatHostTools");
            field.setAccessible(true);
            MaiChatHostTools tools = (MaiChatHostTools) field.get(activity);
            JSONObject contacts = requestPermission(tools, "contacts");
            assertEquals("granted", contacts.getString("status"));
            assertEquals(PackageManager.PERMISSION_GRANTED,
                context.checkSelfPermission(Manifest.permission.READ_CONTACTS));
            assertNotEquals(PackageManager.PERMISSION_GRANTED,
                context.checkSelfPermission(Manifest.permission.CALL_PHONE));

            Field permissionsField = MaiChatHostTools.class.getDeclaredField("permissions");
            permissionsField.setAccessible(true);
            MobilePermissionTools permissions = (MobilePermissionTools)
                permissionsField.get(tools);
            List<Intent> calls = new ArrayList<>();
            MobileTelephonyTools dialer = new MobileTelephonyTools(activity, permissions,
                calls::add);
            JSONObject first = runAsync(() -> dialer.dial(new JSONObject()
                .put("phone_number", "+15550100")));
            JSONObject second = runAsync(() -> dialer.dial(new JSONObject()
                .put("phone_number", "+15550101")));
            assertEquals("call_requested", first.getString("status"));
            assertEquals("call_requested", second.getString("status"));
            assertEquals(2, calls.size());
            assertEquals(Intent.ACTION_CALL, calls.get(0).getAction());
            assertEquals("+15550100", calls.get(0).getData().getSchemeSpecificPart());
            assertEquals("granted", requestPermission(tools, "phone_call").getString("status"));
            assertEquals(false, requestPermission(tools, "phone_call")
                .getBoolean("prompted"));
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }

    private static JSONObject requestPermission(MaiChatHostTools tools, String permission)
            throws Exception {
        return runAsync(() -> (JSONObject) tools.execute("mobile_request_permission",
            new JSONObject().put("permission", permission)));
    }

    private static JSONObject runAsync(Callable<JSONObject> call) throws Exception {
        FutureTask<JSONObject> request = new FutureTask<>(call);
        new Thread(request, "phone-permission-test").start();
        return request.get(125, TimeUnit.SECONDS);
    }

    @Test public void grantedSmsPermissionReadsABoundedInboxResult() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_SMS)
            == PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            MobilePermissionTools permissions = new MobilePermissionTools(activity,
                new MobilePhotoTools(activity));
            JSONObject result = new MobileTelephonyTools(activity, permissions).readSms(
                new JSONObject().put("limit", 1));
            assertTrue(result.toString(), result.has("messages"));
            assertEquals("android_sms_inbox", result.getString("source"));
            assertTrue(result.getJSONArray("messages").length() <= 1);
            String expected = InstrumentationRegistry.getArguments().getString("expectedSmsBody");
            if (expected != null) {
                assertEquals(1, result.getJSONArray("messages").length());
                assertEquals(expected, result.getJSONArray("messages")
                    .getJSONObject(0).getString("body"));
            }
            JSONObject future = new MobileTelephonyTools(activity, permissions).readSms(
                new JSONObject().put("after_ms", System.currentTimeMillis() + 86400000L));
            assertEquals(0, future.getJSONArray("messages").length());
            // The ordinary API uses the same implementation as the tool adapter.
            assertTrue(new MobileSMSInbox(context, () -> true).readRecent(1, 0).size() <= 1);
            try {
                new MobileSMSInbox(context, () -> false).readRecent(1, 0);
                throw new AssertionError("background SMS read succeeded");
            } catch (IllegalStateException backgroundDenied) { }
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }

    @Test public void readsOnlyAfterPhonePermissionAndNeverReadsSmsWithoutItsGrant()
            throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_PHONE_NUMBERS)
            != PackageManager.PERMISSION_GRANTED);
        Assume.assumeTrue(context.checkSelfPermission(Manifest.permission.READ_SMS)
            != PackageManager.PERMISSION_GRANTED);
        MainActivity activity = (MainActivity) InstrumentationRegistry.getInstrumentation()
            .startActivitySync(new Intent(context, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            MobilePermissionTools permissions = new MobilePermissionTools(activity,
                new MobilePhotoTools(activity));
            MobileTelephonyTools tools = new MobileTelephonyTools(activity, permissions);
            assertEquals("permission_denied", tools.getPhoneNumber().getString("code"));
            assertEquals("permission_denied", tools.readSms(new JSONObject()).getString("code"));

            automation.grantRuntimePermission(context.getPackageName(),
                Manifest.permission.READ_PHONE_NUMBERS);
            JSONObject permission = new MobilePermissionTools(activity,
                new MobilePhotoTools(activity)).request(
                    new JSONObject().put("permission", "phone_number"));
            assertEquals("granted", permission.getString("status"));
            assertNotEquals("permission_denied", tools.getPhoneNumber().optString("code"));
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(activity::finish);
        }
    }
}
