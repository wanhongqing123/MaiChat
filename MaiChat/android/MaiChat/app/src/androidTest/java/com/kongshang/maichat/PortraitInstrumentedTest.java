// 设备上的 JNI、模型、输出登记及本地候选训练回归；只用公开样例和合成评价。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.ColorSpace;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class PortraitInstrumentedTest {
    @Test public void localPortraitFeedbackProducesUnactivatedCandidate() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario = ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AtomicReference<AIAssistantController> reference = new AtomicReference<>();
            scenario.onActivity(activity -> reference.set(activity.panel.controller));
            AIAssistantController controller = reference.get();
            long deadline = System.currentTimeMillis() + 30_000;
            while (!controller.state.ready && System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue("Test controller did not initialize", controller.state.ready);
            File original = controller.workspaceFile("portrait-original.jpg");
            copyAsset("portrait-test-face.jpg", original);
            byte[] unchanged = Files.readAllBytes(original.toPath());
            AIAssistantController.ImportedFile first = controller.transformImageForHost(new JSONObject()
                .put("path", original.getName()).put("operation", "beautify").put("smoothing", 0)
                .put("brightening", 0).put("warmth", 0).put("whitening", 0.2).put("use_personal_model", false));
            JSONObject preferred = controller.portraitReceiptForHost(first.relativePath);
            assertTrue(preferred.has("result_id"));
            assertEquals(0.2, preferred.getJSONObject("parameters").getDouble("whitening"), 0.0001);
            assertTrue(controller.workspaceImageForHost(first.relativePath).length() > 0);
            AIAssistantController.ImportedFile second = controller.transformImageForHost(new JSONObject()
                .put("path", first.relativePath).put("operation", "beautify").put("smoothing", 0)
                .put("brightening", 0).put("warmth", 0).put("whitening", 0.7).put("use_personal_model", false));
            JSONObject rejected = controller.portraitReceiptForHost(second.relativePath);
            assertEquals(original.getCanonicalPath(), rejected.getString("original_path"));
            assertArrayEquals(unchanged, Files.readAllBytes(original.toPath()));
            File mild = controller.workspaceImageForHost(first.relativePath);
            File strong = controller.workspaceImageForHost(second.relativePath);
            assertWhitening(original, mild, strong);
            File qa = InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir();
            Files.copy(original.toPath(), new File(qa, "portrait-original.jpg").toPath(),
                StandardCopyOption.REPLACE_EXISTING);
            Files.copy(strong.toPath(), new File(qa, "portrait-whitened.png").toPath(),
                StandardCopyOption.REPLACE_EXISTING);
            JSONObject feedback = controller.portraitPersonalizationForHost(new JSONObject()
                .put("preferred_result_id", preferred.getString("result_id"))
                .put("rejected_result_id", rejected.getString("result_id"))
                .put("attributes", new JSONArray().put("whitening")), true);
            assertEquals(1, feedback.getInt("recorded"));
            JSONObject trained = controller.portraitPersonalizationForHost(new JSONObject().put("action", "train"), false);
            assertEquals("candidate_only", trained.getString("status"));
            assertEquals(0, trained.getInt("active_version"));
            assertTrue(trained.getBoolean("personal_weights_updated"));
            File body = controller.workspaceFile("portrait-body.jpg");
            copyAsset("portrait-test-body.jpg", body);
            AIAssistantController.ImportedFile reshaped = controller.transformImageForHost(new JSONObject()
                .put("path", body.getName()).put("operation", "reshape_body")
                .put("slim_waist", 0.5).put("leg_length", 0.2).put("use_personal_model", false));
            assertTrue(controller.workspaceImageForHost(reshaped.relativePath).length() > 0);
            assertEquals(1, controller.portraitReceiptForHost(reshaped.relativePath).getInt("person_count"));
            // 用正常 MainActivity 的媒体视图展示真实 JNI 输出，截图供开发者检查色彩和布局。
            try (ActivityScenario<MainActivity> main = ActivityScenario.launch(MainActivity.class)) {
                main.onActivity(activity -> activity.showFullScreenImage(strong.getPath()));
                InstrumentationRegistry.getInstrumentation().waitForIdleSync();
                Thread.sleep(1000);
                AtomicReference<Bitmap> preview = new AtomicReference<>();
                main.onActivity(activity -> {
                    try {
                        java.lang.reflect.Field field = MainActivity.class.getDeclaredField("galleryImageView");
                        field.setAccessible(true);
                        preview.set(((MaiGraphicsImageView) field.get(activity)).snapshot());
                    } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
                });
                assertPreviewMatchesPng(strong, preview.get());
                Bitmap screenshot = InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
                assertNotNull(screenshot);
                File target = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir(),
                    "portrait-whitening-preview.png");
                try (FileOutputStream stream = new FileOutputStream(target)) {
                    assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, stream));
                }
                screenshot.recycle();
            }
        }
    }
    private static void assertPreviewMatchesPng(File png, Bitmap actual) {
        assertNotNull("Image preview was not presented", actual);
        Bitmap source = BitmapFactory.decodeFile(png.getPath());
        float scale = Math.min((float) actual.getWidth() / source.getWidth(),
            (float) actual.getHeight() / source.getHeight());
        int width = Math.round(source.getWidth() * scale), height = Math.round(source.getHeight() * scale);
        Bitmap scaled = Bitmap.createScaledBitmap(source, width, height, true);
        int left = (actual.getWidth() - width) / 2, top = (actual.getHeight() - height) / 2;
        double error = 0;
        int count = 0;
        for (int y = 8; y < height - 8; y += 8)
            for (int x = 8; x < width - 8; x += 8) {
                int expected = scaled.getPixel(x, y), shown = actual.getPixel(x + left, y + top);
                error += Math.abs(Color.red(expected) - Color.red(shown))
                    + Math.abs(Color.green(expected) - Color.green(shown))
                    + Math.abs(Color.blue(expected) - Color.blue(shown));
                count += 3;
            }
        double meanError = error / count;
        System.out.println("Image preview mean channel error: " + meanError);
        actual.recycle();
        if (scaled != source) scaled.recycle();
        source.recycle();
        assertTrue("Image preview differs from saved sRGB PNG: " + meanError, meanError < 8);
    }
    private static void assertWhitening(File original, File mild, File stronger) {
        Bitmap[] images = {BitmapFactory.decodeFile(original.getPath()), BitmapFactory.decodeFile(mild.getPath()),
            BitmapFactory.decodeFile(stronger.getPath())};
        double[] saturation = new double[3], lightness = new double[3];
        int count = 0;
        float[] hsv = new float[3];
        float[] color = new float[3];
        ColorSpace.Connector toLab = ColorSpace.connect(ColorSpace.get(ColorSpace.Named.SRGB),
            ColorSpace.get(ColorSpace.Named.CIE_LAB));
        for (Bitmap image : images) assertNotNull(image);
        for (int y = 0; y < images[0].getHeight(); y++) {
            assertEquals("Whitening changed distant background", images[0].getPixel(0, y), images[2].getPixel(0, y));
            for (int x = 0; x < images[0].getWidth(); x++) {
                if (images[0].getPixel(x, y) == images[2].getPixel(x, y)) continue;
                count++;
                for (int index = 0; index < 3; index++) {
                    int pixel = images[index].getPixel(x, y);
                    Color.colorToHSV(pixel, hsv);
                    saturation[index] += hsv[1];
                    // HSV V 是最大 RGB 通道，降色度时可能下降；独立使用平台 Lab L* 验明度。
                    color[0] = Color.red(pixel) / 255f;
                    color[1] = Color.green(pixel) / 255f;
                    color[2] = Color.blue(pixel) / 255f;
                    lightness[index] += toLab.transform(color)[0];
                }
            }
        }
        assertTrue("Whitening did not affect skin", count > 100);
        System.out.println("Whitening L*: " + lightness[0] / count + " -> " + lightness[1] / count
            + " -> " + lightness[2] / count + "; HSV saturation: " + saturation[0] / count
            + " -> " + saturation[1] / count + " -> " + saturation[2] / count);
        assertTrue("Whitening did not reduce saturation independently", saturation[0] > saturation[1]
            && saturation[1] > saturation[2]);
        assertTrue("Whitening did not lift lightness independently", lightness[0] < lightness[1]
            && lightness[1] < lightness[2]);
        for (Bitmap image : images) image.recycle();
    }
    private static void copyAsset(String asset, File destination) throws Exception {
        try (java.io.InputStream input = InstrumentationRegistry.getInstrumentation().getContext().getAssets().open(asset)) {
            Files.copy(input, destination.toPath());
        }
    }
}
