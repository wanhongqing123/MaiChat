// Verifies the production image preview's composed pixels, including the area outside the image.
// Uses an app-local synthetic image and never sends messages or changes account data or drafts.
package com.kongshang.maichat;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Instrumentation;
import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class MaiImagePreviewBackgroundInstrumentedTest {
    private static final String PREVIEW_DESCRIPTION = "图片预览，单击关闭，双击缩放";

    @Test public void completeWhiteImageHasBlackLetterboxingInProductionPreview() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context target = instrumentation.getTargetContext();
        File source = File.createTempFile("image-preview-background-", ".png", target.getCacheDir());
        MainActivity activity = null;
        Bitmap capture = null;
        try {
            Bitmap white = Bitmap.createBitmap(120, 120, Bitmap.Config.ARGB_8888);
            white.eraseColor(Color.WHITE);
            // A white center alone also matches unrelated screenshots. This unique 24 x 24
            // marker proves that the composed foreground belongs to this test's source.
            for (int y = 48; y < 72; y++) {
                for (int x = 48; x < 72; x++) white.setPixel(x, y, Color.MAGENTA);
            }
            try (FileOutputStream output = new FileOutputStream(source)) {
                assertTrue("Write the synthetic preview image",
                    white.compress(Bitmap.CompressFormat.PNG, 100, output));
            } finally {
                white.recycle();
            }
            Intent launch = new Intent(target, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            activity = (MainActivity) instrumentation.startActivitySync(launch);
            instrumentation.waitForIdleSync();
            MainActivity host = activity;
            awaitStableHost(instrumentation, host);
            instrumentation.runOnMainSync(() -> host.showFullScreenImage(source.getAbsolutePath()));

            UiAutomation automation = instrumentation.getUiAutomation();
            Rect viewport = new Rect();
            capture = awaitPresentedPreview(automation, viewport);
            // Retain an actual composed screenshot on both RED and GREEN runs for host inspection.
            File evidenceDirectory = target.getExternalFilesDir("image-preview-background-regression");
            assertNotNull("External app files must be available for screenshot evidence", evidenceDirectory);
            assertTrue("Create screenshot evidence directory",
                evidenceDirectory.isDirectory() || evidenceDirectory.mkdirs());
            File evidence = File.createTempFile("preview-", ".png", evidenceDirectory);
            try (FileOutputStream output = new FileOutputStream(evidence)) {
                assertTrue(capture.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            Bundle result = new Bundle();
            result.putString("imagePreviewBackgroundScreenshot", evidence.getAbsolutePath());
            instrumentation.sendStatus(0, result);

            int centerX = viewport.centerX(), centerY = viewport.centerY();
            int imageHalfSize = Math.min(viewport.width(), viewport.height()) / 2;
            assertPatch(capture, centerX, centerY, Color.MAGENTA, "Presented test image marker");
            // The square must remain complete and fitted, rather than being stretched into the
            // letterbox. Sample its interior near all four sides, away from the action buttons.
            int inner = imageHalfSize * 3 / 4;
            assertPatch(capture, centerX - inner, centerY, Color.WHITE, "Image left interior");
            assertPatch(capture, centerX + inner, centerY, Color.WHITE, "Image right interior");
            assertPatch(capture, centerX, centerY - inner, Color.WHITE, "Image top interior");
            assertPatch(capture, centerX, centerY + inner, Color.WHITE, "Image bottom interior");

            if (viewport.height() > viewport.width()) {
                int margin = (viewport.height() - viewport.width()) / 2;
                int inset = Math.min(viewport.height() / 8, margin / 2);
                assertTrue("Portrait preview must have measurable top and bottom letterboxing", inset > 3);
                assertPatch(capture, centerX, viewport.top + inset, Color.BLACK, "Top letterbox");
                assertPatch(capture, centerX, viewport.bottom - inset - 1, Color.BLACK, "Bottom letterbox");
            } else {
                int margin = (viewport.width() - viewport.height()) / 2;
                int inset = Math.min(viewport.width() / 8, margin / 2);
                assertTrue("Landscape preview must have measurable left and right letterboxing", inset > 3);
                assertPatch(capture, viewport.left + inset, centerY, Color.BLACK, "Left letterbox");
                assertPatch(capture, viewport.right - inset - 1, centerY, Color.BLACK, "Right letterbox");
            }
        } finally {
            if (capture != null) capture.recycle();
            if (activity != null) {
                UiAutomation automation = instrumentation.getUiAutomation();
                Rect preview = previewBounds(automation);
                // Accessibility labels are shared by every preview. Never Back an unrelated
                // image merely because it exposes the same label as this test's dialog.
                if (preview != null && hasTestMarker(automation, preview)) {
                    instrumentation.sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);
                    instrumentation.waitForIdleSync();
                    SystemClock.sleep(450);
                }
                // finish() also dismisses the Activity's dialogs. Leave an unrelated preview
                // untouched if ownership was not proven, including on a readiness timeout.
                if (previewBounds(automation) == null) {
                    MainActivity host = activity;
                    instrumentation.runOnMainSync(host::finish);
                }
            }
            source.delete();
        }
    }

    private static Bitmap awaitPresentedPreview(UiAutomation automation, Rect viewport) {
        long deadline = SystemClock.uptimeMillis() + 15_000;
        long markerSince = 0;
        while (SystemClock.uptimeMillis() < deadline) {
            Rect bounds = previewBounds(automation);
            Bitmap screenshot = bounds == null ? null : automation.takeScreenshot();
            if (screenshot != null && bounds.intersect(0, 0, screenshot.getWidth(), screenshot.getHeight())
                    && isPatch(screenshot, bounds.centerX(), bounds.centerY(), Color.MAGENTA)) {
                // Require the preview accessibility root AND this exact source's marker, then let
                // the independent background presenter settle before checking its letterboxing.
                if (markerSince == 0) markerSince = SystemClock.uptimeMillis();
                if (SystemClock.uptimeMillis() - markerSince >= 750) {
                    viewport.set(bounds);
                    return screenshot;
                }
            } else {
                markerSince = 0;
            }
            if (screenshot != null) screenshot.recycle();
            SystemClock.sleep(100);
        }
        throw new AssertionError("Production preview root and this source's stable magenta marker were not presented within 15 seconds");
    }

    private static void awaitStableHost(Instrumentation instrumentation, MainActivity activity)
            throws Exception {
        // render() dismisses image dialogs when the account generation changes. Observe the
        // existing restore state without issuing auth requests, altering preferences or logging
        // its private identity, so the test does not open a preview during that transition.
        Field sessionField = MainActivity.class.getDeclaredField("session");
        sessionField.setAccessible(true);
        AtomicReference<String> identity = new AtomicReference<>();
        String previousIdentity = null;
        long stableSince = 0;
        long deadline = SystemClock.uptimeMillis() + 20_000;
        while (SystemClock.uptimeMillis() < deadline) {
            instrumentation.runOnMainSync(() -> {
                try {
                    RemoteIMSessionController session = (RemoteIMSessionController) sessionField.get(activity);
                    identity.set(!activity.isFinishing() && !activity.isDestroyed()
                        && activity.hasWindowFocus() && activity.getWindow().getDecorView().isAttachedToWindow()
                        && session != null && !session.isRestoringAccount()
                        ? session.passwordChangeIdentity() : null);
                } catch (IllegalAccessException error) {
                    throw new IllegalStateException("Read host restore readiness", error);
                }
            });
            String observed = identity.get();
            if (observed != null && observed.equals(previousIdentity)) {
                if (SystemClock.uptimeMillis() - stableSince >= 750) return;
            } else {
                stableSince = SystemClock.uptimeMillis();
                previousIdentity = observed;
            }
            SystemClock.sleep(100);
        }
        throw new AssertionError("Test Activity focus and restored account generation did not stabilize within 20 seconds");
    }

    private static boolean hasTestMarker(UiAutomation automation, Rect bounds) {
        Bitmap screenshot = automation.takeScreenshot();
        if (screenshot == null) return false;
        try {
            return isPatch(screenshot, bounds.centerX(), bounds.centerY(), Color.MAGENTA);
        } finally {
            screenshot.recycle();
        }
    }

    private static Rect previewBounds(UiAutomation automation) {
        AccessibilityNodeInfo root = automation.getRootInActiveWindow();
        if (root == null) return null;
        try {
            return findPreview(root);
        } finally {
            root.recycle();
        }
    }

    private static Rect findPreview(AccessibilityNodeInfo node) {
        if (PREVIEW_DESCRIPTION.contentEquals(node.getContentDescription() == null
                ? "" : node.getContentDescription()) && node.isVisibleToUser()) {
            Rect bounds = new Rect();
            node.getBoundsInScreen(bounds);
            return bounds.isEmpty() ? null : bounds;
        }
        for (int index = 0; index < node.getChildCount(); index++) {
            AccessibilityNodeInfo child = node.getChild(index);
            if (child == null) continue;
            try {
                Rect found = findPreview(child);
                if (found != null) return found;
            } finally {
                child.recycle();
            }
        }
        return null;
    }

    private static void assertPatch(Bitmap bitmap, int x, int y, int expected, String label) {
        int observed = bitmap.getPixel(x, y);
        assertTrue(label + " at " + x + "," + y + ": expected RGB "
            + Integer.toHexString(expected) + ", observed " + Integer.toHexString(observed),
            isPatch(bitmap, x, y, expected));
    }

    private static boolean isPatch(Bitmap bitmap, int x, int y, int expected) {
        if (x < 3 || y < 3 || x + 3 >= bitmap.getWidth() || y + 3 >= bitmap.getHeight()) return false;
        for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
                int pixel = bitmap.getPixel(x + dx, y + dy);
                if (Math.abs(Color.red(pixel) - Color.red(expected)) > 3
                        || Math.abs(Color.green(pixel) - Color.green(expected)) > 3
                        || Math.abs(Color.blue(pixel) - Color.blue(expected)) > 3) return false;
            }
        }
        return true;
    }
}
