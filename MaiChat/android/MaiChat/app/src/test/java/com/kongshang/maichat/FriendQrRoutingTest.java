// 文件作用：在 JVM 中验证普通扫码页面的分流与生命周期，不打开相机或访问账号服务。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import com.google.android.gms.tasks.Tasks;
import com.google.mlkit.vision.barcode.BarcodeScanner;
import com.google.mlkit.vision.barcode.BarcodeScannerOptions;
import com.google.mlkit.vision.barcode.BarcodeScanning;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.Shadows;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.shadows.ShadowActivity;

import java.lang.reflect.Proxy;
import java.util.Collections;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29, shadows = FriendQrRoutingTest.ShadowBarcodeScanning.class)
public final class FriendQrRoutingTest {
    private static final String REQUEST_ID = "6aaf55c7-8b63-4a62-9fd1-122dd7d5652a";
    private static final String DESKTOP_QR = "maichat://auth/desktop?request_id=" + REQUEST_ID;
    private static final String FRIEND_ID = "mtest_friend_12345";

    @Before public void keepTheJvmFixtureAwayFromCameraHardware() {
        Shadows.shadowOf(RuntimeEnvironment.getApplication()).denyPermissions(Manifest.permission.CAMERA);
    }

    @Test public void scannerPromptIncludesFriendAndDesktopLoginCodes() {
        ActivityController<FriendQrScanActivity> controller =
            Robolectric.buildActivity(FriendQrScanActivity.class).create();
        try {
            assertTrue(hasText(controller.get().getWindow().getDecorView(),
                "扫描好友二维码或电脑登录二维码"));
        } finally { controller.destroy(); }
    }

