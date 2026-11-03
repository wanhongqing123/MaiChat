// 文件作用：用本地合成图和公开 Lena 样例验证 Android 原尺寸 ROI、方向及输出登记。
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
import java.util.concurrent.atomic.AtomicReference;
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
            File mildFile = null, strongFile = null;
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
                assertNativeReceipt(mildReceipt, original, 0.25);
                assertUprightPng(mildFile, source.width, source.height);
                assertArrayEquals("First edit changed the original file", before, sha256(original));

                // Editing an output must resolve the recorded original, not apply a second whitening pass.
                AIAssistantController.ImportedFile strong = controller.transformImageForHost(
                    beautify(mild.relativePath, 0.75));
                JSONObject strongReceipt = controller.portraitReceiptForHost(strong.relativePath);
                strongFile = controller.workspaceImageForHost(strong.relativePath);
                assertNativeReceipt(strongReceipt, original, 0.75);
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
                PixelMetrics pixels = compareInStrips(source, mildFile, strongFile,
                    receiptRect(strongReceipt));
                assertTrue("No visible native ROI edit", pixels.changed > 100);
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
                logMemory("edits-and-strip-verification-complete");
                System.out.println("PortraitRegion 2447万像素/EXIF6: "
                    + (SystemClock.elapsedRealtime() - started) + " ms; ROI="
                    + strongReceipt.getJSONObject("roi") + "; changed=" + pixels.changed
                    + "; saved L*=" + pixels.originalL / pixels.changed + " -> "
                    + pixels.mildL / pixels.changed + " -> " + pixels.strongL / pixels.changed
                    + "; saved C*=" + pixels.originalC / pixels.changed + " -> "
                    + pixels.mildC / pixels.changed + " -> " + pixels.strongC / pixels.changed);
            } finally {
                if (strongFile != null) strongFile.delete();
                if (mildFile != null) mildFile.delete();
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
                assertHostFailure(controller, beautify(original.getName(), 0.5), "face_detection_failed");
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

    private static void assertNativeReceipt(JSONObject receipt, File original, double whitening)
            throws Exception {
        assertTrue(receipt.has("result_id"));
        assertEquals(original.getCanonicalPath(), new File(receipt.getString("original_path")).getCanonicalPath());
        assertEquals(whitening, receipt.getJSONObject("parameters").getDouble("whitening"), 0.0001);
        assertEquals(SOURCE_WIDTH, receipt.getInt("source_width"));
        assertEquals(SOURCE_HEIGHT, receipt.getInt("source_height"));
        assertTrue(receipt.getBoolean("face_analysis_performed"));
        assertFalse("Beautify must not invent body analysis", receipt.getBoolean("person_analysis_performed"));
        assertTrue(receipt.isNull("person_count"));
        assertEquals(1, receipt.getInt("face_count"));
        assertTrue(receipt.getString("processing_version").startsWith("native-face-roi-"));
        assertEquals("diagnostic_skin_alpha; rgba_is_already_composited; do_not_apply_mask_again",
            receipt.getString("mask_semantics"));
        Rect roi = receiptRect(receipt);
        assertTrue(roi.width() > 0 && roi.width() <= 2048);
        assertTrue(roi.height() > 0 && roi.height() <= 2048);
        assertTrue(new Rect(0, 0, SOURCE_WIDTH, SOURCE_HEIGHT).contains(roi));
        assertTrue(receipt.getLong("skin_pixel_count") > 100);
        assertTrue(receipt.getLong("changed_pixel_count") > 100);
        assertTrue(receipt.getDouble("skin_weight") > 0);
        assertTrue(receipt.getDouble("mask_coverage_in_roi") > 0);
        assertTrue(receipt.getDouble("mask_coverage_in_roi") <= 1);
        assertTrue(receipt.getDouble("changed_rgb_fraction_in_roi") > 0);
        assertTrue(receipt.getDouble("changed_rgb_fraction_in_roi") <= 1);
        assertTrue("Core skin L* must increase", receipt.getDouble("skin_L_after")
            > receipt.getDouble("skin_L_before"));
        assertTrue("Core skin C* must decrease", receipt.getDouble("skin_C_after")
            < receipt.getDouble("skin_C_before"));
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

    private static final class PixelMetrics {
        long changed, mildChanged;
        double originalL, mildL, strongL, originalC, mildC, strongC;
    }

    /** Compare all pixels using bounded strips; no verification Bitmap spans the full source. */
    private static PixelMetrics compareInStrips(MaiPortraitBitmap.Source source, File mild, File strong,
                                                Rect roi) throws Exception {
        BitmapRegionDecoder originalDecoder = null, mildDecoder = null, strongDecoder = null;
        PixelMetrics result = new PixelMetrics();
        ColorSpace.Connector toLab = ColorSpace.connect(ColorSpace.get(ColorSpace.Named.SRGB),
            ColorSpace.get(ColorSpace.Named.CIE_LAB));
        float[] color = new float[3];
        int[] originalPixels = new int[source.width * STRIP_ROWS];
        int[] mildPixels = new int[originalPixels.length], strongPixels = new int[originalPixels.length];
        try {
            originalDecoder = BitmapRegionDecoder.newInstance(source.file.getPath(), false);
            mildDecoder = BitmapRegionDecoder.newInstance(mild.getPath(), false);
            strongDecoder = BitmapRegionDecoder.newInstance(strong.getPath(), false);
            for (int top = 0; top < source.height; top += STRIP_ROWS) {
                Rect strip = new Rect(0, top, source.width, Math.min(source.height, top + STRIP_ROWS));
                Bitmap originalImage = null, mildImage = null, strongImage = null;
                try {
                    originalImage = MaiPortraitBitmap.region(source, originalDecoder, strip);
                    mildImage = mildDecoder.decodeRegion(strip, decodeOptions());
                    strongImage = strongDecoder.decodeRegion(strip, decodeOptions());
                    assertNotNull(mildImage);
                    assertNotNull(strongImage);
                    assertEquals(ColorSpace.get(ColorSpace.Named.SRGB), strongImage.getColorSpace());
                    originalImage.getPixels(originalPixels, 0, source.width, 0, 0, source.width, strip.height());
                    mildImage.getPixels(mildPixels, 0, source.width, 0, 0, source.width, strip.height());
                    strongImage.getPixels(strongPixels, 0, source.width, 0, 0, source.width, strip.height());
                    for (int y = 0; y < strip.height(); ++y) {
                        for (int x = 0; x < source.width; ++x) {
                            int index = y * source.width + x;
                            int before = originalPixels[index], after = strongPixels[index];
                            if (!roi.contains(x, top + y)) {
                                if (before != mildPixels[index] || before != after)
                                    fail("ROI changed distant background at " + x + "," + (top + y));
                            }
                            if (Color.alpha(before) != Color.alpha(after)
                                    || Color.alpha(before) != Color.alpha(mildPixels[index]))
                                fail("ROI changed alpha at " + x + "," + (top + y));
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
                    if (strongImage != null) strongImage.recycle();
                    if (mildImage != null) mildImage.recycle();
                    if (originalImage != null) originalImage.recycle();
                }
            }
        } finally {
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
