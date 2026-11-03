// 文件作用：Android 设备集成测试；GraphicsDirectPresenterInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：图片解码、直接呈现及 resize；Graphics 宿主使用独立 Dialog，避免 Main 异步换页移除测试 View。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class GraphicsDirectPresenterInstrumentedTest {
    @Test public void resizingUsesTheNewBufferAndRetainsTheDecodedImage() throws Exception {
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        File source = File.createTempFile("graphics-resize-", ".png", context.getCacheDir());
        Bitmap red = Bitmap.createBitmap(20, 20, Bitmap.Config.ARGB_8888);
        red.eraseColor(Color.RED);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(red.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        red.recycle();
        Intent intent = new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Activity activity = instrumentation.startActivitySync(intent);
        AtomicReference<MaiGraphicsImageView> image = new AtomicReference<>();
        AtomicReference<Dialog> host = new AtomicReference<>();
        CountDownLatch ready = new CountDownLatch(1);
        try {
            instrumentation.runOnMainSync(() -> {
                MaiGraphicsImageView view = new MaiGraphicsImageView(activity);
                view.setPresentationListener(success -> { if (success) ready.countDown(); });
                view.showImage(source.getPath(), false, null);
                image.set(view);
                host.set(showGraphicsHost(activity, view));
            });
            boolean initialFrame = ready.await(10, TimeUnit.SECONDS);
            assertTrue("Initial Graphics frame: " + surfaceState(image.get(), null), initialFrame);
            // A resize must preserve the decoded texture/context, even when its input is gone.
            assertTrue(source.delete());
            for (int[] size : new int[][]{{160, 96}, {80, 160}, {160, 96}}) {
                instrumentation.runOnMainSync(() -> image.get().setLayoutParams(
                    new FrameLayout.LayoutParams(size[0], size[1])));
                long deadline = System.currentTimeMillis() + 2500;
                boolean correct = false;
                String lastCapture = "no snapshot attempted";
                while (!correct && System.currentTimeMillis() < deadline) {
                    AtomicReference<Bitmap> captured = new AtomicReference<>();
                    instrumentation.runOnMainSync(() -> captured.set(image.get().snapshot()));
                    Bitmap bitmap = captured.get();
                    lastCapture = surfaceState(image.get(), bitmap);
                    if (bitmap != null && bitmap.getWidth() == size[0] && bitmap.getHeight() == size[1]) {
                        int center = bitmap.getPixel(size[0] / 2, size[1] / 2);
                        int edge = size[0] > size[1] ? bitmap.getPixel(2, size[1] / 2)
                            : bitmap.getPixel(size[0] / 2, 2);
                        correct = Color.red(center) >= 240 && Color.green(center) <= 10
                            && Color.blue(center) <= 10 && Color.red(edge) <= 10
                            && Color.green(edge) <= 10 && Color.blue(edge) <= 10;
                    }
                    if (bitmap != null) bitmap.recycle();
                    if (!correct) Thread.sleep(40);
                }
                assertTrue("Resized viewport must center the red image with black letterboxing: "
                    + size[0] + "x" + size[1] + "; " + lastCapture, correct);
            }
        } finally {
            instrumentation.runOnMainSync(() -> {
                if (host.get() != null) host.get().dismiss();
                activity.finish();
            });
            source.delete();
        }
    }

    @Test public void messagePreviewDecodesPngIntoNativeImageView() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "message-preview-red.png");
        Bitmap original = Bitmap.createBitmap(24, 16, Bitmap.Config.ARGB_8888);
        original.eraseColor(Color.RED);
        try (FileOutputStream output = new FileOutputStream(source)) {
            assertTrue(original.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        original.recycle();
        ImageView image = new ImageView(context);
        CountDownLatch decoded = new CountDownLatch(1);
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            MessageImageLoader.load(source.getAbsolutePath(), 48, 48, image, decoded::countDown);
        });
        AtomicReference<Bitmap> result = new AtomicReference<>();
        long deadline = System.currentTimeMillis() + 10000;
        while (result.get() == null && System.currentTimeMillis() < deadline) {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                if (image.getDrawable() instanceof android.graphics.drawable.BitmapDrawable)
                    result.set(((android.graphics.drawable.BitmapDrawable) image.getDrawable())
                        .getBitmap());
            });
            Thread.sleep(25);
        }
        assertNotNull("FFmpeg failed to decode the PNG preview", result.get());
        assertEquals(Color.RED, result.get().getPixel(8, 8));
        assertEquals(1L, decoded.getCount());
    }

    // 作用：验证图形效果直接呈现到 TextureView 且不经过多余的图片副本。
    // 按真实调用顺序检查输入和可观察结果，失败时保留定位所需的上下文。
    @Test public void effectRendersStraightToTextureView() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File source = new File(context.getCacheDir(), "graphics-direct-red.ppm");
        File pngSource = new File(context.getCacheDir(), "graphics-direct-red.png");
        byte[] ppm = {
            'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n',
            (byte) 255, 0, 0, (byte) 255, 0, 0,
            (byte) 255, 0, 0, (byte) 255, 0, 0
        };
        try (FileOutputStream output = new FileOutputStream(source)) { output.write(ppm); }

        Intent intent = new Intent(context, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Activity activity = InstrumentationRegistry.getInstrumentation().startActivitySync(intent);
        CountDownLatch presented = new CountDownLatch(1);
        AtomicReference<MaiGraphicsImageView> surface = new AtomicReference<>();
        AtomicReference<Dialog> host = new AtomicReference<>();
        AtomicReference<Boolean> success = new AtomicReference<>(false);
        try {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                MaiGraphicsImageView view = new MaiGraphicsImageView(activity);
                view.setPresentationListener(rendered -> {
                    success.set(rendered);
                    presented.countDown();
                });
                view.showImage(source.getAbsolutePath(), false, null);
                surface.set(view);
                host.set(showGraphicsHost(activity, view));
            });
            boolean initialFrame = presented.await(10, TimeUnit.SECONDS);
            assertTrue("Initial Graphics frame: " + surfaceState(surface.get(), null), initialFrame);
            assertTrue("Initial Graphics result: " + surfaceState(surface.get(), null), success.get());
            File effects = new File(context.getFilesDir(), "MaiAgentGraphics");
            assertTrue(new File(effects, "default.effect").isFile());
            assertTrue(new File(effects, "solid.effect").isFile());
            Thread.sleep(250);
            AtomicReference<Bitmap> captured = new AtomicReference<>();
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            assertSnapshotSize(surface.get(), captured.get());
            assertEquals(surfaceState(surface.get(), captured.get()), Color.RED, captured.get().getPixel(32, 32));

            Bitmap png = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            png.eraseColor(Color.RED);
            try (FileOutputStream output = new FileOutputStream(pngSource)) {
                assertTrue(png.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            png.recycle();
            CountDownLatch pngPresented = new CountDownLatch(1);
            AtomicReference<Boolean> pngSuccess = new AtomicReference<>(false);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                surface.get().setPresentationListener(rendered -> {
                    pngSuccess.set(rendered);
                    pngPresented.countDown();
                });
                surface.get().showImage(pngSource.getAbsolutePath(), false, null);
            });
            assertTrue(pngPresented.await(10, TimeUnit.SECONDS));
            assertTrue("PNG must decode through FFmpeg Graphics", pngSuccess.get());

            CountDownLatch nextFrame = new CountDownLatch(1);
            AtomicReference<Boolean> frameSuccess = new AtomicReference<>(false);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                surface.get().setPresentationListener(rendered -> {
                    frameSuccess.set(rendered);
                    nextFrame.countDown();
                }));
            byte[] green = {
                0, (byte) 255, 0, (byte) 255, 0, (byte) 255, 0, (byte) 255,
                0, (byte) 255, 0, (byte) 255, 0, (byte) 255, 0, (byte) 255
            };
            assertTrue(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                .showRgbaFrame(green, 2, 2, 8, false));
            assertTrue(nextFrame.await(10, TimeUnit.SECONDS));
            assertTrue(frameSuccess.get());
            Thread.sleep(250);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            assertSnapshotSize(surface.get(), captured.get());
            assertEquals(surfaceState(surface.get(), captured.get()), Color.GREEN, captured.get().getPixel(32, 32));

            File videoFile = new File(context.getCacheDir(), "graphics-direct-red.mp4");
            try (InputStream input = InstrumentationRegistry.getInstrumentation()
                    .getContext().getAssets().open("tiny-red.mp4");
                 FileOutputStream output = new FileOutputStream(videoFile)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
            CountDownLatch videoFrames = new CountDownLatch(2);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                surface.get().setPresentationListener(rendered -> {
                    if (rendered) videoFrames.countDown();
                }));
            assertTrue(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                .playVideo(videoFile.getAbsolutePath()));
            assertTrue(videoFrames.await(10, TimeUnit.SECONDS));
            ((MaiGraphicsTextureView) surface.get().getChildAt(0)).pauseVideo();
            Thread.sleep(250);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> captured.set(((MaiGraphicsTextureView) surface.get().getChildAt(0))
                    .getBitmap()));
            assertSnapshotSize(surface.get(), captured.get());
            int videoPixel = captured.get().getPixel(32, 32);
            assertTrue("video pixel: " + Integer.toHexString(videoPixel) + "; "
                + surfaceState(surface.get(), captured.get()), Color.red(videoPixel) >= 240);
            assertTrue(Color.green(videoPixel) <= 10);
            assertTrue(Color.blue(videoPixel) <= 10);

            ((MaiGraphicsTextureView) surface.get().getChildAt(0)).stopVideo();
            File invalid = new File(context.getCacheDir(), "graphics-invalid.png");
            try (FileOutputStream output = new FileOutputStream(invalid)) {
                output.write("not an image".getBytes("UTF-8"));
            }
            CountDownLatch rejected = new CountDownLatch(1);
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                surface.get().setPresentationListener(rendered -> {
                    if (!rendered) rejected.countDown();
                });
                surface.get().showImage(invalid.getAbsolutePath(), false, null);
            });
            assertTrue(rejected.await(10, TimeUnit.SECONDS));
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                assertEquals(2, surface.get().getChildCount());
                assertTrue(surface.get().getChildAt(1) instanceof TextView);
                assertTrue(!(surface.get().getChildAt(1) instanceof ImageView));
                assertEquals(View.VISIBLE, surface.get().getChildAt(1).getVisibility());
            });
            invalid.delete();
            videoFile.delete();
        } finally {
            InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
                if (host.get() != null) host.get().dismiss();
                activity.finish();
            });
            source.delete();
            pngSource.delete();
        }
    }

    private static Dialog showGraphicsHost(Activity activity, MaiGraphicsImageView view) {
        // Main 的认证/导航回调可以重建 Activity content，但不能移除另一个 Window 的测试 surface。
        FrameLayout content = new FrameLayout(activity);
        content.addView(view, new FrameLayout.LayoutParams(64, 64));
        Dialog dialog = new Dialog(activity, android.R.style.Theme_Black_NoTitleBar);
        dialog.setContentView(content);
        dialog.show();
        return dialog;
    }

    private static String surfaceState(MaiGraphicsImageView view, Bitmap bitmap) {
        String state = view == null ? "view=null" : "attached=" + view.isAttachedToWindow()
            + " view=" + view.getWidth() + "x" + view.getHeight();
        if (bitmap == null) return state + " bitmap=null";
        int width = bitmap.getWidth(), height = bitmap.getHeight();
        int center = bitmap.getPixel(width / 2, height / 2);
        int edge = width > height ? bitmap.getPixel(Math.min(2, width - 1), height / 2)
            : bitmap.getPixel(width / 2, Math.min(2, height - 1));
        return state + " bitmap=" + width + "x" + height
            + " center=" + Integer.toHexString(center) + " edge=" + Integer.toHexString(edge);
    }

    private static void assertSnapshotSize(MaiGraphicsImageView view, Bitmap bitmap) {
        String state = surfaceState(view, bitmap);
        assertNotNull(state, bitmap);
        assertEquals(state, 64, bitmap.getWidth());
        assertEquals(state, 64, bitmap.getHeight());
    }
}
