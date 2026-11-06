// 文件作用：用本地合成图和公开人像样例验证 Android 原尺寸皮肤分块、方向及输出登记。
// 运行边界：不发送模型请求、不使用真实账号；大图夹具释放后才执行正式宿主处理路径。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorSpace;
import android.graphics.Rect;
import android.media.ExifInterface;
import android.os.Debug;
import android.os.SystemClock;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.kongshang.maichat.tools.MobileImageVision;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class PortraitRegionInstrumentedTest {
    private static final int SOURCE_WIDTH = 4284;
    private static final int SOURCE_HEIGHT = 5712;
    private static final int STRIP_ROWS = 64;

    @Test
    public void allExifOrientationsPreserveExactPixelsAndRegionCoordinates() throws Exception {
        File directory = testDirectory("exif");
        try {
            for (int orientation = 1; orientation <= 8; ++orientation) {
                File source = new File(directory, "orientation-" + orientation + ".jpg");
                writePatternJpeg(source, 37, 23);
                setOrientation(source, orientation);
                byte[] before = sha256(source);
                Bitmap raw = BitmapFactory.decodeFile(source.getPath(), decodeOptions());
                assertNotNull(raw);
                MaiPortraitBitmap.Source metadata = MaiPortraitBitmap.inspect(source);
                assertEquals(orientation, metadata.orientation);
                assertEquals(orientation >= 5 ? 23 : 37, metadata.width);
                assertEquals(orientation >= 5 ? 37 : 23, metadata.height);
                Bitmap upright = null;
                Bitmap proxy = null;
                Bitmap region = null;
                BitmapRegionDecoder decoder = null;
                try {
                    upright = MaiPortraitBitmap.decodeMutable(metadata);
                    proxy = MaiPortraitBitmap.proxy(metadata, 128);
                    assertTrue("Final source must remain mutable", upright.isMutable());
                    assertEquals(Bitmap.Config.ARGB_8888, upright.getConfig());
                    assertEquals(ColorSpace.get(ColorSpace.Named.SRGB), upright.getColorSpace());
                    for (int y = 0; y < raw.getHeight(); ++y) {
                        for (int x = 0; x < raw.getWidth(); ++x) {
                            int[] point = orientedPixel(orientation, x, y, raw.getWidth(), raw.getHeight());
                            String label = "EXIF " + orientation + " raw pixel " + x + "," + y;
                            assertEquals(label, raw.getPixel(x, y), upright.getPixel(point[0], point[1]));
                            assertEquals(label + " proxy", raw.getPixel(x, y), proxy.getPixel(point[0], point[1]));
                            Rect one = new Rect(x, y, x + 1, y + 1);
                            Rect expected = new Rect(point[0], point[1], point[0] + 1, point[1] + 1);
                            assertEquals(label + " pixel edges", expected,
                                MaiPortraitBitmap.rawToUpright(metadata, one));
                            assertEquals(label + " inverse edges", one,
                                MaiPortraitBitmap.uprightToRaw(metadata, expected));
                        }
                    }
                    // 非对称、非整 MCU 边界的区域能发现 transpose/transverse 或 off-by-one 错误。
                    Rect crop = new Rect(3, 4, metadata.width - 2, metadata.height - 3);
                    decoder = BitmapRegionDecoder.newInstance(source.getPath(), false);
                    region = MaiPortraitBitmap.region(metadata, decoder, crop);
                    for (int y = 0; y < crop.height(); ++y)
                        for (int x = 0; x < crop.width(); ++x)
                            assertEquals("EXIF " + orientation + " region " + x + "," + y,
                                upright.getPixel(crop.left + x, crop.top + y), region.getPixel(x, y));
                    assertArrayEquals("Direction handling modified the source", before, sha256(source));
                } finally {
                    if (region != null) region.recycle();
                    if (decoder != null) decoder.recycle();
                    if (proxy != null) proxy.recycle();
                    if (upright != null) upright.recycle();
                    raw.recycle();
                }
            }
            assertJpegMcuContextAndStripComposition(directory);
        } finally { deleteChildren(directory); }
    }

    private static void assertJpegMcuContextAndStripComposition(File directory) throws Exception {
        File source = new File(directory, "mcu-context.jpg");
        writePatternJpeg(source, 2049, 1031);
        setOrientation(source, ExifInterface.ORIENTATION_ROTATE_90);
        Bitmap raw = BitmapFactory.decodeFile(source.getPath(), decodeOptions());
        MaiPortraitBitmap.Source metadata = MaiPortraitBitmap.inspect(source);
        Bitmap upright = null;
        BitmapRegionDecoder decoder = null;
        try {
            upright = MaiPortraitBitmap.decodeMutable(metadata);
            int[] row = new int[raw.getWidth()], column = new int[row.length];
            for (int y = 0; y < raw.getHeight(); ++y) {
                raw.getPixels(row, 0, row.length, 0, y, row.length, 1);
                upright.getPixels(column, 0, 1, raw.getHeight() - 1 - y, 0, 1, raw.getWidth());
                assertArrayEquals("Strip composition must equal complete JPEG decode at raw row " + y,
                    row, column);
            }
            decoder = BitmapRegionDecoder.newInstance(source.getPath(), false);
            // Native JPEG crop alone changes the first/last columns, even at an MCU boundary.
            // A halo must reproduce complete decode exactly, including rotated validation strips.
            for (Rect rawBounds : new Rect[] {new Rect(2, 4, 34, 20),
                    new Rect(64, 0, 128, raw.getHeight())}) {
                Rect bounds = MaiPortraitBitmap.rawToUpright(metadata, rawBounds);
                Bitmap region = MaiPortraitBitmap.region(metadata, decoder, bounds);
                try {
                    for (int y = 0; y < bounds.height(); ++y)
                        for (int x = 0; x < bounds.width(); ++x)
                            assertEquals("JPEG context pixel " + (bounds.left + x) + "," + (bounds.top + y),
                                upright.getPixel(bounds.left + x, bounds.top + y), region.getPixel(x, y));
                } finally { region.recycle(); }
            }
        } finally {
            if (decoder != null) decoder.recycle();
            if (upright != null) upright.recycle();
            raw.recycle();
        }
    }

    @Test
    public void highResolutionHostEditKeepsOriginalBackgroundAndResultLineage() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AIAssistantController controller = readyController(scenario);
            File original = controller.workspaceFile("portrait-region-" + UUID.randomUUID() + ".jpg");
            File mildFile = null, strongFile = null, mildMask = null, strongMask = null;
            try {
                writeHighResolutionJpeg(original);
                byte[] before = sha256(original);
                MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(original);
                assertEquals(ExifInterface.ORIENTATION_ROTATE_90, source.orientation);
                assertEquals(SOURCE_WIDTH, source.width);
                assertEquals(SOURCE_HEIGHT, source.height);
                assertTrue((long) source.width * source.height > 12_000_000L);
                org.json.JSONArray faces = MobileImageVision.detectFaces(original).getJSONArray("faces");
                assertEquals("Overlapping windows must reject the shoulder-only crop candidate", 1,
                    faces.length());
                JSONObject face = faces.getJSONObject(0);
                assertTrue("Existing ordinary detector yaw field missing", face.has("yaw_degrees"));
                assertTrue("Existing ordinary detector roll field missing", face.has("roll_degrees"));
                assertTrue(Double.isFinite(face.getDouble("yaw_degrees")));
                assertTrue(Double.isFinite(face.getDouble("roll_degrees")));
                dumpAnalyzeProxyCounterfactual(source, faces, beautify(original.getName(), 0.25));
                // writeHighResolutionJpeg has recycled its only full-sized fixture and detached Canvas.
                // These measurements document the test process; they are not a substitute for a heap trace.
                Runtime.getRuntime().gc();
                System.runFinalization();
                logMemory("fixture-released");
                long started = SystemClock.elapsedRealtime();
                AIAssistantController.ImportedFile mild = controller.transformImageForHost(
                    beautify(original.getName(), 0.25));
                JSONObject mildReceipt = controller.portraitReceiptForHost(mild.relativePath);
                assertEquals("image/png", mild.mimeType);
                assertEquals("Receipt is delivered once", 0,
                    controller.portraitReceiptForHost(mild.relativePath).length());
                mildFile = controller.workspaceImageForHost(mild.relativePath);
                mildMask = controller.workspaceImageForHost(mildReceipt.getString("mask_path"));
                assertNativeReceipt(mildReceipt, original, 0.25, source.width, source.height);
                assertEquals(1, mildReceipt.getInt("face_count"));
                assertUprightPng(mildFile, source.width, source.height);
                assertGrayscaleMask(mildMask, source.width, source.height);
                assertArrayEquals("First edit changed the original file", before, sha256(original));

                // Editing an output must resolve the recorded original, not apply a second whitening pass.
                AIAssistantController.ImportedFile strong = controller.transformImageForHost(
                    beautify(mild.relativePath, 0.75));
                JSONObject strongReceipt = controller.portraitReceiptForHost(strong.relativePath);
                strongFile = controller.workspaceImageForHost(strong.relativePath);
                strongMask = controller.workspaceImageForHost(strongReceipt.getString("mask_path"));
                assertNativeReceipt(strongReceipt, original, 0.75, source.width, source.height);
                assertEquals(1, strongReceipt.getInt("face_count"));
                for (JSONObject partial : new JSONObject[] {mildReceipt, strongReceipt}) {
                    assertFalse("Overlapping person seeds must not claim reliable body ownership",
                        partial.getBoolean("person_ownership_reliable"));
                    assertEquals("face_skin_only", partial.getString("skin_scope"));
                    assertEquals("overlapping_person_seeds", partial.getString("body_ownership_skip_reason"));
                    assertFalse("A face-only result must not claim body cleanup", partial.getBoolean("clean_applied"));
                }
                assertNotEquals(mildReceipt.getString("result_id"), strongReceipt.getString("result_id"));
                assertEquals(receiptRect(mildReceipt), receiptRect(strongReceipt));
                assertEquals(mildReceipt.getDouble("skin_L_before"),
                    strongReceipt.getDouble("skin_L_before"), 0.0001);
                assertEquals(mildReceipt.getDouble("skin_C_before"),
                    strongReceipt.getDouble("skin_C_before"), 0.0001);
                assertTrue("Stronger whitening must lift skin L* further",
                    strongReceipt.getDouble("skin_L_after") > mildReceipt.getDouble("skin_L_after"));
                assertTrue("Stronger whitening must reduce skin C* further",
                    strongReceipt.getDouble("skin_C_after") < mildReceipt.getDouble("skin_C_after"));
                assertUprightPng(strongFile, source.width, source.height);
                assertGrayscaleMask(strongMask, source.width, source.height);
                assertFalse("Eye protection regression must use color processing without geometry",
                    strongReceipt.getBoolean("geometry_applied"));
                assertEyeCentersProtected(source, strongFile, strongMask, face.getJSONObject("landmarks"));
                PixelMetrics pixels = compareInStrips(source, mildFile, strongFile,
                    strongMask);
                assertTrue("No visible native skin edit", pixels.changed > 100);
                // Saved pixels must be the core's composed output; an extra host mask pass can
                // discard low-alpha changes and would make these native changed-pixel counts drift.
                assertEquals(strongReceipt.getLong("changed_pixel_count"), pixels.changed);
                assertEquals(mildReceipt.getLong("changed_pixel_count"), pixels.mildChanged);
                assertTrue("Actual saved skin pixels did not gain L*", pixels.strongL > pixels.originalL);
                assertTrue("Actual saved skin pixels did not lose C*", pixels.strongC < pixels.originalC);
                assertTrue("Saved stronger result did not gain more L*", pixels.strongL > pixels.mildL);
                assertTrue("Saved stronger result did not lose more C*", pixels.strongC < pixels.mildC);
                assertArrayEquals("Result-lineage edit changed the original file", before, sha256(original));
                assertNoTemporaryOutputs(original.getParentFile());
                File cache = InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir();
                java.nio.file.Files.copy(original.toPath(), new File(cache,
                    "portrait-region-original-exif6.jpg").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                java.nio.file.Files.copy(strongFile.toPath(), new File(cache,
                    "portrait-region-whitened.png").toPath(),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                java.nio.file.Files.copy(strongMask.toPath(), new File(cache,
                    "portrait-region-skin-mask.png").toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                java.nio.file.Files.write(new File(cache, "portrait-region-receipts.json").toPath(),
                    new JSONObject().put("mild", mildReceipt).put("strong", strongReceipt).toString(2)
                        .getBytes(java.nio.charset.StandardCharsets.UTF_8));
                logMemory("edits-and-strip-verification-complete");
                System.out.println("PortraitRegion 2447万像素/EXIF6: "
                    + (SystemClock.elapsedRealtime() - started) + " ms; ROI="
                    + strongReceipt.getJSONObject("roi") + "; changed=" + pixels.changed
                    + "; saved L*=" + pixels.originalL / pixels.changed + " -> "
                    + pixels.mildL / pixels.changed + " -> " + pixels.strongL / pixels.changed
                    + "; saved C*=" + pixels.originalC / pixels.changed + " -> "
                    + pixels.mildC / pixels.changed + " -> " + pixels.strongC / pixels.changed);
            } finally {
                if (strongMask != null) strongMask.delete();
                if (mildMask != null) mildMask.delete();
                if (strongFile != null) strongFile.delete();
                if (mildFile != null) mildFile.delete();
                original.delete();
            }
        }
    }

    @Test
    public void bodySkinUsesFullSourceMaskAndZeroParametersPreservePixels() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AIAssistantController controller = readyController(scenario);
            File original = controller.workspaceFile("portrait-skin-body-" + UUID.randomUUID() + ".jpg");
            File zeroFile = null, editedFile = null, zeroMask = null, editedMask = null;
            try {
                copyAsset("portrait-test-body.jpg", original);
                byte[] before = sha256(original);
                MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(original);
                assertEquals("Native protection coordinates require the staged public pose fixture", 1000,
                    source.width);
                assertEquals(667, source.height);
                JSONObject zeroReceipt = controller.transformImageResultForHost(
                    beautify(original.getName(), 0));
                assertEquals("agent_send_media", zeroReceipt.getString("next_tool"));
                assertEquals("image/png", zeroReceipt.getString("mime_type"));
                zeroFile = controller.workspaceImageForHost(zeroReceipt.getString("path"));
                zeroMask = controller.workspaceImageForHost(zeroReceipt.getString("mask_path"));
                assertNativeReceipt(zeroReceipt, original, 0, source.width, source.height);
                assertUprightPng(zeroFile, source.width, source.height);
                assertGrayscaleMask(zeroMask, source.width, source.height);

                JSONObject receipt = controller.transformImageResultForHost(
                    beautify(original.getName(), 0.6));
                assertEquals("agent_send_media", receipt.getString("next_tool"));
                assertEquals("image/png", receipt.getString("mime_type"));
                assertEquals(source.width, receipt.getInt("width"));
                assertEquals(source.height, receipt.getInt("height"));
                editedFile = controller.workspaceImageForHost(receipt.getString("path"));
                editedMask = controller.workspaceImageForHost(receipt.getString("mask_path"));
                assertNativeReceipt(receipt, original, 0.6, source.width, source.height);
                assertTrue("Body fixture did not receive an owned skin mask",
                    receipt.getBoolean("person_ownership_reliable"));
                assertTrue("Full-body native path did not report body skin",
                    receipt.getLong("body_skin_pixel_count") > 0);
                assertTrue("Body cleanup was not applied by the actual Android host",
                    receipt.getBoolean("clean_applied"));
                assertEquals("auto", receipt.getString("body_cleanup_request_mode"));
                assertEquals("selected_person_class2_body_interior", receipt.getString("clean_scope"));
                assertEquals(0.25, receipt.getDouble("whitening_shader_strength"), 0.00001);
                assertEquals("source_hue_chroma_cap", receipt.getString("whitening_color_policy"));
                // The fixed curve amplitude D=25*s*(2-s) is not each pixel's actual L* increase.
                assertEquals(21, receipt.getDouble("whitening_shadow_amount"), 0.0001);
                assertFalse("AUTO is a separate mode, not a string in the boolean parameter",
                    receipt.getJSONObject("parameters").has("remove_body_hair"));
                assertUprightPng(editedFile, source.width, source.height);
                assertGrayscaleMask(editedMask, source.width, source.height);
                assertBodyPixels(source, zeroFile, editedFile, editedMask, receipt);
                assertArrayEquals("Full-body edit changed the original file", before, sha256(original));
                assertNoTemporaryOutputs(original.getParentFile());
                File cache = InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir();
                for (File file : new File[] {original, editedFile, editedMask}) {
                    String name = file == original ? "portrait-body-original.jpg"
                        : file == editedFile ? "portrait-body-whitened.png" : "portrait-body-skin-mask.png";
                    java.nio.file.Files.copy(file.toPath(), new File(cache, name).toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                }
                java.nio.file.Files.write(new File(cache, "portrait-body-receipt.json").toPath(),
                    receipt.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                showBodyInNormalMainActivity(new File(cache, "portrait-body-whitened.png"));
            } finally {
                for (File file : new File[] {editedMask, zeroMask, editedFile, zeroFile, original})
                    if (file != null) file.delete();
            }
        }
    }

    // This fixture invokes the normal MainActivity's existing production preview with the
    // actual saved body result. It does not simulate an Agent conversation or model delivery.
    private static void showBodyInNormalMainActivity(File output) throws Exception {
        android.app.UiAutomation automation = InstrumentationRegistry.getInstrumentation().getUiAutomation();
        AtomicReference<Throwable> recordingError = new AtomicReference<>();
        Thread recorder = new Thread(() -> {
            try (InputStream command = new android.os.ParcelFileDescriptor.AutoCloseInputStream(
                    automation.executeShellCommand("screenrecord --time-limit 16 /data/local/tmp/maichat-portrait-body268-ui.mp4"))) {
                byte[] buffer = new byte[1024];
                while (command.read(buffer) != -1) { /* Drain the recorder until its bounded duration ends. */ }
            } catch (Throwable error) { recordingError.set(error); }
        }, "portrait-body-ui-recording");
        recorder.start();
        Thread.sleep(500);
        try (ActivityScenario<MainActivity> normal = ActivityScenario.launch(MainActivity.class)) {
            normal.onActivity(activity -> activity.showFullScreenImage(output.getPath()));
            long deadline = SystemClock.elapsedRealtime() + 10_000;
            AtomicBoolean presented = new AtomicBoolean();
            while (!presented.get() && SystemClock.elapsedRealtime() < deadline) {
                normal.onActivity(activity -> {
                    try {
                        java.lang.reflect.Field viewField = MainActivity.class.getDeclaredField("galleryImageView");
                        viewField.setAccessible(true);
                        MaiGraphicsImageView view = (MaiGraphicsImageView) viewField.get(activity);
                        Bitmap pixels = view == null ? null : view.snapshot();
                        if (pixels != null) { presented.set(true); pixels.recycle(); }
                    } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
                });
                if (!presented.get()) Thread.sleep(100);
            }
            assertTrue("Normal MainActivity did not present the actual body output", presented.get());
            InstrumentationRegistry.getInstrumentation().waitForIdleSync();
            Thread.sleep(2000);
            // Acknowledge only the observed Android SystemUI tutorial through its actual
            // visible button. Do not hide overlays through settings or accept a covered image.
            android.view.accessibility.AccessibilityNodeInfo root = automation.getRootInActiveWindow();
            if (root != null) {
                try {
                    for (android.view.accessibility.AccessibilityNodeInfo button : root.findAccessibilityNodeInfosByText("Got it")) {
                        try {
                            if ("com.android.systemui".contentEquals(button.getPackageName() == null ? "" : button.getPackageName())
                                    && "Got it".contentEquals(button.getText() == null ? "" : button.getText())) {
                                assertTrue("Observed SystemUI tutorial could not be acknowledged",
                                    button.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK));
                                Thread.sleep(500);
                            }
                        } finally { button.recycle(); }
                    }
                } finally { root.recycle(); }
            }
            saveNormalScreenshot("portrait-body-normal-main-preview.png");
            // Use a real tap through the production gesture recognizer, then inspect the
            // actual returned page. A system overlay that intercepts it must fail visibly.
            android.util.DisplayMetrics metrics = InstrumentationRegistry.getInstrumentation()
                .getTargetContext().getResources().getDisplayMetrics();
            long eventTime = SystemClock.uptimeMillis();
            android.view.MotionEvent down = android.view.MotionEvent.obtain(eventTime, eventTime,
                android.view.MotionEvent.ACTION_DOWN, metrics.widthPixels / 2f, metrics.heightPixels / 2f, 0);
            android.view.MotionEvent up = android.view.MotionEvent.obtain(eventTime, eventTime + 80,
                android.view.MotionEvent.ACTION_UP, metrics.widthPixels / 2f, metrics.heightPixels / 2f, 0);
            try {
                InstrumentationRegistry.getInstrumentation().sendPointerSync(down);
                InstrumentationRegistry.getInstrumentation().sendPointerSync(up);
            } finally { down.recycle(); up.recycle(); }
            Thread.sleep(1000);
            saveNormalScreenshot("portrait-body-normal-main-after-close.png");
            normal.onActivity(activity -> {
                try {
                    java.lang.reflect.Field field = MainActivity.class.getDeclaredField("imageDialog");
                    field.setAccessible(true);
                    android.app.Dialog dialog = (android.app.Dialog) field.get(activity);
                    assertTrue("Actual preview tap did not return to the normal App page",
                        dialog == null || !dialog.isShowing());
                } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
            });
        } finally {
            recorder.join(20_000);
        }
        assertFalse("Screen recording did not finish", recorder.isAlive());
        if (recordingError.get() != null) throw new AssertionError("Actual App screen recording failed", recordingError.get());
    }

    private static void saveNormalScreenshot(String name) throws Exception {
        Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
        assertNotNull("Normal App screenshot is unavailable", screenshot);
        // 保留实际运行截图，避免低存储时系统回收 cache 后无法检查画面。
        File directory = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getFilesDir(),
            "PortraitTestArtifacts");
        assertTrue(directory.isDirectory() || directory.mkdirs());
        File target = new File(directory, name);
        try (FileOutputStream stream = new FileOutputStream(target)) {
            assertTrue("Cannot save actual normal App screenshot", screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
        } finally { screenshot.recycle(); }
    }

    @Test
    public void cancelledPublicationAndRejectedCommitRemoveImageAndMask() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                ActivityScenario.launch(AIAssistantTestActivity.class)) {
            // Readiness configures the bundled native models; this ordinary API test then uses
            // isolated local paths and a fake committer, without registering a portrait result.
            readyController(scenario);
            File directory = testDirectory("cancel-publish");
            File profile = testDirectory("cancel-profile");
            try {
                File original = new File(directory, "original.jpg");
                copyAsset("portrait-test-face.jpg", original);
                byte[] before = sha256(original);
                Set<String> files = fileNames(directory);
                AtomicBoolean publishedMaskSeen = new AtomicBoolean();
                AtomicInteger commits = new AtomicInteger();
                try {
                    MaiPortraitImage.process(original, profile, directory, beautify(original.getName(), 0),
                        () -> {
                            boolean published = hasPublishedMask(directory);
                            if (published) publishedMaskSeen.set(true);
                            return !published;
                        }, (metadata, output) -> {
                            commits.incrementAndGet();
                            return new JSONObject();
                        });
                    fail("Account cancellation between mask and image publication was accepted");
                } catch (java.io.IOException expected) {
                    assertTrue(expected.getMessage(), expected.getMessage() != null
                        && expected.getMessage().contains("account or workspace changed"));
                }
                assertTrue("Cancellation did not reach the publication boundary", publishedMaskSeen.get());
                assertEquals("Cancelled processing registered a result", 0, commits.get());
                assertEquals("Cancellation left an image or mask in the workspace", files, fileNames(directory));
                assertNoTemporaryOutputs(directory);

                try {
                    MaiPortraitImage.process(original, profile, directory, beautify(original.getName(), 0),
                        () -> true, (metadata, output) -> {
                            commits.incrementAndGet();
                            assertTrue("Commit was called before the image was complete", output.isFile());
                            assertTrue("Commit was called before the mask was complete", hasPublishedMask(directory));
                            throw new java.io.IOException("portrait-test-commit-rejected");
                        });
                    fail("A rejected result commit was accepted");
                } catch (java.io.IOException expected) {
                    assertEquals("portrait-test-commit-rejected", expected.getMessage());
                }
                assertEquals(1, commits.get());
                assertEquals("Rejected commit left an image or mask in the workspace", files, fileNames(directory));
                assertArrayEquals("Failed processing changed the source", before, sha256(original));
                assertNoTemporaryOutputs(directory);
            } finally {
                deleteChildren(directory);
                deleteChildren(profile);
            }
        }
    }

    @Test
    public void diagnosticMaskPngPreservesEveryEightBitSample() throws Exception {
        File directory = testDirectory("mask-samples");
        try {
            byte[] samples = new byte[256];
            for (int index = 0; index < samples.length; ++index) samples[index] = (byte) index;
            File mask = new File(directory, "samples.png");
            MaiPortraitImage.writeMask(mask, samples, 16, 16, () -> true);
            assertGrayscaleMask(mask, 16, 16);
            Bitmap pixels = BitmapFactory.decodeFile(mask.getPath(), decodeOptions());
            assertNotNull(pixels);
            try {
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < 16; ++x) {
                        int expected = y * 16 + x;
                        int pixel = pixels.getPixel(x, y);
                        assertEquals("Diagnostic mask changed sample " + expected,
                            Color.rgb(expected, expected, expected), pixel);
                    }
            } finally { pixels.recycle(); }
        } finally { deleteChildren(directory); }
    }

    @Test
    public void faceGeometryStillRunsAfterZeroStrengthSkinTiles() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AIAssistantController controller = readyController(scenario);
            File original = controller.workspaceFile("portrait-skin-geometry-" + UUID.randomUUID() + ".jpg");
            File edited = null, mask = null;
            Bitmap beforePixels = null, afterPixels = null;
            try {
                copyAsset("portrait-test-face.jpg", original);
                byte[] before = sha256(original);
                MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(original);
                AIAssistantController.ImportedFile result = controller.transformImageForHost(
                    beautify(original.getName(), 0).put("slim_face", 0.2).put("enlarge_eyes", 0.2));
                JSONObject receipt = controller.portraitReceiptForHost(result.relativePath);
                edited = controller.workspaceImageForHost(result.relativePath);
                mask = controller.workspaceImageForHost(receipt.getString("mask_path"));
                assertNativeReceipt(receipt, original, 0, source.width, source.height);
                assertTrue("SkinPlan discarded the requested existing face geometry", receipt.getBoolean("geometry_applied"));
                assertEquals("skin_color_and_smoothing_before_optional_face_geometry",
                    receipt.getString("metrics_scope"));
                JSONArray bounds = receipt.getJSONArray("geometry_processing_bbox");
                assertEquals(4, bounds.length());
                Rect region = new Rect(bounds.getInt(0), bounds.getInt(1),
                    bounds.getInt(0) + bounds.getInt(2), bounds.getInt(1) + bounds.getInt(3));
                assertTrue(region.width() > 0 && region.width() <= 2048);
                assertTrue(region.height() > 0 && region.height() <= 2048);
                assertTrue(new Rect(0, 0, source.width, source.height).contains(region));
                assertUprightPng(edited, source.width, source.height);
                assertGrayscaleMask(mask, source.width, source.height);
                beforePixels = MaiPortraitBitmap.decodeMutable(source);
                afterPixels = BitmapFactory.decodeFile(edited.getPath(), decodeOptions());
                assertNotNull(afterPixels);
                long changed = 0;
                for (int y = 0; y < source.height; ++y)
                    for (int x = 0; x < source.width; ++x) {
                        int pixel = beforePixels.getPixel(x, y), warped = afterPixels.getPixel(x, y);
                        assertEquals("Face geometry changed alpha", Color.alpha(pixel), Color.alpha(warped));
                        if (pixel == warped) continue;
                        assertTrue("Face geometry changed a pixel outside its declared region", region.contains(x, y));
                        ++changed;
                    }
                assertTrue("Face geometry did not reach the saved image", changed > 0);
                assertArrayEquals(before, sha256(original));
                assertNoTemporaryOutputs(original.getParentFile());
            } finally {
                if (afterPixels != null) afterPixels.recycle();
                if (beforePixels != null) beforePixels.recycle();
                if (mask != null) mask.delete();
                if (edited != null) edited.delete();
                original.delete();
            }
        }
    }

    @Test
    public void malformedArgumentsAndNonFaceFailWithoutPublishingOutput() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AIAssistantController controller = readyController(scenario);
            File original = controller.workspaceFile("portrait-region-invalid-" + UUID.randomUUID() + ".png");
            Bitmap flat = Bitmap.createBitmap(320, 240, Bitmap.Config.ARGB_8888);
            flat.eraseColor(Color.rgb(54, 74, 94));
            try { writeBitmap(flat, original, Bitmap.CompressFormat.PNG); }
            finally { flat.recycle(); }
            try {
                byte[] before = sha256(original);
                Set<String> files = fileNames(original.getParentFile());
                assertHostFailure(controller, beautify(original.getName(), 0.5)
                    .put("invalid_string", "rejected-before-analysis"), "unknown_parameter");
                assertHostFailure(controller, beautify(original.getName(), 0.5)
                    .put("whitening", "high"), "invalid_parameters");
                assertHostFailure(controller, beautify(original.getName(), 0.5)
                    .put("subject_index", -1), "invalid_parameters");
                assertHostFailure(controller, beautify(original.getName(), 0.5), "subject_required");
                assertEquals("Failed edits published workspace files", files, fileNames(original.getParentFile()));
                assertArrayEquals(before, sha256(original));
                assertNoTemporaryOutputs(original.getParentFile());
            } finally { original.delete(); }
        }
    }

    @Test
    public void transparentPixelsAndDecodeBudgetHaveExplicitOutcomes() throws Exception {
        File directory = testDirectory("alpha-budget");
        File transparent = new File(directory, "transparent.png");
        Bitmap fixture = Bitmap.createBitmap(19, 13, Bitmap.Config.ARGB_8888);
        try {
            for (int y = 0; y < fixture.getHeight(); ++y)
                for (int x = 0; x < fixture.getWidth(); ++x)
                    fixture.setPixel(x, y, Color.argb((x * 13 + y * 17) & 255,
                        160, (x * 11) & 255, (y * 19) & 255));
            writeBitmap(fixture, transparent, Bitmap.CompressFormat.PNG);
        } finally { fixture.recycle(); }
        try {
            byte[] before = sha256(transparent);
            Bitmap reference = BitmapFactory.decodeFile(transparent.getPath(), decodeOptions());
            Bitmap actual = null;
            try {
                MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(transparent);
                actual = MaiPortraitBitmap.decodeMutable(source);
                assertNotNull(reference);
                for (int y = 0; y < source.height; ++y)
                    for (int x = 0; x < source.width; ++x)
                        assertEquals("Straight colors/alpha must match Android PNG decode",
                            reference.getPixel(x, y), actual.getPixel(x, y));
                assertTrue(Color.alpha(actual.getPixel(1, 0)) < 255);
            } finally {
                if (actual != null) actual.recycle();
                if (reference != null) reference.recycle();
            }
            assertArrayEquals(before, sha256(transparent));
            File excessive = new File(directory, "metadata-budget.jpg");
            writePatternJpeg(excessive, 8, 8);
            patchJpegDimensions(excessive, 20_001, 8);
            try {
                MaiPortraitBitmap.inspect(excessive);
                fail("Oversized metadata was accepted");
            } catch (java.io.IOException expected) {
                assertTrue(expected.getMessage(), expected.getMessage().contains("metadata budget"));
            }
            // Source metadata below the inspection ceiling still exceeds the raster allocation
            // ceiling. The compressed raster remains tiny: rejection must precede its decode.
            File rasterBudget = new File(directory, "raster-budget.jpg");
            writePatternJpeg(rasterBudget, 8, 8);
            patchJpegDimensions(rasterBudget, 8001, 4000);
            MaiPortraitBitmap.Source oversized = MaiPortraitBitmap.inspect(rasterBudget);
            try {
                Bitmap unexpected = MaiPortraitBitmap.decodeMutable(oversized);
                unexpected.recycle();
                fail("Oversized full-resolution allocation was accepted");
            } catch (java.io.IOException expected) {
                assertTrue(expected.getMessage(), expected.getMessage().contains("memory budget"));
            }
        } finally { deleteChildren(directory); }
    }

    private static AIAssistantController readyController(
            ActivityScenario<AIAssistantTestActivity> scenario) throws Exception {
        AtomicReference<AIAssistantController> reference = new AtomicReference<>();
        scenario.onActivity(activity -> reference.set(activity.panel.controller));
        AIAssistantController controller = reference.get();
        long deadline = SystemClock.elapsedRealtime() + 30_000;
        while (!controller.state.ready && SystemClock.elapsedRealtime() < deadline) Thread.sleep(50);
        assertTrue("Local test controller did not initialize", controller.state.ready);
        return controller;
    }

    private static JSONObject beautify(String path, double whitening) throws Exception {
        return new JSONObject().put("path", path).put("operation", "beautify")
            .put("smoothing", 0).put("brightening", 0).put("warmth", 0)
            .put("whitening", whitening).put("use_personal_model", false);
    }

    // Preserve the exact public fixture pixels and run the same JNI Analyze entry with two
    // host samplers. This diagnostic changes no ownership threshold or editing parameter.
    private static void dumpAnalyzeProxyCounterfactual(MaiPortraitBitmap.Source source, JSONArray faces,
                                                       JSONObject arguments) throws Exception {
        File cache = InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir();
        File directory = new File(cache, "portrait-proxy-evidence");
        assertTrue(directory.mkdirs() || directory.isDirectory());
        java.nio.file.Files.copy(source.file.toPath(), new File(directory, "source-exif6.jpg").toPath(),
            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        Bitmap original = MaiPortraitBitmap.decodeMutable(source), global = null;
        java.lang.reflect.Method scale = MaiPortraitImage.class.getDeclaredMethod("scaledRegion",
            Bitmap.class, Rect.class, int.class, int.class);
        java.lang.reflect.Method fingerprint = MaiPortraitImage.class.getDeclaredMethod("fingerprint",
            Bitmap.class, java.util.function.BooleanSupplier.class);
        java.lang.reflect.Method analyze = MaiPortraitImage.class.getDeclaredMethod("nativeAnalyze",
            Bitmap.class, Bitmap.class, int.class, int.class, byte[].class, byte[].class,
            byte[].class, byte[].class, byte[].class, long[].class);
        java.lang.reflect.Method free = MaiPortraitImage.class.getDeclaredMethod("nativeFree", long.class);
        for (java.lang.reflect.Method method : new java.lang.reflect.Method[] {scale, fingerprint, analyze, free})
            method.setAccessible(true);
        try {
            Rect full = new Rect(0, 0, source.width, source.height);
            global = (Bitmap) scale.invoke(null, original, full, 64, 64);
            String identity = (String) fingerprint.invoke(null, original, (java.util.function.BooleanSupplier) () -> true);
            JSONObject specification = new JSONObject(arguments.toString());
            specification.remove("path");
            String hints = new JSONObject().put("_face_hints", faces).toString();
            JSONObject evidence = new JSONObject().put("source_width", source.width).put("source_height", source.height)
                .put("raw_width", source.rawWidth).put("raw_height", source.rawHeight).put("exif", source.orientation)
                .put("hints", new JSONObject(hints)).put("arguments", specification).put("fingerprint", identity);
            writeDiagnosticRgba(new File(directory, "global-64x64.rgba"), global);
            for (String label : new String[] {"power-of-two", "exact-512"}) {
                int width = Math.max(1, Math.round(source.width * 512f / Math.max(source.width, source.height)));
                int height = Math.max(1, Math.round(source.height * 512f / Math.max(source.width, source.height)));
                Bitmap proxy = label.equals("power-of-two") ? MaiPortraitBitmap.proxy(source, 512)
                    : (Bitmap) scale.invoke(null, original, full, width, height);
                long[] context = new long[1];
                try {
                    writeDiagnosticRgba(new File(directory, label + ".rgba"), proxy);
                    byte[] response = (byte[]) analyze.invoke(null, proxy, global, source.width, source.height,
                        hints.getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        specification.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        new File(directory, "profile").getPath().getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        source.file.getPath().getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        identity.getBytes(java.nio.charset.StandardCharsets.UTF_8), context);
                    evidence.put(label, new JSONObject().put("width", proxy.getWidth()).put("height", proxy.getHeight())
                        .put("stride", proxy.getWidth() * 4).put("response", new JSONObject(
                            new String(response, java.nio.charset.StandardCharsets.UTF_8))));
                } finally {
                    if (context[0] != 0) free.invoke(null, context[0]);
                    proxy.recycle();
                }
            }
            java.nio.file.Files.write(new File(directory, "analyze-comparison.json").toPath(),
                evidence.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } finally { if (global != null) global.recycle(); original.recycle(); }
    }

    private static void writeDiagnosticRgba(File output, Bitmap image) throws Exception {
        int[] pixels = new int[image.getWidth()];
        byte[] row = new byte[image.getWidth() * 4];
        try (FileOutputStream stream = new FileOutputStream(output)) {
            for (int y = 0; y < image.getHeight(); ++y) {
                image.getPixels(pixels, 0, pixels.length, 0, y, pixels.length, 1);
                for (int x = 0; x < pixels.length; ++x) {
                    int offset = x * 4, color = pixels[x];
                    row[offset] = (byte) Color.red(color); row[offset + 1] = (byte) Color.green(color);
                    row[offset + 2] = (byte) Color.blue(color); row[offset + 3] = (byte) Color.alpha(color);
                }
                stream.write(row);
            }
        }
    }

    private static void assertNativeReceipt(JSONObject receipt, File original, double whitening,
                                            int width, int height)
            throws Exception {
        assertTrue(receipt.has("result_id"));
        assertEquals(original.getCanonicalPath(), new File(receipt.getString("original_path")).getCanonicalPath());
        assertEquals(whitening, receipt.getJSONObject("parameters").getDouble("whitening"), 0.0001);
        assertEquals(width, receipt.getInt("source_width"));
        assertEquals(height, receipt.getInt("source_height"));
        assertEquals(width, receipt.getInt("mask_width"));
        assertEquals(height, receipt.getInt("mask_height"));
        assertEquals("top_left", receipt.getString("mask_coordinate_origin"));
        assertFalse(receipt.getString("mask_path").isEmpty());
        assertTrue(receipt.getBoolean("face_analysis_performed"));
        assertTrue("SkinPlan must perform person ownership analysis", receipt.getBoolean("person_analysis_performed"));
        assertTrue(receipt.getInt("person_count") >= 0);
        assertTrue(receipt.getInt("face_count") >= 0);
        assertEquals(receipt.getBoolean("person_ownership_reliable")
                ? "selected_person_visible_skin" : "face_skin_only", receipt.getString("skin_scope"));
        assertEquals("native-owned-skin-tiles-20261105-v8", receipt.getString("processing_version"));
        assertEquals("diagnostic_skin_alpha; rgba_is_already_composited; do_not_apply_mask_again",
            receipt.getString("mask_semantics"));
        assertEquals("SkinPlan receipt coordinates must describe the upright whole source",
            new Rect(0, 0, width, height), receiptRect(receipt));
        assertTrue(receipt.getLong("skin_pixel_count") > 100);
        assertTrue(receipt.getDouble("skin_weight") > 0);
        assertTrue(receipt.getDouble("mask_coverage_in_roi") > 0);
        assertTrue(receipt.getDouble("mask_coverage_in_roi") <= 1);
        if (whitening == 0) {
            assertEquals("Zero parameters must not change RGB pixels", 0, receipt.getLong("changed_pixel_count"));
            assertEquals(0, receipt.getDouble("changed_rgb_fraction_in_roi"), 0);
        } else {
            assertTrue(receipt.getLong("changed_pixel_count") > 100);
            assertTrue(receipt.getDouble("changed_rgb_fraction_in_roi") > 0);
            assertTrue(receipt.getDouble("changed_rgb_fraction_in_roi") <= 1);
            assertTrue("Core skin L* must increase", receipt.getDouble("skin_L_after")
                > receipt.getDouble("skin_L_before"));
        }
    }

    private static Rect receiptRect(JSONObject receipt) throws Exception {
        JSONObject roi = receipt.getJSONObject("roi");
        int x = roi.getInt("x"), y = roi.getInt("y");
        return new Rect(x, y, x + roi.getInt("width"), y + roi.getInt("height"));
    }

    private static void assertUprightPng(File output, int width, int height) throws Exception {
        assertTrue(output.isFile() && output.length() > 0);
        try (InputStream input = new FileInputStream(output)) {
            byte[] header = new byte[8];
            assertEquals(8, input.read(header));
            assertArrayEquals(new byte[] {(byte) 137, 80, 78, 71, 13, 10, 26, 10}, header);
        }
        MaiPortraitBitmap.Source metadata = MaiPortraitBitmap.inspect(output);
        assertEquals(width, metadata.rawWidth);
        assertEquals(height, metadata.rawHeight);
        assertEquals(ExifInterface.ORIENTATION_NORMAL, metadata.orientation);
    }

    private static void assertGrayscaleMask(File output, int width, int height) throws Exception {
        assertUprightPng(output, width, height);
        // A gray-looking RGBA image is not the L8 diagnostic contract. Inspect PNG IHDR
        // independently of BitmapFactory, which expands grayscale to ARGB when decoding.
        try (DataInputStream input = new DataInputStream(new FileInputStream(output))) {
            assertEquals(0x89504e470d0a1a0aL, input.readLong());
            assertEquals(13, input.readInt());
            assertEquals(0x49484452, input.readInt());
            assertEquals(width, input.readInt());
            assertEquals(height, input.readInt());
            assertEquals("Mask PNG must contain eight-bit samples", 8, input.readUnsignedByte());
            assertEquals("Mask PNG must use grayscale color type", 0, input.readUnsignedByte());
        }
    }

    private static void assertBodyPixels(MaiPortraitBitmap.Source source, File zero, File edited,
                                         File mask, JSONObject receipt) throws Exception {
        Bitmap originalPixels = null, zeroPixels = null, editedPixels = null, maskPixels = null;
        try {
            originalPixels = MaiPortraitBitmap.decodeMutable(source);
            zeroPixels = BitmapFactory.decodeFile(zero.getPath(), decodeOptions());
            editedPixels = BitmapFactory.decodeFile(edited.getPath(), decodeOptions());
            maskPixels = BitmapFactory.decodeFile(mask.getPath(), decodeOptions());
            assertNotNull(zeroPixels);
            assertNotNull(editedPixels);
            assertNotNull(maskPixels);
            long changed = 0;
            for (int y = 0; y < source.height; ++y)
                for (int x = 0; x < source.width; ++x) {
                    int before = originalPixels.getPixel(x, y), after = editedPixels.getPixel(x, y);
                    if (before != zeroPixels.getPixel(x, y))
                        fail("Zero parameters changed pixel " + x + "," + y);
                    if (Color.alpha(before) != Color.alpha(after)) fail("Skin edit changed alpha at " + x + "," + y);
                    if (Color.red(maskPixels.getPixel(x, y)) == 0 && before != after)
                        fail("Skin edit changed a zero-mask pixel at " + x + "," + y);
                    if (before != after) ++changed;
                }
            assertEquals("Published pixels differ from native changed-pixel metrics",
                receipt.getLong("changed_pixel_count"), changed);
            // These verified sky/ocean/sand/shirt/shorts and arm/leg coordinates are shared
            // with MaiPortraitSkinTests.mm; they check routing and protection, not aesthetics.
            for (int[] point : new int[][] {{500, 100}, {800, 450}, {800, 600}, {500, 380}, {495, 450}}) {
                int x = point[0], y = point[1];
                assertEquals("Non-skin fixture point received a skin mask at " + x + "," + y,
                    0, Color.red(maskPixels.getPixel(x, y)));
                assertEquals("Protected clothes/background changed at " + x + "," + y,
                    originalPixels.getPixel(x, y), editedPixels.getPixel(x, y));
            }
            for (int[] point : new int[][] {{650, 320}, {625, 570}}) {
                int changedInRegion = 0;
                for (int y = point[1] - 8; y <= point[1] + 8; ++y)
                    for (int x = point[0] - 8; x <= point[0] + 8; ++x)
                        if (originalPixels.getPixel(x, y) != editedPixels.getPixel(x, y)) ++changedInRegion;
                assertTrue("Arm/leg skin received no saved change near " + point[0] + "," + point[1],
                    changedInRegion > 0);
            }
        } finally {
            for (Bitmap image : new Bitmap[] {maskPixels, editedPixels, zeroPixels, originalPixels})
                if (image != null) image.recycle();
        }
    }

    private static final class PixelMetrics {
        long changed, mildChanged;
        double originalL, mildL, strongL, originalC, mildC, strongC;
    }

    private static void assertEyeCentersProtected(MaiPortraitBitmap.Source source, File edited, File mask,
                                                   JSONObject landmarks) throws Exception {
        MaiPortraitBitmap.Source editedSource = MaiPortraitBitmap.inspect(edited);
        MaiPortraitBitmap.Source maskSource = MaiPortraitBitmap.inspect(mask);
        BitmapRegionDecoder originalDecoder = null, editedDecoder = null, maskDecoder = null;
        try {
            originalDecoder = BitmapRegionDecoder.newInstance(source.file.getPath(), false);
            editedDecoder = BitmapRegionDecoder.newInstance(edited.getPath(), false);
            maskDecoder = BitmapRegionDecoder.newInstance(mask.getPath(), false);
            for (String eye : new String[] {"left_eye", "right_eye"}) {
                JSONObject point = landmarks.getJSONObject(eye);
                double sourceX = point.getDouble("x"), sourceY = point.getDouble("y");
                assertTrue("Eye landmark must use finite upright-source coordinates",
                    Double.isFinite(sourceX) && Double.isFinite(sourceY));
                int x = (int) Math.round(sourceX), y = (int) Math.round(sourceY);
                assertTrue("Eye landmark is outside the upright source",
                    x >= 0 && x < source.width && y >= 0 && y < source.height);
                // Check only the detected center: nearby eyelid skin may legitimately receive
                // color treatment. Region decoding retains the JPEG halo and EXIF mapping.
                Rect center = new Rect(x, y, x + 1, y + 1);
                Bitmap before = null, after = null, coverage = null;
                try {
                    before = MaiPortraitBitmap.region(source, originalDecoder, center);
                    after = MaiPortraitBitmap.region(editedSource, editedDecoder, center);
                    coverage = MaiPortraitBitmap.region(maskSource, maskDecoder, center);
                    assertEquals(eye + " center received a skin-color mask at " + x + "," + y,
                        0, Color.red(coverage.getPixel(0, 0)));
                    assertEquals(eye + " center changed saved pixels at " + x + "," + y,
                        before.getPixel(0, 0), after.getPixel(0, 0));
                } finally {
                    if (coverage != null) coverage.recycle();
                    if (after != null) after.recycle();
                    if (before != null) before.recycle();
                }
            }
        } finally {
            if (maskDecoder != null) maskDecoder.recycle();
            if (editedDecoder != null) editedDecoder.recycle();
            if (originalDecoder != null) originalDecoder.recycle();
        }
    }

    /** Compare all pixels using bounded strips; no verification Bitmap spans the full source. */
    private static PixelMetrics compareInStrips(MaiPortraitBitmap.Source source, File mild, File strong,
                                                File mask) throws Exception {
        BitmapRegionDecoder originalDecoder = null, mildDecoder = null, strongDecoder = null, maskDecoder = null;
        PixelMetrics result = new PixelMetrics();
        ColorSpace.Connector toLab = ColorSpace.connect(ColorSpace.get(ColorSpace.Named.SRGB),
            ColorSpace.get(ColorSpace.Named.CIE_LAB));
        float[] color = new float[3];
        int[] originalPixels = new int[source.width * STRIP_ROWS];
        int[] mildPixels = new int[originalPixels.length], strongPixels = new int[originalPixels.length];
        int[] maskPixels = new int[originalPixels.length];
        try {
            originalDecoder = BitmapRegionDecoder.newInstance(source.file.getPath(), false);
            mildDecoder = BitmapRegionDecoder.newInstance(mild.getPath(), false);
            strongDecoder = BitmapRegionDecoder.newInstance(strong.getPath(), false);
            maskDecoder = BitmapRegionDecoder.newInstance(mask.getPath(), false);
            for (int top = 0; top < source.height; top += STRIP_ROWS) {
                Rect strip = new Rect(0, top, source.width, Math.min(source.height, top + STRIP_ROWS));
                Bitmap originalImage = null, mildImage = null, strongImage = null, maskImage = null;
                try {
                    originalImage = MaiPortraitBitmap.region(source, originalDecoder, strip);
                    mildImage = mildDecoder.decodeRegion(strip, decodeOptions());
                    strongImage = strongDecoder.decodeRegion(strip, decodeOptions());
                    maskImage = maskDecoder.decodeRegion(strip, decodeOptions());
                    assertNotNull(mildImage);
                    assertNotNull(strongImage);
                    assertNotNull(maskImage);
                    assertEquals(ColorSpace.get(ColorSpace.Named.SRGB), strongImage.getColorSpace());
                    originalImage.getPixels(originalPixels, 0, source.width, 0, 0, source.width, strip.height());
                    mildImage.getPixels(mildPixels, 0, source.width, 0, 0, source.width, strip.height());
                    strongImage.getPixels(strongPixels, 0, source.width, 0, 0, source.width, strip.height());
                    maskImage.getPixels(maskPixels, 0, source.width, 0, 0, source.width, strip.height());
                    for (int y = 0; y < strip.height(); ++y) {
                        for (int x = 0; x < source.width; ++x) {
                            int index = y * source.width + x;
                            int before = originalPixels[index], after = strongPixels[index];
                            if (x < 64 || x >= source.width - 64 || top + y < 64 || top + y >= source.height - 64) {
                                if (before != mildPixels[index] || before != after)
                                    fail("Skin edit changed distant background at " + x + "," + (top + y));
                            }
                            if (Color.red(maskPixels[index]) == 0 && before != after)
                                fail("Skin edit changed a zero-mask pixel at " + x + "," + (top + y));
                            if (Color.alpha(before) != Color.alpha(after)
                                    || Color.alpha(before) != Color.alpha(mildPixels[index]))
                                fail("Skin edit changed alpha at " + x + "," + (top + y));
                            if (before != mildPixels[index]) ++result.mildChanged;
                            if (before == after) continue;
                            ++result.changed;
                            toLab(before, color, toLab);
                            result.originalL += color[0];
                            result.originalC += Math.hypot(color[1], color[2]);
                            toLab(mildPixels[index], color, toLab);
                            result.mildL += color[0];
                            result.mildC += Math.hypot(color[1], color[2]);
                            toLab(after, color, toLab);
                            result.strongL += color[0];
                            result.strongC += Math.hypot(color[1], color[2]);
                        }
                    }
                } finally {
                    if (maskImage != null) maskImage.recycle();
                    if (strongImage != null) strongImage.recycle();
                    if (mildImage != null) mildImage.recycle();
                    if (originalImage != null) originalImage.recycle();
                }
            }
        } finally {
            if (maskDecoder != null) maskDecoder.recycle();
            if (strongDecoder != null) strongDecoder.recycle();
            if (mildDecoder != null) mildDecoder.recycle();
            if (originalDecoder != null) originalDecoder.recycle();
        }
        return result;
    }

    private static void toLab(int pixel, float[] color, ColorSpace.Connector connector) {
        color[0] = Color.red(pixel) / 255f;
        color[1] = Color.green(pixel) / 255f;
        color[2] = Color.blue(pixel) / 255f;
        connector.transform(color);
    }

    private static void writeHighResolutionJpeg(File destination) throws Exception {
        // Actual reported raw dimensions 5712x4284 + EXIF6 yield upright 4284x5712. The public
        // Lena placement also reproduces the shoulder-only detection in an overlapping edge tile.
        // Only one full-sized synthetic fixture exists; no private user photograph is used.
        Bitmap image = Bitmap.createBitmap(SOURCE_HEIGHT, SOURCE_WIDTH, Bitmap.Config.ARGB_8888);
        Bitmap face = null;
        Canvas canvas = null;
        try {
            image.eraseColor(Color.rgb(54, 74, 94));
            canvas = new Canvas(image);
            android.graphics.Paint paint = new android.graphics.Paint();
            paint.setColor(Color.rgb(31, 91, 123));
            canvas.drawRect(0, 0, 128, SOURCE_WIDTH, paint);
            paint.setColor(Color.rgb(112, 72, 38));
            canvas.drawRect(SOURCE_HEIGHT - 128, 0, SOURCE_HEIGHT, SOURCE_WIDTH, paint);
            try (InputStream input = InstrumentationRegistry.getInstrumentation().getContext()
                    .getAssets().open("portrait-test-face.jpg")) {
                face = BitmapFactory.decodeStream(input, null, decodeOptions());
            }
            assertNotNull("Staged OpenCV Lena fixture missing", face);
            // Inverse of EXIF6: upright local (x,y) is stored as raw local (y,side-x).
            int side = 1536, uprightLeft = 1536, uprightTop = 1980;
            int rawLeft = uprightTop, rawTop = SOURCE_WIDTH - uprightLeft - side;
            canvas.save();
            canvas.translate(rawLeft, rawTop + side);
            canvas.rotate(-90);
            canvas.drawBitmap(face, null, new Rect(0, 0, side, side), null);
            canvas.restore();
            writeBitmap(image, destination, Bitmap.CompressFormat.JPEG);
        } finally {
            if (canvas != null) canvas.setBitmap(null);
            if (face != null) face.recycle();
            image.recycle();
        }
        setOrientation(destination, ExifInterface.ORIENTATION_ROTATE_90);
    }

    private static int[] orientedPixel(int orientation, int x, int y, int width, int height) {
        switch (orientation) {
            case 2: return new int[] {width - 1 - x, y};
            case 3: return new int[] {width - 1 - x, height - 1 - y};
            case 4: return new int[] {x, height - 1 - y};
            case 5: return new int[] {y, x};
            case 6: return new int[] {height - 1 - y, x};
            case 7: return new int[] {height - 1 - y, width - 1 - x};
            case 8: return new int[] {y, width - 1 - x};
            default: return new int[] {x, y};
        }
    }

    private static void writePatternJpeg(File destination, int width, int height) throws Exception {
        Bitmap image = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        try {
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    image.setPixel(x, y, Color.rgb((x * 31 + y * 7) & 255,
                        (y * 47 + x * 3) & 255, (x * 17 + y * 19) & 255));
            writeBitmap(image, destination, Bitmap.CompressFormat.JPEG);
        } finally { image.recycle(); }
    }

    private static void writeBitmap(Bitmap image, File destination, Bitmap.CompressFormat format)
            throws Exception {
        try (FileOutputStream output = new FileOutputStream(destination)) {
            assertTrue(image.compress(format, 100, output));
            output.getFD().sync();
        }
    }

    private static void copyAsset(String asset, File destination) throws Exception {
        try (InputStream input = InstrumentationRegistry.getInstrumentation().getContext().getAssets().open(asset)) {
            java.nio.file.Files.copy(input, destination.toPath());
        }
    }

    private static void setOrientation(File file, int orientation) throws Exception {
        ExifInterface exif = new ExifInterface(file.getPath());
        exif.setAttribute(ExifInterface.TAG_ORIENTATION, Integer.toString(orientation));
        exif.saveAttributes();
    }

    private static BitmapFactory.Options decodeOptions() {
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        options.inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
        options.inScaled = false;
        return options;
    }

    private static void assertHostFailure(AIAssistantController controller, JSONObject arguments,
                                          String expectedCode) throws Exception {
        try {
            controller.transformImageForHost(arguments);
            fail("Expected portrait failure " + expectedCode);
        } catch (java.io.IOException expected) {
            assertTrue(expected.getMessage(), expected.getMessage() != null
                && expected.getMessage().contains(expectedCode));
        }
    }

    private static byte[] sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return digest.digest();
    }

    private static File testDirectory(String suffix) {
        File directory = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir(),
            "portrait-region-" + suffix + "-" + UUID.randomUUID());
        assertTrue(directory.mkdirs());
        return directory;
    }

    private static Set<String> fileNames(File directory) {
        String[] names = directory.list();
        assertNotNull(names);
        return new HashSet<>(Arrays.asList(names));
    }

    private static boolean hasPublishedMask(File directory) {
        for (String name : fileNames(directory))
            if (name.startsWith("skin-mask-") && new File(directory, name).isFile()) return true;
        return false;
    }

    private static void assertNoTemporaryOutputs(File directory) {
        for (String name : fileNames(directory))
            assertFalse("Unpublished portrait temporary remains: " + name, name.startsWith(".portrait-"));
    }

    private static void deleteChildren(File directory) {
        File[] children = directory.listFiles();
        if (children != null) for (File file : children) file.delete();
        directory.delete();
    }

    private static void logMemory(String phase) {
        Runtime runtime = Runtime.getRuntime();
        System.out.println(String.format(Locale.ROOT,
            "PortraitRegion memory %s: javaUsed=%d nativeAllocated=%d maxJava=%d",
            phase, runtime.totalMemory() - runtime.freeMemory(), Debug.getNativeHeapAllocatedSize(),
            runtime.maxMemory()));
    }

    /** Mutate only SOF metadata; inspect must reject before attempting a raster allocation. */
    private static void patchJpegDimensions(File file, int width, int height) throws Exception {
        byte[] bytes = java.nio.file.Files.readAllBytes(file.toPath());
        for (int offset = 2; offset + 8 < bytes.length;) {
            assertEquals("Malformed generated JPEG marker", 255, bytes[offset] & 255);
            int marker = bytes[offset + 1] & 255;
            int length = ((bytes[offset + 2] & 255) << 8) | (bytes[offset + 3] & 255);
            if (marker == 0xc0 || marker == 0xc2) {
                bytes[offset + 5] = (byte) (height >>> 8);
                bytes[offset + 6] = (byte) height;
                bytes[offset + 7] = (byte) (width >>> 8);
                bytes[offset + 8] = (byte) width;
                try (FileOutputStream output = new FileOutputStream(file)) { output.write(bytes); }
                return;
            }
            if (marker == 0xda || length < 2) break;
            offset += length + 2;
        }
        fail("Generated JPEG has no SOF dimensions");
    }
}
