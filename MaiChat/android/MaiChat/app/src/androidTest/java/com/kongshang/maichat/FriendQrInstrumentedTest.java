// 文件作用：用自有二维码验证统一扫码的真实解码、好友结果及桌面登录路由边界。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.lifecycle.Lifecycle;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import com.google.android.gms.tasks.Tasks;
import com.google.mlkit.vision.barcode.BarcodeScanner;
import com.google.mlkit.vision.barcode.BarcodeScannerOptions;
import com.google.mlkit.vision.barcode.BarcodeScanning;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.common.InputImage;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.common.BitMatrix;
import com.google.zxing.qrcode.QRCodeWriter;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public class FriendQrInstrumentedTest {
    private static final String REQUEST_ID = "6aaf55c7-8b63-4a62-9fd1-122dd7d5652a";
    private static final String DESKTOP_QR = "maichat://auth/desktop?request_id=" + REQUEST_ID;

    @Test public void scannerOpensWithFriendAndDesktopPrompt() throws Exception {
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.onActivity(activity -> assertTrue(
                hasText(activity.getWindow().getDecorView(), "扫描好友二维码或电脑登录二维码")));
        }
    }

    @Test public void generatedFriendCodeStillReturnsOnlyTheFriendAccount() throws Exception {
        String userId = "mtest_friend_12345";
        String decoded = decode(FriendQrPolicy.link(userId));
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        DesktopRouteMonitor monitor = new DesktopRouteMonitor();
        instrumentation.addMonitor(monitor);
        Context context = instrumentation.getTargetContext();
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launchActivityForResult(
                     new Intent(context, FriendQrScanActivity.class))) {
            scenario.onActivity(activity -> assertTrue(activity.acceptQrUri(decoded)));
            Instrumentation.ActivityResult result = scenario.getResult();
            assertEquals(Activity.RESULT_OK, result.getResultCode());
            assertNotNull(result.getResultData());
            assertEquals(userId, result.getResultData().getStringExtra("friend_user_id"));
            assertEquals(0, monitor.starts.get());
        } finally { instrumentation.removeMonitor(monitor); }
    }

    @Test public void generatedDesktopCodeOpensExistingApprovalExactlyOnce() throws Exception {
        String decoded = decode(DESKTOP_QR);
        assertEquals(REQUEST_ID, DesktopLoginQrUri.requestId(decoded));
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        DesktopRouteMonitor monitor = new DesktopRouteMonitor();
        instrumentation.addMonitor(monitor);
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.onActivity(activity -> {
                assertTrue("Desktop QR must leave the friend-only rejection path",
                    activity.acceptQrUri(decoded));
                assertFalse("A second frame must not start another approval",
                    activity.acceptQrUri(decoded));
            });
            assertEquals(1, monitor.starts.get());
            Intent routed = monitor.intent.get();
            assertNotNull(routed);
            assertNotNull(routed.getComponent());
            assertEquals(DesktopLoginApprovalActivity.class.getName(),
                routed.getComponent().getClassName());
            assertEquals(decoded, routed.getStringExtra(DesktopLoginApprovalActivity.EXTRA_QR_URI));
            assertFalse("A scan must never encode an approval decision",
                routed.hasExtra("approve"));
        } finally { instrumentation.removeMonitor(monitor); }
    }

    @Test public void invalidCodesCannotDispatchEitherAccountFlow() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        DesktopRouteMonitor monitor = new DesktopRouteMonitor();
        instrumentation.addMonitor(monitor);
        String[] invalid = {null, "", "https://example.invalid/qr", DESKTOP_QR + "&token=secret",
            DESKTOP_QR + "&request_id=" + REQUEST_ID, DESKTOP_QR + "#fragment",
            "maichat://auth:80/desktop?request_id=" + REQUEST_ID,
            FriendQrPolicy.link("mtest_friend_12345") + "&token=secret"};
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.onActivity(activity -> {
                for (String raw : invalid) assertFalse(activity.acceptQrUri(raw));
                assertFalse(activity.isFinishing());
            });
            assertEquals(0, monitor.starts.get());
        } finally { instrumentation.removeMonitor(monitor); }
    }

    @Test public void pausedScannerCannotDispatchLateFriendOrDesktopFrames() throws Exception {
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.moveToState(Lifecycle.State.CREATED);
            scenario.onActivity(activity -> {
                assertFalse(activity.acceptQrUri(FriendQrPolicy.link("mtest_friend_12345")));
                assertFalse(activity.acceptQrUri(DESKTOP_QR));
                assertFalse(activity.isFinishing());
            });
        }
    }

    @Test public void destroyedScannerCannotDispatchLateFrames() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        AtomicReference<FriendQrScanActivity> activity = new AtomicReference<>();
        try (ActivityScenario<FriendQrScanActivity> scenario =
                 ActivityScenario.launch(FriendQrScanActivity.class)) {
            scenario.onActivity(activity::set);
            scenario.moveToState(Lifecycle.State.DESTROYED);
            instrumentation.runOnMainSync(() -> {
                assertFalse(activity.get().acceptQrUri(FriendQrPolicy.link("mtest_friend_12345")));
                assertFalse(activity.get().acceptQrUri(DESKTOP_QR));
            });
        }
    }

    private boolean hasText(View view, String text) {
        if (view instanceof TextView && text.contentEquals(((TextView) view).getText())) return true;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++)
                if (hasText(group.getChildAt(index), text)) return true;
        }
        return false;
    }

    @Test public void generatedAccountCodeScansBackToSameUser() throws Exception {
        String userId = "mtest_friend_12345";
        assertEquals(userId, FriendQrPolicy.userId(decode(FriendQrPolicy.link(userId))));
    }

    private static String decode(String raw) throws Exception {
        BitMatrix matrix = new QRCodeWriter().encode(
            raw, BarcodeFormat.QR_CODE, 600, 600);
        Bitmap bitmap = Bitmap.createBitmap(600, 600, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < 600; y++)
            for (int x = 0; x < 600; x++)
                bitmap.setPixel(x, y, matrix.get(x, y) ? 0xFF000000 : 0xFFFFFFFF);
        BarcodeScanner scanner = BarcodeScanning.getClient(
            new BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build());
        try {
            List<Barcode> codes = Tasks.await(scanner.process(InputImage.fromBitmap(bitmap, 0)),
                20, TimeUnit.SECONDS);
            assertTrue("Own QR must be recognized", !codes.isEmpty());
            return codes.get(0).getRawValue();
        } finally { scanner.close(); bitmap.recycle(); }
    }

    /** 拦截确认 Activity，路由测试不会访问账号服务或批准任何实际桌面请求。 */
    private static final class DesktopRouteMonitor extends Instrumentation.ActivityMonitor {
        final AtomicInteger starts = new AtomicInteger();
        final AtomicReference<Intent> intent = new AtomicReference<>();

        @Override public Instrumentation.ActivityResult onStartActivity(Intent candidate) {
            if (candidate.getComponent() == null || !DesktopLoginApprovalActivity.class.getName()
                .equals(candidate.getComponent().getClassName())) return null;
            intent.set(new Intent(candidate));
            starts.incrementAndGet();
            return new Instrumentation.ActivityResult(Activity.RESULT_CANCELED, null);
        }
    }
}