    @Test public void friendCodeRetainsTheOriginalResultContract() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            assertTrue(activity.acceptQrUri(FriendQrPolicy.link(FRIEND_ID)));
            ShadowActivity shadow = Shadows.shadowOf(activity);
            assertEquals(Activity.RESULT_OK, shadow.getResultCode());
            assertNotNull(shadow.getResultIntent());
            assertEquals(FRIEND_ID, shadow.getResultIntent().getStringExtra("friend_user_id"));
            assertNull(shadow.getNextStartedActivityForResult());
            assertTrue(activity.isFinishing());
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void desktopCodeOpensExistingConfirmationWithoutAnApprovalDecision() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            assertTrue("Desktop QR must not be rejected as an invalid friend code",
                activity.acceptQrUri(DESKTOP_QR));
            ShadowActivity.IntentForResult opened =
                Shadows.shadowOf(activity).getNextStartedActivityForResult();
            assertNotNull(opened);
            assertNotNull(opened.intent.getComponent());
            assertEquals(DesktopLoginApprovalActivity.class.getName(),
                opened.intent.getComponent().getClassName());
            assertEquals(DESKTOP_QR,
                opened.intent.getStringExtra(DesktopLoginApprovalActivity.EXTRA_QR_URI));
            assertFalse(opened.intent.hasExtra("approve"));
            assertFalse("The scanner must remain available behind confirmation", activity.isFinishing());
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void repeatedDesktopFramesCannotOpenAnotherConfirmation() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            assertTrue(activity.acceptQrUri(DESKTOP_QR));
            assertNotNull(Shadows.shadowOf(activity).getNextStartedActivityForResult());
            assertFalse(activity.acceptQrUri(DESKTOP_QR));
            assertNull(Shadows.shadowOf(activity).getNextStartedActivityForResult());
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void invalidCodesCannotOpenConfirmationOrFinishTheScanner() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            String[] invalid = {null, "", "https://example.invalid/qr",
                DESKTOP_QR + "&token=secret", DESKTOP_QR + "&request_id=" + REQUEST_ID,
                DESKTOP_QR + "#fragment", "maichat://auth:80/desktop?request_id=" + REQUEST_ID,
                FriendQrPolicy.link(FRIEND_ID) + "&token=secret"};
            for (String raw : invalid) assertFalse(activity.acceptQrUri(raw));
            assertNull(Shadows.shadowOf(activity).getNextStartedActivityForResult());
            assertFalse(activity.isFinishing());
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void pausedScannerRejectsLateFriendAndDesktopFrames() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        controller.pause().stop();
        try {
            FriendQrScanActivity activity = controller.get();
            assertFalse(activity.acceptQrUri(FriendQrPolicy.link(FRIEND_ID)));
            assertFalse(activity.acceptQrUri(DESKTOP_QR));
            assertNull(Shadows.shadowOf(activity).getNextStartedActivityForResult());
            assertFalse(activity.isFinishing());
        } finally { controller.destroy(); }
    }

    @Test public void destroyedScannerRejectsLateFrames() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        controller.pause().stop().destroy();
        FriendQrScanActivity activity = controller.get();
        assertFalse(activity.acceptQrUri(FriendQrPolicy.link(FRIEND_ID)));
        assertFalse(activity.acceptQrUri(DESKTOP_QR));
        assertNull(Shadows.shadowOf(activity).getNextStartedActivityForResult());
    }

    @Test public void retryReturnsToTheSameScannerAndRetainsFriendResults() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            assertTrue(activity.acceptQrUri(DESKTOP_QR));
            ShadowActivity.IntentForResult opened = Shadows.shadowOf(activity).getNextStartedActivityForResult();
            activity.onActivityResult(opened.requestCode, DesktopLoginApprovalActivity.RESULT_SCAN_AGAIN, null);
            assertFalse(activity.isFinishing());
            assertTrue(activity.acceptQrUri(FriendQrPolicy.link(FRIEND_ID)));
            assertEquals(FRIEND_ID, Shadows.shadowOf(activity).getResultIntent().getStringExtra("friend_user_id"));
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void loginCompletionCannotBeMistakenForAFriendResult() {
        ActivityController<FriendQrScanActivity> controller = resumedScanner();
        try {
            FriendQrScanActivity activity = controller.get();
            assertTrue(activity.acceptQrUri(DESKTOP_QR));
            ShadowActivity.IntentForResult opened = Shadows.shadowOf(activity).getNextStartedActivityForResult();
            activity.onActivityResult(opened.requestCode, Activity.RESULT_OK,
                new Intent().putExtra("friend_user_id", FRIEND_ID));
            assertTrue(activity.isFinishing());
            assertEquals(Activity.RESULT_CANCELED, Shadows.shadowOf(activity).getResultCode());
            assertNull(Shadows.shadowOf(activity).getResultIntent());
            assertFalse(activity.acceptQrUri(DESKTOP_QR));
        } finally { controller.pause().stop().destroy(); }
    }

    @Test public void scannedLoginUriLoadsDetailsBeforeShowingAnyDecision() {
        Intent intent = new Intent(RuntimeEnvironment.getApplication(), DesktopLoginApprovalActivity.class)
            .putExtra(DesktopLoginApprovalActivity.EXTRA_QR_URI, DESKTOP_QR);
        ActivityController<DesktopLoginApprovalActivity> controller =
            Robolectric.buildActivity(DesktopLoginApprovalActivity.class, intent).create();
        try {
            View root = controller.get().getWindow().getDecorView();
            assertTrue(hasText(root, "正在读取登录请求"));
            assertFalse(hasText(root, "数字一致，批准登录"));
            assertFalse(hasText(root, "将电脑上的登录二维码放入框内"));
        } finally { controller.destroy(); }
    }

    @Test public void invalidHandoffShowsAnErrorInsteadOfLoadingOrOpeningCamera() {
        Intent intent = new Intent(RuntimeEnvironment.getApplication(), DesktopLoginApprovalActivity.class)
            .putExtra(DesktopLoginApprovalActivity.EXTRA_QR_URI, DESKTOP_QR + "&token=secret");
        ActivityController<DesktopLoginApprovalActivity> controller =
            Robolectric.buildActivity(DesktopLoginApprovalActivity.class, intent).create();
        try {
            View root = controller.get().getWindow().getDecorView();
            assertTrue(hasText(root, "这不是 MaiChat 电脑登录二维码"));
            assertFalse(hasText(root, "正在读取登录请求"));
            assertFalse(hasText(root, "将电脑上的登录二维码放入框内"));
        } finally { controller.destroy(); }
    }

    private static ActivityController<FriendQrScanActivity> resumedScanner() {
        ActivityController<FriendQrScanActivity> controller =
            Robolectric.buildActivity(FriendQrScanActivity.class).create().start().resume();
        // 拒绝硬件权限会产生系统权限 Intent；先核对并取走它，再只观察业务路由。
        ShadowActivity.IntentForResult permission =
            Shadows.shadowOf(controller.get()).getNextStartedActivityForResult();
        assertNotNull(permission);
        assertEquals("android.content.pm.action.REQUEST_PERMISSIONS", permission.intent.getAction());
        return controller;
    }

    private static boolean hasText(View view, String text) {
        if (view instanceof TextView && text.contentEquals(((TextView) view).getText())) return true;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++)
                if (hasText(group.getChildAt(index), text)) return true;
        }
        return false;
    }

    /** 只替换无法在 JVM 打开摄像头的解码器对象，页面与普通分流入口仍用生产实现。 */
    @Implements(value = BarcodeScanning.class, isInAndroidSdk = false)
    public static final class ShadowBarcodeScanning {
        @Implementation protected static BarcodeScanner getClient(BarcodeScannerOptions options) {
            return (BarcodeScanner) Proxy.newProxyInstance(BarcodeScanner.class.getClassLoader(),
                new Class<?>[]{BarcodeScanner.class}, (proxy, method, arguments) -> {
                    if (method.getName().equals("process")) return Tasks.forResult(Collections.emptyList());
                    if (method.getName().equals("hashCode")) return System.identityHashCode(proxy);
                    if (method.getName().equals("equals")) return proxy == arguments[0];
                    if (method.getName().equals("toString")) return "JvmQrScanner";
                    return null;
                });
        }
    }
}
